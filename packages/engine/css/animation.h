// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// gea::css — animation descriptors: a keyframe track on one node property, with
// CSS-animation timing (duration/delay/iterations/direction/fill) and an easing
// timing function. Maps a UI Property to how its value interpolates. Header-only
// (inline). Part of the gea CSS animation engine. See docs/geaos-grand-vision.md (M1).

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "../ui/style.h"
#include "easing.h"
#include "interpolate.h"

namespace gea::css {

using gea::embedded::ui::Property;

// How a property's value interpolates.
enum class ValueKind : uint8_t { Scalar, Angle, Color };

inline ValueKind kindOf(Property p)
{
  switch (p) {
  case Property::BackgroundColor:
  case Property::ActiveBackgroundColor:
  case Property::Color:
  case Property::BorderColor:
    return ValueKind::Color;
  case Property::RotateAngle:
  case Property::TransformRotate:
  case Property::TransformRotateX:
  case Property::TransformRotateY:
    return ValueKind::Angle;
  default:
    return ValueKind::Scalar;
  }
}

enum class Direction : uint8_t { Normal, Reverse, Alternate, AlternateReverse };
enum class Fill : uint8_t { None, Forwards, Backwards, Both };

struct Keyframe {
  double offset;  // 0..1 along the animation
  double value;   // scalar/angle value; for Color, an RGB565 int stored as double
};

class KeyframeList {
public:
  static constexpr std::size_t kInlineCapacity = 3;

  KeyframeList() = default;

  KeyframeList(std::initializer_list<Keyframe> values) { *this = values; }

  KeyframeList &operator=(std::initializer_list<Keyframe> values)
  {
    clear();
    for (const Keyframe &value : values) push_back(value);
    return *this;
  }

  KeyframeList &operator=(const std::vector<Keyframe> &values)
  {
    clear();
    if (values.size() <= kInlineCapacity) {
      for (const Keyframe &value : values) inline_[count_++] = value;
    } else {
      spill_ = values;
      spilled_ = true;
    }
    return *this;
  }

  bool empty() const { return size() == 0; }
  std::size_t size() const { return spilled_ ? spill_.size() : count_; }

  void clear()
  {
    count_ = 0;
    if (spilled_) spill_.clear();
    spilled_ = false;
  }

  void push_back(const Keyframe &value)
  {
    if (!spilled_ && count_ < kInlineCapacity) {
      inline_[count_++] = value;
      return;
    }
    if (!spilled_) {
      spill_.assign(inline_, inline_ + count_);
      spilled_ = true;
    }
    spill_.push_back(value);
  }

  const Keyframe &operator[](std::size_t index) const
  {
    return spilled_ ? spill_[index] : inline_[index];
  }

  Keyframe &operator[](std::size_t index)
  {
    return spilled_ ? spill_[index] : inline_[index];
  }

  const Keyframe &front() const { return (*this)[0]; }
  const Keyframe &back() const { return (*this)[size() - 1]; }

private:
  Keyframe inline_[kInlineCapacity]{};
  std::vector<Keyframe> spill_;
  std::uint8_t count_ = 0;
  bool spilled_ = false;
};

// One animated property on one node. A 2-keyframe track is a CSS transition; an
// N-keyframe track is a @keyframes rule. Easing applies per segment (CSS default).
struct RotationAxis { double x = 0, y = 0, z = 1; };
struct RotationValue { double angle; RotationAxis axis; };

struct Animation {
  int nodeId = -1;
  Property property = Property::Opacity;
  ValueKind kind = ValueKind::Scalar;
  KeyframeList keyframes;  // >= 2, sorted by offset
  // Only an individual rotate track owns axes. Keep its angle and axis coupled
  // during interpolation; independent scalar axis tracks produce wrong turns.
  std::vector<RotationAxis> rotationAxes;
  Easing easing = Easing::ease();
  uint32_t durationMs = 300;
  uint32_t delayMs = 0;
  int iterations = 1;  // < 0 == infinite
  Direction direction = Direction::Normal;
  Fill fill = Fill::None;

  static Animation transition(int nodeId, Property property, double from, double to,
                              uint32_t durationMs, Easing easing = Easing::ease(),
                              uint32_t delayMs = 0)
  {
    Animation a;
    a.nodeId = nodeId;
    a.property = property;
    a.kind = kindOf(property);
    a.keyframes = {{0.0, from}, {1.0, to}};
    a.easing = easing;
    a.durationMs = durationMs;
    a.delayMs = delayMs;
    return a;
  }
};

inline RotationValue sampleRotation(const Animation &a, double p)
{
  const auto &kf = a.keyframes;
  std::size_t i = 0;
  while (i + 1 < kf.size() && kf[i + 1].offset <= p) ++i;
  if (i + 1 == kf.size() || p <= kf.front().offset)
    return {kf[i].value, a.rotationAxes[i]};
  const double span = kf[i + 1].offset - kf[i].offset;
  const double t = span > 0 ? (p - kf[i].offset) / span : 0;
  const double u = a.easing.kind() == EasingKind::Linear ? t : a.easing(t);
  RotationValue from{kf[i].value, a.rotationAxes[i]}, to{kf[i + 1].value, a.rotationAxes[i + 1]};
  auto normalize = [](RotationValue &r) {
    const double length = std::sqrt(r.axis.x*r.axis.x + r.axis.y*r.axis.y + r.axis.z*r.axis.z);
    if (length == 0) { r.angle = 0; r.axis = {}; }
    else { r.axis.x /= length; r.axis.y /= length; r.axis.z /= length; }
  };
  normalize(from); normalize(to);
  const bool sameAxis = std::abs(from.axis.x-to.axis.x) < 1e-9 &&
      std::abs(from.axis.y-to.axis.y) < 1e-9 && std::abs(from.axis.z-to.axis.z) < 1e-9;
  // Preserve full turns about a common axis. An identity endpoint adopts the
  // other endpoint's axis (CSS Transforms 2, interpolation of rotate3d).
  if (sameAxis || from.angle == 0 || to.angle == 0)
    return {lerp(from.angle, to.angle, u), from.angle != 0 ? from.axis : to.axis};
  constexpr double radians = 3.14159265358979323846 / 180;
  const double s0 = std::sin(from.angle*radians/2), s1 = std::sin(to.angle*radians/2);
  double q0[4] = {from.axis.x*s0, from.axis.y*s0, from.axis.z*s0, std::cos(from.angle*radians/2)};
  double q1[4] = {to.axis.x*s1, to.axis.y*s1, to.axis.z*s1, std::cos(to.angle*radians/2)};
  double dot = 0;
  for (int k = 0; k < 4; ++k) dot += q0[k]*q1[k];
  if (dot < 0) { for (double &v : q1) v = -v; dot = -dot; }
  dot = std::clamp(dot, 0.0, 1.0);
  double w0 = 1-u, w1 = u;
  if (dot < 0.999999) {
    const double theta = std::acos(dot), divisor = std::sin(theta);
    w0 = std::sin((1-u)*theta)/divisor; w1 = std::sin(u*theta)/divisor;
  }
  double q[4], norm = 0;
  for (int k = 0; k < 4; ++k) { q[k] = w0*q0[k] + w1*q1[k]; norm += q[k]*q[k]; }
  norm = std::sqrt(norm);
  if (norm == 0) return {0, {}};
  for (double &v : q) v /= norm;
  const double axisLength = std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]);
  if (axisLength < 1e-12) return {0, {}};
  return {2*std::atan2(axisLength, q[3])/radians, {q[0]/axisLength,q[1]/axisLength,q[2]/axisLength}};
}

template <typename Apply>
inline void applyRotationSample(const Animation &a, double p, Apply apply)
{
  const auto r = sampleRotation(a, p);
  apply(Property::RotateAngle, static_cast<int>(std::llround(r.angle*10)));
  apply(Property::RotateAxisX, static_cast<int>(std::llround(r.axis.x*1000000)));
  apply(Property::RotateAxisY, static_cast<int>(std::llround(r.axis.y*1000000)));
  apply(Property::RotateAxisZ, static_cast<int>(std::llround(r.axis.z*1000000)));
}

inline double sampleSegment(const Animation &a, const Keyframe &k0, const Keyframe &k1, double p)
{
  const double span = k1.offset - k0.offset;
  const double localT = span > 1e-9 ? (p - k0.offset) / span : 0.0;
  const double eased = a.easing.kind() == EasingKind::Linear ? localT : a.easing(localT);

  if (a.kind == ValueKind::Color) {
    return static_cast<double>(
        lerpColor565(static_cast<int>(std::llround(k0.value)), static_cast<int>(std::llround(k1.value)), eased));
  }
  return lerp(k0.value, k1.value, eased);
}

// Sample the property value at timeline progress p (0..1 within one iteration):
// find the bracketing keyframes, ease the in-segment progress, interpolate per
// kind. Returns the value to apply (degrees for Angle, RGB565 for Color).
inline double sampleTrack(const Animation &a, double p)
{
  const auto &kf = a.keyframes;
  const std::size_t count = kf.size();
  if (count == 0) return 0.0;
  if (count == 1) return kf[0].value;
  if (count == 2) {
    const Keyframe &k0 = kf[0];
    const Keyframe &k1 = kf[1];
    if (p <= k0.offset) return k0.value;
    if (p >= k1.offset) return k1.value;
    return sampleSegment(a, k0, k1, p);
  }
  if (p <= kf.front().offset) return kf.front().value;
  if (p >= kf.back().offset) return kf.back().value;

  std::size_t i = 0;
  if (count == 3 && p >= kf[1].offset) {
    i = 1;
  } else if (count > 3) {
    while (i + 1 < count && kf[i + 1].offset <= p) ++i;
  }

  const Keyframe &k0 = kf[i];
  const Keyframe &k1 = kf[i + 1];
  return sampleSegment(a, k0, k1, p);
}

// Timeline state of an animation at a given elapsed time (since it started).
struct Progress {
  bool active;  // should a value be applied this frame? (false before start w/o backwards fill)
  bool done;    // all iterations complete
  double p;     // iteration progress 0..1, already direction-adjusted
};

namespace detail {
// Apply direction to a raw in-iteration fraction (CSS animation-direction).
inline double directionAdjust(Direction dir, long iterationIndex, double frac)
{
  bool reverse = false;
  switch (dir) {
  case Direction::Normal:
    reverse = false;
    break;
  case Direction::Reverse:
    reverse = true;
    break;
  case Direction::Alternate:
    reverse = (iterationIndex % 2) == 1;
    break;
  case Direction::AlternateReverse:
    reverse = (iterationIndex % 2) == 0;
    break;
  }
  return reverse ? 1.0 - frac : frac;
}
}  // namespace detail

// Compute the CSS-animation progress at `elapsedMs` since start: honors delay,
// duration, iteration count (negative == infinite), and direction
// (normal/reverse/alternate). `p` is the value to feed sampleTrack().
inline Progress computeProgress(const Animation &a, double elapsedMs)
{
  const double dur = a.durationMs <= 0 ? 1.0 : static_cast<double>(a.durationMs);

  // Before the start delay: only meaningful if filling backwards.
  if (elapsedMs < static_cast<double>(a.delayMs)) {
    const bool fillBack = a.fill == Fill::Backwards || a.fill == Fill::Both;
    return {fillBack, false, detail::directionAdjust(a.direction, 0, 0.0)};
  }

  const double activeMs = elapsedMs - static_cast<double>(a.delayMs);
  const double iterF = activeMs / dur;

  // Finite and complete: hold the final value iff filling forwards.
  if (a.iterations >= 0 && iterF >= static_cast<double>(a.iterations)) {
    const bool fillFwd = a.fill == Fill::Forwards || a.fill == Fill::Both;
    const long lastIter = a.iterations > 0 ? a.iterations - 1 : 0;
    return {fillFwd, true, detail::directionAdjust(a.direction, lastIter, 1.0)};
  }

  const long idx = static_cast<long>(iterF);
  const double frac = iterF - static_cast<double>(idx);
  return {true, false, detail::directionAdjust(a.direction, idx, frac)};
}

}  // namespace gea::css
