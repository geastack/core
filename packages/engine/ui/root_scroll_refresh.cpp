// SPDX-License-Identifier: Apache-2.0
#include "root_scroll_refresh.h"

#include "display.h"
#include "internal.h"
#include "layout_snapshot.h"
#include "refresh_perf.h"
#include "tree_state.h"
#include "viewport_region.h"

#include <algorithm>
#include <cstdlib>

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

namespace gea::embedded::ui {

namespace {

bool hasTransformState(const Node &node)
{
	return hasIndividualLinearTransform(rstyle(node.style)) || hadIndividualLinearTransform(node.render) ||
	       rstyle(node.style).transform_rotate != 0 ||
	       node.render.previous_transform_rotate != 0 ||
	       rstyle(node.style).transform_rotate_x != 0 ||
	       node.render.previous_transform_rotate_x != 0 ||
	       rstyle(node.style).transform_rotate_y != 0 ||
	       node.render.previous_transform_rotate_y != 0 ||
	       composedTranslateX(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_x != 0 ||
	       composedTranslateY(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_y != 0 ||
	       composedTranslateZ(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_z != 0 ||
	       composedTranslateXPercent(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_x_percent != 0 ||
	       composedTranslateYPercent(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_y_percent != 0 ||
	       rstyle(node.style).transform_scale_x != 1000 ||
	       node.render.previous_transform_scale_x != 1000 ||
	       rstyle(node.style).transform_scale_y != 1000 ||
	       rstyle(node.style).transform_scale_z != 1000 ||
	       node.render.previous_transform_scale_y != 1000 ||
	       node.render.previous_transform_scale_z != 1000 ||
	       rstyle(node.style).perspective > 0 ||
	       node.render.previous_perspective > 0;
}

constexpr int kMaxExtraDirtyNodes = 8;
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
		if (state.nodes[cursor].style.mask_right_fade_width > 0) return true;
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
	const auto parentPosition = state.nodes[scrollNode].style.position;
	if (parentPosition != 1 && parentPosition != 2) return false;
	if (n.style.position != 1) return false;
	// Anchored via top/bottom (the windowed axis); refresh() recomputes y from
	// these offsets directly.
	if (n.style.pos_offsets[0] == kUnset && n.style.pos_offset_percent[0] == kUnset &&
	    n.style.pos_offsets[2] == kUnset && n.style.pos_offset_percent[2] == kUnset)
		return false;
	if (hasTransformState(n)) return false;
	// The reconcile translates the slot box; a pending size change needs layout.
	if (n.layout.width != n.layout.previous_width || n.layout.height != n.layout.previous_height)
		return false;
	return true;
}

int resolvedSlotOffset(const Node &slot, int side, int basis)
{
	int offset = slot.style.pos_offsets[side] != kUnset ? slot.style.pos_offsets[side] : 0;
	const int percent = slot.style.pos_offset_percent[side];
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
	if (usesRowLayout(p.style)) return true;
	if (p.style.flex_direction_explicit || p.style.display != kDisplayBlock) return false;
	bool anyInFlow = false;
	for (int c = p.first_child; c >= 0; c = state.nodes[c].next_sibling) {
		const Node &child = state.nodes[c];
		if (child.style.display == kDisplayNone || isOutOfFlowPosition(child.style.position)) continue;
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
	if (p.style.flex_wrap || p.style.justify_content != 0) return;
	const bool row = flowsChildrenAsRow(state, p);
	const int delta = row ? n.layout.width - oldWidth : n.layout.height - oldHeight;
	if (delta == 0) return;
	int totalFlex = 0;
	for (int c = p.first_child; c >= 0; c = state.nodes[c].next_sibling) {
		const Node &child = state.nodes[c];
		if (child.style.display == kDisplayNone || isOutOfFlowPosition(child.style.position)) continue;
		totalFlex += child.style.flex;
	}
	if (totalFlex > 0) return;
	for (int c = n.next_sibling; c >= 0; c = state.nodes[c].next_sibling) {
		Node &sib = state.nodes[c];
		if (sib.style.display == kDisplayNone || isOutOfFlowPosition(sib.style.position)) continue;
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
		const int align = n.style.align_self >= 0 ? n.style.align_self : p.style.align_items;
		const bool stretched = align == 0 && n.style.width == kUnset && n.style.width_percent == kUnset;
		if (!TextRenderer::remeasureContentBox(id, stretched)) return;
		if (siblingsCoveredByReplay) shiftSiblingsForRemeasuredText(state, id, oldWidth, oldHeight);
		return;
	}
	if (!TextRenderer::remeasureContentBox(id)) return;
	const int oldRight = n.layout.x + oldWidth + n.style.margin[1];
	const int parentRight = p.layout.x + p.layout.width - p.style.padding[1];
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
	for (int i = 0; i < state.nodeCount; i++) {
		Node *n = &state.nodes[i];
		if (!n->render.dirty) continue;

		if (n->render.scroll_dirty && !n->render.non_scroll_dirty) {
			if (foundNode >= 0) return -1;
			if (!isViewLikeNodeType(n->type) || n->style.display == 1) return -1;
			// Accept a pure scroll on exactly one axis the node overflows. The
			// horizontal case mirrors the vertical fast path (see refresh()): a
			// virtual-list is vertical-only. Rejecting a both-axes change keeps the
			// single-axis scrollRect blit unambiguous.
			const bool changedY = n->layout.scroll_y != n->layout.previous_scroll_y;
			const bool changedX = n->layout.scroll_x != n->layout.previous_scroll_x;
			const bool canScrollYAxis = n->type == NodeType::VirtualList || scrollsOverflowY(n->style);
			const bool canScrollXAxis = n->type != NodeType::VirtualList && scrollsOverflowX(n->style);
			const bool vertical = changedY && !changedX && canScrollYAxis;
			const bool horizontal = changedX && !changedY && canScrollXAxis;
			if (!vertical && !horizontal) return -1;
			if (!layoutUnchanged(*n)) return -1;
			if (hasTransformState(*n)) return -1;
			if (n->style.opacity != 255 || hasAnyBorder(n->style)) return -1;
			for (int r = 0; r < 4; r++) {
				if (n->style.border_radius[r] != 0 || n->style.border_radius_percent[r] != kUnset) return -1;
			}
			foundNode = i;
			continue;
		}

		if (n->render.scroll_dirty) return -1;
	}
	if (foundNode < 0) return -1;

	// Overlay guard: scrollRect shifts EVERY framebuffer pixel in the scroll
	// viewport, so it is only valid when those pixels belong to the scroll
	// subtree. A node outside the subtree that overlaps the viewport would be
	// dragged with the content. Right-fade masks already disable scrollRect and
	// re-rasterize the complete viewport from the rebuilt root display list; in
	// that branch overlapping siblings are composited afresh, so rejecting them
	// would unnecessarily force a full-tree refresh (the weather city rail has
	// exactly this shape because its negative margin overlaps the hero).
	if (!hasRightFadeMask(state, foundNode)) {
		const Node &sn = state.nodes[foundNode];
		const int svx0 = sn.layout.x, svy0 = sn.layout.y;
		const int svx1 = sn.layout.x + sn.layout.width - 1;
		const int svy1 = sn.layout.y + sn.layout.height - 1;
		for (int i = 0; i < state.nodeCount; i++) {
			if (i == foundNode) continue;
			const Node &o = state.nodes[i];
			if (o.style.display == 1 || o.style.opacity == 0) continue;
			if (o.layout.width <= 0 || o.layout.height <= 0) continue;
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
				if (state.nodes[a].style.display == 1) { hidden = true; break; }
			}
			if (related || hidden) continue;
			for (int a = foundNode; a >= 0; a = state.nodes[a].parent) {
				if (a == i) { related = true; break; }
			}
			if (related) continue;
			if (o.layout.x > svx1 || o.layout.x + o.layout.width - 1 < svx0 ||
			    o.layout.y > svy1 || o.layout.y + o.layout.height - 1 < svy0)
				continue;
			return -1;
		}
	}

	// Pass 2: classify every other dirty node. Recycled slot children of a
	// scrolling <virtual-list> get repositioned natively by refresh(); their
	// dirty descendants (rebound label text, recolored row content) are covered
	// by the slot's old/new-rect replays over the rebuilt display list, so they
	// are skipped here — including ones whose pre-measured text box changed,
	// which would otherwise reject the whole frame back to the full-relayout
	// path on every fling tick.
	for (int i = 0; i < state.nodeCount; i++) {
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
		    ((n->style.height != kUnset && n->style.height != n->layout.height) ||
		     (n->style.width != kUnset && n->style.width != n->layout.width))) {
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
	perf.rootScrollCalls++;
	DirtyNode extraDirtyNodes[kMaxExtraDirtyNodes];
	int extraDirtyCount = 0;
	int slotNodes[kMaxSlotRepositionNodes];
	int slotCount = 0;
	const std::int64_t scanStartUs = refreshPerfNowUs();
	int scroll_node = refreshNode(root, extraDirtyNodes, &extraDirtyCount, slotNodes, &slotCount, width, height);
	perf.rootScrollScanUs += refreshPerfNowUs() - scanStartUs;
	if (scroll_node < 0) {
		perf.rootScrollRejected++;
		return 0;
	}
	perf.rootScrollAccepted++;
	perf.rootScrollExtraDirtyNodes += extraDirtyCount;

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
		if (slot.style.display == 1) continue;  // display:none — erase old rect only

		int newX = slot.layout.x;
		if (slot.style.pos_offsets[3] != kUnset || slot.style.pos_offset_percent[3] != kUnset)
			newX = n->layout.x + resolvedSlotOffset(slot, 3, n->layout.width);
		else if (slot.style.pos_offsets[1] != kUnset || slot.style.pos_offset_percent[1] != kUnset)
			newX = n->layout.x + n->layout.width - slot.layout.width -
			       resolvedSlotOffset(slot, 1, n->layout.width);

		int newY = slot.layout.y;
		if (slot.style.pos_offsets[0] != kUnset || slot.style.pos_offset_percent[0] != kUnset)
			newY = n->layout.y - n->layout.scroll_y + resolvedSlotOffset(slot, 0, n->layout.height);
		else if (slot.style.pos_offsets[2] != kUnset || slot.style.pos_offset_percent[2] != kUnset)
			newY = n->layout.y - n->layout.scroll_y + n->layout.height - slot.layout.height -
			       resolvedSlotOffset(slot, 2, n->layout.height);

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

	int vx = n->layout.x;
	int vy = n->layout.y;
	int vw = n->layout.width;
	int vh = n->layout.height;
	if (!ViewportRegion::clipRect(width, height, &vx, &vy, &vw, &vh)) {
		const std::int64_t snapshotStartUs = refreshPerfNowUs();
		LayoutSnapshot::capture();
		perf.rootScrollSnapshotUs += refreshPerfNowUs() - snapshotStartUs;
		return 1;
	}

	// The scroll replay needs a display list whose commands match the
	// post-shift layout, and recordNode drops commands for nodes outside the
	// record clip — so off-viewport content has no commands until it scrolls
	// in. Historically that forced a full clear()+recordNode() every scroll
	// frame (~2ms). The translate-only path below skips it, mirroring the
	// horizontal-pan fast path: record ONCE with the record clip expanded
	// vertically by kScrollRecordPadding around the viewport (coverage), then
	// per scroll frame just translate the scroll subtree's recorded commands
	// in place (they follow shiftDescendantLayoutY), un-translate the scroll
	// node's own viewport-fixed box, and patch the scrollbar thumb. Coverage
	// is re-recorded when the accumulated scroll leaves the padded window or
	// any other record replaced the list (recordSerial). Frames that change
	// content (extra dirty nodes, virtual-list slot rebinds, horizontal
	// scrolls) keep the plain full rebuild — a translate can't express them.
	// (An earlier hybrid that recorded/cleared subtrees incrementally failed
	// because DisplayList::replay() walks state.commands[] linearly and
	// orphaned ranges kept replaying; wholesale translate avoids that
	// entirely: no command is added or removed.)
	{
		static std::uint32_t coverageSerial = 0;
		static int coverageRoot = -1;
		static int coverageNode = -1;
		static int coverageScrollY = 0;
		static int coveragePadTop = 0;
		static int coveragePadBottom = 0;
		// Window fallback for content too tall to pre-record in one pass
		// (command coords are int16, and the command buffer is finite).
		constexpr int kScrollRecordFallbackPadding = 512;
		constexpr int kMaxFullContentHeight = 16000;

		auto &list = DisplayList::instance();
		const int scrollYNow = n->layout.scroll_y;
		const bool translateEligible = GEA_EMBEDDED_ROOT_SCROLL_TRANSLATE != 0 &&
		                               !horizontal &&
		                               n->type != NodeType::VirtualList &&
		                               slotCount == 0 &&
		                               extraDirtyCount == 0;
		const int coverageDelta = scrollYNow - coverageScrollY;
		const bool coverageValid = translateEligible &&
		                           coverageRoot == root &&
		                           coverageNode == scroll_node &&
		                           coverageSerial == list.recordSerial() &&
		                           coverageDelta >= -coveragePadTop &&
		                           coverageDelta <= coveragePadBottom;

		const std::int64_t rebuildStartUs = refreshPerfNowUs();
		bool listReady = false;
		if (coverageValid) {
			list.translateSubtreeCommands(scroll_node, 0, move);
			// translateSubtreeCommands includes the scroll node's own box
			// commands (background/border), which are viewport-fixed — undo.
			list.translateNodeCommands(scroll_node, 0, -move);
			listReady = list.patchScrollbarThumb(scroll_node);
		}
		if (!listReady && translateEligible) {
			// Pre-record the WHOLE scroll content when it fits: one record,
			// replayed (translated) forever — scrolling never re-records. For
			// very tall content, fall back to a padded window around the
			// viewport; a truncated record (command-buffer overflow) also
			// falls back so coverage is never silently incomplete.
			int padTop = scrollYNow;
			int padBottom = n->layout.scroll_content_height - scrollYNow - n->layout.height;
			if (padBottom < 0) padBottom = 0;
			if (n->layout.scroll_content_height > kMaxFullContentHeight) {
				padTop = padTop < kScrollRecordFallbackPadding ? padTop : kScrollRecordFallbackPadding;
				padBottom = padBottom < kScrollRecordFallbackPadding ? padBottom : kScrollRecordFallbackPadding;
			}
			for (int attempt = 0; attempt < 2 && !listReady; attempt++) {
				list.clear();
				list.recordNodeWithExpandedClip(root,
				                                255,
				                                scroll_node,
				                                vx,
				                                vy - padTop,
				                                vx + vw - 1,
				                                vy + vh - 1 + padBottom);
				list.weldTransformedFaces();
				if (!list.commandOverflow()) {
					listReady = true;
					coverageSerial = list.recordSerial();
					coverageRoot = root;
					coverageNode = scroll_node;
					coverageScrollY = scrollYNow;
					coveragePadTop = padTop;
					coveragePadBottom = padBottom;
					break;
				}
				// Overflowed: retry once with the small window.
				padTop = padTop < kScrollRecordFallbackPadding ? padTop : kScrollRecordFallbackPadding;
				padBottom = padBottom < kScrollRecordFallbackPadding ? padBottom : kScrollRecordFallbackPadding;
			}
			if (listReady) state.displayListDirty = false;
		}
		if (!listReady) {
			list.clear();
			list.recordNode(root, 255);
			state.displayListDirty = false;
			coverageRoot = -1;
		}
		perf.rootScrollRebuildUs += refreshPerfNowUs() - rebuildStartUs;
	}
	perf.rootScrollMovePx += abs_move;
	perf.rootScrollViewportPx += vw * vh;
	perf.rootScrollStripPx += horizontal ? (vh * (abs_move < vw ? abs_move : vw))
	                                     : (vw * (abs_move < vh ? abs_move : vh));

	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::setAlpha(255);
	constexpr bool kSelfFlushingScroll = GEA_EMBEDDED_DISPLAY_SCROLLRECT_SELF_FLUSHING != 0;
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
		ViewportRegion::replayDisplayRegion(rx, ry, rw, rh);
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

	const int viewportExtent = horizontal ? vw : vh;
	if (!contentHasRightFadeMask && abs_move > 0 && abs_move < viewportExtent) {
		const std::int64_t scrollRectStartUs = refreshPerfNowUs();
		gea::platform::display::Display::scrollRect(vx, vy, vw, vh, horizontal ? move : 0, horizontal ? 0 : move);
		perf.rootScrollScrollRectUs += refreshPerfNowUs() - scrollRectStartUs;

		const std::int64_t stripReplayStartUs = refreshPerfNowUs();
		if (horizontal) {
			// Repaint the newly-revealed vertical edge strip.
			if (move < 0) replayScrollNodeRegion(vx + vw - abs_move, vy, abs_move, vh);
			else replayScrollNodeRegion(vx, vy, abs_move, vh);
		} else if (move < 0) {
			replayScrollNodeRegion(vx, vy + vh - abs_move, vw, abs_move);
		} else {
			replayScrollNodeRegion(vx, vy, vw, abs_move);
		}
		perf.rootScrollStripReplayUs += refreshPerfNowUs() - stripReplayStartUs;

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
					const int damageY = shiftedOldY < newThumbY ? shiftedOldY : newThumbY;
					const int damageBottom = shiftedOldBottom > newThumbBottom ? shiftedOldBottom : newThumbBottom;
					replayScrollbarDamage(damageY, damageBottom - damageY + 1);
				} else {
					replayScrollbarDamage(shiftedOldY, oldThumbH);
					replayScrollbarDamage(newThumbY, newThumbH);
				}
			} else {
				replayScrollbarDamage(n->layout.y, n->layout.height);
			}
			perf.rootScrollScrollbarReplayUs += refreshPerfNowUs() - scrollbarReplayStartUs;
		}
	} else {
		const std::int64_t fullReplayStartUs = refreshPerfNowUs();
		replayScrollNodeRegion(vx, vy, vw, vh);
		perf.rootScrollFullReplayUs += refreshPerfNowUs() - fullReplayStartUs;
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
	perf.rootScrollStripReplayUs += refreshPerfNowUs() - slotReplayStartUs;

	const std::int64_t extraReplayStartUs = refreshPerfNowUs();
	for (int i = 0; i < extraDirtyCount; i++) {
		DirtyNode dirty = extraDirtyNodes[i];
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
		gea::platform::display::Display::resetClip();
		gea::platform::display::Display::setAlpha(255);
		gea::platform::display::Display::pushClip(dirty.x0, dirty.y0, dirty.x1 - dirty.x0 + 1, dirty.y1 - dirty.y0 + 1);
		noteDamage(dirty.x0, dirty.y0, dirty.x1 - dirty.x0 + 1, dirty.y1 - dirty.y0 + 1);
		DisplayList::instance().replay();
		gea::platform::display::Display::popClip();
	}
	perf.rootScrollExtraReplayUs += refreshPerfNowUs() - extraReplayStartUs;

	const std::int64_t flushStartUs = refreshPerfNowUs();
	if (kSelfFlushingScroll && !damageOverflow) {
		if (damageCount > 0)
			gea::platform::display::Display::flushRects(damageRects, damageCount);
	} else {
		gea::platform::display::Display::flush();
	}
	perf.rootScrollFlushUs += refreshPerfNowUs() - flushStartUs;

	const std::int64_t snapshotStartUs = refreshPerfNowUs();
	LayoutSnapshot::capture();
	perf.rootScrollSnapshotUs += refreshPerfNowUs() - snapshotStartUs;

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
	state.nodes[state.mountedRoot].render.non_scroll_dirty = 1;
}

}  // namespace gea::embedded::ui
