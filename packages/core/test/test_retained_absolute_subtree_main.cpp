#include "native_test_harness.h"

#include "display.h"
#include "pixel.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/refresh_perf.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>


namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {

int pixelLuma(std::uint16_t pixel)
{
	int r = 0;
	int g = 0;
	int b = 0;
	gea::framework::graphics::pixel::unpackRgb565(pixel, &r, &g, &b);
	r = (r * 255 + 15) / 31;
	g = (g * 255 + 31) / 63;
	b = (b * 255 + 15) / 31;
	return (r * 77 + g * 150 + b * 29 + 128) / 256;
}

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[test_retained_absolute_subtree_main] %s expected %d, got %d\n", label, expected, actual);
	return false;
}

bool expectReplayRegionContaining(const gea::embedded::ui::RefreshPerfStats &perf,
                                  int x0,
                                  int y0,
                                  int x1,
                                  int y1,
                                  const char *label)
{
	for (int i = 0; i < perf.treeReplayRegionSampleCount; ++i) {
		if (perf.treeReplayRegionX0[i] <= x0 &&
		    perf.treeReplayRegionY0[i] <= y0 &&
		    perf.treeReplayRegionX1[i] >= x1 &&
		    perf.treeReplayRegionY1[i] >= y1)
			return true;
	}
	std::fprintf(stderr,
	             "[test_retained_absolute_subtree_main] %s expected replay region covering (%d,%d)-(%d,%d), samples=%d\n",
	             label,
	             x0,
	             y0,
	             x1,
	             y1,
	             perf.treeReplayRegionSampleCount);
	for (int i = 0; i < perf.treeReplayRegionSampleCount; ++i) {
		std::fprintf(stderr,
		             "  sample[%d]=(%d,%d)-(%d,%d) origin=%d\n",
		             i,
		             perf.treeReplayRegionX0[i],
		             perf.treeReplayRegionY0[i],
		             perf.treeReplayRegionX1[i],
		             perf.treeReplayRegionY1[i],
		             perf.treeReplayRegionOrigin[i]);
	}
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::embedded::ui::setViewportMetrics(120, 80, 1.0);

	{
		const int rootId = Tree::instance().createView();
		const int leafId = Tree::instance().createView();
		NodeHandle(rootId).appendChild(NodeHandle(leafId));
		NodeHandle(rootId).style().width(120);
		NodeHandle(rootId).style().height(80);
		NodeHandle(rootId).style().backgroundColor(0x0000);
		NodeHandle(leafId).style().position(1);
		NodeHandle(leafId).style().left(10);
		NodeHandle(leafId).style().top(10);
		NodeHandle(leafId).style().width(8);
		NodeHandle(leafId).style().height(8);
		NodeHandle(leafId).style().backgroundColor(0xffff);

		Tree::instance().mount(rootId, 120, 80);
		refreshPerfStatsReset();
		gea::platform::display::Display::flushStatsReset();
		NodeHandle(leafId).style().top(20);
		Tree::instance().refresh(rootId, 120, 80);
		const auto leafMovePerf = refreshPerfStatsRead();
		if (!expectReplayRegionContaining(leafMovePerf, 8, 8, 19, 29, "retained moved leaf guard"))
			return 1;
		if (pixelLuma(displayPixelAt(12, 12)) > 32 || pixelLuma(displayPixelAt(12, 22)) < 220) {
			std::fprintf(stderr,
			             "[test_retained_absolute_subtree_main] retained leaf movement should clear guarded old footprint, old=%d new=%d\n",
			             pixelLuma(displayPixelAt(12, 12)),
			             pixelLuma(displayPixelAt(12, 22)));
			return 1;
		}
	}

	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::embedded::ui::setViewportMetrics(120, 80, 1.0);

	const int rootId = Tree::instance().createView();
	const int cameraId = Tree::instance().createView();
	const int tileId = Tree::instance().createView();
	NodeHandle(rootId).appendChild(NodeHandle(cameraId));
	NodeHandle(cameraId).appendChild(NodeHandle(tileId));

	NodeHandle(rootId).style().width(120);
	NodeHandle(rootId).style().height(80);
	NodeHandle(rootId).style().backgroundColor(0x0000);
	NodeHandle(cameraId).style().position(1);
	NodeHandle(cameraId).style().left(20);
	NodeHandle(cameraId).style().top(10);
	NodeHandle(cameraId).style().width(60);
	NodeHandle(cameraId).style().height(40);
	NodeHandle(tileId).style().position(1);
	NodeHandle(tileId).style().left(0);
	NodeHandle(tileId).style().top(0);
	NodeHandle(tileId).style().width(12);
	NodeHandle(tileId).style().height(12);
	NodeHandle(tileId).style().backgroundColor(0xffff);

	Tree::instance().mount(rootId, 120, 80);
	const int initialCommandCount = DisplayList::instance().commandCount();
	if (initialCommandCount <= 0) {
		std::fprintf(stderr, "[test_retained_absolute_subtree_main] expected initial display commands\n");
		return 1;
	}
	if (!expectEqual(Tree::instance().node(cameraId).layout.x, 20, "initial camera x")) return 1;
	if (!expectEqual(Tree::instance().node(tileId).layout.x, 20, "initial tile x")) return 1;
	if (pixelLuma(displayPixelAt(22, 12)) < 220) {
		std::fprintf(stderr, "[test_retained_absolute_subtree_main] expected initial tile pixels before movement\n");
		return 1;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	NodeHandle(cameraId).style().left(35);
	Tree::instance().refresh(rootId, 120, 80);
	const auto movePerf = refreshPerfStatsRead();
	if (movePerf.treeRecordCalls != 0 ||
	    movePerf.treeRecordedNodes != 0 ||
	    DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] moving absolute non-leaf should retain display list, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d\n",
		             movePerf.treeRecordCalls,
		             movePerf.treeRecordedNodes,
		             DisplayList::instance().commandCount(),
		             initialCommandCount);
		return 1;
	}
	if (movePerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] moving absolute non-leaf should direct replay dirty regions, direct=%d refreshes=%d\n",
		             movePerf.treeDirectReplayCalls,
		             movePerf.treeRefreshCalls);
		return 1;
	}
	if (!expectEqual(Tree::instance().node(cameraId).layout.x, 35, "moved camera x")) return 1;
	if (!expectEqual(Tree::instance().node(tileId).layout.x, 35, "moved tile x")) return 1;
	if (pixelLuma(displayPixelAt(22, 12)) > 32 || pixelLuma(displayPixelAt(37, 12)) < 220) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] retained subtree pixels should move old=%d new=%d\n",
		             pixelLuma(displayPixelAt(22, 12)),
		             pixelLuma(displayPixelAt(37, 12)));
		return 1;
	}
	if (flushPixelCount() >= (120 * 80) / 2) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] retained move should not flush most of the viewport, flushed=%d full=%d\n",
		             flushPixelCount(),
		             120 * 80);
		return 1;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	NodeHandle(cameraId).style().left(45);
	NodeHandle(tileId).style().left(5);
	Tree::instance().refresh(rootId, 120, 80);
	const auto childMovePerf = refreshPerfStatsRead();
	if (childMovePerf.treeRecordCalls != 0 ||
	    childMovePerf.treeRecordedNodes != 0 ||
	    DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] moving absolute parent and child should retain display list, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d\n",
		             childMovePerf.treeRecordCalls,
		             childMovePerf.treeRecordedNodes,
		             DisplayList::instance().commandCount(),
		             initialCommandCount);
		return 1;
	}
	if (childMovePerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] moving absolute parent and child should direct replay, direct=%d refreshes=%d\n",
		             childMovePerf.treeDirectReplayCalls,
		             childMovePerf.treeRefreshCalls);
		return 1;
	}
	if (!expectEqual(Tree::instance().node(cameraId).layout.x, 45, "moved camera x with child movement")) return 1;
	if (!expectEqual(Tree::instance().node(tileId).layout.x, 50, "moved tile x with child movement")) return 1;
	if (pixelLuma(displayPixelAt(37, 12)) > 32 || pixelLuma(displayPixelAt(52, 12)) < 220) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] retained parent+child pixels should move old=%d new=%d\n",
		             pixelLuma(displayPixelAt(37, 12)),
		             pixelLuma(displayPixelAt(52, 12)));
		return 1;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	NodeHandle(cameraId).style().top(25);
	Tree::instance().refresh(rootId, 120, 80);
	const auto verticalMovePerf = refreshPerfStatsRead();
	if (verticalMovePerf.treeRecordCalls != 0 ||
	    verticalMovePerf.treeRecordedNodes != 0 ||
	    DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] moving absolute parent vertically should retain display list, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d\n",
		             verticalMovePerf.treeRecordCalls,
		             verticalMovePerf.treeRecordedNodes,
		             DisplayList::instance().commandCount(),
		             initialCommandCount);
		return 1;
	}
	if (verticalMovePerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] vertical absolute movement should direct replay, direct=%d refreshes=%d\n",
		             verticalMovePerf.treeDirectReplayCalls,
		             verticalMovePerf.treeRefreshCalls);
		return 1;
	}
	if (!expectEqual(Tree::instance().node(cameraId).layout.y, 25, "moved camera y")) return 1;
	if (!expectEqual(Tree::instance().node(tileId).layout.y, 25, "moved tile y")) return 1;
	if (pixelLuma(displayPixelAt(52, 12)) > 32 || pixelLuma(displayPixelAt(52, 27)) < 220) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] retained vertical movement should clear old footprint, old=%d new=%d\n",
		             pixelLuma(displayPixelAt(52, 12)),
		             pixelLuma(displayPixelAt(52, 27)));
		return 1;
	}
	if (flushPixelCount() >= (120 * 80) / 2) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] retained vertical move should not flush most of the viewport, flushed=%d full=%d\n",
		             flushPixelCount(),
		             120 * 80);
		return 1;
	}

	// An overflow clip is a scope around a node's descendants and therefore sits
	// outside that node's paint-command range in the retained display list. It must
	// still follow the subtree when the parent is translated. Before this regression
	// was fixed, moving this card down left its clip at y=10, so only the overlap at
	// y=30..39 remained visible at the card's new y=30..59 position.
	resetNativeHost();
	setNativeDisplaySize(120, 80);
	gea::embedded::ui::setViewportMetrics(120, 80, 1.0);

	const int clippedRootId = Tree::instance().createView();
	const int clippedCardId = Tree::instance().createView();
	const int clippedCardChildId = Tree::instance().createView();
	NodeHandle(clippedRootId).appendChild(NodeHandle(clippedCardId));
	NodeHandle(clippedCardId).appendChild(NodeHandle(clippedCardChildId));
	NodeHandle(clippedRootId).style().width(120);
	NodeHandle(clippedRootId).style().height(80);
	NodeHandle(clippedRootId).style().backgroundColor(0x0000);
	NodeHandle(clippedCardId).style().position(1);
	NodeHandle(clippedCardId).style().left(10);
	NodeHandle(clippedCardId).style().top(10);
	NodeHandle(clippedCardId).style().width(30);
	NodeHandle(clippedCardId).style().height(30);
	NodeHandle(clippedCardId).style().backgroundColor(0xffff);
	NodeHandle(clippedCardId).style().set(Property::Overflow, 1);
	NodeHandle(clippedCardChildId).style().position(1);
	NodeHandle(clippedCardChildId).style().left(4);
	NodeHandle(clippedCardChildId).style().top(4);
	NodeHandle(clippedCardChildId).style().width(8);
	NodeHandle(clippedCardChildId).style().height(8);
	NodeHandle(clippedCardChildId).style().backgroundColor(0x0000);

	Tree::instance().mount(clippedRootId, 120, 80);
	NodeHandle(clippedCardId).style().top(30);
	Tree::instance().refresh(clippedRootId, 120, 80);
	if (pixelLuma(displayPixelAt(20, 50)) < 220) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] translated overflow clip must follow its subtree, luma=%d\n",
		             pixelLuma(displayPixelAt(20, 50)));
		return 1;
	}

	resetNativeHost();
	setNativeDisplaySize(90, 60);
	gea::embedded::ui::setViewportMetrics(90, 60, 1.0);

	const int scopedRootId = Tree::instance().createView();
	const int viewportId = Tree::instance().createView();
	const int scopedWorldId = Tree::instance().createView();
	const int clippedTileId = Tree::instance().createView();
	const int visibleTileId = Tree::instance().createView();
	const int overlayId = Tree::instance().createView();
	NodeHandle(scopedRootId).appendChild(NodeHandle(viewportId));
	NodeHandle(viewportId).appendChild(NodeHandle(scopedWorldId));
	NodeHandle(scopedWorldId).appendChild(NodeHandle(clippedTileId));
	NodeHandle(scopedWorldId).appendChild(NodeHandle(visibleTileId));
	NodeHandle(scopedRootId).appendChild(NodeHandle(overlayId));

	NodeHandle(scopedRootId).style().width(90);
	NodeHandle(scopedRootId).style().height(60);
	NodeHandle(scopedRootId).style().backgroundColor(0x0000);
	NodeHandle(viewportId).style().position(1);
	NodeHandle(viewportId).style().left(10);
	NodeHandle(viewportId).style().top(10);
	NodeHandle(viewportId).style().width(50);
	NodeHandle(viewportId).style().height(30);
	NodeHandle(viewportId).style().set(Property::Overflow, 1);
	NodeHandle(scopedWorldId).style().position(1);
	NodeHandle(scopedWorldId).style().left(0);
	NodeHandle(scopedWorldId).style().top(0);
	NodeHandle(scopedWorldId).style().width(80);
	NodeHandle(scopedWorldId).style().height(30);
	NodeHandle(clippedTileId).style().position(1);
	NodeHandle(clippedTileId).style().left(4);
	NodeHandle(clippedTileId).style().top(-8);
	NodeHandle(clippedTileId).style().width(12);
	NodeHandle(clippedTileId).style().height(8);
	NodeHandle(clippedTileId).style().backgroundColor(0xffff);
	NodeHandle(visibleTileId).style().position(1);
	NodeHandle(visibleTileId).style().left(4);
	NodeHandle(visibleTileId).style().top(4);
	NodeHandle(visibleTileId).style().width(12);
	NodeHandle(visibleTileId).style().height(12);
	NodeHandle(visibleTileId).style().backgroundColor(0xffff);
	NodeHandle(overlayId).style().position(1);
	NodeHandle(overlayId).style().left(22);
	NodeHandle(overlayId).style().top(14);
	NodeHandle(overlayId).style().width(10);
	NodeHandle(overlayId).style().height(10);
	NodeHandle(overlayId).style().backgroundColor(0x0000);
	NodeHandle(overlayId).style().opacity(128);

	Tree::instance().mount(scopedRootId, 90, 60);
	if (pixelLuma(displayPixelAt(16, 4)) > 32) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] clipped child should not paint outside overflow viewport, luma=%d\n",
		             pixelLuma(displayPixelAt(16, 4)));
		return 1;
	}
	const int initialScopedOverlay = pixelLuma(displayPixelAt(24, 16));
	if (initialScopedOverlay < 80 || initialScopedOverlay > 180) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] expected translucent overlay to blend once before movement, luma=%d\n",
		             initialScopedOverlay);
		return 1;
	}

	refreshPerfStatsReset();
	NodeHandle(scopedWorldId).style().left(8);
	Tree::instance().refresh(scopedRootId, 90, 60);
	const auto scopedPerf = refreshPerfStatsRead();
	if (scopedPerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] clipped subtree plus translucent overlay should still use scoped direct replay, direct=%d refreshes=%d\n",
		             scopedPerf.treeDirectReplayCalls,
		             scopedPerf.treeRefreshCalls);
		return 1;
	}
	if (pixelLuma(displayPixelAt(16, 4)) > 32) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] direct replay must preserve ancestor overflow clip, luma=%d\n",
		             pixelLuma(displayPixelAt(16, 4)));
		return 1;
	}
	const int movedScopedOverlay = pixelLuma(displayPixelAt(24, 16));
	if (movedScopedOverlay < 80 || movedScopedOverlay > 180) {
		std::fprintf(stderr,
		             "[test_retained_absolute_subtree_main] direct replay must preserve translucent overlay alpha, luma=%d\n",
		             movedScopedOverlay);
		return 1;
	}

	// A floating drag card with children is not a camera/world viewport. Both
	// shapes used to enter the horizontal-pan path, shifting the entire screen
	// and then repainting every stationary sibling to repair that shift.
	for (bool world : {false, true}) {
		resetNativeHost();
		setNativeDisplaySize(120, 80);
		setViewportMetrics(120, 80, 1.0);
		const int root = Tree::instance().createView();
		const int fixed = Tree::instance().createView();
		const int moving = Tree::instance().createView();
		const int child = Tree::instance().createView();
		NodeHandle(root).appendChild(NodeHandle(moving));
		NodeHandle(moving).appendChild(NodeHandle(child));
		NodeHandle(root).appendChild(NodeHandle(fixed));
		NodeHandle(root).style().width(120);
		NodeHandle(root).style().height(80);
		NodeHandle(root).style().backgroundColor(0x0000);
		NodeHandle(root).style().set(Property::Overflow, 1);
		NodeHandle(moving).style().position(1);
		NodeHandle(moving).style().left(world ? -20 : 10);
		NodeHandle(moving).style().top(0);
		NodeHandle(moving).style().width(world ? 240 : 20);
		NodeHandle(moving).style().height(world ? 80 : 16);
		NodeHandle(child).style().position(1);
		NodeHandle(child).style().left(world ? 40 : 4);
		NodeHandle(child).style().top(4);
		NodeHandle(child).style().width(8);
		NodeHandle(child).style().height(8);
		NodeHandle(child).style().backgroundColor(0xffff);
		NodeHandle(fixed).style().position(1);
		NodeHandle(fixed).style().left(90);
		NodeHandle(fixed).style().top(50);
		NodeHandle(fixed).style().width(8);
		NodeHandle(fixed).style().height(8);
		NodeHandle(fixed).style().backgroundColor(0xffff);
		Tree::instance().mount(root, 120, 80);
		refreshPerfStatsReset();
		gea::platform::display::Display::flushStatsReset();
		NodeHandle(moving).style().left(world ? -10 : 20);
		Tree::instance().refresh(root, 120, 80);
		const auto perf = refreshPerfStatsRead();
		if (!expectEqual(perf.treePanReplayCalls, world ? 1 : 0, world ? "world pan retained" : "small drag must not pan viewport")) return 1;
		if (!world && flushPixelCount() >= 120 * 80 / 2) {
			std::fprintf(stderr, "small drag repainted most of screen: %d pixels\n", flushPixelCount());
			return 1;
		}
		const int oldX = world ? 22 : 16;
		const int newX = oldX + 10;
		if (pixelLuma(displayPixelAt(oldX, 6)) > 32 ||
		    pixelLuma(displayPixelAt(newX, 6)) < 220 ||
		    pixelLuma(displayPixelAt(92, 52)) < 220 ||
		    pixelLuma(displayPixelAt(102, 52)) > 32) {
			std::fprintf(stderr, "drag/world movement left stale pixels, world=%d old=%d new=%d fixed=%d ghost=%d\n", world, pixelLuma(displayPixelAt(oldX, 6)), pixelLuma(displayPixelAt(newX, 6)), pixelLuma(displayPixelAt(92, 52)), pixelLuma(displayPixelAt(102, 52)));
			return 1;
		}
	}

	resetNativeHost();
	{
		auto &tree = Tree::instance();
		const int root = tree.createView(), child = tree.createView(); NodeHandle(root).appendChild(NodeHandle(child));
		auto parent = NodeHandle(root).style(), abs = NodeHandle(child).style();
		parent.position(2); parent.width(100); parent.height(60); parent.backgroundColor(0x0000);
		parent.setProperty("padding", "5px"); parent.setProperty("border", "2px solid #000000");
		abs.position(1); abs.width(8); abs.height(8); abs.backgroundColor(0xffff);
		// The retained path requires explicit zero minimums; CSS auto minimums
		// deliberately remain on the full-layout path.
		abs.setProperty("min-width", "0"); abs.setProperty("min-height", "0");
		abs.setProperty("left", "10%"); abs.setProperty("top", "10%"); tree.mount(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 13, "bordered containing block initial left") ||
		    !expectEqual(tree.node(child).layout.y, 9, "bordered containing block initial top")) return 1;
		refreshPerfStatsReset();
		abs.setProperty("left", "20%"); abs.setProperty("top", "20%"); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 24, "retained percentage left excludes borders") ||
		    !expectEqual(tree.node(child).layout.y, 16, "retained percentage top excludes borders")) return 1;
		if (pixelLuma(displayPixelAt(15, 11)) > 32 || pixelLuma(displayPixelAt(26, 18)) < 220) return 1;
		refreshPerfStatsReset(); abs.top(24); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 24, "retained movement keeps percentage horizontal basis") ||
		    !expectEqual(tree.node(child).layout.y, 26, "retained pixel top includes border") ||
		    !expectEqual(refreshPerfStatsRead().treeAbsModeFast, 1, "bordered containing block remains on retained path")) return 1;
		abs.setProperty("left", "auto"); abs.setProperty("top", "auto");
		abs.setProperty("right", "10%"); abs.setProperty("bottom", "10%"); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 93, "retained right excludes borders") ||
		    !expectEqual(tree.node(child).layout.y, 57, "retained bottom excludes borders")) return 1;
		const int wrapper = tree.createView(); NodeHandle(root).appendChild(NodeHandle(wrapper));
		NodeHandle(wrapper).appendChild(NodeHandle(child));
		NodeHandle(wrapper).style().width(40); NodeHandle(wrapper).style().height(30);
		NodeHandle(wrapper).style().setProperty("padding", "5px");
		tree.refresh(root, 120, 80);
		refreshPerfStatsReset(); abs.setProperty("right", "20%"); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 82, "static wrapper does not replace the containing block on mutation") ||
		    !expectEqual(refreshPerfStatsRead().treeAbsModeFast, 0, "static wrapper delegates positioning to full layout")) return 1;
	}

	resetNativeHost();
	{
		auto &tree = Tree::instance();
		const int root = tree.createView(), before = tree.createView(), child = tree.createView();
		NodeHandle(root).appendChild(NodeHandle(before)); NodeHandle(root).appendChild(NodeHandle(child));
		auto parent = NodeHandle(root).style(), preceding = NodeHandle(before).style(), abs = NodeHandle(child).style();
		parent.position(2); parent.width(100); parent.height(60); parent.backgroundColor(0x0000);
		parent.setProperty("padding", "5px"); parent.setProperty("border", "2px solid #000000");
		preceding.height(20); preceding.setProperty("margin-bottom", "7px"); preceding.position(2); preceding.top(10);
		abs.position(1); abs.width(8); abs.height(8); abs.backgroundColor(0xffff);
		abs.setProperty("min-width", "0"); abs.setProperty("min-height", "0");
		tree.mount(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 7, "block static position starts at content left") ||
		    !expectEqual(tree.node(child).layout.y, 34, "block static position follows sibling before relative offset")) return 1;
		refreshPerfStatsReset(); abs.left(20); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.x, 22, "retained explicit inset uses padding edge") ||
		    !expectEqual(tree.node(child).layout.y, 34, "retained movement preserves block static anchor") ||
		    !expectEqual(refreshPerfStatsRead().treeAbsModeFast, 1, "static anchor remains usable on retained path")) return 1;
		if (pixelLuma(displayPixelAt(9, 36)) > 32 || pixelLuma(displayPixelAt(24, 36)) < 220) return 1;
		refreshPerfStatsReset(); abs.width(12); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.y, 34, "retained resize preserves static anchor") ||
		    !expectEqual(refreshPerfStatsRead().treeAbsModeFast, 1, "static anchor supports retained resize")) return 1;
		preceding.height(30); tree.refresh(root, 120, 80);
		if (!expectEqual(tree.node(child).layout.y, 44, "normal sibling resize recomputes static anchor")) return 1;
	}

	return 0;
}
