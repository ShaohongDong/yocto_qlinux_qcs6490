// SPDX-License-Identifier: MIT
#pragma once
// Original implementation of the documented MediaPipe Hands model geometry.
// See scripts/hand_frontend.py for the PC reference and upstream attribution.
#include "core.hpp"
#include "qnn.hpp"
#include <array>
#include <chrono>
namespace ai {
using Rect = std::array<double, 4>;
using Point = std::array<double, 3>;
inline constexpr double pi = 3.14159265358979323846;
inline double angle_normalize(double a) {
  return a - 2 * pi * std::floor((a + pi) / (2 * pi));
}
inline std::vector<uint8_t> sample_rect(const uint8_t *rgb, int w, int h,
                                        const Rect &r, int side) {
  if (!rgb || w <= 0 || h <= 0 || (side != 192 && side != 224) || r[2] <= 0)
    throw std::runtime_error("Invalid hand crop");
  for (double v : r)
    if (!std::isfinite(v))
      throw std::runtime_error("Nonfinite hand crop");
  std::vector<uint8_t> out(size_t(side) * side * 3);
  double co = std::cos(r[3]), si = std::sin(r[3]);
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x) {
      double u = (x + .5) / side - .5, v = (y + .5) / side - .5;
      double sx = r[0] + r[2] * (co * u - si * v) - .5,
             sy = r[1] + r[2] * (si * u + co * v) - .5;
      if (sx < -2 || sx > w + 1 || sy < -2 || sy > h + 1)
        continue;
      int ix = int(std::floor(sx)), iy = int(std::floor(sy));
      double dx = sx - ix, dy = sy - iy;
      auto get = [&](int xx, int yy, int c) -> double {
        return xx >= 0 && xx < w && yy >= 0 && yy < h
                   ? rgb[(yy * w + xx) * 3 + c]
                   : 0;
      };
      for (int c = 0; c < 3; ++c)
        out[(y * side + x) * 3 + c] = uint8_t(std::floor(
            ((get(ix, iy, c) * (1 - dx) + get(ix + 1, iy, c) * dx) * (1 - dy) +
             (get(ix, iy + 1, c) * (1 - dx) + get(ix + 1, iy + 1, c) * dx) *
                 dy) +
            .5));
    }
  return out;
}
inline std::vector<float> unit_rgb(const std::vector<uint8_t> &rgb) {
  std::vector<float> x;
  x.reserve(rgb.size());
  for (auto v : rgb)
    x.push_back(v / 255.f);
  return x;
}
inline const std::vector<float> &tensor(const std::vector<TensorResult> &ys,
                                        const std::string &name) {
  for (const auto &y : ys)
    if (y.spec.name == name)
      return y.values;
  throw std::runtime_error("Missing hand output: " + name);
}
struct Palm {
  Rect rect;
  double score;
};
inline std::vector<Palm> decode_palms(const std::vector<float> &boxes,
                                      const std::vector<float> &logits, int w,
                                      int h) {
  if (boxes.size() != 2016 * 18 || logits.size() != 2016)
    throw std::runtime_error("Palm output shape");
  struct Box {
    std::array<double, 18> v;
    double score;
  };
  std::vector<Box> all;
  size_t index = 0;
  for (auto spec : {std::pair<int, int>{24, 2}, {12, 6}})
    for (int y = 0; y < spec.first; ++y)
      for (int x = 0; x < spec.first; ++x)
        for (int k = 0; k < spec.second; ++k, ++index) {
          double score =
              1 / (1 + std::exp(-std::clamp(double(logits[index]), -80., 80.)));
          if (score < .5)
            continue;
          Box b{};
          b.score = score;
          for (int j = 0; j < 18; ++j)
            b.v[j] = boxes[index * 18 + j] / 192.;
          for (int j : {0, 4, 6, 8, 10, 12, 14, 16}) {
            b.v[j] += (x + .5) / spec.first;
            b.v[j + 1] += (y + .5) / spec.first;
          }
          all.push_back(b);
        }
  std::stable_sort(all.begin(), all.end(), [](const Box &a, const Box &b) {
    return a.score > b.score;
  });
  std::vector<Palm> result;
  while (!all.empty()) {
    auto first = all.front();
    std::array<double, 18> mean{};
    double weight = 0;
    std::vector<Box> remaining;
    for (const auto &b : all) {
      double iw = std::max(
          0., std::min(first.v[0] + first.v[2] / 2, b.v[0] + b.v[2] / 2) -
                  std::max(first.v[0] - first.v[2] / 2, b.v[0] - b.v[2] / 2));
      double ih = std::max(
          0., std::min(first.v[1] + first.v[3] / 2, b.v[1] + b.v[3] / 2) -
                  std::max(first.v[1] - first.v[3] / 2, b.v[1] - b.v[3] / 2));
      double overlap = iw * ih,
             area = std::max(0., first.v[2]) * std::max(0., first.v[3]) +
                    std::max(0., b.v[2]) * std::max(0., b.v[3]);
      if (overlap / std::max(area - overlap, 1e-9) > .3) {
        weight += b.score;
        for (int j = 0; j < 18; ++j)
          mean[j] += b.score * b.v[j];
      } else
        remaining.push_back(b);
    }
    if (weight == 0) {
      all.erase(all.begin());
      continue;
    }
    all = std::move(remaining);
    for (auto &v : mean)
      v /= weight;
    double scale = std::max(w, h);
    double rotation = angle_normalize(
        pi / 2 + std::atan2(mean[9] - mean[5], mean[8] - mean[4]));
    Rect r{mean[0] * scale + (w - scale) / 2 +
               .5 * mean[3] * scale * std::sin(rotation),
           mean[1] * scale + (h - scale) / 2 -
               .5 * mean[3] * scale * std::cos(rotation),
           std::max(mean[2], mean[3]) * scale * 2.6, rotation};
    if (r[2] >= 8)
      result.push_back({r, first.score});
  }
  return result;
}
inline std::array<Point, 21> project_hand(const std::vector<float> &raw,
                                          const Rect &r) {
  if (raw.size() != 63)
    throw std::runtime_error("Landmark shape");
  std::array<Point, 21> p{};
  double co = std::cos(r[3]), si = std::sin(r[3]);
  for (int i = 0; i < 21; ++i) {
    double u = raw[i * 3] / 224. - .5, v = raw[i * 3 + 1] / 224. - .5;
    p[i] = {r[0] + r[2] * (co * u - si * v), r[1] + r[2] * (si * u + co * v),
            raw[i * 3 + 2] / 224. * r[2] / .4};
  }
  return p;
}
inline Rect next_hand_rect(const std::array<Point, 21> &p) {
  double dx = (p[5][0] + p[9][0] + p[13][0]) / 3 - p[0][0],
         dy = (p[5][1] + p[9][1] + p[13][1]) / 3 - p[0][1];
  double a = angle_normalize(pi / 2 + std::atan2(dy, dx)), co = std::cos(a),
         si = std::sin(a);
  double lx = 1e100, ly = 1e100, hx = -1e100, hy = -1e100;
  for (auto q : p) {
    double x = co * q[0] + si * q[1], y = -si * q[0] + co * q[1];
    lx = std::min(lx, x);
    ly = std::min(ly, y);
    hx = std::max(hx, x);
    hy = std::max(hy, y);
  }
  double x = (lx + hx) / 2, y = (ly + hy) / 2 - .1 * (hy - ly);
  return {co * x - si * y, si * x + co * y, std::max(hx - lx, hy - ly) * 2, a};
}
struct HandObservation {
  std::vector<uint8_t> rgb;
  std::vector<float> geometry = std::vector<float>(134, 0);
  bool valid = false, reset = false;
  double palm_ms = 0, landmark_ms = 0;
};
class HandFrontend {
  Qnn palm, hand;
  Rect rect{};
  bool have_rect = false, have_previous = false;
  std::array<double, 65> previous{};
  double last_detection = -1e9, last_valid = -1e9, previous_time = -1;

public:
  explicit HandFrontend(const std::string &dir)
      : palm(dir + "/palm.bin", {1, 192, 192, 3},
             std::vector<TensorSpec>{{"Identity", {1, 2016, 18}},
                                     {"Identity_1", {1, 2016, 1}}}),
        hand(dir + "/hand.bin", {1, 224, 224, 3},
             std::vector<TensorSpec>{{"Identity", {1, 63}},
                                     {"Identity_1", {1, 1}},
                                     {"Identity_2", {1, 1}},
                                     {"Identity_3", {1, 63}}}) {}
  void reset() {
    have_rect = have_previous = false;
    last_detection = last_valid = -1e9;
    previous_time = -1;
  }
  std::string palm_profile() const { return palm.profile_json(); }
  std::string hand_profile() const { return hand.profile_json(); }
  HandObservation step(const uint8_t *rgb, int w, int h, double now) {
    using C = std::chrono::steady_clock;
    HandObservation r;
    r.rgb.resize(224 * 224 * 3);
    bool switched = false;
    if (!have_rect || now - last_detection >= .2 - 1e-6) {
      auto x = unit_rgb(sample_rect(
          rgb, w, h, {w / 2., h / 2., double(std::max(w, h)), 0}, 192));
      auto start = C::now();
      auto ys = palm.execute_all(x);
      r.palm_ms =
          std::chrono::duration<double, std::milli>(C::now() - start).count();
      auto palms =
          decode_palms(tensor(ys, "Identity"), tensor(ys, "Identity_1"), w, h);
      last_detection = now;
      if (!palms.empty()) {
        auto chosen = palms.front();
        if (have_rect)
          for (auto p : palms)
            if (std::hypot(p.rect[0] - rect[0], p.rect[1] - rect[1]) /
                        std::max(rect[2], 1.) -
                    .1 * p.score <
                std::hypot(chosen.rect[0] - rect[0], chosen.rect[1] - rect[1]) /
                        std::max(rect[2], 1.) -
                    .1 * chosen.score)
              chosen = p;
        switched = have_rect && std::hypot(chosen.rect[0] - rect[0],
                                           chosen.rect[1] - rect[1]) > rect[2];
        rect = chosen.rect;
        have_rect = true;
      }
    }
    if (have_rect) {
      r.rgb = sample_rect(rgb, w, h, rect, 224);
      auto x = unit_rgb(r.rgb);
      auto start = C::now();
      auto ys = hand.execute_all(x);
      r.landmark_ms =
          std::chrono::duration<double, std::milli>(C::now() - start).count();
      double confidence = tensor(ys, "Identity_1")[0];
      auto points = project_hand(tensor(ys, "Identity"), rect);
      r.valid = confidence >= .5;
      if (r.valid) {
        double scale = 0;
        for (int c = 0; c < 3; ++c)
          scale += std::pow(points[9][c] - points[0][c], 2);
        scale = std::max(std::sqrt(scale), 1.);
        std::array<double, 65> current{};
        for (int i = 0; i < 21; ++i)
          for (int c = 0; c < 3; ++c)
            current[i * 3 + c] = (points[i][c] - points[0][c]) / scale;
        current[63] = points[0][0] / w;
        current[64] = points[0][1] / h;
        for (int i = 0; i < 63; ++i)
          r.geometry[i] = float(current[i]);
        double dt = now - previous_time;
        if (have_previous && dt > 0 && dt <= .15 && !switched) {
          for (int i = 0; i < 63; ++i)
            r.geometry[63 + i] =
                float(std::clamp((current[i] - previous[i]) / dt, -20., 20.));
          for (int i = 0; i < 2; ++i)
            r.geometry[130 + i] = float(std::clamp(
                (current[63 + i] - previous[63 + i]) / dt, -10., 10.));
        }
        r.geometry[126] = float(current[63]);
        r.geometry[127] = float(current[64]);
        for (int c = 0; c < 2; ++c) {
          double lo = 1e100, hi = -1e100;
          for (auto p : points) {
            lo = std::min(lo, p[c]);
            hi = std::max(hi, p[c]);
          }
          r.geometry[128 + c] = float((hi - lo) / (c ? h : w));
        }
        r.geometry[132] = float(confidence);
        r.geometry[133] = 1;
        previous = current;
        have_previous = true;
        previous_time = now;
        last_valid = now;
        rect = next_hand_rect(points);
      } else {
        have_rect = have_previous = false;
        previous_time = -1;
      }
    }
    r.reset = switched || now - last_valid >= .5;
    if (r.reset) {
      have_previous = false;
      previous_time = -1;
    }
    if (!r.valid)
      std::fill(r.rgb.begin(), r.rgb.end(), 0);
    return r;
  }
};
} // namespace ai
