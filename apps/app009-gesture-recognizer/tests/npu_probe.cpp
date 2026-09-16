// SPDX-License-Identifier: MIT
#include "qnn.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc, char **argv) {
  try {
    if (argc != 5)
      throw std::runtime_error(
          "npu-probe FEATURES.bin TEMPORAL.bin RGB.raw REPORT.json");
    ai::Qnn features(argv[1], {1, 224, 224, 3}, 576),
        temporal(argv[2], {1, 1, 32, 576}, 12);
    std::vector<float> x(224 * 224 * 3);
    std::ifstream f(argv[3], std::ios::binary);
    f.read(reinterpret_cast<char *>(x.data()), x.size() * 4);
    if (!f || f.peek() != EOF)
      throw std::runtime_error("Bad input file");
    std::vector<float> window;
    for (int i = 0; i < 32; ++i) {
      auto v = features.execute(x);
      window.insert(window.end(), v.begin(), v.end());
    }
    auto logits = temporal.execute(window);
    for (float v : logits)
      if (!std::isfinite(v))
        throw std::runtime_error("Nonfinite result");
    std::ofstream out(argv[4]);
    out << "{\"status\":\"PASS\",\"smoke_only\":true,\"features_profile\":"
        << features.profile_json()
        << ",\"temporal_profile\":" << temporal.profile_json()
        << ",\"logits\":[";
    for (size_t i = 0; i < logits.size(); ++i) {
      if (i)
        out << ',';
      out << logits[i];
    }
    out << "]}\n";
    out.close();
    if (!out)
      throw std::runtime_error("Cannot write report");
    std::cout << "Both NPU graphs executed in one process\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
