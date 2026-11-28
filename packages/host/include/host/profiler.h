// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace gea::host {

struct ProfilerFacade {
  double nowUs() const {
#ifdef ESP_PLATFORM
    return static_cast<double>(esp_timer_get_time());
#else
    using Clock = std::chrono::steady_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
#endif
  }

  // Raw CPU cycle counter. On Xtensa this is a single CCOUNT register read,
  // where nowUs() is an esp_timer_get_time() FUNCTION CALL — which forces
  // values live across it and spills registers. Instrumenting a hot leaf
  // function with nowUs() therefore measures the probe as much as the code:
  // bubble-grid's computeFisheye read 106us/call with inner nowUs() probes,
  // and four separate optimisations aimed at that number changed nothing.
  // Use this for any in-function split; keep nowUs() for wall-clock spans.
  //
  // CCOUNT is 32-bit and wraps roughly every 18s at 240MHz, so only DIFFERENCES
  // over short intervals are meaningful; a span crossing a wrap reads negative.
  double nowCycles() const {
#ifdef ESP_PLATFORM
    return static_cast<double>(esp_cpu_get_cycle_count());
#else
    using Clock = std::chrono::steady_clock;
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
#endif
  }
};

inline constexpr ProfilerFacade Profiler{};

struct ResourceTiming {
  double transferSize;
  std::string initiatorType;
  double duration;
  double startTime;
  double responseEnd;
};

struct PerformanceFacade {
  double now() const {
    static const double originUs = Profiler.nowUs();
    return (Profiler.nowUs() - originUs) / 1000.0;
  }

  std::vector<ResourceTiming> getEntriesByType(const std::string&) const {
    // The native host does not publish resource timing entries.
    return {};
  }
};

inline constexpr PerformanceFacade performance{};

}  // namespace gea::host
