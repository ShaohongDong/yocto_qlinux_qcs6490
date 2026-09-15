// SPDX-License-Identifier: MIT
#include "wifi.hpp"
#include <NetworkManager.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace wifi {
namespace {
template<class T> struct Unref { void operator()(T* p) const { if (p) g_object_unref(p); } };
template<class T> using Object = std::unique_ptr<T, Unref<T>>;
using Clock = std::chrono::steady_clock;
std::string str(const char* p) { return p ? p : ""; }
std::string ssid(NMAccessPoint* ap) {
    GBytes* bytes = nm_access_point_get_ssid(ap);
    if (!bytes) return {};
    gsize size; const auto* p = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    return {p, size};
}
std::string display(const std::string& bytes) {
    gchar* p = g_utf8_make_valid(bytes.data(), bytes.size()); std::string out(p); g_free(p); return out;
}
std::string security(NMAccessPoint* ap) {
    const auto flags = nm_access_point_get_rsn_flags(ap);
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X) return "enterprise";
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_SAE) return "wpa3";
    if (flags & NM_802_11_AP_SEC_KEY_MGMT_PSK) return "wpa2";
    if (flags || nm_access_point_get_wpa_flags(ap) || (nm_access_point_get_flags(ap) & NM_802_11_AP_FLAGS_PRIVACY)) return "unsupported";
    return "open";
}
struct Async {
    GAsyncResult* result = nullptr;
    ~Async() { if (result) g_object_unref(result); }
    static void done(GObject*, GAsyncResult* result, gpointer data) {
        static_cast<Async*>(data)->result = G_ASYNC_RESULT(g_object_ref(result));
    }
};
struct Session {
    GMainContext* context = g_main_context_new();
    Object<GCancellable> cancel{g_cancellable_new()};
    GCancellable* user;
    Clock::time_point deadline;
    Object<NMClient> client;
    std::string stop_reason;
    explicit Session(GCancellable* u, int seconds) : user(u), deadline(Clock::now() + std::chrono::seconds(seconds)) {
        g_main_context_push_thread_default(context);
    }
    ~Session() {
        client.reset();
        while (g_main_context_iteration(context, FALSE)) {}
        g_main_context_pop_thread_default(context); g_main_context_unref(context);
    }
    void tick(bool cancellable = true) {
        while (g_main_context_iteration(context, FALSE)) {}
        if (cancellable) {
            if (g_cancellable_is_cancelled(user)) stop_reason = "操作已取消";
            else if (Clock::now() >= deadline) stop_reason = "操作超时";
            if (!stop_reason.empty()) g_cancellable_cancel(cancel.get());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    void check() { if (!stop_reason.empty()) throw std::runtime_error(stop_reason); }
    void wait(Async& pending, bool cancellable = true) { while (!pending.result) tick(cancellable); }
    void error(GError* error) {
        if (error) {
            // NetworkManager errors do not include submitted settings/secrets.
            std::string message = str(error->message); g_error_free(error);
            if (!stop_reason.empty()) throw std::runtime_error(stop_reason);
            throw std::runtime_error(message);
        }
        check();
    }
    void init() {
        Async pending; nm_client_new_async(cancel.get(), Async::done, &pending); wait(pending);
        GError* e = nullptr; client.reset(nm_client_new_finish(pending.result, &e)); error(e);
        if (!nm_client_get_nm_running(client.get())) throw std::runtime_error("NetworkManager 服务未运行");
    }
};
NMDevice* device(Session& s, const Request& r, Result& out) {
    const GPtrArray* devices = nm_client_get_devices(s.client.get());
    NMDevice* selected = nullptr;
    for (guint i = 0; i < devices->len; ++i) {
        auto* d = NM_DEVICE(g_ptr_array_index(devices, i));
        if (!NM_IS_DEVICE_WIFI(d)) continue;
        out.interfaces.push_back(str(nm_device_get_iface(d)));
        if (r.interface == nm_device_get_iface(d)) selected = d;
        else if (r.interface.empty() && !selected) selected = d;
    }
    if (r.interface.empty() && out.interfaces.size() > 1) throw std::runtime_error("发现多个无线网卡，请指定 --interface");
    if (!selected) throw std::runtime_error("找不到无线网卡，请检查驱动或接口名称");
    out.interface = str(nm_device_get_iface(selected));
    return selected;
}
void snapshot(NMDevice* d, Result& out) {
    auto* enum_class = G_ENUM_CLASS(g_type_class_ref(NM_TYPE_DEVICE_STATE));
    auto* value = g_enum_get_value(enum_class, nm_device_get_state(d));
    out.state = value ? str(value->value_nick) : "unknown"; g_type_class_unref(enum_class);
    out.addresses.clear(); out.gateway.clear(); out.ssid.clear();
    for (auto* config : {nm_device_get_ip4_config(d), nm_device_get_ip6_config(d)}) {
        if (!config) continue;
        if (out.gateway.empty()) out.gateway = str(nm_ip_config_get_gateway(config));
        const auto* addresses = nm_ip_config_get_addresses(config);
        for (guint i = 0; addresses && i < addresses->len; ++i) {
            auto* address = static_cast<NMIPAddress*>(g_ptr_array_index(addresses, i));
            out.addresses.push_back(str(nm_ip_address_get_address(address)) + "/" + std::to_string(nm_ip_address_get_prefix(address)));
        }
    }
    if (auto* ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(d))) out.ssid = display(ssid(ap));
}
void scan(Session& s, NMDevice* d, Result& out) {
    auto* wifi = NM_DEVICE_WIFI(d);
    const gint64 previous = nm_device_wifi_get_last_scan(wifi);
    Async pending; nm_device_wifi_request_scan_async(wifi, s.cancel.get(), Async::done, &pending); s.wait(pending);
    GError* e = nullptr; nm_device_wifi_request_scan_finish(wifi, pending.result, &e); s.error(e);
    while (nm_device_wifi_get_last_scan(wifi) <= previous) { s.tick(); s.check(); }
    const auto* aps = nm_device_wifi_get_access_points(wifi);
    auto* active = nm_device_wifi_get_active_access_point(wifi);
    for (guint i = 0; aps && i < aps->len; ++i) {
        auto* ap = NM_ACCESS_POINT(g_ptr_array_index(aps, i)); const auto raw = ssid(ap);
        out.access_points.push_back({display(raw), hex(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()),
            str(nm_access_point_get_bssid(ap)), security(ap), nm_access_point_get_strength(ap), nm_access_point_get_frequency(ap), ap == active});
    }
    std::sort(out.access_points.begin(), out.access_points.end(), [](const auto& a, const auto& b) { return a.strength > b.strength; });
}
std::string failure(NMDevice* d) {
    auto* klass = G_ENUM_CLASS(g_type_class_ref(NM_TYPE_DEVICE_STATE_REASON));
    auto* value = g_enum_get_value(klass, nm_device_get_state_reason(d));
    std::string reason = value ? value->value_nick : "unknown"; g_type_class_unref(klass);
    return "连接失败（认证、链路或地址配置）：" + reason;
}
void disconnect(Session& s, NMDevice* d) {
    Async pending; nm_device_disconnect_async(d, s.cancel.get(), Async::done, &pending); s.wait(pending);
    GError* e = nullptr; nm_device_disconnect_finish(d, pending.result, &e); s.error(e);
}
void connect(Session& s, NMDevice* d, Request& r, const Progress& progress) {
    std::string mode = r.security;
    NMAccessPoint* chosen = nullptr;
    if (!r.hidden) {
        Result ignored; scan(s, d, ignored);
        const auto* aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(d));
        for (guint i = 0; aps && i < aps->len; ++i) {
            auto* ap = NM_ACCESS_POINT(g_ptr_array_index(aps, i));
            if (ssid(ap) == r.ssid && (r.bssid.empty() || g_ascii_strcasecmp(r.bssid.c_str(), nm_access_point_get_bssid(ap)) == 0)) {
                if (!chosen || nm_access_point_get_strength(ap) > nm_access_point_get_strength(chosen)) chosen = ap;
            }
        }
        if (!chosen) throw std::runtime_error("未扫描到指定热点；隐藏网络请勾选隐藏 SSID");
        const auto found = security(chosen);
        if (found == "enterprise" || found == "unsupported") throw std::runtime_error("此网络安全类型暂不支持");
        if (mode == "auto") mode = found;
        // Explicit WPA2 is valid on WPA2/WPA3 transition networks.
        if (mode != found && !(mode == "wpa2" && (nm_access_point_get_rsn_flags(chosen) & NM_802_11_AP_SEC_KEY_MGMT_PSK)))
            throw std::runtime_error("所选安全类型与热点不匹配");
    }
    if (mode == "wpa2" && !((r.password.size() >= 8 && r.password.size() <= 63) ||
        (r.password.size() == 64 && r.password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)))
        throw std::runtime_error("WPA2 密码应为 8–63 字节或 64 位十六进制密钥");
    if (mode == "wpa3" && (r.password.empty() || r.password.size() > 63)) throw std::runtime_error("WPA3 密码应为 1–63 字节");
    Object<NMConnection> connection{nm_simple_connection_new()};
    auto* general = nm_setting_connection_new();
    gchar* uuid = nm_utils_uuid_generate(); std::string id(uuid); g_free(uuid);
    g_object_set(general, NM_SETTING_CONNECTION_ID, ("wifi-test-" + id).c_str(), NM_SETTING_CONNECTION_UUID, id.c_str(),
        NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME, NM_SETTING_CONNECTION_INTERFACE_NAME, nm_device_get_iface(d),
        NM_SETTING_CONNECTION_AUTOCONNECT, FALSE, nullptr);
    // A normal desktop user may create their own profiles under the standard
    // Polkit policy, while creating system-wide profiles requires an administrator.
    if (getuid() != 0)
        nm_setting_connection_add_permission(NM_SETTING_CONNECTION(general), "user", g_get_user_name(), nullptr);
    nm_connection_add_setting(connection.get(), general);
    auto* wireless = nm_setting_wireless_new(); GBytes* bytes = g_bytes_new(r.ssid.data(), r.ssid.size());
    g_object_set(wireless, NM_SETTING_WIRELESS_SSID, bytes, NM_SETTING_WIRELESS_MODE, "infrastructure",
        NM_SETTING_WIRELESS_HIDDEN, r.hidden, nullptr); g_bytes_unref(bytes);
    if (!r.bssid.empty()) g_object_set(wireless, NM_SETTING_WIRELESS_BSSID, r.bssid.c_str(), nullptr);
    nm_connection_add_setting(connection.get(), wireless);
    if (mode != "open") {
        auto* sec = nm_setting_wireless_security_new();
        g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, mode == "wpa3" ? "sae" : "wpa-psk",
            NM_SETTING_WIRELESS_SECURITY_PSK, r.password.c_str(), nullptr);
        nm_setting_wireless_security_add_proto(NM_SETTING_WIRELESS_SECURITY(sec), "rsn");
        if (mode == "wpa3") g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PMF, NM_SETTING_WIRELESS_SECURITY_PMF_REQUIRED, nullptr);
        nm_connection_add_setting(connection.get(), sec);
    }
    auto* ipv4 = nm_setting_ip4_config_new(); g_object_set(ipv4, NM_SETTING_IP_CONFIG_METHOD, "auto", nullptr);
    nm_connection_add_setting(connection.get(), ipv4);
    auto* ipv6 = nm_setting_ip6_config_new(); g_object_set(ipv6, NM_SETTING_IP_CONFIG_METHOD, "auto", nullptr);
    nm_connection_add_setting(connection.get(), ipv6);
    Object<NMActiveConnection> active;
    try {
        GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "persist", g_variant_new_string("volatile"));
        GVariant* opts = g_variant_ref_sink(g_variant_builder_end(&options));
        Async pending;
        // Let this short D-Bus transaction return its created profile even when the
        // user cancels, so cleanup cannot lose a newly created connection UUID.
        nm_client_add_and_activate_connection2(s.client.get(), connection.get(), d,
            chosen ? nm_object_get_path(NM_OBJECT(chosen)) : nullptr, opts, nullptr, Async::done, &pending);
        g_variant_unref(opts); s.wait(pending);
        GError* e = nullptr; active.reset(nm_client_add_and_activate_connection2_finish(s.client.get(), pending.result, nullptr, &e)); s.error(e);
        NMDeviceState last = NM_DEVICE_STATE_UNKNOWN;
        while (nm_active_connection_get_state(active.get()) != NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
            s.tick(); s.check(); auto state = nm_device_get_state(d);
            if (progress && state != last) { Result status; snapshot(d, status); progress("连接状态：" + status.state); last = state; }
            if (nm_active_connection_get_state(active.get()) >= NM_ACTIVE_CONNECTION_STATE_DEACTIVATING || state == NM_DEVICE_STATE_FAILED)
                throw std::runtime_error(failure(d));
        }
        if (r.remember) {
            auto* remote = nm_active_connection_get_connection(active.get());
            if (!remote) throw std::runtime_error("连接成功但无法保存配置");
            g_object_set(general, NM_SETTING_CONNECTION_AUTOCONNECT, TRUE, nullptr);
            GVariant* settings = nm_connection_to_dbus(connection.get(), NM_CONNECTION_SERIALIZE_ALL);
            Async save; nm_remote_connection_update2(remote, settings, NM_SETTINGS_UPDATE2_FLAG_TO_DISK, nullptr,
                s.cancel.get(), Async::done, &save); g_variant_unref(settings); s.wait(save);
            GVariant* result = nm_remote_connection_update2_finish(remote, save.result, &e);
            if (result) g_variant_unref(result);
            s.error(e);
        }
    } catch (...) {
        // Delete only the profile created by this operation. Never touch other saved networks.
        if (active && nm_active_connection_get_state(active.get()) < NM_ACTIVE_CONNECTION_STATE_DEACTIVATING) {
            Async deactivate;
            nm_client_deactivate_connection_async(s.client.get(), active.get(), nullptr, Async::done, &deactivate);
            s.wait(deactivate, false);
            GError* e = nullptr; nm_client_deactivate_connection_finish(s.client.get(), deactivate.result, &e);
            if (e) { if (progress) progress("测试连接断开失败，请检查无线接口状态"); g_error_free(e); }
        }
        auto* remote = nm_client_get_connection_by_uuid(s.client.get(), id.c_str());
        if (remote) {
            Async cleanup; nm_remote_connection_delete_async(remote, nullptr, Async::done, &cleanup); s.wait(cleanup, false);
            GError* e = nullptr; nm_remote_connection_delete_finish(remote, cleanup.result, &e);
            if (e) { if (progress) progress("测试连接清理失败，请检查 NetworkManager 配置"); g_error_free(e); }
        }
        nm_connection_clear_secrets(connection.get()); throw;
    }
    nm_connection_clear_secrets(connection.get());
}
void ping(Session& s, Request& r, Result& out, const Progress& progress) {
    out.target = r.target.empty() ? out.gateway : r.target;
    if (out.target.empty()) throw std::runtime_error("无线接口没有网关，请先连接或指定目标");
    Request checked = r; checked.target = out.target; wipe(checked.password); validate(checked);
    const std::string count = std::to_string(r.count);
    // A dedicated iputils path avoids BusyBox ping option/format differences on existing boards.
    std::string executable = "/usr/bin/ping.iputils";
    // Portable deployment on OSTree boards keeps the app and iputils together
    // under /var/opt. Resolve our actual executable, never the current directory.
    gchar* self = g_file_read_link("/proc/self/exe", nullptr);
    if (self) {
        gchar* directory = g_path_get_dirname(self);
        std::string adjacent = std::string(directory) + "/ping.iputils";
        if (g_file_test(adjacent.c_str(), G_FILE_TEST_IS_EXECUTABLE)) executable = adjacent;
        g_free(directory); g_free(self);
    }
    // With -w and -c together, iputils counts replies rather than transmitted
    // packets. Enforce the overall deadline ourselves so count means packets sent.
    const char* argv[] = {executable.c_str(), "-n", "-I", out.interface.c_str(), "-c", count.c_str(), "-W", "2", "--", out.target.c_str(), nullptr};
    Object<GSubprocessLauncher> launcher{g_subprocess_launcher_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE))};
    g_subprocess_launcher_setenv(launcher.get(), "LC_ALL", "C", TRUE);
    GError* e = nullptr; Object<GSubprocess> process{g_subprocess_launcher_spawnv(launcher.get(), argv, &e)}; s.error(e);
    Object<GDataInputStream> stream{g_data_input_stream_new(g_subprocess_get_stdout_pipe(process.get()))};
    bool interrupted = false;
    while (true) {
        Async line; g_data_input_stream_read_line_async(stream.get(), G_PRIORITY_DEFAULT, s.cancel.get(), Async::done, &line); s.wait(line);
        gsize size = 0; gchar* text = g_data_input_stream_read_line_finish_utf8(stream.get(), line.result, &size, &e);
        if (!s.stop_reason.empty()) {
            if (text) g_free(text);
            if (e) { g_error_free(e); e = nullptr; }
            interrupted = true; g_subprocess_send_signal(process.get(), SIGINT); break;
        }
        if (e) { g_subprocess_force_exit(process.get()); g_subprocess_wait(process.get(), nullptr, nullptr); s.error(e); }
        if (!text) break;
        std::string value(text, size); g_free(text); out.output += value + '\n'; if (progress) progress(value);
    }
    Async end; g_subprocess_wait_async(process.get(), nullptr, Async::done, &end);
    auto limit = Clock::now() + std::chrono::seconds(2);
    while (!end.result) { s.tick(false); if (Clock::now() > limit) g_subprocess_force_exit(process.get()); }
    g_subprocess_wait_finish(process.get(), end.result, &e); if (e) g_error_free(e);
    if (interrupted) {
        // Child has exited; drain the summary written in response to SIGINT.
        gchar buffer[4096]; gssize size;
        while ((size = g_input_stream_read(G_INPUT_STREAM(stream.get()), buffer, sizeof(buffer), nullptr, nullptr)) > 0)
            out.output.append(buffer, static_cast<size_t>(size));
    }
    parse_ping(out); s.check();
    if (!g_subprocess_get_successful(process.get())) throw std::runtime_error("Ping 失败，请查看输出（DNS、路由或目标无响应）");
}
}
Result run(Request& request, GCancellable* cancelled, const Progress& progress) {
    Result result; result.command = request.command;
    try {
        validate(request); Session session(cancelled, request.timeout); session.init();
        auto* d = device(session, request, result); snapshot(d, result);
        try {
            if (request.command != "status") {
                if (!nm_device_get_managed(d)) throw std::runtime_error("无线接口未由 NetworkManager 管理，请检查现有网络服务");
                if (!nm_client_wireless_get_enabled(session.client.get()) || !nm_client_wireless_hardware_get_enabled(session.client.get()))
                    throw std::runtime_error("Wi-Fi 已禁用或被 rfkill 屏蔽，请先启用无线设备");
            }
            if (request.command == "scan") scan(session, d, result);
            else if (request.command == "connect") connect(session, d, request, progress);
            else if (request.command == "disconnect") disconnect(session, d);
            else if (request.command == "ping") {
                if (nm_device_get_state(d) != NM_DEVICE_STATE_ACTIVATED) throw std::runtime_error("所选无线接口尚未连接");
                ping(session, request, result, progress);
            }
        } catch (...) {
            snapshot(d, result);
            throw;
        }
        snapshot(d, result); result.ok = true;
        result.message = request.command == "connect" ? (request.remember ? "已连接并保存，启用自动重连" : "已临时连接，断开后删除测试配置") : "操作完成";
    } catch (const std::exception& e) { result.message = e.what(); }
    wipe(request.password);
    return result;
}
}
