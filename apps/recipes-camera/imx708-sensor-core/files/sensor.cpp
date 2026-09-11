// SPDX-License-Identifier: GPL-2.0-only
// Register sequence and timing derived from Raspberry Pi's IMX708 driver.
// Copyright (C) 2022 Raspberry Pi Ltd (upstream register data).
#include "sensor.hpp"
#include "registers.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace imx708 {
const Timing &timing(Mode mode) {
    static constexpr Timing preview{2304, 1296, 7824, 585600000, 1336, 2494, 4, 2};
    static constexpr Timing still{4608, 2592, 15648, 595200000, 2650, 2650, 8, 1};
    switch (mode) {
    case Mode::Preview: return preview;
    case Mode::Still: return still;
    }
    throw std::invalid_argument("Unknown IMX708 mode");
}

Exposure calculate(Mode mode, const Request &request) {
    const auto &t = timing(mode);
    if (!std::isfinite(request.total_gain) || request.total_gain <= 0)
        throw std::invalid_argument("Gain must be finite and positive");
    uint32_t frame = request.frame_lines ? request.frame_lines : t.default_frame;
    if (frame < t.minimum_frame || frame > (65535U << 7))
        throw std::invalid_argument("Frame length outside IMX708 mode limits");
    uint8_t shift = 0;
    while (frame > 65535) { frame >>= 1; ++shift; }
    // Round down to the representable frame before limiting exposure.
    uint32_t effective_frame = frame << shift;
    uint32_t exposure = std::clamp(request.exposure_lines, t.minimum_exposure,
                                   effective_frame - 48);
    exposure -= exposure % t.exposure_step;
    exposure >>= shift;
    exposure = std::max(exposure, 1U);
    const double analog = std::clamp(request.total_gain, 1024.0 / 912.0, 16.0);
    const auto code = static_cast<uint16_t>(std::clamp(
        std::floor(1024.0 - 1024.0 / analog), 112.0, 960.0));
    const double actual_analog = 1024.0 / (1024.0 - code);
    const auto digital = static_cast<uint16_t>(std::clamp(
        std::floor(request.total_gain / actual_analog * 256.0), 256.0, 65535.0));
    return {static_cast<uint16_t>(frame), static_cast<uint16_t>(exposure), code,
            digital, shift, effective_frame, exposure << shift,
            actual_analog * digital / 256.0};
}

std::vector<Register> controls(Mode mode, const Request &request) {
    const auto e = calculate(mode, request);
    // Backend must apply live updates using the verified CHI frame scheduling /
    // group-hold contract. This sequence by itself is not an atomic live update.
    return {{0x0340, e.frame_register, 2}, {0x3100, e.shift, 1},
            {0x0202, e.exposure_register, 2}, {0x0204, e.analog_code, 2},
            {0x020e, e.digital_code, 2},
            {0x0101, static_cast<uint16_t>(request.hflip | (request.vflip << 1)), 1},
            {0x0600, static_cast<uint16_t>(request.color_bars ? 2 : 0), 1}};
}

std::vector<Register> startup(Mode mode, const Request &request, uint8_t spc_left,
                              uint32_t link_hz, unsigned lanes) {
    const auto &t = timing(mode);
    auto updates = controls(mode, request); // Validate before producing any sequence.
    if (lanes != 2)
        throw std::invalid_argument("Q6A CAM3 requires two CSI-2 lanes");
    if (link_hz != 450000000 && link_hz != 447000000 && link_hz != 453000000)
        throw std::invalid_argument("Use a reviewed two-lane IMX708 link frequency");
    std::vector<Register> result(std::begin(mode_common_regs), std::end(mode_common_regs));
    if (spc_left == 0x40) {
        static constexpr uint8_t gains[2][9] = {
            {0x4c, 0x4c, 0x4c, 0x46, 0x3e, 0x39, 0x36, 0x36, 0x36},
            {0x36, 0x36, 0x36, 0x39, 0x3e, 0x46, 0x4c, 0x4c, 0x4c}};
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned i = 0; i < 54; ++i)
                result.push_back({static_cast<uint16_t>((side ? 0x7c00 : 0x7b10) + i),
                                  gains[side][i % 9], 1});
    }
    result.push_back({0x0114, 1, 1});
    result.push_back({0x0306, static_cast<uint16_t>(uint64_t(t.pixel_rate) * 20 / 96000000), 2});
    if (mode == Mode::Preview)
        result.insert(result.end(), std::begin(mode_2x2binned_regs), std::end(mode_2x2binned_regs));
    else
        result.insert(result.end(), std::begin(mode_4608x2592_regs), std::end(mode_4608x2592_regs));
    result.push_back({0x0342, static_cast<uint16_t>(t.line_length), 2});
    result.push_back({0x030e, static_cast<uint16_t>(uint64_t(link_hz) * 16 / 24000000), 2});
    if (mode == Mode::Still) result.push_back({0xc429, 2, 1});
    result.push_back({0xc428, static_cast<uint16_t>(mode == Mode::Still ? 0 : 1), 1});
    result.insert(result.end(), updates.begin(), updates.end());
    return result;
}

std::vector<uint8_t> encode(const Register &reg) {
    if ((reg.bytes != 1 && reg.bytes != 2) || (reg.bytes == 1 && reg.value > 255))
        throw std::invalid_argument("Invalid register width/value");
    std::vector<uint8_t> bytes{static_cast<uint8_t>(reg.address >> 8),
                                static_cast<uint8_t>(reg.address)};
    if (reg.bytes == 2) bytes.push_back(static_cast<uint8_t>(reg.value >> 8));
    bytes.push_back(static_cast<uint8_t>(reg.value));
    return bytes;
}
} // namespace imx708
