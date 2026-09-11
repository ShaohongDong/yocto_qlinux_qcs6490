// SPDX-License-Identifier: MIT
#include "camera.hpp"
#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#ifdef IMX708_WITH_QMMF
#include <qmmf-sdk/qmmf_recorder.h>
#include <qmmf-sdk/qmmf_vendor_tag_descriptor.h>
#endif

namespace imx708 {
void set_enum(GstElement* object, const char* property, const char* nick) {
    auto* spec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), property);
    if (!spec || !G_IS_PARAM_SPEC_ENUM(spec)) throw std::runtime_error(std::string("Missing camera control: ") + property);
    auto* klass = static_cast<GEnumClass*>(g_type_class_ref(G_PARAM_SPEC_VALUE_TYPE(spec)));
    auto* value = g_enum_get_value_by_nick(klass, nick);
    if (!value) { g_type_class_unref(klass); throw std::runtime_error(std::string("Unsupported control value: ") + nick); }
    g_object_set(object, property, value->value, nullptr);
    g_type_class_unref(klass);
}
std::vector<Device> enumerate() {
#ifdef IMX708_WITH_QMMF
    qmmf::recorder::Recorder recorder;
    qmmf::recorder::RecorderCb callback {};
    callback.event_cb = [](auto, void*, size_t) {};
    if (recorder.Connect(callback) != 0) throw std::runtime_error("Cannot connect to qti-cam-server; check camera-service logs");
    std::vector<qmmf::CameraMetadata> metadata;
    const auto status = recorder.GetCamStaticInfo(metadata);
    if (status != 0) { recorder.Disconnect(); throw std::runtime_error("CamX camera enumeration failed"); }
    auto tags = qmmf::VendorTagDescriptor::getGlobalVendorTagDescriptor();
    std::vector<uint32_t> ids(tags ? tags->getTagCount() : 0);
    if (tags) tags->getTagArray(ids.data());
    std::vector<Device> devices;
    for (unsigned i = 0; i < metadata.size(); ++i) {
        std::string name = "Unidentified sensor";
        bool matched = false;
        for (auto tag : ids) {
            const auto* tag_name = tags->getTagName(tag);
            if (!tag_name) continue;
            std::string key = tag_name;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
            if (key.find("sensorname") == std::string::npos) continue;
            const auto entry = metadata[i].find(tag);
            if (entry.type != TYPE_BYTE || !entry.count) continue;
            name.assign(reinterpret_cast<const char*>(entry.data.u8), entry.count);
            name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
            auto lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            matched = lower.find("imx708") != std::string::npos;
            break;
        }
        devices.push_back({i, name, matched});
    }
    recorder.Disconnect();
    return devices;
#else
    throw std::runtime_error("This host test build has no Qualcomm backend; use the target build for cameras");
#endif
}
void autofocus(GstElement* camera) {
#ifdef IMX708_WITH_QMMF
    set_enum(camera, "focus-mode", "auto");
    qmmf::CameraMetadata* raw = nullptr;
    g_object_get(camera, "video-metadata", &raw, nullptr);
    std::unique_ptr<qmmf::CameraMetadata> metadata(raw);
    if (!metadata) throw std::runtime_error("Camera returned no autofocus metadata");
    uint8_t trigger = ANDROID_CONTROL_AF_TRIGGER_START;
    if (metadata->update(ANDROID_CONTROL_AF_TRIGGER, &trigger, 1) != 0) throw std::runtime_error("Cannot set AF trigger");
    g_object_set(camera, "video-metadata", metadata.get(), nullptr);
    // Allow the repeating request to reach the sensor before clearing START.
    // The timer owns a reference even if the UI tears down this pipeline.
    g_timeout_add_full(G_PRIORITY_DEFAULT, 200, [](gpointer data) -> gboolean {
        auto* element = GST_ELEMENT(data);
        GstState state = GST_STATE_NULL;
        gst_element_get_state(element, &state, nullptr, 0);
        if (state < GST_STATE_PAUSED) return G_SOURCE_REMOVE;
        qmmf::CameraMetadata* current = nullptr;
        g_object_get(element, "video-metadata", &current, nullptr);
        std::unique_ptr<qmmf::CameraMetadata> next(current);
        if (next) {
            const uint8_t idle = ANDROID_CONTROL_AF_TRIGGER_IDLE;
            if (next->update(ANDROID_CONTROL_AF_TRIGGER, &idle, 1) == 0)
                g_object_set(element, "video-metadata", next.get(), nullptr);
        }
        return G_SOURCE_REMOVE;
    }, gst_object_ref(camera), reinterpret_cast<GDestroyNotify>(gst_object_unref));
#else
    (void)camera;
    throw std::runtime_error("Autofocus needs the Qualcomm backend");
#endif
}
void check_still_mode(GstElement* camera) {
#ifdef IMX708_WITH_QMMF
    qmmf::CameraMetadata* raw = nullptr;
    g_object_get(camera, "static-metadata", &raw, nullptr);
    std::unique_ptr<qmmf::CameraMetadata> metadata(raw);
    if (!metadata) throw std::runtime_error("Camera returned no capabilities");
    const auto configs = metadata->find(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    // HAL_PIXEL_FORMAT_BLOB = 0x21, output direction = 0.
    for (size_t i = 0; configs.type == TYPE_INT32 && i + 3 < configs.count; i += 4)
        if (configs.data.i32[i] == 0x21 && configs.data.i32[i+1] == 4608 &&
            configs.data.i32[i+2] == 2592 && configs.data.i32[i+3] == 0) return;
    throw std::runtime_error("CamX does not advertise 4608 x 2592 JPEG capture for this sensor");
#else
    (void)camera;
    throw std::runtime_error("Still mode query needs the Qualcomm backend");
#endif
}
void capture_still(GstElement* camera) {
#ifdef IMX708_WITH_QMMF
    qmmf::CameraMetadata *image_raw = nullptr, *video_raw = nullptr;
    g_object_get(camera, "image-metadata", &image_raw, "video-metadata", &video_raw, nullptr);
    std::unique_ptr<qmmf::CameraMetadata> image(image_raw), video(video_raw);
    if (!image || !video) throw std::runtime_error("Camera returned no capture controls");
    // Preserve the still request template while applying the user's current
    // controls. Do not replace the still template with a preview request.
    std::vector<uint32_t> tags = {
        ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
        ANDROID_CONTROL_AWB_MODE, ANDROID_CONTROL_AF_MODE,
        ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_SENSITIVITY,
    };
    auto vendor = qmmf::VendorTagDescriptor::getGlobalVendorTagDescriptor();
    for (const auto* name : {"org.codeaurora.qcamera3.manualWB.color_temperature",
                             "org.codeaurora.qcamera3.manualWB.gains"}) {
        uint32_t id = 0;
        if (qmmf::CameraMetadata::getTagFromName(name, vendor.get(), &id) == 0) tags.push_back(id);
    }
    for (auto tag : tags) {
        const auto entry = video->find(tag);
        if (entry.count && image->updateImpl(tag, entry.data.u8, entry.count) != 0)
            throw std::runtime_error("Cannot apply controls to still request");
    }
    GPtrArray* requests = g_ptr_array_new();
    g_ptr_array_add(requests, image.get());
    gboolean accepted = FALSE;
    g_signal_emit_by_name(camera, "capture-image", 1U, 1U, requests, &accepted);
    g_ptr_array_unref(requests);
    if (!accepted) throw std::runtime_error("CamX rejected still capture");
#else
    (void)camera;
    throw std::runtime_error("Still capture needs the Qualcomm backend");
#endif
}
}
