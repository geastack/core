#pragma once

// The board's own sensors, for gea::host::Board. A board that hosts an app
// using Board defines both; NaN reports a quantity the board cannot measure.
namespace gea::platform::board {

// Temperature in °C (the chip's die sensor on most boards).
float temperatureC();
// Supply voltage in volts.
float supplyVolts();

}  // namespace gea::platform::board
