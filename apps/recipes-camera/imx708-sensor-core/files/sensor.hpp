// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <vector>

namespace imx708 {
struct Register {
    uint16_t address;
    uint16_t value;
    uint8_t bytes; // Value bytes, big endian; register address is always 16 bits.
};
enum class Mode { Preview, Still };
struct Timing {
    uint32_t width, height, line_length, pixel_rate;
    uint32_t minimum_frame, default_frame, minimum_exposure, exposure_step;
};
struct Request {
    uint32_t frame_lines = 0; // Zero selects the mode default.
    uint32_t exposure_lines = 1600;
    double total_gain = 1.0;
    bool hflip = false;
    bool vflip = false;
    bool color_bars = false;
};
struct Exposure {
    uint16_t frame_register, exposure_register, analog_code, digital_code;
    uint8_t shift;
    uint32_t effective_frame, effective_exposure;
    double applied_gain;
};
const Timing &timing(Mode mode);
Exposure calculate(Mode mode, const Request &request);
std::vector<Register> controls(Mode mode, const Request &request);
// Caller must stop streaming and verify chip ID, power/reset and receiver first.
// spc_left is the successful 8-bit read of 0x7b10, never an assumed value.
// The result ends in standby: the backend explicitly starts after receiver setup.
std::vector<Register> startup(Mode mode, const Request &request, uint8_t spc_left,
                              uint32_t link_hz = 450000000, unsigned lanes = 2);
std::vector<uint8_t> encode(const Register &reg);
} // namespace imx708
