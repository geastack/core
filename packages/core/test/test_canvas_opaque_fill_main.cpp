#include "canvas.h"
#include "pixel.h"
#include "display.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/tree_internal.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated

namespace {
namespace pixel = gea::framework::graphics::pixel;
using namespace gea::embedded::ui;
using namespace gea::embedded::test;

void checkRect(int x, int y, int width, int height, pixel::native_t expected) {
  for (int row = y; row < y + height; ++row) {
    for (int column = x; column < x + width; ++column) {
      const auto actual = displayPixelAt(column, row);
      if (actual != expected) {
        std::fprintf(stderr, "format %d: (%d,%d) got 0x%08x, expected 0x%08x\n",
                     GEA_EMBEDDED_PIXEL_FORMAT, column, row,
                     static_cast<unsigned>(actual), static_cast<unsigned>(expected));
        std::abort();
      }
    }
  }
}

void checkGradients() {
  auto &list = DisplayList::instance();
  for (const int fill : {1, 2}) {
    // Repeatedly replay the same box to warm its cache, then change only blue.
    for (const auto color : {pixel::nativeColor(0, 0, 0), pixel::nativeColor(0, 0, 255)}) {
      for (int repeat = 0; repeat < 3; ++repeat) {
        list.clear();
        Node node{};
        node.type = NodeType::View;
        node.layout.x = 8;
        node.layout.y = 8;
        node.layout.width = 48;
        node.layout.height = 48;
        node.style.has_bg = 1;
        node.style.bg_alpha = 255;
        node.style.bg_fill = fill;
        auto &style = rstyleMut(node.style);
        style.bg_gradient_from_color = color;
        style.bg_gradient_to_color = color;
        style.bg_gradient_from_alpha = 255;
        style.bg_gradient_to_alpha = 255;
        style.bg_gradient_angle = 900;
        style.bg_radial_gradient = fill == 2;
        style.bg_radial_gradient_from_color = color;
        style.bg_radial_gradient_to_color = color;
        style.bg_radial_gradient_from_alpha = 255;
        style.bg_radial_gradient_to_alpha = 255;
        style.bg_radial_gradient_cx = 500;
        style.bg_radial_gradient_cy = 500;
        style.bg_radial_gradient_rx = 500;
        style.bg_radial_gradient_ry = 500;
        style.bg_radial_gradient_stop = 1000;
        ViewRenderer::recordBox(node);
        list.replay();
        checkRect(8, 8, 48, 48, color);
      }
    }
  }
  list.clear();
}

void checkPressAndRecolor() {
  auto &document = Document::instance();
  auto &tree = Tree::instance();
  const auto root = document.createView();
  const auto button = document.createButton();
  button.style().width(48);
  button.style().height(32);
  button.style().setProperty("border-radius", "0");
  button.style().setProperty("background-color", "#123456");
  root.appendChild(button);
  document.mount(root, 64, 64);
  const auto normal = pixel::nativeColor(0x12, 0x34, 0x56);
  const auto &layout = tree.node(button.id()).layout;
  checkRect(layout.x, layout.y, 48, 32, normal);
  tree.pointerDown(layout.x + 8, layout.y + 8);
  document.refreshMountedIfDirty();
  const auto pressed = tree.node(button.id()).computedStyle().bg_color;
  assert(pressed != normal);
  checkRect(layout.x, layout.y, 48, 32, pressed);
  tree.pointerUp();
  document.refreshMountedIfDirty();
  checkRect(layout.x, layout.y, 48, 32, normal);
  button.style().setProperty("background-color", "#1234EF");
  document.refreshMountedIfDirty();
  checkRect(layout.x, layout.y, 48, 32, pixel::nativeColor(0x12, 0x34, 0xEF));
}
}

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
  resetNativeHost();
  setNativeDisplaySize(64, 64);
  checkGradients();
  checkPressAndRecolor();
  std::printf("PASS: native color pixel checks in format %d\n", GEA_EMBEDDED_PIXEL_FORMAT);
  std::puts(
      "PASS: contiguous opaque fills preserve clipping, stride padding, scroll mapping and alpha");
}
