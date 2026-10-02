#include "pixel.h"
#include <array>
#include <cassert>
#include <cstdio>
namespace pixel = gea::framework::graphics::pixel;

int main() {
  alignas(16) std::array<pixel::native_t, 416> source{}, output{};
  for (unsigned i = 0; i < source.size(); ++i) source[i] = pixel::native_t(i * 197u + 31u);
  unsigned cases = 0;
  for (unsigned sourceOffset = 0; sourceOffset < 8; ++sourceOffset)
    for (unsigned outputOffset = 0; outputOffset < 8; ++outputOffset)
      for (unsigned count = 0; count <= 396; ++count) {
        output.fill(pixel::native_t(0xa55a));
        pixel::copyNative(output.data() + outputOffset, source.data() + sourceOffset, count);
        for (unsigned i = 0; i < output.size(); ++i) {
          const auto expected = i >= outputOffset && i < outputOffset + count
              ? source[sourceOffset + i - outputOffset] : pixel::native_t(0xa55a);
          assert(output[i] == expected);
        }
        ++cases;
      }
  std::printf("Native pixel copy: %u exact cases with guards\n", cases);
}
