// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include "tree_state.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

// A <virtual-list> is a generic overflow:scroll container that windows a small
// pool of real child "slot" nodes (the row template, supplied by the app) over
// a virtual content height of itemCount * rowHeight. It owns NO row content:
// labels, fonts, colours, padding and the row height itself all come from the
// child slots' own CSS, exactly like any other node. The C++ side only tracks
// the total item count and reports the virtual content height so the generic
// scroll/scrollbar/render machinery can drive it; the app positions and rebinds
// the recycled slots reactively from the element's scrollTop.

namespace gea::embedded::ui {

namespace {

void dispatchScrollEvent(int node)
{
	Tree &tree = Tree::instance();
	if (!tree.hasListenersForType("scroll")) return;
	gea::framework::events::PointerEvent event{};
	event.type = gea::framework::events::PointerEventType::Scroll;
	event.targetId = node;
	event.bubbles = false;
	event.cancelable = false;
	tree.dispatchEvent(event);
}

int parseIntAttribute(const char *value, int fallback)
{
	if (!value || !value[0]) return fallback;
	char *end = nullptr;
	errno = 0;
	const long parsed = std::strtol(value, &end, 10);
	if (errno != 0 || end == value) return fallback;
	if (parsed < 0) return 0;
	if (parsed > 0x3fffffffL) return 0x3fffffff;
	return static_cast<int>(parsed);
}

bool sameName(const char *a, const char *b)
{
	if (!a || !b) return false;
	while (*a && *b) {
		char ca = *a == '-' ? '_' : *a;
		char cb = *b == '-' ? '_' : *b;
		if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
		if (ca != cb) return false;
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}

int clampScrollTop(const Node &n, int scrollTop)
{
	int maxY = n.layout.scroll_content_height - n.layout.height;
	if (maxY < 0) maxY = 0;
	if (scrollTop < 0) scrollTop = 0;
	if (scrollTop > maxY) scrollTop = maxY;
	return scrollTop;
}

bool canScrollY(int node, const Node &n)
{
	if (n.type == NodeType::VirtualList) return VirtualListRenderer::scrollMaxY(node) > 0;
	if (!isViewLikeNodeType(n.type) || !scrollsOverflowY(n.style)) return false;
	return ViewRenderer::scrollMaxY(n) > 0;
}

int nearestScrollableAncestorY(const TreeState &state, int node)
{
	for (int current = state.nodes[node].parent; current >= 0; current = state.nodes[current].parent) {
		if (canScrollY(current, state.nodes[current])) return current;
	}
	return -1;
}

bool targetContentBoundsY(const TreeState &state, int scroller, int target, int *top, int *bottom)
{
	if (!top || !bottom) return false;
	if (scroller < 0 || target < 0 || scroller >= state.nodeCount || target >= state.nodeCount) return false;

	int y = 0;
	for (int current = target; current >= 0 && current != scroller; current = state.nodes[current].parent) {
		const Node &node = state.nodes[current];
		if (!state.nodeActive[current] || node.style.display == kDisplayNone) return false;
		y += node.layout.y;

		const int parent = node.parent;
		if (parent < 0 || parent >= state.nodeCount) return false;
		if (parent != scroller && state.nodes[parent].style.overflow == 2 && scrollsOverflowY(state.nodes[parent].style)) {
			y -= state.nodes[parent].layout.scroll_y;
		}
	}

	if (target != scroller) {
		int current = target;
		while (current >= 0 && current != scroller) current = state.nodes[current].parent;
		if (current != scroller) return false;
	}

	*top = y;
	*bottom = y + state.nodes[target].layout.height;
	return true;
}

bool scrollResolvedNodeIntoView(TreeState &state, int node)
{
	const int scroller = nearestScrollableAncestorY(state, node);
	if (scroller < 0) return false;

	const Node &target = state.nodes[node];
	const Node &container = state.nodes[scroller];
	const int viewportTop = container.layout.scroll_y;
	const int viewportBottom = viewportTop + container.layout.height;
	const int targetTop = target.layout.y - container.layout.y + container.layout.scroll_y;
	const int targetBottom = targetTop + target.layout.height;

	int next = container.layout.scroll_y;
	if (targetTop < viewportTop) {
		next = targetTop;
	} else if (targetBottom > viewportBottom) {
		next = targetBottom - container.layout.height;
	} else {
		return true;
	}
	Tree::instance().setScrollTop(scroller, next);
	return true;
}

}  // namespace

void VirtualListRenderer::init(int node)
{
	if (node < 0 || node >= kMaxNodes) return;
	auto &state = treeState();
	ensureRareData(node).virtualList.itemCount = 0;
	// Behave like any overflow:scroll container so the generic scroll, clip and
	// scrollbar paths drive the list. The app's CSS may also set this; forcing
	// it here keeps a bare <virtual-list> scrollable.
	state.nodes[node].style.overflow = 2;
	state.nodes[node].style.overflow_x = 0;
	state.nodes[node].style.overflow_y = 2;
}

void VirtualListRenderer::configureAttribute(int node, const char *name, const char *value)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return;
	auto &list = ensureRareData(node).virtualList;

	if (sameName(name, "item-count") || sameName(name, "itemCount")) {
		const int next = parseIntAttribute(value, list.itemCount);
		if (next == list.itemCount) return;
		list.itemCount = next;
		Node &n = state.nodes[node];
		n.render.dirty = 1;
		n.render.layout_dirty = 1;
		n.render.non_scroll_dirty = 1;
		Tree::instance().markNodeDisplayCommandsDirty(node);
	}
}

int VirtualListRenderer::itemCount(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return 0;
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->virtualList.itemCount : 0;
}

int VirtualListRenderer::virtualContentHeight(int node, int rowHeight)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return 0;
	// Cache the measured row height (the first slot child's rendered CSS layout
	// height) so the app can read it back via the element's `rowHeight` property
	// and position/window its recycled slots from the SAME value the native
	// scroll geometry uses — one source of truth, instead of the app
	// recomputing ITEM_HEIGHT and hand-syncing it to the CSS.
	if (rowHeight > 0) ensureRareData(node).virtualList.rowHeight = static_cast<int32_t>(rowHeight);
	const NodeRareData *rd = rareDataFor(node);
	const int count = rd ? rd->virtualList.itemCount : 0;
	if (count <= 0 || rowHeight <= 0) return 0;
	const int64_t raw = static_cast<int64_t>(count) * static_cast<int64_t>(rowHeight);
	return raw > 0x3fffffffLL ? 0x3fffffff : static_cast<int>(raw);
}

int VirtualListRenderer::rowHeight(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return 0;
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->virtualList.rowHeight : 0;
}

int VirtualListRenderer::scrollTop(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return 0;
	return state.nodes[node].layout.scroll_y;
}

int VirtualListRenderer::previousScrollTop(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return 0;
	return state.nodes[node].layout.previous_scroll_y;
}

int VirtualListRenderer::scrollMaxY(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return 0;
	const Node &n = state.nodes[node];
	const int maxY = n.layout.scroll_content_height - n.layout.height;
	return maxY > 0 ? maxY : 0;
}

bool VirtualListRenderer::setScrollTop(int node, int scrollTop)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return false;
	Node &n = state.nodes[node];
	const int clamped = clampScrollTop(n, scrollTop);
	if (clamped == n.layout.scroll_y) return false;
	n.layout.scroll_y = static_cast<int32_t>(clamped);
	n.render.dirty = 1;
	n.render.layout_dirty = 1;
	Tree::instance().markScrollDirty(node);
	dispatchScrollEvent(node);
	return true;
}

void VirtualListRenderer::captureSnapshot(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || state.nodes[node].type != NodeType::VirtualList) return;
	Node &n = state.nodes[node];
	n.layout.previous_scroll_y = n.layout.scroll_y;
}

int Tree::scrollTop(int node) const
{
	if (node < 0 || node >= nodeCount()) return 0;
	return treeState().nodes[node].layout.scroll_y;
}

int Tree::scrollLeft(int node) const
{
	if (node < 0 || node >= nodeCount()) return 0;
	return treeState().nodes[node].layout.scroll_x;
}

void Tree::setScrollLeft(int node, int scrollLeft)
{
	if (node < 0 || node >= nodeCount()) return;
	Node &n = treeState().nodes[node];
	if (!isViewLikeNodeType(n.type) || !scrollsOverflowX(n.style) || n.type == NodeType::VirtualList) return;
	const int maxX = ViewRenderer::scrollMaxX(n);
	if (scrollLeft < 0) scrollLeft = 0;
	if (scrollLeft > maxX) scrollLeft = maxX;
	if (scrollLeft == n.layout.scroll_x) return;
	n.layout.scroll_x = static_cast<int32_t>(scrollLeft);
	n.render.dirty = 1;
	n.render.layout_dirty = 1;
	n.render.non_scroll_dirty = 1;
	markScrollDirty(node);
}

void Tree::setScrollTop(int node, int scrollTop)
{
	if (node < 0 || node >= nodeCount()) return;
	Node &n = treeState().nodes[node];
	if (n.type == NodeType::VirtualList) {
		VirtualListRenderer::setScrollTop(node, scrollTop);
		return;
	}
	if (!isViewLikeNodeType(n.type) || !scrollsOverflowY(n.style)) return;
	const int maxY = ViewRenderer::scrollMaxY(n);
	if (scrollTop < 0) scrollTop = 0;
	if (scrollTop > maxY) scrollTop = maxY;
	if (scrollTop == n.layout.scroll_y) return;
	n.layout.scroll_y = static_cast<int32_t>(scrollTop);
	n.render.dirty = 1;
	n.render.layout_dirty = 1;
	markScrollDirty(node);
}

void Tree::scrollIntoView(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || !state.nodeActive[node]) return;
	state.pendingScrollIntoViewNode = node;
	scrollResolvedNodeIntoView(state, node);
	markDisplayListContentDirty();
}

void Tree::applyPendingScrollIntoView()
{
	auto &state = treeState();
	const int node = state.pendingScrollIntoViewNode;
	if (node < 0) return;
	if (node >= state.nodeCount || !state.nodeActive[node]) {
		state.pendingScrollIntoViewNode = -1;
		return;
	}
	const int scroller = nearestScrollableAncestorY(state, node);
	if (scroller < 0) return;

	const Node &container = state.nodes[scroller];
	const int viewportTop = container.layout.scroll_y;
	const int viewportBottom = viewportTop + container.layout.height;
	int targetTop = 0;
	int targetBottom = 0;
	if (!targetContentBoundsY(state, scroller, node, &targetTop, &targetBottom)) {
		state.pendingScrollIntoViewNode = -1;
		return;
	}

	int next = container.layout.scroll_y;
	if (targetTop < viewportTop) {
		next = targetTop;
	} else if (targetBottom > viewportBottom) {
		next = targetBottom - container.layout.height;
	} else {
		state.pendingScrollIntoViewNode = -1;
		return;
	}
	state.pendingScrollIntoViewNode = -1;
	setScrollTop(scroller, next);
}

}  // namespace gea::embedded::ui
