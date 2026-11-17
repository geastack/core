#pragma once

// PCA9557: 8-bit I2C I/O expander, addresses 0x18-0x1f. Its register map is
// the TCA9554/PCA9554 one (0 input, 1 output, 2 polarity, 3 configuration with
// 1 = input), so it shares that driver. Two differences matter and both are
// covered by it: the polarity register resets to 0xf0 (IO4-IO7 read inverted),
// which Driver::configure clears to 0, and IO0 is open-drain, so a board that
// drives a push-pull line from IO0 needs its own pull-up.

#include "expanders/tca9554/tca9554.h"

namespace gea::chips::pca9557 {

using Bus = gea::chips::tca9554::Bus;
using Driver = gea::chips::tca9554::Driver;

}  // namespace gea::chips::pca9557
