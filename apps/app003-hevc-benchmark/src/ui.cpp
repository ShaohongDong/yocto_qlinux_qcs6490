// SPDX-License-Identifier: MIT
#include "engine.hpp"
#include <gtk/gtk.h>
#include <thread>
namespace hevc {
namespace {
struct Window {
    Options options;
    View view;
    std::atomic<bool> cancel{false}, running{false};
    std::thread worker;
    GtkWidget *window, *mode, *profile, *seconds, *bitrate, *output, *status, *picture, *start, *stop;
    bool closing = false;
    std::string last_text;
    static void begin(GtkButton*, gpointer data) {
        auto& w = *static_cast<Window*>(data);
        if (w.running) return;
        if (w.worker.joinable()) w.worker.join();
        w.options.mode = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w.mode));
        w.options.latency_profile = gtk_combo_box_get_active_id(GTK_COMBO_BOX(w.profile));
        w.options.seconds = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.seconds));
        w.options.bitrate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w.bitrate)) * 1000000;
        w.options.output = gtk_entry_get_text(GTK_ENTRY(w.output));
        w.options.preview = w.options.mode != "encode";
        w.cancel = false; w.running = true;
        gtk_widget_set_sensitive(w.start, false);
        gtk_widget_set_sensitive(w.stop, true);
        { std::lock_guard<std::mutex> lock(w.view.mutex); w.view.rgb.clear(); w.view.submitted_frames = 0; w.view.text = "Checking hardware and starting pipeline..."; }
        gtk_image_clear(GTK_IMAGE(w.picture));
        w.worker = std::thread([&w] { run(w.options, w.cancel, &w.view); w.running = false; });
    }
    static void end(GtkButton*, gpointer data) { static_cast<Window*>(data)->cancel = true; }
    static gboolean close(GtkWidget*, GdkEvent*, gpointer data) {
        auto& w = *static_cast<Window*>(data);
        w.cancel = true; w.closing = true;
        gtk_widget_set_sensitive(w.start, false);
        if (!w.running) gtk_main_quit();
        return true;
    }
    static gboolean tick(gpointer data) {
        auto& w = *static_cast<Window*>(data);
        if (w.closing && !w.running) { gtk_main_quit(); return G_SOURCE_CONTINUE; }
        gtk_widget_set_sensitive(w.start, !w.running && !w.closing);
        gtk_widget_set_sensitive(w.stop, w.running);
        std::lock_guard<std::mutex> lock(w.view.mutex);
        if (w.last_text != w.view.text) {
            gtk_label_set_text(GTK_LABEL(w.status), w.view.text.c_str());
            w.last_text = w.view.text;
        }
        if (!w.view.rgb.empty()) {
            auto* pixels = static_cast<guchar*>(g_memdup2(w.view.rgb.data(), w.view.rgb.size()));
            auto* pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, false, 8, 640, 360,
                w.view.stride, [](guchar* p, gpointer) { g_free(p); }, nullptr);
            gtk_image_set_from_pixbuf(GTK_IMAGE(w.picture), pixbuf);
            ++w.view.submitted_frames;
            g_object_unref(pixbuf);
            w.view.rgb.clear();
        }
        return G_SOURCE_CONTINUE;
    }
};
}
int ui(int, char**, Options options) {
    if (!gtk_init_check(nullptr, nullptr)) {
        g_printerr("No GTK display available. Run from Weston or use CLI without --gui.\n");
        return 1;
    }
    Window w;
    w.options = std::move(options);
    if (w.options.output == "hevc-results") w.options.output = std::string(g_get_home_dir()) + "/hevc-results";
    w.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w.window), "HEVC 1080p30 — Encode / Decode / Y-PSNR");
    gtk_window_set_default_size(GTK_WINDOW(w.window), 960, 760);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_container_add(GTK_CONTAINER(w.window), box);
    auto* title = gtk_label_new("1920 × 1080 · HEVC Main 8-bit · Hardware benchmark");
    gtk_box_pack_start(GTK_BOX(box), title, false, false, 0);
    auto* controls = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(controls), 10);
    gtk_grid_set_row_spacing(GTK_GRID(controls), 8);
    gtk_box_pack_start(GTK_BOX(box), controls, false, false, 0);
    w.mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.mode), "loopback", "30fps loopback + Y-PSNR");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.mode), "encode", "Encode throughput (unpaced)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.mode), "decode", "Decode throughput (preloaded)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.mode), w.options.mode.c_str());
    w.profile = gtk_combo_box_text_new();
    for (auto name : {"baseline", "low", "encode-low", "decode-low"})
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w.profile), name, name);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(w.profile), w.options.latency_profile.c_str());
    w.seconds = gtk_spin_button_new_with_range(1, 600, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w.seconds), w.options.seconds);
    w.bitrate = gtk_spin_button_new_with_range(1, 80, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w.bitrate), w.options.bitrate / 1000000.);
    w.output = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w.output), w.options.output.c_str());
    gtk_widget_set_hexpand(w.output, true);
    const char* labels[] = {"Mode", "Seconds", "Mbps", "Report directory"};
    GtkWidget* widgets[] = {w.mode, w.seconds, w.bitrate, w.output};
    for (int i = 0; i < 4; ++i) {
        gtk_grid_attach(GTK_GRID(controls), gtk_label_new(labels[i]), i, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(controls), widgets[i], i, 1, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(controls), gtk_label_new("Latency profile"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(controls), w.profile, 1, 2, 2, 1);
    auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    w.start = gtk_button_new_with_label("Start"); w.stop = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(w.stop, false);
    gtk_box_pack_start(GTK_BOX(buttons), w.start, false, false, 0);
    gtk_box_pack_start(GTK_BOX(buttons), w.stop, false, false, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, false, false, 0);
    w.picture = gtk_image_new();
    gtk_widget_set_size_request(w.picture, 640, 360);
    gtk_box_pack_start(GTK_BOX(box), w.picture, true, true, 0);
    w.status = gtk_label_new(w.view.text.c_str());
    gtk_label_set_xalign(GTK_LABEL(w.status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(w.status), true);
    gtk_label_set_selectable(GTK_LABEL(w.status), true);
    gtk_box_pack_start(GTK_BOX(box), w.status, false, false, 0);
    auto* note = gtk_label_new("Latency includes driver queueing. Y-PSNR compares original and decoded luma.\n"
                               "Throughput tests and preview/quality runs have different CPU overhead.");
    gtk_box_pack_start(GTK_BOX(box), note, false, false, 0);
    g_signal_connect(w.start, "clicked", G_CALLBACK(Window::begin), &w);
    g_signal_connect(w.stop, "clicked", G_CALLBACK(Window::end), &w);
    g_signal_connect(w.window, "delete-event", G_CALLBACK(Window::close), &w);
    guint timer = g_timeout_add(16, Window::tick, &w);
    gtk_widget_show_all(w.window);
    gtk_main();
    w.cancel = true;
    if (w.worker.joinable()) w.worker.join();
    g_source_remove(timer);
    gtk_widget_destroy(w.window);
    return 0;
}
}
