// SPDX-License-Identifier: MIT
#include "core.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void check(bool condition) { if(!condition) throw std::runtime_error("Test assertion failed"); }
static gpu::Options options(std::initializer_list<const char*> args) {
    std::vector<char*> argv; for(auto a:args)argv.push_back(const_cast<char*>(a));
    return gpu::parse(argv.size(),argv.data());
}
int main() {
    check(gpu::self_test()==0);
    check(options({"test","--suite","stability"}).duration==60);
    check(options({"test"}).duration==10);
    check(options({"test","--gui"}).suite=="model3d");
    check(options({"test","--gui"}).window);
    check(options({"test","--suite","model3d"}).duration==60);
    check(options({"test","--size","1280x720","--duration","1"}).height==720);
    for(auto args : {std::vector<const char*>{"test","--duration","0"}, {"test","--size","1x1080"},
                    {"test","--suite","unknown"}, {"test","--gui","--headless"}, {"test","--window"},
                    {"test","--duration","1x"}, {"test","--duration"}}) {
        std::vector<char*> argv; for(auto a:args)argv.push_back(const_cast<char*>(a));
        bool threw=false; try { gpu::parse(argv.size(),argv.data()); } catch(...) { threw=true; } check(threw);
    }
    check(gpu::percentile({},.95)==0); check(gpu::percentile({10},.99)==10);
    check(gpu::percentile({5,1,4,2,3},.5)==3);
    check(gpu::percentile({5,1},0)==1); check(gpu::percentile({5,1},1)==5);
    check(gpu::software_renderer("LLVMpipe")); check(gpu::software_renderer("Google SwiftShader"));
    gpu::Report report;
    report.options.output=(std::filesystem::temp_directory_path()/("gpu-core-test-"+std::to_string(getpid()))).string();
    report.error="quote\" newline\n"; report.status="cancelled";
    gpu::Scene scene; scene.name="mixed"; scene.frames={1,2,3}; scene.seconds=.01;
    report.scenes.push_back(scene); report.save();
    std::ifstream f(std::filesystem::path(report.options.output)/"summary.json");
    std::string json((std::istreambuf_iterator<char>(f)),{});
    check(json.find("\"fps\":300")!=std::string::npos);
    check(json.find("\"completed\":false")!=std::string::npos);
    check(json.find("\"mean_gpu_ms\":null")!=std::string::npos);
    check(json.find("quote\\\" newline\\u000a")!=std::string::npos);
    std::filesystem::remove_all(report.options.output);
    std::cout<<"Core metrics, validation and partial reports passed\n";
}
