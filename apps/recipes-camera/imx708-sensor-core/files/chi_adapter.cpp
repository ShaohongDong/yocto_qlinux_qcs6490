// SPDX-License-Identifier: GPL-2.0-only
// CHI ABI declarations are supplied externally; no vendor headers are bundled.
#include "camxsensordriverapi.h"
#include "sensor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

static_assert(MAX_REG_SETTINGS >= 7, "Exposure sequence requires seven register slots");

static bool mode_for(UINT32 index, imx708::Mode &mode) {
    if (index > 1) return false;
    mode = index == 0 ? imx708::Mode::Preview : imx708::Mode::Still;
    return true;
}

static BOOL calculate_exposure(SensorExposureInfo *out, SensorCalculateExposureData *in) {
    if (!out || !in || !std::isfinite(in->realGain) || in->realGain <= 0) return false;
    imx708::Mode mode;
    if (!mode_for(in->sensorResolutionIndex, mode)) return false;
    try {
        imx708::Request request;
        request.total_gain = in->realGain;
        request.exposure_lines = in->lineCount;
        // CHI separately chooses frame length; do not clamp to the default fps.
        request.frame_lines = 65535;
        auto exposure = imx708::calculate(mode, request);
        *out = {};
        out->analogRegisterGain = exposure.analog_code;
        out->analogRealGain = 1024.0f / (1024 - exposure.analog_code);
        out->digitalRegisterGain = exposure.digital_code;
        out->digitalRealGain = exposure.digital_code / 256.0f;
        out->ISPDigitalGain = in->realGain / (out->analogRealGain * out->digitalRealGain);
        out->lineCount = exposure.effective_exposure;
        out->sensitivityCorrectionFactor = 1;
        return true;
    } catch (...) { return false; }
}

static BOOL fill_exposure(RegSettingsInfo *out, SensorFillExposureData *in) {
    if (!out) return false;
    out->regSettingCount = 0;
    if (!in || !in->pRegInfo || in->applyShortExposure || in->applyMiddleExposure ||
        in->pStrobeInfo || in->analogRegisterGain < 112 || in->analogRegisterGain > 960 ||
        in->digitalRegisterGain < 256 || in->digitalRegisterGain > 65535) return false;
    imx708::Mode mode;
    if (!mode_for(in->sensorResolutionIndex, mode)) return false;
    const auto &t = imx708::timing(mode);
    // Long exposure shifts require additional CamX timing metadata; not enabled
    // through this reference adapter until that contract is verified.
    if (in->frameLengthLines < t.minimum_frame || in->frameLengthLines > 65535 ||
        in->lineCount < t.minimum_exposure || in->lineCount > in->frameLengthLines - 48 ||
        in->lineCount % t.exposure_step) return false;
    const auto &info = *in->pRegInfo;
    if (info.frameLengthLinesAddr != 0x0340 || info.coarseIntgTimeAddr != 0x0202 ||
        info.globalAnalogGainAddr != 0x0204 || info.GlobalDigitalGainAddr != 0x020e)
        return false;
    const auto &on = info.groupHoldOnSettings;
    const auto &off = info.groupHoldOffSettings;
    // Require the matching IMX708 group-hold sequence, not arbitrary register IO.
    if (on.regSettingCount != 1 || off.regSettingCount != 1) return false;
    auto hold_ok = [](const RegSetting &r, unsigned value) {
        return r.registerAddr == 0x0104 && r.registerData == value &&
               r.regAddrType == I2CRegAddressDataTypeWord &&
               r.regDataType == I2CRegAddressDataTypeByte && r.operation == IOOperationTypeWrite;
    };
    if (!hold_ok(on.regSetting[0], 1) || !hold_ok(off.regSetting[0], 0)) return false;
    RegSettingsInfo settings{};
    auto add = [&](unsigned address, unsigned value, I2CRegAddressDataType width) {
        auto &reg = settings.regSetting[settings.regSettingCount++];
        reg.registerAddr = address;
        reg.registerData = value;
        reg.regAddrType = I2CRegAddressDataTypeWord;
        reg.regDataType = width;
        reg.operation = IOOperationTypeWrite;
    };
    add(0x0104, 1, I2CRegAddressDataTypeByte);
    add(0x0340, in->frameLengthLines, I2CRegAddressDataTypeWord);
    add(0x3100, 0, I2CRegAddressDataTypeByte);
    add(0x0202, in->lineCount, I2CRegAddressDataTypeWord);
    add(0x0204, in->analogRegisterGain, I2CRegAddressDataTypeWord);
    add(0x020e, in->digitalRegisterGain, I2CRegAddressDataTypeWord);
    add(0x0104, 0, I2CRegAddressDataTypeByte);
    *out = settings;
    return true;
}

extern "C" CDK_VISIBILITY_PUBLIC void GetSensorLibraryAPIs(SensorLibraryAPI *api) {
    if (!api || api->size < sizeof(SensorLibraryAPI)) return;
    const auto capacity = api->size;
    *api = {};
    api->size = capacity;
    api->majorVersion = 1;
    api->minorVersion = 0;
    api->pCalculateExposure = calculate_exposure;
    api->pFillExposureSettings = fill_exposure;
}
