// SPDX-License-Identifier: MIT
#include "native.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace imx708 {
bool valid_raw_buffer(size_t bytes, size_t expected, bool error, bool monotonic,
                      uint64_t timestamp, uint64_t previous) {
    return bytes == expected && !error && monotonic && timestamp > previous;
}
std::vector<uint8_t> raw_to_rgb(const uint8_t* raw, size_t bytes, unsigned width,
                              unsigned height, unsigned stride, const RawSettings& settings) {
    if (!raw || !width || width % 4 || !height || height % 2 ||
        stride < width / 4 * 5 || bytes < size_t(stride) * height ||
        settings.black < 0 || settings.black >= 1023 ||
        !std::isfinite(settings.red) || !std::isfinite(settings.blue) ||
        settings.red <= 0 || settings.blue <= 0)
        throw std::runtime_error("Invalid RAW10 frame or colour settings");
    std::array<std::array<uint8_t, 1024>, 3> lut {};
    const double gains[] = {settings.red, 1.0, settings.blue};
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned i = 0; i < 1024; ++i) {
            const double linear = std::clamp((double(i) - settings.black) /
                                             (1023 - settings.black) * gains[c], 0.0, 1.0);
            lut[c][i] = uint8_t(std::lround(std::pow(linear, 1.0 / 2.2) * 255));
        }
    std::vector<uint8_t> rgb(size_t(width / 2) * (height / 2) * 3);
    auto pixel = [](const uint8_t* row, unsigned x) -> unsigned {
        const auto* p = row + (x / 4) * 5;
        return (unsigned(p[x % 4]) << 2) | ((p[4] >> ((x % 4) * 2)) & 3);
    };
    size_t dest = 0;
    for (unsigned y = 0; y < height; y += 2) {
        const auto* top = raw + size_t(y) * stride;
        const auto* bottom = top + stride;
        for (unsigned x = 0; x < width; x += 2) {
            rgb[dest++] = lut[0][pixel(top, x)];
            rgb[dest++] = lut[1][(pixel(top, x + 1) + pixel(bottom, x)) / 2];
            rgb[dest++] = lut[2][pixel(bottom, x + 1)];
        }
    }
    return rgb;
}
}
