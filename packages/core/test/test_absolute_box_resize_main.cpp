// An absolutely positioned box with children (a progress fill cropping its
// artwork) changing width is out of flow: it must repaint only its own old and
// new boxes, not the whole viewport, and match a full repaint.
#include "display.h"
#include "canvas.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/refresh_perf.h"
#include "ui/tree_state.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::embedded::test;

static constexpr int kW = 120;
static constexpr int kH = 80;

// Everything except the sentinel square at (20..23, 15..18).
static std::vector<std::uint16_t> presented()
{
	std::vector<std::uint16_t> result;
	for (int y = 0; y < kH; ++y)
		for (int x = 0; x < kW; ++x)
			if (x < 20 || x > 23 || y < 15 || y > 18) result.push_back(presentedPixelAt(x, y));
	return result;
}

int main()
{
	resetNativeHost();
	setNativeDisplaySize(kW, kH);
	auto &document = Document::instance();
	auto root = document.createView();
	root.style().setProperty("display", "block");
	root.style().width(kW); root.style().height(kH);
	root.style().backgroundColor(0x0010);
	// Static content elsewhere on screen that the resize must not repaint.
	auto label = document.createView();
	label.style().setProperty("position", "absolute");
	label.style().left(10); label.style().top(10);
	label.style().width(40); label.style().height(20);
	label.style().backgroundColor(0x07e0);
	root.appendChild(label);
	auto bar = document.createView();
	bar.style().setProperty("position", "absolute");
	bar.style().left(10); bar.style().top(60);
	bar.style().width(100); bar.style().height(10);
	root.appendChild(bar);
	auto fill = document.createView();
	fill.style().setProperty("position", "absolute");
	fill.style().left(0); fill.style().top(0);
	fill.style().width(20); fill.style().height(10);
	fill.style().setProperty("overflow", "hidden");
	bar.appendChild(fill);
	auto art = document.createView();
	art.style().setProperty("position", "absolute");
	art.style().left(0); art.style().top(0);
	art.style().width(100); art.style().height(10);
	art.style().backgroundColor(0xf800);
	fill.appendChild(art);
	document.mount(root, kW, kH);
	document.refresh(root, kW, kH);
	const std::uint16_t artColor = presentedPixelAt(15, 65);
	const std::uint16_t barBack = presentedPixelAt(100, 65);
	const std::uint16_t sentinel = static_cast<std::uint16_t>(~presentedPixelAt(21, 16));

	int failures = 0;
	for (int width : {35, 50, 42, 80, 10}) {
		// Paint a sentinel into static content: a full-viewport repaint would erase it.
		gea::platform::display::Display::fillRect(20, 15, 4, 4, sentinel);
		gea::platform::display::Display::flush();
		gea::platform::display::Display::canvas()->resetDirty();
		refreshPerfStatsReset();
		fill.style().width(width);
		document.refresh(root, kW, kH);
		if (presentedPixelAt(21, 16) != sentinel) {
			std::fprintf(stderr, "[test_absolute_box_resize] width %d repainted the whole viewport\n", width);
			++failures;
		}
		if (presentedPixelAt(10 + width - 1, 65) != artColor || (width < 100 && presentedPixelAt(10 + width, 65) != barBack)) {
			std::fprintf(stderr, "[test_absolute_box_resize] width %d fill edge wrong\n", width);
			++failures;
		}
		// Compare against a full repaint, outside the sentinel.
		const auto incremental = presented();
		Tree::instance().markDisplayListDirty();
		Tree::instance().node(root.id()).render.dirty = true;
		document.refresh(root, kW, kH);
		if (presented() != incremental) {
			std::fprintf(stderr, "[test_absolute_box_resize] width %d differs from a full repaint\n", width);
			++failures;
		}
	}
	if (failures) return EXIT_FAILURE;
	std::puts("PASS: an absolute box with children resizes with a local repaint that matches a full repaint");
	return EXIT_SUCCESS;
}
