// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/profiler.h"

namespace gea::host {

// The Web's `performance.now()`: milliseconds, fractional, from a monotonic
// clock, counted from an origin near the start of the process.
//
// It reads the clock `Profiler.nowUs()` reads (steady_clock, or esp_timer on
// ESP32), so a span measured with either agrees. The origin is subtracted
// because the Web defines `now()` relative to `timeOrigin` and returns a small
// number early in a program's life, while steady_clock's epoch is unspecified
// (boot time on Linux and Windows): a raw reading would be an uptime in the
// millions, which a program that logs it or keeps it in a float would notice.
// The origin is read once: during static initialization (below), or by the
// first call if that comes earlier, from another unit's initializer.
struct PerformanceFacade {
  double now() const { return (Profiler.nowUs() - origin()) / 1000.0; }

  static double origin() {
    static const double captured = Profiler.nowUs();
    return captured;
  }
};

inline constexpr PerformanceFacade performance{};

namespace detail {
// Reads the origin before main, so it is the process's start rather than the
// program's first call.
inline const double performanceOrigin = PerformanceFacade::origin();
}  // namespace detail

}  // namespace gea::host
