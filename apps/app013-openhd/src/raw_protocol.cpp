// SPDX-License-Identifier: MIT
#include "raw_protocol.hpp"
#include <algorithm>
#include <stdexcept>

namespace q6a {
namespace {
constexpr std::array<uint8_t, 8> magic{'Q', '6', 'A', 'R', 'A', 'W', '0', '1'};
constexpr size_t radio_size = 13, wifi_size = 24, probe_size = 42;
uint64_t read_le(const uint8_t *p, unsigned count) {
    uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
void append_le(std::vector<uint8_t> &out, uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out.push_back((value >> (8 * i)) & 255);
}
uint8_t payload_byte(uint64_t sequence, size_t index) {
    return static_cast<uint8_t>(index ^ (sequence & 255) ^ ((sequence >> 8) & 255));
}
}
RunId parse_run_id(const std::string &text) {
    if (text.size() != 32) throw std::invalid_argument("run-id must be 32 hex digits");
    const auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("run-id must be hexadecimal");
    };
    RunId id{};
    for (size_t i = 0; i < id.size(); ++i) id[i] = (nibble(text[2*i]) << 4) | nibble(text[2*i+1]);
    return id;
}
std::vector<uint8_t> make_frame(const RunId &id, uint64_t sequence,
                              uint64_t timestamp_ns, uint16_t payload_bytes,
                              uint8_t mcs) {
    if (payload_bytes < 64 || payload_bytes > 1400 || mcs > 7)
        throw std::invalid_argument("payload must be 64..1400 bytes; 1SS HT MCS must be 0..7");
    std::vector<uint8_t> out{0, 0};
    append_le(out, radio_size, 2);
    append_le(out, (1U << 15) | (1U << 19), 4); // TX_FLAGS + MCS
    append_le(out, 0x0008, 2); // TX_NOACK
    out.insert(out.end(), {0x03, 0x00, mcs}); // known BW + MCS; HT20, long GI
    out.insert(out.end(), {0x08, 0x00, 0x00, 0x00}); // data, no DS, duration=0
    out.insert(out.end(), 6, 0xff); // broadcast destination
    out.insert(out.end(), {0x02, 0x51, 0x36, 0x41, 0x00, 0x01}); // source
    out.insert(out.end(), {0x02, 0x51, 0x36, 0x41, 0x00, 0x02}); // BSSID
    append_le(out, (sequence & 4095) << 4, 2);
    out.insert(out.end(), magic.begin(), magic.end());
    out.insert(out.end(), id.begin(), id.end());
    append_le(out, sequence, 8);
    append_le(out, timestamp_ns, 8);
    append_le(out, payload_bytes, 2);
    for (size_t i = 0; i < payload_bytes; ++i) out.push_back(payload_byte(sequence, i));
    return out;
}
std::optional<Packet> parse_frame(const uint8_t *bytes, size_t size) {
    if (size < 8 || bytes[0] != 0) return std::nullopt;
    const size_t rt = read_le(bytes + 2, 2);
    if (rt < 8 || rt > size || size - rt < wifi_size + probe_size) return std::nullopt;
    // Exact unencrypted diagnostic data format, including fixed source/BSSID.
    const auto *wifi = bytes + rt;
    constexpr uint8_t source[]{0x02, 0x51, 0x36, 0x41, 0x00, 0x01};
    constexpr uint8_t bssid[]{0x02, 0x51, 0x36, 0x41, 0x00, 0x02};
    if (wifi[0] != 0x08 || wifi[1] != 0 ||
        !std::all_of(wifi + 4, wifi + 10, [](uint8_t b) { return b == 255; }) ||
        !std::equal(std::begin(source), std::end(source), wifi + 10) ||
        !std::equal(std::begin(bssid), std::end(bssid), wifi + 16)) return std::nullopt;
    const auto *probe = wifi + wifi_size;
    if (!std::equal(magic.begin(), magic.end(), probe)) return std::nullopt;
    Packet packet{};
    std::copy_n(probe + 8, 16, packet.run_id.begin());
    packet.sequence = read_le(probe + 24, 8);
    packet.sender_monotonic_ns = read_le(probe + 32, 8);
    packet.payload_bytes = read_le(probe + 40, 2);
    const size_t body = size - rt - wifi_size - probe_size;
    // RX Radiotap implementations may include a four-byte trailing FCS.
    if (packet.payload_bytes < 64 || packet.payload_bytes > 1400 ||
        (body != packet.payload_bytes && body != size_t(packet.payload_bytes) + 4)) return std::nullopt;
    for (size_t i = 0; i < packet.payload_bytes; ++i)
        if (probe[probe_size + i] != payload_byte(packet.sequence, i)) return std::nullopt;
    return packet;
}
bool self_test() {
    const RunId id = parse_run_id("0123456789abcdef0123456789abcdef");
    auto frame = make_frame(id, 1025, 123456, 1200, 3);
    const auto packet = parse_frame(frame.data(), frame.size());
    return packet && packet->run_id == id && packet->sequence == 1025 &&
           packet->sender_monotonic_ns == 123456 && packet->payload_bytes == 1200;
}
}
