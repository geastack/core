#include "canvas.h"
#include "pixel.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated

int main() {
  using gea::framework::graphics::Canvas;
  namespace pixel = gea::framework::graphics::pixel;
  const auto base = pixel::nativeFromRrggbbaa(0x123456ff);
  const auto color = pixel::nativeFromRrggbbaa(0xc19bffff);
  for (const int stride : {19, 24}) {
    for (const bool scrolling : {false, true}) {
      std::vector<pixel::native_t> pixels(stride * 11, base);
      Canvas canvas;
      canvas.bindPixels(pixels.data(), 19, 11, stride);
      if (scrolling)
        canvas.setScrollRegion(2, 7, 3);
      canvas.pushClip(0, 2, 19, 4);
      canvas.fillRect(-5, -5, 30, 30, color);
      for (int row = 0; row < 11; ++row) {
        const int physical = canvas.rowToPhysical(row);
        for (int column = 0; column < stride; ++column) {
          assert(pixels[physical * stride + column] ==
                 (row >= 2 && row <= 5 && column < 19 ? color : base));
        }
      }
      canvas.popClip();
      canvas.clear(base);
      assert(std::all_of(pixels.begin(), pixels.end(), [&](auto value) { return value == base; }));
      canvas.setGlobalAlpha(127);
      canvas.fillRect(0, 0, 19, 11, color);
      const auto blended = pixel::blendNative(color, base, 127);
      assert(pixels[canvas.rowToPhysical(5) * stride + 5] == blended);
      if (stride > 19)
        assert(pixels[5 * stride + 19] == base);
      canvas.fillRectOpaque(0, 0, 19, 11, color);
      assert(pixels[canvas.rowToPhysical(5) * stride + 5] == color);
      if (stride > 19)
        assert(pixels[5 * stride + 19] == base);
    }
  }
  std::puts(
      "PASS: contiguous opaque fills preserve clipping, stride padding, scroll mapping and alpha");
}
