// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <zstd.h>
namespace hevc {
// Qualcomm linear NV12/NV21 DMA layout, passed explicitly in GstVideoMeta.
// MMAP copying using visible height shifts chroma when height is not Y-aligned.
struct RawLayout {
    size_t stride, y_rows, uv_rows, bytes;
    RawLayout(int width, int height) {
        if (width < 2 || height < 2 || width > 8192 || height > 8192 || width % 2 || height % 2)
            throw std::invalid_argument("Invalid linear 4:2:0 dimensions");
        stride = (width + 127) & ~size_t(127);
        y_rows = (height + 31) & ~size_t(31);
        uv_rows = (height / 2 + 15) & ~size_t(15);
        bytes = (stride * (y_rows + uv_rows) + 4095) & ~size_t(4095);
    }
};
// One Zstandard frame with an advertised content size, or a plain raw file.
// Never materializes the decompressed file; validates checksum and exact EOF.
class RawStream {
    std::ifstream stream;
    bool compressed, ended = false;
    ZSTD_DStream* decoder = nullptr;
    std::vector<char> input;
    ZSTD_inBuffer buffered{nullptr, 0, 0};
public:
    uint64_t size;
    explicit RawStream(const std::string& path) : stream(path, std::ios::binary),
        compressed(std::filesystem::path(path).extension() == ".zst"), input(128 * 1024) {
        if (!stream) throw std::runtime_error("Cannot open raw input");
        size = std::filesystem::file_size(path);
        if (compressed) {
            char header[18]; // Maximum Zstandard frame header size.
            stream.read(header, sizeof(header));
            auto declared = ZSTD_getFrameContentSize(header, stream.gcount());
            if (declared == ZSTD_CONTENTSIZE_ERROR || declared == ZSTD_CONTENTSIZE_UNKNOWN)
                throw std::runtime_error("Zstandard input requires one frame with known content size");
            size = declared;
            decoder = ZSTD_createDStream();
            if (!decoder) throw std::runtime_error("Cannot allocate Zstandard decoder");
        }
        rewind();
    }
    ~RawStream() { if (decoder) ZSTD_freeDStream(decoder); }
    RawStream(const RawStream&) = delete;
    RawStream& operator=(const RawStream&) = delete;
    void rewind() {
        stream.clear(); stream.seekg(0); ended = false;
        buffered = {input.data(), 0, 0};
        if (decoder && ZSTD_isError(ZSTD_initDStream(decoder)))
            throw std::runtime_error("Cannot reset Zstandard input");
    }
    size_t read(uint8_t* destination, size_t bytes) {
        if (!compressed) {
            stream.read(reinterpret_cast<char*>(destination), bytes);
            if (stream.bad() || (stream.fail() && !stream.eof())) throw std::runtime_error("Raw input read failed");
            return stream.gcount();
        }
        ZSTD_outBuffer output{destination, bytes, 0};
        while (output.pos < output.size && !ended) {
            if (buffered.pos == buffered.size) {
                stream.read(input.data(), input.size());
                buffered = {input.data(), size_t(stream.gcount()), 0};
                if (!buffered.size) throw std::runtime_error("Truncated Zstandard input");
            }
            auto result = ZSTD_decompressStream(decoder, &output, &buffered);
            if (ZSTD_isError(result)) throw std::runtime_error(ZSTD_getErrorName(result));
            if (!result) {
                ended = true;
                if (buffered.pos != buffered.size || stream.peek() != std::char_traits<char>::eof())
                    throw std::runtime_error("Trailing/concatenated Zstandard input is not supported");
            }
        }
        return output.pos;
    }
};
// Four-frame bounded prefetch. Input is immutable for the duration of a run.
class RawInput {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<uint8_t>> queue;
    std::thread worker;
    bool stopping = false;
    std::string error;
public:
    size_t frames = 0, high_water = 0;
    ~RawInput() { stop(); }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        ready.notify_all();
        if (worker.joinable()) worker.join();
    }
    void open(const std::string& path, size_t bytes) {
        const auto size = RawStream(path).size;
        if (!bytes || !size || size % bytes) throw std::runtime_error("Raw input is empty or has a truncated frame");
        frames = size / bytes;
        worker = std::thread([this, path, bytes] {
            try {
                RawStream stream(path);
                size_t index = 0;
                while (true) {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [this] { return stopping || queue.size() < 4; });
                    if (stopping) break;
                    lock.unlock();
                    std::vector<uint8_t> frame(bytes);
                    if (index == frames) {
                        uint8_t extra;
                        if (stream.read(&extra, 1)) throw std::runtime_error("Raw input exceeds declared size");
                        stream.rewind(); index = 0;
                    }
                    if (stream.read(frame.data(), bytes) != bytes) throw std::runtime_error("Truncated raw input");
                    ++index;
                    lock.lock();
                    queue.push_back(std::move(frame));
                    high_water = std::max(high_water, queue.size());
                    ready.notify_all();
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex);
                error = e.what(); ready.notify_all();
            }
        });
    }
    std::vector<uint8_t> next() {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this] { return stopping || !error.empty() || !queue.empty(); });
        if (!error.empty()) throw std::runtime_error(error);
        if (stopping) return {};
        auto result = std::move(queue.front()); queue.pop_front();
        ready.notify_all(); return result;
    }
};
}
