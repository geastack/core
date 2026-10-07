#include "display.h"
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated

namespace {
using namespace gea::framework::graphics;
constexpr int kFamily = 888;
constexpr std::uint8_t ink[]{255, 255, 255, 255};
constexpr Glyph glyphs[]{
    {32, 0, 0, 0, 0, 5, 0, 0, 80},   {45, 0, 0, 2, 2, 6, 0, 2, 96},
    {46, 0, 0, 2, 2, 4, 0, 2, 64},   {65, 0, 0, 2, 2, 10, 0, 2, 160},
    {86, 0, 0, 2, 2, 10, 0, 2, 160},
};
constexpr FontKerningPair pairs[]{
    {45, 86, -16},
    {46, 46, -16},
    {65, 45, -16},
    {65, 86, -32},
};
constexpr RasterizedFontData data{
    kFamily, 10, 10, 8, 2, 5, glyphs, 2, 2, ink, 8, 4, pairs,
};
} // namespace

namespace gea::framework::graphics::generated {
const RasterizedFontData *lookupFont(int id) {
  return id == kFamily ? &data : nullptr;
}
const RasterizedFontData *lookupFontForFamily(int family, int size) {
  return family == kFamily && size == 10 ? &data : nullptr;
}
} // namespace gea::framework::graphics::generated

int main() {
  using namespace gea::embedded::ui;
  using namespace gea::embedded::test;
  resetNativeHost();
  Document::setPreferredMountSize(0, 0);
  setNativeDisplaySize(80, 80);
  setViewportMetrics(80, 80, 1.0);

  auto root = Document::instance().createView();
  root.style().width(22);
  root.style().height(80);
  root.style().display(kDisplayFlex);
  root.style().set(Property::FlexDirection, 1);
  root.style().set(Property::FontId, kFamily);
  root.style().set(Property::FontSize, 10);
  auto paragraph = Document::instance().createView();
  paragraph.setTagName("p");
  auto text = Document::instance().createText("AV AV");
  root.appendChild(paragraph);
  paragraph.appendChild(text);
  Document::instance().mount(root, 80, 80);

  const auto &node = Tree::instance().node(text.id());
  if (node.layout.width != 22 || node.layout.height != 20) {
    std::fprintf(stderr, "Kerning wrap mismatch: width=%d height=%d\n",
                 node.layout.width, node.layout.height);
    return 1;
  }
  if (displayPixelAt(8, 6) == 0 || displayPixelAt(10, 6) != 0 ||
      displayPixelAt(8, 16) == 0) {
    std::fprintf(stderr,
                 "Painted glyph positions disagree with pair-aware wrapping\n");
    return 1;
  }

  root.style().width(16);
  text.setText("A-V V");
  Document::instance().mount(root, 80, 80);
  const auto &hyphen = Tree::instance().node(text.id());
  if (hyphen.layout.width != 16 || hyphen.layout.height != 30) {
    std::fprintf(stderr,
                 "A wrapped terminal hyphen retained cross-line kerning: "
                 "width=%d height=%d\n",
                 hyphen.layout.width, hyphen.layout.height);
    return 1;
  }
  if (displayPixelAt(9, 6) == 0 || displayPixelAt(0, 16) == 0 ||
      displayPixelAt(0, 26) == 0) {
    std::fprintf(stderr,
                 "Terminal hyphen or following wrapped lines painted at "
                 "the wrong positions\n");
    return 1;
  }

  std::puts("PASS: imported pair advances agree across wrapping, terminal "
            "breaks and painted positions");
  return 0;
}
