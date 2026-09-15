// SPDX-License-Identifier: MIT
#include "engine.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/utsname.h>
#include <unistd.h>
#include <thread>

namespace hevc {
namespace {
constexpr size_t corpus_limit = 128 * 1024 * 1024;
struct Packet {
    std::vector<uint8_t> data;
    bool delta = false;
};
std::string device_info(const std::string& path) {
    if (path.empty()) return "null";
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return "{\"query_error\":" + quote(std::strerror(errno)) + "}";
    v4l2_capability caps{};
    int result = ioctl(fd, VIDIOC_QUERYCAP, &caps);
    int saved_errno = errno;
    close(fd);
    if (result < 0) return "{\"query_error\":" + quote(std::strerror(saved_errno)) + "}";
    return "{\"driver\":" + quote(reinterpret_cast<const char*>(caps.driver)) +
        ",\"card\":" + quote(reinterpret_cast<const char*>(caps.card)) +
        ",\"bus\":" + quote(reinterpret_cast<const char*>(caps.bus_info)) +
        ",\"version\":" + std::to_string(caps.version) + "}";
}
struct Control {
    const char* name;
    uint32_t id;
    int requested;
};
std::vector<Control> requested_controls(const Options& o, bool encoder) {
    std::vector<Control> result;
    if (encoder) result.push_back({"video_bitrate", V4L2_CID_MPEG_VIDEO_BITRATE, o.bitrate});
    bool low = o.latency_profile == "low" || o.latency_profile == (encoder ? "encode-low" : "decode-low");
    if (low) {
        result.push_back({"lowlatency_mode", 0x00992003, 1});
        if (encoder) {
            result.push_back({"video_b_frames", V4L2_CID_MPEG_VIDEO_B_FRAMES, 0});
            result.push_back({"frame_skip_mode", 0x00990b86, 0});
        }
    }
    if (!encoder && o.operating_rate)
        result.push_back({"operating_rate", 0x00992006, o.operating_rate << 16});
    return result;
}
std::string check_controls(int fd, const std::vector<Control>& controls) {
    if (fd < 0) throw std::runtime_error("Codec device-fd unavailable for control verification");
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& c : controls) {
        v4l2_control query{};
        query.id = c.id;
        if (ioctl(fd, VIDIOC_G_CTRL, &query) < 0)
            throw std::runtime_error(std::string("Cannot query active control ") + c.name + ": " + std::strerror(errno));
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << quote(c.name) << ",\"id\":" << c.id
            << ",\"requested\":" << c.requested << ",\"effective\":" << query.value << '}';
        if (query.value != c.requested)
            throw std::runtime_error(std::string("Active control mismatch: ") + c.name +
                " requested=" + std::to_string(c.requested) + " effective=" + std::to_string(query.value));
    }
    return out.str() + ']';
}
void set_controls(GstElement* element, const std::vector<Control>& controls) {
    auto* values = gst_structure_new_empty("controls");
    for (const auto& c : controls) gst_structure_set(values, c.name, G_TYPE_INT, c.requested, nullptr);
    g_object_set(element, "extra-controls", values, nullptr);
    gst_structure_free(values);
}
struct Session {
    Options options;
    std::atomic<bool>& cancel;
    View* view;
    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    std::mutex mutex;
    Frames frames;
    std::map<uint64_t, std::vector<uint8_t>> originals;
    std::vector<Packet> corpus;
    std::vector<uint8_t> raw_corpus;
    std::string raw_sha256;
    static constexpr size_t raw_frame_bytes = size_t(width) * height * 3 / 2;
    size_t corpus_bytes = 0, fed = 0, preview_count = 0;
    std::string error, enc_caps, dec_caps, enc_device, dec_device;
    std::string pipeline_text;
    std::string enc_controls = "null", dec_controls = "null";
    double start = 0, finished = 0, cpu_ms = 0, initial_cpu_ms = 0;
    long rss_kib = 0;
    bool eos = false, stopped = false;
    bool source_eos_sent = false; // Accessed only by the serialized source streaming thread.
    Session(Options o, std::atomic<bool>& c, View* v) : options(std::move(o)), cancel(c), view(v) {}
    ~Session() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        if (source) gst_object_unref(source);
    }
    bool loopback() const { return options.mode == "loopback"; }
    bool quality() const { return loopback() && options.quality_mode == "full"; }
    bool realtime() const { return options.mode != "prepare" &&
        (options.pacing == "realtime" || (options.pacing == "auto" && loopback())); }
    bool app_input() const { return options.mode == "decode" || !options.input_nv12.empty(); }
    bool has_encoder() const { return options.mode != "decode"; }
    bool has_decoder() const { return options.mode == "decode" || loopback(); }
    double begin() const { return start + options.warmup * 1000.; }
    double end() const { return begin() + options.seconds * 1000.; }
    bool measured(double time) const { return time >= begin() && time < end(); }
    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.empty()) error = message;
    }
    static void require_factory(const std::string& name, bool encoder) {
        auto* f = gst_element_factory_find(name.c_str());
        if (!f) throw std::runtime_error("Hardware element unavailable: " + name + "; inspect gst-inspect-1.0 and /dev/video* permissions/driver");
        const char* plugin = gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(f));
        const char* klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
        bool valid = plugin && std::string(plugin) == "video4linux2" && klass &&
            std::string(klass).find(encoder ? "Encoder" : "Decoder") != std::string::npos &&
            name.find("h265") != std::string::npos;
        gst_object_unref(f);
        if (!valid) throw std::runtime_error("Only explicit V4L2 HEVC hardware factories are accepted: " + name);
    }
    static std::vector<uint8_t> luma(GstBuffer* buffer, GstCaps* caps) {
        GstVideoInfo info;
        if (!caps || !gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
            throw std::runtime_error("Y-PSNR requires mappable linear 8-bit NV12; compressed UBWC is not linear NV12");
        int x = 0, y = 0;
        if (auto* crop = gst_buffer_get_video_crop_meta(buffer)) {
            if (crop->width != width || crop->height != height) throw std::runtime_error("Unexpected visible crop size");
            x = crop->x; y = crop->y;
        } else if (GST_VIDEO_INFO_WIDTH(&info) != width || GST_VIDEO_INFO_HEIGHT(&info) != height) {
            throw std::runtime_error("Expected visible 1920x1080 frame");
        }
        if (x < 0 || y < 0 || x + width > GST_VIDEO_INFO_WIDTH(&info) || y + height > GST_VIDEO_INFO_HEIGHT(&info))
            throw std::runtime_error("Crop outside frame");
        GstVideoFrame frame;
        if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) throw std::runtime_error("Cannot map NV12 frame for Y-PSNR");
        auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        if (stride < x + width) {
            gst_video_frame_unmap(&frame);
            throw std::runtime_error("Invalid Y stride");
        }
        std::vector<uint8_t> result(size_t(width) * height);
        auto* base = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        for (int row = 0; row < height; ++row)
            std::memcpy(result.data() + row * width, base + (row + y) * stride + x, width);
        gst_video_frame_unmap(&frame);
        return result;
    }
    static GstPadProbeReturn input_probe(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
        auto& s = *static_cast<Session*>(data);
        auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer) return GST_PAD_PROBE_OK;
        // Cut off at the source streaming boundary, before counting this buffer.
        // A bus-thread EOS can overtake an already-counted buffer before the
        // encoder accepts it. Serialize EOS behind every previously forwarded
        // frame instead; the execute() watchdog still bounds stalled drains.
        if (s.options.mode != "prepare" && !s.app_input() &&
            s.start > 0 && monotonic_ms() >= s.end()) {
            if (!s.source_eos_sent) {
                s.source_eos_sent = true;
                if (!gst_pad_push_event(pad, gst_event_new_eos()))
                    s.fail("Source EOS event was rejected");
            }
            return GST_PAD_PROBE_DROP;
        }
        try {
            const double before_copy = monotonic_ms();
            uint64_t pts = GST_BUFFER_PTS(buffer);
            if (pts == GST_CLOCK_TIME_NONE) throw std::runtime_error("Missing source PTS");
            std::vector<uint8_t> y;
            if (s.quality()) {
                auto* caps = gst_pad_get_current_caps(pad);
                try { y = luma(buffer, caps); }
                catch (...) { if (caps) gst_caps_unref(caps); throw; }
                if (caps) gst_caps_unref(caps);
            }
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.frames.rows.size() >= 1000000) throw std::runtime_error("Frame metadata limit exceeded");
            if (s.frames.rows.count(pts)) throw std::runtime_error("Duplicate source PTS");
            Frame f;
            f.source_index = s.frames.rows.size();
            f.source_before_copy = before_copy;
            f.source = monotonic_ms();
            s.frames.rows.emplace(pts, f);
            if (s.quality()) {
                if (s.originals.size() >= 120) throw std::runtime_error("More than 120 unmatched reference frames; refusing unbounded memory growth");
                s.originals.emplace(pts, std::move(y));
            }
        } catch (const std::exception& e) { s.fail(e.what()); return GST_PAD_PROBE_DROP; }
        return GST_PAD_PROBE_OK;
    }
    struct Probe { Session* session; double Frame::*field; bool encoder_output; };
    static GstPadProbeReturn timing_probe(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
        auto& p = *static_cast<Probe*>(data);
        auto& s = *p.session;
        auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer) return GST_PAD_PROBE_OK;
        const double time = monotonic_ms();
        auto pts = GST_BUFFER_PTS(buffer);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (p.encoder_output && s.frames.rows.count(pts))
            s.frames.rows.at(pts).encoded_bytes += gst_buffer_get_size(buffer);
        if ((p.encoder_output && s.enc_controls == "null") ||
            (p.field == &Frame::decode_out && s.dec_controls == "null")) {
            auto* codec = gst_pad_get_parent_element(pad);
            int fd = -1;
            g_object_get(codec, "device-fd", &fd, nullptr);
            gst_object_unref(codec);
            try {
                auto values = check_controls(fd, requested_controls(s.options, p.encoder_output));
                (p.encoder_output ? s.enc_controls : s.dec_controls) = values;
            } catch (const std::exception& e) {
                if (s.error.empty()) s.error = e.what();
            }
        }
        // Codec configuration buffers are not pictures and have no timing sample.
        if (p.encoder_output && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_HEADER) &&
            (!s.frames.rows.count(pts) || !gst_buffer_get_size(buffer))) return GST_PAD_PROBE_OK;
        s.frames.mark(pts, p.field, time);
        if ((p.encoder_output && s.enc_caps.empty()) || (p.field == &Frame::decode_out && s.dec_caps.empty())) {
            auto* caps = gst_pad_get_current_caps(pad);
            if (caps) {
                char* text = gst_caps_to_string(caps);
                (p.encoder_output ? s.enc_caps : s.dec_caps) = text;
                g_free(text);
                gst_caps_unref(caps);
            }
        }
        return GST_PAD_PROBE_OK;
    }
    void probe(GstElement* element, const char* pad_name, double Frame::*field, bool output = false) {
        auto* pad = gst_element_get_static_pad(element, pad_name);
        if (!pad) throw std::runtime_error("Missing codec pad");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, timing_probe, new Probe{this, field, output},
                         [](gpointer p) { delete static_cast<Probe*>(p); });
        gst_object_unref(pad);
    }
    static GstFlowReturn encoded_sample(GstAppSink* sink, gpointer data) {
        auto& s = *static_cast<Session*>(data);
        auto* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        auto* buffer = gst_sample_get_buffer(sample);
        try {
            if (s.options.mode == "prepare") {
                const size_t size = gst_buffer_get_size(buffer);
                if (s.corpus_bytes + size > corpus_limit) throw std::runtime_error("Preloaded HEVC corpus exceeds 128 MiB");
                Packet packet;
                packet.data.resize(size);
                if (gst_buffer_extract(buffer, 0, packet.data.data(), size) != size) throw std::runtime_error("Cannot read encoded buffer");
                packet.delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
                s.corpus_bytes += size;
                s.corpus.push_back(std::move(packet));
            }
        } catch (const std::exception& e) {
            s.fail(e.what()); gst_sample_unref(sample); return GST_FLOW_ERROR;
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    static GstFlowReturn decoded_sample(GstAppSink* sink, gpointer data) {
        auto& s = *static_cast<Session*>(data);
        auto* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        try {
            if (s.quality()) {
                auto* buffer = gst_sample_get_buffer(sample);
                auto decoded = luma(buffer, gst_sample_get_caps(sample));
                const uint64_t pts = GST_BUFFER_PTS(buffer);
                std::vector<uint8_t> reference;
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    auto it = s.originals.find(pts);
                    if (it == s.originals.end()) throw std::runtime_error("Decoded PTS has no original Y reference");
                    reference = std::move(it->second);
                    s.originals.erase(it);
                }
                auto result = compare_luma(reference.data(), width, decoded.data(), width, width, height);
                std::lock_guard<std::mutex> lock(s.mutex);
                auto& row = s.frames.rows.at(pts);
                row.y = result; row.quality = true;
            }
        } catch (const std::exception& e) {
            s.fail(e.what()); gst_sample_unref(sample); return GST_FLOW_ERROR;
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    static GstFlowReturn preview_sample(GstAppSink* sink, gpointer data) {
        auto& s = *static_cast<Session*>(data);
        auto* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        GstVideoInfo info;
        GstVideoFrame frame;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
            gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
            if (s.view) {
                std::lock_guard<std::mutex> lock(s.view->mutex);
                s.view->stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
                auto* p = static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                s.view->rgb.assign(p, p + s.view->stride * 360);
            }
            gst_video_frame_unmap(&frame);
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.preview_count;
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    static void need_data(GstAppSrc* src, guint, gpointer data) {
        auto& s = *static_cast<Session*>(data);
        if (s.realtime()) {
            const double deadline = s.start + s.fed * 1000. / fps;
            while (!s.cancel && monotonic_ms() < deadline && monotonic_ms() < s.end())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (s.cancel || (s.options.mode != "prepare" && monotonic_ms() >= s.end()) ||
            (s.options.mode == "prepare" && s.fed >= 60)) {
            gst_app_src_end_of_stream(src); return;
        }
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        bool delta = false;
        if (s.options.mode == "decode") {
            if (s.corpus.empty()) { s.fail("Empty preloaded HEVC corpus"); gst_app_src_end_of_stream(src); return; }
            const auto& packet = s.corpus[s.fed % s.corpus.size()];
            bytes = packet.data.data(); size = packet.data.size(); delta = packet.delta;
        } else {
            if (s.raw_corpus.empty()) { s.fail("Empty raw corpus"); gst_app_src_end_of_stream(src); return; }
            size = raw_frame_bytes;
            bytes = s.raw_corpus.data() + (s.fed % 60) * size;
        }
        auto* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        if (!buffer) { s.fail("Input allocation failed"); gst_app_src_end_of_stream(src); return; }
        gst_buffer_fill(buffer, 0, bytes, size);
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(s.fed, GST_SECOND, fps);
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, fps);
        if (delta) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        ++s.fed;
        const auto result = gst_app_src_push_buffer(src, buffer);
        if (result != GST_FLOW_OK && result != GST_FLOW_FLUSHING && result != GST_FLOW_EOS)
            s.fail("Failed to feed preloaded input");
    }
    static void connect_sink(GstElement* pipeline, const char* name, GstFlowReturn (*callback)(GstAppSink*, gpointer), Session* s) {
        auto* element = gst_bin_get_by_name(GST_BIN(pipeline), name);
        if (!element) throw std::runtime_error(std::string("Missing sink: ") + name);
        g_signal_connect(element, "new-sample", G_CALLBACK(callback), s);
        gst_object_unref(element);
    }
    void setup() {
        if (has_encoder()) require_factory(options.encoder, true);
        if (has_decoder()) require_factory(options.decoder, false);
        std::ostringstream p;
        if (options.mode == "decode") {
            p << "appsrc name=input format=time is-live=" << (realtime() ? "true" : "false")
              << " block=true max-bytes=2097152 "
                 "caps=\"video/x-h265,stream-format=byte-stream,alignment=au,width=1920,height=1080,framerate=30/1\" ! ";
        } else {
            if (!options.input_nv12.empty()) {
                if (std::filesystem::file_size(options.input_nv12) != raw_frame_bytes * 60)
                    throw std::runtime_error("Raw corpus must contain exactly 60 tightly packed 1080p NV12 frames");
                raw_corpus.resize(raw_frame_bytes * 60);
                std::ifstream input(options.input_nv12, std::ios::binary);
                input.exceptions(std::ios::failbit | std::ios::badbit);
                input.read(reinterpret_cast<char*>(raw_corpus.data()), raw_corpus.size());
                gchar* digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, raw_corpus.data(), raw_corpus.size());
                raw_sha256 = digest; g_free(digest);
                p << "appsrc name=input format=time is-live=" << (realtime() ? "true" : "false")
                  << " block=true max-bytes=3110400 caps=\"video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1,colorimetry=bt709\"";
            } else {
                p << "videotestsrc name=input pattern=ball is-live=" << (realtime() ? "true" : "false");
                if (options.mode == "prepare") p << " num-buffers=60";
            }
            p << " ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1,colorimetry=bt709 "
                 "! identity name=reference ! " << options.encoder << " name=encoder "
                 "! video/x-h265,profile=main ! h265parse config-interval=-1 "
                 "! video/x-h265,stream-format=byte-stream,alignment=au ! ";
        }
        if (has_decoder()) {
            p << options.decoder << " name=decoder ! video/x-raw,format=NV12 ! ";
            if (options.preview) p << "tee name=display ! queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 ! ";
            p << "appsink name=result emit-signals=true sync=false max-buffers=4 drop=false ";
            if (options.preview) p << "display. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 "
                "! videoconvertscale ! video/x-raw,format=RGB,width=640,height=360 "
                "! appsink name=preview emit-signals=true sync=false max-buffers=1 drop=true ";
        } else p << "appsink name=result emit-signals=true sync=false max-buffers=4 drop=false";
        pipeline_text = p.str();
        GError* parse_error = nullptr;
        pipeline = gst_parse_launch(pipeline_text.c_str(), &parse_error);
        if (parse_error) {
            std::string message = parse_error->message;
            g_error_free(parse_error);
            throw std::runtime_error(message);
        }
        if (!pipeline) throw std::runtime_error("Pipeline creation failed");
        source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        GstElement* reference = options.mode == "decode" ? GST_ELEMENT(gst_object_ref(source)) :
            gst_bin_get_by_name(GST_BIN(pipeline), "reference");
        auto* source_pad = gst_element_get_static_pad(reference, "src");
        gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER, input_probe, this, nullptr);
        gst_object_unref(source_pad); gst_object_unref(reference);
        if (has_encoder()) {
            auto* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
            set_controls(encoder, requested_controls(options, true));
            probe(encoder, "sink", &Frame::encode_in);
            probe(encoder, "src", &Frame::encode_out, true);
            gchar* device = nullptr;
            g_object_get(encoder, "device", &device, nullptr);
            enc_device = device ? device : "";
            g_free(device); gst_object_unref(encoder);
        }
        if (has_decoder()) {
            auto* decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
            set_controls(decoder, requested_controls(options, false));
            probe(decoder, "sink", &Frame::decode_in);
            probe(decoder, "src", &Frame::decode_out);
            gchar* device = nullptr;
            g_object_get(decoder, "device", &device, nullptr);
            dec_device = device ? device : "";
            g_free(device); gst_object_unref(decoder);
        }
        if (app_input()) g_signal_connect(source, "need-data", G_CALLBACK(need_data), this);
        connect_sink(pipeline, "result", has_decoder() ? decoded_sample : encoded_sample, this);
        if (options.preview && has_decoder()) connect_sink(pipeline, "preview", preview_sample, this);
    }
    std::string progress() {
        std::lock_guard<std::mutex> lock(mutex);
        size_t enc = 0, dec = 0, q = 0, pending = 0;
        double et = 0, dt = 0;
        LumaError y;
        for (const auto& item : frames.rows) {
            const auto& f = item.second;
            if (!measured(f.source)) continue;
            if ((has_encoder() && f.encode_out < 0) || (has_decoder() && f.decode_out < 0)) ++pending;
            if (f.encode_out >= f.encode_in && f.encode_in >= 0) { ++enc; et += f.encode_out - f.encode_in; }
            if (f.decode_out >= f.decode_in && f.decode_in >= 0) { ++dec; dt += f.decode_out - f.decode_in; }
            if (f.quality) { ++q; y.squared += f.y.squared; y.pixels += f.y.pixels; }
        }
        double sec = std::clamp((monotonic_ms() - begin()) / 1000., 0., double(options.seconds));
        std::ostringstream text;
        text << options.mode << "  " << std::fixed << std::setprecision(1) << sec << "/" << options.seconds << " s\n"
             << "Encode: " << enc << " frames, " << (sec > 0 ? enc / sec : 0) << " fps, " << (enc ? et / enc : 0) << " ms\n"
             << "Decode: " << dec << " frames, " << (sec > 0 ? dec / sec : 0) << " fps, " << (dec ? dt / dec : 0) << " ms\n"
             << "Y-PSNR: " << (q ? number(y.psnr()) : "pending") << " dB";
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        double cpu = usage.ru_utime.tv_sec * 1000. + usage.ru_utime.tv_usec / 1000. +
                     usage.ru_stime.tv_sec * 1000. + usage.ru_stime.tv_usec / 1000.;
        text << "\nCPU: " << (start > 0 ? (cpu - initial_cpu_ms) * 100. / std::max(1., monotonic_ms() - start) : 0)
             << "% (100%/core), peak RSS: " << usage.ru_maxrss / 1024. << " MiB"
             << "\nIn flight: " << pending << ", unmatched: " << frames.unmatched << ", duplicate: " << frames.duplicates;
        if (!error.empty()) text << "\nERROR: " << error;
        return text.str();
    }
    void execute() {
        setup();
        rusage before{}, after{};
        getrusage(RUSAGE_SELF, &before);
        initial_cpu_ms = before.ru_utime.tv_sec * 1000. + before.ru_utime.tv_usec / 1000. + before.ru_stime.tv_sec * 1000. + before.ru_stime.tv_usec / 1000.;
        start = monotonic_ms();
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Hardware pipeline refused PLAYING state");
        auto* bus = gst_element_get_bus(pipeline);
        double last_update = 0;
        while (true) {
            auto* message = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND,
                GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (message) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) eos = true;
                else {
                    GError* detail = nullptr; gchar* debug = nullptr;
                    gst_message_parse_error(message, &detail, &debug);
                    fail(std::string(GST_OBJECT_NAME(message->src)) + ": " + (detail ? detail->message : "unknown error") +
                         (debug ? std::string("\n") + debug : ""));
                    if (detail) g_error_free(detail);
                    g_free(debug);
                }
                gst_message_unref(message);
                break;
            }
            double now = monotonic_ms();
            { std::lock_guard<std::mutex> lock(mutex); if (!error.empty()) break; }
            if (now - last_update >= 1000) {
                auto text = progress();
                if (view) { std::lock_guard<std::mutex> lock(view->mutex); view->text = text; }
                else std::cout << text << std::endl;
                last_update = now;
            }
            if (cancel) { stopped = true; break; }
            double deadline = options.mode == "prepare" ? start + 30000 : end() + 15000;
            if (now > deadline) { fail("Pipeline timeout while waiting for data/EOS drain"); break; }
        }
        finished = monotonic_ms();
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        getrusage(RUSAGE_SELF, &after);
        auto millis = [](const timeval& t) { return t.tv_sec * 1000. + t.tv_usec / 1000.; };
        cpu_ms = millis(after.ru_utime) + millis(after.ru_stime) - millis(before.ru_utime) - millis(before.ru_stime);
        rss_kib = after.ru_maxrss;
    }
    std::string report(bool& passed) {
        size_t ui_submitted = 0, ui_pending = 0;
        if (view) {
            std::lock_guard<std::mutex> lock(view->mutex);
            ui_submitted = view->submitted_frames;
            ui_pending = view->rgb.empty() ? 0 : 1;
        }
        std::lock_guard<std::mutex> lock(mutex);
        Distribution enc, dec, roundtrip, before_copy_roundtrip, psnr;
        uint64_t encoded_window_bytes = 0;
        LumaError total_y;
        size_t encoded = 0, decoded = 0, enc_window = 0, dec_window = 0, quality_count = 0;
        size_t missing_encode = 0, missing_decode = 0, missing_quality = 0, invalid_timing = 0;
        std::ostringstream rows;
        bool first = true;
        for (const auto& item : frames.rows) {
            const auto& f = item.second;
            if ((f.encode_out >= 0 && (f.encode_in < 0 || f.encode_out < f.encode_in)) ||
                (f.decode_out >= 0 && (f.decode_in < 0 || f.decode_out < f.decode_in))) ++invalid_timing;
            if (f.encode_out >= 0) ++encoded;
            if (f.decode_out >= 0) ++decoded;
            if (measured(f.encode_out)) { ++enc_window; encoded_window_bytes += f.encoded_bytes; }
            if (measured(f.decode_out)) ++dec_window;
            if (has_encoder() && f.encode_out < 0) ++missing_encode;
            if (has_decoder() && f.decode_out < 0) ++missing_decode;
            if (quality() && !f.quality) ++missing_quality;
            if (!measured(f.source)) continue;
            double et = f.encode_in >= 0 && f.encode_out >= f.encode_in ? f.encode_out - f.encode_in : NAN;
            double dt = f.decode_in >= 0 && f.decode_out >= f.decode_in ? f.decode_out - f.decode_in : NAN;
            if (std::isfinite(et)) enc.values.push_back(et);
            if (std::isfinite(dt)) dec.values.push_back(dt);
            if (f.decode_out >= f.source) roundtrip.values.push_back(f.decode_out - f.source);
            if (f.source_before_copy >= 0 && f.decode_out >= f.source_before_copy)
                before_copy_roundtrip.values.push_back(f.decode_out - f.source_before_copy);
            if (f.quality) {
                ++quality_count;
                total_y.squared += f.y.squared; total_y.pixels += f.y.pixels;
                psnr.values.push_back(f.y.psnr());
            }
            if (!first) rows << ',';
            first = false;
            rows << "{\"pts_ns\":" << item.first << ",\"source_index\":" << f.source_index
                 << ",\"y_squared_error\":" << f.y.squared << ",\"y_pixels\":" << f.y.pixels
                 << ",\"encode_ms\":" << number(et)
                 << ",\"source_to_decode_ms\":" << number(f.decode_out >= f.source ? f.decode_out - f.source : NAN)
                 << ",\"before_copy_to_decode_ms\":" << number(f.source_before_copy >= 0 && f.decode_out >= f.source_before_copy ? f.decode_out - f.source_before_copy : NAN)
                 << ",\"source_monotonic_ms\":" << number(f.source)
                 << ",\"encode_in_monotonic_ms\":" << number(f.encode_in)
                 << ",\"encode_out_monotonic_ms\":" << number(f.encode_out)
                 << ",\"decode_in_monotonic_ms\":" << number(f.decode_in)
                 << ",\"decode_out_monotonic_ms\":" << number(f.decode_out)
                 << ",\"decode_ms\":" << number(dt) << ",\"y_psnr_db\":" << (f.quality ? number(f.y.psnr()) : "null") << '}';
        }
        double efps = enc_window / double(options.seconds), dfps = dec_window / double(options.seconds);
        const double threshold = realtime() ? 29.7 : 30.;
        passed = error.empty() && eos && !cancel && finished >= end() && !invalid_timing && !frames.unmatched && !frames.duplicates &&
            !frames.rows.empty() && !missing_encode && !missing_decode && !missing_quality &&
            (!has_encoder() || efps >= threshold) && (!has_decoder() || dfps >= threshold) &&
            (!quality() || quality_count > 0);
        if (!passed && error.empty() && !cancel) {
            std::ostringstream reason;
            reason << "Acceptance failed: encode_fps=" << efps << ", decode_fps=" << dfps
                   << ", threshold=" << threshold << ", missing_encode=" << missing_encode
                   << ", missing_decode=" << missing_decode << ", missing_quality=" << missing_quality
                   << ", unmatched=" << frames.unmatched << ", duplicates=" << frames.duplicates
                   << ", invalid_timing=" << invalid_timing << ", full_window=" << (finished >= end()) << ", eos=" << eos;
            error = reason.str();
        }
        utsname system{}; uname(&system);
        gchar* version = gst_version_string();
        std::string version_text = version;
        g_free(version);
        std::ostringstream out;
        out << "{\"schema_version\":1,\"status\":" << quote(cancel ? "CANCELLED" : passed ? "PASS" : "FAIL")
            << ",\"error\":" << quote(error) << ",\"mode\":" << quote(options.mode)
            << ",\"platform\":" << quote(std::string(system.machine) + " " + system.release + " " + system.nodename)
            << ",\"gstreamer\":" << quote(version_text)
            << ",\"width\":1920,\"height\":1080,\"input_fps\":30,\"requested_bitrate\":" << options.bitrate
            << ",\"warmup_seconds\":" << options.warmup << ",\"measurement_seconds\":" << options.seconds
            << ",\"elapsed_seconds\":" << number(start > 0 ? (finished - start) / 1000 : 0)
            << ",\"encoder\":" << quote(has_encoder() ? options.encoder : "") << ",\"decoder\":" << quote(has_decoder() ? options.decoder : "")
            << ",\"encoder_device\":" << quote(enc_device) << ",\"decoder_device\":" << quote(dec_device)
            << ",\"encoder_driver\":" << device_info(enc_device) << ",\"decoder_driver\":" << device_info(dec_device)
            << ",\"invalid_timing_frames\":" << invalid_timing
            << ",\"encoded_caps\":" << quote(enc_caps) << ",\"decoded_caps\":" << quote(dec_caps)
            << ",\"pipeline\":" << quote(pipeline_text)
            << ",\"eos\":" << (eos ? "true" : "false")
            << ",\"input_frames\":" << frames.rows.size() << ",\"encoded_frames\":" << encoded
            << ",\"decoded_frames\":" << decoded << ",\"missing_encoded_frames\":" << missing_encode
            << ",\"missing_decoded_frames\":" << missing_decode << ",\"missing_quality_frames\":" << missing_quality
            << ",\"unmatched_timestamps\":" << frames.unmatched << ",\"duplicate_timestamps\":" << frames.duplicates
            << ",\"encode_fps\":" << (has_encoder() ? number(efps) : "null") << ",\"decode_fps\":" << (has_decoder() ? number(dfps) : "null")
            << ",\"encode_latency_ms\":" << enc.json() << ",\"decode_latency_ms\":" << dec.json()
            << ",\"source_to_decode_ms\":" << roundtrip.json()
            << ",\"before_copy_to_decode_ms\":" << before_copy_roundtrip.json()
            << ",\"pacing\":" << quote(realtime() ? "realtime" : "unpaced")
            << ",\"quality_mode\":" << quote(options.quality_mode)
            << ",\"quality_validated\":" << (quality() && quality_count ? "true" : "false")
            << ",\"input_nv12\":" << quote(options.input_nv12)
            << ",\"input_sha256\":" << quote(raw_sha256)
            << ",\"latency_profile\":" << quote(options.latency_profile)
            << ",\"requested_operating_rate\":" << options.operating_rate
            << ",\"encoder_controls\":" << enc_controls << ",\"decoder_controls\":" << dec_controls
            << ",\"actual_encoded_bitrate\":" << (has_encoder() ? number(encoded_window_bytes * 8. / options.seconds) : "null")
            << ",\"y_psnr_db\":" << psnr.json() << ",\"y_psnr_from_pooled_mse_db\":" << number(total_y.psnr())
            << ",\"preview_enabled\":" << (options.preview ? "true" : "false")
            << ",\"ui_submitted_frames\":" << ui_submitted << ",\"ui_pending_frames\":" << ui_pending
            << ",\"ui_coalesced_frames\":" << (preview_count - std::min(preview_count, ui_submitted + ui_pending))
            << ",\"preview_frames\":" << preview_count << ",\"preview_skipped_frames\":" << (options.preview ? decoded - std::min(decoded, preview_count) : 0)
            << ",\"process_cpu_percent_one_core\":" << number(finished > start ? cpu_ms * 100. / (finished - start) : 0)
            << ",\"process_lifetime_peak_rss_kib\":" << rss_kib
            << ",\"corpus_encoder\":" << quote(options.mode == "decode" ? options.encoder : "")
            << ",\"decode_corpus_frames\":" << corpus.size() << ",\"decode_corpus_bytes\":" << corpus_bytes
            << ",\"latency_definition\":\"monotonic codec sink-to-src matched by PTS; includes driver queueing, excludes source Y copy; not hardware core time\""
            << ",\"quality_definition\":\"visible 8-bit Y code values without range conversion; loopback only; no quality threshold\""
            << ",\"frames\":[" << rows.str() << "]}\n";
        return out.str();
    }
};
}
int run(const Options& options, std::atomic<bool>& cancel, View* view) {
    gst_init(nullptr, nullptr);
    Session session(options, cancel, view);
    std::filesystem::path directory;
    try {
        auto stamp = std::chrono::system_clock::now().time_since_epoch();
        auto id = std::chrono::duration_cast<std::chrono::microseconds>(stamp).count();
        directory = std::filesystem::path(options.output) / (options.mode + "-" + std::to_string(id) + "-" + std::to_string(getpid()));
        std::filesystem::create_directories(directory);
        if (options.mode == "decode") {
            Options preparation = options;
            preparation.mode = "prepare"; preparation.preview = false;
            Session prepare(preparation, cancel, view);
            prepare.execute();
            if (!prepare.error.empty() || !prepare.eos || prepare.corpus.size() != 60 || prepare.corpus.front().delta || cancel)
                throw std::runtime_error("Hardware decode corpus preparation failed: " + prepare.error);
            session.corpus = std::move(prepare.corpus);
            session.corpus_bytes = prepare.corpus_bytes;
            session.raw_sha256 = prepare.raw_sha256;
            session.enc_caps = prepare.enc_caps;
            session.enc_device = prepare.enc_device;
            std::ofstream corpus(directory / "input.h265", std::ios::binary);
            corpus.exceptions(std::ios::badbit | std::ios::failbit);
            for (const auto& packet : session.corpus)
                corpus.write(reinterpret_cast<const char*>(packet.data.data()), packet.data.size());
        }
        session.execute();
    } catch (const std::exception& e) {
        session.fail(e.what());
        if (session.pipeline) gst_element_set_state(session.pipeline, GST_STATE_NULL);
        session.finished = monotonic_ms();
    }
    bool passed = false;
    auto json = session.report(passed);
    try {
        if (directory.empty()) throw std::runtime_error("No report directory");
        std::ofstream report(directory / "report.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << json;
        report.close();
        std::ofstream log(directory / "run.log");
        log.exceptions(std::ios::badbit | std::ios::failbit);
        log << session.pipeline_text << '\n' << session.error << '\n' << json;
    } catch (const std::exception& e) {
        passed = false;
        session.fail(std::string("Cannot save report: ") + e.what());
    }
    std::string result = (cancel ? "CANCELLED" : passed ? "PASS" : "FAIL") + std::string("\nReport: ") + directory.string() + "/report.json\n" + session.error;
    std::cout << result << std::endl;
    if (view) { std::lock_guard<std::mutex> lock(view->mutex); view->text = session.progress() + "\n" + result; }
    return cancel ? 2 : passed ? 0 : 1;
}
}
