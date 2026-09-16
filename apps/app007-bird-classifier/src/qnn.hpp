// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <string>
#include <vector>
namespace ai {
class Qnn {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit Qnn(const std::string& model);
    ~Qnn();
    std::vector<float> execute(const std::vector<float>& nhwc);
    std::string profile_json() const;
};
}
