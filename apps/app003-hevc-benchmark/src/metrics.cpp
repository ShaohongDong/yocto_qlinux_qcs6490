// SPDX-License-Identifier: MIT
#include "metrics.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace hevc {
double monotonic_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}
std::string number(double value) {
    if (std::isinf(value)) return quote(value > 0 ? "Infinity" : "-Infinity");
    if (!std::isfinite(value)) return "null";
    std::ostringstream out;
    out << std::setprecision(12) << value;
    return out.str();
}
std::string Distribution::json() const {
    if (values.empty()) return "{\"count\":0,\"mean\":null,\"p50\":null,\"p95\":null,\"max\":null}";
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double p) { return sorted[size_t(std::ceil(p * sorted.size())) - 1]; };
    return "{\"count\":" + std::to_string(values.size()) + ",\"mean\":" +
        number(std::accumulate(values.begin(), values.end(), 0.0) / values.size()) +
        ",\"p50\":" + number(percentile(.50)) + ",\"p95\":" + number(percentile(.95)) +
        ",\"max\":" + number(sorted.back()) + "}";
}
double LumaError::psnr() const {
    if (!pixels) return std::numeric_limits<double>::quiet_NaN();
    if (!squared) return std::numeric_limits<double>::infinity();
    return 10 * std::log10(65025.0 * double(pixels) / double(squared));
}
LumaError compare_luma(const uint8_t* a, ptrdiff_t as, const uint8_t* b, ptrdiff_t bs, int w, int h) {
    if (!a || !b || w <= 0 || h <= 0 || as < w || bs < w)
        throw std::invalid_argument("invalid luma layout");
    LumaError error;
    error.pixels = uint64_t(w) * h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int d = int(a[y * as + x]) - int(b[y * bs + x]);
            error.squared += d * d;
        }
    return error;
}
bool Frames::mark(uint64_t pts, double Frame::*field, double time) {
    auto it = rows.find(pts);
    if (it == rows.end()) { ++unmatched; return false; }
    if (it->second.*field >= 0) { ++duplicates; return false; }
    it->second.*field = time;
    return true;
}
int self_test() {
    uint8_t a[] = {0, 10, 99, 20, 30, 99};
    uint8_t b[] = {1, 11, 88, 88, 21, 31, 88, 88};
    auto e = compare_luma(a, 3, b, 4, 2, 2);
    if (e.squared != 4 || e.pixels != 4 || std::abs(e.psnr() - 48.1308036087) > 1e-8) return 1;
    if (!std::isinf(compare_luma(a, 3, a, 3, 2, 2).psnr())) return 2;
    Frames f;
    f.rows[1] = {}; f.rows[2] = {};
    if (!f.mark(2, &Frame::decode_out, 3) || !f.mark(1, &Frame::decode_out, 4)) return 3;
    if (f.mark(2, &Frame::decode_out, 5) || f.mark(3, &Frame::decode_out, 6)) return 4;
    if (f.duplicates != 1 || f.unmatched != 1 || f.rows[2].decode_out != 3) return 5;
    if (quote("\n\"\\") != "\"\\u000a\\\"\\\\\"") return 6;
    return 0;
}
}
