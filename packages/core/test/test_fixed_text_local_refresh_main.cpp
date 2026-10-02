#include "native_test_harness.h"

#include "display.h"
#include "pixel.h"
#include "ui/document.h"
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

bool expect_scaled_absolute_circle_moves_without_ghosting()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	setNativeDisplaySize(120, 90);
	// resetNativeHost() pins the preferred mount size to the panel default; mount at the fixture size
	// so refresh(root, 120, 90) below is not a viewport change that forces a full repaint.
	Document::setPreferredMountSize(120, 90);
	setViewportMetrics(120, 90, 1.0);
	gea::platform::display::Display::setAA(2);

	auto root = Document::instance().createView();
	auto circle = Document::instance().createView();

	root.style().width(120);
	root.style().height(90);
	root.style().backgroundColor(0x0000);

	circle.style().position(1);
	circle.style().left(20);
	circle.style().top(20);
	circle.style().width(40);
	circle.style().height(40);
	circle.style().backgroundColor(0xf800);
	circle.style().set(Property::BorderRadiusTopLeft, 20);
	circle.style().set(Property::BorderRadiusTopRight, 20);
	circle.style().set(Property::BorderRadiusBottomRight, 20);
	circle.style().set(Property::BorderRadiusBottomLeft, 20);

	root.appendChild(circle);
	Document::instance().mount(root, 120, 90);

	if (pixelLuma(displayPixelAt(40, 40)) < 40) {
		std::fprintf(stderr, "[test_fixed_text_local_refresh] initial scaled-circle fixture did not draw\n");
		return false;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	circle.style().left(60);
	circle.style().top(20);
	circle.style().scale(0.5);
	Document::instance().refresh(root, 120, 90);

	const auto perf = refreshPerfStatsRead();
	if (perf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] scaled absolute circle should use dirty replay, direct=%d refreshes=%d fullRecords=%d reprojects=%d absReject=%d/%d\n",
		             perf.treeDirectReplayCalls,
		             perf.treeRefreshCalls,
		             perf.treeFullRecords,
		             perf.treeReprojects,
		             perf.treeAbsModeRejectNode,
		             perf.treeAbsModeRejectReason);
		return false;
	}

	const int oldCenter = pixelLuma(displayPixelAt(40, 40));
	const int oldRightEdge = pixelLuma(displayPixelAt(58, 40));
	const int topLeftScaledCenter = pixelLuma(displayPixelAt(70, 40));
	const int newCenter = pixelLuma(displayPixelAt(80, 40));
	if (oldCenter > 24 || oldRightEdge > 24 || newCenter < 40) {
		const Node &node = Tree::instance().node(circle.id());
		const DisplayCommand *command = DisplayList::instance().nodeCommandAt(circle.id(), 0);
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] scaled absolute circle should clear old footprint and draw at new center oldCenter=%d oldEdge=%d topLeftScaledCenter=%d newCenter=%d layout=(%d,%d %dx%d) cmd=%d bbox=(%d,%d %dx%d)\n",
		             oldCenter,
		             oldRightEdge,
		             topLeftScaledCenter,
		             newCenter,
		             node.layout.x,
		             node.layout.y,
		             node.layout.width,
		             node.layout.height,
		             command ? static_cast<int>(command->type) : -1,
		             command ? command->bx : -1,
		             command ? command->by : -1,
		             command ? command->bw : -1,
		             command ? command->bh : -1);
		return false;
	}

	return true;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	if (!expect_scaled_absolute_circle_moves_without_ghosting()) return 1;

	resetNativeHost();
	setNativeDisplaySize(220, 220);
	Document::setPreferredMountSize(220, 220);
	gea::embedded::ui::setViewportMetrics(220, 220, 1.0);

	auto root = Document::instance().createView();
	auto face = Document::instance().createView();
	auto label = Document::instance().createText("23C");
	auto tick = Document::instance().createView();
	auto button = Document::instance().createButton();
	auto buttonLabel = Document::instance().createText("+");

	root.style().width(220);
	root.style().height(220);
	root.style().backgroundColor(0x0000);

	face.style().position(1);
	face.style().left(20);
	face.style().top(20);
	face.style().width(180);
	face.style().height(180);
	face.style().backgroundColor(0xffff);
	face.style().display(kDisplayFlex);
	face.style().set(Property::FlexDirection, 0);
	face.style().set(Property::AlignItems, 1);
	face.style().set(Property::JustifyContent, 1);

	label.style().width(160);
	label.style().set(Property::FontSize, 36);
	label.style().set(Property::LineHeight, 36);
	label.style().set(Property::TextAlign, 1);
	label.style().color(0x0000);

	tick.style().position(1);
	tick.style().left(105);
	tick.style().top(30);
	tick.style().width(6);
	tick.style().height(40);
	tick.style().backgroundColor(0x001f);
	tick.style().rotateDegrees(38);

	button.style().position(1);
	button.style().left(150);
	button.style().top(158);
	button.style().width(46);
	button.style().height(46);
	button.style().backgroundColor(0x000f);
	button.style().display(kDisplayFlex);
	button.style().set(Property::AlignItems, 1);
	button.style().set(Property::JustifyContent, 1);
	button.style().set(Property::TransformTranslateXPercent, -500);
	button.style().set(Property::TransformTranslateYPercent, -500);
	buttonLabel.style().width(20);
	buttonLabel.style().set(Property::FontSize, 24);
	buttonLabel.style().set(Property::LineHeight, 24);
	buttonLabel.style().set(Property::TextAlign, 1);
	buttonLabel.style().color(0xffff);

	root.appendChild(face);
	face.appendChild(label);
	root.appendChild(tick);
	button.appendChild(buttonLabel);
	root.appendChild(button);

	Document::instance().mount(root, 220, 220);
	const int initialCommandCount = DisplayList::instance().commandCount();
	const int initialLabelX = Tree::instance().node(label.id()).layout.x;
	const int initialLabelY = Tree::instance().node(label.id()).layout.y;
	const int initialLabelWidth = Tree::instance().node(label.id()).layout.width;
	const int initialLabelHeight = Tree::instance().node(label.id()).layout.height;
	if (initialCommandCount <= 0 || initialLabelWidth != 160 || initialLabelHeight != 36) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] unexpected initial state commands=%d label=%dx%d\n",
		             initialCommandCount,
		             initialLabelWidth,
		             initialLabelHeight);
		return 1;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	label.setText("23.5C");
	Document::instance().refresh(root, 220, 220);
	const auto perf = refreshPerfStatsRead();

	if (Tree::instance().node(label.id()).layout.x != initialLabelX ||
	    Tree::instance().node(label.id()).layout.y != initialLabelY ||
	    Tree::instance().node(label.id()).layout.width != initialLabelWidth ||
	    Tree::instance().node(label.id()).layout.height != initialLabelHeight) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] stable fixed-width text should keep its layout box, before=(%d,%d %dx%d) after=(%d,%d %dx%d)\n",
		             initialLabelX,
		             initialLabelY,
		             initialLabelWidth,
		             initialLabelHeight,
		             Tree::instance().node(label.id()).layout.x,
		             Tree::instance().node(label.id()).layout.y,
		             Tree::instance().node(label.id()).layout.width,
		             Tree::instance().node(label.id()).layout.height);
		return 1;
	}

	if (perf.treeRecordCalls != 0 ||
	    perf.treeRecordedNodes != 0 ||
	    DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] fixed-box text content changes should retain the display list, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d\n",
		             perf.treeRecordCalls,
		             perf.treeRecordedNodes,
		             DisplayList::instance().commandCount(),
		             initialCommandCount);
		return 1;
	}

	if (perf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] fixed-box text content changes should direct-replay dirty regions, direct=%d refreshes=%d fullRecords=%d reprojects=%d absReject=%d/%d\n",
		             perf.treeDirectReplayCalls,
		             perf.treeRefreshCalls,
		             perf.treeFullRecords,
		             perf.treeReprojects,
		             perf.treeAbsModeRejectNode,
		             perf.treeAbsModeRejectReason);
		return 1;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	tick.style().backgroundColor(0xf800);
	Document::instance().refresh(root, 220, 220);
	const auto transformedPaintPerf = refreshPerfStatsRead();

	if (transformedPaintPerf.treeRecordCalls != 0 ||
	    transformedPaintPerf.treeRecordedNodes != 0 ||
	    DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] static transformed leaf paint changes should retain the display list, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d\n",
		             transformedPaintPerf.treeRecordCalls,
		             transformedPaintPerf.treeRecordedNodes,
		             DisplayList::instance().commandCount(),
		             initialCommandCount);
		return 1;
	}

	if (transformedPaintPerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] static transformed leaf paint changes should direct-replay dirty regions, direct=%d refreshes=%d\n",
		             transformedPaintPerf.treeDirectReplayCalls,
		             transformedPaintPerf.treeRefreshCalls);
		return 1;
	}

	refreshPerfStatsReset();
	gea::platform::display::Display::flushStatsReset();
	button.style().backgroundColor(0x07e0);
	Document::instance().refresh(root, 220, 220);
	const auto transformedContainerPaintPerf = refreshPerfStatsRead();

	if (transformedContainerPaintPerf.treeRecordCalls != 0 ||
	    transformedContainerPaintPerf.treeRecordedNodes != 0 ||
	    DisplayList::instance().commandCount() != initialCommandCount) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] static transformed container paint changes should retain the display list, recordCalls=%d recordedNodes=%d commands=%d initialCommands=%d\n",
		             transformedContainerPaintPerf.treeRecordCalls,
		             transformedContainerPaintPerf.treeRecordedNodes,
		             DisplayList::instance().commandCount(),
		             initialCommandCount);
		return 1;
	}

	if (transformedContainerPaintPerf.treeDirectReplayCalls <= 0) {
		std::fprintf(stderr,
		             "[test_fixed_text_local_refresh] static transformed container paint changes should direct-replay dirty regions, direct=%d refreshes=%d\n",
		             transformedContainerPaintPerf.treeDirectReplayCalls,
		             transformedContainerPaintPerf.treeRefreshCalls);
		return 1;
	}

	return 0;
}
