// SPDX-License-Identifier: MIT
#include "../src/output_monitor.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool condition, const char* label) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--watch") {
        OutputMonitor monitor;
        int phase = 0;
        for (int n = 0; n < 900; ++n) {
            monitor.poll();
            if ((phase == 0 || phase == 2) && monitor.valid) {
                const auto* output = monitor.find("auto_null");
                if (!output) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
                require(!output->usable(), "isolated dummy server must be rejected");
                std::puts("CONNECTED_NULL"); std::fflush(stdout);
                if (++phase == 3) return 0;
            } else if (phase == 1 && !monitor.valid) {
                std::puts("DISCONNECTED"); std::fflush(stdout); ++phase;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 1;
    }
    if (argc > 1) {
        OutputMonitor monitor;
        for (int n = 0; n < 200 && !monitor.valid; ++n) {
            monitor.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto* output = monitor.find("");
        std::printf("connected=%d default=%s usable=%d\n", monitor.valid,
            monitor.default_name.c_str(), output && output->usable());
        return output && output->usable() ? 0 : 2;
    }
    pa_sink_info info{};
    info.name = "pal_sink_headset_ll"; info.driver = "PipeWire";
    info.proplist = pa_proplist_new();
    pa_proplist_sets(info.proplist, "node.virtual", "true");
    pa_proplist_sets(info.proplist, "q6a.jack.connected", "false");
    require(!output_from_info(info).usable(), "PAL unplug property rejected");
    pa_proplist_sets(info.proplist, "q6a.jack.connected", "true");
    require(output_from_info(info).usable(), "PAL connected property accepted");
    pa_proplist_free(info.proplist);
    AudioOutput hardware{7, "alsa_headphones", "Headphones", "alsa", "", false};
    AudioOutput pal{8, "pal_sink_headset_ll", "PAL virtual headset", "PipeWire", "", false};
    AudioOutput dummy{9, "auto_null", "Dummy Output", "PipeWire", "", false};
    AudioOutput named_null{10, "renamed_null", "Null", "module-null-sink.c", "", false};
    require(hardware.usable() && pal.usable(), "physical and PAL virtual sinks accepted");
    require(!dummy.usable() && !named_null.usable(), "dummy sinks rejected");
    named_null.driver = "PipeWire"; named_null.factory = "support.null-audio-sink";
    require(!named_null.usable(), "PipeWire null factory rejected");
    std::vector<AudioOutput> outputs{hardware, pal, dummy};
    require(route_usable(outputs, true, pal.name, pal.index, true), "PAL route works");
    require(!route_usable(outputs, false, pal.name, pal.index, true), "disconnect stops playback");
    require(!route_usable(outputs, true, pal.name, dummy.index, true), "move to dummy rejected");
    require(!route_usable(outputs, true, pal.name, hardware.index, true), "no silent fallback to other hardware");
    outputs[1].port_missing = true;
    require(!route_usable(outputs, true, pal.name, pal.index, true), "jack removal stops playback");
    outputs.erase(outputs.begin() + 1);
    require(!route_usable(outputs, true, pal.name, pal.index, true), "sink disappearance stops playback");
    outputs.push_back(pal);
    require(route_usable(outputs, true, pal.name, PA_INVALID_INDEX, false), "bounded stream startup allowed");
    require(!route_usable(outputs, true, pal.name, PA_INVALID_INDEX, true), "missing stream expires");
    require(route_usable(outputs, true, pal.name, pal.index, true), "explicit replay after recovery allowed");
    std::puts("PASS: output availability and stream routing");
}
