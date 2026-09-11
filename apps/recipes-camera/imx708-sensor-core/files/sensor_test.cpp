// SPDX-License-Identifier: GPL-2.0-only
#include "sensor.hpp"
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <iostream>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F> static void rejects(F f) {
    try { f(); } catch (const std::invalid_argument &) { return; }
    throw std::runtime_error("Invalid input accepted");
}
int main() {
    using namespace imx708;
    try {
        for (auto mode : {Mode::Preview, Mode::Still}) {
            const auto &t = timing(mode);
            auto regs = startup(mode, {}, 0x30);
            std::map<uint16_t, uint8_t> memory;
            for (const auto &r : regs) {
                auto wire = encode(r);
                for (unsigned i = 0; i < r.bytes; ++i) memory[r.address + i] = wire[2 + i];
                check(!(r.address == 0x0100 && r.value == 1), "Premature stream-on");
            }
            auto word = [&](uint16_t a) { return (memory.at(a) << 8) | memory.at(a + 1); };
            check(word(0x034c) == int(t.width) && word(0x034e) == int(t.height), "Output dimensions");
            check(word(0x0342) == int(t.line_length), "Line timing");
            check(word(0x0340) == int(t.default_frame), "Frame timing");
            check(word(0x0136) == 0x1800 && word(0x030e) == 300, "Clock setup");
            check(memory.at(0x0114) == 1 && memory.at(0x0112) == 10, "Lane / RAW format");
            check(memory.at(0xc428) == (mode == Mode::Still ? 0 : 1), "Remosaic selection");
            check(word(0x0306) == (mode == Mode::Still ? 124 : 122), "Pixel PLL");
            check(memory.count(0x7b10) == 0, "Overwriting calibrated SPC");
            check(startup(mode, {}, 0x40).size() == regs.size() + 108, "SPC fallback count");
            Request r;
            r.exposure_lines = 1;
            check(calculate(mode, r).effective_exposure == t.minimum_exposure, "Minimum exposure");
            r.exposure_lines = 0xffffffffU;
            auto e = calculate(mode, r);
            check(e.effective_exposure <= t.default_frame - 48, "Exposure frame margin");
            check(e.effective_exposure % t.exposure_step == 0, "Exposure granularity");
            r.frame_lines = t.minimum_frame - 1;
            rejects([&] { calculate(mode, r); });
        }
        Request r;
        r.total_gain = 2;
        auto e = calculate(Mode::Preview, r);
        check(e.analog_code == 512 && e.digital_code == 256 && e.applied_gain == 2, "2x gain");
        r.total_gain = 32;
        e = calculate(Mode::Preview, r);
        check(e.analog_code == 960 && e.digital_code == 512 && e.applied_gain == 32, "32x gain");
        r.total_gain = std::numeric_limits<double>::max();
        e = calculate(Mode::Preview, r);
        check(e.digital_code == 65535 && e.analog_code == 960, "Gain saturation");
        for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            r.total_gain = bad;
            rejects([&] { calculate(Mode::Preview, r); });
        }
        r = {};
        r.frame_lines = 131071;
        r.exposure_lines = 131071;
        e = calculate(Mode::Preview, r);
        check(e.shift == 1 && e.effective_frame == 131070, "Long-frame quantization");
        check(e.effective_exposure <= e.effective_frame - 48, "Long exposure margin");
        r.frame_lines = (65535U << 7) + 1;
        rejects([&] { calculate(Mode::Preview, r); });
        r.frame_lines = 65535U << 7;
        r.exposure_lines = 0xffffffffU;
        e = calculate(Mode::Preview, r);
        check(e.shift == 7 && e.effective_exposure <= e.effective_frame - 48, "Maximum long exposure");
        r = {};
        r.hflip = r.vflip = r.color_bars = true;
        auto update = controls(Mode::Preview, r);
        check(update.at(5).value == 3 && update.at(6).value == 2, "Orientation and test pattern");
        for (auto hz : {447000000U, 453000000U}) {
            auto seq = startup(Mode::Preview, {}, 0, hz);
            bool found = false;
            for (const auto &reg : seq)
                if (reg.address == 0x030e) {
                    check(reg.value == (hz == 447000000U ? 298 : 302), "Alternative link PLL");
                    found = true;
                }
            check(found, "Missing output PLL");
        }
        rejects([] { startup(Mode::Preview, {}, 0, 450000000, 4); });
        rejects([] { startup(Mode::Preview, {}, 0, 450000001); });
        rejects([] { encode({0x100, 0x100, 1}); });
        check(encode({0x030e, 0x012c, 2}) == std::vector<uint8_t>({3, 14, 1, 44}), "16-bit bus encoding");
        check(encode({0x0114, 1, 1}) == std::vector<uint8_t>({1, 20, 1}), "8-bit bus encoding");
        std::cout << "PASS: register decoding, timing, gains, long exposure and rejected inputs\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
