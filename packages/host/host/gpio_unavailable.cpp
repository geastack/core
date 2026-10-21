// SPDX-License-Identifier: Apache-2.0
//
// GpioBackend for targets with no GPIO driver (desktop, mobile, the
// simulator). Every call reports that the board did nothing, so `new Gpio()`
// and `new Digital()` throw and the static calls return false, and an app
// written for a board still links and runs here.
//
// ESP32 builds compile host/gpio.cpp over targets/esp32/gpio.cpp instead, and
// this file to nothing: both come from gea_sources.mjs there.
#ifndef ESP_PLATFORM

#include "host/backends.h"

namespace gea::framework::gpio {

bool GpioBackend::configureOutput(double) { return false; }
bool GpioBackend::configureInput(double, bool) { return false; }
bool GpioBackend::write(double, bool) { return false; }
bool GpioBackend::read(double) { return false; }
double GpioBackend::open(double, double, double, double, std::function<void()>) { return -1; }
double GpioBackend::get(double) { return 0; }
bool GpioBackend::set(double, double) { return false; }
void GpioBackend::close(double) {}
void GpioBackend::runCallbacks() {}

bool AddressableLedBackend::set(double, double, double, double) { return false; }
bool AddressableLedBackend::off(double) { return false; }
bool AddressableLedBackend::attach(double, double) { return false; }
bool AddressableLedBackend::setPixel(double, double, double, double) { return false; }
bool AddressableLedBackend::show() { return false; }
void AddressableLedBackend::detach() {}

}  // namespace gea::framework::gpio

#endif
