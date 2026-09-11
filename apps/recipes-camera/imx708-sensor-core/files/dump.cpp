// SPDX-License-Identifier: GPL-2.0-only
#include "sensor.hpp"
#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    if (argc != 3 || (std::string(argv[1]) != "preview" && std::string(argv[1]) != "still") ||
        (std::string(argv[2]) != "spc-default" && std::string(argv[2]) != "spc-calibrated")) {
        std::fprintf(stderr, "Usage: imx708-register-dump preview|still spc-default|spc-calibrated\n"
                     "Offline sequence dump only; no device access. Select SPC branch explicitly.\n");
        return 2;
    }
    auto mode = std::string(argv[1]) == "preview" ? imx708::Mode::Preview : imx708::Mode::Still;
    auto regs = imx708::startup(mode, {}, std::string(argv[2]) == "spc-default" ? 0x40 : 0);
    std::puts("# Offline register plan. Requires verified power, ID, SPC read and CSI setup.");
    std::puts("# address value bytes (big endian); stream-on intentionally excluded");
    for (const auto &reg : regs)
        std::printf("%04x %04x %u\n", reg.address, reg.value, reg.bytes);
}
