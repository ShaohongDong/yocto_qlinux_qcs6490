// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "qnn.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc, char **argv) {
  try {
    if (argc != 6)
      throw std::runtime_error(
          "qnn-multi-probe MODEL SIDE INPUT_LIST OUTPUT_DIR REPORT");
    unsigned side = std::stoul(argv[2]);
    if (side != 192 && side != 224)
      throw std::runtime_error("Expected side 192 or 224");
    ai::Qnn model(argv[1], {1, side, side, 3}, std::vector<ai::TensorSpec>{});
    auto list = std::filesystem::absolute(argv[3]);
    std::filesystem::path out(argv[4]);
    std::filesystem::create_directories(out);
    std::ifstream lines(list);
    if (!lines)
      throw std::runtime_error("Missing input list");
    std::string line;
    size_t count = 0;
    auto start = std::chrono::steady_clock::now();
    std::vector<ai::TensorSpec> specs;
    while (std::getline(lines, line)) {
      if (line.empty())
        continue;
      std::ifstream f(list.parent_path() / line, std::ios::binary);
      std::vector<float> x(side * side * 3);
      f.read(reinterpret_cast<char *>(x.data()), x.size() * sizeof(float));
      if (!f || f.peek() != EOF)
        throw std::runtime_error("Wrong input bytes");
      auto ys = model.execute_all(x);
      specs.clear();
      for (size_t i = 0; i < ys.size(); ++i) {
        specs.push_back(ys[i].spec);
        std::ofstream raw(
            out / (std::to_string(count) + "-" + std::to_string(i) + ".raw"),
            std::ios::binary);
        raw.write(reinterpret_cast<const char *>(ys[i].values.data()),
                  ys[i].values.size() * sizeof(float));
        raw.close();
        if (!raw)
          throw std::runtime_error("Output write failed");
      }
      ++count;
    }
    if (!count)
      throw std::runtime_error("Empty input list");
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::ofstream report(argv[5]);
    report << "{\"status\":\"PASS\",\"samples\":" << count
           << ",\"seconds\":" << elapsed << ",\"outputs\":[";
    for (size_t i = 0; i < specs.size(); ++i) {
      if (i)
        report << ',';
      report << "{\"name\":" << ai::quote(specs[i].name) << ",\"shape\":[";
      for (size_t j = 0; j < specs[i].shape.size(); ++j) {
        if (j)
          report << ',';
        report << specs[i].shape[j];
      }
      report << "]}";
    }
    report << "],\"profile\":" << model.profile_json() << "}\n";
    report.close();
    if (!report)
      throw std::runtime_error("Report write failed");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
