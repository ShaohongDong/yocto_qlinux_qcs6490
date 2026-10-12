// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace q6a {
// Original Radiotap + 802.11 bytes, with receiver realtime timestamps.
// Capturing packets is not OpenHD/FEC or radio-parameter qualification.
class PcapCapture {
public:
    static constexpr uint32_t snaplen = 8192;
    static constexpr uint64_t default_limit = 256ULL * 1024 * 1024;
    explicit PcapCapture(const std::string &path, uint64_t limit = default_limit);
    ~PcapCapture();
    PcapCapture(const PcapCapture &) = delete;
    PcapCapture &operator=(const PcapCapture &) = delete;
    void append(const uint8_t *frame, size_t size, uint64_t realtime_us);
    void finish();
    uint64_t packets() const { return packets_; }
    uint64_t bytes() const { return bytes_; }
    bool limit_reached() const { return limit_reached_; }
    bool write_failed() const { return write_failed_; }
private:
    void write_bytes(const uint8_t *data, size_t size);
    int fd_ = -1;
    uint64_t limit_, bytes_ = 0, packets_ = 0;
    bool limit_reached_ = false, write_failed_ = false;
};
}
