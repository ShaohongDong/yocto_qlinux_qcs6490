// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
namespace ai {
inline uint32_t quantize(float v, float scale, int offset, int maximum) {
  if (!std::isfinite(v) || !std::isfinite(scale) || scale <= 0)
    throw std::runtime_error("Invalid quantization input");
  return uint32_t(
      std::clamp(std::round(double(v) / scale) - offset, 0.0, double(maximum)));
}
inline std::vector<float> preprocess(const uint8_t *data, int w, int h,
                                     int stride, int channels) {
  if (!data || w <= 0 || h <= 0 || channels < 3)
    throw std::runtime_error("Invalid image");
  double scale = std::min(224.0 / w, 224.0 / h);
  int nw = int(std::nearbyint(w * scale)), nh = int(std::nearbyint(h * scale)),
      left = (224 - nw) / 2, top = (224 - nh) / 2;
  const float mean[] = {.485f, .456f, .406f}, dev[] = {.229f, .224f, .225f};
  std::vector<float> out(224 * 224 * 3);
  for (int y = 0; y < 224; ++y)
    for (int x = 0; x < 224; ++x)
      for (int c = 0; c < 3; ++c)
        out[(y * 224 + x) * 3 + c] = (114 / 255.f - mean[c]) / dev[c];
  for (int y = 0; y < nh; ++y)
    for (int x = 0; x < nw; ++x) {
      double sx = std::clamp((x + .5) * w / nw - .5, 0.0, double(w - 1));
      double sy = std::clamp((y + .5) * h / nh - .5, 0.0, double(h - 1));
      int ix = int(sx), iy = int(sy), jx = std::min(ix + 1, w - 1),
          jy = std::min(iy + 1, h - 1);
      double dx = sx - ix, dy = sy - iy;
      for (int c = 0; c < 3; ++c) {
        double a = data[iy * stride + ix * channels + c] * (1 - dx) +
                   data[iy * stride + jx * channels + c] * dx;
        double b = data[jy * stride + ix * channels + c] * (1 - dx) +
                   data[jy * stride + jx * channels + c] * dx;
        float value = float(std::floor(a * (1 - dy) + b * dy + .5));
        out[((y + top) * 224 + x + left) * 3 + c] =
            (value / 255.f - mean[c]) / dev[c];
      }
    }
  return out;
}
inline std::vector<float> softmax(const std::vector<float> &x) {
  if (x.size() != 12)
    throw std::runtime_error("Expected 12 logits");
  float maximum = *std::max_element(x.begin(), x.end());
  double total = 0;
  std::vector<float> p;
  for (float v : x) {
    if (!std::isfinite(v))
      throw std::runtime_error("Nonfinite model output");
    p.push_back(std::exp(v - maximum));
    total += p.back();
  }
  for (float &v : p)
    v /= total;
  return p;
}
inline std::vector<int> rank(const std::vector<float> &p) {
  std::vector<int> r(p.size());
  std::iota(r.begin(), r.end(), 0);
  std::stable_sort(r.begin(), r.end(),
                   [&](int a, int b) { return p[a] > p[b]; });
  return r;
}
inline std::string quote(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c == '\n')
      out += "\\n";
    else if (c == '\t')
      out += "\\t";
    else if (c < 32)
      out += ' ';
    else
      out += char(c);
  }
  return out + '"';
}
} // namespace ai
