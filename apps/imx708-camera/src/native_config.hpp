// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace imx708 {
inline std::string dt_string(const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::string value((std::istreambuf_iterator<char>(stream)), {});
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}

// Return the expected sensor OF node; adapter and subdevice numbers may change.
inline std::filesystem::path native_sensor_node(const std::filesystem::path& dt) {
    const auto symbol = [&](const char* name) {
        const auto value = dt_string(dt / "__symbols__" / name);
        if (value.empty() || value.front() != '/')
            throw std::runtime_error("摄像头设备树缺少节点定义");
        return dt / value.substr(1);
    };
    const auto cci = symbol("cci1"), bus = symbol("cci1_i2c1");
    const auto sensor = symbol("cam3_raw_sensor");
    const auto enabled = [](const auto& node) {
        return std::filesystem::exists(node) &&
            (!std::filesystem::exists(node / "status") ||
             dt_string(node / "status") == "okay" || dt_string(node / "status") == "ok");
    };
    if (enabled(symbol("cci0")) || enabled(symbol("cci1_i2c0")) ||
        enabled(dt / "i2c-cam3-gpio"))
        throw std::runtime_error("摄像头总线配置冲突");
    if (!enabled(cci) || !enabled(bus) || !enabled(sensor) || !enabled(symbol("camss")) ||
        bus.parent_path() != cci || sensor.parent_path() != bus ||
        dt_string(cci / "compatible").find("qcom,sc7280-cci") == std::string::npos ||
        dt_string(sensor / "compatible") != "sony,imx708")
        throw std::runtime_error("请启动完整的原生 CCI 摄像头配置");
    return sensor;
}
}
