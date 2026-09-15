// SPDX-License-Identifier: MIT
#include "native_config.hpp"
#include <unistd.h>
#include <iostream>
using namespace std::filesystem;
static void put(const path& file, const std::string& value) {
    create_directories(file.parent_path());
    std::ofstream stream(file); stream << value << '\0';
}
int main() {
    char name[] = "/tmp/imx708-dt-XXXXXX";
    if (!mkdtemp(name)) return 1;
    const path root(name);
    try {
        for (const auto* label : {"cci0", "cci1", "cci1_i2c0", "cci1_i2c1", "cam3_raw_sensor", "camss"}) {
            std::string node = std::string("/") + label;
            if (std::string(label) == "cci1_i2c0") node = "/cci1/bus0";
            if (std::string(label) == "cci1_i2c1") node = "/cci1/bus1";
            if (std::string(label) == "cam3_raw_sensor") node = "/cci1/bus1/imx708@1a";
            put(root / "__symbols__" / label, node);
            put(root / node.substr(1) / "status", "okay");
        }
        put(root / "cci0/status", "disabled"); put(root / "cci1/bus0/status", "disabled");
        put(root / "cci1/compatible", "qcom,sc7280-cci");
        put(root / "cci1/bus1/imx708@1a/compatible", "sony,imx708");
        const auto sensor = root / "cci1/bus1/imx708@1a";
        if (imx708::native_sensor_node(root) != sensor) throw std::runtime_error("wrong sensor");
        for (const auto& item : {std::pair{"i2c-cam3-gpio/status", "okay"},
                                 {"cci1/bus0/status", "okay"}, {"cci0/status", "okay"},
                                 {"cci1/status", "disabled"}, {"camss/status", "disabled"},
                                 {"cci1/compatible", "qcom,cci"},
                                 {"__symbols__/cam3_raw_sensor", "/other/imx708@1a"}}) {
            const path file = root / item.first;
            const bool existed = exists(file);
            const auto old = imx708::dt_string(file);
            put(file, item.second);
            bool rejected = false;
            try { imx708::native_sensor_node(root); } catch (const std::runtime_error&) { rejected = true; }
            if (existed) put(file, old); else remove_all(file.parent_path());
            if (!rejected) throw std::runtime_error(std::string("accepted invalid ") + item.first);
        }
        remove(root / "cci1/bus1/imx708@1a/status");
        if (imx708::native_sensor_node(root) != sensor) throw std::runtime_error("implicit status");
        remove_all(root);
        std::cout << "PASS CCI configuration and conflict rejection\n";
    } catch (const std::exception& error) {
        remove_all(root); std::cerr << error.what() << '\n'; return 1;
    }
}
