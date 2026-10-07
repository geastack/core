// SPDX-License-Identifier: Apache-2.0
#include "host/camera_frame.h"
#include "camera.h"

#include <array>
#include <cassert>
#include <iostream>

int main() {
  using namespace gea::host::camera_frame;
  const auto ov3640 = uploadSize(1024, 768);
  const auto sc101iot = uploadSize(1280, 720);
  assert(ov3640.width == 176 && ov3640.height == 240);
  assert(sc101iot.width == 128 && sc101iot.height == 240);
  assert(uploadSize(0, 720).width == 0);
  // Two rows, four luma samples per row, and four bytes of padding per row.
  // Rotation must ignore the row padding and preserve the UYVY pair layout.
  const std::uint8_t source[] = {
      128,16,128,64, 128,128,128,235, 0,0,0,0,
      128,235,128,128, 128,64,128,16, 0,0,0,0};
  std::array<std::uint8_t, 24> rgb{};
  assert(uyvyToRgb888Ccw90(source, sizeof(source), 4, 2, 12, rgb.data(), rgb.size(), 2, 4));
  assert(rgb[0] == 255 && rgb[3] == 0 && rgb[18] == 0 && rgb[21] == 255);
  assert(rgb[6] == 130 && rgb[9] == 56);
  assert(uyvyToRgb888Ccw90(source, sizeof(source), 4, 2, 12, rgb.data(), rgb.size(), 2, 4, true));
  assert(rgb[0] == 0 && rgb[3] == 255);
  assert(!uyvyToRgb888Ccw90(source, 23, 4, 2, 12, rgb.data(), rgb.size(), 2, 4));
  assert(!uyvyToRgb888Ccw90(source, sizeof(source), 3, 2, 12, rgb.data(), rgb.size(), 2, 4));
  assert(!uyvyToRgb888Ccw90(source, sizeof(source), 4, 2, 12, rgb.data(), 23, 2, 4));
  const std::uint8_t jpeg4[] = {255,216,255,217};
  const std::uint8_t jpeg5[] = {255,216,0,255,217};
  const std::uint8_t jpeg6[] = {255,216,0,1,255,217};
  assert(jpegDataUrl(jpeg4, 4) == "data:image/jpeg;base64,/9j/2Q==");
  assert(jpegDataUrl(jpeg5, 5) == "data:image/jpeg;base64,/9gA/9k=");
  assert(jpegDataUrl(jpeg6, 6) == "data:image/jpeg;base64,/9gAAf/Z");
  assert(jpegDataUrl(jpeg4, 3).empty());
  assert(jpegDataUrl(jpeg4, 96 * 1024 + 1).empty());
  struct Provider : gea::platform::camera::CameraFrameProvider {
    std::string pending = "frame";
    std::string takeFrameDataUrl() override { std::string result; result.swap(pending); return result; }
  } provider;
  using gea::platform::camera::Camera;
  assert(Camera::captureFrame().empty());
  Camera::setFrameProvider(&provider);
  assert(Camera::captureFrame() == "frame");
  assert(Camera::captureFrame().empty());
  Camera::setFrameProvider(nullptr);
  assert(Camera::captureFrame().empty());
  std::cout << "Camera UYVY rotation, bounded sizing, JPEG base64 and fresh-frame ownership passed\n";
}
