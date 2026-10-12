// SPDX-License-Identifier: MIT
#include "pcap.hpp"
#include "raw_protocol.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
uint32_t le(const std::vector<uint8_t> &data, size_t offset, size_t width = 4) {
    uint32_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint32_t(data.at(offset + i)) << (8 * i);
    return value;
}
struct Directory {
    char path[64] = "/tmp/q6a-pcap-test-XXXXXX";
    Directory() { if (!mkdtemp(path)) throw std::runtime_error("cannot create test directory"); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}
int main(int argc, char **argv) {
    try {
        Directory temporary;
        const auto id = q6a::parse_run_id("0123456789abcdef0123456789abcdef");
        const auto first = q6a::make_frame(id, 0, 1234, 64, 3);
        const auto second = q6a::make_frame(id, 1, 2345, 65, 7);
        const std::string path = std::string(temporary.path) + "/capture.pcap";
        q6a::PcapCapture capture(path);
        capture.append(first.data(), first.size(), 1700000000123456);
        capture.append(second.data(), second.size(), 1700000001654321);
        capture.finish();
        std::ifstream input(path, std::ios::binary);
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        require(le(data, 0) == 0xa1b2c3d4 && le(data, 4, 2) == 2 && le(data, 6, 2) == 4,
                "PCAP format or timestamp resolution is wrong");
        require(le(data, 8) == 0 && le(data, 12) == 0 && le(data, 16) == 8192 && le(data, 20) == 127,
                "PCAP link type or snapshot length is wrong");
        const size_t second_offset = 24 + 16 + first.size();
        require(second_offset % 4 != 0, "test must cover unaligned packet records");
        for (const auto offset : {size_t(24), second_offset}) {
            const auto &frame = offset == 24 ? first : second;
            require(le(data, offset + 8) == frame.size() && le(data, offset + 12) == frame.size(),
                    "packet length changed");
            require(std::equal(frame.begin(), frame.end(), data.begin() + offset + 16),
                    "capture did not preserve the original Radiotap/frame bytes");
        }
        require(le(data, 24) == 1700000000 && le(data, 28) == 123456 &&
                le(data, second_offset) == 1700000001 && le(data, second_offset + 4) == 654321,
                "receiver realtime timestamps changed");
        require(data.size() == 24 + 32 + first.size() + second.size() && capture.packets() == 2 &&
                capture.bytes() == data.size(), "capture accounting is wrong");
        bool refused = false;
        try { q6a::PcapCapture overwrite(path); } catch (const std::runtime_error &) { refused = true; }
        require(refused && std::filesystem::file_size(path) == data.size(), "existing evidence was overwritten");
        const std::string bounded_path = std::string(temporary.path) + "/bounded.pcap";
        q6a::PcapCapture bounded(bounded_path, 24 + 16 + first.size());
        bounded.append(first.data(), first.size(), 1700000000000000);
        refused = false;
        try { bounded.append(second.data(), second.size(), 1700000000000000); }
        catch (const std::runtime_error &) { refused = true; }
        require(refused && bounded.limit_reached() && bounded.packets() == 1 &&
                std::filesystem::file_size(bounded_path) == 24 + 16 + first.size(),
                "size cap did not stop before writing a partial record");
        refused = false;
        try { capture.append(first.data(), first.size(), (uint64_t(1) << 32) * 1000000); }
        catch (const std::invalid_argument &) { refused = true; }
        require(refused && capture.packets() == 2, "out-of-range timestamp was silently truncated");
        if (argc == 2) {
            // Optional fixture for verification by an independent PCAP reader.
            std::filesystem::copy_file(path, argv[1]);
        } else if (argc != 1) throw std::invalid_argument("usage: pcap-test [new-fixture-path]");
        std::cout << "PCAP byte preservation, record boundaries, timestamp and size-cap tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
