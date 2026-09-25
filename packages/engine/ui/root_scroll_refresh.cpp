// SPDX-License-Identifier: Apache-2.0
#include "root_scroll_refresh.h"

#include "display.h"
#include "internal.h"
#include "layout_snapshot.h"
#include "memory.h"
#include "refresh_perf.h"
#include "tree_state.h"
#include "viewport_region.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

// When the platform's Display::scrollRect needs no framebuffer flush of the
// scrolled region itself (e.g. rp2350 driving the panel's hardware vertical
// scroll register), the final flush only needs the replayed damage rects —
// strip, scrollbar, slots, extras — instead of the canvas dirty bounding box
// that the region-wide scroll would otherwise inflate to.
#ifndef GEA_EMBEDDED_DISPLAY_SCROLLRECT_SELF_FLUSHING
#define GEA_EMBEDDED_DISPLAY_SCROLLRECT_SELF_FLUSHING 0
#endif

// Translate-only display-list maintenance for pure vertical scroll frames
// (skips the per-frame clear()+recordNode()). On by default; a build can set
// this to 0 to A/B against the full-rebuild reference.
#ifndef GEA_EMBEDDED_ROOT_SCROLL_TRANSLATE
#define GEA_EMBEDDED_ROOT_SCROLL_TRANSLATE 1
#endif

// A display may decide at run time that its scrollRect publishes itself (the
// ESP32-S3 RGB panel's ring scroll); the weak default defers to the macro.
extern "C" __attribute__((weak)) bool gea_display_scroll_rect_self_flushing() { return false; }
// A display that can run a sideways scroll's shifted copy on another core while
// this one repaints the exposed strip starts it here (false: it cannot, and the
// caller blits with Display::scrollRect) and joins it in the end call.
extern "C" __attribute__((weak)) bool gea_display_scroll_rect_begin(int, int, int, int, int, int) { return false; }
#if defined(GEA_LINKED_HOOK_DEFAULTS) && GEA_LINKED_HOOK_DEFAULTS
extern "C" void gea_display_prepare_repaint(const gea::platform::display::DisplayFlushRect *, int);
#else
extern "C" __attribute__((weak)) void gea_display_prepare_repaint(const gea::platform::display::DisplayFlushRect *, int) {}
#endif
// Per screen row from y0: what the coming sideways scroll copy leaves unchanged
// (DisplayList::scrollRowSignatures), so the display may skip rows its back
// buffer already holds. Taken by the next gea_display_scroll_rect_begin.
extern "C" __attribute__((weak)) void gea_display_scroll_rect_rows(const std::uint32_t *, int, int) {}
extern "C" __attribute__((weak)) void gea_display_scroll_rect_end() {}

namespace gea::embedded::ui {

#if GEA_CSS_SCROLLING
namespace {

bool hasTransformState(const Node &node)
{
	return hasIndividualLinearTransform(rstyle(node.computedStyle())) || hadIndividualLinearTransform(node.render) ||
	       rstyle(node.computedStyle()).transform_rotate != 0 ||
	       node.render.previous_transform_rotate != 0 ||
	       rstyle(node.computedStyle()).transform_rotate_x != 0 ||
	       node.render.previous_transform_rotate_x != 0 ||
	       rstyle(node.computedStyle()).transform_rotate_y != 0 ||
	       node.render.previous_transform_rotate_y != 0 ||
	       composedTranslateX(rstyle(node.computedStyle())) != 0 ||
	       node.render.previous_transform_translate_x != 0 ||
	       composedTranslateY(rstyle(node.computedStyle())) != 0 ||
	       node.render.previous_transform_translate_y != 0 ||
	       composedTranslateZ(rstyle(node.computedStyle())) != 0 ||
	       node.render.previous_transform_translate_z != 0 ||
	       composedTranslateXPercent(rstyle(node.computedStyle())) != 0 ||
	       node.render.previous_transform_translate_x_percent != 0 ||
	       composedTranslateYPercent(rstyle(node.computedStyle())) != 0 ||
	       node.render.previous_transform_translate_y_percent != 0 ||
	       rstyle(node.computedStyle()).transform_scale_x != 1000 ||
	       node.render.previous_transform_scale_x != 1000 ||
	       rstyle(node.computedStyle()).transform_scale_y != 1000 ||
	       rstyle(node.computedStyle()).transform_scale_z != 1000 ||
	       node.render.previous_transform_scale_y != 1000 ||
	       node.render.previous_transform_scale_z != 1000 ||
	       rstyle(node.computedStyle()).perspective > 0 ||
	       node.render.previous_perspective > 0;
}

// Animated content inside a scroller (the CrowPanel Analytics meters: arcs,
// needles, legends and readouts) dirties a dozen or more nodes per frame; at 8
// every such scroll frame fell to the full relayout + repaint (~220 ms there).
constexpr int kMaxExtraDirtyNodes = 32;
// One full recycled-slot pool: a fast fling can rewindow every slot of a
// <virtual-list> in a single tick.
constexpr int kMaxSlotRepositionNodes = 24;

bool layoutUnchanged(const Node &n)
{
	return n.layout.x == n.layout.previous_x && n.layout.y == n.layout.previous_y &&
	       n.layout.width == n.layout.previous_width && n.layout.height == n.layout.previous_height;
}

bool isDescendantOf(const TreeState &state, int node, int ancestor)
{
	for (int cursor = state.nodes[node].parent; cursor >= 0; cursor = state.nodes[cursor].parent) {
		if (cursor == ancestor) return true;
	}
	return false;
}

bool hasRightFadeMask(const TreeState &state, int node)
{
	for (int cursor = node; cursor >= 0; cursor = state.nodes[cursor].parent) {
		if (state.nodes[cursor].computedStyle().mask_right_fade_width > 0) return true;
	}
	return false;
}

// A <virtual-list> windows a pool of absolutely-positioned slot children whose
// `top` the app rebinds as the scroll advances. Those style writes only land in
// layout via a layout pass — which this fast path skips — so without native
// reconciliation the rebuilt display list records recycled slots at their STALE
// positions and the revealed strip replays blank rows. Detect them here and let
// refresh() reposition them in place (mirroring the layout engine's
// absolute-child math), keeping the blit + strip replay path valid for every
// momentum tick instead of falling back to a full relayout + repaint.
bool isSlotRepositionCandidate(const TreeState &state, int node, int scrollNode)
{
	const Node &n = state.nodes[node];
	if (n.parent != scrollNode) return false;
	if (state.nodes[scrollNode].type != NodeType::VirtualList) return false;
	// The list is the slot's containing block only when it is itself positioned
	// (matches containingBlockForAbsoluteNode in the layout engine).
	const auto parentPosition = state.nodes[scrollNode].computedStyle().position;
	if (parentPosition != 1 && parentPosition != 2) return false;
	if (n.computedStyle().position != 1) return false;
	// Anchored via top/bottom (the windowed axis); refresh() recomputes y from
	// these offsets directly.
	if (GEA_CSS_POSITION_PX_0(n.computedStyle()) == kUnset && GEA_CSS_POSITION_PERCENT_0(n.computedStyle()) == kUnset &&
	    GEA_CSS_POSITION_PX_2(n.computedStyle()) == kUnset && GEA_CSS_POSITION_PERCENT_2(n.computedStyle()) == kUnset)
		return false;
	if (hasTransformState(n)) return false;
	// The reconcile translates the slot box; a pending size change needs layout.
	if (n.layout.width != n.layout.previous_width || n.layout.height != n.layout.previous_height)
		return false;
	return true;
}

template <int side>
int resolvedSlotOffset(const Node &slot, int basis)
{
	int offset = GEA_CSS_POSITION_PX(slot.computedStyle(), side) != kUnset ? GEA_CSS_POSITION_PX(slot.computedStyle(), side) : 0;
	const int percent = GEA_CSS_POSITION_PERCENT(slot.computedStyle(), side);
	if (percent != kUnset) {
		const int numerator = basis * percent;
		offset += (numerator + (numerator >= 0 ? 500 : -500)) / 1000;
	}
	return offset;
}

void translateSlotDescendants(TreeState &state, int node, int dx, int dy)
{
	if (dx == 0 && dy == 0) return;
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		state.nodes[child].layout.x += dx;
		state.nodes[child].layout.y += dy;
		translateSlotDescendants(state, child, dx, dy);
	}
}

// Mirror of ViewRenderer::recordScrollbar's thumb math (view.cpp:209-243),
// exposed so the root-scroll refresh can compute old and new thumb bounds
// for a non-virtual-list scroll node and damage only the diff between them.
// Returns false when no scrollbar would be drawn for this node.
bool nonVirtualListScrollbarThumbBounds(const Node &n, int scrollTop, int *thumbY, int *thumbH)
{
	if (n.layout.height <= 0) return false;
	if (n.layout.scroll_content_height <= n.layout.height) return false;
	const int track_h = n.layout.height - 12;
	if (track_h < 24) return false;
	int thumb_h = (n.layout.height * track_h) / n.layout.scroll_content_height;
	if (thumb_h < 24) thumb_h = 24;
	if (thumb_h > track_h) thumb_h = track_h;
	const int max_scroll = n.layout.scroll_content_height - n.layout.height;
	int y = n.layout.y + 6;
	if (max_scroll > 0)
		y += (scrollTop * (track_h - thumb_h)) / max_scroll;
	if (thumbY) *thumbY = y;
	if (thumbH) *thumbH = thumb_h;
	return true;
}

// Mirror of LayoutNodePass::resolveRowDirection (layout.cpp): an explicit
// flex-direction or display:flex wins; otherwise a plain block flows its
// children as a row exactly when every in-flow child is inline-level (CSS
// inline formatting), and stacks them as a column otherwise.
bool flowsChildrenAsRow(const TreeState &state, const Node &p)
{
	if (usesRowLayout(p.computedStyle())) return true;
	if (p.computedStyle().flex_direction_explicit || p.computedStyle().display != kDisplayBlock) return false;
	bool anyInFlow = false;
	for (int c = p.first_child; c >= 0; c = state.nodes[c].next_sibling) {
		const Node &child = state.nodes[c];
		if (child.computedStyle().display == kDisplayNone || isOutOfFlowPosition(child.computedStyle().position)) continue;
		if (!LayoutEngine::isInlineLevelNode(child)) return false;
		anyInFlow = true;
	}
	return anyInFlow;
}

// A left-anchored re-measure moves where the layout engine would place every
// later sibling on the line: positionLineChildren advances its main-axis
// cursor by each child's box. The fast path runs no layout, so a recycled
// row's trailing spans (the pixel-value rail after the "#1234" index label)
// otherwise keep the PREVIOUS binding's offsets for the whole fling and only
// snap over at the settle relayout. Mirror the cursor in place: shift the
// following in-flow siblings by the main-axis delta. Only shapes the cursor
// math fully determines are mirrored — a single non-wrapping flex-start line
// with no flex-grow children (wrap re-lines, justify redistributes, flex
// resizes) — anything else keeps the stale-until-settle behaviour. The caller
// guarantees the moved siblings' pixels are replayed (a dirty slot's old/new
// rects cover its whole row).
void shiftSiblingsForRemeasuredText(TreeState &state, int id, int oldWidth, int oldHeight)
{
	const Node &n = state.nodes[id];
	const Node &p = state.nodes[n.parent];
	if (p.computedStyle().flex_wrap || p.computedStyle().justify_content != 0) return;
	const bool row = flowsChildrenAsRow(state, p);
	const int delta = row ? n.layout.width - oldWidth : n.layout.height - oldHeight;
	if (delta == 0) return;
	int totalFlex = 0;
	for (int c = p.first_child; c >= 0; c = state.nodes[c].next_sibling) {
		const Node &child = state.nodes[c];
		if (child.computedStyle().display == kDisplayNone || isOutOfFlowPosition(child.computedStyle().position)) continue;
		totalFlex += child.computedStyle().flex;
	}
	if (totalFlex > 0) return;
	for (int c = n.next_sibling; c >= 0; c = state.nodes[c].next_sibling) {
		Node &sib = state.nodes[c];
		if (sib.computedStyle().display == kDisplayNone || isOutOfFlowPosition(sib.computedStyle().position)) continue;
		if (row) sib.layout.x += delta;
		else sib.layout.y += delta;
		translateSlotDescendants(state, c, row ? delta : 0, row ? 0 : delta);
	}
}

// Re-measure a rebound text node in place. The fast path runs no layout, but
// the display-list rebuild records text wrapped/clipped by layout.width — so a
// string that grew past its stale box truncates ("#111" drawing as "#11") for
// the whole fling, snapping right only at the settle frame's full relayout.
// Fresh string, fresh box instead. Right-flush text (a space-between flex
// row's trailing column, e.g. HUD value rails) keeps its right edge so the
// grown box stays inside the parent; everything else keeps its left anchor —
// and, when the caller's replay rects cover the parent's pixels
// (siblingsCoveredByReplay), the following in-flow siblings shift with the
// new width so they don't sit at the previous binding's offsets mid-fling.
// text_layout_stable stays 0, so the settle frame still runs the full layout
// that reflows siblings and exact flex alignment.
void remeasureDirtyTextInPlace(TreeState &state, int id, bool siblingsCoveredByReplay)
{
	Node &n = state.nodes[id];
	if (n.type != NodeType::Text || n.render.text_layout_stable || n.first_child >= 0) return;
	if (hasTransformState(n)) return;
	const int oldWidth = n.layout.width;
	const int oldHeight = n.layout.height;
	if (n.parent < 0) {
		TextRenderer::remeasureContentBox(id);
		return;
	}
	const Node &p = state.nodes[n.parent];
	if (!flowsChildrenAsRow(state, p)) {
		// Column flow (block-level text stacking). The default align stretch
		// gives the box the parent's full content width, so its right edge
		// ALWAYS touches the parent's — the row path's right-flush heuristic
		// below would misread that as a right-flush rail and throw the rebound
		// glyphs to the right edge for the whole fling (settle relayout then
		// snaps them back left). The box geometry is independent of the string
		// here: keep the stretched box, re-wrap the new text within it, and
		// shift the following stacked siblings by any height delta. Hugged
		// (non-stretch / explicit-width) boxes re-measure naturally and keep
		// their left anchor — the cross axis never right-flushes by default.
		const int align = n.computedStyle().align_self >= 0 ? n.computedStyle().align_self : p.computedStyle().align_items;
		const bool stretched = align == 0 && n.computedStyle().width == kUnset && n.computedStyle().width_percent == kUnset;
		if (!TextRenderer::remeasureContentBox(id, stretched)) return;
		if (siblingsCoveredByReplay) shiftSiblingsForRemeasuredText(state, id, oldWidth, oldHeight);
		return;
	}
	if (!TextRenderer::remeasureContentBox(id)) return;
	const int oldRight = n.layout.x + oldWidth + n.computedStyle().margin[1];
	const int parentRight = p.layout.x + p.layout.width - p.computedStyle().padding[1];
	if (oldRight == parentRight) {
		n.layout.x += oldWidth - n.layout.width;
		return;
	}
	if (siblingsCoveredByReplay) shiftSiblingsForRemeasuredText(state, id, oldWidth, oldHeight);
}

bool dirtyBounds(const Node &n, int width, int height, RootScrollOnlyRefresh::DirtyNode *out)
{
	int x0 = width, y0 = height, x1 = -1, y1 = -1;
	if (n.layout.previous_width > 0 && n.layout.previous_height > 0) {
		ViewRenderer::transformedBounds(n, true, &x0, &y0, &x1, &y1);
	}
	if (n.layout.width > 0 && n.layout.height > 0) {
		int nx0, ny0, nx1, ny1;
		ViewRenderer::transformedBounds(n, false, &nx0, &ny0, &nx1, &ny1);
		if (nx0 < x0) x0 = nx0;
		if (ny0 < y0) y0 = ny0;
		if (nx1 > x1) x1 = nx1;
		if (ny1 > y1) y1 = ny1;
	}
	if (x0 > x1 || y0 > y1) return false;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width) x1 = width - 1;
	if (y1 >= height) y1 = height - 1;
	if (x0 > x1 || y0 > y1) return false;
	out->x0 = x0;
	out->y0 = y0;
	out->x1 = x1;
	out->y1 = y1;
	return true;
}

// An overlay's painted box: its layout box widened by its box shadow.
bool overlayBounds(const Node &n, int width, int height, RootScrollOnlyRefresh::DirtyNode *out)
{
	if (!dirtyBounds(n, width, height, out)) return false;
	const auto &style = rstyle(n.computedStyle());
	const int reach = std::abs(static_cast<int>(style.box_shadow_blur_radius)) +
	                  std::abs(static_cast<int>(style.box_shadow_spread)) +
	                  std::max(std::abs(static_cast<int>(style.box_shadow_offset_x)),
	                           std::abs(static_cast<int>(style.box_shadow_offset_y)));
	out->x0 = std::max(0, out->x0 - reach);
	out->y0 = std::max(0, out->y0 - reach);
	out->x1 = std::min(width - 1, out->x1 + reach);
	out->y1 = std::min(height - 1, out->y1 + reach);
	return true;
}

// A plain <div> that paints nothing of its own: no background, border, or box
// shadow, and no scrollbar. Its pixels are whatever lies under it, so a scroll
// blit cannot drag anything of its own (a transparent hit target over a chart).
bool paintsNothingOwn(int id, const Node &n)
{
	if (n.type != NodeType::View) return false;
	const char *tag = Tree::instance().tagName(id);
	if (!tag || std::strcmp(tag, "div") != 0) return false;
	const auto &style = n.computedStyle();
	if (style.has_bg || style.border_width != 0) return false;
	if (scrollsOverflowX(style) || scrollsOverflowY(style)) return false;
	const auto &rare = rstyle(style);
	return rare.box_shadow_blur_radius == 0 && rare.box_shadow_spread == 0 && rare.box_shadow_offset_x == 0 &&
	       rare.box_shadow_offset_y == 0;
}

}  // namespace

// A border or a rounded corner on the scroller itself: pixels a scroll blit
// must not move (see blitScroll in refresh()).
static bool scrollerHasBoxFrame(const Node &n)
{
	if (hasAnyBorder(n.computedStyle())) return true;
	for (int r = 0; r < 4; r++) {
		if (n.computedStyle().border_radius[GEA_CSS_RADIUS_INDEX(r)] != 0) return true;
#if GEA_CSS_PERCENT_RADIUS
		if (n.computedStyle().border_radius_percent[GEA_CSS_RADIUS_INDEX(r)] != kUnset) return true;
#endif
	}
	return false;
}

namespace {
// The nodes a scroll fast-path frame found dirty, in id order: pass 2 of
// refreshNode classifies only these, and the frame's snapshot captures only
// these and the scroller's subtree, instead of reading every node from PSRAM.
std::vector<int> gScrollFrameDirty;
#if GEA_CSS_STICKY
// Inclusive pixel bounds, empty while x1 < x0. Trivial, so an unused array
// of them costs a scroll frame nothing.
struct PaintBounds {
	int x0, y0, x1, y1;

	static PaintBounds none() { return {0, 0, -1, -1}; }
	bool empty() const { return x1 < x0 || y1 < y0; }
	void unite(const PaintBounds &b)
	{
		if (b.empty()) return;
		if (empty()) {
			*this = b;
			return;
		}
		x0 = std::min(x0, b.x0);
		y0 = std::min(y0, b.y0);
		x1 = std::max(x1, b.x1);
		y1 = std::max(y1, b.y1);
	}
	void shift(int dx, int dy)
	{
		x0 += dx;
		x1 += dx;
		y0 += dy;
		y1 += dy;
	}
	bool touches(const PaintBounds &b) const
	{
		return !empty() && !b.empty() && x0 <= b.x1 + 1 && b.x0 <= x1 + 1 && y0 <= b.y1 + 1 && b.y0 <= y1 + 1;
	}
};

// A sticky box the scroll re-stuck: its shift on top of the content's, and
// its paint in the previous and the current display list. A frame follows
// this many; more record the list afresh and repaint the scrollport.
constexpr int kMaxStickyMoves = 4;
struct StickyMove {
	int node, dx, dy;
	PaintBounds old, now;
};

// translateSubtreeCommands walks every node's ancestors and every clip
// command's; a sticky box's subtree is a few nodes, so move it node by node
// (each node with children scans the list once for its clip scopes).
int countInnerNodes(const TreeState &state, int id, int limit)
{
	int count = state.nodes[id].first_child >= 0;
	for (int child = state.nodes[id].first_child; child >= 0 && count <= limit; child = state.nodes[child].next_sibling)
		count += countInnerNodes(state, child, limit - count);
	return count;
}

void translateEachNodeCommands(const TreeState &state, int id, int dx, int dy)
{
	DisplayList::instance().translateNodeCommands(id, dx, dy);
	for (int child = state.nodes[id].first_child; child >= 0; child = state.nodes[child].next_sibling)
		translateEachNodeCommands(state, child, dx, dy);
}

void translateStickyCommands(const TreeState &state, int id, int dx, int dy)
{
	constexpr int kMaxInnerNodes = 4;
	if (countInnerNodes(state, id, kMaxInnerNodes) <= kMaxInnerNodes) translateEachNodeCommands(state, id, dx, dy);
	else DisplayList::instance().translateSubtreeCommands(id, dx, dy);
}

void uniteSubtreeCommandBounds(const TreeState &state, int id, PaintBounds &bounds)
{
	PaintBounds own;
	if (DisplayList::instance().nodeCommandBounds(id, &own.x0, &own.y0, &own.x1, &own.y1)) bounds.unite(own);
	for (int child = state.nodes[id].first_child; child >= 0; child = state.nodes[child].next_sibling)
		uniteSubtreeCommandBounds(state, child, bounds);
}

// A re-stuck box that kept its place on screen and whose border box covers
// the scrollport across the scroll axis from its leading or trailing edge,
// with nothing that scrolls painting over it there: that band keeps its
// pixels. Widens *lead or *trail (pixels along the axis) to it.
bool stuckBand(const TreeState &state, const StickyMove &moved, bool horizontal, int move, int scrollNode, int vx, int vy, int vw,
               int vh, int *lead, int *trail)
{
	if (horizontal ? (moved.dx != -move || moved.dy != 0) : (moved.dy != -move || moved.dx != 0)) return false;
	const LayoutBox &box = state.nodes[moved.node].layout;
	if (box.width <= 0 || box.height <= 0) return false;
	// Scrollport-relative extents along (a) and across (c) the scroll axis.
	const int a0 = horizontal ? box.x - vx : box.y - vy;
	const int a1 = horizontal ? box.x + box.width - 1 - vx : box.y + box.height - 1 - vy;
	const int c0 = horizontal ? box.y - vy : box.x - vx;
	const int c1 = horizontal ? box.y + box.height - 1 - vy : box.x + box.width - 1 - vx;
	const int extent = horizontal ? vw : vh;
	if (c0 > 0 || c1 < (horizontal ? vh : vw) - 1) return false;
	int band0, band1;
	if (a0 <= 0 && a1 >= 0) {
		band0 = 0;
		band1 = std::min(a1, extent - 1);
	} else if (a0 <= extent - 1 && a1 >= extent - 1) {
		band0 = std::max(a0, 0);
		band1 = extent - 1;
	} else {
		return false;
	}
	const int x0 = horizontal ? vx + band0 : vx, x1 = horizontal ? vx + band1 : vx + vw - 1;
	const int y0 = horizontal ? vy : vy + band0, y1 = horizontal ? vy + vh - 1 : vy + band1;
	if (!DisplayList::instance().regionPaintedOnlyBy(x0, y0, x1, y1, moved.node, scrollNode)) return false;
	if (band0 == 0) *lead = std::max(*lead, band1 + 1);
	else *trail = std::max(*trail, extent - band0);
	return true;
}

// The re-stuck boxes move apart from the content, which the slot and
// extra-dirty damage of refresh() assume they follow.
bool stickyMovesFollowable(const TreeState &state, const int *slots, int count, const RootScrollOnlyRefresh::DirtyNode *extra,
                           int extraCount, int slotCount)
{
	if (slotCount > 0) return false;
	for (int e = 0; e < extraCount; e++) {
		for (int s = 0; s < count; s++) {
			const int box = state.stickyBoxes[slots[s]].node;
			if (extra[e].id == box || isDescendantOf(state, extra[e].id, box)) return false;
		}
	}
	return true;
}
#endif

}  // namespace

int RootScrollOnlyRefresh::refreshNode(int root,
                                       DirtyNode *extraDirtyNodes,
                                       int *extraDirtyCount,
                                       int *slotNodes,
                                       int *slotCount,
                                       int width,
                                       int height)
{
	auto &state = treeState();
	int foundNode = -1;
	*extraDirtyCount = 0;
	*slotCount = 0;
	// A scroll blit would move fixed pixels along with the scrolling content.
	// Fall back to layout and repaint until the blit path can exclude them.
	if (LayoutEngine::containsViewportFixed(root)) return -1;

	// Pass 1: exactly one pure-scroll node.
	gScrollFrameDirty.clear();
	for (int i = 0; i < state.nodeCount; i++) {
		Node *n = &state.nodes[i];
		if (!n->render.dirty) {
			if (n->render.bg_recolor_pending || n->render.canvas_pixels_only_dirty) gScrollFrameDirty.push_back(i);
			continue;
		}
		gScrollFrameDirty.push_back(i);

		if (n->render.scroll_dirty && !n->render.non_scroll_dirty) {
			if (foundNode >= 0) return -1;
			if (!isViewLikeNodeType(n->type) || n->computedStyle().display == 1) return -1;
			// Accept a pure scroll on exactly one axis the node overflows. The
			// horizontal case mirrors the vertical fast path (see refresh()): a
			// virtual-list is vertical-only. Rejecting a both-axes change keeps the
			// single-axis scrollRect blit unambiguous.
			const bool changedY = n->layout.scroll_y != n->layout.previous_scroll_y;
			const bool changedX = n->layout.scroll_x != n->layout.previous_scroll_x;
			const bool canScrollYAxis = n->type == NodeType::VirtualList || scrollsOverflowY(n->computedStyle());
			const bool canScrollXAxis = n->type != NodeType::VirtualList && scrollsOverflowX(n->computedStyle());
			const bool vertical = changedY && !changedX && canScrollYAxis;
			const bool horizontal = changedX && !changedY && canScrollXAxis;
			if (!vertical && !horizontal) return -1;
			if (!layoutUnchanged(*n)) return -1;
			if (hasTransformState(*n)) return -1;
			// A bordered or rounded scroller (a card that scrolls its list) is
			// accepted too: refresh() repaints its box from the translated list
			// instead of blitting it, which would drag the border and the
			// corners along with the content. Rejecting it sent every scroll
			// frame of the Shop tab's product card through a full relayout and
			// a whole-screen replay (~180 ms a frame).
			if (n->computedStyle().opacity != 255) return -1;
			foundNode = i;
			continue;
		}

		if (n->render.scroll_dirty) return -1;
	}
	if (foundNode < 0) return -1; // no scroll

	// Overlay guard: scrollRect shifts EVERY framebuffer pixel in the scroll
	// viewport, so it is only valid when those pixels belong to the scroll
	// subtree. A node outside the subtree that overlaps the viewport would be
	// dragged with the content. Right-fade masks already disable scrollRect and
	// re-rasterize the complete viewport from the rebuilt root display list; in
	// that branch overlapping siblings are composited afresh, so rejecting them
	// would unnecessarily force a full-tree refresh (the weather city rail has
	// exactly this shape because its negative margin overlaps the hero).
	// The overlays found for this scroller stay valid while nothing outside it
	// changes: no other node is dirty and the display list has not been
	// re-recorded since (any layout or style change records it). Re-deriving
	// them read every node's style and box from PSRAM, ~1.7 ms of each frame of
	// a CrowPanel chart scroll.
	static int overlayCacheNode = -1, overlayCacheRoot = -1;
	static std::uint32_t overlayCacheSerial = 0;
	static int overlayCacheCount = 0;
	static DirtyNode overlayCache[kMaxExtraDirtyNodes];
	// Other dirty nodes (the Analytics meters animate under a scrolling chart)
	// leave it valid while they keep their boxes clear of the viewport: only a
	// node over the viewport can be an overlay.
	bool onlyScrollerDirty = true;
	{
		const Node &sn = state.nodes[foundNode];
		for (const int i : gScrollFrameDirty) {
			if (i == foundNode) continue;
			const Node &o = state.nodes[i];
			if (o.render.layout_dirty || !layoutUnchanged(o) ||
			    !(o.layout.x > sn.layout.x + sn.layout.width - 1 || o.layout.x + o.layout.width - 1 < sn.layout.x ||
			      o.layout.y > sn.layout.y + sn.layout.height - 1 || o.layout.y + o.layout.height - 1 < sn.layout.y)) {
				onlyScrollerDirty = false;
				break;
			}
		}
	}
	const bool overlaysCached = onlyScrollerDirty && overlayCacheNode == foundNode && overlayCacheRoot == root &&
	                            overlayCacheSerial == DisplayList::instance().recordSerial();
	if (overlaysCached) {
		for (int e = 0; e < overlayCacheCount; e++) extraDirtyNodes[e] = overlayCache[e];
		*extraDirtyCount = overlayCacheCount;
	} else {
		overlayCacheNode = -1;
	}
	if (!overlaysCached && !hasRightFadeMask(state, foundNode)) {
		const Node &sn = state.nodes[foundNode];
		const int svx0 = sn.layout.x, svy0 = sn.layout.y;
		const int svx1 = sn.layout.x + sn.layout.width - 1;
		const int svy1 = sn.layout.y + sn.layout.height - 1;
		for (int i = 0; i < state.nodeCount; i++) {
			if (i == foundNode) continue;
			const Node &o = state.nodes[i];
			if (o.computedStyle().display == 1 || o.computedStyle().opacity == 0) continue;
			if (o.layout.width <= 0 || o.layout.height <= 0) continue;
			// Only a node over the viewport can be dragged by the blit. Test that
			// first: the ancestor walks below, run for every node of the tree,
			// cost ~4 ms of each CrowPanel chart scroll frame.
			if (o.layout.x > svx1 || o.layout.x + o.layout.width - 1 < svx0 ||
			    o.layout.y > svy1 || o.layout.y + o.layout.height - 1 < svy0)
				continue;
			// Skip the scroll node's own subtree and its ancestor chain — and any
			// node inside a display:none subtree: a hidden ancestor removes the
			// whole branch from painting even though the descendants' own display
			// stays 0, so they can never contribute viewport pixels the scrollRect
			// shift would drag. (A reader page hidden under a full-screen Contents
			// panel otherwise rejected every scroll frame through its children.)
			bool related = false;
			bool hidden = false;
			for (int a = i; a >= 0; a = state.nodes[a].parent) {
				if (a == foundNode) { related = true; break; }
				if (state.nodes[a].computedStyle().display == 1) { hidden = true; break; }
			}
			if (related || hidden) continue;
			for (int a = foundNode; a >= 0; a = state.nodes[a].parent) {
				if (a == i) { related = true; break; }
			}
			if (related) continue;
			if (paintsNothingOwn(i, o)) continue;
			// An overlay: repaint it after the blit instead of giving up the
			// blit. Its descendants widen the entry of the top-most listed
			// ancestor rather than taking entries of their own.
			RootScrollOnlyRefresh::DirtyNode box{};
			if (!overlayBounds(o, width, height, &box)) continue;
			RootScrollOnlyRefresh::DirtyNode *owner = nullptr;
			for (int a = o.parent; a >= 0 && !owner; a = state.nodes[a].parent) {
				for (int e = 0; e < *extraDirtyCount; e++) {
					if (extraDirtyNodes[e].overlay && extraDirtyNodes[e].id == a) {
						owner = &extraDirtyNodes[e];
						break;
					}
				}
			}
			if (owner) {
				owner->x0 = std::min(owner->x0, box.x0);
				owner->y0 = std::min(owner->y0, box.y0);
				owner->x1 = std::max(owner->x1, box.x1);
				owner->y1 = std::max(owner->y1, box.y1);
				continue;
			}
			if (*extraDirtyCount >= kMaxExtraDirtyNodes) return -1;
			box.id = i;
			box.overlay = true;
			extraDirtyNodes[(*extraDirtyCount)++] = box;
		}
	}

	if (onlyScrollerDirty && !overlaysCached) {
		// Taken before pass 2 adds this frame's own dirty nodes.
		for (int e = 0; e < *extraDirtyCount; e++) overlayCache[e] = extraDirtyNodes[e];
		overlayCacheCount = *extraDirtyCount;
		overlayCacheNode = foundNode;
		overlayCacheRoot = root;
		overlayCacheSerial = DisplayList::instance().recordSerial();
	}
	// Pass 2: classify every other dirty node. Recycled slot children of a
	// scrolling <virtual-list> get repositioned natively by refresh(); their
	// dirty descendants (rebound label text, recolored row content) are covered
	// by the slot's old/new-rect replays over the rebuilt display list, so they
	// are skipped here — including ones whose pre-measured text box changed,
	// which would otherwise reject the whole frame back to the full-relayout
	// path on every fling tick.
	for (const int i : gScrollFrameDirty) {
		Node *n = &state.nodes[i];
		if (!n->render.dirty || i == foundNode) continue;

		if (isSlotRepositionCandidate(state, i, foundNode)) {
			if (*slotCount >= kMaxSlotRepositionNodes) return -1;
			slotNodes[(*slotCount)++] = i;
			continue;
		}

		// Skipping is only safe when the ancestor slot is itself dirty: only then
		// does refresh() replay its old/new rects. A clean slot with a dirty
		// child keeps the regular extra-dirty handling below.
		bool insideSlot = false;
		for (int cursor = n->parent; cursor >= 0; cursor = state.nodes[cursor].parent) {
			if (cursor == foundNode) break;
			if (state.nodes[cursor].render.dirty && isSlotRepositionCandidate(state, cursor, foundNode)) {
				insideSlot = true;
				break;
			}
		}
		if (insideSlot) {
			// Rebound slot text draws from the rebuilt display list, which wraps
			// and clips by the node's measured box — refresh the box so the new
			// label isn't truncated to the old one's width. The slot's old/new
			// rect replays already cover the pixels, including the in-flow
			// siblings the remeasure shifts along the line.
			remeasureDirtyTextInPlace(state, i, true);
			continue;
		}

		// No replay rect covers this node's siblings (only its own dirtyBounds
		// below), so the remeasure must not move them.
		remeasureDirtyTextInPlace(state, i, false);
		// `layoutUnchanged` compares the LAST LAID-OUT box against the PREVIOUS
		// laid-out box, so a node whose style geometry just changed but which has
		// not been laid out since reads "unchanged" and is admitted as paint-only.
		// The pending relayout is then lost for good: this path clears the dirt
		// and returns, and no later refresh recovers it. That is how dismissing
		// the virtual keyboard left `.todo-app` at its shrunken height --
		// `restoreAppResize` writes Height/Flex/Overflow/Position in the same
		// frame the list is still scroll-dirty, and this path claimed the frame.
		// Consult the pending flag, not just the two laid-out boxes; the sibling
		// fast path already screens on exactly this pair (`absolute_leaf_refresh.cpp`).
		if (n->render.layout_dirty &&
		    ((n->computedStyle().height != kUnset && n->computedStyle().height != n->layout.height) ||
		     (n->computedStyle().width != kUnset && n->computedStyle().width != n->layout.width))) {
			return -1;
		}
		if (!layoutUnchanged(*n)) {
			// A re-measured text box is the one layout change this path accepts:
			// dirtyBounds below unions the old and new boxes so the replay covers
			// both, and the anchor stayed put. Anything else still rejects to the
			// full-relayout path.
			const bool remeasuredTextBox = n->type == NodeType::Text &&
			                               !n->render.text_layout_stable &&
			                               n->layout.y == n->layout.previous_y;
			if (!remeasuredTextBox) return -1;
		}
		if (*extraDirtyCount >= kMaxExtraDirtyNodes) return -1;
		DirtyNode dirty{};
		dirty.id = i;
		if (dirtyBounds(*n, width, height, &dirty))
			extraDirtyNodes[(*extraDirtyCount)++] = dirty;
	}
	return foundNode;
}

void RootScrollOnlyRefresh::shiftDescendantLayoutY(int id, int dy)
{
	if (dy == 0) return;
	auto &state = treeState();
	for (int c = state.nodes[id].first_child; c >= 0; c = state.nodes[c].next_sibling) {
		state.nodes[c].layout.y += dy;
		shiftDescendantLayoutY(c, dy);
	}
}

void RootScrollOnlyRefresh::shiftDescendantLayoutX(int id, int dx)
{
	if (dx == 0) return;
	auto &state = treeState();
	for (int c = state.nodes[id].first_child; c >= 0; c = state.nodes[c].next_sibling) {
		state.nodes[c].layout.x += dx;
		shiftDescendantLayoutX(c, dx);
	}
}

int RootScrollOnlyRefresh::refresh(int root, int width, int height)
{
	auto &state = treeState();
	auto &perf = refreshPerfStatsMutable();
	GEA_REFRESH_PERF(perf.rootScrollCalls++);
	DirtyNode extraDirtyNodes[kMaxExtraDirtyNodes];
	int extraDirtyCount = 0;
	int slotNodes[kMaxSlotRepositionNodes];
	int slotCount = 0;
	const std::int64_t scanStartUs = refreshPerfNowUs();
	int scroll_node = refreshNode(root, extraDirtyNodes, &extraDirtyCount, slotNodes, &slotCount, width, height);
	GEA_REFRESH_PERF(perf.rootScrollScanUs += refreshPerfNowUs() - scanStartUs);
	if (scroll_node < 0) {
		GEA_REFRESH_PERF(perf.rootScrollRejected++);
		return 0;
	}
#if GEA_CSS_STICKY
	// Shifting content by the scroll delta drags sticky boxes along; the ones
	// this scroll re-sticks move back after the shift and repaint their old
	// and new places below.
	int stickySlots[TreeState::kMaxStickyBoxes];
	int stickyCount = state.stickyPresent ? LayoutEngine::stickyBoxesScrolledBy(scroll_node, stickySlots) : 0;
	if (stickyCount > 0 && !stickyMovesFollowable(state, stickySlots, stickyCount, extraDirtyNodes, extraDirtyCount, slotCount))
		stickyCount = -1;
	if (stickyCount < 0) {
		GEA_REFRESH_PERF(perf.rootScrollRejected++);
		return 0;
	}
#endif
	GEA_REFRESH_PERF(perf.rootScrollAccepted++);
	GEA_REFRESH_PERF(perf.rootScrollExtraDirtyNodes += extraDirtyCount);

	Node *n = &state.nodes[scroll_node];
	// refreshNode accepted a pure single-axis scroll; derive which axis moved.
	const bool horizontal = n->layout.scroll_x != n->layout.previous_scroll_x;
	const int scroll_delta = horizontal ? (n->layout.scroll_x - n->layout.previous_scroll_x)
	                                     : (n->layout.scroll_y - n->layout.previous_scroll_y);
	const int move = -scroll_delta;
	const int abs_move = move < 0 ? -move : move;

	// Children's resolved coords already encode the previous scroll (layout applies
	// `x -= scroll_x` / `y -= scroll_y`); skipping layout, we apply only this
	// frame's delta along the scrolled axis to keep them in sync.
	if (horizontal) shiftDescendantLayoutX(scroll_node, move);
	else shiftDescendantLayoutY(scroll_node, move);
#if GEA_CSS_STICKY
	StickyMove stickyMoves[kMaxStickyMoves];
	int stickyMoveCount = 0;
	bool stickyRepaintAll = false;
	for (int s = 0; s < stickyCount; s++) {
		const int node = state.stickyBoxes[stickySlots[s]].node;
		int dx, dy;
		LayoutEngine::restickScrolledBox(stickySlots[s], &dx, &dy);
		if (!dx && !dy) continue;
		if (stickyMoveCount == kMaxStickyMoves) stickyRepaintAll = true;
		else stickyMoves[stickyMoveCount++] = {node, dx, dy, PaintBounds::none(), PaintBounds::none()};
	}
	if (stickyRepaintAll) stickyMoveCount = 0;
#else
	constexpr bool stickyRepaintAll = false;
#endif

	// Reconcile repositioned <virtual-list> slots natively (mirrors the layout
	// engine's absolute-child resolution: the positioned list is the containing
	// block, so y = list.y - scroll_y + top). This runs after the scroll shift
	// and before the display-list rebuild, so recycled slots record at their
	// fresh positions without a layout pass; their old and new rects are
	// replayed after the strip below. Width/height stay as laid out — a slot
	// rebind only moves the box.
	struct SlotDamage {
		int oldX0, oldY0, oldX1, oldY1;
		int newX0, newY0, newX1, newY1;
		bool moved;
	};
	SlotDamage slotDamage[kMaxSlotRepositionNodes];
	for (int s = 0; s < slotCount; s++) {
		Node &slot = state.nodes[slotNodes[s]];
		SlotDamage &damage = slotDamage[s];
		damage.oldX0 = slot.layout.x;
		damage.oldY0 = slot.layout.y;
		damage.oldX1 = slot.layout.x + slot.layout.width - 1;
		damage.oldY1 = slot.layout.y + slot.layout.height - 1;
		damage.moved = false;
		if (slot.computedStyle().display == 1) continue;  // display:none — erase old rect only

		int newX = slot.layout.x;
		if (GEA_CSS_POSITION_PX_3(slot.computedStyle()) != kUnset || GEA_CSS_POSITION_PERCENT_3(slot.computedStyle()) != kUnset)
			newX = n->layout.x + resolvedSlotOffset<3>(slot, n->layout.width);
		else if (GEA_CSS_POSITION_PX_1(slot.computedStyle()) != kUnset || GEA_CSS_POSITION_PERCENT_1(slot.computedStyle()) != kUnset)
			newX = n->layout.x + n->layout.width - slot.layout.width -
			       resolvedSlotOffset<1>(slot, n->layout.width);

		int newY = slot.layout.y;
		if (GEA_CSS_POSITION_PX_0(slot.computedStyle()) != kUnset || GEA_CSS_POSITION_PERCENT_0(slot.computedStyle()) != kUnset)
			newY = n->layout.y - n->layout.scroll_y + resolvedSlotOffset<0>(slot, n->layout.height);
		else if (GEA_CSS_POSITION_PX_2(slot.computedStyle()) != kUnset || GEA_CSS_POSITION_PERCENT_2(slot.computedStyle()) != kUnset)
			newY = n->layout.y - n->layout.scroll_y + n->layout.height - slot.layout.height -
			       resolvedSlotOffset<2>(slot, n->layout.height);

		const int dx = newX - slot.layout.x;
		const int dy = newY - slot.layout.y;
		slot.layout.x = newX;
		slot.layout.y = newY;
		translateSlotDescendants(state, slotNodes[s], dx, dy);
		damage.newX0 = newX;
		damage.newY0 = newY;
		damage.newX1 = newX + slot.layout.width - 1;
		damage.newY1 = newY + slot.layout.height - 1;
		damage.moved = dx != 0 || dy != 0;
	}

#if GEA_CSS_STICKY
	// Where each re-stuck box painted last frame, in the list that painted it;
	// the scroll blit below drags those pixels by `move`.
	for (int s = 0; s < stickyMoveCount; s++) {
		PaintBounds &old = stickyMoves[s].old;
		old = PaintBounds::none();
		uniteSubtreeCommandBounds(state, stickyMoves[s].node, old);
		old.shift(horizontal ? move : 0, horizontal ? 0 : move);
	}
#endif

	int vx = n->layout.x;
	int vy = n->layout.y;
	int vw = n->layout.width;
	int vh = n->layout.height;
	if (!ViewportRegion::clipRect(width, height, &vx, &vy, &vw, &vh)) {
		const std::int64_t snapshotStartUs = refreshPerfNowUs();
		LayoutSnapshot::capture();
		GEA_REFRESH_PERF(perf.rootScrollSnapshotUs += refreshPerfNowUs() - snapshotStartUs);
		return 1;
	}
	// The scroll replay needs a display list whose commands match the
	// post-shift layout, and recordNode drops commands for nodes outside the
	// record clip — so off-viewport content has no commands until it scrolls
	// in. Historically that forced a full clear()+recordNode() every scroll
	// frame (~2ms). The translate-only path below skips it, mirroring the
	// horizontal-pan fast path: record ONCE with the record clip expanded
	// along the scrolled axis around the viewport (coverage), then per scroll
	// frame just translate the scroll subtree's recorded commands in place
	// (they follow shiftDescendantLayoutX/Y), un-translate the scroll node's
	// own viewport-fixed box, and patch the scrollbar thumb. Coverage is
	// re-recorded when the accumulated scroll leaves the padded window or any
	// other record replaced the list (recordSerial). Virtual-list slot rebinds
	// keep the plain full rebuild — a translate can't express them.
	// (An earlier hybrid that recorded/cleared subtrees incrementally failed
	// because DisplayList::replay() walks state.commands[] linearly and
	// orphaned ranges kept replaying; wholesale translate avoids that
	// entirely: no command is added or removed.)
	{
		static std::uint32_t coverageSerial = 0;
		static int coverageRoot = -1;
		static int coverageNode = -1;
		static bool coverageHorizontal = false;
		static int coverageScroll = 0;
		static int coveragePadBefore = 0;
		static int coveragePadAfter = 0;
		// Window fallback for content too long to pre-record in one pass
		// (command coords are int16, and the command buffer is finite).
		constexpr int kScrollRecordFallbackPadding = 512;
		constexpr int kMaxFullContentExtent = 16000;

		auto &list = DisplayList::instance();
		// The coverage window runs along the scrolled axis: rows for a vertical
		// scroller, columns for a horizontal one (a tab pager), so a pager's
		// pages are recorded once and every frame of a swipe only translates
		// them -- a whole-list rebuild cost ~32 ms a frame on the CrowPanel.
		const int scrollNow = horizontal ? n->layout.scroll_x : n->layout.scroll_y;
		const int viewExtent = horizontal ? n->layout.width : n->layout.height;
		const int contentExtent = horizontal ? n->layout.scroll_content_width : n->layout.scroll_content_height;
		// Overlays only need repainting; their commands do not change.
		int changedExtras = 0;
		for (int e = 0; e < extraDirtyCount; e++)
			if (!extraDirtyNodes[e].overlay) changedExtras++;
		const bool translateEligible = GEA_EMBEDDED_ROOT_SCROLL_TRANSLATE != 0 &&
		                               n->type != NodeType::VirtualList &&
		                               slotCount == 0 &&
		                               !stickyRepaintAll &&
		                               !list.hasColumnCopies();
		const int coverageDelta = scrollNow - coverageScroll;
		const bool coverageValid = translateEligible &&
		                           coverageRoot == root &&
		                           coverageNode == scroll_node &&
		                           coverageHorizontal == horizontal &&
		                           coverageSerial == list.recordSerial() &&
		                           coverageDelta >= -coveragePadBefore &&
		                           coverageDelta <= coveragePadAfter;
		// The coverage window as it lies on screen now, moved by the scroll since
		// it was recorded.
		auto coverageWindow = [&](int padBefore, int padAfter, int delta, int *x0, int *y0, int *x1, int *y1) {
			*x0 = vx;
			*y0 = vy;
			*x1 = vx + vw - 1;
			*y1 = vy + vh - 1;
			if (horizontal) {
				*x0 = vx - padBefore - delta;
				*x1 = vx + vw - 1 + padAfter - delta;
			} else {
				*y0 = vy - padBefore - delta;
				*y1 = vy + vh - 1 + padAfter - delta;
			}
		};

		const std::int64_t rebuildStartUs = refreshPerfNowUs();
		bool listReady = false;
		if (coverageValid) {
			const int mx = horizontal ? move : 0;
			const int my = horizontal ? 0 : move;
			list.translateSubtreeCommands(scroll_node, mx, my);
			// translateSubtreeCommands includes the scroll node's own box
			// commands (background/border), which are viewport-fixed — undo.
			list.translateNodeCommands(scroll_node, -mx, -my);
#if GEA_CSS_STICKY
			for (int s = 0; s < stickyMoveCount; s++)
				translateStickyCommands(state, stickyMoves[s].node, stickyMoves[s].dx, stickyMoves[s].dy);
#endif
			listReady = list.patchScrollbarThumb(scroll_node);
			// Changed content re-records in place at its shifted layout, under the
			// coverage window the list was recorded with (now moved by the scroll
			// since), so nodes off screen keep their commands for when they scroll
			// in. Rebuilding the whole list for them cost 100+ ms per frame on a
			// tab of animated meters.
			int wx0, wy0, wx1, wy1;
			coverageWindow(coveragePadBefore, coveragePadAfter, coverageDelta, &wx0, &wy0, &wx1, &wy1);
			for (int e = 0; e < extraDirtyCount && listReady; e++) {
				if (extraDirtyNodes[e].overlay) continue;
				listReady = list.rerecordNodeCommandsWithExpandedClip(extraDirtyNodes[e].id, scroll_node, wx0, wy0, wx1, wy1);
			}
		}
		if (!listReady && translateEligible) {
			// Pre-record the WHOLE scroll content when it fits: one record,
			// replayed (translated) forever — scrolling never re-records. For
			// very long content, fall back to a padded window around the
			// viewport; a truncated record (command-buffer overflow) also
			// falls back so coverage is never silently incomplete.
			int padBefore = scrollNow;
			int padAfter = contentExtent - scrollNow - viewExtent;
			if (padAfter < 0) padAfter = 0;
			// A pager swipe ends one page over, so a horizontal record stops a page
			// either side of the viewport: recording all three of the CrowPanel
			// demo's pages cost ~71 ms on a swipe's first frame. A drag that goes
			// further leaves the window and re-records, like any other scroll.
			if (horizontal) {
				padBefore = padBefore < viewExtent ? padBefore : viewExtent;
				padAfter = padAfter < viewExtent ? padAfter : viewExtent;
			}
			if (contentExtent > kMaxFullContentExtent) {
				padBefore = padBefore < kScrollRecordFallbackPadding ? padBefore : kScrollRecordFallbackPadding;
				padAfter = padAfter < kScrollRecordFallbackPadding ? padAfter : kScrollRecordFallbackPadding;
			}
			for (int attempt = 0; attempt < 2 && !listReady; attempt++) {
				int wx0, wy0, wx1, wy1;
				coverageWindow(padBefore, padAfter, 0, &wx0, &wy0, &wx1, &wy1);
				list.clear();
				list.recordNodeWithExpandedClip(root, 255, scroll_node, wx0, wy0, wx1, wy1);
				list.weldTransformedFaces();
				if (!list.commandOverflow()) {
					listReady = true;
					coverageSerial = list.recordSerial();
					coverageRoot = root;
					coverageNode = scroll_node;
					coverageHorizontal = horizontal;
					coverageScroll = scrollNow;
					coveragePadBefore = padBefore;
					coveragePadAfter = padAfter;
					break;
				}
				// Overflowed: retry once with the small window.
				padBefore = padBefore < kScrollRecordFallbackPadding ? padBefore : kScrollRecordFallbackPadding;
				padAfter = padAfter < kScrollRecordFallbackPadding ? padAfter : kScrollRecordFallbackPadding;
			}
			if (listReady) state.displayListDirty = false;
		}
		if (!listReady) {
			list.clear();
			list.recordNode(root, 255);
			state.displayListDirty = false;
			coverageRoot = -1;
		}
		GEA_REFRESH_PERF(perf.rootScrollRebuildUs += refreshPerfNowUs() - rebuildStartUs);
	}
#if GEA_CSS_STICKY
	// A box stuck to an edge of the scrollport stays put on screen. When every
	// re-stuck box holds such a band, the blit and the strip leave the bands out
	// and nothing repaints there; each box repaints only what it paints outside.
	int bandLead = 0, bandTrail = 0;
	bool banded = stickyMoveCount > 0;
	for (int s = 0; s < stickyMoveCount; s++) {
		StickyMove &moved = stickyMoves[s];
		moved.now = PaintBounds::none();
		uniteSubtreeCommandBounds(state, moved.node, moved.now);
		banded = banded && stuckBand(state, moved, horizontal, move, scroll_node, vx, vy, vw, vh, &bandLead, &bandTrail);
	}
	if (!banded) bandLead = bandTrail = 0;
#endif
	GEA_REFRESH_PERF(perf.rootScrollMovePx += abs_move);
	GEA_REFRESH_PERF(perf.rootScrollViewportPx += vw * vh);
	GEA_REFRESH_PERF(perf.rootScrollStripPx += horizontal ? (vh * (abs_move < vw ? abs_move : vw))
	                                     : (vw * (abs_move < vh ? abs_move : vh)));

	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::setAlpha(255);
	const bool kSelfFlushingScroll = GEA_EMBEDDED_DISPLAY_SCROLLRECT_SELF_FLUSHING != 0 || gea_display_scroll_rect_self_flushing();
	constexpr int kMaxDamageRects = 24;
	gea::platform::display::DisplayFlushRect damageRects[kMaxDamageRects];
	int damageCount = 0;
	bool damageOverflow = false;
	auto noteDamage = [&](int rx, int ry, int rw, int rh) {
		if (!kSelfFlushingScroll) return;
		if (damageCount >= kMaxDamageRects) {
			damageOverflow = true;
			return;
		}
		damageRects[damageCount++] = {rx, ry, rx + rw - 1, ry + rh - 1};
	};
	gScrollRanThisFrame = true;
	// Hold image blits only when scroll frames are CONSECUTIVE (live drag /
	// fling); an isolated jump (row-snap commit) paints images inline.
	const bool holdImages = gUiFrameCounter - gLastScrollUiFrame <= 3;
	gLastScrollUiFrame = gUiFrameCounter;
	auto replayScrollNodeRegion = [&](int rx, int ry, int rw, int rh) {
		noteDamage(rx, ry, rw, rh);
		gScrollStripReplayActive = holdImages;
		// The revealed band lands right beside rows the panel is scanning: always
		// stage it (a drag's band is one to three strips).
		ViewportRegion::replayDisplayRegionStaged(rx, ry, rw, rh);
		gScrollStripReplayActive = false;
	};

	// A right-edge fade mask (`mask-image: linear-gradient(..., transparent)`) makes
	// each child's alpha depend on its X relative to the container's *fixed* right
	// edge — it is NOT translation-invariant. The scrollRect blit below shifts the
	// already-faded framebuffer pixels and only repaints the freshly-revealed edge
	// strip, which drags the faded band along with the content (the gradient
	// "scrolls like content" instead of staying pinned to the edge). recordNode
	// above already rebuilt the display list with the per-child fade recomputed at
	// the post-shift positions, so for a masked scroller we skip the blit and
	// re-rasterize the whole viewport from that list — the fade stays anchored.
	const bool contentHasRightFadeMask = hasRightFadeMask(state, scroll_node);
	// A software-rotated canvas (e.g. a 172x640 panel driven as 640x172)
	// stores logical rows as framebuffer columns; neither the scroll-register
	// row remap nor the pixel blit shifts it correctly, and the stale pixels
	// left behind showed as content drawn over itself. Re-rasterize the whole
	// viewport from the rebuilt display list instead.
	// A bordered or rounded scroller (a card that scrolls its list) repaints its
	// whole box from the translated list: a blit would drag the frame along,
	// and a partial-width blit on a ring-scrolled RGB panel relayouts and
	// moves the scanout buffers, which garbled the card on the CrowPanel.
	bool blitScroll = !contentHasRightFadeMask && !scrollerHasBoxFrame(*n);
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (auto *canvas = gea::platform::display::Display::canvas(); canvas && canvas->isLandscapeRotated()) blitScroll = false;
#endif

	const int viewportExtent = horizontal ? vw : vh;
	// The part of the scrollport the blit moves.
	int bx = vx, by = vy, bw = vw, bh = vh;
#if GEA_CSS_STICKY
	if (horizontal) {
		bx += bandLead;
		bw -= bandLead + bandTrail;
	} else {
		by += bandLead;
		bh -= bandLead + bandTrail;
	}
	const bool bands = bandLead > 0 || bandTrail > 0;
	static bool lastBands = false;
	bool blit = !stickyRepaintAll && blitScroll && abs_move > 0 && abs_move < (horizontal ? bw : bh);
	// A software scroll register (esp32 display.cpp) rotates the rows of the
	// rect it scrolled last and restarts at zero for a new rect, which
	// scrambles those rows. Bands change the rect: reset the register and
	// repaint its rows and the scrollport once instead of blitting.
	if (blit && !horizontal && (bands || lastBands)) {
		auto *canvas = gea::platform::display::Display::canvas();
		const int regionY = canvas ? canvas->scrollRegionY() : 0, regionH = canvas ? canvas->scrollRegionH() : 0;
		if (regionH > 0 && (regionY != by || regionH != bh)) {
			gea::platform::display::Display::resetScrollRegion();
			replayScrollNodeRegion(0, regionY, width, regionH);
			blit = false;
		}
	}
	lastBands = bands;
#else
	const bool blit = blitScroll && abs_move > 0 && abs_move < viewportExtent;
#endif
	if (blit) {
		const std::int64_t scrollRectStartUs = refreshPerfNowUs();
		// A sideways copy started above overlaps the strip repaint: the strip is
		// exactly the columns the copy leaves alone.
		// The copy starts only now, after the display list is up to date: the
		// rebuild touches no pixels, so it runs while the frame waits for the
		// panel to release the back buffer, and the copy (bus-bound, ~21 ms for a
		// CrowPanel chart) shares PSRAM with nothing but the strip repaint. Started
		// before the rebuild, the two contended and a chart frame missed the panel
		// period by ~3 ms.
		// The exposed strip, the repositioned slots and the overlay boxes all
		// replay from one display-list walk, filtered for the box holding them
		// all (each region otherwise walked the whole list: ~1.2 ms apiece).
		// Filtered before the copy starts, while the frame still waits for the
		// back buffer.
		if (horizontal) {
			int ux0 = move < 0 ? bx + bw - abs_move : bx, uy0 = by;
			int ux1 = ux0 + abs_move - 1, uy1 = by + bh - 1;
			auto unite = [&](int x0, int y0, int x1, int y1) {
				ux0 = std::min(ux0, x0); uy0 = std::min(uy0, y0);
				ux1 = std::max(ux1, x1); uy1 = std::max(uy1, y1);
			};
			for (int s = 0; s < slotCount; s++) {
				unite(slotDamage[s].oldX0, slotDamage[s].oldY0, slotDamage[s].oldX1, slotDamage[s].oldY1);
				if (slotDamage[s].moved) unite(slotDamage[s].newX0, slotDamage[s].newY0, slotDamage[s].newX1, slotDamage[s].newY1);
			}
			for (int i = 0; i < extraDirtyCount; i++)
				unite(extraDirtyNodes[i].x0 - abs_move, extraDirtyNodes[i].y0, extraDirtyNodes[i].x1 + abs_move, extraDirtyNodes[i].y1);
			int uw = ux1 - ux0 + 1, uh = uy1 - uy0 + 1;
			if (ViewportRegion::clipRect(width, height, &ux0, &uy0, &uw, &uh)) {
				gea::platform::display::Display::resetClip();
				gea::platform::display::Display::pushClip(ux0, uy0, uw, uh);
				DisplayList::instance().beginRegionReplay();
				gea::platform::display::Display::popClip();
			}
		}
		bool asyncCopy = false;
		if (horizontal) {
			gea::platform::display::Display::resetClip();
			// Rows only full-width fills touch (a chart's empty band and rules) look
			// the same after the shift; the display skips those its back buffer
			// already holds, cutting the bus-bound copy.
			constexpr int kMaxSigRows = 1024;
			GEA_EXT_BSS static std::uint32_t rowSig[kMaxSigRows];
			// No signatures (a filter blur, too tall) must be said too: the
			// buffer may hold a half-written walk.
			if (bh <= kMaxSigRows && DisplayList::instance().scrollRowSignatures(bx, by, bw, bh, rowSig))
				gea_display_scroll_rect_rows(rowSig, by, bh);
			else
				gea_display_scroll_rect_rows(nullptr, 0, 0);
			asyncCopy = gea_display_scroll_rect_begin(bx, by, bw, bh, move, 0);
		}
		if (!asyncCopy)
			gea::platform::display::Display::scrollRect(bx, by, bw, bh, horizontal ? move : 0, horizontal ? 0 : move);
		GEA_REFRESH_PERF(perf.rootScrollScrollRectUs += refreshPerfNowUs() - scrollRectStartUs);

		const std::int64_t stripReplayStartUs = refreshPerfNowUs();
		// The newly-revealed edge strip.
		int stripX = bx, stripY = by, stripW = bw, stripH = bh;
		if (horizontal) {
			stripW = abs_move;
			if (move < 0) stripX = bx + bw - abs_move;
		} else {
			stripH = abs_move;
			if (move < 0) stripY = by + bh - abs_move;
		}
#if GEA_CSS_STICKY
		// A re-stuck box left the content: repaint, inside the blit, where the
		// blit dragged its pixels and where it sits now. A rect touching the
		// strip joins it; then nothing holds images, since a stuck header would
		// repaint every frame and never show a held one.
		PaintBounds stickyDamage[2 * kMaxStickyMoves];
		int stickyDamageCount = 0;
		bool stripHoldsImages = holdImages;
		{
			const PaintBounds blitRect{bx, by, bx + bw - 1, by + bh - 1};
			auto clipToBlit = [&](PaintBounds b) {
				b.x0 = std::max(b.x0, blitRect.x0);
				b.y0 = std::max(b.y0, blitRect.y0);
				b.x1 = std::min(b.x1, blitRect.x1);
				b.y1 = std::min(b.y1, blitRect.y1);
				return b;
			};
			PaintBounds strip{stripX, stripY, stripX + stripW - 1, stripY + stripH - 1};
			for (int s = 0; s < stickyMoveCount; s++) {
				const StickyMove &moved = stickyMoves[s];
				// The stale pixels: last frame's paint dragged by the blit, which is
				// also the box's paint now taken back by its own shift.
				PaintBounds stale = moved.now;
				stale.shift(-moved.dx, -moved.dy);
				stale.unite(moved.old);
				stale = clipToBlit(stale);
				PaintBounds now = clipToBlit(moved.now);
				if (stale.touches(now)) {
					stale.unite(now);
					now = PaintBounds::none();
				}
				for (const PaintBounds &b : {stale, now}) {
					if (b.empty()) continue;
					if (b.touches(strip)) {
						strip.unite(b);
						stripHoldsImages = false;
					} else {
						stickyDamage[stickyDamageCount++] = b;
					}
				}
			}
			stripX = strip.x0;
			stripY = strip.y0;
			stripW = strip.x1 - strip.x0 + 1;
			stripH = strip.y1 - strip.y0 + 1;
		}
		// Staged like replayScrollNodeRegion: a staged replay outside the
		// region filtered above re-filters instead of walking a list that
		// lacks this rect's commands.
		noteDamage(stripX, stripY, stripW, stripH);
		gScrollStripReplayActive = stripHoldsImages;
		ViewportRegion::replayDisplayRegionStaged(stripX, stripY, stripW, stripH);
		gScrollStripReplayActive = false;
		for (int d = 0; d < stickyDamageCount; d++) {
			const PaintBounds &b = stickyDamage[d];
			noteDamage(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
			ViewportRegion::replayDisplayRegionStaged(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
		}
#else
		replayScrollNodeRegion(stripX, stripY, stripW, stripH);
#endif
		if (asyncCopy) gea_display_scroll_rect_end();
		GEA_REFRESH_PERF(perf.rootScrollStripReplayUs += refreshPerfNowUs() - stripReplayStartUs);

		// Scrollbar damage is vertical-only: the horizontal forecast/rail rows use
		// scrollbar-width:none and draw no thumb, so a horizontal scroll skips it.
		if (!horizontal) {
			const int move_y = move;
			const std::int64_t scrollbarReplayStartUs = refreshPerfNowUs();
			auto replayScrollbarDamage = [&](int damagedY, int damagedH) {
				int sx = n->layout.x + n->layout.width - 10;
				int sy = damagedY;
				int sw = 10;
				int sh = damagedH;
				if (ViewportRegion::clipRect(width, height, &sx, &sy, &sw, &sh))
					replayScrollNodeRegion(sx, sy, sw, sh);
			};
			int oldThumbY = 0, oldThumbH = 0;
			int newThumbY = 0, newThumbH = 0;
			const bool hasOldThumb = nonVirtualListScrollbarThumbBounds(
				*n, n->layout.previous_scroll_y, &oldThumbY, &oldThumbH);
			const bool hasNewThumb = nonVirtualListScrollbarThumbBounds(
				*n, n->layout.scroll_y, &newThumbY, &newThumbH);
			if (hasOldThumb && hasNewThumb) {
				const int shiftedOldY = oldThumbY + move_y;
				const int shiftedOldBottom = shiftedOldY + oldThumbH - 1;
				const int newThumbBottom = newThumbY + newThumbH - 1;
				if (shiftedOldBottom + 1 >= newThumbY && newThumbBottom + 1 >= shiftedOldY) {
					int damageY = shiftedOldY < newThumbY ? shiftedOldY : newThumbY;
					int damageBottom = shiftedOldBottom > newThumbBottom ? shiftedOldBottom : newThumbBottom;
#if GEA_CSS_STICKY
					// The thumb rows inside a band were not blitted.
					if (bands) {
						damageY = std::min(damageY, oldThumbY);
						damageBottom = std::max(damageBottom, oldThumbY + oldThumbH - 1);
					}
#endif
					replayScrollbarDamage(damageY, damageBottom - damageY + 1);
				} else {
					replayScrollbarDamage(shiftedOldY, oldThumbH);
					replayScrollbarDamage(newThumbY, newThumbH);
#if GEA_CSS_STICKY
					if (bands) replayScrollbarDamage(oldThumbY, oldThumbH);
#endif
				}
			} else {
				replayScrollbarDamage(n->layout.y, n->layout.height);
			}
			GEA_REFRESH_PERF(perf.rootScrollScrollbarReplayUs += refreshPerfNowUs() - scrollbarReplayStartUs);
		}
	} else {
		const std::int64_t fullReplayStartUs = refreshPerfNowUs();
		// The viewport is about to be painted whole when an opaque fill covers it:
		// ready a double-buffered panel's back buffer without first copying the
		// previous frame's pixels into the viewport. That copy cost as much as the
		// sideways blit it replaces (~20 ms for a CrowPanel chart).
		gea::platform::display::Display::pushClip(vx, vy, vw, vh);
		DisplayList::instance().replayCullStart();
		const bool viewportCovered = DisplayList::instance().replayCullCovered();
		gea::platform::display::Display::popClip();
		if (viewportCovered) {
			const gea::platform::display::DisplayFlushRect whole = {vx, vy, vx + vw - 1, vy + vh - 1};
			gea_display_prepare_repaint(&whole, 1);
		}
		replayScrollNodeRegion(vx, vy, vw, vh);
		GEA_REFRESH_PERF(perf.rootScrollFullReplayUs += refreshPerfNowUs() - fullReplayStartUs);
	}

	// Repaint repositioned slots: the vacated box (now showing whatever sits
	// beneath) and the destination box (the slot at its fresh position). Both
	// replay the rebuilt display list clipped to the list viewport — slot pixels
	// outside it are clipped by the list at record time anyway.
	const std::int64_t slotReplayStartUs = refreshPerfNowUs();
	auto replaySlotRect = [&](int x0, int y0, int x1, int y1) {
		if (x0 < vx) x0 = vx;
		if (y0 < vy) y0 = vy;
		if (x1 > vx + vw - 1) x1 = vx + vw - 1;
		if (y1 > vy + vh - 1) y1 = vy + vh - 1;
		if (x0 > x1 || y0 > y1) return;
		replayScrollNodeRegion(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
	};
	// When abs_move >= viewportExtent the branch above already replayed the
	// whole viewport from the fresh display list — per-slot rects would only
	// repaint subsets of it.
	if (abs_move < viewportExtent) {
		for (int s = 0; s < slotCount; s++) {
			const SlotDamage &damage = slotDamage[s];
			replaySlotRect(damage.oldX0, damage.oldY0, damage.oldX1, damage.oldY1);
			if (damage.moved)
				replaySlotRect(damage.newX0, damage.newY0, damage.newX1, damage.newY1);
		}
	}
	GEA_REFRESH_PERF(perf.rootScrollStripReplayUs += refreshPerfNowUs() - slotReplayStartUs);

	const std::int64_t extraReplayStartUs = refreshPerfNowUs();
	// Every extra box is collected, overlapping ones merged (an animated meter's
	// arc, needle and readout share one box), and each merged box is staged
	// through SRAM strips. Painted directly, a scroll frame's dozen meter boxes
	// left hundreds of KB of dirty framebuffer lines in the cache, which the
	// bounce refill's reads then evicted -- written back inside its window, so
	// it overran and the panel showed a half-refilled band (thin flickering
	// lines while scrolling the CrowPanel Analytics tab).
	struct ExtraBox { int x0, y0, x1, y1; };
	ExtraBox boxes[kMaxExtraDirtyNodes * 2];
	int boxCount = 0;
	auto addBox = [&](int x0, int y0, int x1, int y1) {
		x0 = std::max(x0, 0);
		y0 = std::max(y0, 0);
		x1 = std::min(x1, width - 1);
		y1 = std::min(y1, height - 1);
		if (x0 > x1 || y0 > y1) return;
		boxes[boxCount++] = {x0, y0, x1, y1};
	};
	for (int i = 0; i < extraDirtyCount; i++) {
		DirtyNode dirty = extraDirtyNodes[i];
		if (dirty.overlay) {
			// The blit dragged the overlay's pixels by `move` inside the viewport:
			// repaint that shifted box (clipped to the viewport) as well as the box.
			int sx0 = dirty.x0 + (horizontal ? move : 0), sy0 = dirty.y0 + (horizontal ? 0 : move);
			int sx1 = dirty.x1 + (horizontal ? move : 0), sy1 = dirty.y1 + (horizontal ? 0 : move);
			sx0 = std::max(sx0, vx);
			sy0 = std::max(sy0, vy);
			sx1 = std::min(sx1, vx + vw - 1);
			sy1 = std::min(sy1, vy + vh - 1);
			if (sx0 <= sx1 && sy0 <= sy1) addBox(sx0, sy0, sx1, sy1);
		}
		// Bounds were computed before the scroll shift; content inside the
		// scrolled container has since moved by `move` along the scrolled axis.
		// Cover both the stale and shifted positions.
		if (isDescendantOf(state, dirty.id, scroll_node)) {
			if (horizontal) {
				if (move < 0) dirty.x0 += move; else dirty.x1 += move;
			} else {
				if (move < 0) dirty.y0 += move; else dirty.y1 += move;
			}
		}
		addBox(dirty.x0, dirty.y0, dirty.x1, dirty.y1);
	}
	for (bool merged = true; merged;) {
		merged = false;
		for (int a = 0; a < boxCount; a++) {
			for (int b = a + 1; b < boxCount; b++) {
				if (boxes[a].x0 > boxes[b].x1 || boxes[b].x0 > boxes[a].x1 ||
				    boxes[a].y0 > boxes[b].y1 || boxes[b].y0 > boxes[a].y1)
					continue;
				boxes[a] = {std::min(boxes[a].x0, boxes[b].x0), std::min(boxes[a].y0, boxes[b].y0),
				            std::max(boxes[a].x1, boxes[b].x1), std::max(boxes[a].y1, boxes[b].y1)};
				boxes[b] = boxes[--boxCount];
				merged = true;
				--b;
			}
		}
	}
	for (int i = 0; i < boxCount; i++) {
		const ExtraBox &b = boxes[i];
		gea::platform::display::Display::resetClip();
		gea::platform::display::Display::setAlpha(255);
		noteDamage(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
		ViewportRegion::replayDisplayRegionStaged(b.x0, b.y0, b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
	}
	GEA_REFRESH_PERF(perf.rootScrollExtraReplayUs += refreshPerfNowUs() - extraReplayStartUs);
	DisplayList::instance().endRegionReplay();

	const std::int64_t flushStartUs = refreshPerfNowUs();
	if (kSelfFlushingScroll && !damageOverflow) {
		if (damageCount > 0)
			gea::platform::display::Display::flushRects(damageRects, damageCount);
	} else {
		gea::platform::display::Display::flush();
	}
	GEA_REFRESH_PERF(perf.rootScrollFlushUs += refreshPerfNowUs() - flushStartUs);

	const std::int64_t snapshotStartUs = refreshPerfNowUs();
	LayoutSnapshot::captureNodes(gScrollFrameDirty.data(), static_cast<int>(gScrollFrameDirty.size()), scroll_node);
	GEA_REFRESH_PERF(perf.rootScrollSnapshotUs += refreshPerfNowUs() - snapshotStartUs);

	return 1;
}


void rootScrollImageHoldTick()
{
	gUiFrameCounter++;
	// Runs at frame entry, before any scroll work. A frame that scrolled last
	// tick re-arms below; the first quiet frame after held (skipped) image
	// blits schedules one normal full refresh so covers repaint.
	if (gScrollRanThisFrame) {
		gScrollRanThisFrame = false;
		return;
	}
	if (!gScrollImageHoldPending) return;
	gScrollImageHoldPending = false;
	auto &state = treeState();
	if (state.mountedRoot < 0) return;
	Tree::instance().markDisplayListDirty();
	state.nodes[state.mountedRoot].render.dirty = 1;
#if GEA_CSS_SCROLLING
	state.nodes[state.mountedRoot].render.non_scroll_dirty = 1;
#endif
}

#else
int RootScrollOnlyRefresh::refresh(int root, int width, int height)
{
	(void)root; (void)width; (void)height;
	return 0;
}

void rootScrollImageHoldTick() {}
#endif

}  // namespace gea::embedded::ui
