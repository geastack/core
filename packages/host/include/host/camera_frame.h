// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gea::host::camera_frame {

struct Size { int width = 0; int height = 0; };

// JPEG DMA works on whole 16-pixel blocks. Keep the image inside the upload
// budget while retaining the sensor aspect ratio after its mounting rotation.
inline Size uploadSize(int sourceWidth, int sourceHeight) {
  if (sourceWidth <= 0 || sourceHeight <= 0) return {};
  const double scale = std::min(320.0 / sourceHeight, 240.0 / sourceWidth);
  return {std::max(16, int(sourceHeight * scale) / 16 * 16),
          std::max(16, int(sourceWidth * scale) / 16 * 16)};
}

inline bool uyvyToRgb888Ccw90(const std::uint8_t *source, std::size_t sourceBytes,
                            int width, int height, std::size_t stride,
                            std::uint8_t *output, std::size_t outputBytes,
                            int outputWidth, int outputHeight, bool mirror = false) {
  if (!source || !output || width <= 0 || (width & 1) || height <= 0 ||
      outputWidth <= 0 || outputHeight <= 0 || outputWidth > 320 || outputHeight > 240 ||
      stride < std::size_t(width) * 2 ||
      sourceBytes / stride < std::size_t(height) ||
      outputBytes / 3 < std::size_t(outputWidth) * outputHeight) return false;
  const auto channel = [](int value) { return std::uint8_t(std::clamp(value, 0, 255)); };
  for (int y = 0; y < outputHeight; ++y) {
    const int sx = width - 1 - y * width / outputHeight;
    for (int x = 0; x < outputWidth; ++x) {
      const int sy = (mirror ? outputWidth - 1 - x : x) * height / outputWidth;
      const auto *pair = source + std::size_t(sy) * stride + std::size_t(sx / 2) * 4;
      const int c = std::max(0, int(pair[sx & 1 ? 3 : 1]) - 16);
      const int u = int(pair[0]) - 128;
      const int v = int(pair[2]) - 128;
      auto *pixel = output + (std::size_t(y) * outputWidth + x) * 3;
      pixel[0] = channel((298 * c + 409 * v + 128) >> 8);
      pixel[1] = channel((298 * c - 100 * u - 208 * v + 128) >> 8);
      pixel[2] = channel((298 * c + 516 * u + 128) >> 8);
    }
  }
  return true;
}

inline std::string jpegDataUrl(const std::uint8_t *jpeg, std::size_t size) {
  // Reject codec failure/truncation and keep every queued image bounded.
  if (!jpeg || size < 4 || size > 96 * 1024 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
      jpeg[size - 2] != 0xff || jpeg[size - 1] != 0xd9) return {};
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output = "data:image/jpeg;base64,";
  const std::size_t prefix = output.size();
  output.resize(prefix + (size + 2) / 3 * 4);
  for (std::size_t i = 0, j = prefix; i < size; i += 3, j += 4) {
    const std::uint32_t value = (std::uint32_t(jpeg[i]) << 16) |
        (i + 1 < size ? std::uint32_t(jpeg[i + 1]) << 8 : 0) |
        (i + 2 < size ? jpeg[i + 2] : 0);
    output[j] = alphabet[value >> 18];
    output[j + 1] = alphabet[(value >> 12) & 63];
    output[j + 2] = i + 1 < size ? alphabet[(value >> 6) & 63] : '=';
    output[j + 3] = i + 2 < size ? alphabet[value & 63] : '=';
  }
  return output;
}

} // namespace gea::host::camera_frame
