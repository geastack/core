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
	std::int16_t svg, g, path, polyline, polygon, line, circle, ellipse, rect, defs, title, desc, text;
};

const TagIds &tagIds()
{
	static const TagIds ids = {internTag("svg"),      internTag("g"),      internTag("path"),   internTag("polyline"),
	                           internTag("polygon"),  internTag("line"),   internTag("circle"), internTag("ellipse"),
	                           internTag("rect"),     internTag("defs"),   internTag("title"),  internTag("desc"),
	                           internTag("text")};
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
	if (tag == t.defs || tag == t.title || tag == t.desc || tag == t.text) return Kind::Skip;
	return Kind::None;
}

bool isSvgTagName(const char *tag)
{
	static const char *const names[] = {"svg", "g", "path", "polyline", "polygon", "line", "circle",
	                                    "ellipse", "rect", "defs", "title", "desc", "text"};
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

struct ResolvedPaint {
	bool on = false;
	gea::framework::graphics::pixel::native_t color = 0;
	float alpha = 1.0f;
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
	Bounds damage;
	// The svg box as last recorded, to tell a scene-only change apart from a
	// change to the box's own paint (background, border, position).
	bool boxChanged = true;
	bool haveBox = false;
	int boxX = 0, boxY = 0;
	alignas(ComputedStyle) unsigned char boxStyle[sizeof(ComputedStyle)] = {};
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
	if (!it->second.recorded) tree().markDisplayListContentDirty();
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
	return a.on == b.on && (!a.on || (a.color == b.color && a.alpha == b.alpha));
}

// Everything an item can touch on screen, in svg-local pixels.
void addItemDamage(Bounds &damage, const Item &item)
{
	constexpr float kAntialias = 2.0f;
	for (const Bounds *b : {&item.fillBounds, &item.strokeBounds}) {
		if (b->empty()) continue;
		damage.add(Point{b->x0 - kAntialias, b->y0 - kAntialias});
		damage.add(Point{b->x1 + kAntialias, b->y1 + kAntialias});
	}
}

// The previous scene's items, consumed as the new scene reuses them; also
// accumulates the rebuild's damage.
struct ItemCache {
	std::vector<Item> *items = nullptr;
	std::size_t hint = 0;
	long lastTaken = -1;
	bool full = false;
	Bounds damage;
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
	// Old items nobody reused (removed, hidden or rebuilt) leave damage behind.
	void finish()
	{
		if (!items) return;
		for (const Item &it : *items)
			if (it.node >= 0) addItemDamage(damage, it);
	}
};

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
				kind == Kind::Line ? ResolvedPaint{} : resolvePaint(ctx.fill, node, ctx.opacity * ctx.fillOpacity);
		const ResolvedPaint stroke = resolvePaint(ctx.stroke, node, ctx.opacity * ctx.strokeOpacity);
		const float width = ctx.strokeWidth * ctx.matrix.scale();
		const bool stroked = stroke.on && width > 0;
		auto it = g_attrs.find(id);
		std::uint64_t key = mixKey(0x51ed27ull, (static_cast<std::uint64_t>(id) << 32) |
		                                            (it == g_attrs.end() ? 0u : it->second.generation));
		key = mixKey(key, static_cast<std::uint16_t>(node.tag_id));
		for (float f : {ctx.matrix.a, ctx.matrix.b, ctx.matrix.c, ctx.matrix.d})
			key = mixKey(key, floatBits(f));
		key = mixKey(key, static_cast<std::uint64_t>(fill.on) | static_cast<std::uint64_t>(stroked) << 1 |
		                      static_cast<std::uint64_t>(ctx.evenOdd) << 2);
		if (stroked)
			key = mixKey(key, floatBits(width) | static_cast<std::uint64_t>(ctx.cap) << 32 |
			                      static_cast<std::uint64_t>(ctx.join) << 40);
		if (stroked && ctx.join == LineJoin::Miter) key = mixKey(key, floatBits(ctx.miterLimit));
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
		item.node = id;
		item.geometryKey = key;
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
			// distance to the path), so keep its edges and drop the polygons.
			std::vector<Contour> outline;
			strokePath(item.geometry, style, outline);
			item.halfStroke = width / 2;
			item.strokeBounds.add(outline);
			buildEdges(outline, item.strokeEdges);
		}
		if (item.fill.on) {
			item.fillBounds.add(item.geometry.contours);
			buildEdges(item.geometry.contours, item.fillEdges);
		}
		if (item.strokeEdges.empty() && !item.fill.on && !item.hittable) return;
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
// One svg's rows split across both cores: the frame core paints the top half
// while this worker, pinned to the other core, paints the bottom half. It owns
// its own task (never the display's row worker, whose single job slot may be
// in flight around us) and its own scratch bank; only the core that created
// it submits, and only when idle, so a paint that is itself running in a
// band on the second core simply paints inline.
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

void paintCoverage(gea::framework::graphics::Canvas *canvas, int bank, bool markDirty, const EdgeList &edges,
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
	fillCoverage(raster, row, edges, x0, y0, x1, y1, evenOdd, std::min(1.0f, paint.alpha),
	             [&](int y, int x, const std::uint8_t *cov, int count) {
		             canvas->blendCoverageRow(y + oy, x + ox, count, cov, paint.color, markDirty);
	             });
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
		if (paint.kind == PaintKind::Unset) return false;
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

bool damage(int root, int &x0, int &y0, int &x1, int &y1)
{
	auto it = g_scenes.find(root);
	if (it == g_scenes.end()) return false;
	Scene &scene = it->second;
	const bool valid = scene.damageValid && !scene.damageFull && !scene.boxChanged;
	scene.damageValid = false;
	if (!valid) return false;
	if (scene.damage.empty()) {
		x0 = y0 = 0;
		x1 = y1 = -1;
		return true;
	}
	x0 = std::max(0, static_cast<int>(std::floor(scene.damage.x0)));
	y0 = std::max(0, static_cast<int>(std::floor(scene.damage.y0)));
	x1 = std::min(scene.width - 1, static_cast<int>(std::ceil(scene.damage.x1)));
	y1 = std::min(scene.height - 1, static_cast<int>(std::ceil(scene.damage.y1)));
	if (x0 > x1 || y0 > y1) x0 = y0 = 0, x1 = y1 = -1;
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
	if (g_scenes.empty()) return;
	auto &state = treeState();
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
	};
	auto paintRows = [](void *p, int y0, int y1) {
		const Band &b = *static_cast<const Band *>(p);
		for (const Item &item : *b.items) {
			if (item.fill.on)
				paintCoverage(b.canvas, b.bank, b.markDirty, item.fillEdges, item.fillBounds, item.evenOdd, item.fill, b.ox,
				              b.oy, b.cx0, y0, b.cx1, y1);
			if (!item.strokeEdges.empty())
				paintCoverage(b.canvas, b.bank, b.markDirty, item.strokeEdges, item.strokeBounds, false, item.stroke, b.ox,
				              b.oy, b.cx0, y0, b.cx1, y1);
		}
	};
	Band band{&it->second.items, canvas, c.svg.x, c.svg.y, cx0, cx1, scratchBank(), true};
#if GEA_SVG_PARALLEL_PAINT
	// Split tall paints across both cores; rows are disjoint, so the two
	// halves never touch the same pixels. Neither marks rows dirty (the dirty
	// rect list is not concurrent); the clipped box is marked once below.
	constexpr int kMinSplitRows = 48;
	if (cy1 - cy0 >= kMinSplitRows && (cx1 - cx0) * (cy1 - cy0) >= 8000) {
		const int mid = (cy0 + cy1) / 2;
		Band low = band;
		low.bank = kWorkerBank;
		low.markDirty = false;
		band.markDirty = false;
		if (paintWorkerSubmit(paintRows, &low, mid, cy1)) {
			paintRows(&band, cy0, mid);
			paintWorkerWait();
			canvas->markDirty(cx0, cy0, cx1 - 1, cy1 - 1);
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
