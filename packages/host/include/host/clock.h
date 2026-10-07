// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)
#include <chrono>
#else
#include <sys/time.h>
#endif

namespace gea::platform::clock {
bool setEpochMs(double timestamp);
}

namespace gea::host {

// Wall-clock access for app code.
//
// `epochMs()` returns milliseconds since the Unix epoch read from the platform
// real-time clock (`gettimeofday`). Crucially, this DOES reflect a host-set
// time — `GEADEV SETTIME` calls `settimeofday`, and the companion app issues it
// on launch — so a watch face can show real wall-clock time once the host has
// synced it.
//
// This is deliberately distinct from JavaScript `Date.now()`, whose generated
// runtime impl (runtime/date.cpp) reads `std::chrono::system_clock`, which is
// monotonic on this ESP32 build and is NOT affected by `settimeofday`. A face
// reads `epochMs()` and falls back to a boot frame clock when the value is
// implausibly small (the RTC has never been set this power cycle).
//
// Header-only by design: it links from any translation unit that includes
// gea/embedded.h without needing a new entry in the protected
// idf_component_register(SRCS ...) list.
struct ClockFacade {
  bool setEpochMs(double timestamp) const { return gea::platform::clock::setEpochMs(timestamp); }

  double epochMs() const {
#if defined(_WIN32)
    return std::chrono::duration<double, std::milli>(
      std::chrono::system_clock::now().time_since_epoch()).count();
#else
    struct timeval tv{};
    if (gettimeofday(&tv, nullptr) != 0) return 0.0;
    return static_cast<double>(tv.tv_sec) * 1000.0 +
           static_cast<double>(tv.tv_usec) / 1000.0;
#endif
  }
};

inline constexpr ClockFacade Clock{};

}  // namespace gea::host
