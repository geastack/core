// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <memory>

namespace gea::host {

class DOMRect {
  struct Coordinates {
    double x, y, width, height;
  };

  std::shared_ptr<Coordinates> coordinates_;

public:
  static DOMRect create(double x = 0, double y = 0,
                        double width = 0, double height = 0) {
    DOMRect rect;
    rect.coordinates_ = std::make_shared<Coordinates>(Coordinates{x, y, width, height});
    return rect;
  }

  explicit operator bool() const { return static_cast<bool>(coordinates_); }
  bool operator==(const DOMRect &) const = default;

  double &x() const { return coordinates_->x; }
  double &y() const { return coordinates_->y; }
  double &width() const { return coordinates_->width; }
  double &height() const { return coordinates_->height; }

  double top() const { return minimum(y(), y() + height()); }
  double right() const { return maximum(x(), x() + width()); }
  double bottom() const { return maximum(y(), y() + height()); }
  double left() const { return minimum(x(), x() + width()); }

private:
  static double minimum(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return b < a || (a == b && std::signbit(b)) ? b : a;
  }

  static double maximum(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return b > a || (a == b && !std::signbit(b)) ? b : a;
  }
};

} // namespace gea::host
