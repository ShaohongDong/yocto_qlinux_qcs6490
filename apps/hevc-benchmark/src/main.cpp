// SPDX-License-Identifier: MIT
#include "engine.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
namespace {
std::atomic<bool> cancelled{false};
void stop(int) { cancelled.store(true); }
int integer(const std::string& text, int low, int high) {
    size_t consumed = 0;
    int value = std::stoi(text, &consumed);
    if (consumed != text.size() || value < low || value > high) throw std::invalid_argument("Out of range: " + text);
    return value;
}
}
int main(int argc, char** argv) {
    hevc::Options options;
    bool gui = argc == 1;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--self-test") {
                int result = hevc::self_test();
                std::cout << "HEVC hardware-independent self-test: " << (result ? "FAIL" : "PASS") << '\n';
                return result;
            }
            if (arg == "--help") {
                std::cout << "hevc-benchmark [--gui] [--mode loopback|encode|decode] [--seconds 60]\n"
                             "  [--warmup 5] [--bitrate 8000000] [--output DIRECTORY]\n"
                             "  [--pacing auto|realtime|unpaced] [--quality full|off] [--input-nv12 FILE]\n"
                             "  [--latency-profile baseline|low|encode-low|decode-low] [--operating-rate 0|30|60|120]\n"
                             "  [--encoder v4l2h265enc] [--decoder v4l2h265dec] [--self-test]\n"
                             "1080p30 NV12 / HEVC Main. CLI is headless. GUI preview is independent.\n"
                             "Exit: 0 measured PASS; 1 failure; 2 cancelled/invalid arguments.\n";
                return 0;
            }
            if (arg == "--gui") { gui = true; continue; }
            if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
            std::string value = argv[i];
            if (arg == "--mode") options.mode = value;
            else if (arg == "--seconds") options.seconds = integer(value, 1, 600);
            else if (arg == "--warmup") options.warmup = integer(value, 0, 60);
            else if (arg == "--bitrate") options.bitrate = integer(value, 100000, 80000000);
            else if (arg == "--pacing") options.pacing = value;
            else if (arg == "--quality") options.quality_mode = value;
            else if (arg == "--input-nv12") options.input_nv12 = value;
            else if (arg == "--latency-profile") options.latency_profile = value;
            else if (arg == "--operating-rate") options.operating_rate = integer(value, 0, 120);
            else if (arg == "--output") options.output = value;
            else if (arg == "--encoder") options.encoder = value;
            else if (arg == "--decoder") options.decoder = value;
            else throw std::invalid_argument("Unknown option: " + arg);
        }
        if (options.mode != "encode" && options.mode != "decode" && options.mode != "loopback")
            throw std::invalid_argument("Mode must be encode, decode or loopback");
        if (options.latency_profile != "baseline" && options.latency_profile != "low" &&
            options.latency_profile != "encode-low" && options.latency_profile != "decode-low")
            throw std::invalid_argument("Invalid latency profile");
        if (options.operating_rate != 0 && options.operating_rate != 30 && options.operating_rate != 60 && options.operating_rate != 120)
            throw std::invalid_argument("Operating rate must be 0, 30, 60 or 120");
        if (options.operating_rate && options.mode == "encode")
            throw std::invalid_argument("Operating rate requires a decoder");
        if (options.pacing != "auto" && options.pacing != "realtime" && options.pacing != "unpaced")
            throw std::invalid_argument("Pacing must be auto, realtime or unpaced");
        if (options.quality_mode != "full" && options.quality_mode != "off")
            throw std::invalid_argument("Quality must be full or off");
        if (options.output.empty()) throw std::invalid_argument("Output directory cannot be empty");
        if (gui) return hevc::ui(argc, argv, options);
        static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        return hevc::run(options, cancelled);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
