// SPDX-License-Identifier: MIT
#pragma once
#include "metrics.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
namespace hevc {
struct Options {
    std::string mode = "loopback", output = "hevc-results";
    std::string encoder = "v4l2h265enc", decoder = "v4l2h265dec";
    std::string latency_profile = "baseline";
    std::string pacing = "auto", quality_mode = "full", input_nv12;
    int operating_rate = 0; // 0 preserves driver default; otherwise decoder fps in Q16.
    int seconds = 60, warmup = 5, bitrate = 8000000;
    bool preview = false;
};
struct View {
    std::mutex mutex;
    std::string text = "Ready. Hardware measurements have not run.";
    std::vector<uint8_t> rgb;
    int stride = 640 * 3;
    size_t submitted_frames = 0;
};
// Returns 0 for a measured PASS, 1 for failure, 2 for cancellation.
int run(const Options&, std::atomic<bool>& cancel, View* view = nullptr);
int ui(int argc, char** argv, Options options);
}
