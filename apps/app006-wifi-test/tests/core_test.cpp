// SPDX-License-Identifier: MIT
#include "wifi.hpp"
#include <iostream>
#include <stdexcept>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void invalid(wifi::Request request) {
    bool rejected = false; try { wifi::validate(request); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "unsafe input accepted");
}
int main() {
    try {
        require(wifi::self_test() == 0, "self-test");
        wifi::Request r; r.command = "connect"; r.ssid = "咖啡 Wi-Fi:'\\\"$();"; wifi::validate(r);
        r.ssid = std::string(33, 'a'); invalid(r); r.ssid = std::string("a\0b", 3); invalid(r); r.ssid = "test";
        r.hidden = true; invalid(r); r.security = "wpa3"; wifi::validate(r); r.hidden = false;
        r.interface = "wlan0;id"; invalid(r); r.interface = "wlan0";
        r.bssid = "aa:bb:cc:dd:ee:ff"; wifi::validate(r); r.bssid = "wrong"; invalid(r); r.bssid.clear();
        for (const auto* target : {"-I", "$(id)", "host name", "https://example.org", "a\nb"}) { r.target = target; invalid(r); }
        for (const auto* target : {"192.0.2.1", "2001:db8::1", "fe80::1%wlan0", "example.org"}) { r.target = target; wifi::validate(r); }
        r.count = 0; invalid(r); r.count = 1001; invalid(r); r.count = 4; r.timeout = 301; invalid(r);
        wifi::Result result; result.output = "4 packets transmitted, 0 received, 100% packet loss, time 3020ms\n";
        wifi::parse_ping(result); require(result.has_statistics && !result.has_rtt && result.loss == 100, "total loss");
        result = {}; result.output = "ping: invalid name"; wifi::parse_ping(result); require(!result.has_statistics, "DNS failure has no statistics");
        result = {}; result.output = "1 packets transmitted, 1 received, 0% packet loss, time 0ms\nrtt min/avg/max/mdev = 0.123/0.123/0.123/0.000 ms\n";
        wifi::parse_ping(result); require(result.has_rtt && result.average == 0.123, "fractional latency");
        std::string secret = "private"; wifi::wipe(secret); require(secret.empty(), "wipe");
        require(wifi::hex(reinterpret_cast<const unsigned char*>("\0\xff"), 2) == "00ff", "binary SSID");
        std::cout << "Wi-Fi core tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
