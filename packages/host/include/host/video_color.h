// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gea::host::media {

#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3
extern "C" void gea_i420_rgb565_row_s3(const uint8_t*, const uint8_t*, const uint8_t*,
                                     uint16_t*, unsigned);
extern "C" void gea_i420_rgb565_row_be_s3(const uint8_t*, const uint8_t*, const uint8_t*,
                                        uint16_t*, unsigned);
struct I420SecondRow { const uint8_t* y; uint16_t* output; };
extern "C" void gea_i420_rgb565_rows_s3(const uint8_t*, const uint8_t*, const uint8_t*,
                                      uint16_t*, unsigned, const I420SecondRow*);
extern "C" void gea_i420_rgb565_rows_be_s3(const uint8_t*, const uint8_t*, const uint8_t*,
                                         uint16_t*, unsigned, const I420SecondRow*);
extern "C" void gea_rgb565_swap_s3(uint16_t*, unsigned);
#endif

// Convert in place before publication, never through another full-frame image.
inline void rgb565SwapBytes(uint16_t* pixels, size_t count) {
  size_t firstScalar = 0;
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3
  if (!(uintptr_t(pixels) & 15) && count >= 8) {
    firstScalar = count & ~size_t(7);
    gea_rgb565_swap_s3(pixels, unsigned(firstScalar));
  }
#endif
  for (size_t i = firstScalar; i < count; ++i)
    pixels[i] = uint16_t((pixels[i] << 8) | (pixels[i] >> 8));
}

// Limited-range BT.601, matching the VP8 decoder's existing conversion. Each
// I420 chroma sample belongs to a 2x2 luma block. Reuse its three products for
// all four pixels, including the odd-width/height tails and padded strides.
inline void i420ToRgb565(const uint8_t* yPlane, ptrdiff_t yStride,
                        const uint8_t* uPlane, ptrdiff_t uStride,
                        const uint8_t* vPlane, ptrdiff_t vStride,
                        unsigned width, unsigned height, uint16_t* output,
                        bool panelEndian = false) {
  for (unsigned y = 0; y < height; y += 2) {
    const auto* yy = yPlane + y * yStride;
    const auto* uu = uPlane + (y / 2) * uStride;
    const auto* vv = vPlane + (y / 2) * vStride;
    auto* row = output + size_t(y) * width;
    unsigned firstScalar = 0;
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3
    // VP8 planes are aligned with padded strides. Keep the portable path for
    // arbitrary callers and short tails; never round a caller's pointer down.
    if (width >= 8 && !(uintptr_t(yy) & 7) && !(uintptr_t(uu) & 3) &&
        !(uintptr_t(vv) & 3) && !(uintptr_t(row) & 7) &&
        (y + 1 == height || (!(yStride & 7) && !(width & 3)))) {
      firstScalar = width & ~7u;
      if (y + 1 < height) {
        const I420SecondRow second{yy + yStride, row + width};
        const auto convert = panelEndian ? gea_i420_rgb565_rows_be_s3 : gea_i420_rgb565_rows_s3;
        convert(yy, uu, vv, row, firstScalar, &second);
      } else {
        const auto convert = panelEndian ? gea_i420_rgb565_row_be_s3 : gea_i420_rgb565_row_s3;
        convert(yy, uu, vv, row, firstScalar);
      }
    }
#endif
    for (unsigned x = firstScalar; x < width; x += 2) {
      const int d = int(uu[x / 2]) - 128, e = int(vv[x / 2]) - 128;
      const int red = 409 * e + 128;
      const int green = -100 * d - 208 * e + 128;
      const int blue = 516 * d + 128;
      const auto pixel = [=](uint8_t luma) {
        const int c = 298 * (int(luma) - 16);
        const int r = std::clamp((c + red) >> 8, 0, 255);
        const int g = std::clamp((c + green) >> 8, 0, 255);
        const int b = std::clamp((c + blue) >> 8, 0, 255);
        const auto value = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        return panelEndian ? uint16_t((value << 8) | (value >> 8)) : value;
      };
      row[x] = pixel(yy[x]);
      if (x + 1 < width) row[x + 1] = pixel(yy[x + 1]);
      if (y + 1 < height) {
        row[width + x] = pixel(yy[yStride + x]);
        if (x + 1 < width) row[width + x + 1] = pixel(yy[yStride + x + 1]);
      }
    }
  }
}

} // namespace gea::host::media
