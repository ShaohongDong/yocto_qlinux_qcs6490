// SPDX-License-Identifier: MIT
#include "qnn.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc, char **argv) {
  try {
    if (argc != 7)
      throw std::runtime_error(
          "qnn-batch MODEL ROLE WINDOW INPUT_LIST OUTPUT_DIR REPORT");
    std::string role = argv[2];
    unsigned window = std::stoul(argv[3]);
    if (role != "features" && role != "temporal")
      throw std::runtime_error("Unknown role");
    if (window != 32 && window != 48)
      throw std::runtime_error("Invalid window");
    ai::Qnn model(argv[1],
                  role == "features" ? std::vector<unsigned>{1, 224, 224, 3}
                                     : std::vector<unsigned>{1, 1, window, 576},
                  role == "features" ? 576 : 12);
    std::filesystem::path list = std::filesystem::absolute(argv[4]),
                          out = std::filesystem::absolute(argv[5]);
    std::filesystem::create_directories(out);
    std::ifstream lines(list);
    if (!lines)
      throw std::runtime_error("Missing input list");
    std::string line;
    size_t count = 0;
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty())
        continue;
      auto path = list.parent_path() / line;
      std::ifstream file(path, std::ios::binary);
      std::vector<float> input(role == "features" ? 224 * 224 * 3
                                                  : window * 576);
      file.read(reinterpret_cast<char *>(input.data()), input.size() * 4);
      if (!file || file.peek() != EOF)
        throw std::runtime_error("Wrong input bytes: " + path.string());
      auto y = model.execute(input);
      for (float f : y)
        if (!std::isfinite(f))
          throw std::runtime_error("Nonfinite model result");
      std::ofstream f(out / (std::to_string(count) + ".raw"), std::ios::binary);
      f.write(reinterpret_cast<const char *>(y.data()), y.size() * 4);
      f.close();
      if (!f)
        throw std::runtime_error("Output write failed");
      ++count;
    }
    if (!count)
      throw std::runtime_error("Empty input list");
    std::ofstream report(argv[6]);
    report << "{\"status\":\"PASS\",\"samples\":" << count
           << ",\"profile\":" << model.profile_json() << "}\n";
    report.close();
    if (!report)
      throw std::runtime_error("Report write failed");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
