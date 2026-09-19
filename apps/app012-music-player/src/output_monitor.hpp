// SPDX-License-Identifier: MIT
#pragma once
#include <pulse/pulseaudio.h>
#include <chrono>
#include <string>
#include <vector>
#include <unistd.h>

struct AudioOutput {
    uint32_t index = PA_INVALID_INDEX;
    std::string name, description, driver, factory;
    bool port_missing = false;
    bool usable() const {
        // PAL sinks are virtual too. Only reject known null implementations,
        // not PA_SINK_HARDWARE absence or device.class=abstract.
        return !name.empty() && name != "auto_null" &&
            driver != "module-null-sink" && driver != "module-null-sink.c" &&
            factory != "support.null-audio-sink" && !port_missing;
    }
};

inline AudioOutput output_from_info(const pa_sink_info& sink) {
    const auto value = [](const char* text) { return std::string(text ? text : ""); };
    return {sink.index, value(sink.name), value(sink.description), value(sink.driver),
        value(pa_proplist_gets(sink.proplist, "factory.name")),
        (sink.active_port && sink.active_port->available == PA_PORT_AVAILABLE_NO) ||
        value(pa_proplist_gets(sink.proplist, "q6a.jack.connected")) == "false"};
}

inline bool route_usable(const std::vector<AudioOutput>& outputs, bool valid,
                         const std::string& name, uint32_t stream, bool require_stream) {
    if (!valid) return false;
    for (const auto& output : outputs) {
        if (output.name == name)
            return output.usable() && (stream == output.index ||
                (!require_stream && stream == PA_INVALID_INDEX));
    }
    return false;
}

class OutputMonitor {
    using Clock = std::chrono::steady_clock;
    pa_mainloop* loop = pa_mainloop_new();
    pa_context* context = nullptr;
    bool ready = false, busy = false, dirty = true, failed = false;
    Clock::time_point updated{}, started{}, retry{};
    std::vector<AudioOutput> pending;
    std::string pending_default;
    uint32_t pending_stream = PA_INVALID_INDEX;
    static std::string value(const char* s) { return s ? s : ""; }
    void operation(pa_operation* op) {
        if (op) pa_operation_unref(op);
        else failed = true;
    }
    void disconnect() {
        ready = false; busy = false; valid = false; failed = false;
        if (context) {
            pa_context_set_state_callback(context, nullptr, nullptr);
            pa_context_set_subscribe_callback(context, nullptr, nullptr);
            pa_context_disconnect(context);
            pa_context_unref(context); context = nullptr;
        }
    }
    void connect() {
        failed = false;
        context = pa_context_new(pa_mainloop_get_api(loop), "Music player output monitor");
        if (!context) { failed = true; return; }
        started = Clock::now();
        pa_context_set_state_callback(context, [](pa_context* c, void* p) {
            auto& self = *static_cast<OutputMonitor*>(p);
            const auto state = pa_context_get_state(c);
            if (state == PA_CONTEXT_READY) {
                self.ready = true;
                pa_context_set_subscribe_callback(c, [](pa_context*, pa_subscription_event_type_t, uint32_t, void* d) {
                    static_cast<OutputMonitor*>(d)->dirty = true;
                }, p);
                self.operation(pa_context_subscribe(c, static_cast<pa_subscription_mask_t>(
                    PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT),
                    nullptr, nullptr));
                self.dirty = true;
            } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) self.failed = true;
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) failed = true;
    }
    void refresh() {
        busy = true; dirty = false; started = Clock::now();
        pending.clear(); pending_default.clear(); pending_stream = PA_INVALID_INDEX;
        operation(pa_context_get_server_info(context, [](pa_context* c, const pa_server_info* info, void* p) {
            auto& self = *static_cast<OutputMonitor*>(p);
            if (!info) { self.failed = true; return; }
            self.pending_default = value(info->default_sink_name);
            self.operation(pa_context_get_sink_info_list(c, [](pa_context* c2, const pa_sink_info* sink, int end, void* p2) {
                auto& s = *static_cast<OutputMonitor*>(p2);
                if (end < 0) { s.failed = true; return; }
                if (!end && sink) {
                    s.pending.push_back(output_from_info(*sink));
                    return;
                }
                s.operation(pa_context_get_sink_input_info_list(c2, [](pa_context*, const pa_sink_input_info* input, int e, void* p3) {
                    auto& m = *static_cast<OutputMonitor*>(p3);
                    if (e < 0) { m.failed = true; return; }
                    if (!e && input) {
                        if (value(pa_proplist_gets(input->proplist, PA_PROP_APPLICATION_ID)) == m.stream_id)
                            m.pending_stream = input->sink;
                        return;
                    }
                    m.outputs = m.pending; m.default_name = m.pending_default;
                    m.stream_sink = m.pending_stream; m.valid = true;
                    m.updated = Clock::now(); m.busy = false;
                }, p2));
            }, p));
        }, this));
    }
public:
    const std::string stream_id = "org.q6a.music-player." + std::to_string(getpid());
    std::vector<AudioOutput> outputs;
    std::string default_name;
    uint32_t stream_sink = PA_INVALID_INDEX;
    bool valid = false;
    OutputMonitor() { if (loop) connect(); }
    ~OutputMonitor() { disconnect(); if (loop) pa_mainloop_free(loop); }
    OutputMonitor(const OutputMonitor&) = delete;
    OutputMonitor& operator=(const OutputMonitor&) = delete;
    void poll() {
        if (!loop) return;
        const auto now = Clock::now();
        if (!context && now >= retry) connect();
        // Bound work on the GTK thread; all callbacks run on this same thread.
        for (int n = 0; n < 64 && pa_mainloop_iterate(loop, 0, nullptr) > 0; ++n) {}
        if (failed || (context && (!ready || busy) && now - started > std::chrono::seconds(3))) {
            disconnect(); retry = now + std::chrono::seconds(1); return;
        }
        if (ready && !busy && (dirty || now - updated > std::chrono::seconds(1))) refresh();
    }
    const AudioOutput* find(const std::string& name) const {
        if (!valid) return nullptr;
        for (const auto& o : outputs) if (o.name == (name.empty() ? default_name : name)) return &o;
        return nullptr;
    }
    bool stream_usable() const {
        if (!valid) return false;
        for (const auto& o : outputs) if (o.index == stream_sink) return o.usable();
        return false;
    }
};
