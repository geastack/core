// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// gea::css — value interpolation for animated CSS properties. Pure math, no UI
// coupling (the Property→kind mapping lives in the animation layer). Header-only
// (inline). Part of the gea CSS animation engine. See docs/geaos-grand-vision.md (M1).

#include <cmath>
#include <cstdint>

namespace gea::css {

// Linear interpolation for scalar / angle property values. t is the eased
// progress; overshoot (t<0 or t>1, e.g. spring-like cubic-beziers) is allowed —
// callers clamp the *property* range where it matters.
inline double lerp(double from, double to, double t) { return from + (to - from) * t; }

namespace detail {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int roundi(double v) { return static_cast<int>(std::lround(v)); }
}  // namespace detail

// Interpolate two RGB565 colors. Interpolating the packed 16-bit int directly is
// wrong (channels bleed), so unpack R5/G6/B5, lerp each channel, clamp, repack.
inline int lerpColor565(int fromRgb565, int toRgb565, double t)
{
  const int ar = (fromRgb565 >> 11) & 0x1F;
  const int ag = (fromRgb565 >> 5) & 0x3F;
  const int ab = fromRgb565 & 0x1F;
  const int br = (toRgb565 >> 11) & 0x1F;
  const int bg = (toRgb565 >> 5) & 0x3F;
  const int bb = toRgb565 & 0x1F;
  const int r = detail::clampi(detail::roundi(lerp(ar, br, t)), 0, 0x1F);
  const int g = detail::clampi(detail::roundi(lerp(ag, bg, t)), 0, 0x3F);
  const int b = detail::clampi(detail::roundi(lerp(ab, bb, t)), 0, 0x1F);
  return (r << 11) | (g << 5) | b;
}

// Interpolate two 32-bit colors one byte at a time. Each byte is one channel in
// both RGBA8888 and ARGB8888, so the channel order does not matter here.
inline int lerpColor8888(int from, int to, double t)
{
  const auto a = static_cast<std::uint32_t>(from);
  const auto b = static_cast<std::uint32_t>(to);
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int ca = static_cast<int>((a >> shift) & 0xFFu);
    const int cb = static_cast<int>((b >> shift) & 0xFFu);
    out |= static_cast<std::uint32_t>(detail::clampi(detail::roundi(lerp(ca, cb, t)), 0, 0xFF)) << shift;
  }
  return static_cast<int>(out);
}

}  // namespace gea::css
