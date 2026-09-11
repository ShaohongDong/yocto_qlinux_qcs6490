// SPDX-License-Identifier: MIT
#include "camera.hpp"
#include "core.hpp"
#include <gtk/gtk.h>
#include <gst/app/gstappsink.h>
#include <glib-unix.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/statvfs.h>

namespace {
using namespace imx708;
struct App {
    GtkWidget *window = nullptr, *view = nullptr, *status = nullptr;
    GtkWidget *start = nullptr, *photo = nullptr, *record = nullptr, *devices = nullptr;
    GtkWidget *controls = nullptr, *ae = nullptr, *wb = nullptr, *exposure = nullptr, *iso = nullptr, *temperature = nullptr;
    GstElement *pipeline = nullptr, *camera = nullptr;
    guint watch = 0, deadline = 0;
    Session session;
    Output picture, movie;
    std::filesystem::path picture_dir, movie_dir;
    std::atomic<bool> photo_pending {false};
    std::atomic<unsigned long> frames {0};
    unsigned long previous_frames = 0;
    int no_frames = 0, explicit_id = -1;
    bool test = false, closing = false, exercise = false, inject_error = false;
    int exercise_step = 0, exercise_ticks = 0, result = 0;
    std::vector<Device> cameras;

    void message(const std::string& text) {
        gtk_label_set_text(GTK_LABEL(status), text.c_str());
        std::cerr << text << '\n';
    }
    void buttons() {
        gtk_widget_set_sensitive(start, session.mode == Mode::idle || session.mode == Mode::error);
        gtk_widget_set_sensitive(devices, session.mode == Mode::idle || session.mode == Mode::error);
        gtk_widget_set_sensitive(photo, session.can_photo());
        gtk_widget_set_sensitive(record, session.can_record() || session.mode == Mode::recording);
        gtk_button_set_label(GTK_BUTTON(record), session.mode == Mode::recording ? "停止录像" : "开始录像");
        gtk_widget_set_sensitive(controls, !test && (session.mode == Mode::preview || session.mode == Mode::recording));
    }
    void cancel_deadline() { if (deadline) { g_source_remove(deadline); deadline = 0; } }
    void stop_pipeline() {
        cancel_deadline();
        if (watch) { g_source_remove(watch); watch = 0; }
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline); pipeline = nullptr;
        }
        if (camera) { gst_object_unref(camera); camera = nullptr; }
        photo_pending = false;
        GList* children = view ? gtk_container_get_children(GTK_CONTAINER(view)) : nullptr;
        for (auto* item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
        g_list_free(children);
    }
    void fail(const std::string& text) {
        stop_pipeline(); picture.close(); movie.close(); session.mode = Mode::error;
        message("错误：" + text + "。未完成文件保留为 .partial。"); buttons();
        if (exercise || closing) { result = 1; gtk_main_quit(); }
    }
    void safe(const std::function<void()>& action) {
        try { action(); } catch (const std::exception& error) { fail(error.what()); }
    }
    void timeout(const char* operation) {
        cancel_deadline();
        deadline = g_timeout_add_seconds(15, [](gpointer data) -> gboolean {
            auto& self = *static_cast<App*>(data); self.deadline = 0;
            self.fail("相机操作超时，请检查相机服务和驱动日志"); return G_SOURCE_REMOVE;
        }, this);
        message(operation);
    }
    unsigned selected() {
        if (test) return 0;
        const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(devices));
        if (index < 0 || static_cast<size_t>(index) >= cameras.size()) throw std::runtime_error("未找到 IMX708，请检查 CamX 传感器和调校文件");
        const auto& device = cameras[index];
        if (!device.imx708 && explicit_id != static_cast<int>(device.id))
            throw std::runtime_error("相机未报告 IMX708 身份；核实 CAM3 对应 ID 后通过 --camera-id 指定");
        return device.id;
    }
    void refresh_devices() {
        if (test) return;
        cameras = enumerate();
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(devices));
        int selected_index = -1, matches = 0;
        for (size_t i = 0; i < cameras.size(); ++i) {
            const auto& device = cameras[i];
            const auto text = "ID " + std::to_string(device.id) + " · " + device.name;
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(devices), text.c_str());
            if ((explicit_id < 0 && device.imx708) || explicit_id == static_cast<int>(device.id)) {
                selected_index = static_cast<int>(i); ++matches;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(devices), matches == 1 ? selected_index : -1);
        if (cameras.empty()) message("未发现相机，请检查 CAM3 接线和传感器配置");
    }
    void apply_controls() {
        if (!camera || test) return;
        const bool automatic = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ae));
        set_enum(camera, "exposure-mode", automatic ? "auto" : "off");
        set_enum(camera, "iso-mode", automatic ? "auto" : "manual");
        g_object_set(camera, "manual-exposure-time", static_cast<gint64>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(exposure)) * 1000000),
                     "manual-iso-value", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(iso)), nullptr);
        const auto* mode = gtk_combo_box_get_active_id(GTK_COMBO_BOX(wb));
        set_enum(camera, "white-balance-mode", mode ? mode : "auto");
        if (mode && std::string(mode) == "manual-cc-temp") {
            const auto settings = "manual-wb-settings,color_temperature=(int)" +
                std::to_string(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(temperature)));
            g_object_set(camera, "manual-wb-settings", settings.c_str(), nullptr);
        }
    }
    static GstFlowReturn sample(GstAppSink* sink, gpointer data) {
        auto& self = *static_cast<App*>(data);
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        if (!self.photo_pending.exchange(false)) { gst_sample_unref(sample); return GST_FLOW_OK; }
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map {};
        std::string error;
        if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) error = "JPEG 缓冲区不可读";
        else {
            try {
                const auto* caps = gst_sample_get_caps(sample);
                int width = 0, height = 0;
                const auto* structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
                if (!structure || !gst_structure_get_int(structure, "width", &width) ||
                    !gst_structure_get_int(structure, "height", &height) || width != 4608 || height != 2592 ||
                    map.size < 4 || map.data[0] != 0xff || map.data[1] != 0xd8)
                    throw std::runtime_error("未收到 4608 x 2592 JPEG，拒绝标记拍照成功");
                self.picture.write(map.data, map.size);
            } catch (const std::exception& exception) { error = exception.what(); }
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        if (self.test) {
            auto* gate = gst_bin_get_by_name(GST_BIN(self.pipeline), "photo_gate");
            g_object_set(gate, "drop", TRUE, nullptr); gst_object_unref(gate);
        }
        gst_element_post_message(self.pipeline, gst_message_new_application(GST_OBJECT(sink),
            gst_structure_new("imx708-photo", "error", G_TYPE_STRING, error.c_str(), nullptr)));
        return GST_FLOW_OK;
    }
    void build_pipeline(bool recording) {
        const auto id = selected();
        stop_pipeline();
        const std::string source = test ? "videotestsrc is-live=true name=camera ! " :
            "qtiqmmfsrc name=camera camera=" + std::to_string(id) + " camera.video_0 ! ";
        std::string description = source +
            "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! tee name=video "
            "video. ! queue max-size-buffers=2 leaky=downstream ! videoconvert ! gtksink name=display sync=false ";
        if (recording) description += std::string("video. ! queue ! ") +
            (test ? "x264enc tune=zerolatency speed-preset=ultrafast bitrate=10000 ! " : "v4l2h264enc ! ") +
            "h264parse ! mp4mux ! filesink name=output ";
        else if (test) description +=
            "videotestsrc is-live=true ! video/x-raw,width=4608,height=2592,framerate=1/1 ! "
            "valve name=photo_gate drop=true ! jpegenc ! appsink name=jpeg emit-signals=true sync=false async=false max-buffers=1 drop=true ";
        else description += "camera.image_1 ! image/jpeg,width=4608,height=2592 ! "
            "appsink name=jpeg emit-signals=true sync=false async=false max-buffers=1 drop=true ";
        GError* error = nullptr;
        pipeline = gst_parse_launch(description.c_str(), &error);
        if (error) {
            std::string reason = error->message; g_error_free(error); throw std::runtime_error(reason);
        }
        if (!pipeline) throw std::runtime_error("Cannot construct camera pipeline");
        camera = gst_bin_get_by_name(GST_BIN(pipeline), "camera");
        if (recording) {
            auto* output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
            g_object_set(output, "location", movie.partial().c_str(), nullptr); gst_object_unref(output);
        } else {
            auto* jpeg = gst_bin_get_by_name(GST_BIN(pipeline), "jpeg");
            g_signal_connect(jpeg, "new-sample", G_CALLBACK(sample), this); gst_object_unref(jpeg);
        }
        auto* display = gst_bin_get_by_name(GST_BIN(pipeline), "display");
        GtkWidget* widget = nullptr;
        g_object_get(display, "widget", &widget, nullptr);
        if (!widget) { gst_object_unref(display); throw std::runtime_error("Cannot create GTK preview widget"); }
        gtk_box_pack_start(GTK_BOX(view), widget, TRUE, TRUE, 0);
        gtk_widget_show(widget); g_object_unref(widget); gst_object_unref(display);
        auto* video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
        auto* pad = gst_element_get_static_pad(video, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer data) {
            ++static_cast<App*>(data)->frames; return GST_PAD_PROBE_OK;
        }, this, nullptr);
        gst_object_unref(pad); gst_object_unref(video);
        auto* bus = gst_element_get_bus(pipeline);
        watch = gst_bus_add_watch(bus, [](GstBus*, GstMessage* message, gpointer data) -> gboolean {
            auto& self = *static_cast<App*>(data);
            self.safe([&] { self.bus_message(message); });
            return G_SOURCE_CONTINUE;
        }, this);
        gst_object_unref(bus);
        if (gst_element_set_state(pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("相机服务无法打开传感器");
        if (!test) {
            set_enum(camera, "focus-mode", "continuous");
            apply_controls();
            if (!recording) check_still_mode(camera);
        }
        frames = 0; previous_frames = 0; no_frames = 0;
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("相机无法开始预览，请检查 ISP 配置");
        session.preview();
        if (recording) session.record();
        buttons();
        message(test ? "测试图案模式：不验证摄像头或 ISP" : recording ? "正在录像" : "相机预览已启动，等待图像");
    }
    void bus_message(GstMessage* event) {
        if (GST_MESSAGE_TYPE(event) == GST_MESSAGE_ERROR) {
            GError* error = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(event, &error, &debug);
            std::string text = error ? error->message : "GStreamer error";
            if (debug) std::cerr << debug << '\n';
            g_clear_error(&error); g_free(debug); throw std::runtime_error(text);
        }
        if (GST_MESSAGE_TYPE(event) == GST_MESSAGE_EOS) {
            if (session.mode != Mode::finishing) throw std::runtime_error("相机意外停止输出");
            stop_pipeline();
            const auto saved = movie.commit(); session.saved();
            message("录像已保存：" + saved.string());
            if (closing) { gtk_main_quit(); return; }
            build_pipeline(false); return;
        }
        const auto* structure = gst_message_get_structure(event);
        if (GST_MESSAGE_TYPE(event) == GST_MESSAGE_APPLICATION && structure && gst_structure_has_name(structure, "imx708-photo")) {
            const auto* error = gst_structure_get_string(structure, "error");
            if (error && *error) throw std::runtime_error(error);
            cancel_deadline();
            const auto saved = picture.commit(); session.saved(); buttons();
            message("照片已保存：" + saved.string());
        }
    }
    void capture() {
        if (!session.can_photo()) throw std::runtime_error("请先启动预览并停止录像");
        picture.reserve(picture_dir, ".jpg"); session.photo(); photo_pending = true; buttons();
        timeout("正在拍照…");
        if (test) {
            auto* gate = gst_bin_get_by_name(GST_BIN(pipeline), "photo_gate");
            g_object_set(gate, "drop", FALSE, nullptr); gst_object_unref(gate);
        } else {
            capture_still(camera);
        }
    }
    void recording() {
        if (session.mode == Mode::recording) {
            session.finish(); buttons(); timeout("正在完成录像文件…");
            if (!gst_element_send_event(pipeline, gst_event_new_eos())) throw std::runtime_error("不能结束录像");
        } else {
            if (!session.can_record()) throw std::runtime_error("请先启动预览");
            movie.reserve(movie_dir, ".mp4"); build_pipeline(true);
        }
    }
    void quit() {
        if (session.mode == Mode::recording) { closing = true; recording(); }
        else if (session.mode == Mode::finishing) closing = true;
        else { stop_pipeline(); picture.close(); gtk_main_quit(); }
    }
    void tick() {
        if (pipeline && (session.mode == Mode::preview || session.mode == Mode::recording)) {
            const auto current = frames.load();
            no_frames = current == previous_frames ? no_frames + 1 : 0;
            std::cerr << "frames=" << current << " interval_frames=" << current - previous_frames << '\n';
            previous_frames = current;
            if (no_frames >= 10) throw std::runtime_error("10 秒内没有收到新图像");
            if (session.mode == Mode::recording) {
                struct statvfs stats {};
                if (statvfs(movie_dir.c_str(), &stats) != 0 ||
                    static_cast<unsigned long long>(stats.f_bavail) * stats.f_frsize < 64ULL*1024*1024) {
                    recording(); message("空间不足，正在结束录像");
                }
            }
        }
        if (!exercise) return;
        if (++exercise_ticks > 45) throw std::runtime_error("离线媒体测试超时");
        if (exercise_step == 0 && frames > 10) { capture(); exercise_step = 1; }
        else if (exercise_step == 1 && session.mode == Mode::preview) { recording(); exercise_step = 2; }
        else if (exercise_step == 2 && frames > 90) {
            if (inject_error) {
                GError* error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ, "Injected camera service failure");
                gst_element_post_message(pipeline, gst_message_new_error(GST_OBJECT(camera), error, "synthetic failure test"));
                g_error_free(error); exercise_step = 6;
            } else { recording(); exercise_step = 3; }
        }
        else if (exercise_step == 3 && session.mode == Mode::preview) { recording(); exercise_step = 4; }
        else if (exercise_step == 4 && frames > 60) { closing = true; recording(); exercise_step = 5; }
    }
    ~App() { stop_pipeline(); }
};

GtkWidget* button(GtkWidget* box, const char* label, GCallback callback, App* app) {
    auto* widget = gtk_button_new_with_label(label);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    g_signal_connect(widget, "clicked", callback, app); return widget;
}
}

int main(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--self-test") return imx708::self_test();
        if (argument == "--help") {
            std::cout << "imx708-camera [--camera-id ID] [--test-source] [--exercise-dir DIR]\n"
                         "  --self-test: hardware-independent session checks\n"
                         "  --camera-id: explicit CAM3 binding after hardware identification\n"
                         "  --test-source: synthetic images only, no ISP validation\n"
                         "  --exercise-dir: synthetic JPEG and repeated MP4 lifecycle test\n"; return 0;
        }
        if (argument == "--test-source") app.test = true;
        else if (argument == "--exercise-failure") app.inject_error = true;
        else if (argument == "--exercise-dir" && i+1 < argc) {
            app.exercise = app.test = true; app.picture_dir = app.movie_dir = argv[++i];
        } else if (argument == "--camera-id" && i+1 < argc) {
            try {
                size_t used = 0; const std::string value = argv[++i]; app.explicit_id = std::stoi(value, &used);
                if (used != value.size() || app.explicit_id < 0 || app.explicit_id > 255) throw std::invalid_argument("id");
            } catch (...) { std::cerr << "Invalid camera ID\n"; return 2; }
        } else { std::cerr << "Unknown or incomplete option: " << argument << '\n'; return 2; }
    }
    if (app.inject_error && !app.exercise) { std::cerr << "--exercise-failure requires --exercise-dir\n"; return 2; }
    gst_init(nullptr, nullptr);
    if (!gtk_init_check(nullptr, nullptr)) { std::cerr << "需要 HDMI 图形会话（Wayland/X11）\n"; return 1; }
    if (app.picture_dir.empty()) {
        const auto* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
        const auto* videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        app.picture_dir = pictures ? std::filesystem::path(pictures) : std::filesystem::path(g_get_home_dir()) / "Pictures";
        app.movie_dir = videos ? std::filesystem::path(videos) : std::filesystem::path(g_get_home_dir()) / "Videos";
    }
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), app.test ? "IMX708 Camera — 测试图案" : "IMX708 Camera · Q6A CAM3");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1100, 720);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(app.window), root);
    auto* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
    app.devices = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(toolbar), app.devices, TRUE, TRUE, 0);
    app.start = button(toolbar, "启动预览 / 重试", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& self = *static_cast<App*>(data); self.safe([&] {
            if (self.cameras.empty()) self.refresh_devices();
            self.build_pipeline(false);
        });
    }), &app);
    app.photo = button(toolbar, "拍照", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.capture(); });
    }), &app);
    app.record = button(toolbar, "开始录像", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.recording(); });
    }), &app);
    app.view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(root), app.view, TRUE, TRUE, 0);
    app.controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(root), app.controls, FALSE, FALSE, 0);
    button(app.controls, "单次对焦", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& self = *static_cast<App*>(data); self.safe([&] { autofocus(self.camera); self.message("已请求单次对焦"); });
    }), &app);
    button(app.controls, "连续对焦", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& self = *static_cast<App*>(data); self.safe([&] { set_enum(self.camera, "focus-mode", "continuous"); });
    }), &app);
    app.ae = gtk_check_button_new_with_label("自动曝光");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.ae), TRUE);
    gtk_box_pack_start(GTK_BOX(app.controls), app.ae, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app.controls), gtk_label_new("曝光 ms"), FALSE, FALSE, 0);
    app.exposure = gtk_spin_button_new_with_range(0.1, 33.0, 0.1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.exposure), 10);
    gtk_box_pack_start(GTK_BOX(app.controls), app.exposure, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app.controls), gtk_label_new("ISO"), FALSE, FALSE, 0);
    app.iso = gtk_spin_button_new_with_range(100, 3200, 100);
    gtk_box_pack_start(GTK_BOX(app.controls), app.iso, FALSE, FALSE, 0);
    app.wb = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.wb), "auto", "自动白平衡");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.wb), "daylight", "日光");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.wb), "cloudy-daylight", "阴天");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.wb), "incandescent", "白炽灯");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.wb), "fluorescent", "荧光灯");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.wb), "manual-cc-temp", "手动色温");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app.wb), 0);
    gtk_box_pack_start(GTK_BOX(app.controls), app.wb, FALSE, FALSE, 0);
    app.temperature = gtk_spin_button_new_with_range(2000, 10000, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.temperature), 5000);
    gtk_box_pack_start(GTK_BOX(app.controls), app.temperature, FALSE, FALSE, 0);
    button(app.controls, "应用参数", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.apply_controls(); });
    }), &app);
    app.status = gtk_label_new("请启动预览");
    gtk_label_set_line_wrap(GTK_LABEL(app.status), TRUE);
    gtk_box_pack_start(GTK_BOX(root), app.status, FALSE, FALSE, 0);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.quit(); }); return TRUE;
    }), &app);
    if (app.test) {
        app.cameras.push_back({0, "Synthetic test pattern", false});
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app.devices), "测试图案（不验证硬件）");
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.devices), 0);
    } else {
        app.safe([&] { app.refresh_devices(); });
    }
    app.buttons(); gtk_widget_show_all(app.window);
    const auto timer = g_timeout_add_seconds(1, [](gpointer data) -> gboolean {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.tick(); }); return G_SOURCE_CONTINUE;
    }, &app);
    const auto sigint = g_unix_signal_add(SIGINT, [](gpointer data) -> gboolean {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.quit(); }); return G_SOURCE_CONTINUE;
    }, &app);
    const auto sigterm = g_unix_signal_add(SIGTERM, [](gpointer data) -> gboolean {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.quit(); }); return G_SOURCE_CONTINUE;
    }, &app);
    if (app.exercise) g_idle_add([](gpointer data) -> gboolean {
        auto& self = *static_cast<App*>(data); self.safe([&] { self.build_pipeline(false); }); return G_SOURCE_REMOVE;
    }, &app);
    gtk_main();
    g_source_remove(timer); g_source_remove(sigint); g_source_remove(sigterm);
    app.stop_pipeline();
    // App owns view/status widgets until its destructor has stopped pipelines.
    return app.result;
}
