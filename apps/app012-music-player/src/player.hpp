// SPDX-License-Identifier: MIT
#pragma once
#include <gst/gst.h>
#include <string>

// All methods run on the GTK main thread. The bus is consumed by that thread.
class Player {
public:
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    bool playing = false;
    bool repeat = false;
    bool ended = false;
    std::string uri;
    std::string error;

    Player() {
        pipeline = gst_element_factory_make("playbin", nullptr);
        if (!pipeline) { error = "缺少 GStreamer 播放组件"; return; }
        bus = gst_element_get_bus(pipeline);
        // AUDIO | SOFT_VOLUME: audio only, with volume for sinks such as ALSA.
        g_object_set(pipeline, "flags", 2u | 16u, "volume", 0.5, nullptr);
    }
    ~Player() {
        if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) gst_object_unref(bus);
        if (pipeline) gst_object_unref(pipeline);
    }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    void stop() {
        playing = false;
        ended = false;
        if (!pipeline) return;
        gst_element_set_state(pipeline, GST_STATE_NULL);
        // Discard messages belonging to the previous file/output/session.
        gst_bus_set_flushing(bus, TRUE);
        gst_bus_set_flushing(bus, FALSE);
    }
    bool load(const char* path) {
        stop();
        uri.clear();
        error.clear();
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            error = "文件不存在或不是普通文件";
            return false;
        }
        GError* e = nullptr;
        gchar* absolute = g_canonicalize_filename(path, nullptr);
        gchar* value = gst_filename_to_uri(absolute, &e);
        g_free(absolute);
        if (!value) {
            error = e ? e->message : "无法打开文件路径";
            g_clear_error(&e);
            return false;
        }
        uri = value;
        g_free(value);
        if (pipeline) g_object_set(pipeline, "uri", uri.c_str(), nullptr);
        return pipeline != nullptr;
    }
    bool play() {
        if (!pipeline || uri.empty()) return false;
        if (ended) stop();
        error.clear();
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            error = "无法开始播放，请检查文件及音频输出";
            stop();
            return false;
        }
        playing = true;
        return true;
    }
    void pause() {
        if (!pipeline) return;
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
            error = "暂停失败";
            stop();
        } else playing = false;
    }
    bool seek(gint64 position) {
        return pipeline && gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), position);
    }
    void output(GstElement* sink) {
        stop();
        error.clear();
        if (pipeline) g_object_set(pipeline, "audio-sink", sink, nullptr);
    }
    // Returns true when a terminal playback event is received.
    bool poll() {
        if (!bus) return false;
        bool terminal = false;
        while (GstMessage* message = gst_bus_pop(bus)) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* e = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &e, &debug);
                error = e ? e->message : "播放失败";
                g_clear_error(&e);
                g_free(debug);
                stop();
                terminal = true;
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
                auto* source = GST_MESSAGE_SRC(message);
                auto* factory = GST_IS_ELEMENT(source) ? gst_element_get_factory(GST_ELEMENT(source)) : nullptr;
                // autoaudiosink silently substitutes fakesink after a warning
                // when all real outputs fail. Treat that as a playback failure.
                if (factory && g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "autoaudiosink") == 0) {
                    GError* e = nullptr;
                    gchar* debug = nullptr;
                    gst_message_parse_warning(message, &e, &debug);
                    error = "没有可用的音频输出：" + std::string(e ? e->message : "请检查系统音频设备");
                    g_clear_error(&e);
                    g_free(debug);
                    stop();
                    terminal = true;
                }
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                if (repeat) { stop(); play(); }
                else { stop(); ended = true; }
                terminal = true;
            }
            gst_message_unref(message);
        }
        return terminal;
    }
};
