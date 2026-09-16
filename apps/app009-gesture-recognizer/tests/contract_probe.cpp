// SPDX-License-Identifier: MIT
#include "events.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
int main(int argc, char **argv) {
  try {
    if (argc != 6)
      throw std::runtime_error("mode width height input output");
    std::ifstream in(argv[4], std::ios::binary);
    std::ofstream out(argv[5], std::ios::binary);
    if (!in || !out)
      throw std::runtime_error("Cannot open files");
    if (std::string(argv[1]) == "preprocess") {
      int w = std::stoi(argv[2]), h = std::stoi(argv[3]);
      if (w < 1 || h < 1 || w > 16384 || h > 16384)
        throw std::runtime_error("Bad dimensions");
      std::vector<uint8_t> data(size_t(w) * h * 3);
      in.read(reinterpret_cast<char *>(data.data()), data.size());
      if (!in)
        throw std::runtime_error("Short input");
      auto x = ai::preprocess(data.data(), w, h, w * 3, 3);
      out.write(reinterpret_cast<char *>(x.data()), x.size() * 4);
    } else {
      ai::Decoder d;
      double t;
      while (in >> t) {
        std::vector<float> p(12);
        for (float &v : p)
          in >> v;
        if (!in)
          throw std::runtime_error("Short scores");
        d.update(p, t);
      }
      d.close(999, false);
      out << std::setprecision(9);
      for (auto &e : d.events)
        out << e.label << ' ' << e.start << ' ' << e.end << ' ' << e.confirmed
            << ' ' << e.confidence << ' ' << e.complete << '\n';
    }
    return out ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
