// SPDX-License-Identifier: MIT
#include "wifi.hpp"
#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace wifi {
std::string quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { const char* digits = "0123456789abcdef"; out += "\\u00"; out += digits[c >> 4]; out += digits[c & 15]; }
        else out += c;
    }
    return out + '"';
}
std::string hex(const unsigned char* bytes, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) { out += "0123456789abcdef"[bytes[i] >> 4]; out += "0123456789abcdef"[bytes[i] & 15]; }
    return out;
}
void wipe(std::string& value) {
    volatile char* p = value.empty() ? nullptr : &value[0];
    for (size_t i = 0; i < value.size(); ++i) p[i] = 0;
    value.clear();
}
void validate(const Request& r) {
    if (r.command != "scan" && r.command != "status" && r.command != "connect" && r.command != "disconnect" && r.command != "ping")
        throw std::runtime_error("未知命令");
    if (r.timeout < 1 || r.timeout > 300 || r.count < 1 || r.count > 1000)
        throw std::runtime_error("超时范围为 1–300 秒，ping 次数范围为 1–1000");
    if (!r.interface.empty() && (r.interface[0] == '-' || !std::regex_match(r.interface, std::regex("[a-zA-Z0-9_.:-]{1,15}"))))
        throw std::runtime_error("无线接口名称无效");
    if (!r.bssid.empty() && !std::regex_match(r.bssid, std::regex("([a-fA-F0-9]{2}:){5}[a-fA-F0-9]{2}")))
        throw std::runtime_error("BSSID 格式无效");
    if (r.command == "connect") {
        if (r.ssid.empty() || r.ssid.size() > 32 || r.ssid.find('\0') != std::string::npos)
            throw std::runtime_error("SSID 必须为 1–32 字节且不包含 NUL");
        if (r.security != "auto" && r.security != "open" && r.security != "wpa2" && r.security != "wpa3")
            throw std::runtime_error("安全类型仅支持 auto/open/wpa2/wpa3");
        if (r.hidden && r.security == "auto") throw std::runtime_error("隐藏网络需要明确选择安全类型");
        if (r.password.find('\0') != std::string::npos) throw std::runtime_error("密码不能包含 NUL");
    }
    if (!r.target.empty() && (r.target.size() > 253 || r.target[0] == '-' ||
        !std::regex_match(r.target, std::regex("[a-zA-Z0-9_.:%-]+"))))
        throw std::runtime_error("请输入 IP 地址或域名，不要输入 URL 或命令参数");
}
void parse_ping(Result& r) {
    std::smatch match;
    if (std::regex_search(r.output, match, std::regex("([0-9]+) packets transmitted, ([0-9]+) (?:packets )?received,.*?([0-9.]+)% packet loss"))) {
        r.sent = std::stoi(match[1]); r.received = std::stoi(match[2]); r.loss = std::stod(match[3]); r.has_statistics = true;
    }
    if (std::regex_search(r.output, match, std::regex("(?:rtt|round-trip) min/avg/max/(?:mdev|stddev) = ([0-9.]+)/([0-9.]+)/([0-9.]+)/"))) {
        r.minimum = std::stod(match[1]); r.average = std::stod(match[2]); r.maximum = std::stod(match[3]); r.has_rtt = true;
    }
}
std::string Result::json() const {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << "{\"ok\":" << (ok ? "true" : "false") << ",\"command\":" << quote(command)
      << ",\"message\":" << quote(message) << ",\"interface\":" << quote(interface)
      << ",\"ssid\":" << quote(ssid) << ",\"state\":" << quote(state) << ",\"gateway\":" << quote(gateway)
      << ",\"target\":" << quote(target) << ",\"output\":" << quote(output) << ",\"interfaces\":[";
    for (size_t i = 0; i < interfaces.size(); ++i) { if (i) o << ','; o << quote(interfaces[i]); }
    o << "],\"addresses\":[";
    for (size_t i = 0; i < addresses.size(); ++i) { if (i) o << ','; o << quote(addresses[i]); }
    o << "],\"access_points\":[";
    for (size_t i = 0; i < access_points.size(); ++i) {
        const auto& a = access_points[i]; if (i) o << ',';
        o << "{\"ssid\":" << quote(a.ssid) << ",\"ssid_hex\":" << quote(a.ssid_hex) << ",\"bssid\":" << quote(a.bssid)
          << ",\"security\":" << quote(a.security) << ",\"strength\":" << a.strength << ",\"frequency_mhz\":" << a.frequency
          << ",\"active\":" << (a.active ? "true" : "false") << '}';
    }
    o << "],\"ping\":";
    if (!has_statistics) o << "null";
    else {
        o << "{\"sent\":" << sent << ",\"received\":" << received << ",\"loss_percent\":" << loss << ",\"rtt_ms\":";
        if (!has_rtt) o << "null";
        else o << "{\"min\":" << minimum << ",\"avg\":" << average << ",\"max\":" << maximum << '}';
        o << '}';
    }
    return o.str() + '}';
}
int self_test() {
    try {
        if (quote("a\"\\\n") != "\"a\\\"\\\\\\u000a\"") return 1;
        Result r; r.output = "4 packets transmitted, 3 received, 25% packet loss, time 3000ms\nrtt min/avg/max/mdev = 1.000/2.000/3.000/0.500 ms\n";
        parse_ping(r);
        if (!r.has_statistics || !r.has_rtt || r.received != 3 || r.loss != 25 || r.average != 2) return 1;
        Request request; request.command = "connect"; request.ssid = "测试: \\\""; validate(request);
        request.target = "-c";
        try { validate(request); return 1; } catch (const std::runtime_error&) {}
        return 0;
    } catch (...) { return 1; }
}
}
