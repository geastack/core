// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <optional>

namespace gea::platform::audio {

// Worklet render time and the physical output scheduler can have different
// origins. Translate an explicit context deadline at the host boundary.
inline double physicalAudioDeadline(double when, double contextNow,
                                    double physicalNow) {
  return when > 0 ? physicalNow + when - contextNow : when;
}

struct FiniteToneTiming {
  int durationMs;
  int delayMs;
};

// The embedded oscillator backend schedules one finite tone when stop() is
// first called. It does not support replacing/cancelling an already queued
// individual voice. Further stops must never submit that voice a second time.
class FiniteOscillatorSchedule {
public:
  void start(double when, double now) {
    startTime_ = when > 0 ? when : now;
    started_ = true;
    submitted_ = false;
  }

  std::optional<FiniteToneTiming> stop(double when, double now) {
    if (submitted_)
      return std::nullopt;
    submitted_ = true;
    const double start = started_ ? startTime_ : now;
    const double end = std::max(when > 0 ? when : now, start);
    const int duration =
        static_cast<int>(std::min((end - start) * 1000.0 + 0.5, 2500.0));
    if (duration <= 0)
      return std::nullopt;
    return FiniteToneTiming{
        duration, static_cast<int>(std::max(start - now, 0.0) * 1000.0 + 0.5)};
  }

private:
  double startTime_ = 0;
  bool started_ = false;
  bool submitted_ = false;
};

} // namespace gea::platform::audio
