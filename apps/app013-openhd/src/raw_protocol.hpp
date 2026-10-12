// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace q6a {
using RunId = std::array<uint8_t, 16>;
struct Packet {
    RunId run_id;
    uint64_t sequence;
    uint64_t sender_monotonic_ns;
    uint16_t payload_bytes;
};
RunId parse_run_id(const std::string &text);
std::vector<uint8_t> make_frame(const RunId &id, uint64_t sequence,
                              uint64_t timestamp_ns, uint16_t payload_bytes,
                              uint8_t mcs);
// Only accepts this application's diagnostic frames; NOT the OpenHD protocol.
// sender_monotonic_ns must never be subtracted from a different host's clock.
std::optional<Packet> parse_frame(const uint8_t *bytes, size_t size);
bool self_test();
}
