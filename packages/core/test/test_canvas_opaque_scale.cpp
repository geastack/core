#include "canvas.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
  using namespace gea::framework::graphics;
  constexpr int width = 43, height = 47;
  std::array<pixel::native_t, width * height> actual{}, reference{};
  Canvas fast, general;
  fast.bindPixels(actual.data(), width, height);
  general.bindPixels(reference.data(), width, height);
  uint32_t random = 42;
  auto next = [&] { random = random * 1664525 + 1013904223; return random; };
  for (unsigned sample = 0; sample < 256; ++sample) {
    const int sw = 1 + next() % 17, sh = 1 + next() % 17;
    const int dw = sw + 1 + next() % 30, dh = sh + 1 + next() % 30;
    const int dx = int(next() % 13) - 6, dy = int(next() % 13) - 6;
    std::vector<pixel::native_t> input(sw * sh);
    std::vector<uint8_t> alpha(input.size(), 255);
    for (auto& p : input) p = pixel::fromRgb565(next());
    for (const uint8_t opacity : {0, 77, 255}) {
      actual.fill(pixel::nativeColor(12, 34, 56));
      reference = actual;
      fast.setGlobalAlpha(opacity);
      general.setGlobalAlpha(opacity);
      fast.pushClip(2, 3, width - 5, height - 7);
      general.pushClip(2, 3, width - 5, height - 7);
      fast.drawImage(input.data(), nullptr, sw, sh, dx, dy, dw, dh);
      // An explicit opaque alpha plane forces the established general filter.
      general.drawImage(input.data(), alpha.data(), sw, sh, dx, dy, dw, dh);
      fast.popClip();
      general.popClip();
      for (size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != reference[i]) {
          std::fprintf(stderr, "Opaque scale differs: sample=%u pixel=%zu alpha=%u actual=%04x expected=%04x\n",
                       sample, i, opacity, unsigned(actual[i]), unsigned(reference[i]));
          return 1;
        }
      }
    }
  }
  std::puts("Opaque bilinear scaling matches general filtering across dimensions, clipping and opacity");
}
