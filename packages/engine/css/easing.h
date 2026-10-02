// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// gea::css — CSS-style timing functions (easing). Header-only (inline) so the
// engine links into any TU that uses it without a build-system source-list
// change. Part of the gea CSS transitions/keyframes/animation engine.
// See docs/geaos-grand-vision.md (M1).
//
// Maps linear progress t∈[0,1] to eased progress, matching the CSS
// <easing-function> grammar: the cubic-bezier() presets (linear/ease/ease-in/
// ease-out/ease-in-out), parametric cubic-bezier(x1,y1,x2,y2), steps(n,
// jumpterm), and a custom function escape hatch.

#include <cmath>
#include <cstdint>

namespace gea::css {

enum class EasingKind : uint8_t { Linear, CubicBezier, Steps, Custom };
enum class StepPosition : uint8_t { JumpStart, JumpEnd, JumpNone, JumpBoth };

using CustomEasingFn = double (*)(double t);

namespace detail {
inline double clamp01(double t) { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }

// Cubic bezier with endpoints P0=0, P3=1 and one interior control value.
inline double bezierAxis(double c1, double c2, double t)
{
  const double u = 1.0 - t;
  return 3.0 * u * u * t * c1 + 3.0 * u * t * t * c2 + t * t * t;
}
inline double bezierAxisDeriv(double c1, double c2, double t)
{
  const double u = 1.0 - t;
  return 3.0 * u * u * c1 + 6.0 * u * t * (c2 - c1) + 3.0 * t * t * (1.0 - c2);
}
// Solve the cubic-bezier timing function for y given x (= input progress).
inline double solveCubicBezier(double x1, double y1, double x2, double y2, double x)
{
  if (x <= 0.0) return 0.0;
  if (x >= 1.0) return 1.0;
  double s = x;  // Newton-Raphson
  for (int i = 0; i < 8; ++i) {
    const double xs = bezierAxis(x1, x2, s) - x;
    if (std::fabs(xs) < 1e-6) break;
    const double d = bezierAxisDeriv(x1, x2, s);
    if (std::fabs(d) < 1e-6) break;
    s -= xs / d;
  }
  if (s < 0.0 || s > 1.0) {  // bisection fallback
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 24; ++i) {
      s = 0.5 * (lo + hi);
      const double xs = bezierAxis(x1, x2, s);
      if (std::fabs(xs - x) < 1e-6) break;
      if (xs < x)
        lo = s;
      else
        hi = s;
    }
  }
  return bezierAxis(y1, y2, s);
}
}  // namespace detail

class Easing {
public:
  constexpr Easing() = default;

  static Easing linear()
  {
    Easing e;
    e.kind_ = EasingKind::Linear;
    return e;
  }
  static Easing cubicBezier(double x1, double y1, double x2, double y2)
  {
    Easing e;
    e.kind_ = EasingKind::CubicBezier;
    e.bx1_ = x1;
    e.by1_ = y1;
    e.bx2_ = x2;
    e.by2_ = y2;
    return e;
  }
  static Easing ease() { return cubicBezier(0.25, 0.1, 0.25, 1.0); }
  static Easing easeIn() { return cubicBezier(0.42, 0.0, 1.0, 1.0); }
  static Easing easeOut() { return cubicBezier(0.0, 0.0, 0.58, 1.0); }
  static Easing easeInOut() { return cubicBezier(0.42, 0.0, 0.58, 1.0); }
  static Easing steps(int count, StepPosition position = StepPosition::JumpEnd)
  {
    Easing e;
    e.kind_ = EasingKind::Steps;
    e.steps_ = count < 1 ? 1 : count;
    e.stepPosition_ = position;
    return e;
  }
  static Easing custom(CustomEasingFn fn)
  {
    Easing e;
    e.kind_ = EasingKind::Custom;
    e.custom_ = fn;
    return e;
  }

  double operator()(double t) const { return eval(t); }
  EasingKind kind() const { return kind_; }

  double eval(double t) const
  {
    t = detail::clamp01(t);
    switch (kind_) {
    case EasingKind::Linear:
      return t;
    case EasingKind::CubicBezier: {
      // One-entry memo: the per-property tracks of one CSS animation evaluate the
      // IDENTICAL curve at the IDENTICAL t within a tick (a transform spin
      // registers rotateX/rotateY/rotateZ as separate tracks), and the
      // double-precision iterative solve is soft-float on the LX7 (~50us each).
      // Animations tick only on the frame task, so a plain static is safe.
      static double mT = -2.0, mX1 = 0, mY1 = 0, mX2 = 0, mY2 = 0, mOut = 0;
      if (t == mT && bx1_ == mX1 && by1_ == mY1 && bx2_ == mX2 && by2_ == mY2) return mOut;
      mT = t;
      mX1 = bx1_;
      mY1 = by1_;
      mX2 = bx2_;
      mY2 = by2_;
      mOut = detail::solveCubicBezier(bx1_, by1_, bx2_, by2_, t);
      return mOut;
    }
    case EasingKind::Steps: {
      const double n = static_cast<double>(steps_);
      switch (stepPosition_) {
      case StepPosition::JumpStart:
        return detail::clamp01(std::ceil(t * n) / n);
      case StepPosition::JumpEnd:
        return detail::clamp01(std::floor(t * n) / n);
      case StepPosition::JumpNone:
        return steps_ <= 1 ? 0.0 : detail::clamp01(std::floor(t * n) / (n - 1.0));
      case StepPosition::JumpBoth:
        return detail::clamp01((std::floor(t * n) + 1.0) / (n + 1.0));
      }
      return t;
    }
    case EasingKind::Custom:
      return custom_ ? detail::clamp01(custom_(t)) : t;
    }
    return t;
  }

  bool operator==(const Easing &other) const
  {
    return kind_ == other.kind_ && bx1_ == other.bx1_ && by1_ == other.by1_ &&
           bx2_ == other.bx2_ && by2_ == other.by2_ && steps_ == other.steps_ &&
           stepPosition_ == other.stepPosition_ && custom_ == other.custom_;
  }

private:
  EasingKind kind_ = EasingKind::Linear;
  double bx1_ = 0.0, by1_ = 0.0, bx2_ = 1.0, by2_ = 1.0;
  int steps_ = 1;
  StepPosition stepPosition_ = StepPosition::JumpEnd;
  CustomEasingFn custom_ = nullptr;
};

}  // namespace gea::css
