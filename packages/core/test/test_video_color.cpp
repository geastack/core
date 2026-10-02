// SPDX-License-Identifier: Apache-2.0
#include "host/video_color.h"
#include "host/video_pixels.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

static uint16_t reference(uint8_t y, uint8_t u, uint8_t v) {
  const int c = int(y) - 16, d = int(u) - 128, e = int(v) - 128;
  const int r = std::clamp((298*c + 409*e + 128) >> 8, 0, 255);
  const int g = std::clamp((298*c - 100*d - 208*e + 128) >> 8, 0, 255);
  const int b = std::clamp((298*c + 516*d + 128) >> 8, 0, 255);
  return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

int main() {
  for (unsigned count = 0; count < 35; ++count) {
    std::vector<uint16_t> values(count + 2, 0xdead);
    for (unsigned i = 0; i < count; ++i) values[i + 1] = uint16_t(i * 1877);
    gea::host::media::rgb565SwapBytes(values.data() + 1, count);
    assert(values.front() == 0xdead && values.back() == 0xdead);
    for (unsigned i = 0; i < count; ++i) {
      const auto input = uint16_t(i * 1877);
      assert(values[i + 1] == uint16_t((input << 8) | (input >> 8)));
    }
  }
  // Default pixel buffers retain their usual value initialization. Only the explicit
  // full-overwrite allocation used by the decoder bypasses a redundant memset.
  gea::host::media::VideoPixels initialized(17);
  for (const auto pixel : initialized) assert(pixel == 0);
  auto overwrite = gea::host::media::videoPixelsForOverwrite(17);
  for (unsigned i = 0; i < overwrite.size(); ++i) overwrite[i] = uint16_t(i * 137);
  initialized = std::move(overwrite);
  for (unsigned i = 0; i < initialized.size(); ++i) assert(initialized[i] == i * 137);
  initialized.resize(25);
  for (unsigned i = 17; i < initialized.size(); ++i) assert(initialized[i] == 0);
  assert(reinterpret_cast<std::uintptr_t>(initialized.data()) % 16 == 0);
  const auto* allocation = initialized.data();
  const auto snapshot = initialized;
  assert(!initialized.tryResizeForOverwrite(std::numeric_limits<std::size_t>::max()));
  assert(initialized.data() == allocation && initialized == snapshot);
  initialized = initialized; // self-assignment keeps live pixels intact
  assert(initialized == snapshot);
  auto moved = std::move(initialized);
  assert(initialized.empty() && moved == snapshot);
  moved = std::move(moved);
  assert(moved == snapshot);
  assert(moved.tryResizeForOverwrite(0) && moved.empty());
  // Exhaust all 16,777,216 Y/U/V combinations, including out-of-range luma.
  for (unsigned u = 0; u < 256; ++u) for (unsigned v = 0; v < 256; ++v)
    for (unsigned y = 0; y < 256; y += 4) {
      const uint8_t yy[]{uint8_t(y), uint8_t(y+1), uint8_t(y+2), uint8_t(y+3)};
      const uint8_t uu = u, vv = v;
      uint16_t result[4];
      gea::host::media::i420ToRgb565(yy, 2, &uu, 1, &vv, 1, 2, 2, result);
      for (unsigned i = 0; i < 4; ++i) assert(result[i] == reference(yy[i], uu, vv));
    }
  std::mt19937 random(91823);
  for (unsigned w = 1; w < 35; ++w) for (unsigned h = 1; h < 35; ++h) {
    const unsigned ys = w + 7, us = (w + 1) / 2 + 3, vs = us + 2;
    std::vector<uint8_t> y(ys*h), u(us*((h+1)/2)), v(vs*((h+1)/2));
    for (auto* plane : {&y, &u, &v}) for (auto& value : *plane) value = random();
    for (const bool panelEndian : {false, true}) {
      std::vector<uint16_t> output(w*h+2, 0xdead);
      gea::host::media::i420ToRgb565(y.data(), ys, u.data(), us, v.data(), vs, w, h, output.data()+1, panelEndian);
      assert(output.front() == 0xdead && output.back() == 0xdead);
      for (unsigned row = 0; row < h; ++row) for (unsigned x = 0; x < w; ++x) {
        const auto value = reference(y[row*ys+x], u[(row/2)*us+x/2], v[(row/2)*vs+x/2]);
        assert(output[1+row*w+x] == (panelEndian ? uint16_t((value << 8) | (value >> 8)) : value));
      }
    }
  }
  std::puts("I420 RGB565 exhaustive color, odd dimensions and strides passed");
}
