// SPDX-License-Identifier: MIT
#include "raw_protocol.hpp"
#include <iostream>
#include <stdexcept>

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        require(q6a::self_test(), "roundtrip failed");
        const auto id = q6a::parse_run_id("0123456789abcdef0123456789ABCDEF");
        for (const auto size : {64, 512, 1200, 1400}) {
            auto frame = q6a::make_frame(id, 65537, 1024, size, 7);
            require(q6a::parse_frame(frame.data(), frame.size()).has_value(), "valid frame rejected");
            for (size_t length = 0; length < frame.size(); ++length)
                require(!q6a::parse_frame(frame.data(), length), "truncated frame accepted");
            frame.back() ^= 1;
            require(!q6a::parse_frame(frame.data(), frame.size()), "corrupt payload accepted");
        }
        auto frame = q6a::make_frame(id, 1, 2, 64, 3);
        auto mutate = [&](size_t index, uint8_t byte) {
            auto copy = frame; copy[index] = byte;
            require(!q6a::parse_frame(copy.data(), copy.size()), "malformed frame accepted");
        };
        mutate(0, 1); mutate(2, 7); mutate(3, 255);
        mutate(13, 0x88); mutate(14, 0x40); // QoS and encrypted frames are not our probe
        mutate(13+10, 0x03); mutate(13+24, 0x00);
        mutate(13+24+40, 1); // invalid payload length
        frame.insert(frame.end(), {0, 0, 0, 0});
        require(q6a::parse_frame(frame.data(), frame.size()).has_value(), "RX FCS trailer rejected");
        frame.push_back(0);
        require(!q6a::parse_frame(frame.data(), frame.size()), "extra bytes accepted");
        for (const std::string input : {"bad", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"}) {
            bool threw = false;
            try { q6a::parse_run_id(input); } catch (const std::invalid_argument &) { threw = true; }
            require(threw, "invalid run id accepted");
        }
        bool threw = false;
        try { q6a::make_frame(id, 0, 0, 1200, 8); } catch (const std::invalid_argument &) { threw = true; }
        require(threw, "multi-stream MCS accepted");
        std::cout << "raw frame bounds, identity, payload and configuration tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
