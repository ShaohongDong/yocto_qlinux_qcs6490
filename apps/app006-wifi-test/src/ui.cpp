// SPDX-License-Identifier: MIT
#include "wifi.hpp"
#include <gtk/gtk.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace wifi {
namespace {
struct UI {
    GtkWidget *window, *interface, *ssid, *password, *security, *remember, *hidden, *target, *count, *status, *controls, *stop, *tree, *log;
    GtkListStore* list;
    GCancellable* cancel = nullptr;
    std::thread worker;
    std::atomic<bool> done{false};
    bool running = false, closing = false;
    std::mutex mutex;
    std::vector<std::string> pending;
    Result result;
    std::string selected_bssid, selected_ssid;
    void append(const std::string& text) {
        auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log)); GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end); const std::string line = text + '\n';
        gtk_text_buffer_insert(buffer, &end, line.c_str(), -1);
        if (gtk_text_buffer_get_line_count(buffer) > 2000) {
            GtkTextIter start, cutoff; gtk_text_buffer_get_start_iter(buffer, &start);
            gtk_text_buffer_get_iter_at_line(buffer, &cutoff, 100); gtk_text_buffer_delete(buffer, &start, &cutoff);
        }
        gtk_text_buffer_get_end_iter(buffer, &end); gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(log), &end, 0, FALSE, 0, 0);
    }
    void start(const char* command) {
        if (running) return;
        Request request; request.command = command;
        gchar* iface = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(interface));
        if (iface) { request.interface = iface; g_free(iface); }
        if (request.command == "connect") {
            request.ssid = gtk_entry_get_text(GTK_ENTRY(ssid)); request.password = gtk_entry_get_text(GTK_ENTRY(password));
            gtk_entry_set_text(GTK_ENTRY(password), "");
            request.security = gtk_combo_box_get_active_id(GTK_COMBO_BOX(security));
            request.remember = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(remember));
            request.hidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hidden));
            if (request.ssid == selected_ssid && !request.hidden) request.bssid = selected_bssid;
        }
        if (request.command == "ping") {
            request.target = gtk_entry_get_text(GTK_ENTRY(target));
            request.count = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(count));
        }
        cancel = g_cancellable_new(); running = true; done = false;
        gtk_widget_set_sensitive(controls, FALSE); gtk_widget_set_sensitive(stop, TRUE);
        gtk_label_set_text(GTK_LABEL(status), "操作进行中… 可点击停止（最长 60 秒）");
        worker = std::thread([this, request = std::move(request)]() mutable {
            result = run(request, cancel, [this](const std::string& text) { std::lock_guard<std::mutex> lock(mutex); pending.push_back(text); });
            done = true;
        });
    }
    void poll() {
        { std::lock_guard<std::mutex> lock(mutex); for (const auto& text : pending) append(text); pending.clear(); }
        if (!running || !done) return;
        worker.join(); running = false; g_object_unref(cancel); cancel = nullptr;
        gtk_widget_set_sensitive(controls, TRUE); gtk_widget_set_sensitive(stop, FALSE);
        gchar* old = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(interface));
        if (!result.interfaces.empty()) {
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(interface)); int selected = 0;
            for (size_t i = 0; i < result.interfaces.size(); ++i) {
                const auto& value = result.interfaces[i]; gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(interface), value.c_str());
                if (old && value == old) selected = static_cast<int>(i);
            }
            gtk_combo_box_set_active(GTK_COMBO_BOX(interface), selected);
        }
        g_free(old);
        if (result.command == "scan" && result.ok) {
            gtk_list_store_clear(list);
            for (const auto& ap : result.access_points) {
                GtkTreeIter row; gtk_list_store_append(list, &row);
                const std::string band = ap.frequency < 2500 ? "2.4 GHz" : ap.frequency < 5925 ? "5 GHz" : "6 GHz";
                const std::string signal = std::to_string(ap.strength) + "%";
                gtk_list_store_set(list, &row, 0, ap.active ? "已连接" : "", 1, ap.ssid.empty() ? "（隐藏网络）" : ap.ssid.c_str(),
                    2, ap.bssid.c_str(), 3, signal.c_str(), 4, band.c_str(), 5, ap.security.c_str(), 6, ap.ssid.c_str(), -1);
            }
        }
        std::string summary = result.message + "  |  " + result.interface + " " + result.state;
        if (!result.ssid.empty()) summary += "  |  " + result.ssid;
        for (const auto& ip : result.addresses) summary += "  |  " + ip;
        if (!result.gateway.empty()) summary += "  网关 " + result.gateway;
        if (result.has_statistics) {
            summary += "  丢包 " + std::to_string(result.loss) + "%";
            if (result.has_rtt) summary += "  平均 " + std::to_string(result.average) + " ms";
        }
        gtk_label_set_text(GTK_LABEL(status), summary.c_str()); append(summary);
        if (closing) gtk_main_quit();
    }
};
GtkWidget* button(GtkWidget* box, const char* label, UI* ui, const char* command) {
    auto* widget = gtk_button_new_with_label(label); gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(widget), "command", const_cast<char*>(command));
    g_signal_connect(widget, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer data) {
        static_cast<UI*>(data)->start(static_cast<const char*>(g_object_get_data(G_OBJECT(b), "command")));
    }), ui); return widget;
}
GtkWidget* row(GtkWidget* parent) { auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_pack_start(GTK_BOX(parent), box, FALSE, FALSE, 0); return box; }
void label(GtkWidget* box, const char* text) { gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text), FALSE, FALSE, 0); }
GtkWidget* entry(GtkWidget* box, const char* hint) {
    auto* value = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(value), hint);
    gtk_box_pack_start(GTK_BOX(box), value, TRUE, TRUE, 0); return value;
}
}
int gui() {
    if (!gtk_init_check(nullptr, nullptr)) { g_printerr("需要可用的 Wayland/X11 桌面会话\n"); return 1; }
    UI ui;
    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(ui.window), "Wi-Fi 测试 · Q6A");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 980, 540);
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(ui.window), root);
    auto* title = gtk_label_new("Wi-Fi 扫描、连接与网络 Ping"); gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), title, FALSE, FALSE, 0);
    ui.controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_box_pack_start(GTK_BOX(root), ui.controls, TRUE, TRUE, 0);
    auto* toolbar = row(ui.controls); label(toolbar, "无线网卡"); ui.interface = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(toolbar), ui.interface, FALSE, FALSE, 0);
    button(toolbar, "刷新状态", &ui, "status"); button(toolbar, "扫描 Wi-Fi", &ui, "scan"); button(toolbar, "断开连接", &ui, "disconnect");
    ui.list = gtk_list_store_new(7, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    ui.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui.list));
    const char* headings[] = {"状态", "网络名称 (SSID)", "BSSID", "信号", "频段", "安全类型"};
    for (int i = 0; i < 6; ++i) {
        auto* column = gtk_tree_view_column_new_with_attributes(headings[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_resizable(column, TRUE); if (i == 1) gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ui.tree), column);
    }
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr); gtk_widget_set_size_request(scroll, -1, 128);
    gtk_container_add(GTK_CONTAINER(scroll), ui.tree); gtk_box_pack_start(GTK_BOX(ui.controls), scroll, TRUE, TRUE, 0);
    auto* network = row(ui.controls); label(network, "SSID"); ui.ssid = entry(network, "选择上方热点或输入隐藏 SSID");
    ui.security = gtk_combo_box_text_new();
    for (const char* mode : {"auto", "open", "wpa2", "wpa3"}) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.security), mode, mode);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.security), 0); gtk_box_pack_start(GTK_BOX(network), ui.security, FALSE, FALSE, 0);
    ui.hidden = gtk_check_button_new_with_label("隐藏 SSID"); gtk_box_pack_start(GTK_BOX(network), ui.hidden, FALSE, FALSE, 0);
    auto* auth = row(ui.controls); label(auth, "密码"); ui.password = entry(auth, "开放网络无需密码"); gtk_entry_set_visibility(GTK_ENTRY(ui.password), FALSE);
    ui.remember = gtk_check_button_new_with_label("记住此网络（自动重连）"); gtk_box_pack_start(GTK_BOX(auth), ui.remember, FALSE, FALSE, 0);
    button(auth, "连接 Wi-Fi", &ui, "connect");
    auto* test = row(ui.controls); label(test, "Ping 目标"); ui.target = entry(test, "留空使用无线网关，也可输入 IP / 域名");
    label(test, "次数"); ui.count = gtk_spin_button_new_with_range(1, 1000, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.count), 4);
    gtk_box_pack_start(GTK_BOX(test), ui.count, FALSE, FALSE, 0); button(test, "开始 Ping", &ui, "ping");
    auto* footer = row(root); ui.stop = gtk_button_new_with_label("停止"); gtk_widget_set_sensitive(ui.stop, FALSE);
    gtk_box_pack_start(GTK_BOX(footer), ui.stop, FALSE, FALSE, 0);
    ui.status = gtk_label_new("正在读取无线设备…"); gtk_label_set_line_wrap(GTK_LABEL(ui.status), TRUE); gtk_label_set_xalign(GTK_LABEL(ui.status), 0);
    gtk_box_pack_start(GTK_BOX(footer), ui.status, TRUE, TRUE, 0);
    ui.log = gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(ui.log), FALSE); gtk_text_view_set_monospace(GTK_TEXT_VIEW(ui.log), TRUE);
    auto* logs = gtk_scrolled_window_new(nullptr, nullptr); gtk_widget_set_size_request(logs, -1, 96); gtk_container_add(GTK_CONTAINER(logs), ui.log);
    gtk_box_pack_start(GTK_BOX(root), logs, TRUE, TRUE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree)), "changed", G_CALLBACK(+[](GtkTreeSelection* selection, gpointer data) {
        auto& u = *static_cast<UI*>(data); GtkTreeModel* model; GtkTreeIter iter;
        if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
        gchar* ssid = nullptr; gchar* bssid = nullptr; gchar* mode = nullptr; gtk_tree_model_get(model, &iter, 6, &ssid, 2, &bssid, 5, &mode, -1);
        u.selected_ssid = ssid; u.selected_bssid = bssid; gtk_entry_set_text(GTK_ENTRY(u.ssid), ssid);
        gtk_entry_set_text(GTK_ENTRY(u.password), ""); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.hidden), FALSE);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(u.security), mode)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(u.security), "auto");
        g_free(ssid); g_free(bssid); g_free(mode);
    }), &ui);
    g_signal_connect(ui.stop, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) { auto& u = *static_cast<UI*>(data); if (u.cancel) g_cancellable_cancel(u.cancel); }), &ui);
    g_signal_connect(ui.window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
        auto& u = *static_cast<UI*>(data); if (u.running) { u.closing = true; g_cancellable_cancel(u.cancel); }
        else gtk_main_quit();
        return TRUE;
    }), &ui);
    guint timer = g_timeout_add(50, +[](gpointer data) -> gboolean { static_cast<UI*>(data)->poll(); return G_SOURCE_CONTINUE; }, &ui);
    GdkDisplay* display = gdk_display_get_default();
    GdkMonitor* monitor = gdk_display_get_monitor(display, 0);
    if (monitor) {
        GdkRectangle workarea; gdk_monitor_get_workarea(monitor, &workarea);
        if (workarea.height <= 720) gtk_window_maximize(GTK_WINDOW(ui.window));
    }
    gtk_widget_show_all(ui.window); ui.start("status"); gtk_main();
    g_source_remove(timer); if (ui.worker.joinable()) ui.worker.join();
    gtk_widget_destroy(ui.window); g_object_unref(ui.list); return 0;
}
}
