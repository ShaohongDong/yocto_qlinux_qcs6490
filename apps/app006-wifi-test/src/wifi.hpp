// SPDX-License-Identifier: MIT
#pragma once
#include <gio/gio.h>
#include <functional>
#include <string>
#include <vector>

namespace wifi {
struct Request {
    std::string command = "status", interface, ssid, bssid, security = "auto", password, target;
    bool remember = false, hidden = false;
    int count = 4, timeout = 60;
};
struct AccessPoint {
    std::string ssid, ssid_hex, bssid, security;
    unsigned strength = 0, frequency = 0;
    bool active = false;
};
struct Result {
    bool ok = false;
    std::string command, message, interface, ssid, state, gateway, target, output;
    std::vector<std::string> interfaces, addresses;
    std::vector<AccessPoint> access_points;
    int sent = 0, received = 0;
    double loss = 0, minimum = 0, average = 0, maximum = 0;
    bool has_statistics = false, has_rtt = false;
    std::string json() const;
};
using Progress = std::function<void(const std::string&)>;
std::string quote(const std::string&);
std::string hex(const unsigned char*, size_t);
void wipe(std::string&);
void validate(const Request&);
void parse_ping(Result&);
Result run(Request&, GCancellable*, const Progress& = {});
int self_test();
int gui();
}
