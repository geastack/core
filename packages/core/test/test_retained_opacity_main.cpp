#include "display.h"
#include "canvas.h"
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

static void testScrollFallbackPaint() {
  resetNativeHost();
  setNativeDisplaySize(80, 80);
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().setProperty("display", "block");
  root.style().width(80);
  root.style().height(80);
  auto list = document.createView();
  list.style().setProperty("display", "block");
  list.style().width(80);
  list.style().height(80);
  list.style().setProperty("overflow", "scroll");
  root.appendChild(list);
  for (const auto color : {0xf800, 0x07e0, 0x001f, 0xffff}) {
    auto row = document.createView();
    row.style().setProperty("display", "block");
    row.style().width(80);
    row.style().height(40);
    row.style().backgroundColor(color);
    list.appendChild(row);
  }
  // A real overlapping sibling must reject framebuffer scroll blitting.
  auto overlay = document.createView();
  overlay.style().setProperty("position", "absolute");
  overlay.style().left(60);
  overlay.style().top(0);
  overlay.style().width(20);
  overlay.style().height(20);
  overlay.style().backgroundColor(0xffff);
  root.appendChild(overlay);
  document.mount(root, 80, 80);
  document.refresh(root, 80, 80);
  const auto firstRow = treeState().nodes[list.id()].first_child;
  const auto secondRow = treeState().nodes[firstRow].next_sibling;
  const auto red = treeState().nodes[firstRow].computedStyle().bg_color;
  const auto green = treeState().nodes[secondRow].computedStyle().bg_color;
  assert(displayPixelAt(10, 10) == red);
  auto &tree = Tree::instance();
  auto &node = tree.node(list.id());
  node.layout.scroll_y = 40;
  node.render.dirty = 1;
  node.render.layout_dirty = 1;
  tree.markScrollDirty(list.id());
  document.refresh(root, 80, 80);
  assert(displayPixelAt(10, 10) == green);
  assert(displayPixelAt(70, 10) == 0xffff);
  const auto scrolled = pixels();
  tree.markDisplayListDirty();
  tree.node(root.id()).render.dirty = 1;
  document.refresh(root, 80, 80);
  assert(pixels() == scrolled);
  std::puts("PASS: rejected scroll blits repaint moved content and preserve overlays");
}

static void testMenuCaretLayers() {
  resetNativeHost();
  setNativeDisplaySize(80, 80);
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().width(80);
  root.style().height(80);
  root.style().backgroundColor(0x0000);
  auto caret = document.createView();
  caret.style().setProperty("position", "absolute");
  caret.style().setProperty("z-index", "1");
  caret.style().left(7);
  caret.style().top(20);
  caret.style().width(10);
  caret.style().height(40);
  caret.style().backgroundColor(0xffff);
  auto ink = document.createView();
  ink.style().width(6);
  ink.style().height(30);
  ink.style().backgroundColor(0x07e0);
  caret.appendChild(ink);
  root.appendChild(caret);
  // Match the launcher: the left overlay precedes the moving icon in JSX.
  auto icon = document.createView();
  icon.style().setProperty("position", "absolute");
  icon.style().top(10);
  icon.style().width(40);
  icon.style().height(60);
  icon.style().backgroundColor(0xf800);
  root.appendChild(icon);
  document.mount(root, 80, 80);
  document.refresh(root, 80, 80);
  for (int x : {35, 10, -15, 0, 30}) {
    icon.style().left(x);
    document.refresh(root, 80, 80);
    assert(displayPixelAt(9, 25) == treeState().nodes[ink.id()].computedStyle().bg_color);
    const auto incremental = pixels();
    Tree::instance().markDisplayListDirty();
    document.refresh(root, 80, 80);
    assert(pixels() == incremental);
  }
  std::puts("PASS: raised caret descendants stay above moving menu icons in retained and full replay");
}

static void testClassLeafGeometry() {
  resetNativeHost();
  setNativeDisplaySize(80, 80);
  auto &sheet = StyleSheet::instance();
  sheet.clear();
  sheet.registerRule("snap-dot", "position", "absolute");
  sheet.registerRule("snap-dot", "width", "8px");
  sheet.registerRule("snap-dot", "height", "8px");
  sheet.registerRule("snap-dot", "left", "30px");
  sheet.registerRule("snap-dot", "top", "30px");
  sheet.registerRule("snap-dot", "background", "#808080");
  sheet.registerRule("snap-dot", "border-radius", "50%");
  sheet.registerRule("snap-selected", "width", "14px");
  sheet.registerRule("snap-selected", "height", "14px");
  sheet.registerRule("snap-selected", "left", "27px");
  sheet.registerRule("snap-selected", "top", "27px");
  sheet.registerRule("snap-selected", "background", "#ffffff");
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().width(80);
  root.style().height(80);
  root.style().backgroundColor(0x001f);
  auto dot = document.createView();
  dot.classList().set("snap-dot");
  root.appendChild(dot);
  document.mount(root, 80, 80);
  document.refresh(root, 80, 80);
  for (const char *classes : {"snap-dot snap-selected", "snap-dot", "snap-dot snap-selected"}) {
    const auto serial = DisplayList::instance().recordSerial();
    dot.classList().set(classes);
    assert(!treeState().displayListRebuildStructural);
    document.refresh(root, 80, 80);
    assert(DisplayList::instance().recordSerial() == serial);
    const auto local = pixels();
    Tree::instance().markDisplayListDirty();
    document.refresh(root, 80, 80);
    assert(pixels() == local);
  }
  sheet.registerRule("snap-flow", "position", "relative");
  dot.classList().set("snap-dot snap-flow");
  document.refresh(root, 80, 80);
  dot.classList().set("snap-dot snap-flow snap-selected");
  assert(treeState().displayListRebuildStructural);
  std::puts("PASS: class-driven absolute dot size/position changes retain local replay; in-flow changes preserve reflow");
  sheet.clear();
}

static void testRegionScopedSimpleReplay() {
  resetNativeHost();
  setNativeDisplaySize(160, 160);
  auto &document = Document::instance();
  auto root = document.createView();
  root.style().width(160);
  root.style().height(160);
  root.style().backgroundColor(0x001f);
  auto clock = document.createView();
  clock.style().setProperty("position", "absolute");
  clock.style().width(70);
  clock.style().height(80);
  clock.style().setProperty("transform", "rotate(6deg)");
  auto glyph = document.createText("12");
  glyph.style().width(64);
  glyph.style().height(80);
  glyph.style().set(Property::FontId, 9302);
  glyph.style().set(Property::FontSize, 64);
  glyph.style().setProperty("color", "#ffffff");
  clock.appendChild(glyph);
  root.appendChild(clock);
  auto title = document.createText("12");
  title.style().setProperty("position", "absolute");
  title.style().left(12);
  title.style().top(90);
  title.style().width(64);
  title.style().height(80);
  title.style().set(Property::FontId, 9302);
  title.style().set(Property::FontSize, 64);
  title.style().setProperty("color", "#ffffff");
  root.appendChild(title);
  document.mount(root, 160, 160);
  const DisplayReplayRegion menu{7, 90, 152, 159, -1};
  const DisplayReplayRegion clockRegion{0, 0, 159, 89, -1};
  for (int opacity : {255, 128, 0, 64, 255}) {
    title.style().set(Property::Opacity, opacity);
    Tree::instance().markDisplayListDirty();
    document.refresh(root, 160, 160);
    assert(!DisplayList::instance().canReplaySimpleDirtyRegions(160, 160));
    assert(DisplayList::instance().canReplaySimpleDirtyRegions(160, 160, &menu, 1));
    assert(!DisplayList::instance().canReplaySimpleDirtyRegions(160, 160, &clockRegion, 1));
    auto *canvas = gea::platform::display::Display::canvas();
    auto *framebuffer = canvas->pixels();
    const std::vector<std::uint16_t> expected(framebuffer, framebuffer + 160 * 160);
    for (int y = 90; y < 160; y += 16) {
      const int rows = std::min(16, 160 - y);
      std::vector<std::uint16_t> transfer(160 * 146 + 160, 0x07e0);
      canvas->bindPixels(transfer.data(), 160, 160, 146);
      gea::platform::display::Display::pushClip(7, y, 146, rows);
      parallelAttempts = 0;
      DisplayList::instance().replaySimpleClippedDirtyRegion(7, y, 152, y + rows - 1, -1);
      assert(parallelAttempts == 0);
      gea::platform::display::Display::popClip();
      for (std::size_t index = 0; index < transfer.size(); ++index) {
        const int offset = static_cast<int>(index) - (y * 146 + 7);
        if (offset >= 0 && offset < rows * 146) {
          assert(transfer[index] == expected[(y + offset / 146) * 160 + 7 + offset % 146]);
        } else {
          assert(transfer[index] == 0x07e0);
        }
      }
      canvas->bindPixels(framebuffer, 160, 160);
    }
  }
  // A content-only re-record must still replay only the damaged title area.
  auto *canvas = gea::platform::display::Display::canvas();
  canvas->pixels()[10 * 160 + 10] = 0xf800;
  title.style().set(Property::Opacity, 128);
  Tree::instance().markDisplayListContentDirty();
  document.refresh(root, 160, 160);
  assert(displayPixelAt(10, 10) == 0xf800);
  // A transform can move ink into the menu despite the original layout box
  // staying above it. Eligibility must use projected command bounds.
  clock.style().setProperty("transform", "translateY(100px)");
  Tree::instance().markDisplayListDirty();
  document.refresh(root, 160, 160);
  assert(!DisplayList::instance().canReplaySimpleDirtyRegions(160, 160, &menu, 1));
  std::puts("PASS: static transformed neighbors do not block simple transfer bands; overlapping transformed ink retains general replay");
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
  assert(!DisplayList::instance().canReplaySimpleDirtyRegions(80, 80));

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
  setNativeDisplaySize(80, 80);
  auto textRoot = document.createView();
  textRoot.style().width(80);
  textRoot.style().height(80);
  textRoot.style().backgroundColor(0x001f);
  auto label = document.createText("12");
  label.style().setProperty("position", "absolute");
  label.style().width(64);
  label.style().height(80);
  label.style().set(Property::FontId, 9302);
  label.style().set(Property::FontSize, 64);
  label.style().setProperty("color", "#ffffff");
  textRoot.appendChild(label);
  document.mount(textRoot, 80, 80);
  document.refresh(textRoot, 80, 80);
  const auto serial = DisplayList::instance().recordSerial();
  std::vector<std::vector<std::uint16_t>> fadedText;
  for (int opacity : values) {
    label.style().set(Property::Opacity, opacity);
    assert(!treeState().displayListDirty);
    assert(DisplayList::instance().canReplaySimpleDirtyRegions(80, 80));
    document.refresh(textRoot, 80, 80);
    assert(DisplayList::instance().recordSerial() == serial);
    fadedText.push_back(pixels());
  }
  for (int i = 0; i < 7; ++i) {
    label.style().set(Property::Opacity, values[i]);
    Tree::instance().markDisplayListDirty();
    document.refresh(textRoot, 80, 80);
    const auto rebuiltText = pixels();
    if (rebuiltText != fadedText[i]) {
      for (std::size_t pixel = 0; pixel < rebuiltText.size(); ++pixel) {
        if (rebuiltText[pixel] != fadedText[i][pixel]) {
          std::fprintf(stderr, "text alpha=%d pixel=%zu retained=%04x rebuilt=%04x\n", values[i], pixel, fadedText[i][pixel], rebuiltText[pixel]);
          break;
        }
      }
    }
    assert(rebuiltText == fadedText[i]);
  }
  assert(fadedText[0] != fadedText[1]);
  assert(fadedText[4] != fadedText[5]);
  // Glyphs can paint outside an explicitly undersized text box. That case
  // keeps the full-record fallback rather than under-invalidating the ink.
  label.style().height(40);
  label.style().set(Property::Opacity, 255);
  Tree::instance().markDisplayListDirty();
  document.refresh(textRoot, 80, 80);
  label.style().set(Property::Opacity, 128);
  assert(treeState().displayListDirty);
  document.refresh(textRoot, 80, 80);
  const auto undersizedText = pixels();
  Tree::instance().markDisplayListDirty();
  document.refresh(textRoot, 80, 80);
  assert(pixels() == undersizedText);
  std::puts("PASS: leaf text fades retain commands and match full recording, including zero and full opacity");
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
  testRegionScopedSimpleReplay();
  testClassLeafGeometry();
  testScrollFallbackPaint();
  testMenuCaretLayers();
}
