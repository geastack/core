#include "native_test_harness.h"
#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include <array>
#include <vector>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int)
{
	static const std::uint8_t atlas[]{255, 128, 0, 255, 0, 255, 128, 0, 0, 128, 255, 0, 255, 0, 0, 255};
	static const Glyph glyphs[]{{32, 0, 0, 0, 0, 6, 0, 0}, {88, 0, 0, 4, 4, 6, 0, 4}};
	static const RasterizedFontData font{9301, 4, 8, 4, -4, 2, glyphs, 4, 4, atlas};
	return &font;
}
}

using namespace gea::embedded::test;
using namespace gea::embedded::ui;
using gea::platform::display::Display;
namespace pixel = gea::framework::graphics::pixel;

static void fingerprint(const char *label)
{
	std::uint64_t hash = 1469598103934665603ull;
	int painted = 0;
	for (int y = 0; y < 120; ++y) for (int x = 0; x < 120; ++x) {
		const auto color = displayPixelAt(x, y);
		hash = (hash ^ color) * 1099511628211ull;
		painted += color != 0;
	}
	assert(painted > 100);
	std::printf("%s %016llx\n", label, static_cast<unsigned long long>(hash));
}


static void triangleOcclusionPixels()
{
	using gea::framework::graphics::Canvas;
	using gea::framework::graphics::TriangleEntry;
	// The first two dimensions execute the accelerated path; the latter two
	// cross its row/column limits. Compare against the ordinary painter path.
	const int widths[]{96, 256, 96, 257};
	const int heights[]{40, 48, 49, 40};
	for (int size = 0; size < 4; ++size) for (int clipped = 0; clipped < 2; ++clipped) {
		const int width = widths[size], height = heights[size];
		std::vector<pixel::native_t> actual(width * height), expected(width * height);
		Canvas batch, painter;
		batch.bindPixels(actual.data(), width, height);
		painter.bindPixels(expected.data(), width, height);
		const auto background = pixel::nativeColor(18, 24, 30);
		batch.clear(background); painter.clear(background);
		if (clipped) { batch.pushClip(7, 5, width - 19, height - 11); painter.pushClip(7, 5, width - 19, height - 11); }
		const int ox = 13, oy = -7;
		std::array<TriangleEntry, 32> triangles{};
		std::uint32_t seed = 0xB041u;
		auto random = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
		for (int i = 0; i < static_cast<int>(triangles.size()); ++i) {
			auto &t = triangles[i];
			t.x0 = ox + static_cast<int>(random() % (width + 40)) - 20;
			t.y0 = oy + static_cast<int>(random() % (height + 20)) - 10;
			t.x1 = ox + static_cast<int>(random() % (width + 40)) - 20;
			t.y1 = oy + static_cast<int>(random() % (height + 20)) - 10;
			t.x2 = ox + static_cast<int>(random() % (width + 40)) - 20;
			t.y2 = oy + static_cast<int>(random() % (height + 20)) - 10;
			t.color = pixel::nativeColor(40 + i * 6, 240 - i * 5, 60 + i * 4);
			if (i % 7 == 0) t.y1 = t.y2 = t.y0;  // horizontal degenerates
			if (i % 11 == 0) t.x1 = t.x2 = t.x0; // vertical degenerates
			painter.fillTriangleOpaque(t.x0 - ox, t.y0 - oy, t.x1 - ox, t.y1 - oy, t.x2 - ox, t.y2 - oy, t.color);
		}
		batch.fillTrianglesOpaqueOccluded(triangles.data(), triangles.size(), ox, oy);
		assert(actual == expected);
		std::uint64_t hash = 1469598103934665603ull;
		int painted = 0;
		for (const auto color : actual) { hash = (hash ^ color) * 1099511628211ull; painted += color != background; }
		assert(painted > 100);
		std::printf("triangles-%d-%d %016llx\n", size, clipped, static_cast<unsigned long long>(hash));
	}
}

static void allCircleCacheSizes()
{
	using gea::framework::graphics::Canvas;
	constexpr int width = 144, height = 144, guardSize = 16;
	const auto guard = pixel::nativeColor(17, 93, 151);
	const auto background = pixel::nativeColor(18, 24, 30);
	std::vector<pixel::native_t> pixels(width * height + guardSize * 2, guard);
	Canvas canvas;
	canvas.bindPixels(pixels.data() + guardSize, width, height);
	Display::setAA(0);
	std::uint64_t hash = 1469598103934665603ull;
	// Ascending cold fills, then descending warm hits after every neighboring
	// table has been populated. Radius 64 exercises the uncached fallback.
	for (int pass = 0; pass < 4; ++pass) {
		canvas.setGlobalAlpha(pass & 1 ? 140 : 255);
		if (pass & 1) canvas.pushClip(13, 9, width - 29, height - 23);
		for (int ordinal = 1; ordinal <= 64; ++ordinal) {
			const int radius = pass >= 2 ? 65 - ordinal : ordinal;
			canvas.clear(background);
			canvas.fillCircle(70, 69, radius, pixel::nativeColor(250, 30, 45));
			if (radius <= 16) canvas.fillRoundedRect(23, 81, radius * 2, radius * 2, radius, radius, radius, radius, pixel::nativeColor(25, 240, 80));
			for (int i = guardSize; i < guardSize + width * height; ++i)
				hash = (hash ^ pixels[i]) * 1099511628211ull;
			for (int i = 0; i < guardSize; ++i) {
				assert(pixels[i] == guard);
				assert(pixels[pixels.size() - 1 - i] == guard);
			}
		}
		if (pass & 1) canvas.popClip();
	}
	std::printf("all-circle-cache-sizes %016llx\n", static_cast<unsigned long long>(hash));
}

int main(int argc, char **argv)
{
	resetNativeHost();
	setNativeDisplaySize(120, 120);
	setViewportMetrics(120, 120, 1.0);
	StyleSheet::instance().clear();
	Display::clearNoFlush();
	auto *canvas = Display::canvas();
	assert(canvas);
	const bool cssOnly = argc == 2 && std::strcmp(argv[1], "--css-storage") == 0;
	if (!cssOnly) {
		triangleOcclusionPixels();
		allCircleCacheSizes();
		// Replay identical native commands, bypassing CSS fields whose absence
		// is separately proven by whole-source analysis. This isolates scratch
		// selection from intentional removal of unused gradient style storage.
		for (int scenario = 0; scenario < 8; ++scenario) {
			canvas->clear(pixel::nativeColor(18, 24, 30));
			DisplayList::instance().clear();
			auto *command = DisplayList::instance().append(); assert(command);
			*command = DisplayCommand{};
			command->type = DisplayCommandType::FillTransformedLinearGradient;
			command->bx = 10; command->by = 10; command->bw = 100; command->bh = 100;
			auto &g = command->transformedGradient;
			g.x0 = 24; g.y0 = 10; g.x1 = 110; g.y1 = 30;
			g.x2 = 86; g.y2 = 110; g.x3 = 10; g.y3 = 84;
			g.fx0 = g.x0 * 8; g.fy0 = g.y0 * 8; g.fx1 = g.x1 * 8; g.fy1 = g.y1 * 8;
			g.fx2 = g.x2 * 8; g.fy2 = g.y2 * 8; g.fx3 = g.x3 * 8; g.fy3 = g.y3 * 8;
			g.lx = 10; g.ly = 10; g.lw = 90; g.lh = 90;
			g.fromColor = pixel::nativeColor(255, 32, 64);
			g.midColor = pixel::nativeColor(20, 230, 70);
			g.toColor = pixel::nativeColor(32, 64, 255);
			g.midStop = 430; g.toStop = 1000; g.angle = 1350;
			g.fromAlpha = scenario & 1 ? 128 : 255;
			g.midAlpha = scenario & 2 ? 80 : g.fromAlpha;
			g.toAlpha = scenario & 2 ? 200 : g.fromAlpha;
			g.hasMid = (scenario >> 2) & 1;
			DisplayList::instance().replay();
			assert(displayPixelAt(60, 60) != pixel::nativeColor(18, 24, 30));
			fingerprint("gradient-scratch");
		}
		if (argc == 2 && std::strcmp(argv[1], "--gradient-scratch") == 0) return 0;
	}
	// Exercise all three circle cache paths, including the rounded CSS circle
	// shortcut whose old cache-miss branch simply returned without painting.
	for (int aa : {0, 4}) {
		Display::setAA(aa);
		canvas->clear(0);
		canvas->fillCircle(25, 25, 12, pixel::nativeColor(255, 20, 20));
		canvas->fillRoundedRect(45, 10, 24, 24, 12, 12, 12, 12, pixel::nativeColor(20, 255, 20));
		canvas->setGlobalAlpha(140);
		canvas->fillRoundedRect(10, 50, 60, 35, 9, 9, 9, 9, pixel::nativeColor(20, 20, 255));
		canvas->setGlobalAlpha(255);
		const std::int16_t xs[]{75, 88}, ys[]{22, 58};
		const pixel::native_t colors[]{pixel::nativeColor(200, 90, 40), pixel::nativeColor(40, 90, 200)};
		canvas->fillRoundedRectBoxesRgb565(xs, ys, 2, 18, 18, 9, 9, 9, 9, colors);
		assert(displayPixelAt(57, 22) != 0);
		fingerprint(aa ? "shapes-aa" : "shapes");
	}

	// At/above every automatically bounded cache edge. The same commands must
	// retain exact pixels when their spans use the integer fallback. Radius 8
	// can use the circle-like box shortcut through size 19, not just size 17.
	for (int size : {16, 17, 18, 19, 20, 32}) {
		for (int alpha : {255, 140}) {
			Display::setAA(0);
			canvas->clear(pixel::nativeColor(18, 24, 30));
			canvas->setGlobalAlpha(alpha);
			canvas->fillCircle(22, 22, size / 2, pixel::nativeColor(255, 20, 20));
			canvas->fillRoundedRect(45, 10, size, size, size / 2, size / 2, size / 2, size / 2, pixel::nativeColor(20, 255, 20));
			const std::int16_t xs[]{5, 100}, ys[]{65, 100};
			const pixel::native_t colors[]{pixel::nativeColor(20, 20, 255), pixel::nativeColor(230, 200, 80)};
			canvas->fillRoundedRectBoxesRgb565(xs, ys, 2, size, size, 8, 8, 8, 8, colors);
			canvas->setGlobalAlpha(255);
			assert(displayPixelAt(22, 22) != pixel::nativeColor(18, 24, 30));
			assert(displayPixelAt(5 + size / 2, 65 + size / 2) != pixel::nativeColor(18, 24, 30));
			fingerprint("circle-cache-boundaries");
		}
	}
	Display::setAA(4);

	auto root = Document::instance().createView();
	root.style().width(120); root.style().height(120);
	root.style().backgroundColor(pixel::nativeColor(18, 24, 30));
	auto card = Document::instance().createView();
	card.style().setProperty("position", "absolute");
	card.style().left(10); card.style().top(10);
	card.style().width(90); card.style().height(90);
	card.style().setProperty("border-radius", "12px");
	card.style().set(Property::FontSize, 8);
	card.style().set(Property::FontId, 9301);
	card.style().setProperty("color", "white");
	card.appendChild(Document::instance().createText("X X X"));
	root.appendChild(card);
	Document::instance().mount(root, 120, 120);
	const char *backgrounds[]{
		"linear-gradient(135deg, #ff2040, #2040ff)",
		"linear-gradient(to bottom, rgba(255, 32, 64, 0.4), rgba(32, 64, 255, 0.8))",
		"radial-gradient(ellipse at center, #ff2040, #2040ff)",
		"radial-gradient(ellipse at center, rgba(255, 32, 64, 0.4), rgba(32, 64, 255, 0.8))",
	};
	// CSS feature elimination and cache selection have different contracts.
	// Authored gradient storage is exercised in the enabled build; both builds
	// replay the identical native gradient commands above.
	if (cssOnly) for (const auto *background : backgrounds) {
		card.style().setProperty("background", background);
		for (int frame = 0; frame < 3; ++frame) {
			Document::instance().refresh(root, 120, 120);
			DisplayList::instance().replay();
			assert(displayPixelAt(60, 60) != pixel::nativeColor(18, 24, 30));
			fingerprint(background);
		}
	}
	// CSS rounded borders: the raster samples only the corner boxes; widths
	// and radii either side of each other, and uneven corners.
	const char *borders[][2]{
		{"1px solid #e0c060", "14px"},
		{"3px solid #40e0a0", "12px"},
		{"6px solid #ff6040", "4px"},
		{"2px solid rgba(255, 255, 255, 0.5)", "20px 6px 30px 0px"},
		{"9px solid #6080ff", "45px"},
		{"1px solid #ffffff", "0px"},
		{"1px solid #e0c060", "50%"},
		{"4px solid #40e0a0", "30% 10%"},
		{"2px solid #ff6040", "80px"},
	};
	for (const auto &border : borders) {
		card.style().setProperty("border", border[0]);
		card.style().setProperty("border-radius", border[1]);
		Document::instance().refresh(root, 120, 120);
		DisplayList::instance().replay();
		fingerprint(border[0]);
	}
	card.style().setProperty("border", "0px solid #000000");
	card.style().setProperty("border-radius", "12px");
	if (cssOnly) card.style().setProperty("background", backgrounds[0]);
	card.style().setProperty("transform", "rotate(17deg) scale(0.8)");
	for (int frame = 0; frame < 3; ++frame) {
		if (frame == 1) card.style().setProperty("transform", "rotate(31deg) scale(0.9)");
		Document::instance().refresh(root, 120, 120);
		fingerprint(cssOnly ? "transformed-gradient" : "transformed-text");
		const Node &node = Tree::instance().node(card.id());
		int16_t xs[4], ys[4];
		ViewRenderer::transformedCorners(node, false, xs, ys);
		assert(xs[0] != node.layout.x || ys[0] != node.layout.y);
		std::printf("corners %d %d %d %d\n", xs[0], ys[0], xs[2], ys[2]);
		ViewRenderer::transformedCorners(node, true, xs, ys);
		std::printf("previous-corners %d %d %d %d\n", xs[0], ys[0], xs[2], ys[2]);
	}
	return 0;
}
