// SPDX-License-Identifier: MIT
#include "native.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>

static void check(bool value) { if (!value) throw std::runtime_error("RAW test failed"); }
int main() {
    using namespace imx708;
    try {
        RawSettings settings; settings.black = 0;
        // Two RGGB cells: red and blue, each with zero green; ignore row padding.
        const uint8_t raw[] = {255,0,0,0,3,99,99, 0,0,0,255,192,99,99};
        const auto rgb = raw_to_rgb(raw, sizeof(raw), 4, 2, 7, settings);
        check(rgb == std::vector<uint8_t>({255,0,0,0,0,255}));
        const uint8_t low[] = {0,0,0,0,255, 0,0,0,0,255};
        check(raw_to_rgb(low, sizeof(low), 4, 2, 5, settings)[0] > 0);
        settings.black = 64;
        check(raw_to_rgb(low, sizeof(low), 4, 2, 5, settings) == std::vector<uint8_t>(6,0));
        settings.red = settings.blue = 8;
        check(raw_to_rgb(raw, sizeof(raw), 4, 2, 7, settings)[0] == 255);
        for (int mode = 0; mode < 4; ++mode) {
            bool rejected = false;
            auto s = settings; if (mode == 3) s.red = std::numeric_limits<double>::quiet_NaN();
            try { raw_to_rgb(raw, mode == 0 ? 4 : sizeof(raw), mode == 1 ? 3 : 4, 2, mode == 2 ? 4 : 7, s); }
            catch (const std::runtime_error&) { rejected = true; }
            check(rejected);
        }
        check(valid_raw_buffer(100,100,false,true,2,1));
        check(!valid_raw_buffer(99,100,false,true,2,1));
        check(!valid_raw_buffer(100,100,true,true,2,1));
        check(!valid_raw_buffer(100,100,false,false,2,1));
        check(!valid_raw_buffer(100,100,false,true,1,1));
        std::cout << "PASS: Bayer order, RAW10 low bits, padding, tone mapping, invalid and error buffers\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
