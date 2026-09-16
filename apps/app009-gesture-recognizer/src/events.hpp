// SPDX-License-Identifier: MIT
#pragma once
#include "core.hpp"
#include <functional>
namespace ai {
inline const std::vector<std::string> labels = {"No target action",
                                                "Click one finger",
                                                "Click two fingers",
                                                "Throw up",
                                                "Throw down",
                                                "Throw left",
                                                "Throw right",
                                                "Open twice",
                                                "Double click one finger",
                                                "Double click two fingers",
                                                "Zoom in",
                                                "Zoom out"};
class Sampler {
  double next = -1, period = .1;

public:
  explicit Sampler(double hz = 10) : period(1 / hz) {
    if (!std::isfinite(hz) || hz < 1 || hz > 60)
      throw std::runtime_error("Invalid sampling rate");
  }
  void reset() { next = -1; }
  bool take(double t) {
    if (!std::isfinite(t) || t < 0)
      throw std::runtime_error("Invalid sample time");
    if (next < 0)
      next = t;
    if (t + 1e-6 < next)
      return false;
    next += period;
    if (next <= t + 1e-6)
      next += (std::floor((t + 1e-6 - next) / period) + 1) * period;
    return true;
  }
};
struct Event {
  int label;
  double start, end, confirmed, confidence;
  bool complete;
};
class Decoder {
  double threshold, release, start = 0, last = 0, score = 0, pending_start = 0;
  int active = 0, pending = 0, count = 0;
  double period, minimum, stable_seconds;

public:
  std::vector<Event> events;
  Decoder(double t = .6, double r = .6, double p = .1, double min_duration = .2,
          double stable_duration = 0)
      : threshold(t), release(r), period(p), minimum(min_duration),
        stable_seconds(stable_duration) {
    if (!std::isfinite(t) || t <= 0 || t >= 1 || !std::isfinite(r) || r < .1 ||
        r > 3 || !std::isfinite(p) || p <= 0 || p > 1 ||
        !std::isfinite(min_duration) || min_duration < 0 ||
        !std::isfinite(stable_duration) || stable_duration < 0 ||
        stable_duration > 1)
      throw std::runtime_error("Invalid event settings");
  }
  void close(double now, bool complete = true) {
    if (active && last - start >= minimum - 1e-6)
      events.push_back({active, start, last + period, now, score, complete});
    active = 0;
    pending = 0;
    count = 0;
  }
  void update(const std::vector<float> &p, double now) {
    if (p.size() != 12 || !std::isfinite(now))
      throw std::runtime_error("Invalid event input");
    int label = int(std::max_element(p.begin(), p.end()) - p.begin());
    if (!label || p[label] < threshold)
      label = 0;
    if (label && label == active) {
      last = now;
      score = std::max(score, double(p[label]));
      pending = 0;
      count = 0;
    } else if (label) {
      if (label == pending)
        ++count;
      else {
        pending = label;
        pending_start = now;
        count = 1;
      }
      if (stable_seconds > 0
              ? now - pending_start + period >= stable_seconds - 1e-6
              : count >= 3) {
        if (active)
          close(now);
        active = label;
        start = pending_start;
        last = now;
        score = p[label];
        pending = 0;
        count = 0;
      }
    } else {
      pending = 0;
      count = 0;
      if (active && now - last >= release - 1e-6)
        close(now);
    }
  }
};
} // namespace ai
