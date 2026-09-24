// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "display.h"
#include "host/display_orientation.h"
#include "mirror.h"
#include "node_lifecycle.h"
#include "tree_state.h"
#include "virtual_keyboard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gea::embedded::ui {

namespace {

bool nodeCanDrawWithoutCanvas(const Node &node)
{
	if (node.style.display == 1) return false;
	if (node.type == NodeType::Canvas) return false;
	// A <camera> leaf paints itself from the platform preview buffer (or a
	// native overlay), never via the box renderer — treat it like a canvas.
	if (node.type == NodeType::Camera) return false;
	if (node.style.has_bg || hasAnyBorder(node.style)) return true;
	if (node.type == NodeType::Text && !node.text.empty()) return true;
	if (node.type == NodeType::Image && node.image_id >= 0) return true;
	if (node.type == NodeType::VirtualList) return true;
	if (isViewLikeNodeType(node.type) && node.style.overflow == 2) return true;
	return false;
}

// Diagnostics for GEADEV: why/how often the display-backed gate fails.
int gDisplayBackedFailReason = 0;
int gDisplayBackedFailExtra = 0;
int gDisplayBackedFailCount = 0;
int gCanvasRebindCount = 0;
int gCanvasLastMode = -1;

bool canUseDisplayFramebuffer(TreeState &state, int id, int width, int height)
{
#ifdef __EMSCRIPTEN__
	// Web/WASM: the display "panel" IS the framebuffer the document render paints
	// into (the browser scans it out after every frame; there is no separate GRAM).
	// A display-backed canvas presents during the rAF phase, but the per-frame
	// document render (Application::frame -> Document::frame, which runs AFTER the
	// rAF callbacks) repaints that same framebuffer from the retained display list,
	// clobbering the canvas's fresh draws with stale content — the canvas freezes
	// at its first frame and only a thin mis-located strip survives. On real panels
	// present() pushes the canvas commands to the panel over its own transport, so
	// it never collides with the framebuffer; on web the fast path's invariant
	// ("nothing else repaints my region") is violated. Use the owned-surface +
	// document-blit path on web, which composites the canvas's current pixels every
	// frame and animates correctly. (Tree-less GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
	// builds don't come through here — with no document render there's no
	// collision, and web Display::present() rasterizes their batches directly
	// into the framebuffer.)
	(void)state;
	(void)id;
	(void)width;
	(void)height;
	return false;
#else
	if (state.mountedRoot < 0) return false;

	static int dbgFalse = 0;
	auto failWhy = [&](int reason, int extra) {
		dbgFalse++;
		gDisplayBackedFailReason = reason;
		gDisplayBackedFailExtra = extra;
		gDisplayBackedFailCount = dbgFalse;
		return false;
	};
	const int displayWidth = gea::framework::display::detail::DisplayOrientationState::width();
	const int displayHeight = gea::framework::display::detail::DisplayOrientationState::height();
	if (displayWidth <= 0 || displayHeight <= 0) return failWhy(1, 0);
	if (width != displayWidth || height != displayHeight) return failWhy(2, width);

	Node &canvasNode = state.nodes[id];
	if (canvasNode.layout.x != 0 || canvasNode.layout.y != 0) return failWhy(3, canvasNode.layout.x);
	if (canvasNode.layout.width != displayWidth ||
	    canvasNode.layout.height != displayHeight)
		return failWhy(4, canvasNode.layout.width);

	int visibleCanvasCount = 0;
	for (int i = 0; i < state.nodeCount; i++) {
		Node &node = state.nodes[i];
		if (node.style.display == 1) continue;
		if (node.type == NodeType::Canvas) visibleCanvasCount++;
		if (i != id && nodeCanDrawWithoutCanvas(node)) return failWhy(5, i);
	}

	if (visibleCanvasCount != 1) return failWhy(6, visibleCanvasCount);
	return true;
#endif
}

void resetNodeSlot(TreeState &state, int id)
{
	if (id < 0 || id >= kMaxNodes) return;
	// The node's text buffer dies with the slot; retained display commands hold
	// a raw pointer into it (see DisplayList::scrubNodeText) — neutralize them
	// before NodeLifecycle::init frees the string, or a stale-list replay in the
	// gap before the next record reads freed memory.
	if (!state.nodes[id].text.empty()) DisplayList::instance().scrubNodeText(state.nodes[id].text.c_str());
	releaseRareData(id);  // frees the rare-data block: attributes, listeners, custom props, virtual-list
	releaseRareStyle(state.nodes[id].style.rare_style);  // frees the cold-style block
	state.classLists[id].clear();
	state.canvases.remove(id);
	state.nodeCommandDirty[id] = 0;
	state.nodeCommandDirtyBoundsValid[id] = 0;
	state.nodeCommandDirtyCanOverpaint[id] = 0;
	state.nodeBackdropCooldown[id] = 0;
	state.nodeInBackdrop[id] = 0;
	state.nodeActive[id] = 0;
	NodeLifecycle::init(&state.nodes[id], NodeType::View);
	state.nodes[id].style.display = kDisplayNone;
}

void trimInactiveTail(TreeState &state)
{
	while (state.nodeCount > 0 && !state.nodeActive[state.nodeCount - 1]) {
		state.nodeCount--;
	}
}

bool localAbsoluteLeafTreeMutation(const TreeState &state, int child)
{
	if (child < 0 || child >= state.nodeCount) return false;
	const Node &node = state.nodes[child];
	if (node.style.display == kDisplayNone) return false;
	if (node.style.position != 1) return false;
	if (node.style.width == kUnset || node.style.height == kUnset) return false;
	if (node.first_child >= 0) return false;
	if (node.type == NodeType::Text || node.type == NodeType::Canvas || node.type == NodeType::VirtualList) return false;
	if (!isViewLikeNodeType(node.type) && node.type != NodeType::Image) return false;
	if (rstyle(node.style).filter_blur_radius > 0 || node.style.mask_right_fade_width > 0) return false;
	if (rstyle(node.style).box_shadow_alpha > 0) return false;
	return true;
}

}  // namespace

void Tree::clear()
{
	auto &state = treeState();
	state.refreshSerial++;
	if (state.refreshSerial == 0) state.refreshSerial = 1;
	for (int i = 0; i < state.nodeCount; i++) {
		releaseRareData(i);  // frees the rare-data block: attributes, listeners, custom props, styles, virtual-list
		state.classLists[i].clear();
	}
	// Document-level handlers live off-tree; drop them so the next app doesn't
	// inherit the previous app's dial/hardware-button/pointer handlers.
	// Delegated document pointer/touch handlers also bind onto the mounted root;
	// releaseRareData above drops those per-node listener slots, so the binding
	// cache must be reset with them.
	resetDocumentEventListeners();
	resetDocumentDelegatedEventListeners();
	resetRareStylePool();  // drop all cold-style entries; node handles reset by the Node{} loop below
	state.canvases.clear();
	std::memset(state.nodeActive, 0, sizeof(state.nodeActive));
	// Reset EVERY slot of state.nodes[], not just [0, nodeCount). If the
	// next app's tree has fewer nodes than the previous app's tree, the
	// leftover slots still carry stale data (transform_rotate from a
	// rotated clock hand, previous_x/y layout, blink_visible, etc.)
	// that refresh / display-list / hit-test paths assume is a clean
	// baseline. std::memset CAN'T be used here: Node contains a
	// `std::string text` member, and zeroing its internal pointer leaks
	// any heap-allocated buffer the previous app left behind AND leaves
	// the string in a state where the next operation null-derefs.
	// Per-slot assignment from a default-constructed Node invokes
	// std::string's operator= which deallocates the LHS's heap if any
	// and resets to an empty SSO state, while still zeroing all the
	// POD fields. Slightly slower than memset but runs once per app
	// switch and bounded at kMaxNodes (~512).
	for (int i = 0; i < kMaxNodes; i++) state.nodes[i] = Node{};
	state.nodeCount = 0;
	state.fixedPositionUsed = false;
	state.mountedRoot = -1;
	state.mountedWidth = 0;
	state.mountedHeight = 0;
	state.displayListDirty = true;
	std::memset(state.nodeCommandDirty, 0, sizeof(state.nodeCommandDirty));
	std::memset(state.nodeCommandDirtyBoundsValid, 0, sizeof(state.nodeCommandDirtyBoundsValid));
	std::memset(state.nodeCommandDirtyCanOverpaint, 0, sizeof(state.nodeCommandDirtyCanOverpaint));
	std::memset(state.scrollDirtyNodes, 0, sizeof(state.scrollDirtyNodes));
	state.scrollDirtyAny = false;
	state.pendingScrollIntoViewNode = -1;
	// Drop display-list commands and growable render scratch from the previous app.
	// Per-frame DisplayList::clear() keeps capacity for reuse; app/tree reset is the
	// point where a large app should stop poisoning a smaller app's memory shape.
	DisplayList::instance().resetStorage();
	Mirror::instance().clearScrollDirty();
	// Drop any active software-scroll-register state so the new app's
	// drawing doesn't get rotated through the previous app's offset. On
	// platforms without the register this is a no-op.
	gea::platform::display::Display::resetScrollRegion();
	// Drop touch / momentum / scroll-tracker state too. Without this the
	// next app's first animation-frame tick can drive a fling against a
	// stale node id from the previous tree — which scrolls (or marks
	// dirty) nodes in the new app that have no business being touched,
	// erasing whatever the synchronous init paint just drew.
	Tree::instance().resetInput();
	// Same for the on-screen keyboard — it holds node ids it mounted as
	// children of the previous app's root, and those slots are about to
	// be reused. Dropping our handles forces re-mount on the next focus
	// event in the new app.
	VirtualKeyboard::instance().reset();
}

int Tree::createNode(NodeType type)
{
	auto &state = treeState();
	int id = -1;
	for (int i = 0; i < state.nodeCount; i++) {
		if (state.nodeActive[i]) continue;
		id = i;
		break;
	}
	if (id < 0) {
		if (state.nodeCount >= kMaxNodes) return -1;
		id = state.nodeCount++;
	}
	state.nodeActive[id] = 1;
	releaseRareData(id);  // free any block from a prior occupant before reinit
	NodeLifecycle::init(&state.nodes[id], type);
	state.classLists[id].clear();
	if (type == NodeType::VirtualList) VirtualListRenderer::init(id);
	return id;
}

int Tree::cloneNode(int sourceId, bool deep)
{
	auto &state = treeState();
	if (sourceId < 0 || sourceId >= state.nodeCount || !state.nodeActive[sourceId]) return -1;
	const int id = createNode(state.nodes[sourceId].type);
	if (id < 0) return -1;
	// nodes[] is a fixed array, so taking the source reference after createNode
	// is safe; an ACTIVE source slot can never be the one createNode reused.
	const Node &source = state.nodes[sourceId];
	Node &clone = state.nodes[id];
	clone.style = source.style;
	// clone.style copied source's rare_style HANDLE (shared pool entry). Give the
	// clone its own entry with the same contents, so mutating one never aliases.
	clone.style.rare_style = -1;
	if (source.style.rare_style >= 0) rstyleMut(clone.style) = rstyle(source.style);
	clone.text = source.text;
	clone.image_id = source.image_id;
	clone.tag_id = source.tag_id;  // interned tag id — just copy the handle
	// Copy attributes + custom properties (DOM cloneNode semantics) but NOT
	// listeners or virtual-list state. All live in the rare-data block; allocate
	// one for the clone only if the source has any. (rareDataFor returns a deque
	// element whose address is stable across the ensureRareData growth below, so
	// srcRd stays valid.)
	if (const NodeRareData *srcRd = rareDataFor(sourceId)) {
		NodeRareData &cloneRd = ensureRareData(id);
		cloneRd.attributes = srcRd->attributes;
		cloneRd.customProperties = srcRd->customProperties;
		cloneRd.defaultStyles = srcRd->defaultStyles;
		cloneRd.inlineStyles = srcRd->inlineStyles;
		cloneRd.inlineCustomProperties = srcRd->inlineCustomProperties;
		cloneRd.inlineGridTemplates[0] = srcRd->inlineGridTemplates[0];
		cloneRd.inlineGridTemplates[1] = srcRd->inlineGridTemplates[1];
		cloneRd.inlineGridShorthandMask = srcRd->inlineGridShorthandMask;
	}
	state.classLists[id] = state.classLists[sourceId];
	// Listeners deliberately NOT copied (DOM cloneNode semantics) — compiled
	// templates attach handlers to the clone after cloning.
	if (deep) {
		for (int child = source.first_child; child >= 0; child = state.nodes[child].next_sibling) {
			const int childClone = cloneNode(child, true);
			if (childClone >= 0) setParent(childClone, id);
		}
	}
	return id;
}

int Tree::firstChildOf(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || !state.nodeActive[node]) return -1;
	return state.nodes[node].first_child;
}

int Tree::childAt(int node, int index)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || !state.nodeActive[node] || index < 0) return -1;
	int child = state.nodes[node].first_child;
	while (child >= 0 && index > 0) {
		child = state.nodes[child].next_sibling;
		index--;
	}
	return child;
}

int Tree::createView() { return createNode(NodeType::View); }
int Tree::createButton() { return createNode(NodeType::Button); }
int Tree::createText() { return createNode(NodeType::Text); }
int Tree::createImage() { return createNode(NodeType::Image); }
int Tree::createCanvas() { return createNode(NodeType::Canvas); }
int Tree::createCamera() { return createNode(NodeType::Camera); }
int Tree::createAudio()
{
	const int id = createNode(NodeType::Audio);
	if (id >= 0) treeState().nodes[id].style.display = kDisplayNone;
	return id;
}
int Tree::createVirtualList() { return createNode(NodeType::VirtualList); }

gea::framework::graphics::Canvas *Tree::ensureCanvas(int id, int width, int height)
{
	auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return nullptr;
	Node &node = state.nodes[id];
	if (node.type != NodeType::Canvas) return nullptr;

	if (width <= 0) width = node.layout.width;
	if (height <= 0) height = node.layout.height;
	if (width <= 0) width = std::atoi(getAttribute(id, "width"));
	if (height <= 0) height = std::atoi(getAttribute(id, "height"));
	if (width <= 0 || height <= 0) return nullptr;

	auto *slot = state.canvases.ensure(id);
	if (!slot) return nullptr;
	const auto previousMode = slot->mode;
	const auto *previousCanvas = slot->canvas();
	const int previousWidth = slot->width;
	const int previousHeight = slot->height;

	gea::framework::graphics::Canvas *canvas = nullptr;
	if (canUseDisplayFramebuffer(state, id, width, height)) {
		auto *displayCanvas = gea::platform::display::Display::canvas();
		canvas = displayCanvas ? state.canvases.bindDisplayFramebuffer(*slot, *displayCanvas) : nullptr;
	} else {
		canvas = state.canvases.bindOwned(*slot, width, height);
	}
	gCanvasLastMode = static_cast<int>(slot->mode);

	if (canvas &&
	    (previousMode != slot->mode ||
	     previousCanvas != slot->canvas() ||
	     previousWidth != slot->width ||
	     previousHeight != slot->height)) {
		gCanvasRebindCount++;
		markNodeDisplayCommandsDirty(id);
	}

	return canvas;
}

bool Tree::canUseDisplayBackedCanvas(int id, int width, int height)
{
	auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return false;
	Node &node = state.nodes[id];
	if (node.type != NodeType::Canvas) return false;
	if (width <= 0) width = node.layout.width;
	if (height <= 0) height = node.layout.height;
	if (width <= 0) width = std::atoi(getAttribute(id, "width"));
	if (height <= 0) height = std::atoi(getAttribute(id, "height"));
	if (width <= 0 || height <= 0) return false;
	return canUseDisplayFramebuffer(state, id, width, height);
}

bool Tree::markDisplayBackedCanvas(int id, int width, int height)
{
	auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return false;
	Node &node = state.nodes[id];
	if (node.type != NodeType::Canvas) return false;
	if (width <= 0) width = node.layout.width;
	if (height <= 0) height = node.layout.height;
	if (width <= 0) width = std::atoi(getAttribute(id, "width"));
	if (height <= 0) height = std::atoi(getAttribute(id, "height"));
	if (width <= 0 || height <= 0) return false;
	if (!canUseDisplayFramebuffer(state, id, width, height)) return false;

	auto *slot = state.canvases.ensure(id);
	if (!slot) return false;
	const auto previousMode = slot->mode;
	const auto *previousCanvas = slot->canvas();
	const int previousWidth = slot->width;
	const int previousHeight = slot->height;

	state.canvases.markDisplayFramebuffer(*slot, width, height);
	gCanvasLastMode = static_cast<int>(slot->mode);
	if (previousMode != slot->mode ||
	    previousCanvas != slot->canvas() ||
	    previousWidth != slot->width ||
	    previousHeight != slot->height) {
		gCanvasRebindCount++;
		markNodeDisplayCommandsDirty(id);
	}
	return true;
}

gea::framework::graphics::Canvas *Tree::ensureCameraSurface(int id, int width, int height)
{
	auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return nullptr;
	Node &node = state.nodes[id];
	if (node.type != NodeType::Camera) return nullptr;

	if (width <= 0) width = node.layout.width;
	if (height <= 0) height = node.layout.height;
	if (width <= 0) width = std::atoi(getAttribute(id, "width"));
	if (height <= 0) height = std::atoi(getAttribute(id, "height"));
	if (width <= 0 || height <= 0) return nullptr;

	auto *slot = state.canvases.ensure(id);
	if (!slot) return nullptr;
	const auto *previousCanvas = slot->canvas();
	const int previousWidth = slot->width;
	const int previousHeight = slot->height;

	// Camera frames are always streamed into an owned buffer (no full-screen
	// display-framebuffer fast path: the backend fills these pixels, not the
	// app, and the buffer is sized to the node's CSS rect).
	gea::framework::graphics::Canvas *canvas = state.canvases.bindOwned(*slot, width, height);

	if (canvas &&
	    (previousCanvas != slot->canvas() || previousWidth != slot->width || previousHeight != slot->height)) {
		markNodeDisplayCommandsDirty(id);
	}
	return canvas;
}

const gea::framework::graphics::Canvas *Tree::canvas(int id) const
{
	const auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return nullptr;
	const auto *slot = state.canvases.find(id);
	return slot && slot->canvas() && slot->canvas()->pixels() ? slot->canvas() : nullptr;
}

void Tree::displayBackedFailDebug(int *reason, int *extra, int *count) const
{
	*reason = gDisplayBackedFailReason;
	*extra = gDisplayBackedFailExtra;
	*count = gDisplayBackedFailCount;
}

void Tree::canvasSlotDebug(int id, int *mode, int *rebinds) const
{
	const auto &state = treeState();
	const auto *slot = state.canvases.find(id);
	*mode = slot ? static_cast<int>(slot->mode) : -1;
	*rebinds = gCanvasRebindCount;
}

bool Tree::isDisplayBackedCanvas(int id) const
{
	const auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return false;
	const auto *slot = state.canvases.find(id);
	return slot && slot->displayBacked();
}

void Tree::markCanvasDirty(int id)
{
	auto &state = treeState();
	if (id < 0 || id >= state.nodeCount) return;
	const auto *surface = state.canvases.find(id);
	if (surface && surface->displayBacked()) return;
	state.nodes[id].render.dirty = 1;
	state.nodes[id].render.layout_dirty = 1;
	state.nodes[id].render.non_scroll_dirty = 1;
	// Content-only repaint: the canvas's BlitImage command is unchanged frame
	// to frame (same surface pointer, same rect) — only the pixels it points
	// at were redrawn. The in-place refresh path re-records and emits a flush
	// rect only for command-dirty nodes; with just render.dirty it sees an
	// identical command, produces no dirty rect, and the fresh draws never
	// reach the display (same trap as the per-frame <camera> preview repaint
	// in tree_render.cpp, which uses the content-dirty mark for this reason).
	markNodeDisplayCommandsDirty(id);
}

// A reparent that puts `parent` under `child` closes a cycle in the parent chain.
// Every consumer walks that chain unbounded -- style's pending-root coverage test,
// the display list's dynamic-subtree test, layout's scope search -- so a single
// cycle wedges the frame task until the watchdog reboots the board. Refuse the
// move and name both ends: the caller that asked for it is the actual defect.
bool reparentWouldCycle(const TreeState &state, int child, int parent)
{
	int steps = 0;
	for (int a = parent; a >= 0 && a < state.nodeCount; a = state.nodes[a].parent) {
		if (a == child) {
			std::printf("[tree] refused reparent child=%d under its own descendant parent=%d\n", child, parent);
			return true;
		}
		if (++steps > state.nodeCount) {
			std::printf("[tree] parent chain from %d already cyclic (count=%d)\n", parent, state.nodeCount);
			return true;
		}
	}
	return false;
}

void Tree::setParent(int child, int parent)
{
	auto &state = treeState();
	if (child < 0 || child >= state.nodeCount || parent < 0 || parent >= state.nodeCount) return;
	if (child == parent) return;
	if (reparentWouldCycle(state, child, parent)) return;
	Node *c = &state.nodes[child];
	const bool localMutation = localAbsoluteLeafTreeMutation(state, child);
	if (c->parent >= 0) {
		Node *old_parent = &state.nodes[c->parent];
		if (old_parent->first_child == child) old_parent->first_child = c->next_sibling;
		if (old_parent->last_child == child) old_parent->last_child = c->prev_sibling;
		if (!localMutation) {
			old_parent->render.dirty = 1;
			old_parent->render.layout_dirty = 1;
			old_parent->render.non_scroll_dirty = 1;
		}
	}
	if (c->prev_sibling >= 0) state.nodes[c->prev_sibling].next_sibling = c->next_sibling;
	if (c->next_sibling >= 0) state.nodes[c->next_sibling].prev_sibling = c->prev_sibling;

	c->parent = parent;
	c->next_sibling = -1;
	c->prev_sibling = state.nodes[parent].last_child;
	if (state.nodes[parent].last_child >= 0)
		state.nodes[state.nodes[parent].last_child].next_sibling = child;
	else
		state.nodes[parent].first_child = child;
	state.nodes[parent].last_child = child;
	if (!localMutation) {
		state.nodes[parent].render.dirty = 1;
		state.nodes[parent].render.layout_dirty = 1;
		state.nodes[parent].render.non_scroll_dirty = 1;
	}
	c->render.dirty = 1;
	c->render.layout_dirty = 1;
	c->render.non_scroll_dirty = 1;
	if (localMutation)
		markDisplayListContentDirty();
	else
		markDisplayListDirty();
	StyleSheet::instance().recomputeSubtree(child);
}

void Tree::insertBefore(int child, int parent, int reference)
{
	auto &state = treeState();
	if (child < 0 || child >= state.nodeCount || parent < 0 || parent >= state.nodeCount) return;
	if (child == parent) return;
	if (reparentWouldCycle(state, child, parent)) return;
	// No valid reference sibling under `parent` (or it's the child itself) → append.
	if (reference < 0 || reference >= state.nodeCount || reference == child ||
	    state.nodes[reference].parent != parent) {
		setParent(child, parent);
		return;
	}

	Node *c = &state.nodes[child];
	const bool localMutation = localAbsoluteLeafTreeMutation(state, child);
	// Detach `child` from its current position first; this may mutate the
	// reference node's prev_sibling if they were adjacent, so re-read it after.
	if (c->parent >= 0) {
		Node *old_parent = &state.nodes[c->parent];
		if (old_parent->first_child == child) old_parent->first_child = c->next_sibling;
		if (old_parent->last_child == child) old_parent->last_child = c->prev_sibling;
		if (!localMutation) {
			old_parent->render.dirty = 1;
			old_parent->render.layout_dirty = 1;
			old_parent->render.non_scroll_dirty = 1;
		}
	}
	if (c->prev_sibling >= 0) state.nodes[c->prev_sibling].next_sibling = c->next_sibling;
	if (c->next_sibling >= 0) state.nodes[c->next_sibling].prev_sibling = c->prev_sibling;

	const int ref_prev = state.nodes[reference].prev_sibling;
	c->parent = parent;
	c->next_sibling = reference;
	c->prev_sibling = ref_prev;
	if (ref_prev >= 0)
		state.nodes[ref_prev].next_sibling = child;
	else
		state.nodes[parent].first_child = child;
	state.nodes[reference].prev_sibling = child;

	if (!localMutation) {
		state.nodes[parent].render.dirty = 1;
		state.nodes[parent].render.layout_dirty = 1;
		state.nodes[parent].render.non_scroll_dirty = 1;
	}
	c->render.dirty = 1;
	c->render.layout_dirty = 1;
	c->render.non_scroll_dirty = 1;
	if (localMutation)
		markDisplayListContentDirty();
	else
		markDisplayListDirty();
	StyleSheet::instance().recomputeSubtree(child);
}

void Tree::removeNode(int id)
{
	auto &state = treeState();
	if (id < 0 || id >= state.nodeCount || !state.nodeActive[id]) return;
	if (containsNode(id, state.hoveredNodeId)) pointerHover(-1, -1);
	while (state.nodes[id].first_child >= 0) {
		removeNode(state.nodes[id].first_child);
		if (id >= state.nodeCount || !state.nodeActive[id]) return;
	}
	Node *n = &state.nodes[id];
	markDisplayListDirty();
	n->render.dirty = 1;
	n->render.layout_dirty = 1;
	n->render.non_scroll_dirty = 1;
	if (n->parent >= 0) {
		Node *p = &state.nodes[n->parent];
		p->render.dirty = 1;
		p->render.layout_dirty = 1;
		p->render.non_scroll_dirty = 1;
		if (p->first_child == id) p->first_child = n->next_sibling;
		if (p->last_child == id) p->last_child = n->prev_sibling;
	}
	if (n->prev_sibling >= 0) state.nodes[n->prev_sibling].next_sibling = n->next_sibling;
	if (n->next_sibling >= 0) state.nodes[n->next_sibling].prev_sibling = n->prev_sibling;
	n->style.display = 1;
	n->parent = -1;
	n->first_child = -1;
	n->last_child = -1;
	n->prev_sibling = -1;
	n->next_sibling = -1;
	resetNodeSlot(state, id);
	trimInactiveTail(state);
}

}  // namespace gea::embedded::ui
