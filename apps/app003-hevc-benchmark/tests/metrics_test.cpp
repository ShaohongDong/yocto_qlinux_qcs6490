// SPDX-License-Identifier: MIT
#include "metrics.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
int main() {
    if (int result = hevc::self_test()) return result;
    hevc::Distribution d{{1, 2, 3, 4, 100}};
    if (d.json() != "{\"count\":5,\"mean\":22,\"p50\":3,\"p95\":100,\"max\":100}") return 10;
    if (hevc::Distribution{}.json().find("null") == std::string::npos) return 11;
    uint8_t a[4] = {}, b[4] = {255, 255, 255, 255};
    if (hevc::compare_luma(a, 2, b, 2, 2, 2).psnr() != 0) return 12;
    try { hevc::compare_luma(a, 1, b, 2, 2, 2); return 13; }
    catch (const std::invalid_argument&) {}
    hevc::Frames frames;
    frames.rows[42] = {};
    frames.mark(42, &hevc::Frame::encode_in, 10);
    // A dropped frame must stay missing, not acquire a zero-latency sample.
    if (frames.rows[42].encode_out != -1) return 14;
    std::cout << "metrics: PSNR, stride, perfect match, reorder, duplicate, missing frame, JSON PASS\n";
}
