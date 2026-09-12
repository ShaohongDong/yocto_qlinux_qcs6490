// SPDX-License-Identifier: MIT
#include "native.hpp"
#include <glib.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace imx708 {
namespace {
struct Fd {
    int value;
    explicit Fd(const std::string& path) : value(open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC)) {
        if (value < 0) throw std::runtime_error(path + ": " + strerror(errno));
    }
    ~Fd() { close(value); }
};
void checked(int fd, unsigned long request, void* arg) {
    int result;
    do { result = ioctl(fd, request, arg); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::runtime_error("V4L2 ioctl " + std::to_string(request) + ": " + strerror(errno));
}
std::string command(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    gchar *out = nullptr, *err = nullptr;
    GError* error = nullptr;
    gint status = 0;
    const bool launched = g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH,
                                      nullptr, nullptr, &out, &err, &status, &error);
    std::string result = out ? out : "", reason = err ? err : "";
    if (error) { reason += error->message; g_error_free(error); }
    g_free(out); g_free(err);
    if (!launched || status != 0) {
        std::string invocation;
        for (const auto& arg : args) invocation += arg + " ";
        throw std::runtime_error(invocation + "(status=" + std::to_string(status) + "): " + reason);
    }
    while (!result.empty() && g_ascii_isspace(result.back())) result.pop_back();
    return result;
}
int control(int fd, unsigned id, int value) {
    v4l2_control c {}; c.id = id; c.value = value;
    checked(fd, VIDIOC_S_CTRL, &c); checked(fd, VIDIOC_G_CTRL, &c);
    return c.value;
}
ControlRange range(int fd, unsigned id) {
    v4l2_queryctrl q {}; q.id = id; checked(fd, VIDIOC_QUERYCTRL, &q);
    v4l2_control c {}; c.id = id; checked(fd, VIDIOC_G_CTRL, &c);
    return {q.minimum, q.maximum, q.step, c.value};
}
std::string dt_string(const std::filesystem::path& path) {
    std::ifstream stream(path); std::string s((std::istreambuf_iterator<char>(stream)), {});
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
struct Stream {
    int fd;
    bool active = false;
    struct Buffer { void* data; size_t length; };
    std::vector<Buffer> buffers;
    explicit Stream(int descriptor) : fd(descriptor) {}
    ~Stream() {
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (active) ioctl(fd, VIDIOC_STREAMOFF, &type);
        for (auto& b : buffers) munmap(b.data, b.length);
        v4l2_requestbuffers req {}; req.type = type; req.memory = V4L2_MEMORY_MMAP;
        ioctl(fd, VIDIOC_REQBUFS, &req);
    }
};
}
NativeCapture::~NativeCapture() { stop(); }
void NativeCapture::prepare() {
    stop();
    const std::filesystem::path dt("/sys/firmware/devicetree/base");
    if (dt_string(dt / "i2c-cam3-gpio/imx708@1a/status") != "okay")
        throw std::runtime_error("请先启动原生摄像头诊断配置");
    for (const auto* name : {"cci0", "cci1"}) {
        const auto path = dt_string(dt / "__symbols__" / name);
        if (path.empty() || dt_string(dt / path.substr(1) / "status") != "disabled")
            throw std::runtime_error("原生模式要求 CCI0/1 保持禁用");
    }
    media_path.clear();
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const auto name = entry.path().filename().string();
        if (name.rfind("media", 0) != 0) continue;
        const auto topology = command({"media-ctl", "-d", entry.path(), "-p"});
        if (topology.find("imx708") != std::string::npos && topology.find("msm_csiphy3") != std::string::npos) {
            if (!media_path.empty()) throw std::runtime_error("Multiple IMX708 graphs");
            media_path = entry.path();
        }
    }
    if (media_path.empty()) throw std::runtime_error("未找到 IMX708 CAMSS 媒体链路");
    sensor_path = command({"media-ctl", "-d", media_path, "-e", "imx708"});
    video_path = command({"media-ctl", "-d", media_path, "-e", "msm_vfe0_video0"});
    command({"media-ctl", "-d", media_path, "-l", "\"msm_csiphy3\":1 -> \"msm_csid0\":0 [1]"});
    command({"media-ctl", "-d", media_path, "-l", "\"msm_csid0\":1 -> \"msm_vfe0_rdi0\":0 [1]"});
    Fd sensor(sensor_path);
    sensor_power_status = (std::filesystem::path("/sys/class/video4linux") /
        std::filesystem::path(sensor_path).filename() / "device/power/runtime_status").string();
    control(sensor.value, V4L2_CID_WIDE_DYNAMIC_RANGE, 0);
    control(sensor.value, V4L2_CID_HFLIP, 0); control(sensor.value, V4L2_CID_VFLIP, 0);
    for (const auto* entity : {"\"imx708\":0", "\"msm_csiphy3\":0", "\"msm_csid0\":0",
                              "\"msm_csid0\":1", "\"msm_vfe0_rdi0\":0"})
        command({"media-ctl", "-d", media_path, "-V", std::string(entity) + " [fmt:SRGGB10_1X10/2304x1296 field:none]"});
    control(sensor.value, V4L2_CID_TEST_PATTERN, test_pattern);
    const auto hblank = range(sensor.value, V4L2_CID_HBLANK).value;
    v4l2_ext_control pixel {}; pixel.id = V4L2_CID_PIXEL_RATE;
    v4l2_ext_controls ext {}; ext.count = 1; ext.controls = &pixel;
    checked(sensor.value, VIDIOC_G_EXT_CTRLS, &ext);
    const int vblank = control(sensor.value, V4L2_CID_VBLANK,
                              int(pixel.value64 / (30 * (2304 + hblank))) - 1296);
    fps = double(pixel.value64) / ((2304 + hblank) * (1296 + vblank));
    exposure = range(sensor.value, V4L2_CID_EXPOSURE);
    gain = range(sensor.value, V4L2_CID_ANALOGUE_GAIN);
    Fd video(video_path);
    v4l2_format fmt {}; fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = 2304; fmt.fmt.pix_mp.height = 1296;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SRGGB10P; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    checked(video.value, VIDIOC_S_FMT, &fmt);
    const auto& p = fmt.fmt.pix_mp;
    if (p.width != 2304 || p.height != 1296 || p.pixelformat != V4L2_PIX_FMT_SRGGB10P || p.num_planes != 1)
        throw std::runtime_error("Unexpected negotiated RAW format");
    stride = p.plane_fmt[0].bytesperline; sizeimage = p.plane_fmt[0].sizeimage;
    if (stride < 2880 || sizeimage != stride * 1296) throw std::runtime_error("Unexpected RAW stride/size");
    requested.exposure = exposure.value; requested.gain = gain.value;
    std::cerr << "native format=2304x1296 pRAA stride=" << stride << " size=" << sizeimage
              << " sensor_fps=" << fps << " video=" << video_path << '\n';
}
void NativeCapture::settings(const RawSettings& value) {
    if (value.exposure < exposure.minimum || value.exposure > exposure.maximum ||
        value.gain < gain.minimum || value.gain > gain.maximum)
        throw std::runtime_error("Exposure/gain outside sensor range");
    std::lock_guard<std::mutex> lock(mutex); requested = value;
}
void NativeCapture::start(std::function<void(NativeFrame&&)> frame, std::function<void(std::string)> error) {
    stop(); frames = 0; bad_frames = 0; gaps = 0; stopping = false;
    worker = std::thread([this, frame, error] {
        try { loop(frame); } catch (const std::exception& e) { if (!stopping) error(e.what()); }
    });
}
void NativeCapture::stop() { stopping = true; if (worker.joinable()) worker.join(); }
void NativeCapture::loop(const std::function<void(NativeFrame&&)>& deliver) {
    // On this module a warm colour-bar -> image transition can leave the
    // receiver without frames. Let the driver's normal 5 s autosuspend perform
    // the power cycle before changing streaming mode; never drive GPIO here.
    // Always gate startup: cached controls may already have changed in a
    // cancelled start or another process. Wait on the worker, so Stop and GTK
    // remain responsive. Already-suspended devices proceed immediately.
    {
        std::cerr << "native waiting for sensor suspend before stream start\n";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        while (!stopping) {
            std::ifstream file(sensor_power_status); std::string state; file >> state;
            if (state == "suspended") break;
            if (!file || std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("切换色条模式前传感器未进入休眠，请停止其他取流进程后重试");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stopping) return;
    }
    Fd sensor(sensor_path), video(video_path);
    Stream stream(video.value);
    v4l2_requestbuffers req {}; req.count = 4; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
    checked(video.value, VIDIOC_REQBUFS, &req);
    if (req.count < 2) throw std::runtime_error("Not enough capture buffers");
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_plane plane {}; v4l2_buffer b {}; b.type = req.type; b.memory = req.memory;
        b.index = i; b.length = 1; b.m.planes = &plane;
        checked(video.value, VIDIOC_QUERYBUF, &b);
        void* data = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, video.value, plane.m.mem_offset);
        if (data == MAP_FAILED) throw std::runtime_error("RAW mmap failed");
        stream.buffers.push_back({data, plane.length});
        if (plane.length < sizeimage) throw std::runtime_error("Short mapped buffer");
        checked(video.value, VIDIOC_QBUF, &b);
    }
    auto type = req.type; checked(video.value, VIDIOC_STREAMON, &type); stream.active = true;
    RawSettings applied; applied.exposure = applied.gain = -1;
    auto last_valid = std::chrono::steady_clock::now();
    uint64_t previous_ts = 0; unsigned previous_seq = 0, settling = 0;
    while (!stopping) {
        RawSettings next;
        { std::lock_guard<std::mutex> lock(mutex); next = requested; }
        if (next.exposure != applied.exposure || next.gain != applied.gain) {
            next.exposure = control(sensor.value, V4L2_CID_EXPOSURE, next.exposure);
            next.gain = control(sensor.value, V4L2_CID_ANALOGUE_GAIN, next.gain);
            settling = 3;
            std::cerr << "native applied exposure=" << next.exposure << " gain=" << next.gain << '\n';
        }
        applied = next;
        pollfd pfd {video.value, POLLIN, 0};
        const int result = poll(&pfd, 1, 100);
        if (stopping) break;
        if (result < 0 && errno != EINTR) throw std::runtime_error("Capture poll failed");
        if (result > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) throw std::runtime_error("Capture device disconnected");
        if (std::chrono::steady_clock::now() - last_valid >= std::chrono::seconds(3))
            throw std::runtime_error("3 秒内未收到有效摄像头帧");
        if (result <= 0) continue;
        v4l2_plane plane {}; v4l2_buffer b {}; b.type = type; b.memory = V4L2_MEMORY_MMAP; b.length = 1; b.m.planes = &plane;
        if (ioctl(video.value, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            throw std::runtime_error("Capture dequeue failed");
        }
        if (b.index >= stream.buffers.size()) throw std::runtime_error("Invalid buffer index");
        const uint64_t ts = uint64_t(b.timestamp.tv_sec) * 1000000000 + uint64_t(b.timestamp.tv_usec) * 1000;
        const bool valid = !plane.data_offset && valid_raw_buffer(plane.bytesused, sizeimage,
            b.flags & V4L2_BUF_FLAG_ERROR, (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, ts, previous_ts);
        NativeFrame frame;
        if (valid) {
            if (previous_ts && b.sequence != previous_seq + 1) ++gaps;
            previous_seq = b.sequence; previous_ts = ts;
            last_valid = std::chrono::steady_clock::now(); ++frames;
            if (!settling) {
                frame.stride = stride; frame.sequence = b.sequence; frame.timestamp_ns = ts; frame.settings = applied;
                const auto* data = static_cast<const uint8_t*>(stream.buffers[b.index].data);
                frame.raw.assign(data, data + sizeimage);
            } else --settling;
        } else { ++bad_frames; std::cerr << "native rejected seq=" << b.sequence << " bytes=" << plane.bytesused << " flags=" << b.flags << '\n'; }
        checked(video.value, VIDIOC_QBUF, &b);
        if (!frame.raw.empty()) {
            frame.rgb = raw_to_rgb(frame.raw.data(), frame.raw.size(), frame.width, frame.height, stride, applied);
            deliver(std::move(frame));
        }
    }
}
}
