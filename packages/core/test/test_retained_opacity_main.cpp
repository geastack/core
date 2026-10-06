#include "display.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_state.h"
#include <cassert>
#include <cstdio>
#include <vector>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated
namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated

using namespace gea::embedded::ui;
using namespace gea::embedded::test;

static std::vector<std::uint16_t> pixels() {
  std::vector<std::uint16_t> result;
  for (int y = 0; y < 80; ++y)
    for (int x = 0; x < 80; ++x)
      result.push_back(displayPixelAt(x, y));
  return result;
}

int main() {
  resetNativeHost();
  setNativeDisplaySize(80, 80);
  StyleSheet::instance().clear();
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().width(80);
  root.style().height(80);
  root.style().backgroundColor(0x001f);
  auto parent = document.createView();
  parent.style().width(60);
  parent.style().height(60);
  parent.style().set(Property::Opacity, 180);
  auto lamp = document.createView();
  lamp.style().width(40);
  lamp.style().height(40);
  lamp.style().backgroundColor(0xf800);
  lamp.style().set(Property::Opacity, 64);
  parent.appendChild(lamp);
  root.appendChild(parent);
  document.mount(root, 80, 80);
  document.refresh(root, 80, 80);

  const int values[]{255, 128, 255, 64, 0, 255, 96};
  std::vector<std::vector<std::uint16_t>> retained;
  for (int i = 0; i < 7; ++i) {
    lamp.style().set(Property::Opacity, values[i]);
    // An existing alpha scope remains valid at full brightness. Crossing
    // 255 must not rebuild the entire display list on each animation tick.
    if (i < 4)
      assert(!treeState().displayListDirty);
    document.refresh(root, 80, 80);
    retained.push_back(pixels());
  }
  for (int i = 0; i < 7; ++i) {
    lamp.style().set(Property::Opacity, values[i]);
    Tree::instance().markDisplayListDirty();
    document.refresh(root, 80, 80);
    assert(pixels() == retained[i]);
  }
  assert(retained[0] != retained[1]);
  assert(retained[3] != retained[4]);
  std::puts(
      "PASS: retained opacity crosses full brightness without rebuilding; "
      "pixels match full repaint, including nested alpha and zero visibility");
}
