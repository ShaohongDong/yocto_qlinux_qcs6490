// SPDX-License-Identifier: MIT
// Includes the implementation to exercise frame accounting without codec devices.
#include "../src/engine.cpp"
#include <iostream>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "failed line " << __LINE__ << ": " #condition "\n"; return 1; } } while (0)
int main() {
    gst_init(nullptr, nullptr);
    std::atomic<bool> cancel{false};
    hevc::Options options;
    options.warmup = 0; options.seconds = 1;
    CHECK(hevc::requested_controls(options, true).size() == 1);
    options.latency_profile = "low"; options.operating_rate = 60;
    CHECK(hevc::requested_controls(options, true).size() == 4);
    auto dc = hevc::requested_controls(options, false);
    CHECK(dc.size() == 2 && dc.back().requested == (60 << 16));
    bool rejected = false;
    try { hevc::check_controls(-1, dc); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    int null_fd = open("/dev/null", O_RDONLY);
    rejected = false;
    try { hevc::check_controls(null_fd, dc); } catch (const std::runtime_error&) { rejected = true; }
    close(null_fd); CHECK(rejected);
    options.latency_profile = "baseline"; options.operating_rate = 0;
    hevc::Session s(options, cancel, nullptr);
    s.start = 1000; s.finished = 2010; s.eos = true;
    for (int i = 0; i < 30; ++i) {
        auto& f = s.frames.rows[i];
        f.source = 1001 + i * 33;
        f.source_before_copy = f.source - 2; f.encoded_bytes = 1000;
        f.encode_in = f.source; f.encode_out = f.source + 3;
        f.decode_in = f.source + 4; f.decode_out = f.source + 5;
        f.quality = true; f.y = {4, 4};
    }
    bool passed = false;
    auto report = s.report(passed);
    CHECK(passed);
    CHECK(report.find("\"source_to_decode_ms\":{\"count\":30,\"mean\":5") != std::string::npos);
    CHECK(report.find("\"before_copy_to_decode_ms\":{\"count\":30,\"mean\":7") != std::string::npos);
    CHECK(report.find("\"actual_encoded_bitrate\":240000") != std::string::npos);
    CHECK(report.find("\"encode_fps\":30") != std::string::npos);
    CHECK(report.find("\"decode_latency_ms\":{\"count\":30,\"mean\":1") != std::string::npos);
    s.frames.rows[0].decode_out = -1;
    report = s.report(passed);
    CHECK(!passed);
    CHECK(report.find("\"missing_decoded_frames\":1") != std::string::npos);
    s.frames.rows[0].decode_out = 1006;
    s.error.clear(); s.frames.unmatched = 1;
    s.report(passed); CHECK(!passed);
    s.frames.unmatched = 0; s.error.clear(); s.eos = false;
    s.report(passed); CHECK(!passed);
    s.eos = true; s.error.clear(); cancel = true;
    report = s.report(passed); CHECK(!passed);
    CHECK(report.find("CANCELLED") != std::string::npos);
    cancel = false; s.error.clear();
    s.frames.rows[0].decode_out = 3000;
    s.report(passed); CHECK(!passed); // Drain must not inflate the one-second FPS window.
    s.frames.rows[0].decode_out = 1006; s.error.clear(); s.finished = 1500;
    s.report(passed); CHECK(!passed); // Early EOS is not a completed measurement window.
    s.finished = 2010; s.error.clear(); s.frames.rows[0].encode_out = 999;
    s.report(passed); CHECK(!passed); // Invalid latency cannot silently disappear from statistics.
    // Exercise actual GStreamer VideoMeta strides plus a visible crop.
    auto* caps = gst_caps_from_string("video/x-raw,format=NV12,width=1928,height=1088,framerate=30/1");
    constexpr int stride = 1952, rows = 1088;
    auto* buffer = gst_buffer_new_allocate(nullptr, stride * rows * 3 / 2, nullptr);
    gst_buffer_memset(buffer, 0, 42, gst_buffer_get_size(buffer));
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, stride * rows, 0, 0};
    gint strides[GST_VIDEO_MAX_PLANES] = {stride, stride, 0, 0};
    gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, 1928, rows, 2, offsets, strides);
    auto* crop = gst_buffer_add_video_crop_meta(buffer);
    crop->x = 4; crop->y = 4; crop->width = 1920; crop->height = 1080;
    auto y = hevc::Session::luma(buffer, caps);
    CHECK(y.size() == 1920 * 1080 && y.front() == 42 && y.back() == 42);
    crop->x = 100;
    rejected = false;
    try { hevc::Session::luma(buffer, caps); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    gst_buffer_unref(buffer); gst_caps_unref(caps);
    // Exercise actual serialized source cutoff: every counted frame must reach
    // the sink before EOS, including the final buffer near the time boundary.
    hevc::Options cutoff_options;
    cutoff_options.mode = "encode"; cutoff_options.seconds = 1; cutoff_options.warmup = 0;
    hevc::Session cutoff(cutoff_options, cancel, nullptr);
    cutoff.pipeline = gst_parse_launch("videotestsrc ! video/x-raw,width=320,height=240 ! identity name=reference ! fakesink name=output sync=false", nullptr);
    CHECK(cutoff.pipeline);
    auto* reference_element = gst_bin_get_by_name(GST_BIN(cutoff.pipeline), "reference");
    auto* reference_pad = gst_element_get_static_pad(reference_element, "src");
    gst_pad_add_probe(reference_pad, GST_PAD_PROBE_TYPE_BUFFER, hevc::Session::input_probe, &cutoff, nullptr);
    gst_object_unref(reference_pad); gst_object_unref(reference_element);
    size_t delivered = 0;
    auto* output_element = gst_bin_get_by_name(GST_BIN(cutoff.pipeline), "output");
    auto* output_pad = gst_element_get_static_pad(output_element, "sink");
    gst_pad_add_probe(output_pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*, GstPadProbeInfo*, gpointer data) {
            ++*static_cast<size_t*>(data); return GST_PAD_PROBE_OK;
        }, &delivered, nullptr);
    gst_object_unref(output_pad); gst_object_unref(output_element);
    cutoff.start = hevc::monotonic_ms();
    CHECK(gst_element_set_state(cutoff.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    auto* cutoff_bus = gst_element_get_bus(cutoff.pipeline);
    auto* message = gst_bus_timed_pop_filtered(cutoff_bus, 3 * GST_SECOND,
        GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    CHECK(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
    gst_message_unref(message); gst_object_unref(cutoff_bus);
    gst_element_set_state(cutoff.pipeline, GST_STATE_NULL);
    CHECK(cutoff.source_eos_sent && cutoff.error.empty());
    CHECK(delivered > 0 && delivered == cutoff.frames.rows.size());
    // Real appsrc need-data pacing must consume wall time, not only stamp PTS.
    hevc::Options paced_options;
    paced_options.mode = "decode"; paced_options.pacing = "realtime";
    paced_options.warmup = 0; paced_options.seconds = 1;
    hevc::Session paced(paced_options, cancel, nullptr);
    CHECK(paced.realtime());
    paced.corpus.push_back({{0}, false});
    paced.pipeline = gst_parse_launch("appsrc name=input format=time is-live=true ! fakesink sync=false", nullptr);
    auto* paced_source = gst_bin_get_by_name(GST_BIN(paced.pipeline), "input");
    g_signal_connect(paced_source, "need-data", G_CALLBACK(hevc::Session::need_data), &paced);
    gst_object_unref(paced_source);
    paced.start = hevc::monotonic_ms();
    gst_element_set_state(paced.pipeline, GST_STATE_PLAYING);
    auto* paced_bus = gst_element_get_bus(paced.pipeline);
    message = gst_bus_timed_pop_filtered(paced_bus, 3 * GST_SECOND,
        GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    CHECK(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
    CHECK(hevc::monotonic_ms() - paced.start >= 990);
    gst_message_unref(message); gst_object_unref(paced_bus);
    gst_element_set_state(paced.pipeline, GST_STATE_NULL);
    CHECK(paced.fed == 30);
    hevc::Options diagnostic;
    diagnostic.quality_mode = "off";
    hevc::Session off(diagnostic, cancel, nullptr);
    CHECK(off.has_decoder() && off.loopback() && !off.quality());
    hevc::RawLayout full_hd(1920, 1080), odd_stride(130, 722);
    CHECK(full_hd.stride == 1920 && full_hd.y_rows == 1088 && full_hd.uv_rows == 544);
    CHECK(full_hd.bytes >= 1920 * 1632 && full_hd.bytes % 4096 == 0);
    CHECK(odd_stride.stride == 256 && odd_stride.y_rows == 736 && odd_stride.uv_rows == 368);
    // Non-default size and rational frame timing, finite-run acceptance.
    hevc::Options dynamic;
    dynamic.width = 640; dynamic.height = 480;
    dynamic.fps_num = 60000; dynamic.fps_den = 1001;
    dynamic.mode = "encode"; dynamic.frame_limit = 1; dynamic.warmup = 0;
    hevc::Session finite(dynamic, cancel, nullptr);
    finite.start = 1000; finite.finished = 1010; finite.eos = true;
    auto& one = finite.frames.rows[0];
    one.source = one.encode_in = 1001; one.encode_out = 1003;
    auto finite_report = finite.report(passed);
    CHECK(passed && finite.raw_frame_bytes == 460800);
    CHECK(finite_report.find("\"fps_den\":1001") != std::string::npos);
    finite.options.frame_limit = 2; finite.report(passed); CHECK(!passed);
    auto root = std::filesystem::temp_directory_path() / ("hevc-engine-test-" + std::to_string(getpid()));
    std::filesystem::create_directory(root);
    auto raw_path = root / "raw";
    { std::ofstream raw_file(raw_path, std::ios::binary); raw_file << "abcdefgh"; }
    {
        hevc::RawInput raw;
        raw.open(raw_path.string(), 4);
        CHECK(raw.frames == 2);
        CHECK(raw.next() == std::vector<uint8_t>({'a','b','c','d'}));
        CHECK(raw.next() == std::vector<uint8_t>({'e','f','g','h'}));
        CHECK(raw.next() == std::vector<uint8_t>({'a','b','c','d'}));
        raw.stop(); CHECK(raw.high_water <= 4);
    }
    rejected = false;
    try { hevc::RawInput raw; raw.open(raw_path.string(), 3); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    CHECK(hevc::Session::hash_file(raw_path.string()) == "9c56cc51b374c3ba189210d5b6d4bf57790d351c96c47c02190ecf1e430635ab");
    auto compressed_path = root / "raw.zst";
    std::vector<uint8_t> zipped(ZSTD_compressBound(8));
    size_t zipped_size = ZSTD_compress(zipped.data(), zipped.size(), "abcdefgh", 8, 1);
    CHECK(!ZSTD_isError(zipped_size));
    { std::ofstream out(compressed_path, std::ios::binary); out.write(reinterpret_cast<char*>(zipped.data()), zipped_size); }
    CHECK(hevc::Session::hash_raw(compressed_path) == hevc::Session::hash_file(raw_path));
    {
        hevc::RawInput raw; raw.open(compressed_path, 4);
        CHECK(raw.frames == 2);
        CHECK(raw.next() == std::vector<uint8_t>({'a','b','c','d'}));
        CHECK(raw.next() == std::vector<uint8_t>({'e','f','g','h'}));
        CHECK(raw.next() == std::vector<uint8_t>({'a','b','c','d'}));
    }
    std::filesystem::resize_file(compressed_path, zipped_size - 1);
    rejected = false;
    try { hevc::Session::hash_raw(compressed_path); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    // Decode order differs from presentation order; PTS mapping must survive.
    { std::ofstream index(raw_path.string() + ".index"); index << "0 2 0\n2 2 3\n4 2 1\n6 2 2\n"; }
    dynamic.mode = "decode"; dynamic.input_hevc = raw_path;
    hevc::Session indexed(dynamic, cancel, nullptr); indexed.load_hevc();
    CHECK(indexed.corpus.size() == 4 && indexed.corpus[1].presentation == 3);
    { std::ofstream index(raw_path.string() + ".index"); index << "0 2 0\n2 6 0\n"; }
    rejected = false;
    try { hevc::Session invalid(dynamic, cancel, nullptr); invalid.load_hevc(); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    { std::ofstream index(raw_path.string() + ".index"); index << "0 8 0\n99"; }
    rejected = false;
    try { hevc::Session invalid(dynamic, cancel, nullptr); invalid.load_hevc(); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    std::filesystem::remove_all(root);
    std::cout << "engine: acceptance thresholds, missing frames, cancellation, EOS, drain window, NV12 stride/crop PASS\n";
}
