#include "native_test_harness.h"

#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <vector>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int)
{
	static const std::uint8_t atlas[] = {
		255, 128, 0, 255,
		0, 255, 128, 0,
		0, 128, 255, 0,
		255, 0, 0, 255,
	};
	static const Glyph glyphs[] = {
		{32, 0, 0, 0, 0, 6, 0, 0},
		{88, 0, 0, 4, 4, 6, 0, 4},
	};
	static const RasterizedFontData font{9301, 4, 8, 4, -4, 2, glyphs, 4, 4, atlas};
	return &font;
}
}

using namespace gea::embedded::ui;
using namespace gea::embedded::test;
namespace pixel = gea::framework::graphics::pixel;

namespace {
constexpr int kFontId = 9301;
constexpr int kFontSize = 8;
constexpr std::uint16_t kWhite = 0xffff;
const std::uint16_t kGreen = pixel::nativeColor(0, 255, 0);
const std::uint16_t kBlue = pixel::nativeColor(0, 0, 255);
const std::uint16_t kRed = pixel::nativeColor(255, 0, 0);

bool expectPixel(int x, int y, std::uint16_t expected, const char *label)
{
	const auto actual = displayPixelAt(x, y);
	if (actual == expected) return true;
	std::fprintf(stderr, "[css_background_text] %s at (%d,%d): expected %04x got %04x\n",
	             label, x, y, expected, actual);
	return false;
}

int countColor(int x0, int y0, int x1, int y1, std::uint16_t color)
{
	int count = 0;
	for (int y = y0; y < y1; ++y)
		for (int x = x0; x < x1; ++x)
			if (displayPixelAt(x, y) == color) ++count;
	return count;
}
}

int main()
{
	resetNativeHost();
	setNativeDisplaySize(180, 120);
	setViewportMetrics(180, 120, 1.0);
	auto &sheet = StyleSheet::instance();
	sheet.clear();
	sheet.registerSelectorRule(".clip-owner", "background-clip", "text");
	sheet.registerSelectorRule(".shorthand-owner", "background", "#ff0000 text");

	auto root = Document::instance().createView();
	root.setTagName("div");
	root.style().width(180); root.style().height(120); root.style().backgroundColor(kWhite);
	root.style().set(Property::FontSize, kFontSize);
	root.style().set(Property::FontId, kFontId);
	root.style().setProperty("color", "#00ff00");

	auto clipped = Document::instance().createView();
	clipped.setTagName("div");
	clipped.classList().set("clip-owner");
	clipped.style().setProperty("position", "absolute");
	clipped.style().left(5); clipped.style().top(5);
	clipped.style().width(40); clipped.style().height(30);
	clipped.style().setProperty("background-color", "#00ff00");
	clipped.style().setProperty("color", "transparent");
	auto clippedSpan = Document::instance().createView(); clippedSpan.setTagName("span");
	clippedSpan.style().setProperty("position", "relative");
	auto clippedText = Document::instance().createText("X");
	clippedSpan.appendChild(clippedText); clipped.appendChild(clippedSpan);

	auto ordinary = Document::instance().createView();
	ordinary.setTagName("div");
	ordinary.style().setProperty("position", "absolute");
	ordinary.style().left(55); ordinary.style().top(5);
	ordinary.style().width(40); ordinary.style().height(30);
	auto ordinaryText = Document::instance().createText("X");
	ordinary.appendChild(ordinaryText);

	// This owner isolates descendant participation: opacity-zero relative text still
	// contributes ink, while absolute-positioned text must not extend the mask.
	auto positioned = Document::instance().createView();
	positioned.setTagName("div");
	positioned.classList().set("shorthand-owner");
	positioned.style().setProperty("position", "absolute");
	positioned.style().left(5); positioned.style().top(55);
	positioned.style().width(130); positioned.style().height(45);
	positioned.style().setProperty("border", "2px solid #0000ff");
	positioned.style().setProperty("color", "transparent");
	auto blank = Document::instance().createText(" ");
	positioned.appendChild(blank);
	auto relative = Document::instance().createView(); relative.setTagName("span");
	relative.style().setProperty("position", "relative");
	relative.style().left(20); relative.style().setProperty("opacity", "0");
	auto relativeText = Document::instance().createText("X");
	relative.appendChild(relativeText); positioned.appendChild(relative);
	auto absolute = Document::instance().createView(); absolute.setTagName("span");
	absolute.style().setProperty("position", "absolute");
	absolute.style().left(80); absolute.style().top(0);
	auto absoluteText = Document::instance().createText("X");
	absolute.appendChild(absoluteText); positioned.appendChild(absolute);

	root.appendChild(clipped); root.appendChild(ordinary); root.appendChild(positioned);
	Document::instance().mount(root, 180, 120);
	bool ok = true;

	// Compare all pixels, including antialiased coverage, against ordinary text
	// using the same glyph rasterizer and a constant horizontal offset.
	for (int y = 5; y < 35; ++y)
		for (int x = 5; x < 45; ++x)
			ok &= expectPixel(x, y, displayPixelAt(x + 50, y), "clipped text matches ordinary painted text");
	std::vector<int> firstInk;
	for (int y = 5; y < 35; ++y)
		for (int x = 5; x < 45; ++x)
			if (displayPixelAt(x, y) != kWhite) firstInk.push_back(y * 180 + x);
	if (firstInk.empty()) {
		std::fprintf(stderr, "[css_background_text] initial clipped glyph produced no colored pixels\n");
		ok = false;
	}

	const auto &relativeLayout = Tree::instance().node(relativeText.id()).layout;
	const auto &absoluteLayout = Tree::instance().node(absoluteText.id()).layout;
	const int relativeInk = countColor(relativeLayout.x - 1, relativeLayout.y - 1,
	                                   relativeLayout.x + 12, relativeLayout.y + 12, kRed);
	if (relativeInk == 0) {
		std::fprintf(stderr, "[css_background_text] opacity-zero relative descendant did not contribute to mask\n");
		ok = false;
	}
	for (int y = absoluteLayout.y; y < absoluteLayout.y + 12; ++y)
		for (int x = absoluteLayout.x; x < absoluteLayout.x + 12; ++x)
			ok &= expectPixel(x, y, kWhite, "absolute descendant excluded from text mask");
	ok &= expectPixel(6, 56, kBlue, "border remains painted outside text mask");

	// Retained updates must remove old ink, move the mask with text layout, and
	// recolor the owner background without leaving pixels from the prior frame.
	NodeHandle(clippedText.id()).setText(" ");
	Document::instance().refresh(root, 180, 120);
	for (const int packed : firstInk) {
		const int x = packed % 180, y = packed / 180;
		ok &= expectPixel(x, y, kWhite, "old text position cleared after content shift");
	}
	if (countColor(5, 5, 45, 35, kGreen) != 0) {
		std::fprintf(stderr, "[css_background_text] clearing text left stale clipped background\n");
		ok = false;
	}
	NodeHandle(clippedText.id()).setText("X");
	clippedSpan.style().left(12);
	Document::instance().refresh(root, 180, 120);
	if (countColor(5, 5, 45, 35, kGreen) == 0) {
		std::fprintf(stderr, "[css_background_text] moved text did not repaint clipped background\n");
		ok = false;
	}
	clipped.style().setProperty("background-color", "#0000ff");
	Document::instance().refresh(root, 180, 120);
	if (countColor(5, 5, 45, 35, kGreen) != 0 || countColor(5, 5, 45, 35, kBlue) == 0) {
		std::fprintf(stderr, "[css_background_text] owner background recolor left stale clipped pixels\n");
		ok = false;
	}
	clipped.style().setProperty("background-clip", "border-box");
	Document::instance().refresh(root, 180, 120);
	ok &= expectPixel(10, 10, kBlue, "clip toggle restores full border-box background");

	// The compositor's exact write path must remain an overwrite at alpha zero.
	auto *canvas = gea::platform::display::Display::canvas();
	if (!canvas) {
		std::fprintf(stderr, "[css_background_text] display canvas unavailable\n");
		ok = false;
	} else {
		constexpr int px = 170, py = 110;
		const auto previous = canvas->readPixelNative(px, py);
		const auto replacement = pixel::toNative(pixel::fromRgb565(0x1234));
		gea::platform::display::Display::setAlpha(0);
		canvas->writePixelNativeExact(px, py, replacement);
		ok &= canvas->readPixelNative(px, py) == replacement;
		canvas->writePixelNativeExact(px, py, previous);
		gea::platform::display::Display::setAlpha(255);
	}

	std::printf("[css_background_text] %s\n", ok ? "ALL PASS" : "FAIL");
	return ok ? 0 : 1;
}
