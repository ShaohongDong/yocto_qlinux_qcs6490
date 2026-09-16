// SPDX-License-Identifier: MIT
#include "events.hpp"
#include <iostream>
void require(bool x) {
  if (!x)
    throw std::runtime_error("Test failed");
}
std::vector<float> prob(int label) {
  std::vector<float> p(12, .001f);
  p[label] = .989f;
  return p;
}
int main() {
  for (int fps : {24, 25, 30, 60}) {
    ai::Sampler sampler;
    int count = 0;
    for (int i = 0; i < fps * 10; ++i)
      if (sampler.take(double(i) / fps))
        ++count;
    require(count == 100);
    sampler.reset();
    require(sampler.take(0));
  }

  ai::Decoder d;
  for (int i = 0; i < 10; ++i)
    d.update(prob(3), i / 10.);
  for (int i = 10; i < 16; ++i)
    d.update(prob(0), i / 10.);
  require(d.events.size() == 1 && d.events[0].label == 3 &&
          d.events[0].complete);
  require(std::abs(d.events[0].start) < 1e-6 &&
          std::abs(d.events[0].end - 1) < 1e-6);
  for (int i = 16; i < 26; ++i)
    d.update(prob(3), i / 10.);
  d.close(2.6, false);
  require(d.events.size() == 2 && !d.events[1].complete);
  ai::Decoder single;
  single.update(prob(1), 0);
  single.update(prob(0), .1);
  single.close(1);
  require(single.events.empty());
  ai::Decoder switching;
  for (int i = 0; i < 8; ++i)
    switching.update(prob(4), i / 10.);
  for (int i = 8; i < 18; ++i)
    switching.update(prob(5), i / 10.);
  switching.close(2);
  require(switching.events.size() == 2 && switching.events[0].label == 4 &&
          switching.events[1].label == 5);
  std::vector<uint8_t> rgb(7 * 5 * 3, 255);
  auto image = ai::preprocess(rgb.data(), 7, 5, 7 * 3, 3);
  require(image.size() == 224 * 224 * 3);
  require(std::abs(image[0] - (114 / 255.f - .485f) / .229f) < 1e-6);
  bool threw = false;
  try {
    ai::softmax({});
  } catch (...) {
    threw = true;
  }
  require(threw);
  std::cout << "Event confirmation, duplicate suppression, class switch, "
               "incomplete tails and image preprocessing PASS\n";
}
