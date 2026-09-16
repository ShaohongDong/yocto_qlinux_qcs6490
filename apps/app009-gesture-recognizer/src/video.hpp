// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <stdexcept>
#include <vector>
struct Frame {
  int width = 0, height = 0;
  std::vector<uint8_t> rgb;
  uint64_t source_index = 0;
  double pts_seconds = 0;
};
class Video {
  GstElement *pipeline = nullptr;
  GstAppSink *sink = nullptr;
  GstBus *bus = nullptr;
  std::atomic<uint64_t> decoded{0};
  static GQuark index_key() {
    return g_quark_from_static_string("gesture-recognizer-source-index");
  }

public:
  ~Video() {
    if (pipeline) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
    }
    if (sink)
      gst_object_unref(sink);
    if (bus)
      gst_object_unref(bus);
  }
  uint64_t decoded_count() const { return decoded.load(); }
  explicit Video(const std::string &path, bool realtime) {
    GError *error = nullptr;
    gchar *uri = g_filename_to_uri(std::filesystem::absolute(path).c_str(),
                                   nullptr, nullptr);
    auto *discoverer = gst_discoverer_new(5 * GST_SECOND, &error);
    if (!discoverer) {
      std::string message =
          error ? error->message : "Cannot create media discoverer";
      g_clear_error(&error);
      g_free(uri);
      throw std::runtime_error(message);
    }
    auto *info = gst_discoverer_discover_uri(discoverer, uri, &error);
    std::string codec;
    if (info) {
      auto *videos = gst_discoverer_info_get_video_streams(info);
      if (videos) {
        auto *caps = gst_discoverer_stream_info_get_caps(
            GST_DISCOVERER_STREAM_INFO(videos->data));
        if (caps) {
          codec = gst_structure_get_name(gst_caps_get_structure(caps, 0));
          gst_caps_unref(caps);
        }
      }
      gst_discoverer_stream_info_list_free(videos);
      g_object_unref(info);
    }
    g_clear_error(&error);
    g_object_unref(discoverer);
    if (codec != "video/x-h264" && codec != "video/x-h265") {
      g_free(uri);
      throw std::runtime_error("Video requires an H.264 or H.265 stream");
    }
    const std::string suffix = codec == "video/x-h264" ? "h264" : "h265";
    // Require linear system-memory frames; Qualcomm UBWC DMABufs cannot be
    // interpreted by the CPU RGB preprocessor. Link the decoder explicitly
    // before negotiation so decodebin cannot select UBWC during autoplugging.
    std::string description =
        "uridecodebin name=source caps=" + codec + " ! " + suffix +
        "parse ! v4l2" + suffix +
        "dec capture-io-mode=mmap ! video/x-raw,format=NV12 ! videoconvert ! "
        "video/x-raw,format=RGB ! appsink name=frames max-buffers=2 " +
        std::string(realtime ? "sync=true drop=true" : "sync=false drop=false");
    pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) {
      std::string message = error->message;
      g_error_free(error);
      g_free(uri);
      if (pipeline)
        gst_object_unref(pipeline);
      pipeline = nullptr;
      throw std::runtime_error(message);
    }
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    g_object_set(source, "uri", uri, nullptr);
    g_free(uri);
    gst_object_unref(source);
    sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "frames"));
    bus = gst_element_get_bus(pipeline);
    auto *pad = gst_element_get_static_pad(GST_ELEMENT(sink), "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad *, GstPadProbeInfo *info,
            gpointer user) -> GstPadProbeReturn {
          auto *self = static_cast<Video *>(user);
          auto *b = GST_PAD_PROBE_INFO_BUFFER(info);
          gst_mini_object_set_qdata(GST_MINI_OBJECT(b), index_key(),
                                    reinterpret_cast<gpointer>(++self->decoded),
                                    nullptr);
          return GST_PAD_PROBE_OK;
        },
        this, nullptr);
    gst_object_unref(pad);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      gst_object_unref(sink);
      gst_object_unref(bus);
      pipeline = nullptr;
      sink = nullptr;
      bus = nullptr;
      throw std::runtime_error("Cannot start video decoder");
    }
  }
  enum class Read { Frame, Wait, End };
  Read next(Frame &frame) {
    auto *sample = gst_app_sink_try_pull_sample(sink, 100 * GST_MSECOND);
    if (!sample) {
      auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      if (message) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::string text = error->message;
        g_error_free(error);
        g_free(debug);
        gst_message_unref(message);
        throw std::runtime_error(text);
      }
      return gst_app_sink_is_eos(sink) ? Read::End : Read::Wait;
    }
    GstVideoInfo info;
    gst_video_info_init(&info);
    GstVideoFrame mapped;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        !gst_video_frame_map(&mapped, &info, gst_sample_get_buffer(sample),
                             GST_MAP_READ)) {
      gst_sample_unref(sample);
      throw std::runtime_error("Cannot map decoded video frame");
    }
    auto *buffer = gst_sample_get_buffer(sample);
    frame.source_index = reinterpret_cast<uintptr_t>(gst_mini_object_get_qdata(
                             GST_MINI_OBJECT(buffer), index_key())) -
                         1;
    if (!GST_BUFFER_PTS_IS_VALID(buffer)) {
      gst_video_frame_unmap(&mapped);
      gst_sample_unref(sample);
      throw std::runtime_error("Video frame has no timestamp");
    }
    // Demuxers may shift buffer PTS to accommodate negative initial DTS.
    // Annotation/event times use the stream timeline, not that segment offset.
    const auto *segment = gst_sample_get_segment(sample);
    auto timestamp = segment && segment->format == GST_FORMAT_TIME
                         ? gst_segment_to_stream_time(segment, GST_FORMAT_TIME,
                                                      GST_BUFFER_PTS(buffer))
                         : GST_CLOCK_TIME_NONE;
    if (!GST_CLOCK_TIME_IS_VALID(timestamp)) {
      gst_video_frame_unmap(&mapped);
      gst_sample_unref(sample);
      throw std::runtime_error("Video frame has no valid stream timestamp");
    }
    frame.pts_seconds = double(timestamp) / GST_SECOND;
    frame.width = GST_VIDEO_INFO_WIDTH(&info);
    frame.height = GST_VIDEO_INFO_HEIGHT(&info);
    frame.rgb.resize(size_t(frame.width) * frame.height * 3);
    const auto *data =
        static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0));
    for (int y = 0; y < frame.height; ++y)
      std::copy_n(data + y * GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0),
                  frame.width * 3,
                  frame.rgb.data() + size_t(y) * frame.width * 3);
    gst_video_frame_unmap(&mapped);
    gst_sample_unref(sample);
    return Read::Frame;
  }
};
