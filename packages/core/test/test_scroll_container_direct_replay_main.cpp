// A scrolling page with an animated child keeps direct dirty-region replay (and,
// built with GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH, the fused flush), and its
// output matches a full repaint. The scrollbar thumb lies outside every node's
// command range, so a change that touches the thumb must still repaint it.
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

static constexpr int kW = 80;
static constexpr int kH = 80;

static std::vector<std::uint16_t> presented()
{
	std::vector<std::uint16_t> result;
	for (int y = 0; y < kH; ++y)
		for (int x = 0; x < kW; ++x) result.push_back(presentedPixelAt(x, y));
	return result;
}

static int failures = 0;
static void check(bool ok, const char *what)
{
	if (ok) return;
	std::fprintf(stderr, "[test_scroll_container_direct_replay] FAIL: %s\n", what);
	++failures;
}

// Row 1 (relative) slides through `lefts`. With width `rowWidth` it stays clear of
// the thumb (x 73..75) or reaches over it.
static void run(int rowWidth, const int (&lefts)[4], bool expectDirect)
{
	resetNativeHost();
	setNativeDisplaySize(kW, kH);
	auto &document = Document::instance();
	auto root = document.createView();
	root.style().setProperty("display", "block");
	root.style().width(kW); root.style().height(kH);
	root.style().backgroundColor(0);
	auto page = document.createView();
	page.style().setProperty("display", "block");
	page.style().width(kW); page.style().height(kH);
	page.style().setProperty("overflow-y", "scroll");
	root.appendChild(page);
	std::vector<decltype(root)> rows;
	for (int i = 0; i < 4; ++i) {
		auto row = document.createView();
		row.style().setProperty("display", "block");
		row.style().width(i == 1 ? rowWidth : 60); row.style().height(30);
		row.style().setProperty("position", "relative");
		row.style().setProperty("margin-bottom", "10px");
		row.style().backgroundColor(i % 2 ? 0xf800 : 0x07e0);
		page.appendChild(row);
		rows.push_back(row);
	}
	document.mount(root, kW, kH);
	document.refresh(root, kW, kH);
	const std::uint16_t thumb = presentedPixelAt(74, 10);
	check(thumb != 0 && thumb != 0x07e0 && thumb != 0xf800, "scrollbar thumb is drawn");

	for (int frame = 0; frame < 4; ++frame) {
		refreshPerfStatsReset();
		rows[1].style().left(lefts[frame]);
		document.refresh(root, kW, kH);
		const auto perf = refreshPerfStatsRead();
		if (expectDirect)
			check(perf.treeDirectReplayCalls > 0, "a change clear of the thumb uses direct replay");
		const auto incremental = presented();
		check(presentedPixelAt(74, 10) == thumb, "thumb survives the incremental frame");
		Tree::instance().markDisplayListDirty();
		Tree::instance().node(root.id()).render.dirty = true;
		document.refresh(root, kW, kH);
		check(presented() == incremental, "incremental frame matches a full repaint");
	}
}

int main()
{
	run(50, {5, 10, 0, 8}, GEA_EMBEDDED_UI_REFRESH_PERF != 0);
	run(70, {10, 4, 10, 0}, false);
	if (failures) return EXIT_FAILURE;
	std::puts("PASS: scroll containers keep direct replay clear of the scrollbar, and match a full repaint");
	return EXIT_SUCCESS;
}
