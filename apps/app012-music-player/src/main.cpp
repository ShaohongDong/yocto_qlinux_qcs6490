// SPDX-License-Identifier: MIT
#include "player.hpp"
#include "output_monitor.hpp"
#include <gtk/gtk.h>
#include <cstdio>
#include <cstring>
#include <vector>

static std::string clock_text(gint64 ns) {
    const auto seconds = static_cast<long long>(ns > 0 ? ns / GST_SECOND : 0);
    char text[64];
    std::snprintf(text, sizeof(text), "%02lld:%02lld", seconds / 60, seconds % 60);
    return text;
}

static bool music_filename(const char* filename) {
    const char* extension = filename ? std::strrchr(filename, '.') : nullptr;
    if (!extension) return false;
    for (const char* allowed : {".mp3", ".wav", ".flac", ".ogg", ".oga"})
        if (!g_ascii_strcasecmp(extension, allowed)) return true;
    return false;
}

class Window {
    Player player;
    GtkWidget *window, *title, *status, *time, *progress, *play_button, *stop_button;
    GtkWidget *outputs, *loop;
    OutputMonitor audio;
    std::vector<std::string> devices;
    std::string selected_output, active_output;
    gint64 playback_started = 0;
    guint timer = 0;
    bool refreshing = false;
    bool output_lost = false;
    std::string output_snapshot;
    gint64 duration = 0;

    void message(const std::string& text) { gtk_label_set_text(GTK_LABEL(status), text.c_str()); }
    void controls() {
        const char* label = player.playing ? "暂停" : "播放";
        if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(play_button)), label))
            gtk_button_set_label(GTK_BUTTON(play_button), label);
        gtk_widget_set_sensitive(play_button, !player.uri.empty() && player.pipeline);
        gtk_widget_set_sensitive(stop_button, !player.uri.empty());
    }
    void reset_progress() {
        duration = 0;
        gtk_range_set_value(GTK_RANGE(progress), 0);
        gtk_widget_set_sensitive(progress, FALSE);
        gtk_label_set_text(GTK_LABEL(time), "00:00 / --:--");
    }
    void open() {
        GtkWidget* dialog = gtk_file_chooser_dialog_new("打开音乐", GTK_WINDOW(window),
            GTK_FILE_CHOOSER_ACTION_OPEN, "取消", GTK_RESPONSE_CANCEL, "打开", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 800, 520);
        gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
        GtkFileFilter* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "音乐文件（MP3 / WAV / FLAC / Ogg）");
        gtk_file_filter_add_custom(filter, GTK_FILE_FILTER_FILENAME,
            +[](const GtkFileFilterInfo* info, gpointer) -> gboolean {
                return music_filename(info->filename);
            }, nullptr, nullptr);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "所有文件");
        gtk_file_filter_add_pattern(filter, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            gchar* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (path) {
                const bool ok = player.load(path);
                gchar* name = g_filename_display_basename(path);
                gtk_label_set_text(GTK_LABEL(title), ok ? name : "未选择音乐");
                g_free(name);
                message(ok ? "已打开，点击播放" : player.error);
                reset_progress();
                controls();
                g_free(path);
            }
        }
        gtk_widget_destroy(dialog);
    }
    void update_outputs() {
        std::string snapshot = audio.valid ? "ready" : "disconnected";
        snapshot += "\n" + audio.default_name + "\n" + selected_output;
        for (const auto& output : audio.outputs)
            snapshot += "\n" + output.name + "\n" + output.description + (output.usable() ? "+" : "-");
        if (snapshot == output_snapshot) return;
        output_snapshot = snapshot;
        refreshing = true;
        devices.clear();
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(outputs));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(outputs), "系统默认输出");
        int selected = 0;
        for (const auto& output : audio.outputs) {
            if (!output.usable() && output.name != selected_output) continue;
            devices.push_back(output.name);
            const auto label = output.description + (output.usable() ? "" : "（不可用）");
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(outputs), label.c_str());
            if (output.name == selected_output) selected = devices.size();
        }
        if (!selected_output.empty() && selected == 0) {
            devices.push_back(selected_output);
            const auto label = selected_output + "（不可用）";
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(outputs), label.c_str());
            selected = devices.size();
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(outputs), selected);
        refreshing = false;
    }
    void refresh_outputs() {
        player.stop(); player.error.clear(); playback_started = 0; output_lost = false;
        reset_progress();
        controls();
        audio.poll();
        selected_output.clear();
        output_snapshot.clear();
        update_outputs();
        message("已选择系统默认输出");
    }
    void select_output() {
        if (refreshing) return;
        const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(outputs));
        selected_output = index > 0 && static_cast<size_t>(index) <= devices.size() ? devices[index - 1] : "";
        player.stop(); player.error.clear(); playback_started = 0; output_lost = false;
        reset_progress();
        controls();
        message("音频输出已切换，点击播放将从头开始");
    }
    void output_error() {
        player.stop();
        playback_started = 0;
        output_lost = true;
        player.error = "没有可用音频输出，请连接耳机或恢复音频服务";
        message(player.error);
        controls();
    }
    void play() {
        audio.poll();
        const auto* target = audio.find(selected_output);
        if (!target || !target->usable()) { output_error(); return; }
        output_lost = false;
        // Pin the chosen sink. A default change must not send headphone audio
        // to an unrelated output while this file is playing.
        if (active_output != target->name || !playback_started) {
            GstElement* sink = gst_element_factory_make("pulsesink", nullptr);
            if (!sink) { output_error(); return; }
            gst_object_ref_sink(sink);
            GstStructure* properties = gst_structure_new("props", "application.id", G_TYPE_STRING,
                audio.stream_id.c_str(), nullptr);
            g_object_set(sink, "device", target->name.c_str(), "stream-properties", properties, nullptr);
            gst_structure_free(properties);
            player.output(sink);
            gst_object_unref(sink);
            active_output = target->name;
        }
        playback_started = g_get_monotonic_time();
        if (!player.play()) message(player.error);
    }
    void tick() {
        audio.poll();
        update_outputs();
        if (output_lost && !player.playing) {
            const auto* target = audio.find(selected_output);
            if (target && target->usable()) {
                output_lost = false;
                player.error.clear();
                message("音频输出已恢复，点击播放");
            }
        }
        if (player.playing) {
            if (!route_usable(audio.outputs, audio.valid, active_output, audio.stream_sink,
                    g_get_monotonic_time() - playback_started > 3000000)) output_error();
        }
        const bool terminal = player.poll();
        if (!player.error.empty()) message("播放错误：" + player.error);
        else if (terminal && player.ended) message("播放结束");
        else if (player.playing) {
            GstState state = GST_STATE_NULL;
            gst_element_get_state(player.pipeline, &state, nullptr, 0);
            message(state == GST_STATE_PLAYING ? "正在播放" : "正在准备音频输出…");
        }
        controls();
        gint64 position = 0;
        gint64 current_duration = 0;
        if (player.pipeline && gst_element_query_duration(player.pipeline, GST_FORMAT_TIME, &current_duration)
            && current_duration > 0) duration = current_duration;
        if (player.pipeline) gst_element_query_position(player.pipeline, GST_FORMAT_TIME, &position);
        if (player.ended) position = duration;
        if (duration > 0) {
            gtk_range_set_range(GTK_RANGE(progress), 0, static_cast<double>(duration) / GST_SECOND);
            gtk_range_set_value(GTK_RANGE(progress), static_cast<double>(position) / GST_SECOND);
        }
        gboolean seekable = FALSE;
        if (player.pipeline) {
            GstQuery* query = gst_query_new_seeking(GST_FORMAT_TIME);
            if (gst_element_query(player.pipeline, query)) gst_query_parse_seeking(query, nullptr, &seekable, nullptr, nullptr);
            gst_query_unref(query);
        }
        gtk_widget_set_sensitive(progress, duration > 0 && seekable && !player.ended);
        const auto text = clock_text(position) + " / " + (duration > 0 ? clock_text(duration) : "--:--");
        gtk_label_set_text(GTK_LABEL(time), text.c_str());
    }
public:
    Window() {
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "音乐播放器");
        gtk_window_set_default_size(GTK_WINDOW(window), 680, 340);
        gtk_container_set_border_width(GTK_CONTAINER(window), 24);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer p) {
            auto& self = *static_cast<Window*>(p);
            if (self.timer) { g_source_remove(self.timer); self.timer = 0; }
            self.player.stop();
            gtk_main_quit();
        }), this);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_container_add(GTK_CONTAINER(window), box);
        auto add = [box](GtkWidget* widget) { gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0); };
        title = gtk_label_new("未选择音乐");
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
        add(title);
        GtkWidget* open_button = gtk_button_new_with_label("打开音乐文件");
        add(open_button);
        g_signal_connect(open_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<Window*>(p)->open(); }), this);
        progress = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.1);
        gtk_scale_set_draw_value(GTK_SCALE(progress), FALSE);
        add(progress);
        // change-value is emitted for user input only; timer updates do not seek.
        g_signal_connect(progress, "change-value", G_CALLBACK(+[](GtkRange*, GtkScrollType, gdouble value, gpointer p) -> gboolean {
            auto& self = *static_cast<Window*>(p);
            if (!self.player.seek(static_cast<gint64>(value * GST_SECOND))) self.message("当前文件暂不支持跳转");
            return TRUE;
        }), this);
        time = gtk_label_new("00:00 / --:--");
        add(time);
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        add(row);
        play_button = gtk_button_new_with_label("播放");
        stop_button = gtk_button_new_with_label("停止");
        loop = gtk_check_button_new_with_label("单曲循环");
        for (auto* widget : {play_button, stop_button, loop}) gtk_box_pack_start(GTK_BOX(row), widget, TRUE, TRUE, 0);
        g_signal_connect(play_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            auto& self = *static_cast<Window*>(p);
            if (self.player.playing) { self.player.pause(); self.message("已暂停"); }
            else self.play();
            self.controls();
        }), this);
        g_signal_connect(stop_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
            auto& self = *static_cast<Window*>(p);
            self.player.stop(); self.player.error.clear(); self.reset_progress(); self.controls(); self.message("已停止");
        }), this);
        g_signal_connect(loop, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer p) {
            static_cast<Window*>(p)->player.repeat = gtk_toggle_button_get_active(b);
        }), this);
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        add(row);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("音量"), FALSE, FALSE, 0);
        GtkWidget* volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_range_set_value(GTK_RANGE(volume), 50);
        gtk_box_pack_start(GTK_BOX(row), volume, TRUE, TRUE, 0);
        g_signal_connect(volume, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer p) {
            auto* pipeline = static_cast<Window*>(p)->player.pipeline;
            if (pipeline) g_object_set(pipeline, "volume", gtk_range_get_value(range) / 100.0, nullptr);
        }), this);
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        add(row);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("音频输出"), FALSE, FALSE, 0);
        outputs = gtk_combo_box_text_new();
        gtk_box_pack_start(GTK_BOX(row), outputs, TRUE, TRUE, 0);
        GtkWidget* refresh = gtk_button_new_with_label("刷新设备");
        gtk_box_pack_start(GTK_BOX(row), refresh, FALSE, FALSE, 0);
        g_signal_connect(outputs, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer p) { static_cast<Window*>(p)->select_output(); }), this);
        g_signal_connect(refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { static_cast<Window*>(p)->refresh_outputs(); }), this);
        status = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(status), 65);
        gtk_label_set_selectable(GTK_LABEL(status), TRUE);
        add(status);
        refresh_outputs();
        message(player.pipeline ? "打开本地音乐文件开始播放" : player.error);
        timer = g_timeout_add(200, +[](gpointer p) -> gboolean { static_cast<Window*>(p)->tick(); return G_SOURCE_CONTINUE; }, this);
        gtk_widget_show_all(window);
    }
    ~Window() {
        if (timer) g_source_remove(timer);

    }
};

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--self-test") && std::strcmp(argv[1], "--gui"))) {
        std::fprintf(stderr, "Usage: music-player [--gui|--self-test]\n");
        return 2;
    }
    if (argc == 2 && !std::strcmp(argv[1], "--self-test")) {
        // QEMU-user safe: no plugins, display, audio server or hardware required.
        gchar* uri = gst_filename_to_uri("/tmp/music test.wav", nullptr);
        const bool ok = uri && !std::strcmp(uri, "file:///tmp/music%20test.wav") && clock_text(125 * GST_SECOND) == "02:05"
            && music_filename("/tmp/音乐.Mp3") && music_filename("song.OGG")
            && !music_filename("song.mp3.txt") && !music_filename(nullptr);
        g_free(uri);
        std::puts(ok ? "music-player self-test PASS (offline only)" : "music-player self-test FAIL");
        return ok ? 0 : 1;
    }
    gst_init(nullptr, nullptr);
    if (!gtk_init_check(nullptr, nullptr)) { std::fprintf(stderr, "无法连接桌面显示服务\n"); return 1; }
    Window window;
    gtk_main();
    return 0;
}
