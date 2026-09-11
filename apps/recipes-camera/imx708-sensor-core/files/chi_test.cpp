// SPDX-License-Identifier: GPL-2.0-only
#include "camxsensordriverapi.h"
#include <dlfcn.h>
#include <cstdio>
#include <cmath>
#include <array>
#include <algorithm>
#include <cstddef>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto entry = reinterpret_cast<void (*)(SensorLibraryAPI *)>(dlsym(handle, "GetSensorLibraryAPIs"));
    if (!entry) return 1;
    struct Guarded { SensorLibraryAPI api{}; unsigned long guard = 0x12345678; } state;
    state.api.size = sizeof(state.api);
    entry(&state.api);
    if (state.guard != 0x12345678 || state.api.majorVersion != 1 ||
        !state.api.pCalculateExposure || !state.api.pFillExposureSettings) return 1;
    SensorCalculateExposureData request{};
    request.realGain = 2;
    request.lineCount = 100;
    struct ExposureBuffer { SensorExposureInfo info{}; std::array<unsigned char, 256> guard; } buffer;
    buffer.guard.fill(0xa5);
    auto &exposure = buffer.info;
    if (!state.api.pCalculateExposure(&exposure, &request) || exposure.lineCount != 100 ||
        !std::all_of(buffer.guard.begin(), buffer.guard.end(), [](auto c) { return c == 0xa5; }) ||
        std::abs(exposure.analogRealGain * exposure.digitalRealGain * exposure.ISPDigitalGain - 2) > .01)
    {
        std::fprintf(stderr, "CALC_ABI_MISMATCH: reference header lineCount offset=%zu, returned=%u, expected=100\n",
                     offsetof(SensorExposureInfo, lineCount), exposure.lineCount);
        dlclose(handle);
        return 1;
    }
    SensorExposureRegInfo registers{};
    registers.frameLengthLinesAddr = 0x0340;
    registers.coarseIntgTimeAddr = 0x0202;
    registers.globalAnalogGainAddr = 0x0204;
    registers.GlobalDigitalGainAddr = 0x020e;
    registers.groupHoldOnSettings.regSettingCount = 1;
    auto &on = registers.groupHoldOnSettings.regSetting[0];
    on.registerAddr = 0x0104; on.registerData = 1;
    on.regAddrType = I2CRegAddressDataTypeWord;
    on.regDataType = I2CRegAddressDataTypeByte;
    on.operation = IOOperationTypeWrite;
    registers.groupHoldOffSettings.regSettingCount = 1;
    registers.groupHoldOffSettings.regSetting[0] = on;
    registers.groupHoldOffSettings.regSetting[0].registerData = 0;
    SensorFillExposureData input{};
    input.pRegInfo = &registers;
    input.frameLengthLines = 2494;
    input.lineCount = exposure.lineCount;
    input.analogRegisterGain = exposure.analogRegisterGain;
    input.digitalRegisterGain = exposure.digitalRegisterGain;
    struct Output { RegSettingsInfo regs{}; unsigned long guard = 0xabcdef; } output;
    if (!state.api.pFillExposureSettings(&output.regs, &input) ||
        output.guard != 0xabcdef || output.regs.regSettingCount == 0 ||
        output.regs.regSettingCount > MAX_REG_SETTINGS) return 1;
    std::printf("PASS: entrypoint and exposure callbacks; API=%zu settings=%zu count=%u\n",
                sizeof(SensorLibraryAPI), sizeof(RegSetting), output.regs.regSettingCount);
    dlclose(handle);
    return 0;
}
