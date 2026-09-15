// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hevc {
constexpr int width = 1920, height = 1080, fps = 30;
double monotonic_ms();
std::string quote(const std::string& value);
std::string number(double value); // Infinity is represented as a JSON string.
struct Distribution {
    std::vector<double> values;
    std::string json() const;
};
struct LumaError {
    uint64_t squared = 0, pixels = 0;
    double psnr() const;
};
LumaError compare_luma(const uint8_t* a, ptrdiff_t a_stride,
                       const uint8_t* b, ptrdiff_t b_stride, int w, int h);
struct Frame {
    size_t source_index = 0;
    size_t encoded_bytes = 0;
    double source_before_copy = -1;
    double source = -1, encode_in = -1, encode_out = -1;
    double decode_in = -1, decode_out = -1;
    bool quality = false;
    LumaError y;
};
class Frames {
public:
    std::map<uint64_t, Frame> rows;
    size_t unmatched = 0, duplicates = 0;
    bool mark(uint64_t pts, double Frame::*field, double time);
};
int self_test();
}
