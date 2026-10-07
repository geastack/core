#include "display.h"
#include "graphics/font.h"
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

const RasterizedFontData *lookupFontForFamily(int, int) {
  static const std::vector<std::uint8_t> atlas(20 * 40, 255);
  static const Glyph glyphs[]{
      {48, 0, 0, 20, 40, 32, 0, 40}, {49, 0, 0, 20, 40, 32, 0, 40},
      {50, 0, 0, 20, 40, 32, 0, 40}, {51, 0, 0, 20, 40, 32, 0, 40},
      {52, 0, 0, 20, 40, 32, 0, 40}, {53, 0, 0, 20, 40, 32, 0, 40},
      {54, 0, 0, 20, 40, 32, 0, 40}, {55, 0, 0, 20, 40, 32, 0, 40},
      {56, 0, 0, 20, 40, 32, 0, 40}, {57, 0, 0, 20, 40, 32, 0, 40},
  };
  static const RasterizedFontData font{9302, 64,     64, 48, -16,
                                       10,   glyphs, 20, 40, atlas.data()};
  return &font;
}
} // namespace gea::framework::graphics::generated

static int parallelAttempts = 0;

extern "C" bool gea_render_parallel_submit(void (*)(void *, int, int), void *,
                                           int, int) {
  ++parallelAttempts;
  return false;
}

extern "C" bool gea_render_parallel_rows_submit(void (*)(void *, int, int),
                                                void *, int, int) {
  ++parallelAttempts;
  return false;
}

using namespace gea::embedded::ui;
using namespace gea::embedded::test;

static std::vector<std::uint16_t> pixels() {
  std::vector<std::uint16_t> result;
  for (int y = 0; y < 80; ++y)
    for (int x = 0; x < 80; ++x)
      result.push_back(displayPixelAt(x, y));
  return result;
}

static void testStableRollingNeighbor() {
  resetNativeHost();
  setNativeDisplaySize(466, 466);
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().width(466);
  root.style().height(466);
  root.style().backgroundColor(0x0000);
  auto panel = document.createView();
  panel.style().setProperty("position", "absolute");
  panel.style().left(300);
  panel.style().top(145);
  panel.style().width(100);
  panel.style().height(132);
  panel.style().setProperty("border-radius", "42px");
  panel.style().setProperty("overflow", "hidden");
  panel.style().backgroundColor(0x2121);
  root.appendChild(panel);
  auto digits = document.createView();
  digits.style().setProperty("position", "absolute");
  digits.style().left(8);
  digits.style().top(36);
  digits.style().width(84);
  digits.style().height(60);
  digits.style().setProperty("overflow", "hidden");
  panel.appendChild(digits);
  std::vector<gea::embedded::ui::ViewElement> moving;
  std::vector<gea::embedded::ui::ViewElement> columns;
  for (int column = 0; column < 2; ++column) {
    std::vector<gea::embedded::ui::ViewElement> labels;
    for (int row = 0; row < 12; ++row) {
      auto label = document.createView();
      label.setTagName("span");
      label.style().setProperty("position", "absolute");
      label.style().top(row * 60 - (column == 0 ? 360 : 240));
      label.style().width(42);
      label.style().height(60);
      label.style().set(Property::FontId, 9302);
      label.style().set(Property::FontSize, 64);
      label.style().setProperty("line-height", "60px");
      label.style().setProperty("text-align", "center");
      label.style().setProperty("color", "#ffffff");
      const char value[]{static_cast<char>('0' + (row + 9) % 10), 0};
      label.appendChild(document.createText(value));
      labels.push_back(label);
      if (column == 1)
        moving.push_back(label);
    }
    auto columnNode = document.createView();
    columnNode.style().setProperty("position", "absolute");
    columnNode.style().left(column * 42);
    columnNode.style().width(42);
    columnNode.style().height(60);
    columnNode.style().set(Property::Opacity, 0);
    for (auto label : labels)
      columnNode.appendChild(label);
    digits.appendChild(columnNode);
    columns.push_back(columnNode);
  }
  // Six visible clock glyphs exceed the four-slot sprite cache. Stable text
  // must survive eviction while the neighboring column rolls and warms new
  // keys.
  const int otherPositions[]{66, 108, 183, 225};
  const char *otherValues[]{"1", "0", "4", "7"};
  for (int i = 0; i < 4; ++i) {
    auto label = document.createView();
    label.style().setProperty("position", "absolute");
    label.style().left(otherPositions[i]);
    label.style().top(181);
    label.style().width(42);
    label.style().height(60);
    label.style().set(Property::FontId, 9302);
    label.style().set(Property::FontSize, 64);
    label.style().setProperty("line-height", "60px");
    label.style().setProperty("color", "#ffffff");
    label.appendChild(document.createText(otherValues[i]));
    root.appendChild(label);
  }
  document.mount(root, 466, 466);
  document.refresh(root, 466, 466);
  std::vector<std::uint16_t> hidden(466 * 466);
  assert(renderRetainedSnapshotRgb565(hidden.data(), 466, 466));
  for (int y = 181; y < 241; ++y)
    for (int x = 308; x < 392; ++x)
      assert(hidden[y * 466 + x] != 0xffff);
  // The real face mounts each column at opacity zero. Its first animation
  // reveals the whole subtree before the units column continues moving.
  const int revealOpacity[]{16, 64, 128, 192, 255};
  for (int opacity : revealOpacity) {
    for (auto column : columns)
      column.style().set(Property::Opacity, opacity);
    document.refresh(root, 466, 466);
    assert(!DisplayList::instance().staticBackdropActive());
  }
  std::vector<std::uint16_t> reference(466 * 466);
  assert(renderRetainedSnapshotRgb565(reference.data(), 466, 466));
  int stableInk = 0;
  for (int y = 181; y < 241; ++y)
    for (int x = 308; x < 350; ++x)
      stableInk += reference[y * 466 + x] == 0xffff;
  assert(stableInk > 0);
  for (int frame = 0; frame < 180; ++frame) {
    const int offset = 240 + frame % 121;
    for (int row = 0; row < 12; ++row)
      moving[row].style().top(row * 60 - offset);
    document.refresh(root, 466, 466);
    assert(!DisplayList::instance().staticBackdropActive());
    std::vector<std::uint16_t> retained(466 * 466);
    assert(renderRetainedSnapshotRgb565(retained.data(), 466, 466));
    bool stableMatches = true;
    for (int y = 181; y < 241; ++y)
      for (int x = 308; x < 350; ++x)
        if (reference[y * 466 + x] == 0xffff &&
            retained[y * 466 + x] != 0xffff) {
          if (stableMatches)
            std::fprintf(stderr,
                         "stable mismatch frame=%d offset=%d x=%d y=%d "
                         "got=%04x expected=%04x\n",
                         frame, offset, x, y, retained[y * 466 + x],
                         reference[y * 466 + x]);
          stableMatches = false;
        }
    if (!stableMatches) {
      Tree::instance().markDisplayListDirty();
      document.refresh(root, 466, 466);
      std::vector<std::uint16_t> rebuilt(466 * 466);
      assert(renderRetainedSnapshotRgb565(rebuilt.data(), 466, 466));
      std::fprintf(stderr, "full rerecord equals retained=%d\n",
                   rebuilt == retained);
      assert(stableMatches);
    }
    if (frame == 179) {
      Tree::instance().markDisplayListDirty();
      document.refresh(root, 466, 466);
      std::vector<std::uint16_t> rebuilt(466 * 466);
      assert(renderRetainedSnapshotRgb565(rebuilt.data(), 466, 466));
      for (std::size_t i = 0; i < retained.size(); ++i)
        assert((retained[i] == 0xffff) == (rebuilt[i] == 0xffff));
    }
  }
  std::puts("PASS: stable text survives rolling neighbor updates in a clipped "
            "rounded panel; snapshot glyph coverage matches full recording");
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
  resetNativeHost();
  setNativeDisplaySize(466, 466);
  auto transparentRoot = document.createView();
  transparentRoot.style().width(466);
  transparentRoot.style().height(466);
  auto mark = document.createView();
  mark.style().width(20);
  mark.style().height(20);
  mark.style().backgroundColor(0xf800);
  transparentRoot.appendChild(mark);
  auto secondMark = document.createView();
  secondMark.style().width(20);
  secondMark.style().height(20);
  secondMark.style().setProperty("position", "absolute");
  secondMark.style().left(100);
  secondMark.style().top(300);
  secondMark.style().backgroundColor(0x001f);
  transparentRoot.appendChild(secondMark);
  auto alphaNode = document.createView();
  alphaNode.style().width(1);
  alphaNode.style().height(1);
  alphaNode.style().set(Property::Opacity, 128);
  transparentRoot.appendChild(alphaNode);
  document.mount(transparentRoot, 466, 466);
  document.refresh(transparentRoot, 466, 466);
  const DisplayReplayRegion fullRegion{0, 0, 465, 465, -1};
  DisplayList::instance().replayDirectDirtyRegions(&fullRegion, 1);
  assert(parallelAttempts > 0);
  parallelAttempts = 0;
  std::vector<std::uint16_t> snapshot(466 * 466, 0x07e0);
  assert(renderRetainedSnapshotRgb565(snapshot.data(), 466, 466));
  assert(parallelAttempts == 0);
  assert(!gSnapshotRasterActive);
  assert(snapshot[10 * 466 + 10] == 0xf800);
  assert(snapshot[310 * 466 + 110] == 0x001f);
  assert(snapshot[450 * 466 + 450] == 0x0000);
  std::puts("PASS: snapshot replay stays on its bound canvas and captures "
            "geometry below the parallel split");
  std::puts("PASS: retained snapshots clear transparent areas instead of "
            "exposing prior heap pixels");
  std::puts(
      "PASS: retained opacity crosses full brightness without rebuilding; "
      "pixels match full repaint, including nested alpha and zero visibility");
  testStableRollingNeighbor();
}
