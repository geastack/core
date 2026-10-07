// SPDX-License-Identifier: Apache-2.0
#include "host/clock.h"
#include "power.h"

#include <cmath>

#if defined(__GNUC__)
#define GEA_WEAK_DEVICE __attribute__((weak))
#else
#define GEA_WEAK_DEVICE
#endif

namespace gea::platform::power {

GEA_WEAK_DEVICE bool Power::charging() { return false; }

GEA_WEAK_DEVICE bool Power::vibrate(double, int) { return false; }

} // namespace gea::platform::power

namespace gea::platform::clock {

GEA_WEAK_DEVICE bool setEpochMs(double timestamp) {
  if (!std::isfinite(timestamp) || timestamp < 0) {
    return false;
  }
#if defined(_WIN32)
  return false;
#else
  const auto seconds = static_cast<time_t>(timestamp / 1000);
  timeval now{seconds, static_cast<suseconds_t>((timestamp - seconds * 1000.0) * 1000)};
  return settimeofday(&now, nullptr) == 0;
#endif
}

} // namespace gea::platform::clock
