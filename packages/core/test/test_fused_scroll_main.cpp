#include "display.h"
#include "canvas.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/tree_state.h"
#include <cassert>
#include <cstdio>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
static std::vector<std::uint16_t> presented() {
  std::vector<std::uint16_t> result;
  for (int y = 0; y < 80; ++y)
    for (int x = 0; x < 80; ++x) result.push_back(presentedPixelAt(x, y));
  return result;
}
static void testScroll(bool fusedClip) {
  resetNativeHost();
  setNativeDisplaySize(80, 80);
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().setProperty("display", "block");
  root.style().width(80); root.style().height(80);
  root.style().backgroundColor(0);
  auto list = document.createView();
  list.style().setProperty("display", "block");
  list.style().width(80); list.style().height(80);
  list.style().setProperty("overflow", fusedClip ? "hidden" : "scroll");
  root.appendChild(list);
  std::vector<decltype(root)> rows;
  for (int i = 0; i < 6; ++i) {
    auto row = document.createView();
    row.style().setProperty("display", "block");
    row.style().setProperty("position", "relative");
    row.style().width(60); row.style().height(30);
    row.style().setProperty("margin-left", "10px");
    row.style().setProperty("margin-bottom", "10px");
    row.style().setProperty("border-radius", "8px");
    row.style().backgroundColor(i % 2 ? 0xf800 : 0x07e0);
    list.appendChild(row);
    rows.push_back(row);
  }
  document.mount(root, 80, 80);
  document.refresh(root, 80, 80);
  // Poison the unused framebuffer so a scroll blit cannot accidentally pass.
  auto &tree = Tree::instance();
  for (int scroll : {17, 39, 72, 93, 61, 28, 0}) {
    gea::platform::display::Display::fillRect(0, 0, 80, 80, 0x001f);
    gea::platform::display::Display::canvas()->resetDirty();
    auto &node = tree.node(list.id());
    if (fusedClip) {
      for (auto &row : rows) row.style().top(-scroll);
      tree.markDisplayListContentDirty();
      tree.node(root.id()).render.dirty = true;
    } else {
      node.layout.scroll_y = scroll;
      node.render.dirty = true; node.render.layout_dirty = true;
      tree.markScrollDirty(list.id());
    }
    document.refresh(root, 80, 80);
    if (!fusedClip) assert(tree.node(list.id()).layout.scroll_y == scroll);
    assert(tree.node(rows[0].id()).layout.y == -scroll);
    const auto moved = presented();
    assert(presentedPixelAt(0, 40) == 0);
    if (fusedClip) assert(displayPixelAt(0, 40) != presentedPixelAt(0, 40));
    tree.markDisplayListDirty();
    tree.node(root.id()).render.dirty = true;
    document.refresh(root, 80, 80);
    assert(presented() == moved);
  }
}
int main() {
  testScroll(true);
  testScroll(false);
  std::puts("PASS: fused DMA and scroll fallback match full repaint through forward/reverse motion with a stale framebuffer");
}
