// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "backends.h"

#include <functional>
#include <utility>

namespace gea::host {

struct GpioFacade {
  bool configureOutput(double pin) const { return gea::framework::gpio::GpioBackend::configureOutput(pin); }
  bool configureInput(double pin, bool pull_up) const
  {
    return gea::framework::gpio::GpioBackend::configureInput(pin, pull_up);
  }
  bool write(double pin, bool level) const { return gea::framework::gpio::GpioBackend::write(pin, level); }
  bool read(double pin) const { return gea::framework::gpio::GpioBackend::read(pin); }

  // The handle API under the TS `Gpio` and `Digital` classes (runtime/gpio.ts).
  double open(double pin, double mode, double edges, double pollMs, std::function<void()> onEdge) const
  {
    return gea::framework::gpio::GpioBackend::open(pin, mode, edges, pollMs, std::move(onEdge));
  }
  double get(double handle) const { return gea::framework::gpio::GpioBackend::get(handle); }
  bool set(double handle, double level) const { return gea::framework::gpio::GpioBackend::set(handle, level); }
  void close(double handle) const { gea::framework::gpio::GpioBackend::close(handle); }
};

struct LedFacade {
  bool set(double pin, double r, double g, double b) const
  {
    return gea::framework::gpio::AddressableLedBackend::set(pin, r, g, b);
  }
  bool off(double pin) const { return gea::framework::gpio::AddressableLedBackend::off(pin); }
  bool attach(double pin, double count) const { return gea::framework::gpio::AddressableLedBackend::attach(pin, count); }
  bool setPixel(double index, double r, double g, double b) const
  {
    return gea::framework::gpio::AddressableLedBackend::setPixel(index, r, g, b);
  }
  bool show() const { return gea::framework::gpio::AddressableLedBackend::show(); }
  void detach() const { gea::framework::gpio::AddressableLedBackend::detach(); }
};

inline constexpr GpioFacade Gpio{};
inline constexpr LedFacade Led{};

}  // namespace gea::host

namespace gea::host::gpio {

// Set by the first GpioBackend::open, so a program that never opens a pin
// (and a platform with no GPIO backend) links none of it.
inline void (*pendingCallbacks)() = nullptr;

// Application::frame: run the edge callbacks of open pins.
inline void runCallbacks()
{
  if (pendingCallbacks) pendingCallbacks();
}

}  // namespace gea::host::gpio
