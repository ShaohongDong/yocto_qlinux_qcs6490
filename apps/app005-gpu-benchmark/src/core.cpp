// SPDX-License-Identifier: MIT
#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <cctype>

namespace gpu {
namespace fs = std::filesystem;
static int integer(const std::string& s, int low, int high) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected a positive integer: " + s);
    auto n = std::stoll(s);
    if (n < low || n > high) throw std::runtime_error("Value out of range: " + s);
    return static_cast<int>(n);
}
Options parse(int argc, char** argv) {
    Options o;
    bool gui = false, headless = false, suite_set = false, offscreen = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::runtime_error("Missing value for " + a);
            return argv[i];
        };
        if (a == "--gui") gui = true;
        else if (a == "--headless") headless = true;
        else if (a == "--self-test") o.self_test = true;
        else if (a == "--check-model") o.check_model = true;
        else if (a == "--model") o.model = value();
        else if (a == "--instances") o.instances = integer(value(), 1, 64);
        else if (a == "--offscreen") offscreen = true;
        else if (a == "--window") o.window = true;
        else if (a == "--autorun") o.autorun = true;
        else if (a == "--quit-after-run") o.quit_after_run = true;
        else if (a == "--suite") { o.suite = value(); suite_set = true; }
        else if (a == "--duration") o.duration = integer(value(), 1, 3600);
        else if (a == "--output") o.output = value();
        else if (a == "--device") o.device = value();
        else if (a == "--size") {
            auto s = value(); auto pos = s.find('x');
            if (pos == std::string::npos) throw std::runtime_error("Size must be WIDTHxHEIGHT");
            o.width = integer(s.substr(0, pos), 64, 4096);
            o.height = integer(s.substr(pos + 1), 64, 4096);
        } else throw std::runtime_error("Unknown option: " + a);
    }
    if (gui && headless) throw std::runtime_error("Choose either --gui or --headless");
    if (gui && !suite_set) o.suite="model3d";
    if (o.suite != "quick" && o.suite != "stability" && o.suite != "model3d") throw std::runtime_error("Suite must be quick, stability or model3d");
    o.gui = gui;
    if (o.window && !gui) throw std::runtime_error("--window requires --gui");
    if (offscreen && (!gui || o.window)) throw std::runtime_error("--offscreen requires --gui and conflicts with --window");
    if (gui) o.window=!offscreen;
    if ((o.autorun || o.quit_after_run) && !gui) throw std::runtime_error("GUI automation options require --gui");
    if (!o.duration) o.duration = o.suite == "quick" ? 10 : 60;
    if (o.output.empty()) o.output = "gpu-results-" + timestamp();
    return o;
}
std::string timestamp() {
    auto t = std::time(nullptr); std::tm tm{}; gmtime_r(&t, &tm);
    std::ostringstream s; s << std::put_time(&tm, "%Y%m%dT%H%M%SZ"); return s.str();
}
std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
bool software_renderer(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    for (auto token : {"llvmpipe", "softpipe", "swrast", "software", "swiftshader", "lavapipe"})
        if (name.find(token) != std::string::npos) return true;
    return name.empty();
}
double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    if(q<=0) return *std::min_element(v.begin(),v.end());
    if(q>=1) return *std::max_element(v.begin(),v.end());
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(q * v.size())) - 1)];
}
static double mean(const std::vector<double>& v) {
    return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}
static std::string nullable(double n) {
    if (n < 0 || !std::isfinite(n)) return "null";
    std::ostringstream s; s << std::setprecision(10) << n; return s.str();
}
void Report::save() const {
    fs::create_directories(options.output);
    std::ofstream out(fs::path(options.output) / "summary.json.tmp");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << "{\n\"schema_version\":1,\"workload_version\":2,\"started_utc\":" << quote(started)
        << ",\"status\":" << quote(status) << ",\"error\":" << quote(error)
        << ",\"performance_verdict\":\"baseline_only\",\"renderer\":" << quote(renderer)
        << ",\"vendor\":" << quote(vendor) << ",\"gl_version\":" << quote(version)
        << ",\"device\":" << quote(device) << ",\"mode\":" << quote(options.window ? "window" : "offscreen")
        << ",\"suite\":" << quote(options.suite) << ",\"width\":" << options.width << ",\"height\":" << options.height
        << ",\"duration_per_scene_s\":" << options.duration << ",\"warmup_per_scene_s\":3"
        << ",\"model\":" << quote(options.model) << ",\"model_sha256\":" << quote(model_hash)
        << ",\"model_triangles_per_instance\":" << model_triangles << ",\"model_instances\":" << options.instances
        << ",\"pixel_check_passed\":" << (pixels_checked ? "true" : "false")
        << ",\"frame_timing\":\"CPU submission through GPU fence completion; presentation excluded\""
        << ",\"scenes\":[";
    bool comma = false;
    for (const auto& s : scenes) {
        if (comma) out << ',';
        comma = true;
        std::vector<double> rates;
        for(const auto& p:samples) if(p.scene==s.name && p.interval>=0.9 && p.second<=s.seconds) rates.push_back(p.fps);
        const size_t count=std::min<size_t>(5,rates.size()/2);
        double first=count?std::accumulate(rates.begin(),rates.begin()+count,0.0)/count:-1;
        double last=count?std::accumulate(rates.end()-count,rates.end(),0.0)/count:-1;
        out << "{\"name\":" << quote(s.name) << ",\"completed\":" << (s.completed ? "true" : "false")
            << ",\"elapsed_s\":" << s.seconds << ",\"frames\":" << s.frames.size()
            << ",\"fps\":" << nullable(s.seconds > 0 ? s.frames.size() / s.seconds : -1)
            << ",\"mean_frame_ms\":" << nullable(s.frames.empty() ? -1 : mean(s.frames))
            << ",\"p95_frame_ms\":" << nullable(s.frames.empty() ? -1 : percentile(s.frames, .95))
            << ",\"p99_frame_ms\":" << nullable(s.frames.empty() ? -1 : percentile(s.frames, .99))
            << ",\"mean_gpu_ms\":" << nullable(s.gpu_times.empty() ? -1 : mean(s.gpu_times))
            << ",\"gpu_valid_samples\":" << s.gpu_times.size()
            << ",\"first_window_fps\":" << nullable(first) << ",\"last_window_fps\":" << nullable(last)
            << ",\"comparison_window_samples\":" << count
            << ",\"last_over_first_fps_ratio\":" << nullable(first>0?last/first:-1) << '}';
    }
    out << "],\"samples\":["; comma = false;
    for (const auto& s : samples) {
        if (comma) out << ',';
        comma = true;
        out << "{\"scene\":" << quote(s.scene) << ",\"elapsed_s\":" << s.second << ",\"interval_s\":" << s.interval << ",\"fps\":" << s.fps
            << ",\"mean_frame_ms\":" << s.mean_ms << ",\"mean_gpu_ms\":" << nullable(s.gpu_ms)
            << ",\"frequency_hz\":" << nullable(s.frequency) << ",\"busy_percent\":" << nullable(s.busy)
            << ",\"temperature_millicelsius\":{";
        bool sep = false;
        for (const auto& t : s.temperatures) { if (sep) out << ','; sep = true; out << quote(t.first) << ':' << nullable(t.second); }
        out << "}}";
    }
    out << "]}\n"; out.close();
    fs::rename(fs::path(options.output) / "summary.json.tmp", fs::path(options.output) / "summary.json");
    std::ofstream csv(fs::path(options.output) / "samples.csv.tmp");
    csv.exceptions(std::ios::badbit | std::ios::failbit);
    csv << "scene,elapsed_s,fps,mean_frame_ms,mean_gpu_ms,frequency_hz,busy_percent\n";
    for (const auto& s : samples)
        csv << s.scene << ',' << s.second << ',' << s.fps << ',' << s.mean_ms << ',' << nullable(s.gpu_ms)
            << ',' << nullable(s.frequency) << ',' << nullable(s.busy) << '\n';
    csv.close(); fs::rename(fs::path(options.output)/"samples.csv.tmp",fs::path(options.output)/"samples.csv");
    std::ofstream thermal(fs::path(options.output)/"thermal.csv.tmp");
    thermal.exceptions(std::ios::badbit|std::ios::failbit);
    thermal<<"scene,elapsed_s,zone,millicelsius\n";
    for(const auto& s:samples) for(const auto& t:s.temperatures) {
        std::string escaped=t.first; size_t pos=0;
        while((pos=escaped.find('"',pos))!=std::string::npos) { escaped.insert(pos,1,'"'); pos+=2; }
        thermal<<s.scene<<','<<s.second<<",\""<<escaped<<"\","<<nullable(t.second)<<'\n';
    }
    thermal.close(); fs::rename(fs::path(options.output)/"thermal.csv.tmp",fs::path(options.output)/"thermal.csv");
    std::ofstream summary(fs::path(options.output)/"summary.txt.tmp");
    summary.exceptions(std::ios::badbit|std::ios::failbit);
    summary<<std::fixed<<std::setprecision(2);
    for(const auto& s:scenes) {
        if(s.frames.empty()) continue;
        summary<<s.name<<": "<<(s.seconds>0?s.frames.size()/s.seconds:0)<<" FPS | "
            <<mean(s.frames)<<" ms/frame | P95 "<<percentile(s.frames,.95)<<" ms";
        if(!s.gpu_times.empty()) summary<<" | GPU "<<mean(s.gpu_times)<<" ms";
        if(!s.completed) summary<<" (partial)";
        summary<<'\n';
    }
    summary.close(); fs::rename(fs::path(options.output)/"summary.txt.tmp",fs::path(options.output)/"summary.txt");
}
static long number(const fs::path& p) { std::ifstream f(p); long n; return f >> n ? n : -1; }
Telemetry::Telemetry() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/devfreq", ec))
        if (e.path().filename().string().find("gpu") != std::string::npos) { devfreq = e.path(); break; }
    for (const auto& e : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (e.path().filename().string().find("thermal_zone") != 0) continue;
        std::ifstream f(e.path() / "type"); std::string type; std::getline(f, type);
        thermal.emplace_back(e.path().filename().string() + ":" + type, e.path() / "temp");
    }
}
void Telemetry::read(Sample& s) const {
    if (!devfreq.empty()) {
        s.frequency = number(devfreq / "cur_freq");
        auto busy = number(devfreq / "device/gpu_busy_percentage");
        if (busy >= 0 && busy <= 100) s.busy = busy;
    }
    for (const auto& t : thermal) s.temperatures.emplace_back(t.first, number(t.second));
}
int self_test() {
    return percentile({1, 2, 3, 4, 100}, .95) == 100 && software_renderer("llvmpipe (LLVM)")
        && !software_renderer("FD643") && quote("a\n\"") == "\"a\\u000a\\\"\"" ? 0 : 1;
}
}
