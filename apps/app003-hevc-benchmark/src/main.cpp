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
                             "[--width 1920 --height 1080 --fps 30/1 --pixel-format NV12|NV21] [--frame-limit N]\n"
                             "  [--input-hevc FILE] (requires FILE.index: offset size presentation_index)\n"
                             "  [--time-delta-rc default|off|on] [--bitrate-mode default|vbr|cbr] [--save-bitstream 0|1] [--pattern ball|snow|smpte]\n"
                             "NV12 / HEVC Main. CLI is headless. GUI preview is independent.\n"
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
            else if (arg == "--width") options.width = integer(value, 64, 8192);
            else if (arg == "--height") options.height = integer(value, 64, 8192);
            else if (arg == "--fps") {
                auto slash = value.find('/');
                options.fps_num = integer(value.substr(0, slash), 1, 240000);
                options.fps_den = slash == std::string::npos ? 1 : integer(value.substr(slash + 1), 1, 10000);
            }
            else if (arg == "--frame-limit") options.frame_limit = integer(value, 1, 1000000);
            else if (arg == "--input-hevc") options.input_hevc = value;
            else if (arg == "--time-delta-rc") options.time_delta_rc = value;
            else if (arg == "--bitrate-mode") options.bitrate_mode = value;
            else if (arg == "--pattern") options.pattern = value;
            else if (arg == "--save-bitstream") options.save_bitstream = integer(value, 0, 1);
            else if (arg == "--pacing") options.pacing = value;
            else if (arg == "--quality") options.quality_mode = value;
            else if (arg == "--pixel-format") options.pixel_format = value;
            else if (arg == "--input-nv12" || arg == "--input-raw") options.input_nv12 = value;
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
        if (options.width % 2 || options.height % 2 ||
            double(options.fps_num) / options.fps_den > 240)
            throw std::invalid_argument("Even dimensions and fps <=240 required");
        if (options.pixel_format != "NV12" && options.pixel_format != "NV21")
            throw std::invalid_argument("Supported raw formats are NV12 and NV21 (8-bit)");
        if (options.time_delta_rc != "default" && options.time_delta_rc != "off" && options.time_delta_rc != "on")
            throw std::runtime_error("Time-delta RC must be default, off or on");
        if (options.bitrate_mode != "default" && options.bitrate_mode != "vbr" && options.bitrate_mode != "cbr")
            throw std::invalid_argument("Invalid bitrate mode");
        if (options.pattern != "ball" && options.pattern != "snow" && options.pattern != "smpte")
            throw std::invalid_argument("Invalid deterministic source pattern");
        if (!options.input_hevc.empty() && (options.mode != "decode" || !options.input_nv12.empty()))
            throw std::invalid_argument("--input-hevc requires decode and excludes --input-nv12");
        if (options.save_bitstream && options.mode == "decode")
            throw std::invalid_argument("--save-bitstream requires encoder");
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
