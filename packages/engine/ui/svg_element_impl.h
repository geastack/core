// SPDX-License-Identifier: Apache-2.0
//
// Implementation of svg_element.h. Included once, from tree_render.cpp, so
// every target's existing engine source list builds it with no manifest
// change. Do not include it anywhere else.

#if GEA_UI_SVG

#include "svg_element.h"
#include "svg_geometry.h"

#include "display.h"
#include "internal.h"
#include "tree_internal.h"
#include "tree_state.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define GEA_SVG_HAS_FREERTOS 1
#else
#define GEA_SVG_HAS_FREERTOS 0
#endif
#if __has_include("pico.h")
#include "pico.h"
#define GEA_SVG_HAS_PICO 1
#else
#define GEA_SVG_HAS_PICO 0
#endif

namespace gea::embedded::ui::svg {

namespace {

enum class Kind : std::uint8_t { None, Svg, Group, Path, Polyline, Polygon, Line, Circle, Ellipse, Rect, Skip };

struct TagIds {
	std::int16_t svg, g, path, polyline, polygon, line, circle, ellipse, rect, defs, title, desc, text, linearGradient, stop;
	// The element factory lowercases tag names the HTML way, so a JSX
	// <linearGradient> arrives as "lineargradient"; accept both spellings.
	std::int16_t linearGradientLower;
	bool isLinearGradient(std::int16_t tag) const { return tag == linearGradient || tag == linearGradientLower; }
};

const TagIds &tagIds()
{
	static const TagIds ids = {internTag("svg"),      internTag("g"),      internTag("path"),   internTag("polyline"),
	                           internTag("polygon"),  internTag("line"),   internTag("circle"), internTag("ellipse"),
	                           internTag("rect"),     internTag("defs"),   internTag("title"),  internTag("desc"),
	                           internTag("text"),     internTag("linearGradient"), internTag("stop"),
	                           internTag("lineargradient")};
	return ids;
}

// -2 until the first SVG tag is interned: isRoot is then a single compare that
// can never match, and the tag table is never touched by apps without <svg>.
std::int16_t g_svgTag = -2;

Kind kindOf(std::int16_t tag)
{
	if (g_svgTag == -2) return Kind::None;
	const TagIds &t = tagIds();
	if (tag == t.svg) return Kind::Svg;
	if (tag == t.g) return Kind::Group;
	if (tag == t.path) return Kind::Path;
	if (tag == t.polyline) return Kind::Polyline;
	if (tag == t.polygon) return Kind::Polygon;
	if (tag == t.line) return Kind::Line;
	if (tag == t.circle) return Kind::Circle;
	if (tag == t.ellipse) return Kind::Ellipse;
	if (tag == t.rect) return Kind::Rect;
	if (tag == t.defs || tag == t.title || tag == t.desc || tag == t.text || t.isLinearGradient(tag) || tag == t.stop) return Kind::Skip;
	return Kind::None;
}

bool isSvgTagName(const char *tag)
{
	static const char *const names[] = {"svg", "g", "path", "polyline", "polygon", "line", "circle",
	                                    "ellipse", "rect", "defs", "title", "desc", "text", "linearGradient", "lineargradient", "stop"};
	for (const char *n : names)
		if (std::strcmp(tag, n) == 0) return true;
	return false;
}

struct Attrs {
	std::vector<std::pair<std::string, std::string>> values;
	std::uint32_t generation = 0;
	const char *get(const char *name) const
	{
		for (const auto &kv : values)
			if (kv.first == name) return kv.second.c_str();
		return nullptr;
	}
};

// A linearGradient paint: its vector (svg-local pixels, or fractions of the
// shape's box with bboxUnits) and up to kMaxStops stops, opacity folded in.
struct GradientPaint {
	static constexpr int kMaxStops = 8;
	bool bboxUnits = true;
	float x1 = 0, y1 = 0, x2 = 1, y2 = 0;
	int stops = 0;
	float offset[kMaxStops] = {};
	Color color[kMaxStops];
	float alpha[kMaxStops] = {};
};

struct ResolvedPaint {
	bool on = false;
	gea::framework::graphics::pixel::native_t color = 0;
	float alpha = 1.0f;
	bool gradient = false;
	GradientPaint grad;
};

// A rebuild's damage as a few boxes rather than one: an svg whose shapes
// change in separate places (two ends of an arc, three gauges' tips) repaints
// each place, not the box spanning them. Boxes that touch, or whose union
// costs little more than both, merge; past kMax the cheapest union is taken.
struct DamageSet {
	static constexpr int kMax = 4;
	Bounds box[kMax];
	int count = 0;
	bool empty() const { return count == 0; }
	static float area(const Bounds &b) { return (b.x1 - b.x0) * (b.y1 - b.y0); }
	static Bounds unite(const Bounds &a, const Bounds &b)
	{
		Bounds u = a;
		u.add(Point{b.x0, b.y0});
		u.add(Point{b.x1, b.y1});
		return u;
	}
	void add(const Bounds &b)
	{
		if (b.empty()) return;
		for (int i = 0; i < count; ++i) {
			const Bounds u = unite(box[i], b);
			const bool touch = box[i].x0 <= b.x1 && b.x0 <= box[i].x1 && box[i].y0 <= b.y1 && b.y0 <= box[i].y1;
			if (touch || area(u) <= area(box[i]) + area(b) + 256.0f) {
				box[i] = u;
				// The grown box may now reach another: fold until stable.
				for (int j = 0; j < count; ++j) {
					if (j == i) continue;
					const Bounds v = unite(box[i], box[j]);
					if (area(v) <= area(box[i]) + area(box[j]) + 256.0f) {
						box[i] = v;
						box[j] = box[--count];
						if (i == count) i = j;
						j = -1;
					}
				}
				return;
			}
		}
		if (count < kMax) {
			box[count++] = b;
			return;
		}
		int best = 0;
		float bestCost = 1e30f;
		for (int i = 0; i < count; ++i) {
			const float cost = area(unite(box[i], b)) - area(box[i]);
			if (cost < bestCost) bestCost = cost, best = i;
		}
		box[best] = unite(box[best], b);
	}
	void clear() { count = 0; }
};

struct Item {
	int node = -1;
	Path geometry;
	EdgeList fillEdges, strokeEdges;  // what paint rasterizes, built once per scene
	ResolvedPaint fill, stroke;
	bool evenOdd = false;
	bool hittable = true;
	float halfStroke = 0.0f;
	Bounds fillBounds, strokeBounds;
	// Everything the geometry and edges depend on (attributes, transform,
	// stroke style): an unchanged item keeps them across scene rebuilds, so a
	// frame that moves one shape re-flattens only that shape.
	std::uint64_t geometryKey = 0;
	// The key without the node's attributes: two builds of one node with the
	// same styleKey differ only in their path, so damage is the path's diff.
	std::uint64_t styleKey = 0;
	// The transform's translation when the geometry was built. The key leaves
	// translation out, so a shape that only moved (a scrolling chart) shifts
	// its edges instead of re-flattening and re-stroking.
	float tx = 0.0f, ty = 0.0f;
};

void translateBounds(Bounds &b, float dx, float dy)
{
	if (b.empty()) return;
	b.x0 += dx;
	b.x1 += dx;
	b.y0 += dy;
	b.y1 += dy;
}

void translateItem(Item &item, float dx, float dy)
{
	for (EdgeList *edges : {&item.fillEdges, &item.strokeEdges})
		for (Edge &e : *edges) {
			e.a.x += dx;
			e.a.y += dy;
			e.b.x += dx;
			e.b.y += dy;
			e.top += dy;
			e.bottom += dy;
		}
	for (Contour &c : item.geometry.contours)
		for (Point &p : c) {
			p.x += dx;
			p.y += dy;
		}
	translateBounds(item.fillBounds, dx, dy);
	translateBounds(item.strokeBounds, dx, dy);
	item.tx += dx;
	item.ty += dy;
}

struct Scene {
	bool dirty = true;
	bool recorded = false;
	// The display list's record serial when the scene was made: once a whole
	// recording has run since and still left the svg out, its box lay outside
	// the record clip (see invalidateRoot).
	std::uint32_t createdSerial = DisplayList::instance().recordSerial();
	bool built = false;  // built at least once
	int width = -1, height = -1;
	std::uint64_t signature = 0;
	std::vector<Item> items;
	// Damage of the last rebuild, in svg-local pixels: the old and new bounds
	// of every item whose geometry or paint changed, and of every item that
	// went away. damageFull asks for the whole box (first build, resize,
	// z-order change, or a change to the svg box itself). Consumed once by
	// refresh's dirty collection through damage().
	bool damageValid = false;
	bool damageFull = true;
	DamageSet damage;
	// The svg box as last recorded, to tell a scene-only change apart from a
	// change to the box's own paint (background, border, position).
	bool boxChanged = true;
	bool haveBox = false;
	int boxX = 0, boxY = 0;
	alignas(ComputedStyle) unsigned char boxStyle[sizeof(ComputedStyle)] = {};
	// Where paint splits the box's rows between the two cores, as a fraction
	// of its height, steered each paint toward equal time on both.
	float split = 0.5f;
};

std::unordered_map<int, Attrs> g_attrs;
std::unordered_map<int, Scene> g_scenes;

Tree &tree() { return Tree::instance(); }

Kind kindOfNode(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || !state.nodeActive[node]) return Kind::None;
	return kindOf(state.nodes[node].tag_id);
}

int rootOf(int node)
{
	auto &state = treeState();
	for (int n = node; n >= 0 && n < state.nodeCount; n = state.nodes[n].parent)
		if (state.nodes[n].tag_id == g_svgTag) return n;
	return -1;
}

void invalidateRoot(int root)
{
	auto it = g_scenes.find(root);
	if (it == g_scenes.end()) return;
	it->second.dirty = true;
	auto &state = treeState();
	Node &n = state.nodes[root];
	tree().markNodeDisplayCommandsDirty(root);
	// A first DrawSvg needs a fresh recording; later ones are replaced in place.
	// Not when a recording since the scene was made already left it out: the
	// svg is outside the record clip (below the fold, or on a pager page out of
	// view), a new recording would leave it out again, and whatever brings it
	// into view (a scroll, a relayout) records it then. Asking anyway rebuilt
	// the whole list every frame for the CrowPanel demo's animated meters on
	// the Analytics page, wherever the pager stood: ~31 ms a frame.
	if (!it->second.recorded && DisplayList::instance().recordSerial() == it->second.createdSerial)
		tree().markDisplayListContentDirty();
	n.render.dirty = 1;
#if GEA_CSS_SCROLLING
	n.render.non_scroll_dirty = 1;
#endif
}

const char *attr(int node, const char *name)
{
	auto it = g_attrs.find(node);
	return it == g_attrs.end() ? nullptr : it->second.get(name);
}

float attrLength(int node, const char *name, float fallback)
{
	float v = fallback;
	const char *s = attr(node, name);
	if (s && parseLength(s, v)) return v;
	return fallback;
}

float attrNumber(int node, const char *name, float fallback)
{
	const char *s = attr(node, name);
	if (!s) return fallback;
	Scanner sc(s);
	float v = fallback;
	if (!sc.number(v)) return fallback;
	if (*sc.rest() == '%') v /= 100.0f;
	return v;
}

// Inherited paint state while walking the svg subtree.
struct Context {
	Paint fill{PaintKind::Color, Color{0, 0, 0, 255}};
	Paint stroke{PaintKind::None, Color{}};
	float strokeWidth = 1.0f;
	float fillOpacity = 1.0f, strokeOpacity = 1.0f;
	LineCap cap = LineCap::Butt;
	LineJoin join = LineJoin::Miter;
	float miterLimit = 4.0f;
	bool evenOdd = false;
	bool visible = true;
	bool hittable = true;
	float opacity = 1.0f;  // group opacity, folded into each shape
	Matrix matrix;
};

ResolvedPaint resolvePaint(const Paint &paint, const Node &node, float opacity)
{
	ResolvedPaint out;
	if (paint.kind == PaintKind::None || paint.kind == PaintKind::Unset) return out;
	if (paint.kind == PaintKind::CurrentColor) {
		out.color = static_cast<gea::framework::graphics::pixel::native_t>(node.computedStyle().text_color);
		out.alpha = opacity * node.computedStyle().text_alpha / 255.0f;
	} else {
		out.color = gea::framework::graphics::pixel::nativeColor(paint.color.r, paint.color.g, paint.color.b);
		out.alpha = opacity * paint.color.a / 255.0f;
	}
	out.on = out.alpha > 0.002f;
	return out;
}

void applyNodePaint(int id, const Node &node, Context &ctx)
{
	// Presentation attributes first; CSS (RareStyle) overrides them.
	if (const char *s = attr(id, "fill")) { const Paint p = parsePaint(s); if (p.kind != PaintKind::Unset) ctx.fill = p; }
	if (const char *s = attr(id, "stroke")) { const Paint p = parsePaint(s); if (p.kind != PaintKind::Unset) ctx.stroke = p; }
	if (const char *s = attr(id, "stroke-width")) { float w; if (parseLength(s, w) && w >= 0) ctx.strokeWidth = w; }
	if (attr(id, "fill-opacity")) ctx.fillOpacity = std::clamp(attrNumber(id, "fill-opacity", 1), 0.0f, 1.0f);
	if (attr(id, "stroke-opacity")) ctx.strokeOpacity = std::clamp(attrNumber(id, "stroke-opacity", 1), 0.0f, 1.0f);
	if (attr(id, "stroke-miterlimit")) ctx.miterLimit = std::max(1.0f, attrNumber(id, "stroke-miterlimit", 4));
	if (const char *s = attr(id, "stroke-linecap")) {
		if (!std::strcmp(s, "round")) ctx.cap = LineCap::Round;
		else if (!std::strcmp(s, "square")) ctx.cap = LineCap::Square;
		else if (!std::strcmp(s, "butt")) ctx.cap = LineCap::Butt;
	}
	if (const char *s = attr(id, "stroke-linejoin")) {
		if (!std::strcmp(s, "round")) ctx.join = LineJoin::Round;
		else if (!std::strcmp(s, "bevel")) ctx.join = LineJoin::Bevel;
		else if (!std::strncmp(s, "miter", 5)) ctx.join = LineJoin::Miter;
	}
	if (const char *s = attr(id, "fill-rule")) ctx.evenOdd = !std::strcmp(s, "evenodd");
	if (const char *s = attr(id, "visibility")) ctx.visible = std::strcmp(s, "hidden") != 0 && std::strcmp(s, "collapse") != 0;
	if (const char *s = attr(id, "pointer-events")) ctx.hittable = std::strcmp(s, "none") != 0;

	const RareStyle &rare = rstyle(node.computedStyle());
	if (rare.svg_fill) ctx.fill = decodePaint(rare.svg_fill);
	if (rare.svg_stroke) ctx.stroke = decodePaint(rare.svg_stroke);
	if (rare.svg_stroke_width >= 0) ctx.strokeWidth = rare.svg_stroke_width / 100.0f;
	if (rare.svg_fill_opacity >= 0) ctx.fillOpacity = rare.svg_fill_opacity / 1000.0f;
	if (rare.svg_stroke_opacity >= 0) ctx.strokeOpacity = rare.svg_stroke_opacity / 1000.0f;
	if (rare.svg_stroke_linecap >= 0) ctx.cap = static_cast<LineCap>(rare.svg_stroke_linecap);
	if (rare.svg_stroke_linejoin >= 0) ctx.join = static_cast<LineJoin>(rare.svg_stroke_linejoin);
	if (rare.svg_fill_rule >= 0) ctx.evenOdd = rare.svg_fill_rule == 1;
	if (node.computedStyle().visibility != 0) ctx.visible = false;
	if (node.computedStyle().pointer_events == 1) ctx.hittable = false;
}

void buildGeometry(int id, Kind kind, const Matrix &m, Path &out)
{
	PathBuilder b(out, m, 0.2f);
	switch (kind) {
	case Kind::Path:
		if (const char *d = attr(id, "d")) parsePathData(d, b);
		break;
	case Kind::Polyline:
	case Kind::Polygon:
		if (const char *p = attr(id, "points")) addPoints(b, p, kind == Kind::Polygon);
		break;
	case Kind::Line:
		b.moveTo({attrLength(id, "x1", 0), attrLength(id, "y1", 0)});
		b.lineTo({attrLength(id, "x2", 0), attrLength(id, "y2", 0)});
		b.finish();
		break;
	case Kind::Circle: {
		const float r = attrLength(id, "r", 0);
		addEllipse(b, attrLength(id, "cx", 0), attrLength(id, "cy", 0), r, r);
		break;
	}
	case Kind::Ellipse: {
		float rx = attrLength(id, "rx", -1), ry = attrLength(id, "ry", -1);
		if (rx < 0) rx = ry;
		if (ry < 0) ry = rx;
		addEllipse(b, attrLength(id, "cx", 0), attrLength(id, "cy", 0), rx, ry);
		break;
	}
	case Kind::Rect: {
		float rx = attrLength(id, "rx", -1), ry = attrLength(id, "ry", -1);
		if (rx < 0) rx = ry;
		if (ry < 0) ry = rx;
		addRect(b, attrLength(id, "x", 0), attrLength(id, "y", 0), attrLength(id, "width", 0),
		        attrLength(id, "height", 0), std::max(rx, 0.0f), std::max(ry, 0.0f));
		break;
	}
	default:
		break;
	}
}

std::uint64_t mixKey(std::uint64_t h, std::uint64_t v)
{
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

std::uint64_t floatBits(float f)
{
	std::uint32_t u;
	std::memcpy(&u, &f, sizeof(u));
	return u;
}

// The previous scene's items, consumed as the new scene reuses them.
bool samePaint(const ResolvedPaint &a, const ResolvedPaint &b)
{
	if (a.on != b.on || a.gradient != b.gradient) return false;
	if (!a.on) return true;
	if (!a.gradient) return a.color == b.color && a.alpha == b.alpha;
	const GradientPaint &g = a.grad, &h = b.grad;
	if (g.bboxUnits != h.bboxUnits || g.x1 != h.x1 || g.y1 != h.y1 || g.x2 != h.x2 || g.y2 != h.y2 || g.stops != h.stops)
		return false;
	for (int i = 0; i < g.stops; ++i)
		if (g.offset[i] != h.offset[i] || g.alpha[i] != h.alpha[i] || g.color[i].r != h.color[i].r ||
		    g.color[i].g != h.color[i].g || g.color[i].b != h.color[i].b)
			return false;
	return true;
}

// Everything an item can touch on screen, in svg-local pixels.
void addItemDamage(DamageSet &damage, const Item &item)
{
	constexpr float kAntialias = 2.0f;
	for (const Bounds *b : {&item.fillBounds, &item.strokeBounds}) {
		if (b->empty()) continue;
		Bounds g;
		g.add(Point{b->x0 - kAntialias, b->y0 - kAntialias});
		g.add(Point{b->x1 + kAntialias, b->y1 + kAntialias});
		damage.add(g);
	}
}

// How far, in half widths, a miter join of `path` reaches from its vertex:
// strokeContour puts a miter tip at hw / cos(turn / 2), and bevels instead
// when that ratio is over the limit; every other outline point stays within
// one half width of the centre line. A flattened curve turns a few degrees
// per vertex, so its joins reach barely past hw -- where the miter limit (4 by
// default, SVG's default join being miter) made a gauge's moving tip damage a
// box ~30 px wider on every side than the stroke can paint.
float miterReach(const Path &path, float miterLimit)
{
	float worst = 1.0f;
	for (std::size_t i = 0; i < path.contours.size(); ++i) {
		const auto &c = path.contours[i];
		const bool closed = path.closed[i] != 0;
		Point first{}, firstDir{}, prev{}, prevDir{};
		bool haveFirst = false, haveDir = false;
		auto turnTo = [&](Point d) {
			if (haveDir) {
				const float cosTurn = std::clamp(prevDir.x * d.x + prevDir.y * d.y, -1.0f, 1.0f);
				const float cosHalf = std::sqrt((1 + cosTurn) * 0.5f);
				if (cosHalf > 1e-4f && 1.0f / cosHalf <= miterLimit) worst = std::max(worst, 1.0f / cosHalf);
			} else {
				firstDir = d;
			}
			prevDir = d;
			haveDir = true;
		};
		auto segmentTo = [&](Point p) {
			const float dx = p.x - prev.x, dy = p.y - prev.y;
			const float len2 = dx * dx + dy * dy;
			if (len2 <= 1e-8f) return false;  // the stroker drops it too
			const float inv = 1.0f / std::sqrt(len2);
			turnTo(Point{dx * inv, dy * inv});
			prev = p;
			return true;
		};
		for (const Point &p : c) {
			if (!haveFirst) {
				first = prev = p;
				haveFirst = true;
				continue;
			}
			segmentTo(p);
		}
		if (closed && haveDir) {
			segmentTo(first);  // the closing segment, then the join back into the first
			turnTo(firstDir);
		}
		if (worst >= miterLimit) return miterLimit;
	}
	return worst;
}

// Damage between two flattenings of one stroked open path. Arcs flatten onto
// a fixed angle grid, so the old and new points coincide over a shared run,
// possibly at an index offset when the start moved too. Each contour aligns
// on that run and damages the two ends outside it (a moved start, a moved
// end), each including the run's boundary point: a stroke piece spans two
// points and a join three, all within `reach` of the centre line. False when
// the contours do not line up (a different count, a closed contour that
// changed); the caller then damages both whole boxes.
bool strokeDiffDamage(const Path &a, const Path &b, float reach, DamageSet &damage)
{
	if (a.contours.size() != b.contours.size()) return false;
	constexpr float kSame = 1.0f / 256.0f;
	auto near = [](Point p, Point q) { return std::fabs(p.x - q.x) <= kSame && std::fabs(p.y - q.y) <= kSame; };
	for (std::size_t i = 0; i < a.contours.size(); ++i) {
		const auto &ca = a.contours[i];
		const auto &cb = b.contours[i];
		const long na = static_cast<long>(ca.size()), nb = static_cast<long>(cb.size());
		if (na == nb) {
			long k = 0;
			while (k < na && near(ca[k], cb[k])) ++k;
			if (k == na) {
				if (a.closed[i] != b.closed[i]) return false;
				continue;
			}
		}
		if (a.closed[i] || b.closed[i]) return false;
		auto range = [&](Bounds &out, const auto &c, long from, long to) {
			for (long k = std::max(0L, from); k <= to && k < static_cast<long>(c.size()); ++k) {
				out.add(Point{c[k].x - reach, c[k].y - reach});
				out.add(Point{c[k].x + reach, c[k].y + reach});
			}
		};
		// Find the shared run: a point of `a` (tried at a few places) that
		// `b` also has, and the offset between their indices.
		long lo = -1, hi = -1, offset = 0;
		for (long probe : {na / 2, na / 4, (3 * na) / 4, 1L, na - 2}) {
			if (probe < 0 || probe >= na) continue;
			for (long j = 0; j < nb; ++j) {
				if (!near(ca[probe], cb[j])) continue;
				offset = j - probe;
				lo = hi = probe;
				while (lo > 0 && lo - 1 + offset >= 0 && near(ca[lo - 1], cb[lo - 1 + offset])) --lo;
				while (hi + 1 < na && hi + 1 + offset < nb && near(ca[hi + 1], cb[hi + 1 + offset])) ++hi;
				break;
			}
			if (lo >= 0) break;
		}
		if (lo < 0) {
			Bounds all;
			range(all, ca, 0, na - 1);
			range(all, cb, 0, nb - 1);
			damage.add(all);
			continue;
		}
		if (!(lo == 0 && offset == 0)) {
			Bounds head;
			range(head, ca, 0, lo);
			range(head, cb, 0, lo + offset);
			damage.add(head);
		}
		if (!(hi == na - 1 && hi + offset == nb - 1)) {
			Bounds tail;
			range(tail, ca, hi, na - 1);
			range(tail, cb, hi + offset, nb - 1);
			damage.add(tail);
		}
	}
	return true;
}

// The previous scene's items, consumed as the new scene reuses them; also
// accumulates the rebuild's damage.
struct ItemCache {
	std::vector<Item> *items = nullptr;
	std::size_t hint = 0;
	long lastTaken = -1;
	bool full = false;
	DamageSet damage;
	Item *take(int node, std::uint64_t key)
	{
		if (!items) return nullptr;
		const std::size_t n = items->size();
		for (std::size_t k = 0; k < n; ++k) {
			const std::size_t index = (hint + k) % n;
			Item &it = (*items)[index];
			if (it.node == node) {
				hint = (index + 1) % n;
				if (it.geometryKey != key) return nullptr;
				// A reused item that moved below one drawn after it changes the
				// stacking of unchanged pixels: repaint everything.
				if (static_cast<long>(index) < lastTaken) full = true;
				lastTaken = static_cast<long>(index);
				return &it;
			}
		}
		return nullptr;
	}
	// The previous item of `node` whose geometry changed: its edge buffers are
	// handed to the rebuilt item, so a shape that changes every frame reuses
	// its allocations instead of regrowing them. Its bounds stay for finish().
	Item *stale(int node)
	{
		if (!items) return nullptr;
		for (Item &it : *items)
			if (it.node == node) return &it;
		return nullptr;
	}
	// Old items nobody reused (removed, hidden or rebuilt) leave damage behind.
	void finish()
	{
		if (!items) return;
		for (const Item &it : *items)
			if (it.node >= 0) addItemDamage(damage, it);
	}
};

// fill/stroke="url(#id)": the svg's <linearGradient id> with its <stop>s.
// Unknown or stopless references paint nothing, as in a browser.
ResolvedPaint resolveGradient(const Paint &paint, int id, const Matrix &matrix, float opacity)
{
	ResolvedPaint out;
	const auto &state = treeState();
	const int root = rootOf(id);
	const std::int16_t stopTag = tagIds().stop;
	int grad = -1;
	for (int n = 0; n < state.nodeCount && grad < 0; ++n) {
		if (!state.nodeActive[n] || !tagIds().isLinearGradient(state.nodes[n].tag_id)) continue;
		const char *gid = attr(n, "id");
		if (gid && !std::strcmp(gid, paint.ref) && rootOf(n) == root) grad = n;
	}
	if (grad < 0) return out;
	GradientPaint &g = out.grad;
	const char *units = attr(grad, "gradientUnits");
	g.bboxUnits = !(units && !std::strcmp(units, "userSpaceOnUse"));
	g.x1 = attrNumber(grad, "x1", 0);
	g.y1 = attrNumber(grad, "y1", 0);
	g.x2 = attrNumber(grad, "x2", 1);
	g.y2 = attrNumber(grad, "y2", 0);
	if (!g.bboxUnits) {
		const Point a = matrix.apply(Point{g.x1, g.y1}), b = matrix.apply(Point{g.x2, g.y2});
		g.x1 = a.x; g.y1 = a.y; g.x2 = b.x; g.y2 = b.y;
	}
	float last = 0;
	bool visible = false;
	for (int s = state.nodes[grad].first_child; s >= 0 && s < state.nodeCount && g.stops < GradientPaint::kMaxStops;
	     s = state.nodes[s].next_sibling) {
		if (!state.nodeActive[s] || state.nodes[s].tag_id != stopTag) continue;
		Color c{0, 0, 0, 255};
		if (const char *sc = attr(s, "stop-color")) parseColor(sc, c);
		const float a = opacity * std::clamp(attrNumber(s, "stop-opacity", 1), 0.0f, 1.0f) * c.a / 255.0f;
		last = std::max(last, std::clamp(attrNumber(s, "offset", 0), 0.0f, 1.0f));
		g.offset[g.stops] = last;
		g.color[g.stops] = c;
		g.alpha[g.stops] = a;
		visible = visible || a > 0.002f;
		++g.stops;
	}
	if (g.stops == 0 || !visible) return out;
	out.on = true;
	out.gradient = true;
	out.alpha = 1.0f;
	out.color = gea::framework::graphics::pixel::nativeColor(g.color[0].r, g.color[0].g, g.color[0].b);
	return out;
}

void walk(int id, const Context &parent, std::vector<Item> &items, ItemCache &cache)
{
	const auto &state = treeState();
	const Node &node = state.nodes[id];
	const Kind kind = kindOf(node.tag_id);
	if (kind == Kind::None || kind == Kind::Skip) return;
	if (node.computedStyle().display == kDisplayNone) return;
	if (const char *d = attr(id, "display"); d && !std::strcmp(d, "none")) return;
	Context ctx = parent;
	applyNodePaint(id, node, ctx);
	if (kind != Kind::Svg) {
		Matrix local;
		if (const char *t = attr(id, "transform")) parseTransform(t, local);
		ctx.matrix = ctx.matrix * local;
		float opacity = std::clamp(attrNumber(id, "opacity", 1), 0.0f, 1.0f);
		opacity *= node.computedStyle().opacity / 255.0f;
		ctx.opacity *= opacity;
	}
	if (kind != Kind::Svg && kind != Kind::Group) {
		if (!ctx.visible || ctx.opacity <= 0) return;
		const ResolvedPaint fill =
				kind == Kind::Line ? ResolvedPaint{}
				: ctx.fill.kind == PaintKind::Url ? resolveGradient(ctx.fill, id, ctx.matrix, ctx.opacity * ctx.fillOpacity)
				                                  : resolvePaint(ctx.fill, node, ctx.opacity * ctx.fillOpacity);
		const ResolvedPaint stroke = ctx.stroke.kind == PaintKind::Url
		                                 ? resolveGradient(ctx.stroke, id, ctx.matrix, ctx.opacity * ctx.strokeOpacity)
		                                 : resolvePaint(ctx.stroke, node, ctx.opacity * ctx.strokeOpacity);
		const float width = ctx.strokeWidth * ctx.matrix.scale();
		const bool stroked = stroke.on && width > 0;
		auto it = g_attrs.find(id);
		std::uint64_t key = mixKey(0x51ed27ull, static_cast<std::uint16_t>(node.tag_id));
		for (float f : {ctx.matrix.a, ctx.matrix.b, ctx.matrix.c, ctx.matrix.d})
			key = mixKey(key, floatBits(f));
		key = mixKey(key, static_cast<std::uint64_t>(fill.on) | static_cast<std::uint64_t>(stroked) << 1 |
		                      static_cast<std::uint64_t>(ctx.evenOdd) << 2);
		if (stroked)
			key = mixKey(key, floatBits(width) | static_cast<std::uint64_t>(ctx.cap) << 32 |
			                      static_cast<std::uint64_t>(ctx.join) << 40);
		if (stroked && ctx.join == LineJoin::Miter) key = mixKey(key, floatBits(ctx.miterLimit));
		const std::uint64_t styleKey = key;
		key = mixKey(key, (static_cast<std::uint64_t>(id) << 32) | (it == g_attrs.end() ? 0u : it->second.generation));
		if (Item *cached = cache.take(id, key)) {
			Item item = std::move(*cached);
			cached->node = -1;
			const float dx = ctx.matrix.e - item.tx, dy = ctx.matrix.f - item.ty;
			if (dx != 0.0f || dy != 0.0f) {
				addItemDamage(cache.damage, item);  // where it was
				translateItem(item, dx, dy);
				addItemDamage(cache.damage, item);  // where it is
			} else if (!samePaint(item.fill, fill) || !samePaint(item.stroke, stroke)) {
				addItemDamage(cache.damage, item);
			}
			item.fill = fill;
			item.stroke = stroke;
			item.hittable = ctx.hittable;
			if (item.strokeEdges.empty() && !item.fill.on && !item.hittable) return;
			items.push_back(std::move(item));
			return;
		}
		Item item;
		Item *old = cache.stale(id);
		if (old) {
			item.fillEdges.swap(old->fillEdges);
			item.strokeEdges.swap(old->strokeEdges);
			item.fillEdges.clear();
			item.strokeEdges.clear();
		}
		item.node = id;
		item.geometryKey = key;
		item.styleKey = styleKey;
		item.tx = ctx.matrix.e;
		item.ty = ctx.matrix.f;
		buildGeometry(id, kind, ctx.matrix, item.geometry);
		if (item.geometry.empty()) return;
		item.fill = fill;
		item.stroke = stroke;
		item.evenOdd = ctx.evenOdd;
		item.hittable = ctx.hittable;
		if (stroked) {
			StrokeStyle style;
			style.width = width;
			style.cap = ctx.cap;
			style.join = ctx.join;
			style.miterLimit = ctx.miterLimit;
			// The stroke outline only feeds the rasterizer (hit testing measures
			// distance to the path), so it goes straight to edges.
			strokePathEdges(item.geometry, style, item.strokeEdges, item.strokeBounds);
			item.halfStroke = width / 2;
		}
		if (item.fill.on) {
			item.fillBounds.add(item.geometry.contours);
			buildEdges(item.geometry.contours, item.fillEdges);
		}
		if (item.strokeEdges.empty() && !item.fill.on && !item.hittable) return;
		// A stroke-only shape rebuilt with the same style and translation
		// changes only around the points its path changed: damage that, and
		// retire the old item so finish() does not add its whole box.
		if (old && stroked && !item.fill.on && !old->fill.on && old->styleKey == styleKey && old->tx == item.tx &&
				old->ty == item.ty && samePaint(old->stroke, item.stroke)) {
			float reach = item.halfStroke;
			if (ctx.join == LineJoin::Miter)
				reach *= std::max(miterReach(old->geometry, ctx.miterLimit), miterReach(item.geometry, ctx.miterLimit));
			if (ctx.cap == LineCap::Square) reach = std::max(reach, item.halfStroke * 1.4143f);
			const bool diffOk = strokeDiffDamage(old->geometry, item.geometry, reach + 2.0f, cache.damage);
			if (diffOk) {
				old->node = -1;
				items.push_back(std::move(item));
				return;
			}
		}
		addItemDamage(cache.damage, item);
		items.push_back(std::move(item));
		return;
	}
	for (int child = node.first_child; child >= 0; child = state.nodes[child].next_sibling) walk(child, ctx, items, cache);
}

void buildScene(int root, Scene &scene, int width, int height)
{
	std::vector<Item> previous = std::move(scene.items);
	scene.items.clear();
	ItemCache cache;
	// A resized viewport changes every matrix, so nothing would match anyway.
	if (scene.width == width && scene.height == height) cache.items = &previous;
	scene.width = width;
	scene.height = height;
	Context ctx;
	float vb[4];
	if (const char *v = attr(root, "viewBox"); v && parseNumberList(v, vb, 4))
		ctx.matrix = viewBoxTransform(vb, static_cast<float>(width), static_cast<float>(height), attr(root, "preserveAspectRatio"));
	walk(root, ctx, scene.items, cache);
	cache.finish();
	scene.dirty = false;
	scene.built = true;
	scene.damageValid = true;
	scene.damageFull = !cache.items || cache.full;
	scene.damage = cache.damage;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v)
{
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}


std::uint64_t subtreeSignature(int root)
{
	const auto &state = treeState();
	std::uint64_t h = 0xcbf29ce484222325ull;
	auto visit = [&](auto &&self, int id) -> void {
		const Node &n = state.nodes[id];
		const auto &s = n.computedStyle();
		const RareStyle &r = rstyle(s);
		h = mix(h, static_cast<std::uint64_t>(id) << 16 | static_cast<std::uint16_t>(n.tag_id));
		auto it = g_attrs.find(id);
		h = mix(h, it == g_attrs.end() ? 0 : it->second.generation);
		h = mix(h, static_cast<std::uint32_t>(r.svg_fill));
		h = mix(h, static_cast<std::uint32_t>(r.svg_stroke));
		h = mix(h, static_cast<std::uint32_t>(r.svg_stroke_width));
		h = mix(h, static_cast<std::uint64_t>(static_cast<std::uint16_t>(r.svg_fill_opacity)) << 16 |
		               static_cast<std::uint16_t>(r.svg_stroke_opacity));
		h = mix(h, static_cast<std::uint64_t>(static_cast<std::uint8_t>(r.svg_stroke_linecap)) << 16 |
		               static_cast<std::uint64_t>(static_cast<std::uint8_t>(r.svg_stroke_linejoin)) << 8 |
		               static_cast<std::uint8_t>(r.svg_fill_rule));
		h = mix(h, static_cast<std::uint64_t>(s.text_color));
		h = mix(h, static_cast<std::uint64_t>(s.text_alpha) << 24 | static_cast<std::uint64_t>(s.opacity) << 16 |
		               static_cast<std::uint64_t>(static_cast<std::uint8_t>(s.display)) << 8 |
		               static_cast<std::uint64_t>(static_cast<std::uint8_t>(s.visibility)) << 4 |
		               static_cast<std::uint8_t>(s.pointer_events));
		if (id != root && kindOf(n.tag_id) == Kind::Svg) return;  // nested svg: its own scene
		for (int c = n.first_child; c >= 0; c = state.nodes[c].next_sibling) self(self, c);
	};
	visit(visit, root);
	return h;
}

inline int scratchBank()
{
#if GEA_SVG_HAS_FREERTOS
	return xPortGetCoreID() & 1;
#elif GEA_SVG_HAS_PICO
	return get_core_num() & 1;
#else
	return 0;
#endif
}

// Scratch bank of the svg paint worker (banks 0/1 belong to the two cores'
// inline paints, which may run at once when the renderer splits a region).
constexpr int kWorkerBank = 2;

#if GEA_SVG_HAS_FREERTOS && !CONFIG_FREERTOS_UNICORE
#define GEA_SVG_PARALLEL_PAINT 1
#if __has_include("esp_cpu.h")
#include "esp_cpu.h"
inline unsigned svgPaintCycles() { return esp_cpu_get_cycle_count(); }
#else
// No cycle counter: the split stays at the middle.
inline unsigned svgPaintCycles() { return 0; }
#endif
// One svg's rows split across both cores: the frame core paints the top half
// while this worker, pinned to the other core, paints the bottom half. It owns
// its own task (never the display's row worker, whose single job slot may be
// in flight around us) and its own scratch bank; only the core that created
// it submits, and only when idle, so a paint that is itself running in a
// band on the second core simply paints inline.
// Whether the display driver is running work of its own on the second core
// right now (see paint); drivers without such work keep this default.
extern "C" __attribute__((weak)) bool gea_display_second_core_busy() { return false; }

struct PaintWorker {
	TaskHandle_t task = nullptr;
	int ownerCore = -1;
	bool attempted = false;
	std::atomic<bool> busy{false};
	std::atomic<bool> done{true};
	void (*fn)(void *, int, int) = nullptr;
	void *ctx = nullptr;
	int y0 = 0, y1 = -1;
};
PaintWorker g_paintWorker;

void paintWorkerTask(void *)
{
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		g_paintWorker.fn(g_paintWorker.ctx, g_paintWorker.y0, g_paintWorker.y1);
		g_paintWorker.done.store(true, std::memory_order_release);
	}
}

bool paintWorkerSubmit(void (*fn)(void *, int, int), void *ctx, int y0, int y1)
{
	const int core = static_cast<int>(xPortGetCoreID());
	if (!g_paintWorker.attempted) {
		g_paintWorker.attempted = true;
		g_paintWorker.ownerCore = core;
		// The stack holds only locals (scratch rows live on the heap), so it
		// may sit in PSRAM: internal RAM is the scarce pool on these parts.
		if (xTaskCreatePinnedToCoreWithCaps(paintWorkerTask, "gea_svg", 6144, nullptr, uxTaskPriorityGet(nullptr),
		                                    &g_paintWorker.task, core == 0 ? 1 : 0,
		                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS &&
		    xTaskCreatePinnedToCore(paintWorkerTask, "gea_svg", 6144, nullptr, uxTaskPriorityGet(nullptr),
		                            &g_paintWorker.task, core == 0 ? 1 : 0) != pdPASS)
			g_paintWorker.task = nullptr;
	}
	if (!g_paintWorker.task || core != g_paintWorker.ownerCore) return false;
	if (g_paintWorker.busy.exchange(true, std::memory_order_acq_rel)) return false;
	g_paintWorker.fn = fn;
	g_paintWorker.ctx = ctx;
	g_paintWorker.y0 = y0;
	g_paintWorker.y1 = y1;
	g_paintWorker.done.store(false, std::memory_order_release);
	xTaskNotifyGive(g_paintWorker.task);
	return true;
}

void paintWorkerWait()
{
	while (!g_paintWorker.done.load(std::memory_order_acquire)) {
	}
	g_paintWorker.busy.store(false, std::memory_order_release);
}
#else
#define GEA_SVG_PARALLEL_PAINT 0
#endif

// Blends a shape's coverage straight into the canvas rows. The rasterizer
// hands over many short runs per row (every anti-aliased edge is one), so each
// run is blended here, inline, into a row pointer fetched once per row: on an
// ESP32-S3 one canvas call costs ~250 cycles before it touches a pixel, more
// than a typical edge run. Rows the canvas cannot expose directly (rotated,
// packed, global alpha) go through its per-run entry points instead. Dirty
// marking is one box for the whole shape.
struct RowSink {
	using Pixel = gea::framework::graphics::pixel::native_t;
	gea::framework::graphics::Canvas *canvas;
	Pixel color;
	int ox, oy;
	int rowY = INT_MIN;
	Pixel *row = nullptr;
	int cx0 = 0, cx1 = -1;
	int dx0 = INT_MAX, dy0 = INT_MAX, dx1 = INT_MIN, dy1 = INT_MIN;
	Pixel lastBg = 0, lastOut = 0;
	int lastA = -1;

	bool select(int y)
	{
		if (y != rowY) {
			rowY = y;
			row = canvas->directRow(y, &cx0, &cx1);
		}
		return row != nullptr;
	}
	void touched(int y, int a, int b)
	{
		if (a < dx0) dx0 = a;
		if (b > dx1) dx1 = b;
		if (y < dy0) dy0 = y;
		if (y > dy1) dy1 = y;
	}
	Pixel blendOne(Pixel bg, int a)
	{
		if (bg != lastBg || a != lastA) {
			lastBg = bg;
			lastA = a;
			lastOut = gea::framework::graphics::pixel::blendNative(color, bg, a);
		}
		return lastOut;
	}
	void span(int y, int x, int count, std::uint8_t cov)
	{
		y += oy;
		x += ox;
		if (!select(y)) {
			canvas->blendCoverageSpan(y, x, count, cov, color, false);
			touched(y, x, x + count - 1);
			return;
		}
		const int a = std::max(x, cx0), b = std::min(x + count - 1, cx1);
		if (a > b) return;
		Pixel *p = row + a;
		const int n = b - a + 1;
		if (cov == 255) {
			std::fill_n(p, n, color);
		} else {
			for (int i = 0; i < n; ++i) p[i] = blendOne(p[i], cov);
		}
		touched(y, a, b);
	}
	void coverage(int y, int x, const std::uint8_t *cov, int count)
	{
		y += oy;
		x += ox;
		if (!select(y)) {
			canvas->blendCoverageRow(y, x, count, cov, color, false);
			touched(y, x, x + count - 1);
			return;
		}
		int a = x, b = x + count - 1;
		if (a < cx0) a = cx0;
		if (b > cx1) b = cx1;
		if (a > b) return;
		for (int px = a; px <= b; ++px) {
			const int c = cov[px - x];
			if (c == 0) continue;
			row[px] = c == 255 ? color : blendOne(row[px], c);
		}
		touched(y, a, b);
	}
	void markDirty()
	{
		if (dx0 <= dx1 && dy0 <= dy1) canvas->markDirty(dx0, dy0, dx1, dy1);
	}
};

GEA_SVG_HOT void paintCoverage(gea::framework::graphics::Canvas *canvas, int bank, bool markDirty, const EdgeList &edges,
                   const Bounds &bounds, bool evenOdd, const ResolvedPaint &paint, int ox, int oy, int cx0, int cy0,
                   int cx1, int cy1)
{
	if (!canvas || bounds.empty()) return;
	// Pixel rectangle in svg-local coordinates, clipped to the canvas clip.
	const int x0 = std::max(static_cast<int>(std::floor(bounds.x0)) - 1, cx0 - ox);
	const int y0 = std::max(static_cast<int>(std::floor(bounds.y0)) - 1, cy0 - oy);
	const int x1 = std::min(static_cast<int>(std::ceil(bounds.x1)) + 1, cx1 - ox);
	const int y1 = std::min(static_cast<int>(std::ceil(bounds.y1)) + 1, cy1 - oy);
	// One scratch bank per core: a large dirty region replays in two bands on
	// both cores at once (canvas.cpp's occlusion banks follow the same rule).
	static Rasterizer rasters[3];
	static ScratchVector<std::uint8_t> rows[3];
	Rasterizer &raster = rasters[bank];
	ScratchVector<std::uint8_t> &row = rows[bank];
	RowSink sink{canvas, paint.color, ox, oy};
	if (paint.gradient) {
		// A 256-step table along the gradient vector: colour and alpha per step.
		const GradientPaint &g = paint.grad;
		float gx1 = g.x1, gy1 = g.y1, gx2 = g.x2, gy2 = g.y2;
		if (g.bboxUnits) {
			const float bw = bounds.x1 - bounds.x0, bh = bounds.y1 - bounds.y0;
			gx1 = bounds.x0 + g.x1 * bw; gx2 = bounds.x0 + g.x2 * bw;
			gy1 = bounds.y0 + g.y1 * bh; gy2 = bounds.y0 + g.y2 * bh;
		}
		const float vx = gx2 - gx1, vy = gy2 - gy1;
		const float len2 = vx * vx + vy * vy;
		RowSink::Pixel lutColor[256];
		std::uint8_t lutAlpha[256];
		for (int i = 0; i < 256; ++i) {
			const float t = i / 255.0f;
			int k = 0;
			while (k + 1 < g.stops && g.offset[k + 1] <= t) ++k;
			float r, gg, b, a;
			if (k + 1 >= g.stops || t <= g.offset[0]) {
				const int s = t <= g.offset[0] ? 0 : k;
				r = g.color[s].r; gg = g.color[s].g; b = g.color[s].b; a = g.alpha[s];
			} else {
				const float span = g.offset[k + 1] - g.offset[k];
				const float f = span > 0 ? (t - g.offset[k]) / span : 1.0f;
				r = g.color[k].r + (g.color[k + 1].r - g.color[k].r) * f;
				gg = g.color[k].g + (g.color[k + 1].g - g.color[k].g) * f;
				b = g.color[k].b + (g.color[k + 1].b - g.color[k].b) * f;
				a = g.alpha[k] + (g.alpha[k + 1] - g.alpha[k]) * f;
			}
			lutColor[i] = gea::framework::graphics::pixel::nativeColor(static_cast<std::uint8_t>(r + 0.5f), static_cast<std::uint8_t>(gg + 0.5f),
			                                                         static_cast<std::uint8_t>(b + 0.5f));
			lutAlpha[i] = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		auto stepAt = [&](float px, float py) {
			if (len2 <= 1e-6f) return 255;
			const float t = std::clamp(((px - gx1) * vx + (py - gy1) * vy) / len2, 0.0f, 1.0f);
			return static_cast<int>(t * 255.0f + 0.5f);
		};
		// A vertical vector (LVGL's chart fade) is one step per row; any other
		// direction is evaluated per pixel.
		const bool perRow = std::fabs(vx) * 64.0f <= std::fabs(vy);
		auto use = [&](int step) {
			if (sink.color != lutColor[step]) { sink.color = lutColor[step]; sink.lastA = -1; }
			return lutAlpha[step];
		};
		static ScratchVector<std::uint8_t> scaled[3];
		ScratchVector<std::uint8_t> &tmp = scaled[bank];
		fillCoverage(
		    raster, row, edges, x0, y0, x1, y1, evenOdd, 1.0f,
		    [&](int y, int x, const std::uint8_t *cov, int count) {
			    if (perRow) {
				    const int a = use(stepAt(gx1, y + 0.5f));
				    if (a == 0) return;
				    if (tmp.size() < static_cast<std::size_t>(count)) tmp.resize(count);
				    for (int i = 0; i < count; ++i) tmp[i] = static_cast<std::uint8_t>((cov[i] * a + 127) / 255);
				    sink.coverage(y, x, tmp.data(), count);
				    return;
			    }
			    for (int i = 0; i < count; ++i) {
				    if (!cov[i]) continue;
				    const int a = use(stepAt(x + i + 0.5f, y + 0.5f));
				    const int c = (cov[i] * a + 127) / 255;
				    if (c) sink.span(y, x + i, 1, static_cast<std::uint8_t>(c));
			    }
		    },
		    [&](int y, int x, int count, std::uint8_t cov) {
			    if (perRow) {
				    const int a = use(stepAt(gx1, y + 0.5f));
				    const int c = (cov * a + 127) / 255;
				    if (c) sink.span(y, x, count, static_cast<std::uint8_t>(c));
				    return;
			    }
			    for (int i = 0; i < count; ++i) {
				    const int a = use(stepAt(x + i + 0.5f, y + 0.5f));
				    const int c = (cov * a + 127) / 255;
				    if (c) sink.span(y, x + i, 1, static_cast<std::uint8_t>(c));
			    }
		    });
		if (markDirty) sink.markDirty();
		return;
	}
	fillCoverage(
	    raster, row, edges, x0, y0, x1, y1, evenOdd, std::min(1.0f, paint.alpha),
	    [&](int y, int x, const std::uint8_t *cov, int count) { sink.coverage(y, x, cov, count); },
	    [&](int y, int x, int count, std::uint8_t cov) { sink.span(y, x, count, cov); });
	if (markDirty) sink.markDirty();
}

}  // namespace

bool cssDeclarationValue(StyleDeclaration declaration, const char *value, Property &property, int &encoded)
{
	if (!value) return false;
	while (*value == ' ' || *value == '\t') ++value;
	std::string v(value);
	if (const auto bang = v.find("!important"); bang != std::string::npos) v.erase(bang);
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
	auto lower = v;
	for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	// CSS-wide keywords reset to "unset" (inherit through the svg tree).
	const bool wide = lower == "inherit" || lower == "unset" || lower == "initial" || lower == "revert";
	switch (declaration) {
	case StyleDeclaration::SvgFill:
	case StyleDeclaration::SvgStroke: {
		property = declaration == StyleDeclaration::SvgFill ? Property::SvgFill : Property::SvgStroke;
		if (wide) { encoded = 0; return true; }
		const Paint paint = parsePaint(v.c_str());
		if (paint.kind == PaintKind::Unset || paint.kind == PaintKind::Url) return false;
		encoded = encodePaint(paint);
		return true;
	}
	case StyleDeclaration::SvgStrokeWidth: {
		property = Property::SvgStrokeWidth;
		if (wide) { encoded = -1; return true; }
		float w = 0;
		if (!parseLength(v.c_str(), w) || w < 0) return false;
		encoded = static_cast<int>(std::lround(w * 100));
		return true;
	}
	case StyleDeclaration::SvgFillOpacity:
	case StyleDeclaration::SvgStrokeOpacity: {
		property = declaration == StyleDeclaration::SvgFillOpacity ? Property::SvgFillOpacity : Property::SvgStrokeOpacity;
		if (wide) { encoded = -1; return true; }
		Scanner sc(v.c_str());
		float o = 1;
		if (!sc.number(o)) return false;
		if (*sc.rest() == '%') o /= 100.0f;
		encoded = static_cast<int>(std::lround(std::clamp(o, 0.0f, 1.0f) * 1000));
		return true;
	}
	case StyleDeclaration::SvgStrokeLinecap:
		property = Property::SvgStrokeLinecap;
		if (wide) encoded = -1;
		else if (lower == "butt") encoded = static_cast<int>(LineCap::Butt);
		else if (lower == "round") encoded = static_cast<int>(LineCap::Round);
		else if (lower == "square") encoded = static_cast<int>(LineCap::Square);
		else return false;
		return true;
	case StyleDeclaration::SvgStrokeLinejoin:
		property = Property::SvgStrokeLinejoin;
		if (wide) encoded = -1;
		else if (lower == "miter" || lower == "miter-clip" || lower == "arcs") encoded = static_cast<int>(LineJoin::Miter);
		else if (lower == "round") encoded = static_cast<int>(LineJoin::Round);
		else if (lower == "bevel") encoded = static_cast<int>(LineJoin::Bevel);
		else return false;
		return true;
	case StyleDeclaration::SvgFillRule:
		property = Property::SvgFillRule;
		if (wide) encoded = -1;
		else if (lower == "nonzero") encoded = 0;
		else if (lower == "evenodd") encoded = 1;
		else return false;
		return true;
	default:
		return false;
	}
}

bool isRoot(const Node &node) { return node.tag_id == g_svgTag; }

bool damage(int root, int &x0, int &y0, int &x1, int &y1, int (*extra)[4], int *extraCount)
{
	if (extraCount) *extraCount = 0;
	auto it = g_scenes.find(root);
	if (it == g_scenes.end()) return false;
	Scene &scene = it->second;
	const bool valid = scene.damageValid && !scene.damageFull && !scene.boxChanged;
	scene.damageValid = false;
	if (!valid) return false;
	auto toPixels = [&](const Bounds &b, int &bx0, int &by0, int &bx1, int &by1) {
		bx0 = std::max(0, static_cast<int>(std::floor(b.x0)));
		by0 = std::max(0, static_cast<int>(std::floor(b.y0)));
		bx1 = std::min(scene.width - 1, static_cast<int>(std::ceil(b.x1)));
		by1 = std::min(scene.height - 1, static_cast<int>(std::ceil(b.y1)));
		return bx0 <= bx1 && by0 <= by1;
	};
	x0 = y0 = 0;
	x1 = y1 = -1;
	const DamageSet &d = scene.damage;
	// Without room for the extra boxes, report their union.
	Bounds all;
	if (!extra || !extraCount)
		for (int k = 0; k < d.count; ++k) all = DamageSet::unite(all.empty() ? d.box[k] : all, d.box[k]);
	if (!extra || !extraCount) {
		if (!all.empty() && !toPixels(all, x0, y0, x1, y1)) x0 = y0 = 0, x1 = y1 = -1;
		return true;
	}
	bool first = true;
	for (int k = 0; k < d.count; ++k) {
		int bx0, by0, bx1, by1;
		if (!toPixels(d.box[k], bx0, by0, bx1, by1)) continue;
		if (first) {
			x0 = bx0, y0 = by0, x1 = bx1, y1 = by1;
			first = false;
		} else if (*extraCount < kExtraDamage) {
			int *r = extra[(*extraCount)++];
			r[0] = bx0, r[1] = by0, r[2] = bx1, r[3] = by1;
		} else {
			x0 = std::min(x0, bx0), y0 = std::min(y0, by0), x1 = std::max(x1, bx1), y1 = std::max(y1, by1);
		}
	}
	return true;
}

bool isShape(int node)
{
	if (g_svgTag == -2) return false;
	const Kind kind = kindOfNode(node);
	return kind != Kind::None && kind != Kind::Svg;
}

void noteTagChanged(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (g_svgTag == -2) {
		// Intern the SVG tag set only once an SVG tag actually appears.
		if (!isSvgTagName(tagFromId(state.nodes[node].tag_id))) return;
		g_svgTag = tagIds().svg;
	}
	const Kind kind = kindOf(state.nodes[node].tag_id);
	if (kind == Kind::None) {
		g_scenes.erase(node);
		return;
	}
	if (kind == Kind::Svg) {
		g_scenes[node];
		return;
	}
	// Shapes, groups and metadata take no part in CSS layout: out of flow, no
	// box. Inline (not default) styles, so this holds in every build.
	tree().setStyle(node, Property::Position, 1);
	tree().setStyle(node, Property::Width, 0);
	tree().setStyle(node, Property::Height, 0);
}

bool noteAttribute(int node, const char *name, const char *value)
{
	if (!name || kindOfNode(node) == Kind::None) return false;
	Attrs &attrs = g_attrs[node];
	auto it = std::find_if(attrs.values.begin(), attrs.values.end(), [&](const auto &kv) { return kv.first == name; });
	if (value) {
		if (it != attrs.values.end()) {
			if (it->second == value) return true;
			it->second = value;
		} else {
			attrs.values.emplace_back(name, value);
		}
	} else if (it != attrs.values.end()) {
		attrs.values.erase(it);
	} else {
		return true;
	}
	// Globally unique, so a reused node id never matches a stale cached item.
	static std::uint32_t serial = 0;
	attrs.generation = ++serial;
	if (kindOfNode(node) == Kind::Svg && (!std::strcmp(name, "width") || !std::strcmp(name, "height"))) {
		// The width/height attributes size the svg box at presentational
		// (lowest) priority: any authored CSS width/height wins.
		float px = 0;
		const bool w = name[0] == 'w';
		const Property p = w ? Property::Width : Property::Height;
		if (value && parseLength(value, px)) {
			// CSS px, like any authored width/height: scaled to device pixels.
			const int device = static_cast<int>(std::lround(px * devicePixelRatio()));
#if GEA_UI_DEFAULT_STYLES
			tree().setDefaultStyle(node, p, device);
#else
			tree().setStyle(node, p, device);
#endif
		}
	}
	noteNodeChanged(node);
	return true;
}

const char *attribute(int node, const char *name) { return name ? attr(node, name) : nullptr; }

void noteNodeChanged(int node)
{
	const int root = rootOf(node);
	if (root >= 0) invalidateRoot(root);
}

void releaseNode(int node)
{
	if (g_svgTag == -2) return;
	g_attrs.erase(node);
	g_scenes.erase(node);
}

void clearAll()
{
	g_attrs.clear();
	g_scenes.clear();
}

void validateScenes()
{
	auto &state = treeState();
#if GEA_SVG_PARALLEL_PAINT
	// A scene is otherwise first built when its svg is first recorded, i.e. when
	// it scrolls into view: on the CrowPanel's Analytics tab the first flick that
	// revealed the meter row built a dozen gauge scenes inside one frame (147 and
	// 226 ms frames), so the page jumped 150-250 px at once mid-scroll. Laid-out
	// svgs off screen get their scene here instead, built below with the rest
	// (on both cores) in the frame that mounted them.
	if (g_svgTag >= 0) {
		for (int id = 0; id < state.nodeCount; ++id) {
			const Node &n = state.nodes[id];
			if (n.tag_id != g_svgTag || !state.nodeActive[id] || n.layout.width <= 0 || n.layout.height <= 0) continue;
			if (g_scenes.find(id) != g_scenes.end()) continue;
			Scene &scene = g_scenes.emplace(id, Scene{}).first->second;
			scene.width = n.layout.width;
			scene.height = n.layout.height;
		}
	}
#endif
	if (g_scenes.empty()) return;
	for (auto it = g_scenes.begin(); it != g_scenes.end();) {
		const int root = it->first;
		if (root >= state.nodeCount || !state.nodeActive[root] || state.nodes[root].tag_id != g_svgTag) {
			it = g_scenes.erase(it);
			continue;
		}
		const std::uint64_t sig = subtreeSignature(root);
		if (sig != it->second.signature) {
			it->second.signature = sig;
			if (it->second.recorded) invalidateRoot(root);
			else it->second.dirty = true;
		}
		++it;
	}
#if GEA_SVG_PARALLEL_PAINT
	// Rebuild the changed scenes across both cores now, before the display
	// list records them (record() then finds them built). Building reads the
	// tree, attributes and styles, which nothing mutates during a refresh, and
	// writes only its own scene. Each scene goes to the less loaded core,
	// weighed by its previous edge count; the heaviest go first.
	struct Job {
		int root;
		Scene *scene;
		std::size_t weight;
	};
	Job jobs[16];
	int count = 0;
	for (auto &[root, scene] : g_scenes) {
		if (!scene.dirty || scene.width <= 0 || count == 16) continue;
		// Off screen and built once already (an animated needle that has not
		// been shown yet): its first record rebuilds it, so don't every frame.
		if (!scene.recorded && scene.built) continue;
		const Node &n = state.nodes[root];
		if (n.layout.width != scene.width || n.layout.height != scene.height) continue;
		std::size_t weight = 1;
		for (const Item &item : scene.items) weight += item.fillEdges.size() + item.strokeEdges.size();
		jobs[count++] = {root, &scene, weight};
	}
	if (count < 2) return;
	std::sort(jobs, jobs + count, [](const Job &a, const Job &b) { return a.weight > b.weight; });
	Job mine[16], theirs[16];
	int nMine = 0, nTheirs = 0;
	std::size_t loadMine = 0, loadTheirs = 0;
	for (int i = 0; i < count; ++i) {
		if (loadMine <= loadTheirs) mine[nMine++] = jobs[i], loadMine += jobs[i].weight;
		else theirs[nTheirs++] = jobs[i], loadTheirs += jobs[i].weight;
	}
	auto build = [](void *p, int, int count) {
		const Job *list = static_cast<const Job *>(p);
		for (int i = 0; i < count; ++i) buildScene(list[i].root, *list[i].scene, list[i].scene->width, list[i].scene->height);
	};
	const bool split = nTheirs > 0 && paintWorkerSubmit(build, theirs, 0, nTheirs);
	build(mine, 0, nMine);
	if (split) paintWorkerWait();
	else build(theirs, 0, nTheirs);
#endif
}

void record(const Node &node)
{
	const int id = static_cast<int>(&node - tree().nodes());
	auto it = g_scenes.find(id);
	if (it == g_scenes.end()) it = g_scenes.emplace(id, Scene{}).first;
	Scene &scene = it->second;
	if (node.layout.width <= 0 || node.layout.height <= 0) return;
	// Padding bytes can only make this report a change that did not happen,
	// which costs a full-box repaint, never a missed one.
	scene.boxChanged = !scene.haveBox || scene.boxX != node.layout.x || scene.boxY != node.layout.y ||
	                   std::memcmp(scene.boxStyle, &node.computedStyle(), sizeof(ComputedStyle)) != 0;
	scene.haveBox = true;
	scene.boxX = node.layout.x;
	scene.boxY = node.layout.y;
	std::memcpy(scene.boxStyle, &node.computedStyle(), sizeof(ComputedStyle));
	if (scene.dirty || scene.width != node.layout.width || scene.height != node.layout.height) {
		buildScene(id, scene, node.layout.width, node.layout.height);
		scene.signature = subtreeSignature(id);
	}
	if (scene.items.empty()) return;
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::DrawSvg;
	cmd->bx = node.layout.x;
	cmd->by = node.layout.y;
	cmd->bw = node.layout.width;
	cmd->bh = node.layout.height;
	cmd->svg = {static_cast<std::int16_t>(id), node.layout.x, node.layout.y, node.layout.width, node.layout.height};
	scene.recorded = true;
}


void paint(const DisplayCommand &c)
{
	auto it = g_scenes.find(c.svg.nodeId);
	if (it == g_scenes.end()) return;
	auto *canvas = gea::platform::display::Display::canvas();
	if (!canvas) return;
	int cx0, cy0, cx1, cy1;
	canvas->currentClip(&cx0, &cy0, &cx1, &cy1);
	// The svg viewport clips its content (overflow: hidden is SVG's default).
	cx0 = std::max(cx0, static_cast<int>(c.svg.x));
	cy0 = std::max(cy0, static_cast<int>(c.svg.y));
	cx1 = std::min(cx1 + 1, c.svg.x + c.svg.w);
	cy1 = std::min(cy1 + 1, c.svg.y + c.svg.h);
	if (cx0 >= cx1 || cy0 >= cy1) return;
	struct Band {
		const std::vector<Item> *items;
		gea::framework::graphics::Canvas *canvas;
		int ox, oy, cx0, cx1;
		int bank;
		bool markDirty;
		unsigned cycles;
	};
	auto paintRows = [](void *p, int y0, int y1) {
		Band &b = *static_cast<Band *>(p);
#if GEA_SVG_PARALLEL_PAINT
		const unsigned c0 = svgPaintCycles();
		struct Time { Band &b; unsigned c0; ~Time() { b.cycles = svgPaintCycles() - c0; } } time{b, c0};
#endif
		// In row bands, every item per band (so stacking holds): the band's
		// pixels stay hot in the cache while each item blends into them, where
		// item-by-item over the whole box re-reads every row once per item. On
		// an RGB panel scanning out of PSRAM that traffic and the dirty lines
		// it evicts landed inside bounce refills: banding took the Analytics
		// meters from 5-9 refill overruns (tears) per 9k refills to about none.
		constexpr int kBandRows = 24;
		for (int by0 = y0; by0 < y1; by0 += kBandRows) {
			const int by1 = std::min(y1, by0 + kBandRows);
			for (const Item &item : *b.items) {
				if (item.fill.on)
					paintCoverage(b.canvas, b.bank, b.markDirty, item.fillEdges, item.fillBounds, item.evenOdd, item.fill, b.ox,
					              b.oy, b.cx0, by0, b.cx1, by1);
				if (!item.strokeEdges.empty())
					paintCoverage(b.canvas, b.bank, b.markDirty, item.strokeEdges, item.strokeBounds, false, item.stroke, b.ox,
					              b.oy, b.cx0, by0, b.cx1, by1);
			}
		}
	};
	Band band{&it->second.items, canvas, c.svg.x, c.svg.y, cx0, cx1, scratchBank(), true, 0};
#if GEA_SVG_PARALLEL_PAINT
	// Split tall paints across both cores; rows are disjoint, so the two
	// halves never touch the same pixels. Neither marks rows dirty (the dirty
	// rect list is not concurrent); the clipped box is marked once below.
	// Ink is rarely spread evenly over the rows (a chart's filled area sits
	// low, a gauge's arc high), so a fixed half split leaves one core waiting
	// on the other; each paint moves the split row to where the last paint's
	// measured time would have divided equally.
	// Not while the display's own work holds the second core (a sideways
	// scroll's shifted copy): the helper would only time-slice with it, and
	// this core spins in paintWorkerWait meanwhile. On the CrowPanel 7.0 that
	// made the Analytics charts cost 13-30 ms of every swipe frame's strip.
	constexpr int kMinSplitRows = 48;
	if (cy1 - cy0 >= kMinSplitRows && (cx1 - cx0) * (cy1 - cy0) >= 8000 && !gea_display_second_core_busy()) {
		Scene &scene = it->second;
		const int rows = cy1 - cy0;
		const int mid = cy0 + std::clamp(static_cast<int>(rows * scene.split + 0.5f), 1, rows - 1);
		Band low = band;
		low.bank = kWorkerBank;
		low.markDirty = false;
		band.markDirty = false;
		if (paintWorkerSubmit(paintRows, &low, mid, cy1)) {
			paintRows(&band, cy0, mid);
			paintWorkerWait();
			canvas->markDirty(cx0, cy0, cx1 - 1, cy1 - 1);
			const float top = static_cast<float>(band.cycles), bottom = static_cast<float>(low.cycles);
			const float half = (top + bottom) * 0.5f;
			if (half > 0) {
				const int topRows = mid - cy0, bottomRows = cy1 - mid;
				// Rows each half's cost spreads over evenly, up to the time midpoint.
				const float at = top >= half ? topRows * (half / top) : topRows + bottomRows * ((half - top) / bottom);
				const float target = at / rows;
				scene.split = std::clamp(0.5f * scene.split + 0.5f * target, 0.1f, 0.9f);
			}
			return;
		}
		band.markDirty = true;
	}
#endif
	paintRows(&band, cy0, cy1);
}

int hitTest(int rootId, int x, int y)
{
	auto it = g_scenes.find(rootId);
	if (it == g_scenes.end()) return -1;
	const Node &root = tree().node(rootId);
	const Point p{static_cast<float>(x - root.layout.x), static_cast<float>(y - root.layout.y)};
	constexpr float kSlop = 3.0f;  // fingers are wider than hairlines
	const auto &items = it->second.items;
	for (auto item = items.rbegin(); item != items.rend(); ++item) {
		if (!item->hittable) continue;
		if (item->fill.on && insideFill(item->geometry.contours, p, item->evenOdd)) return item->node;
		if (!item->strokeEdges.empty() && distanceToPath(item->geometry, p) <= item->halfStroke + kSlop) return item->node;
	}
	return -1;
}

}  // namespace gea::embedded::ui::svg

#endif  // GEA_UI_SVG
