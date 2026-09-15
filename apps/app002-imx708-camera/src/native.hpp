// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace imx708 {
struct RawSettings {
    int exposure = 1306, gain = 112, black = 64;
    double red = 1.0, blue = 1.0;
};
struct NativeFrame {
    unsigned width = 2304, height = 1296, stride = 0, sequence = 0;
    uint64_t timestamp_ns = 0;
    RawSettings settings;
    std::vector<uint8_t> raw, rgb;
};
std::vector<uint8_t> raw_to_rgb(const uint8_t* raw, size_t bytes, unsigned width,
                              unsigned height, unsigned stride, const RawSettings& settings);
bool valid_raw_buffer(size_t bytes, size_t expected, bool error, bool monotonic,
                      uint64_t timestamp, uint64_t previous);
struct ControlRange { int minimum, maximum, step, value; };
class NativeCapture {
public:
    ~NativeCapture();
    void prepare();
    void start(std::function<void(NativeFrame&&)> frame, std::function<void(std::string)> error);
    void stop();
    void settings(const RawSettings& value);
    ControlRange exposure {}, gain {};
    unsigned stride = 0, sizeimage = 0;
    double fps = 0;
    int test_pattern = 0;
    std::string media_path, sensor_path, video_path;
    std::atomic<unsigned long> frames {0}, bad_frames {0}, gaps {0};
private:
    void loop(const std::function<void(NativeFrame&&)>& frame);
    std::atomic<bool> stopping {true};
    std::thread worker;
    std::mutex mutex;
    RawSettings requested;
    std::string sensor_power_status;
};
int native_main(int argc, char** argv);
}
