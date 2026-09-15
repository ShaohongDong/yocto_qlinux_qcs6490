// SPDX-License-Identifier: MIT
#include "wifi.hpp"
#include <glib-unix.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace {
volatile sig_atomic_t password_signal = 0;
void password_interrupted(int signal) { password_signal = signal; }
int number(const std::string& value) {
    size_t end = 0; int result = std::stoi(value, &end);
    if (end != value.size()) throw std::runtime_error("数字参数无效");
    return result;
}
void password(std::string& value, bool from_stdin) {
    termios old{}; bool terminal = isatty(STDIN_FILENO);
    struct sigaction previous_int{}, previous_term{}, handler{};
    if (!from_stdin && !terminal) throw std::runtime_error("非交互连接请使用 --password-stdin，或指定 --security open");
    if (terminal) {
        if (tcgetattr(STDIN_FILENO, &old) != 0) throw std::runtime_error("无法关闭密码回显");
        termios hidden = old; hidden.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) throw std::runtime_error("无法关闭密码回显");
        password_signal = 0; handler.sa_handler = password_interrupted; sigemptyset(&handler.sa_mask);
        sigaction(SIGINT, &handler, &previous_int); sigaction(SIGTERM, &handler, &previous_term);
        std::cerr << "Wi-Fi 密码：";
    }
    bool read = static_cast<bool>(std::getline(std::cin, value));
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        sigaction(SIGINT, &previous_int, nullptr); sigaction(SIGTERM, &previous_term, nullptr);
        std::cerr << '\n';
        if (password_signal) throw std::runtime_error("密码输入已取消");
    }
    if (!read) throw std::runtime_error("无法读取密码");
}
}
int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--gui")) return wifi::gui();
    if (argc == 2 && std::string(argv[1]) == "--self-test") return wifi::self_test();
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "wifi-test --gui | --self-test\n"
            "wifi-test scan|status|disconnect [--interface wlan0] [--json]\n"
            "wifi-test connect --ssid SSID [--security auto|open|wpa2|wpa3] [--hidden]\n"
            "                  [--bssid MAC] [--remember] [--password-stdin] [--interface wlan0] [--json]\n"
            "wifi-test ping [--target IP_OR_HOST] [--count 4] [--interface wlan0] [--json]\n"
            "All operations: --timeout SECONDS (1–300, default 60). Passwords are never accepted as arguments.\n";
        return 0;
    }
    wifi::Request request; request.command = argv[1]; bool json = false, input = false;
    // Detect output mode even if a later argument is invalid.
    for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--json") json = true;
    try {
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--json") continue;
            if (arg == "--remember") { request.remember = true; continue; }
            if (arg == "--hidden") { request.hidden = true; continue; }
            if (arg == "--password-stdin") { input = true; continue; }
            if (arg != "--interface" && arg != "--ssid" && arg != "--bssid" && arg != "--security" && arg != "--target" && arg != "--count" && arg != "--timeout")
                throw std::runtime_error("未知参数；请运行 --help");
            if (++i >= argc) throw std::runtime_error("参数缺少值");
            const std::string value = argv[i];
            if (arg == "--interface") request.interface = value;
            else if (arg == "--ssid") request.ssid = value;
            else if (arg == "--bssid") request.bssid = value;
            else if (arg == "--security") request.security = value;
            else if (arg == "--target") request.target = value;
            else if (arg == "--count") request.count = number(value);
            else if (arg == "--timeout") request.timeout = number(value);
        }
        wifi::validate(request);
        if (request.command == "connect" && request.security != "open") password(request.password, input);
        auto* cancel = g_cancellable_new();
        const guint sigint = g_unix_signal_add(SIGINT, [](gpointer data) -> gboolean { g_cancellable_cancel(G_CANCELLABLE(data)); return G_SOURCE_CONTINUE; }, cancel);
        const guint sigterm = g_unix_signal_add(SIGTERM, [](gpointer data) -> gboolean { g_cancellable_cancel(G_CANCELLABLE(data)); return G_SOURCE_CONTINUE; }, cancel);
        wifi::Result result; std::atomic<bool> done{false};
        std::thread worker([&] {
            result = wifi::run(request, cancel, [&](const std::string& text) { if (!json) std::cerr << text << '\n'; });
            done = true;
        });
        while (!done) { while (g_main_context_iteration(nullptr, FALSE)) {} g_usleep(10000); }
        worker.join(); g_source_remove(sigint); g_source_remove(sigterm); g_object_unref(cancel);
        if (json) std::cout << result.json() << '\n';
        else {
            std::cout << result.message << "\n接口: " << result.interface << "  状态: " << result.state << "  SSID: " << result.ssid << '\n';
            for (const auto& ip : result.addresses) std::cout << "地址: " << ip << '\n';
            if (!result.gateway.empty()) std::cout << "网关: " << result.gateway << '\n';
            for (const auto& ap : result.access_points)
                std::cout << (ap.active ? "* " : "  ") << ap.ssid << "\t" << ap.bssid << "\t" << ap.strength << "%\t" << ap.frequency << " MHz\t" << ap.security << '\n';
        }
        return result.ok ? 0 : 1;
    } catch (const std::exception& e) {
        wifi::wipe(request.password);
        wifi::Result result; result.command = request.command; result.message = e.what();
        std::cout << (json ? result.json() : result.message) << '\n'; return 2;
    }
}
