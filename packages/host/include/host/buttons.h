// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include <input.h>

namespace gea::host {

// The board's named physical buttons, for app code. Presses themselves arrive
// as keydown/keyup events whose `code` is the button's name; this lists the
// names a board has, so an app can show or bind them before the first press.
// Plain reads of a table filled at boot, safe from any task.
struct ButtonsFacade {
  double count() const { return static_cast<double>(gea::framework::input::buttonCount()); }
  std::string code(double index) const { return gea::framework::input::buttonCode(static_cast<int>(index)); }
  std::string key(double index) const { return gea::framework::input::buttonKey(static_cast<int>(index)); }
  double pin(double index) const { return gea::framework::input::buttonPin(static_cast<int>(index)); }
  bool pressed(const std::string &code) const
  {
    return gea::framework::input::buttonPressed(gea::framework::input::findButton(code.c_str()));
  }
};

inline constexpr ButtonsFacade Buttons{};

}  // namespace gea::host
