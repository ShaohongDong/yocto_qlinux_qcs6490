// SPDX-License-Identifier: MIT
#include "pcap.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace q6a {
namespace {
void put_le(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (value >> (8 * i)) & 255;
}
}
PcapCapture::PcapCapture(const std::string &path, uint64_t limit) : limit_(limit) {
    if (limit < 24) throw std::invalid_argument("capture limit is smaller than its file header");
    fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd_ < 0) throw std::runtime_error("capture must be a new writable file");
    // PCAP v2.4, little endian, microsecond timestamps, LINKTYPE_IEEE802_11_RADIOTAP.
    // https://www.ietf.org/archive/id/draft-ietf-opsawg-pcap-09.html
    // https://www.iana.org/assignments/pcap/pcap.xhtml (link type 127)
    std::array<uint8_t, 24> header{};
    put_le(header.data(), 0xa1b2c3d4);
    header[4] = 2;
    header[6] = 4;
    put_le(header.data() + 16, snaplen);
    put_le(header.data() + 20, 127);
    try {
        write_bytes(header.data(), header.size());
        bytes_ = header.size();
    } catch (...) {
        close(fd_);
        fd_ = -1;
        throw;
    }
}
PcapCapture::~PcapCapture() { if (fd_ >= 0) close(fd_); }
void PcapCapture::write_bytes(const uint8_t *data, size_t size) {
    while (size) {
        const auto count = write(fd_, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            write_failed_ = true;
            throw std::runtime_error("cannot write capture");
        }
        data += count;
        size -= count;
    }
}
void PcapCapture::append(const uint8_t *frame, size_t size, uint64_t realtime_us) {
    if (!frame || !size || size > snaplen || realtime_us / 1000000 > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("invalid capture packet size or realtime timestamp");
    if (write_failed_ || limit_reached_) throw std::runtime_error("capture has already failed");
    if (size + 16 > limit_ - bytes_) {
        limit_reached_ = true;
        throw std::runtime_error("capture size limit reached");
    }
    std::array<uint8_t, 16> header{};
    put_le(header.data(), realtime_us / 1000000);
    put_le(header.data() + 4, realtime_us % 1000000);
    put_le(header.data() + 8, size);
    put_le(header.data() + 12, size);
    write_bytes(header.data(), header.size());
    write_bytes(frame, size);
    bytes_ += header.size() + size;
    ++packets_;
}
void PcapCapture::finish() {
    if (fsync(fd_) < 0) {
        write_failed_ = true;
        throw std::runtime_error("cannot sync capture");
    }
}
}
