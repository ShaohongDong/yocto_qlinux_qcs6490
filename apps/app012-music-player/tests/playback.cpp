// SPDX-License-Identifier: MIT
#include "../src/player.hpp"
#include <cstdio>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <unistd.h>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void wait_state(Player& player, GstState expected) {
    GstState actual = GST_STATE_NULL;
    auto result = gst_element_get_state(player.pipeline, &actual, nullptr, 5 * GST_SECOND);
    player.poll();
    require(result != GST_STATE_CHANGE_FAILURE && actual == expected && player.error.empty(), "state transition failed");
}
static void wait_end(Player& player) {
    const gint64 deadline = g_get_monotonic_time() + 6000000;
    while (!player.ended && player.error.empty() && g_get_monotonic_time() < deadline) {
        player.poll();
        g_usleep(10000);
    }
    require(player.ended && player.error.empty(), "EOS not reached");
}
static void set_fake_output(Player& player) {
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    require(sink, "fakesink unavailable");
    gst_object_ref_sink(sink);
    g_object_set(sink, "sync", TRUE, nullptr);
    player.output(sink);
    gst_object_unref(sink);
}
struct Samples {
    std::atomic<int> peak{0};
    std::atomic<bool> valid{true};
};
static void check_volume(Player& player, const char* path, double volume, int expected) {
    Samples samples;
    auto* sink = gst_element_factory_make("fakesink", nullptr);
    require(sink, "volume test sink unavailable");
    gst_object_ref_sink(sink);
    g_object_set(sink, "sync", FALSE, "signal-handoffs", TRUE, nullptr);
    g_signal_connect(sink, "handoff", G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad* pad, gpointer data) {
        auto& observed = *static_cast<Samples*>(data);
        GstCaps* caps = gst_pad_get_current_caps(pad);
        const char* format = caps ? gst_structure_get_string(gst_caps_get_structure(caps, 0), "format") : nullptr;
        if (g_strcmp0(format, "S16LE")) observed.valid = false;
        if (caps) gst_caps_unref(caps);
        GstMapInfo map{};
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            for (gsize i = 0; i + 1 < map.size; i += 2) {
                const auto sample = static_cast<int16_t>(GST_READ_UINT16_LE(map.data + i));
                observed.peak = std::max(observed.peak.load(), std::abs(static_cast<int>(sample)));
            }
            gst_buffer_unmap(buffer, &map);
        } else observed.valid = false;
    }), &samples);
    player.output(sink);
    require(player.load(path), "volume fixture load failed");
    g_object_set(player.pipeline, "volume", volume, nullptr);
    require(player.play(), "volume playback failed");
    wait_end(player);
    player.output(nullptr); // release callback before its stack data expires
    gst_object_unref(sink);
    require(samples.valid && std::abs(samples.peak.load() - expected) <= 1, "decoded sample volume incorrect");
}

int main(int argc, char** argv) {
    gst_init(nullptr, nullptr);
    gchar* path = nullptr;
    const int fd = g_file_open_tmp("music-player-XXXXXX.wav", &path, nullptr);
    if (fd < 0) return 1;
    close(fd);
    // Two seconds of mono PCM, generated without codec/encoder dependencies.
    std::vector<unsigned char> wav;
    auto tag = [&](const char* s) { wav.insert(wav.end(), s, s + 4); };
    auto number = [&](unsigned n, int count) { for (int i = 0; i < count; ++i) wav.push_back((n >> (8 * i)) & 255); };
    tag("RIFF"); number(32000 + 36, 4); tag("WAVE"); tag("fmt "); number(16, 4);
    number(1, 2); number(1, 2); number(8000, 4); number(16000, 4); number(2, 2); number(16, 2);
    tag("data"); number(32000, 4); for (int i = 0; i < 16000; ++i) number(10000, 2);
    int exit_code = 0;
    try {
        require(g_file_set_contents(path, reinterpret_cast<const char*>(wav.data()), wav.size(), nullptr), "fixture write failed");
        Player player;
        require(player.pipeline, "playbin unavailable");
        check_volume(player, path, 0.5, 5000);
        check_volume(player, path, 0.25, 2500);
        check_volume(player, path, 0.0, 0);
        g_object_set(player.pipeline, "volume", 0.5, nullptr);
        set_fake_output(player);
        require(!player.load("/nonexistent/music-player-test.wav"), "missing file accepted");
        require(player.load(path), "load failed");
        require(!player.playing, "load started playback");
        require(player.play(), "play failed");
        wait_state(player, GST_STATE_PLAYING);
        player.pause(); wait_state(player, GST_STATE_PAUSED);
        gint64 before = 0, after = 0;
        gst_element_query_position(player.pipeline, GST_FORMAT_TIME, &before);
        g_usleep(150000);
        gst_element_query_position(player.pipeline, GST_FORMAT_TIME, &after);
        require(before == after, "position advanced while paused");
        require(player.seek(GST_SECOND), "seek failed");
        wait_state(player, GST_STATE_PAUSED);
        gst_element_query_position(player.pipeline, GST_FORMAT_TIME, &after);
        require(after >= GST_SECOND, "seek position incorrect");
        require(player.play(), "resume failed"); wait_end(player);
        require(player.play(), "EOS replay failed"); wait_state(player, GST_STATE_PLAYING);
        player.repeat = true;
        require(player.seek(1900 * GST_MSECOND), "loop seek failed");
        const auto deadline = g_get_monotonic_time() + 3000000;
        bool looped = false;
        while (!looped && g_get_monotonic_time() < deadline) {
            looped = player.poll(); g_usleep(10000);
        }
        require(looped && player.playing && !player.ended && player.error.empty(), "repeat failed");
        player.repeat = false;
        require(player.load(path) && !player.playing, "file switch failed");
        require(player.play(), "switch replay failed"); wait_state(player, GST_STATE_PLAYING);
        set_fake_output(player);
        require(!player.playing, "output switch did not stop");
        require(player.play(), "output replay failed"); wait_state(player, GST_STATE_PLAYING);
        player.stop();
        require(g_file_set_contents(path, "invalid music", -1, nullptr), "bad fixture write failed");
        require(player.load(path), "bad file load failed");
        player.play();
        const auto error_deadline = g_get_monotonic_time() + 5000000;
        while (player.error.empty() && g_get_monotonic_time() < error_deadline) { player.poll(); g_usleep(10000); }
        require(!player.error.empty() && !player.playing, "corrupt file did not fail cleanly");
        require(g_file_set_contents(path, reinterpret_cast<const char*>(wav.data()), wav.size(), nullptr), "fixture restore failed");
        require(player.load(path) && player.play(), "error recovery failed"); wait_end(player);
        GstElement* automatic = gst_element_factory_make("autoaudiosink", nullptr);
        require(automatic, "autoaudiosink unavailable");
        GError* warning = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND, "test unavailable output");
        gst_bus_post(player.bus, gst_message_new_warning(GST_OBJECT(automatic), warning, "test"));
        g_error_free(warning);
        player.poll();
        require(!player.error.empty() && !player.playing, "automatic silent fallback not rejected");
        gst_object_unref(automatic);
        set_fake_output(player);
        require(player.error.empty(), "output recovery kept stale error");
        for (int i = 1; i < argc; ++i) {
            require(player.load(argv[i]) && player.play(), "format load failed");
            wait_end(player);
            std::printf("format PASS: %s\n", argv[i]);
        }
        std::puts("playback lifecycle PASS (fakesink, no physical audio)");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); exit_code = 1;
    }
    unlink(path); g_free(path);
    return exit_code;
}
