#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#include "gea_runtime.h"
#include "native_test_harness.h"
#include "ui/canvas_element.h"

#include <array>
#include <cassert>
#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated

int main() {
  using namespace gea::embedded::ui;
  using namespace gea::embedded::test;
  namespace pixel = gea::framework::graphics::pixel;
  resetNativeHost();
  setNativeDisplaySize(40, 40);
  gea::TypedArray<float> x0(3), y0(3), x1(3), y1(3), x2(3), y2(3);
  gea::TypedArray<std::uint32_t> colors(2);
  gea::TypedArray<std::uint16_t> order(3);
  for (int index = 0; index < 3; ++index) {
    x0.setElement(index, 2.6);
    y0.setElement(index, 2.6);
    x1.setElement(index, 30.4);
    y1.setElement(index, 2.6);
    x2.setElement(index, 2.6);
    y2.setElement(index, 30.4);
  }
  colors.setElement(0, 0xff0000ffu);
  colors.setElement(1, 0x00ff00ffu);
  order.setElement(0, 1);
  order.setElement(1, 0);
  order.setElement(2, 2);
  const auto node = Document::instance().createCanvas();
  CanvasRenderingContext2D context(node.id());
  const auto state = CanvasRenderingContext2D::stateFor(node.id());
  state->presentRecording_ = true;
  state->batchDepth_ = 1;
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, colors, order, 1);
  assert(state->presentCommandCount_ == 1);
  const auto &first = state->presentCommands_[0].triangles;
  assert(first.size() == 1);
  assert(first[0].x0 == 3 && first[0].y0 == 3);
  assert(first[0].x1 == 30 && first[0].y1 == 3);
  assert(first[0].color == pixel::nativeFromRrggbbaa(0x00ff00ffu));
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, colors, order, -1);
  const auto &bounded = state->presentCommands_[1].triangles;
  assert(bounded.size() == 2);
  assert(bounded[0].color == pixel::nativeFromRrggbbaa(0x00ff00ffu));
  assert(bounded[1].color == pixel::nativeFromRrggbbaa(0xff0000ffu));
  // The retained payload owns its coordinates, independent of reused app arrays.
  x0.setElement(1, 20);
  assert(bounded[0].x0 == 3);
  order.setElement(0, 600);
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, colors, order, 1);
  assert(state->presentCommands_[2].triangles[0].color == pixel::nativeFromRrggbbaa(0xff0000ffu));
  gea::TypedArray<double> fractionalColors(2), fractionalOrder(2);
  fractionalColors.setElement(0, 0x00feffffu + 0.75);
  fractionalColors.setElement(1, 0xff0000ffu);
  fractionalOrder.setElement(0, 0.75);
  fractionalOrder.setElement(1, 1.75);
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, fractionalColors, fractionalOrder, 2);
  const auto &fractional = state->presentCommands_[3].triangles;
  assert(fractional[0].color == pixel::nativeFromRrggbbaa(0x00feffffu));
  assert(fractional[1].color == pixel::nativeFromRrggbbaa(0xff0000ffu));
  assert(fractional[0].x0 == 3); // Coordinates still round, independently of integer roles.
  fractionalColors.setElement(0, -0.75);
  fractionalOrder.setElement(0, -0.75);
  fractionalOrder.setElement(1, -1.75);
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, fractionalColors, fractionalOrder, 2);
  assert(state->presentCommands_[4].triangles[0].color == pixel::nativeFromRrggbbaa(0));
  assert(state->presentCommands_[4].triangles[1].color == pixel::nativeFromRrggbbaa(0));
  state->presentRecording_ = false;
  state->batchDepth_ = 0;
  std::array<pixel::native_t, 40 * 40> pixels{};
  gea::framework::graphics::Canvas surface;
  surface.bindPixels(pixels.data(), 40, 40);
  state->batchCanvas_ = &surface;
  x0.setElement(1, 2.6);
  order.setElement(0, 1);
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, colors, order, 1);
  assert(pixels[10 * 40 + 10] == pixel::nativeFromRrggbbaa(0x00ff00ffu));
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, colors, order, 2);
  assert(pixels[10 * 40 + 10] == pixel::nativeFromRrggbbaa(0xff0000ffu));
  fractionalColors.setElement(0, 0x00feffffu + 0.75);
  fractionalOrder.setElement(0, 0.75);
  context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, fractionalColors, fractionalOrder, 1);
  assert(pixels[10 * 40 + 10] == pixel::nativeFromRrggbbaa(0x00feffffu));
  state->batchCanvas_ = nullptr;
  std::puts("PASS: Float32 triangle coordinates, Uint16 paint order, RGBA colors, bounded count "
            "and retained ownership");
}
