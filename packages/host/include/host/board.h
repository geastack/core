// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "platform/sensors.h"

namespace gea::host {

// `Board` for app code: the board's own sensors, NaN where it has none.
// Header-only and instantiated only where an app calls it, so only boards
// that host such an app define the platform sensor functions.
struct BoardFacade {
  double temperature() const { return gea::platform::board::temperatureC(); }
  double supplyVoltage() const { return gea::platform::board::supplyVolts(); }
};

inline constexpr BoardFacade Board{};

}  // namespace gea::host
