#include "native_test_harness.h"

#include "display.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"

#include <cstdio>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int)
{
	static const std::uint8_t atlas[] = {255};
	static const Glyph glyphs[] = {{32, 0, 0, 0, 0, 20, 0, 0}, {88, 0, 0, 1, 1, 20, 0, 1}};
	static const RasterizedFontData font{9301, 20, 20, 16, -4, 2, glyphs, 1, 1, atlas};
	return &font;
}
}

using namespace gea::embedded::ui;
using namespace gea::embedded::test;

namespace {
constexpr std::uint16_t kWhite = 0xffff;
const std::uint16_t kGreen = gea::framework::graphics::pixel::fromRgb565(0x07e0);

bool pixelIs(int x, int y, std::uint16_t color, const char *label)
{
	const auto actual = displayPixelAt(x, y);
	if (actual == color) return true;
	std::fprintf(stderr, "[css_first_line] %s at (%d,%d): expected 0x%04x, got 0x%04x\n",
	             label, x, y, color, actual);
	return false;
}
}

int main()
{
	resetNativeHost();
	setNativeDisplaySize(120, 300);
	setViewportMetrics(120, 300, 1.0);
	StyleSheet::instance().registerSelectorRule(".first::first-line", "background-color", "#00ff00");

	auto root = Document::instance().createView();
	root.style().width(120); root.style().height(300); root.style().backgroundColor(kWhite);
	root.style().set(Property::FontSize, 20); root.style().set(Property::LineHeight, 20);
	root.style().set(Property::FontId, 9301);

	auto wrapped = Document::instance().createView();
	wrapped.setTagName("div");
	wrapped.style().width(40); wrapped.style().height(45);
	NodeHandle(wrapped.id()).classList().set("first");
	auto firstRun = Document::instance().createText("X");
	auto span = Document::instance().createView(); span.setTagName("span");
	auto secondRun = Document::instance().createText(" X X");
	wrapped.appendChild(firstRun); wrapped.appendChild(span); span.appendChild(secondRun);

	auto afterBreak = Document::instance().createView();
	afterBreak.setTagName("div"); afterBreak.style().width(80); afterBreak.style().height(60);
	NodeHandle(afterBreak.id()).classList().set("first");
	// Explicit BR node is used by the formatter; this also exercises nested inline
	// content which begins only after the owner has already closed its first line.
	auto before = Document::instance().createText("X");
	auto br = Document::instance().createView(); br.setTagName("br");
	auto laterSpan = Document::instance().createView(); laterSpan.setTagName("span");
	auto laterText = Document::instance().createText("X X");
	afterBreak.appendChild(before); afterBreak.appendChild(br); afterBreak.appendChild(laterSpan); laterSpan.appendChild(laterText);

	auto fixed = Document::instance().createView();
	fixed.setTagName("div"); fixed.style().width(40); fixed.style().height(60);
	NodeHandle(fixed.id()).classList().set("first");
	auto fixedText = Document::instance().createText("X X X"); fixed.appendChild(fixedText);

	auto blockOwner = Document::instance().createView();
	blockOwner.setTagName("section"); blockOwner.style().width(100); blockOwner.style().height(80);
	NodeHandle(blockOwner.id()).classList().set("first");
	auto firstBlock = Document::instance().createView(); firstBlock.setTagName("div"); firstBlock.style().height(20);
	auto firstBlockText = Document::instance().createText("X X"); firstBlock.appendChild(firstBlockText);
	auto secondBlock = Document::instance().createView(); secondBlock.setTagName("div"); secondBlock.style().height(20);
	auto secondBlockText = Document::instance().createText("X X"); secondBlock.appendChild(secondBlockText);
	blockOwner.appendChild(firstBlock); blockOwner.appendChild(secondBlock);

	root.appendChild(wrapped); root.appendChild(afterBreak); root.appendChild(fixed); root.appendChild(blockOwner);
	Document::instance().mount(root, 120, 300);

	bool ok = true;
	// First line's whitespace is green; wrapped continuation whitespace stays white.
	auto dumpFirstLine = [](const char *label, int ownerId, int textId) {
		const auto *style = rareDataFor(ownerId);
		const auto *fragment = rareDataFor(textId);
		std::printf("[css_first_line] %s owner=(%d,%d %dx%d) style=%d line=%d y=%d h=%d fragment=%d (%d,%d %dx%d) ctx=%d\n",
		            label, Tree::instance().node(ownerId).layout.x, Tree::instance().node(ownerId).layout.y,
		            Tree::instance().node(ownerId).layout.width, Tree::instance().node(ownerId).layout.height,
		            style && style->firstLineBackground.hasColor, style && style->firstLineBackground.lineValid,
		            style ? style->firstLineBackground.lineY : -1, style ? style->firstLineBackground.lineHeight : -1,
		            fragment && fragment->firstLineFragment.valid,
		            fragment ? fragment->firstLineFragment.x : -1, fragment ? fragment->firstLineFragment.y : -1,
		            fragment ? fragment->firstLineFragment.width : -1, fragment ? fragment->firstLineFragment.height : -1,
		            fragment ? fragment->firstLineFragment.contextNode : -1);
	};
	dumpFirstLine("wrapped", wrapped.id(), firstRun.id());
	dumpFirstLine("fixed", fixed.id(), fixedText.id());
	dumpFirstLine("first block child", blockOwner.id(), firstBlockText.id());
	ok &= pixelIs(Tree::instance().node(wrapped.id()).layout.x + 15,
	              Tree::instance().node(wrapped.id()).layout.y + 10, kGreen,
	              "multiple inline runs share first-line background");
	ok &= pixelIs(Tree::instance().node(fixed.id()).layout.x + 15,
	              Tree::instance().node(fixed.id()).layout.y + 10, kGreen,
	              "fixed-height first line background");
	// Nested inline content following BR must not inherit first-line painting.
	const auto &later = Tree::instance().node(laterText.id());
	const int laterSpaceX = later.layout.x + 15;
	const int laterSpaceY = later.layout.y + 10;
	ok &= pixelIs(laterSpaceX, laterSpaceY, kWhite, "nested span after BR");
	ok &= pixelIs(Tree::instance().node(firstBlock.id()).layout.x + 15,
	              Tree::instance().node(firstBlock.id()).layout.y + 10, kGreen,
	              "first in-flow block child inherits first line");
	ok &= pixelIs(Tree::instance().node(secondBlock.id()).layout.x + 15,
	              Tree::instance().node(secondBlock.id()).layout.y + 10, kWhite,
	              "later block child does not inherit first line");
	// Explicit block height must not stretch the fragment to the parent's height.
	const auto &fixedNode = Tree::instance().node(fixed.id());
	ok &= pixelIs(fixedNode.layout.x + 15, fixedNode.layout.y + 30, kWhite, "fixed-height continuation");

	NodeHandle(wrapped.id()).classList().add("unrelated");
	Document::instance().refresh(root, 120, 300);
	ok &= pixelIs(Tree::instance().node(wrapped.id()).layout.x + 15,
	              Tree::instance().node(wrapped.id()).layout.y + 10, kGreen,
	              "unrelated class recompute preserves first-line background");
	NodeHandle(wrapped.id()).classList().remove("first");
	Document::instance().refresh(root, 120, 300);
	ok &= pixelIs(Tree::instance().node(wrapped.id()).layout.x + 15,
	              Tree::instance().node(wrapped.id()).layout.y + 10, kWhite,
	              "class removal clears first-line background");
	std::printf("[css_first_line] %s\n", ok ? "ALL PASS" : "FAIL");
	return ok ? 0 : 1;
}
