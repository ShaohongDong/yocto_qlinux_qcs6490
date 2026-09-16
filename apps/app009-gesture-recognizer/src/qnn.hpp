// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <string>
#include <vector>
namespace ai {
struct TensorSpec {
  std::string name;
  std::vector<unsigned> shape;
};
struct TensorResult {
  TensorSpec spec;
  std::vector<float> values;
};
class Qnn {
  struct Impl;
  std::unique_ptr<Impl> impl;

public:
  explicit Qnn(const std::string &model, const std::vector<unsigned> &shape,
               unsigned outputs);
  // Empty output specs are allowed for operator inspection only. Production
  // callers supply names and shapes from their versioned model manifest.
  Qnn(const std::string &model, const std::vector<unsigned> &shape,
      const std::vector<TensorSpec> &outputs);
  ~Qnn();
  std::vector<float> execute(const std::vector<float> &nhwc);
  std::vector<TensorResult> execute_all(const std::vector<float> &nhwc);
  std::string profile_json() const;
};
} // namespace ai
