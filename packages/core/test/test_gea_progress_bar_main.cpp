#include "native_test_harness.h"

#include "app.h"
#include "events.h"
#include "ui/document.h"
#include "ui/tree_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern void __gea_top_level();

namespace {
constexpr int kViewportWidth = 720;
constexpr int kViewportHeight = 1440;
constexpr double kDevicePixelRatio = 2.0;
constexpr const char *kTest = "test_gea_progress_bar_main";
// The display's own pixel format: 16-bit RGB565 by default, wider or narrower
// on builds that change it.
using Pixel = gea::framework::graphics::pixel::native_t;

double g_nowMs = 0;

// core/runtime.cpp starts CSS animation tracks added by later class changes on
// every frame; pumpFrame does not, so do it here the way the device does.
void advance(double ms)
{
	g_nowMs += ms;
	gea::embedded::ui::StyleSheet::instance().startCssAnimations(static_cast<std::uint32_t>(g_nowMs));
	gea::embedded::test::pumpFrame(g_nowMs);
}

int onlyNodeWithClass(const char *className)
{
	const auto nodes = gea::embedded::test::nodesWithClass(className);
	if (nodes.size() != 1) {
		std::fprintf(stderr, "[%s] expected one .%s node, got %zu\n", kTest, className, nodes.size());
		return -1;
	}
	return nodes[0];
}

bool click(const char *text)
{
	if (!gea::embedded::test::clickFirstText(text, true)) {
		std::fprintf(stderr, "[%s] no button labelled %s\n", kTest, text);
		return false;
	}
	advance(16);
	return true;
}

bool hasAncestorWithClass(int node, const char *className)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int id = node; id >= 0; id = tree.node(id).parent) {
		if (tree.hasClass(id, className)) return true;
	}
	return false;
}

// The visible .partClass nodes inside the bar whose root carries rootClass.
// nodesWithClass skips hidden nodes, so a display:none label is not counted.
std::vector<int> partsOf(const char *rootClass, const char *partClass)
{
	std::vector<int> out;
	for (const int id : gea::embedded::test::nodesWithClass(partClass)) {
		if (hasAncestorWithClass(id, rootClass)) out.push_back(id);
	}
	return out;
}

int partOf(const char *rootClass, const char *partClass)
{
	const auto parts = partsOf(rootClass, partClass);
	if (parts.size() != 1) {
		std::fprintf(stderr, "[%s] expected one .%s in .%s, got %zu\n", kTest, partClass, rootClass, parts.size());
		return -1;
	}
	return parts[0];
}

// The bars are function components, so a re-render replaces their nodes: look
// them up again for every check instead of holding ids across clicks.
constexpr const char *kDemo = "demo-bar";
int trackOf(const char *rootClass) { return partOf(rootClass, "progress-bar__track"); }
int fillOf(const char *rootClass) { return partOf(rootClass, "progress-bar__fill"); }

// Fill width as a share of the track, in per-mille, so the check does not
// depend on the device pixel ratio or the demo's bar width.
int fillPermille(int track, int fill)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	if (track < 0 || fill < 0) return -1;
	const int trackWidth = tree.node(track).layout.width;
	if (trackWidth <= 0) return -1;
	return (tree.node(fill).layout.width * 1000 + trackWidth / 2) / trackWidth;
}

bool expectFill(const char *rootClass, int expectedPermille, const char *phase)
{
	const int track = trackOf(rootClass);
	const int fill = fillOf(rootClass);
	if (track < 0 || fill < 0) return false;
	const int actual = fillPermille(track, fill);
	if (actual < expectedPermille - 5 || actual > expectedPermille + 5) {
		auto &tree = gea::embedded::ui::Tree::instance();
		std::fprintf(stderr,
		             "[%s] %s: fill %d/1000 of track, expected %d (fill=%d track=%d)\n",
		             kTest,
		             phase,
		             actual,
		             expectedPermille,
		             tree.node(fill).layout.width,
		             tree.node(track).layout.width);
		return false;
	}
	return true;
}

// The fill's authored width, in per-mille of the track, before layout rounds it
// to whole pixels and clamps a negative width to 0. A pixel check cannot tell
// 33.3% from 33% or -20% from 0%; this one can.
bool expectWidthPercent(const char *rootClass, int expectedPermille, const char *phase)
{
	const int fill = fillOf(rootClass);
	if (fill < 0) return false;
	const int actual = gea::embedded::ui::Tree::instance().node(fill).computedStyle().width_percent;
	if (actual != expectedPermille) {
		std::fprintf(stderr, "[%s] %s: fill width %d/1000, expected exactly %d\n", kTest, phase, actual, expectedPermille);
		return false;
	}
	return true;
}

// The fill sits at the start of its track whenever no sweep is running.
bool expectFillAtStart(const char *phase)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int track = trackOf(kDemo);
	const int fill = fillOf(kDemo);
	if (track < 0 || fill < 0) return false;
	const int offset = tree.node(fill).layout.x - tree.node(track).layout.x;
	if (offset != 0) {
		std::fprintf(stderr, "[%s] %s: fill offset %d, expected 0\n", kTest, phase, offset);
		return false;
	}
	return true;
}

// An empty label is hidden and takes no space: the bar is as tall as its track.
bool expectNoLabel(const char *rootClass, const char *phase)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const int root = onlyNodeWithClass(rootClass);
	const int track = trackOf(rootClass);
	if (root < 0 || track < 0) return false;
	const std::size_t labels = partsOf(rootClass, "progress-bar__label").size();
	if (labels != 0 || tree.node(root).layout.height != tree.node(track).layout.height) {
		std::fprintf(stderr,
		             "[%s] %s: expected no visible label, got %zu (bar %d tall, track %d)\n",
		             kTest,
		             phase,
		             labels,
		             tree.node(root).layout.height,
		             tree.node(track).layout.height);
		return false;
	}
	return true;
}

bool expectText(const char *needle, const char *phase)
{
	return gea::embedded::test::expectContains(gea::embedded::test::rootTextContent(), needle, phase, kTest);
}

std::uint32_t rowChecksum(int y, int fromX, int toX)
{
	std::uint32_t sum = 0;
	for (int x = fromX; x < toX; ++x) sum = sum * 31u + gea::embedded::test::displayPixelAt(x, y);
	return sum;
}

int trackMidY(int track)
{
	const auto &layout = gea::embedded::ui::Tree::instance().node(track).layout;
	return layout.y + layout.height / 2;
}

// A checksum of the painted pixels along the middle row of the demo track. It
// moves when the fill is painted somewhere else, whatever the animation drives.
std::uint32_t trackRowChecksum()
{
	const int track = trackOf(kDemo);
	if (track < 0) return 0;
	const auto &layout = gea::embedded::ui::Tree::instance().node(track).layout;
	return rowChecksum(trackMidY(track), layout.x, layout.x + layout.width);
}

// The same row left and right of the demo track. The fill must never paint
// there, or the track does not clip it.
std::uint32_t besideTrackChecksum()
{
	const int track = trackOf(kDemo);
	if (track < 0) return 0;
	const auto &layout = gea::embedded::ui::Tree::instance().node(track).layout;
	const int y = trackMidY(track);
	return rowChecksum(y, 0, layout.x) * 31u + rowChecksum(y, layout.x + layout.width, kViewportWidth);
}

Pixel fillCenterPixel()
{
	const int fill = fillOf(kDemo);
	if (fill < 0) return 0;
	const auto &layout = gea::embedded::ui::Tree::instance().node(fill).layout;
	return gea::embedded::test::displayPixelAt(layout.x + layout.width / 2, layout.y + layout.height / 2);
}

int distinctChecksums(int frames, double stepMs)
{
	std::vector<std::uint32_t> seen;
	for (int i = 0; i < frames; ++i) {
		advance(stepMs);
		const std::uint32_t sum = trackRowChecksum();
		bool known = false;
		for (const auto value : seen) known = known || value == sum;
		if (!known) seen.push_back(sum);
	}
	return static_cast<int>(seen.size());
}

// Steps through one sweep cycle and checks the fill starts left of the track,
// travels past 70% of its width, and is clipped to the track throughout.
bool expectSweep(double stepMs, int frames)
{
	auto &tree = gea::embedded::ui::Tree::instance();
	const std::uint32_t beside = besideTrackChecksum();
	int minOffset = 1 << 30;
	int maxOffset = -(1 << 30);
	std::vector<std::uint32_t> seen;
	for (int i = 0; i < frames; ++i) {
		advance(stepMs);
		const int track = trackOf(kDemo);
		const int fill = fillOf(kDemo);
		if (track < 0 || fill < 0) return false;
		const int offset = tree.node(fill).layout.x - tree.node(track).layout.x;
		if (offset < minOffset) minOffset = offset;
		if (offset > maxOffset) maxOffset = offset;
		if (besideTrackChecksum() != beside) {
			std::fprintf(stderr, "[%s] busy: the fill painted outside the track at frame %d (offset %d)\n", kTest, i, offset);
			return false;
		}
		const std::uint32_t sum = trackRowChecksum();
		bool known = false;
		for (const auto value : seen) known = known || value == sum;
		if (!known) seen.push_back(sum);
	}
	const int trackWidth = tree.node(trackOf(kDemo)).layout.width;
	if (minOffset >= -trackWidth / 10 || maxOffset <= trackWidth * 7 / 10 || seen.size() < 3) {
		std::fprintf(stderr,
		             "[%s] busy: sweep offsets %d..%d on a %d track, %zu distinct frames\n",
		             kTest,
		             minOffset,
		             maxOffset,
		             trackWidth,
		             seen.size());
		return false;
	}
	return true;
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	advance(16);

	if (trackOf(kDemo) < 0 || fillOf(kDemo) < 0) {
		dumpTree(kTest);
		return 1;
	}
	const int wideTrack = tree.node(trackOf(kDemo)).layout.width;
	bool ok = wideTrack > 0;
	if (!ok) std::fprintf(stderr, "[%s] track has no width\n", kTest);

	// The fixed bars: no props draws empty with no label, min/max default to
	// 0-100, the width keeps a tenth of a percent, a negative range works, and
	// an inverted range or a NaN value draws empty.
	ok = ok && expectFill("demo-case--defaults", 0, "no props");
	ok = ok && expectNoLabel("demo-case--defaults", "no props label");
	ok = ok && expectFill("demo-case--default-range", 250, "default 0-100 range");
	ok = ok && expectWidthPercent("demo-case--default-range", 250, "default 0-100 range width");
	ok = ok && expectFill("demo-case--fraction", 333, "fractional value");
	ok = ok && expectWidthPercent("demo-case--fraction", 333, "fractional value width");
	ok = ok && expectNoLabel("demo-case--fraction", "empty label");
	ok = ok && expectFill("demo-case--negative", 500, "negative range");
	ok = ok && expectWidthPercent("demo-case--inverted", 0, "inverted range");
	ok = ok && expectWidthPercent("demo-case--nan", 0, "NaN value");
	// A native build parses "NaN%" as 0, so the width alone cannot show that
	// progressFraction catches NaN; the demo prints its result in the label.
	ok = ok && expectText("NaN gives [0]", "NaN fraction");

	// The app's id lands on the bar's root.
	if (ok) {
		const int root = onlyNodeWithClass(kDemo);
		const int byId = gea::embedded::ui::Document::instance().getElementById("demo-bar").id();
		if (root < 0 || byId != root) {
			std::fprintf(stderr, "[%s] id: #demo-bar is node %d, the .demo-bar root is %d\n", kTest, byId, root);
			ok = false;
		}
	}

	ok = ok && expectFill(kDemo, 400, "initial 40/100");
	ok = ok && expectText("value 40 of 0-100", "initial label");
	const Pixel solidFill = fillCenterPixel();
	Pixel emptyTrack = 0;
	if (ok) {
		const auto &layout = tree.node(trackOf(kDemo)).layout;
		emptyTrack = displayPixelAt(layout.x + layout.width - 4, trackMidY(trackOf(kDemo)));
		if (solidFill == emptyTrack) {
			std::fprintf(stderr, "[%s] initial: fill and track paint the same colour\n", kTest);
			ok = false;
		}
	}

	ok = ok && click("Plus") && expectFill(kDemo, 650, "after Plus");
	ok = ok && click("Over") && expectFill(kDemo, 1000, "value above max");
	ok = ok && expectWidthPercent(kDemo, 1000, "value above max width");
	ok = ok && click("Under") && expectFill(kDemo, 0, "value below min");
	ok = ok && expectWidthPercent(kDemo, 0, "value below min width");

	ok = ok && click("Reset") && click("Zero range") && expectFill(kDemo, 0, "zero range");
	ok = ok && expectText("value 40 of 50-50 (zero range)", "zero range label");

	ok = ok && click("Reset") && click("Resize");
	if (ok) {
		const int narrow = trackOf(kDemo);
		if (narrow < 0 || tree.node(narrow).layout.width * 2 != wideTrack) {
			std::fprintf(stderr, "[%s] resize: track %d, expected half of %d\n", kTest, narrow < 0 ? -1 : tree.node(narrow).layout.width, wideTrack);
			ok = false;
		}
	}
	ok = ok && expectFill(kDemo, 400, "after resize");

	ok = ok && click("Reset") && click("Busy");
	ok = ok && expectText("Working", "busy label");
	if (ok && !tree.hasClass(fillOf(kDemo), "progress-bar__fill--indeterminate")) {
		std::fprintf(stderr, "[%s] busy: fill lacks the indeterminate class\n", kTest);
		ok = false;
	}
	ok = ok && expectFill(kDemo, 300, "busy segment");
	ok = ok && expectSweep(100, 12);

	ok = ok && click("Still");
	if (ok && (tree.hasClass(fillOf(kDemo), "progress-bar__fill--indeterminate") || !tree.hasClass(fillOf(kDemo), "progress-bar__fill--static"))) {
		std::fprintf(stderr, "[%s] reduced motion: expected only the static class on the fill\n", kTest);
		ok = false;
	}
	ok = ok && expectFill(kDemo, 1000, "reduced motion fill");
	ok = ok && expectFillAtStart("reduced motion offset");
	if (ok) {
		advance(16);
		const Pixel dimmed = fillCenterPixel();
		if (dimmed == solidFill || dimmed == emptyTrack) {
			std::fprintf(stderr, "[%s] reduced motion: expected a dimmed fill, got %x (solid %x, track %x)\n", kTest, static_cast<unsigned>(dimmed), static_cast<unsigned>(solidFill), static_cast<unsigned>(emptyTrack));
			ok = false;
		}
	}
	if (ok) {
		const int distinct = distinctChecksums(12, 100);
		if (distinct != 1) {
			std::fprintf(stderr, "[%s] reduced motion: expected a still bar, %d distinct frames\n", kTest, distinct);
			ok = false;
		}
	}

	ok = ok && click("Still") && click("Busy");
	if (ok && tree.hasClass(fillOf(kDemo), "progress-bar__fill--indeterminate")) {
		std::fprintf(stderr, "[%s] idle: fill kept the indeterminate class\n", kTest);
		ok = false;
	}
	ok = ok && expectFill(kDemo, 400, "back to determinate");
	ok = ok && expectFillAtStart("idle offset");
	if (ok) {
		advance(16);
		const int distinct = distinctChecksums(12, 100);
		if (distinct != 1) {
			std::fprintf(stderr, "[%s] idle: expected the sweep to stop, %d distinct frames\n", kTest, distinct);
			ok = false;
		}
	}

	if (!ok) {
		dumpTree(kTest);
		return 1;
	}
	return 0;
}
