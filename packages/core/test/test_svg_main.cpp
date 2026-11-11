// Inline SVG: the geometry core (svg_geometry.h) and the <svg> element in the
// node tree (svg_element.h).
//
// Geometry: path-data parsing (every command, absolute and relative, implicit
// repeats, compact arc flags, error recovery), arc endpoints, transform lists,
// colours and their style encoding, and the coverage rasterizer's exactness
// (areas, fractional edges, even-odd holes, seamless strips, stroke outlines).
//
// Tree: an svg renders its shapes into the framebuffer through the display
// list, scaled by viewBox; CSS paint overrides presentation attributes; class
// and attribute changes repaint; touch targets the shape under the finger by
// geometry; pointer-events and display behave as in a browser.
#include "native_test_harness.h"

#include "canvas.h"
#include "display.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/svg_geometry.h"
#include "ui/tree_internal.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }

using namespace gea::embedded::ui;
using namespace gea::embedded::test;
namespace pixel = gea::framework::graphics::pixel;
namespace svg = gea::embedded::ui::svg;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
	if (ok) return;
	++failures;
	std::fprintf(stderr, "[svg] FAIL: %s\n", what);
}

bool near(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

svg::Path parse(const char *d, svg::Matrix m = {}, bool *ok = nullptr)
{
	svg::Path path;
	svg::PathBuilder b(path, m, 0.05f);
	const bool parsed = svg::parsePathData(d, b);
	if (ok) *ok = parsed;
	return path;
}

// Total coverage of contours over a rectangle, in pixels (255 = one pixel).
double coverageArea(const std::vector<svg::Contour> &contours, int x0, int y0, int x1, int y1, bool evenOdd = false)
{
	svg::Rasterizer r;
	svg::ScratchVector<std::uint8_t> row;
	double sum = 0;
	svg::fillCoverage(r, row, contours, x0, y0, x1, y1, evenOdd, 1.0f, [&](int, int, const std::uint8_t *cov, int n) {
		for (int i = 0; i < n; ++i) sum += cov[i];
	});
	return sum / 255.0;
}

void geometryTests()
{
	// M/L/H/V/Z absolute.
	{
		const auto p = parse("M10 20 L30 20 H40 V50 Z");
		check(p.contours.size() == 1 && p.closed[0], "absolute path is one closed contour");
		const auto &c = p.contours[0];
		check(c.size() == 4 && near(c[2].x, 40) && near(c[2].y, 20) && near(c[3].x, 40) && near(c[3].y, 50), "H and V move one axis");
	}
	// Relative commands, implicit lineTo after moveTo, comma/space/sign packing.
	{
		const auto p = parse("m10,10 10,0-5 5h-2v-3z m20 0 l1 1");
		check(p.contours.size() == 2, "relative path: two subpaths");
		const auto &c = p.contours[0];
		check(c.size() == 5 && near(c[1].x, 20) && near(c[2].x, 15) && near(c[2].y, 15) && near(c[3].x, 13) && near(c[4].y, 12),
		      "relative coordinates accumulate, implicit lineTo after m");
		// After z the current point returns to the subpath start (10,10).
		check(near(p.contours[1][0].x, 30) && near(p.contours[1][0].y, 10), "m after z is relative to the closed start");
	}
	// Cubic and smooth cubic end exactly on their endpoints; S reflects.
	{
		const auto p = parse("M0 0 C0 10 10 10 10 0 S20 -10 20 0");
		const auto &c = p.contours[0];
		check(c.size() > 6 && near(c.back().x, 20) && near(c.back().y, 0), "cubic chain ends on its endpoint");
		float minY = 0, maxY = 0;
		for (const auto &pt : c) { minY = std::min(minY, pt.y); maxY = std::max(maxY, pt.y); }
		check(near(maxY, 7.5f, 0.1f) && near(minY, -7.5f, 0.1f), "C bulges +7.5 and the reflected S bulges -7.5");
	}
	// Quadratic + smooth quadratic, relative.
	{
		const auto p = parse("M0 0 q5 10 10 0 t10 0");
		const auto &c = p.contours[0];
		float maxY = -1e9f, minY = 1e9f;
		for (const auto &pt : c) { maxY = std::max(maxY, pt.y); minY = std::min(minY, pt.y); }
		check(near(c.back().x, 20) && near(maxY, 5, 0.05f) && near(minY, -5, 0.05f), "q peaks at +5, reflected t at -5");
	}
	// Arcs: a semicircle through the bottom, compact flags ("0 01" style).
	{
		const auto p = parse("M0 0 A10 10 0 0 0 20 0");
		const auto &c = p.contours[0];
		float maxY = -1e9f;
		for (const auto &pt : c) maxY = std::max(maxY, pt.y);
		check(near(c.back().x, 20) && near(maxY, 10, 0.05f), "sweep-0 arc from (0,0) to (20,0) dips to y=10");
		const auto q = parse("M0 0a10 10 0 0120 0");
		float minY = 1e9f;
		for (const auto &pt : q.contours[0]) minY = std::min(minY, pt.y);
		check(near(q.contours[0].back().x, 20) && near(minY, -10, 0.05f), "compact flags 01: sweep-1 arc rises to y=-10");
		// Radii too small are scaled up so the arc still reaches its endpoint.
		const auto r = parse("M0 0 A1 1 0 0 1 40 0");
		check(near(r.contours[0].back().x, 40) && r.contours[0].size() > 8, "undersized radii scale up");
		// A large arc goes the long way round: a 270 degree arc.
		const auto l = parse("M10 0 A10 10 0 1 1 0 -10");
		float lmaxY = -1e9f;
		for (const auto &pt : l.contours[0]) lmaxY = std::max(lmaxY, pt.y);
		check(near(lmaxY, 10, 0.05f), "large-arc flag takes the 270 degree path");
	}
	// Errors keep everything before them.
	{
		bool ok = true;
		const auto p = parse("M0 0 L10 0 L10 10 X 5 5", {}, &ok);
		check(!ok && p.contours.size() == 1 && p.contours[0].size() == 3, "error: geometry before it survives");
	}
	// Transforms.
	{
		svg::Matrix m;
		check(svg::parseTransform("translate(10 20) rotate(90) scale(2)", m), "transform list parses");
		const auto pt = m.apply({1, 0});
		check(near(pt.x, 10) && near(pt.y, 22), "translate*rotate*scale applies right to left");
		check(svg::parseTransform("rotate(180, 50, 50)", m), "rotate about a centre parses");
		const auto q = m.apply({60, 50});
		check(near(q.x, 40) && near(q.y, 50), "rotate(180 50 50) mirrors about the centre");
		check(!svg::parseTransform("rotate(", m) && near(m.apply({3, 4}).x, 3), "malformed transform is identity");
		const float vb[4] = {0, 0, 100, 50};
		const auto v = svg::viewBoxTransform(vb, 200, 200, nullptr);
		const auto c = v.apply({50, 25});
		check(near(c.x, 100) && near(c.y, 100) && near(v.scale(), 2), "viewBox meet centres and scales uniformly");
		const auto n = svg::viewBoxTransform(vb, 200, 200, "none").apply({100, 50});
		check(near(n.x, 200) && near(n.y, 200), "preserveAspectRatio=none stretches");
	}
	// Colours and paint encoding.
	{
		svg::Color c;
		check(svg::parseColor("#2196f3", c) && c.r == 0x21 && c.g == 0x96 && c.b == 0xf3 && c.a == 255, "#rrggbb");
		check(svg::parseColor("#f00a", c) && c.r == 255 && c.a == 0xaa, "#rgba");
		check(svg::parseColor("rgba(10, 20, 30, 0.5)", c) && c.b == 30 && c.a == 128, "rgba()");
		check(svg::parseColor("rgb(100% 0% 0% / 25%)", c) && c.r == 255 && c.a == 64, "space syntax with / alpha");
		check(svg::parseColor("hsl(120, 100%, 50%)", c) && c.g == 255 && c.r == 0, "hsl()");
		check(svg::parseColor("SteelBlue", c) && c.r == 0x46, "keywords are case-insensitive");
		check(svg::parsePaint("none").kind == svg::PaintKind::None, "none");
		check(svg::parsePaint("currentColor").kind == svg::PaintKind::CurrentColor, "currentColor");
		check(svg::parsePaint("transparent").kind == svg::PaintKind::None, "transparent paints nothing");
		check(svg::parsePaint("bogus").kind == svg::PaintKind::Unset, "invalid paint is unset");
		const auto p = svg::decodePaint(svg::encodePaint(svg::parsePaint("rgba(1,2,3,0.5)")));
		check(p.kind == svg::PaintKind::Color && p.color.r == 1 && p.color.g == 2 && p.color.b == 3 && std::abs(p.color.a - 128) <= 2,
		      "paint encoding round-trips colour and alpha");
		check(svg::decodePaint(svg::encodePaint(svg::parsePaint("#fff"))).color.a == 255, "opaque stays opaque");
	}
	// Rasterizer: exact area for axis-aligned and fractional rectangles.
	{
		svg::Path sq;
		svg::PathBuilder b(sq, {});
		svg::addRect(b, 10, 10, 20, 10, 0, 0);
		check(near(static_cast<float>(coverageArea(sq.contours, 0, 0, 64, 64)), 200, 1.0f), "20x10 rect covers 200 px");
		svg::Path frac;
		svg::PathBuilder fb(frac, {});
		svg::addRect(fb, 10.25f, 10.5f, 7.5f, 3.25f, 0, 0);
		check(near(static_cast<float>(coverageArea(frac.contours, 0, 0, 64, 64)), 24.375f, 0.3f), "fractional rect area is exact");
		// Circle area: the flattened circle is the inscribed polygon whose chords
		// sag 0.1px, short of pi r^2 by about pi r^2 theta^2 / 6 (~8.4px here).
		svg::Path circle;
		svg::PathBuilder cb(circle, {}, 0.1f);
		svg::addEllipse(cb, 32, 32, 20, 20);
		const double circleArea = coverageArea(circle.contours, 0, 0, 64, 64);
		check(near(static_cast<float>(circleArea), 3.14159f * 400 - 8.4f, 1.5f), "circle area is the inscribed polygon's");
		// Clipped to the region: only the inside quarter counts.
		check(near(static_cast<float>(coverageArea(circle.contours, 32, 32, 64, 64)), 3.14159f * 100 - 2.1f, 1.0f),
		      "clipping to a quadrant keeps exactly the quarter");
		// Strips: a tall shape rasterized at widths that force many strips.
		svg::Path tall;
		svg::PathBuilder tb(tall, {});
		svg::addRect(tb, 0.5f, 0.5f, 900, 100, 0, 0);
		check(near(static_cast<float>(coverageArea(tall.contours, 0, 0, 1000, 120)), 90000, 20), "strips leave no seams");
		// Even-odd ring: outer 40x40 minus inner 20x20, same winding.
		auto ring = parse("M0 0H40V40H0Z M10 10H30V30H10Z");
		check(near(static_cast<float>(coverageArea(ring.contours, 0, 0, 64, 64, true)), 1200, 1), "evenodd punches the hole");
		check(near(static_cast<float>(coverageArea(ring.contours, 0, 0, 64, 64, false)), 1600, 1), "nonzero fills it");
	}
	// Stroker: a horizontal line of width 4 with butt, square and round caps.
	{
		svg::Path line;
		svg::PathBuilder b(line, {});
		b.moveTo({10, 20});
		b.lineTo({50, 20});
		b.finish();
		std::vector<svg::Contour> outline;
		svg::StrokeStyle style;
		style.width = 4;
		svg::strokePath(line, style, outline);
		check(near(static_cast<float>(coverageArea(outline, 0, 0, 64, 64)), 160, 1), "butt-capped stroke is length x width");
		outline.clear();
		style.cap = svg::LineCap::Square;
		svg::strokePath(line, style, outline);
		check(near(static_cast<float>(coverageArea(outline, 0, 0, 64, 64)), 176, 1), "square caps add width/2 each end");
		outline.clear();
		style.cap = svg::LineCap::Round;
		svg::strokePath(line, style, outline);
		check(near(static_cast<float>(coverageArea(outline, 0, 0, 64, 64)), 160 + 3.14159f * 4, 1.0f), "round caps add a disc");
		// Overlapping pieces (a sharp polyline with joins) never exceed full coverage.
		auto zig = parse("M5 5 L40 30 L5 55 L40 60");
		outline.clear();
		style.width = 6;
		style.join = svg::LineJoin::Round;
		svg::strokePath(zig, style, outline);
		svg::Rasterizer r;
		svg::ScratchVector<std::uint8_t> row;
		bool saturated = true;
		int fullPixels = 0;
		svg::fillCoverage(r, row, outline, 0, 0, 64, 64, false, 1.0f, [&](int, int, const std::uint8_t *cov, int n) {
			for (int i = 0; i < n; ++i) fullPixels += cov[i] == 255;
		});
		check(saturated && fullPixels > 400, "joined stroke pieces union without cancelling");
		// Hit testing helpers.
		check(svg::distanceToPath(line, {30, 22}) <= 2.01f, "distance to a segment");
		const auto ring = parse("M0 0H40V40H0Z M10 10H30V30H10Z");
		check(svg::insideFill(ring.contours, {5, 5}, true) && !svg::insideFill(ring.contours, {20, 20}, true),
		      "evenodd hit test excludes the hole");
	}
}

// ---------------------------------------------------------------------------

const auto kWhite = pixel::nativeColor(255, 255, 255);
const auto kRed = pixel::nativeColor(255, 0, 0);
const auto kGreen = pixel::nativeColor(0, 255, 0);
const auto kBlue = pixel::nativeColor(0, 0, 255);

bool pixelIs(int x, int y, pixel::native_t expected, const char *label)
{
	const auto actual = displayPixelAt(x, y);
	if (actual == expected) return true;
	std::fprintf(stderr, "[svg] %s at (%d,%d): expected %06x got %06x\n", label, x, y,
	             static_cast<unsigned>(expected), static_cast<unsigned>(actual));
	++failures;
	return false;
}

NodeHandle element(const char *tag, std::initializer_list<std::pair<const char *, const char *>> attrs)
{
	auto node = Document::instance().createElement(tag);
	for (const auto &[name, value] : attrs) node.setAttribute(name, value);
	return node;
}

void treeTests()
{
	resetNativeHost();
	setNativeDisplaySize(200, 120);
	setViewportMetrics(200, 120, 1.0);
	auto &sheet = StyleSheet::instance();
	sheet.clear();
	sheet.registerRule("hot", "fill", "#00ff00");
	sheet.registerRule("thick", "stroke-width", "6px");
	auto &doc = Document::instance();
	auto &tree = Tree::instance();

	auto root = doc.createView();
	root.setTagName("div");
	root.style().width(200);
	root.style().height(120);
	root.style().backgroundColor(0xffff);

	// 100x50 viewBox drawn at 200x100: everything doubles.
	auto chart = element("svg", {{"width", "200"}, {"height", "100"}, {"viewBox", "0 0 100 50"}});
	auto dot = element("circle", {{"cx", "20"}, {"cy", "20"}, {"r", "10"}, {"fill", "#ff0000"}});
	auto group = element("g", {{"transform", "translate(50 0)"}, {"fill", "#0000ff"}});
	auto box = element("rect", {{"x", "10"}, {"y", "10"}, {"width", "20"}, {"height", "20"}});
	auto ghost = element("circle", {{"cx", "90"}, {"cy", "40"}, {"r", "6"}, {"fill", "#ff0000"}, {"pointer-events", "none"}});
	auto line = element("path", {{"d", "M0 45 H100"}, {"stroke", "#0000ff"}, {"stroke-width", "2"}, {"fill", "none"}});
	group.appendChild(box);
	chart.appendChild(dot);
	chart.appendChild(group);
	chart.appendChild(ghost);
	chart.appendChild(line);
	root.appendChild(chart);
	doc.mount(root, 200, 120);
	doc.refresh(root, 200, 120);

	{
		// A frame with nothing changed repaints nothing.
		doc.refresh(root, 200, 120);
		const int before = flushPixelCount();
		doc.refresh(root, 200, 120);
		doc.refresh(root, 200, 120);
		if (flushPixelCount() != before) std::fprintf(stderr, "[svg] idle refresh flushed %d px\n", flushPixelCount() - before);
		check(flushPixelCount() == before, "an idle refresh presents nothing");
	}
	check(tree.node(chart.id()).layout.width == 200 && tree.node(chart.id()).layout.height == 100,
	      "width/height attributes size the svg box");
	check(tree.node(dot.id()).layout.width == 0, "shapes take no layout box");
	pixelIs(40, 40, kRed, "circle centre (20,20)*2");
	pixelIs(40, 5, kWhite, "outside the circle");
	pixelIs(80, 40, kWhite, "the circle edge is at x=60");
	// Group transform + inherited fill: rect at (60..80, 10..30) user = (120..160, 20..60) px.
	pixelIs(140, 40, kBlue, "rect inherits fill from <g> and its translate");
	pixelIs(115, 40, kWhite, "left of the translated rect");
	// Stroke: y=45 user, width 2 -> rows 88..92 px.
	pixelIs(100, 90, kBlue, "stroke centre line");
	pixelIs(100, 86, kWhite, "above the scaled stroke");
	// Anti-aliasing: a pixel on the circle's rim is a blend, not either colour.
	{
		bool blended = false;
		for (int x = 55; x <= 62 && !blended; ++x) {
			const auto p = displayPixelAt(x, 40);
			blended = p != kRed && p != kWhite;
		}
		check(blended, "circle rim is anti-aliased");
	}

	// Hit testing by geometry.
	check(tree.hitTestNode(40, 40) == dot.id(), "tap inside the circle hits the circle");
	check(tree.hitTestNode(140, 40) == box.id(), "tap inside the rect hits the rect");
	check(tree.hitTestNode(100, 10) == chart.id(), "tap on empty svg hits the svg");
	check(tree.hitTestNode(100, 91) == line.id(), "tap on the stroke hits the path");
	check(tree.hitTestNode(180, 80) == chart.id(), "pointer-events=none shape is transparent to touch");

	// Touch dispatch: the click lands on the circle and bubbles through svg.
	{
		int circleClicks = 0, svgClicks = 0;
		dot.addEventListener("click", [&](gea::framework::events::PointerEvent &) { ++circleClicks; });
		chart.addEventListener("click", [&](gea::framework::events::PointerEvent &) { ++svgClicks; });
		dispatchClick(tree.hitTestNode(40, 40));
		check(circleClicks == 1 && svgClicks == 1, "click on a circle fires on it and bubbles to the svg");
	}

	// CSS beats the presentation attribute; a class toggle repaints.
	dot.classList().add("hot");
	doc.refresh(root, 200, 120);
	pixelIs(40, 40, kGreen, "class fill overrides fill attribute");
	dot.classList().remove("hot");
	doc.refresh(root, 200, 120);
	pixelIs(40, 40, kRed, "removing the class restores the attribute fill");

	// Inline style fill and currentColor.
	dot.style().setProperty("fill", "currentColor");
	dot.style().setProperty("color", "#0000ff");
	doc.refresh(root, 200, 120);
	pixelIs(40, 40, kBlue, "fill: currentColor follows color");
	dot.style().setProperty("fill", "#ff0000");
	doc.refresh(root, 200, 120);

	// An attribute change moves the shape and repaints only what it must.
	dot.setAttribute("cx", "70");
	dot.setAttribute("cy", "35");
	doc.refresh(root, 200, 120);
	pixelIs(40, 40, kWhite, "old circle position cleared");
	pixelIs(140, 80, kRed, "circle repainted at (70,35)");
	pixelIs(140, 50, kBlue, "document order: the later rect paints over the circle");

	// Long path data survives (the generic attribute store is bounded).
	std::string d = "M0 0";
	for (int i = 1; i <= 60; ++i) d += " L" + std::to_string(i) + " " + std::to_string(i % 2 ? 2 : 0);
	line.setAttribute("d", d.c_str());
	check(std::string(line.getAttribute("d")) == d, "getAttribute returns the full path data");

	// display:none hides the shape and removes it from hit testing.
	box.style().setProperty("display", "none");
	doc.refresh(root, 200, 120);
	check(tree.hitTestNode(150, 25) != box.id(), "display:none shape is not hit");

	// opacity on a group multiplies into its shapes.
	box.style().setProperty("display", "block");
	group.setAttribute("opacity", "0.5");
	dot.setAttribute("cx", "20");
	dot.setAttribute("cy", "20");
	doc.refresh(root, 200, 120);
	{
		const auto p = displayPixelAt(150, 25);
		int r, g, b, a;
		pixel::unpackNative8(p, &r, &g, &b, &a);
		check(b > 100 && r > 100 && g > 100 && b > r, "half-opacity group blends its rect with the background");
	}

	// The stroke-width class: 6 user px -> 12 device px at viewBox scale 2.
	line.classList().add("thick");
	line.setAttribute("d", "M0 45 H100");
	doc.refresh(root, 200, 120);
	pixelIs(100, 85, kBlue, "CSS stroke-width scales with the viewBox");
}


// GEA_SVG_BENCH=1: the svg-charts example's three charts at 800x480, every
// chart's geometry changing each frame. Prints the mean frame and the share
// spent in rasterization.
std::string chartLine(int t)
{
	std::string d;
	for (int i = 0; i < 24; ++i) {
		const double v = 50 + 30 * std::sin((i + t) / 3.0) + 15 * std::sin((i + t) / 1.7);
		char buf[48];
		std::snprintf(buf, sizeof buf, "%s%.1f %.1f", i ? "L" : "M", i * 300.0 / 23, 140 - v * 1.4);
		d += buf;
	}
	return d;
}

void bench()
{
	resetNativeHost();
	setNativeDisplaySize(800, 480);
	setViewportMetrics(800, 480, 1.0);
	StyleSheet::instance().clear();
	auto &doc = Document::instance();
	auto root = doc.createView();
	root.setTagName("div");
	root.style().width(800);
	root.style().height(480);
	root.style().backgroundColor(0x0000);
	auto line = element("svg", {{"width", "340"}, {"height", "170"}, {"viewBox", "-10 -10 320 160"}});
	auto area = element("path", {{"fill", "#38bdf8"}, {"fill-opacity", "0.18"}});
	auto stroke = element("path", {{"fill", "none"}, {"stroke", "#38bdf8"}, {"stroke-width", "3"}, {"stroke-linecap", "round"}, {"stroke-linejoin", "round"}});
	line.appendChild(area);
	line.appendChild(stroke);
	auto gauge = element("svg", {{"width", "170"}, {"height", "170"}, {"viewBox", "0 0 200 200"}});
	auto arc = element("path", {{"fill", "none"}, {"stroke", "#38bdf8"}, {"stroke-width", "16"}, {"stroke-linecap", "round"}});
	auto needleG = element("g", {});
	auto needle = element("polygon", {{"points", "96,100 104,100 100,34"}, {"fill", "#e2e8f0"}});
	needleG.appendChild(needle);
	gauge.appendChild(arc);
	gauge.appendChild(needleG);
	auto donut = element("svg", {{"width", "170"}, {"height", "170"}, {"viewBox", "0 0 200 200"}});
	std::vector<NodeHandle> slices;
	for (int i = 0; i < 4; ++i) {
		auto sl = element("path", {{"fill", "#a78bfa"}});
		donut.appendChild(sl);
		slices.push_back(sl);
	}
	root.appendChild(line);
	root.appendChild(donut);
	root.appendChild(gauge);
	root.style().setProperty("display", "flex");
	doc.mount(root, 800, 480);
	doc.refresh(root, 800, 480);
	const int frames = std::getenv("GEA_SVG_BENCH_FRAMES") ? std::atoi(std::getenv("GEA_SVG_BENCH_FRAMES")) : 200;
	const auto t0 = std::chrono::steady_clock::now();
	for (int t = 0; t < frames; ++t) {
		const std::string d = chartLine(t);
		stroke.setAttribute("d", d.c_str());
		area.setAttribute("d", (d + "L300 140L0 140Z").c_str());
		const double v = 50 + 45 * std::sin(t / 6.0);
		const double a0 = 150 * M_PI / 180, a1 = a0 + v / 100 * 240 * M_PI / 180;
		char buf[160];
		std::snprintf(buf, sizeof buf, "M%.1f %.1fA80 80 0 %d 1 %.1f %.1f", 100 + 80 * std::cos(a0), 100 + 80 * std::sin(a0),
		              a1 - a0 > M_PI ? 1 : 0, 100 + 80 * std::cos(a1), 100 + 80 * std::sin(a1));
		arc.setAttribute("d", buf);
		std::snprintf(buf, sizeof buf, "rotate(%.1f 100 100)", -120 + v * 2.4);
		needleG.setAttribute("transform", buf);
		double angle = -M_PI / 2;
		for (int i = 0; i < 4; ++i) {
			const double sweep = M_PI / 2 + 0.3 * std::sin((t + i) / 5.0) * (i % 2 ? 1 : -1);
			const double e = angle + sweep - 0.03, s0 = angle + 0.03;
			std::snprintf(buf, sizeof buf, "M%.1f %.1fA90 90 0 0 1 %.1f %.1fL%.1f %.1fA58 58 0 0 0 %.1f %.1fZ",
			              100 + 90 * std::cos(s0), 100 + 90 * std::sin(s0), 100 + 90 * std::cos(e), 100 + 90 * std::sin(e),
			              100 + 58 * std::cos(e), 100 + 58 * std::sin(e), 100 + 58 * std::cos(s0), 100 + 58 * std::sin(s0));
			slices[i].setAttribute("d", buf);
			angle += sweep;
		}
		doc.refresh(root, 800, 480);
	}
	const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::printf("[svg-bench] %.3f ms/frame over %d frames\n", total / frames, frames);
}

}  // namespace

int main()
{
	if (std::getenv("GEA_SVG_BENCH")) {
		bench();
		return 0;
	}
	geometryTests();
	treeTests();
	if (failures) {
		std::fprintf(stderr, "[svg] %d failure(s)\n", failures);
		return 1;
	}
	std::printf("[svg] ok\n");
	return 0;
}
