// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace gpu {
using Clock = std::chrono::steady_clock;
struct Options {
    bool gui = false, self_test = false, window = false, autorun = false, quit_after_run = false, check_model = false;
    int width = 1920, height = 1080, duration = 0, instances = 1;
    std::string suite = "quick", output, device, model;
};
Options parse(int argc, char** argv);
std::string quote(const std::string& value);
bool software_renderer(std::string name);
double percentile(std::vector<double> values, double quantile);
int self_test();
struct Sample {
    std::string scene;
    double second = 0, interval = 0, fps = 0, mean_ms = 0, gpu_ms = -1;
    long frequency = -1, busy = -1;
    std::vector<std::pair<std::string, long>> temperatures;
};
struct Scene {
    std::string name;
    double seconds = 0;
    std::vector<double> frames, gpu_times;
    bool completed = false;
};
struct Report {
    Options options;
    std::string status = "running", error, renderer, vendor, version, device, started;
    bool pixels_checked = false;
    std::string model_hash;
    size_t model_triangles = 0;
    std::vector<Scene> scenes;
    std::vector<Sample> samples;
    void save() const;
};
class Telemetry {
    std::filesystem::path devfreq;
    std::vector<std::pair<std::string, std::filesystem::path>> thermal;
public:
    Telemetry();
    void read(Sample& sample) const;
};
std::string timestamp();
}
