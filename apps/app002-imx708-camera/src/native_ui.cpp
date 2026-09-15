// SPDX-License-Identifier: MIT
#include "native.hpp"
#include "core.hpp"
#include "instance.hpp"
#include <gtk/gtk.h>
#include <gst/app/gstappsrc.h>
#include <glib-unix.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace imx708 {
namespace {
using Clock = std::chrono::steady_clock;
std::string save_frame(const std::filesystem::path& directory, const NativeFrame& frame, int pattern) {
    Output jpg, raw, json;
    jpg.reserve(directory, ".jpg");
    raw.reserve_related(jpg.partial(), ".raw"); json.reserve_related(jpg.partial(), ".json");
    raw.write(frame.raw.data(), frame.raw.size());
    auto* pixbuf = gdk_pixbuf_new_from_data(frame.rgb.data(), GDK_COLORSPACE_RGB, FALSE, 8,
                                           frame.width / 2, frame.height / 2, frame.width / 2 * 3, nullptr, nullptr);
    if (!pixbuf) throw std::runtime_error("Cannot create photo buffer");
    gchar* data = nullptr; gsize length = 0; GError* error = nullptr;
    const bool saved = gdk_pixbuf_save_to_buffer(pixbuf, &data, &length, "jpeg", &error, "quality", "95", nullptr);
    g_object_unref(pixbuf);
    if (!saved) {
        std::string reason = error ? error->message : "JPEG encoding failed";
        g_clear_error(&error); g_free(data); throw std::runtime_error(reason);
    }
    try { jpg.write(data, length); } catch (...) { g_free(data); throw; }
    g_free(data);
    std::ostringstream metadata;
    metadata << "{\n  \"capture_complete\": true, \"backend\": \"native\", \"format\": \"pRAA\",\n"
             << "  \"width\": " << frame.width << ", \"height\": " << frame.height
             << ", \"stride\": " << frame.stride << ", \"bytes\": " << frame.raw.size()
             << ", \"sequence\": " << frame.sequence << ", \"timestamp_monotonic_ns\": " << frame.timestamp_ns
             << ",\n  \"jpeg_width\": " << frame.width / 2 << ", \"jpeg_height\": " << frame.height / 2
             << ", \"exposure_lines\": " << frame.settings.exposure << ", \"analogue_gain_code\": " << frame.settings.gain
             << ", \"black_level\": " << frame.settings.black << ", \"red_gain\": " << frame.settings.red
             << ", \"blue_gain\": " << frame.settings.blue << ", \"gamma\": 2.2, \"test_pattern\": " << pattern << "\n}\n";
    const auto text = metadata.str(); json.write(text.data(), text.size());
    raw.commit(); json.commit(); // Publish the JPEG only after both companions exist.
    return jpg.commit();
}
struct NativeApp {
    GtkWidget *window = nullptr, *view = nullptr, *status = nullptr, *start_button = nullptr,
              *stop_button = nullptr, *photo_button = nullptr, *settings_box = nullptr,
              *exposure = nullptr, *gain = nullptr, *black = nullptr, *red = nullptr, *blue = nullptr;
    GstElement *pipeline = nullptr, *source = nullptr;
    NativeCapture capture;
    std::filesystem::path output;
    std::atomic<unsigned long> displayed {0};
    std::mutex mutex;
    std::string pending_error;
    bool running = false, closing = false, saving = false, first_start = true;
    bool streaming_seen = false;
    RawSettings exercise_baseline;
    std::atomic<bool> photo_requested {false};
    std::atomic<uint64_t> photo_after_ns {0};
    std::unique_ptr<NativeFrame> photo_frame;
    uint64_t first_ts = 0;
    std::future<std::string> save_job;
    unsigned long last_displayed = 0, last_captured = 0;
    Clock::time_point tick_time, run_start;
    unsigned exercise_seconds = 0, exercise_cycle = 0, exercise_photos = 0;
    int result = 0;
    void message(const std::string& text) { gtk_label_set_text(GTK_LABEL(status), text.c_str()); std::cerr << text << '\n'; }
    void buttons() {
        gtk_widget_set_sensitive(start_button, !running && !saving);
        gtk_widget_set_sensitive(stop_button, running);
        gtk_widget_set_sensitive(photo_button, running && !saving && !capture.test_pattern);
        gtk_widget_set_sensitive(settings_box, running);
    }
    RawSettings settings() {
        RawSettings s;
        s.exposure = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(exposure));
        s.gain = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(gain));
        s.black = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(black));
        s.red = gtk_spin_button_get_value(GTK_SPIN_BUTTON(red));
        s.blue = gtk_spin_button_get_value(GTK_SPIN_BUTTON(blue));
        return s;
    }
    void apply() { capture.settings(settings()); message("已请求应用手动参数（黑电平与颜色未经标定）"); }
    void stop() {
        capture.stop(); photo_requested = false;
        { std::lock_guard<std::mutex> lock(mutex); photo_frame.reset(); pending_error.clear(); }
        running = false;
        if (pipeline) { gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(pipeline); pipeline = nullptr; }
        if (source) { gst_object_unref(source); source = nullptr; }
        if (view) {
            auto* children = gtk_container_get_children(GTK_CONTAINER(view));
            for (auto* item = children; item; item = item->next) gtk_widget_destroy(GTK_WIDGET(item->data));
            g_list_free(children);
        }
        if (!save_job.valid()) saving = false;
    }
    void fail(const std::string& error) {
        stop(); message("错误：" + error + "；可重新启动预览，未完成照片保留为 .partial。"); buttons();
        if (exercise_seconds) { result = 1; closing = true; }
    }
    template<class F> void safe(F action) { try { action(); } catch (const std::exception& e) { fail(e.what()); } }
    void start() {
        stop(); capture.prepare();
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(exposure), capture.exposure.minimum, capture.exposure.maximum);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(exposure), capture.exposure.step, capture.exposure.step * 10);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(gain), capture.gain.minimum, capture.gain.maximum);
        gtk_spin_button_set_increments(GTK_SPIN_BUTTON(gain), capture.gain.step, capture.gain.step * 10);
        if (first_start) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(exposure), capture.exposure.value);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(gain), capture.gain.value); first_start = false;
        }
        capture.settings(settings());
        GError* error = nullptr;
        pipeline = gst_parse_launch("appsrc name=raw is-live=true format=time block=false max-buffers=2 max-bytes=0 max-time=0 leaky-type=downstream ! videoconvert ! gtksink name=display sync=false", &error);
        if (error) { std::string reason = error->message; g_error_free(error); throw std::runtime_error(reason); }
        if (!pipeline) throw std::runtime_error("Cannot construct native preview");
        source = gst_bin_get_by_name(GST_BIN(pipeline), "raw");
        auto* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT, 1152,
                                         "height", G_TYPE_INT, 648, "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
        auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "display");
        // Override the native image's preferred widget size to fit the 1024x600 screen.
        g_object_set(sink, "force-aspect-ratio", TRUE, nullptr);
        GtkWidget* widget = nullptr; g_object_get(sink, "widget", &widget, nullptr);
        if (!widget) { gst_object_unref(sink); throw std::runtime_error("GTK preview widget unavailable"); }
        gtk_widget_set_size_request(widget, 320, 180);
        gtk_box_pack_start(GTK_BOX(view), widget, TRUE, TRUE, 0); gtk_widget_show(widget); g_object_unref(widget);
        auto* pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer data) {
            ++static_cast<NativeApp*>(data)->displayed; return GST_PAD_PROBE_OK;
        }, this, nullptr);
        gst_object_unref(pad); gst_object_unref(sink);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Native preview could not start");
        first_ts = 0; displayed = 0; last_displayed = last_captured = 0; streaming_seen = false;
        tick_time = run_start = Clock::now(); running = true;
        capture.start([this](NativeFrame&& frame) {
            if (!first_ts) first_ts = frame.timestamp_ns;
            auto* buffer = gst_buffer_new_allocate(nullptr, frame.rgb.size(), nullptr);
            if (!buffer) throw std::runtime_error("Preview buffer allocation failed");
            gst_buffer_fill(buffer, 0, frame.rgb.data(), frame.rgb.size());
            GST_BUFFER_PTS(buffer) = frame.timestamp_ns - first_ts;
            GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
            const auto flow = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
            if (flow != GST_FLOW_OK) throw std::runtime_error("Preview pipeline stopped accepting frames");
            if (photo_requested.load() && frame.timestamp_ns > photo_after_ns.load() && photo_requested.exchange(false)) {
                std::lock_guard<std::mutex> lock(mutex);
                photo_frame = std::make_unique<NativeFrame>(std::move(frame));
            }
        }, [this](std::string error_text) { std::lock_guard<std::mutex> lock(mutex); pending_error = std::move(error_text); });
        message(capture.test_pattern ? "传感器色条验证（非真实画面）" : "原生预览：手动曝光；无自动对焦、自动白平衡或录像");
        buttons();
    }
    void photo() {
        if (!running || saving || capture.test_pattern) return;
        saving = true;
        photo_after_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        photo_requested = true; buttons(); message("正在保存下一帧 JPEG + RAW…");
    }
    void tick() {
        std::string error;
        std::unique_ptr<NativeFrame> frame;
        { std::lock_guard<std::mutex> lock(mutex); error.swap(pending_error); frame = std::move(photo_frame); }
        if (!error.empty()) throw std::runtime_error(error);
        if (frame) {
            const auto directory = output; const auto pattern = capture.test_pattern;
            save_job = std::async(std::launch::async, [directory, pattern, frame = std::move(frame)] { return save_frame(directory, *frame, pattern); });
        }
        if (save_job.valid() && save_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            saving = false;
            const auto file = save_job.get(); message("照片已保存：" + file); ++exercise_photos; buttons();
            if (exercise_seconds && exercise_cycle == 5) {
                // Exercise the same GTK controls and Apply path as a user.
                if (exercise_photos == 1) {
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(gain),
                        std::min(capture.gain.maximum, std::max(512, exercise_baseline.gain)));
                } else if (exercise_photos == 2) {
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(gain), exercise_baseline.gain);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(exposure),
                        std::max(capture.exposure.minimum, exercise_baseline.exposure / 4));
                } else {
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(exposure), exercise_baseline.exposure);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(gain), exercise_baseline.gain);
                }
                apply();
            }
        }
        if (closing) { if (!saving) gtk_main_quit(); return; }
        if (!running) return;
        if (!streaming_seen && displayed > 0) {
            streaming_seen = true; run_start = Clock::now();
            if (exercise_cycle == 5) exercise_baseline = settings();
        }
        auto* bus = gst_element_get_bus(pipeline);
        auto* msg = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        gst_object_unref(bus);
        if (msg) {
            std::string reason = "Unexpected preview EOS";
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* e = nullptr; gchar* debug = nullptr; gst_message_parse_error(msg, &e, &debug);
                reason = e ? e->message : "Preview error"; g_clear_error(&e); g_free(debug);
            }
            gst_message_unref(msg); throw std::runtime_error(reason);
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - tick_time).count();
        if (elapsed >= 1) {
            const auto d = displayed.load(), c = capture.frames.load();
            std::cerr << "native captured=" << c << " displayed=" << d << " capture_fps=" << (c-last_captured)/elapsed
                      << " display_fps=" << (d-last_displayed)/elapsed << " bad=" << capture.bad_frames
                      << " gaps=" << capture.gaps << '\n';
            last_displayed = d; last_captured = c; tick_time = Clock::now();
        }
        if (exercise_seconds && streaming_seen) {
            const auto seconds = std::chrono::duration<double>(Clock::now() - run_start).count();
            if (exercise_cycle < 5 && seconds > 2 && capture.frames >= 30 && displayed >= 20 && !saving) { ++exercise_cycle; start(); }
            else if (exercise_cycle == 5 && seconds > 3 + exercise_photos * 2 && exercise_photos < 3 && !saving) photo();
            else if (exercise_cycle == 5 && seconds >= exercise_seconds && exercise_photos >= 3 && !saving) {
                stop(); closing = true; message("原生预览自动启停/拍照运行结束");
            }
        }
    }
    void quit() { closing = true; stop(); buttons(); if (!saving) gtk_main_quit(); }
    ~NativeApp() { stop(); if (save_job.valid()) save_job.wait(); }
};
GtkWidget* spin(GtkWidget* box, const char* title, double min, double max, double step, double value) {
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(title), FALSE, FALSE, 0);
    auto* widget = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value);
    gtk_entry_set_width_chars(GTK_ENTRY(widget), 5);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0); return widget;
}
GtkWidget* button(GtkWidget* box, const char* text, GCallback callback, NativeApp* app) {
    auto* widget = gtk_button_new_with_label(text);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0); g_signal_connect(widget, "clicked", callback, app); return widget;
}
}
int native_main(int argc, char** argv) {
    NativeApp app; bool autostart = false, maximized = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help") {
            std::cout << "imx708-camera --backend=native [--output-dir DIR] [--native-autostart] [--maximized]\n"
                         "  --native-test-pattern: sensor colour bars, disables photos\n"
                         "  --native-exercise-seconds N: five restarts, three photos, then N seconds of real preview (10..3600)\n";
            return 0;
        }
        if (arg == "--backend=native") continue;
        if (arg == "--output-dir" && i+1 < argc) app.output = argv[++i];
        else if (arg == "--maximized") maximized = true;
        else if (arg == "--native-autostart") autostart = true;
        else if (arg == "--native-test-pattern") app.capture.test_pattern = 1;
        else if (arg == "--native-exercise-seconds" && i+1 < argc) {
            try {
                size_t used = 0; const std::string value(argv[++i]); const int seconds = std::stoi(value, &used);
                if (used != value.size() || seconds < 10 || seconds > 3600) throw std::invalid_argument("duration");
                app.exercise_seconds = seconds; autostart = true;
            } catch (...) { std::cerr << "Invalid native exercise duration\n"; return 2; }
        } else { std::cerr << "Unsupported native option: " << arg << '\n'; return 2; }
    }
    if (app.capture.test_pattern && app.exercise_seconds) { std::cerr << "Exercise requires real-scene mode\n"; return 2; }
    std::unique_ptr<InstanceLock> instance;
    try {
        instance = std::make_unique<InstanceLock>(std::filesystem::path(g_get_user_runtime_dir()) / "imx708-native.lock");
        if (!instance->held()) { std::cerr << "原生预览已运行\n"; return 0; }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    gst_init(nullptr, nullptr);
    if (!gtk_init_check(nullptr, nullptr)) { std::cerr << "需要 HDMI 图形会话\n"; return 1; }
    if (app.output.empty()) {
        const auto* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
        app.output = pictures ? std::filesystem::path(pictures) : std::filesystem::path(g_get_home_dir()) / "Pictures";
    }
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(app.window), "IMX708 · 原生预览");
    // Leave space for Weston panel and client-side window decorations at 600p.
    gtk_window_set_default_size(GTK_WINDOW(app.window), 960, 480);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); gtk_container_set_border_width(GTK_CONTAINER(root), 6);
    gtk_container_add(GTK_CONTAINER(app.window), root);
    auto* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
    app.start_button = button(toolbar, "启动 / 重试", G_CALLBACK(+[](GtkButton*, gpointer p) { auto& a=*static_cast<NativeApp*>(p); a.safe([&]{a.start();}); }), &app);
    app.stop_button = button(toolbar, "停止预览", G_CALLBACK(+[](GtkButton*, gpointer p) { auto& a=*static_cast<NativeApp*>(p); a.stop(); a.buttons(); a.message("预览已停止"); }), &app);
    app.photo_button = button(toolbar, "拍照", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<NativeApp*>(p)->photo(); }), &app);
    auto* unavailable = gtk_label_new("CAM3 · 录像 / 自动曝光 / 自动白平衡 / 对焦暂不可用");
    gtk_box_pack_start(GTK_BOX(toolbar), unavailable, FALSE, FALSE, 0);
    app.view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_box_pack_start(GTK_BOX(root), app.view, TRUE, TRUE, 0);
    app.settings_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4); gtk_box_pack_start(GTK_BOX(root), app.settings_box, FALSE, FALSE, 0);
    app.exposure = spin(app.settings_box, "曝光(行)", 1, 1306, 1, 1306);
    app.gain = spin(app.settings_box, "增益码", 112, 960, 1, 112);
    app.black = spin(app.settings_box, "黑电平", 0, 1022, 1, 64);
    app.red = spin(app.settings_box, "红", 0.1, 8, 0.1, 1); app.blue = spin(app.settings_box, "蓝", 0.1, 8, 0.1, 1);
    button(app.settings_box, "应用", G_CALLBACK(+[](GtkButton*, gpointer p) { auto& a=*static_cast<NativeApp*>(p); a.safe([&]{a.apply();}); }), &app);
    app.status = gtk_label_new("请启动预览；软件显色未经标定"); gtk_label_set_ellipsize(GTK_LABEL(app.status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(root), app.status, FALSE, FALSE, 0);
    g_signal_connect(app.window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p)->gboolean {static_cast<NativeApp*>(p)->quit(); return TRUE;}), &app);
    const auto timer = g_timeout_add(100, [](gpointer p)->gboolean {auto& a=*static_cast<NativeApp*>(p); a.safe([&]{a.tick();}); return G_SOURCE_CONTINUE;}, &app);
    const auto sigint = g_unix_signal_add(SIGINT, [](gpointer p)->gboolean {static_cast<NativeApp*>(p)->quit(); return G_SOURCE_CONTINUE;}, &app);
    const auto sigterm = g_unix_signal_add(SIGTERM, [](gpointer p)->gboolean {static_cast<NativeApp*>(p)->quit(); return G_SOURCE_CONTINUE;}, &app);
    app.buttons();
    if (maximized) gtk_window_maximize(GTK_WINDOW(app.window));
    gtk_widget_show_all(app.window);
    if (autostart) app.safe([&]{app.start();});
    gtk_main(); g_source_remove(timer); g_source_remove(sigint); g_source_remove(sigterm);
    app.stop(); return app.result;
}
}
