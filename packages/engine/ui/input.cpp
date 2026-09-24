// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

#include "pixel.h"
#include "tree_state.h"

#include "services/frame_scheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gea::embedded::ui {

namespace {

// Scroll-pacing trace (GEA_SCROLL_TRACE=1): logs gesture brackets, every
// applied drag delta, fling launches, and each momentum tick on the shared
// monotonic-ms timebase so uneven per-frame scroll jumps can be correlated
// with touch-event arrival ([sc.ev]) and frame pacing ([sc.fr]).
// A compile-time define as well as the environment variable: an embedded target
// has no environment, so getenv() left this trace -- and the hit-routing audit
// below it -- unreachable on exactly the devices whose input routing is hardest
// to observe from the outside.
#ifndef GEA_SCROLL_TRACE
#define GEA_SCROLL_TRACE 0
#endif

bool scrollTraceEnabled()
{
#if GEA_SCROLL_TRACE
	return true;
#else
	static const bool enabled = [] {
		const char *v = std::getenv("GEA_SCROLL_TRACE");
		return v && *v && *v != '0';
	}();
	return enabled;
#endif
}

class InputController {
	enum class ScrollAxis : std::int8_t {
		None = 0,
		X,
		Y
	};

public:
	static InputController &instance()
	{
		static InputController controller;
		return controller;
	}

	int hitTest(int x, int y)
	{
		Tree &tree = Tree::instance();
		if (tree.nodeCount() == 0) return -1;
		const int nodeId = hitTestNodeId(0, x, y);
		return pressTargetForNode(nodeId);
	}

	int hitTestNode(int x, int y)
	{
		Tree &tree = Tree::instance();
		if (tree.nodeCount() == 0) return -1;
		return hitTestNodeId(0, x, y);
	}

	void pointerDown(int x, int y)
	{
		Tree &tree = Tree::instance();
		clearActiveTouch();
		// A new finger touch pauses any in-flight momentum scroll. Stash
		// the current velocity so that if the user flicks again in the same
		// direction, the new fling can add to it — iOS-style "re-flick to
		// accelerate". If the next gesture turns out to be just a tap, we
		// drop the carry-over in startMomentum().
		carryOverVelocity_ = momentumVelocity_;
		carryOverAxis_ = momentumAxis_;
		cancelMomentum();
		activeScrollNode_ = tree.nodeCount() == 0 ? -1 : findScrollNodeId(0, x, y);
		snapAccumDy_ = 0;
		activeScrollAxis_ = ScrollAxis::None;
		scrollLastX_ = x;
		scrollLastY_ = y;
		resetVelocitySamples();
		pushVelocitySample(x, y);
		if (scrollTraceEnabled())
			std::fprintf(stderr, "[sc.down] t=%d x=%d y=%d node=%d carry=%.3f\n",
			             gea::framework::services::FrameScheduler::nowMs(), x, y, activeScrollNode_,
			             carryOverVelocity_);

		if (tree.nodeCount() == 0) {
			return;
		}

		const int nodeId = hitTestNodeId(0, x, y);
		if (nodeId < 0) {
			return;
		}
		// A `touch-action="none"` element (e.g. a horizontal drag slider) owns the
		// gesture: don't let an ancestor scroll container steal it.
		if (activeScrollNode_ >= 0 && nodeChainDisablesScroll(nodeId)) {
			activeScrollNode_ = -1;
		}
		// Hit-routing audit (camera-studio tap-misrouting investigation): the
		// resolved node, its tag, and its layout box for every pointer down.
		// Behind the same switch as every other trace in this file -- it went out
		// ungated and wrote to stderr on every pointer down in every app, which
		// is a per-tap stdio call on the frame task and, on a target where
		// stderr is not usable, a crash in the input path rather than a log line.
		if (scrollTraceEnabled()) {
			Node &hit = tree.nodes()[nodeId];
			std::fprintf(stderr, "[hit.down] (%d,%d) -> node=%d tag=%s rect=(%d,%d %dx%d)\n", x, y, nodeId,
			             tree.tagName(nodeId) ? tree.tagName(nodeId) : "?", hit.layout.x, hit.layout.y,
			             hit.layout.width, hit.layout.height);
		}

		const int activeNodeId = findActiveTouchNodeId(nodeId);
		if (activeNodeId < 0) {
			return;
		}

		Node &node = tree.nodes()[activeNodeId];
		activeTouchNode_ = activeNodeId;
		savedBackgroundColor_ = node.style.bg_color;
		savedHasBackground_ = node.style.has_bg;
		node.style.bg_color = node.style.has_active_bg ? node.style.active_bg_color : lightenRgb565(node.style.bg_color);
		node.style.has_bg = 1;
		activeBackgroundColor_ = node.style.bg_color;
		activeHasBackground_ = node.style.has_bg;
		node.render.dirty = 1;  // paint-only: press highlight recolors in place
		node.render.non_scroll_dirty = 1;
		// We mutated bg_color directly (bypassing setStyleValue), so the
		// direct-replay refresh path would otherwise keep the cached
		// pre-press color in the display list. Mark commands dirty so the
		// new pressed color actually renders.
		tree.markNodeDisplayCommandsDirty(activeNodeId);
	}

	int pointerMove(int x, int y)
	{
		Tree &tree = Tree::instance();
		if (activeScrollNode_ < 0 || activeScrollNode_ >= tree.nodeCount()) {
			scrollLastX_ = x;
			scrollLastY_ = y;
			return 0;
		}

		Node &node = tree.nodes()[activeScrollNode_];
		const int dx = x - scrollLastX_;
		const int dy = y - scrollLastY_;
		scrollLastX_ = x;
		scrollLastY_ = y;
		// Sample the finger on every move — even when scroll hits a boundary
		// the finger is still moving and that velocity should drive the
		// eventual fling once the user moves back inside the range.
		pushVelocitySample(x, y);
		if (dx == 0 && dy == 0) return 0;

		const bool canScrollX = nodeCanScrollX(activeScrollNode_, node);
		const bool canScrollY = nodeCanScrollY(activeScrollNode_, node);
		if (activeScrollAxis_ == ScrollAxis::None) {
			const int absDx = std::abs(dx);
			const int absDy = std::abs(dy);
			if (canScrollX && (!canScrollY || absDx >= absDy)) {
				activeScrollAxis_ = ScrollAxis::X;
			} else if (canScrollY) {
				activeScrollAxis_ = ScrollAxis::Y;
			}
		}

		bool changed = false;
		if (dx != 0 && canScrollX && activeScrollAxis_ == ScrollAxis::X) {
			int next = scrollLeft(activeScrollNode_) - dx;
			const int maxX = scrollMaxX(activeScrollNode_, node);
			if (next < 0) next = 0;
			if (next > maxX) next = maxX;
			if (next != scrollLeft(activeScrollNode_)) {
				clearActiveTouch();
				setScrollLeft(activeScrollNode_, next);
				changed = true;
				if (scrollTraceEnabled())
					std::fprintf(stderr, "[sc.mv] t=%d x=%d dx=%d sx=%d\n",
					             gea::framework::services::FrameScheduler::nowMs(), x, dx, next);
			}
		}
		if (dy != 0 && canScrollY && activeScrollAxis_ == ScrollAxis::Y) {
			// Row-snap container (snap-step attr): hold the panel still while the
			// finger moves — pointerUp applies ONE quantized scroll, which lands as
			// a single isolated (quality-waveform) refresh on e-paper instead of a
			// refresh per touch-move.
			if (nodeSnapStepY(activeScrollNode_) > 0) {
				snapAccumDy_ += dy;
				if (std::abs(snapAccumDy_) > 8) clearActiveTouch();
				return 1;
			}
			int next = scrollTop(activeScrollNode_) - dy;
			const int maxY = scrollMaxY(activeScrollNode_, node);
			if (next < 0) next = 0;
			if (next > maxY) next = maxY;
			if (next != scrollTop(activeScrollNode_)) {
				clearActiveTouch();
				setScrollTop(activeScrollNode_, next);
				changed = true;
				if (scrollTraceEnabled())
					std::fprintf(stderr, "[sc.mv] t=%d y=%d dy=%d sy=%d\n",
					             gea::framework::services::FrameScheduler::nowMs(), y, dy, next);
			}
		}
		if (!changed) return 0;

		return 1;
	}

	int pointerUp()
	{
		const int scrollNode = activeScrollNode_;
		const ScrollAxis scrollAxis = activeScrollAxis_;
		activeScrollNode_ = -1;
		activeScrollAxis_ = ScrollAxis::None;
		if (scrollTraceEnabled())
			std::fprintf(stderr, "[sc.up] t=%d node=%d\n",
			             gea::framework::services::FrameScheduler::nowMs(), scrollNode);

		const int snapStep = scrollNode >= 0 ? nodeSnapStepY(scrollNode) : 0;
		if (snapStep > 0 && scrollAxis == ScrollAxis::Y) {
			// Commit the held drag as one row-aligned scroll (no momentum).
			const int accum = snapAccumDy_;
			snapAccumDy_ = 0;
			Tree &tree = Tree::instance();
			Node &node = tree.node(scrollNode);
			const int cur = scrollTop(scrollNode);
			const int maxY = scrollMaxY(scrollNode, node);
			int raw = cur - accum;
			if (raw < 0) raw = 0;
			if (raw > maxY) raw = maxY;
			int snapped = ((raw + snapStep / 2) / snapStep) * snapStep;
			// An intentional drag always advances at least one row, even when the
			// quantizer would round back to the current offset.
			if (snapped == cur && std::abs(accum) >= 48)
				snapped = cur + (accum < 0 ? snapStep : -snapStep);
			if (snapped < 0) snapped = 0;
			if (snapped > maxY) snapped = maxY;
			if (snapped != cur) setScrollTop(scrollNode, snapped);
			resetVelocitySamples();
			if (activeTouchNode_ >= 0) clearActiveTouch();
			return -1;
		}
		snapAccumDy_ = 0;
		if (scrollNode >= 0 && nodeHasMomentumOptIn(scrollNode)) {
			pushVelocitySample(scrollLastX_, scrollLastY_);
			startMomentum(scrollNode, scrollAxis);
		}
		resetVelocitySamples();

		if (activeTouchNode_ >= 0) {
			clearActiveTouch();
		}
		return -1;
	}

	// Programmatic vertical scroll for non-pointer input — e.g. hardware
	// Up/Down buttons on touchless boards. Finds the first vertically
	// scrollable container in tree order and nudges it by `dy` layout pixels
	// (positive = toward the end of the content, i.e. "scroll down"), clamped
	// to the scroll range. Returns true if the offset actually changed. This
	// reuses the same scroll primitives as the touch path so VirtualList and
	// overflow:auto views behave identically.
	bool scrollFirstContainerY(int dy)
	{
		if (dy == 0) return false;
		Tree &tree = Tree::instance();
		const int count = tree.nodeCount();
		for (int id = 0; id < count; ++id) {
			Node &node = tree.nodes()[id];
			if (node.style.display == 1) continue;  // display:none
			if (!nodeCanScrollY(id, node)) continue;
			const int cur = scrollTop(id);
			int next = cur + dy;
			const int maxY = scrollMaxY(id, node);
			if (next < 0) next = 0;
			if (next > maxY) next = maxY;
			if (next == cur) return false;
			setScrollTop(id, next);
			return true;
		}
		return false;
	}

	// Drop every piece of per-touch / per-fling state that points into the
	// previous tree's nodes. Called by Tree::clear() on app switch so the
	// next app's first frame doesn't get a momentum tick targeted at a
	// stale node id, an activeScrollNode that points to nothing valid, or
	// velocity samples from a touch the new app never saw.
	void reset()
	{
		treeState().hoveredNodeId = -1;
		activeTouchNode_ = -1;
		savedBackgroundColor_ = 0;
		savedHasBackground_ = 0;
		activeBackgroundColor_ = 0;
		activeHasBackground_ = 0;
		activeScrollNode_ = -1;
		activeScrollAxis_ = ScrollAxis::None;
		snapAccumDy_ = 0;
		scrollLastX_ = 0;
		scrollLastY_ = 0;
		resetVelocitySamples();
		cancelMomentum();
		carryOverVelocity_ = 0.0f;
		carryOverAxis_ = ScrollAxis::None;
	}

	void tick(int timestampMs)
	{
		if (momentumNode_ < 0) return;
		Tree &tree = Tree::instance();
		if (momentumNode_ >= tree.nodeCount()) {
			cancelMomentum();
			return;
		}

		Node &node = tree.nodes()[momentumNode_];
		if (momentumAxis_ == ScrollAxis::None || !nodeCanScrollAxis(momentumNode_, node, momentumAxis_)) {
			cancelMomentum();
			return;
		}

		int dt = timestampMs - momentumLastTickMs_;
		if (dt <= 0) return;
		if (dt > 100) dt = 100;  // clamp huge dt (paused tab, GC, etc.)
		momentumLastTickMs_ = timestampMs;

		// scroll -= velocity * dt   (matches pointerMove convention:
		// finger moving right/down → delta > 0 → scroll decreases → content follows finger).
		momentumScrollAccum_ -= momentumVelocity_ * static_cast<float>(dt);

		int wholePixels = static_cast<int>(momentumScrollAccum_);
		momentumScrollAccum_ -= static_cast<float>(wholePixels);

		if (wholePixels != 0) {
			int next = momentumAxis_ == ScrollAxis::X ? scrollLeft(momentumNode_) + wholePixels
			                                          : scrollTop(momentumNode_) + wholePixels;
			const int maxScroll = momentumAxis_ == ScrollAxis::X ? scrollMaxX(momentumNode_, node)
			                                                     : scrollMaxY(momentumNode_, node);
			bool clamped = false;
			if (next < 0) {
				next = 0;
				clamped = true;
			} else if (next > maxScroll) {
				next = maxScroll;
				clamped = true;
			}
			if (momentumAxis_ == ScrollAxis::X)
				setScrollLeft(momentumNode_, next);
			else
				setScrollTop(momentumNode_, next);
			// A <virtual-list>'s recycled slots stay in sync on the scroll-only
			// fast path: RootScrollOnlyRefresh reconciles repositioned slot
			// children natively (no layout pass) and replays their old/new
			// rects, so momentum ticks no longer force the full-relayout path.
			// (That forced `non_scroll_dirty` halved the frame rate during
			// flings — 33-50ms full repaints — which read as scroll judder.)
			if (scrollTraceEnabled())
				std::fprintf(stderr, "[sc.fl] t=%d dt=%d v=%.3f px=%d s=%d%s\n",
				             timestampMs, dt, momentumVelocity_, wholePixels, next,
				             clamped ? " clamp" : "");
			if (clamped) {
				// Hit a boundary — kill the fling. (Could rubber-band here later.)
				cancelMomentum();
				return;
			}
		} else if (scrollTraceEnabled()) {
			std::fprintf(stderr, "[sc.fl] t=%d dt=%d v=%.3f px=0\n", timestampMs, dt, momentumVelocity_);
		}

		// Exponential decay. Keep the time constant closer to iOS-style
		// inertial scrolling: repeated flicks should build speed, and a long
		// list should coast instead of stopping under heavy friction.
		const float decay = std::exp(-static_cast<float>(dt) / kMomentumDecayMs);
		momentumVelocity_ *= decay;

		// Stop once velocity falls below a visible sub-pixel coast.
		if (std::fabs(momentumVelocity_) < kStopVelocity) {
			if (scrollTraceEnabled())
				std::fprintf(stderr, "[sc.stop] t=%d\n", timestampMs);
			cancelMomentum();
		}
	}

private:
	InputController() = default;

	static bool pointInsideNodeHitArea(const Node &node, int x, int y)
	{
		if (node.layout.width <= 0 || node.layout.height <= 0) return false;

		int16_t xs[4], ys[4];
		ViewRenderer::transformedRectCorners(node,
		                                     false,
		                                     node.layout.x,
		                                     node.layout.y,
		                                     node.layout.width,
		                                     node.layout.height,
		                                     xs,
		                                     ys);
		int minX = xs[0];
		int maxX = xs[0];
		int minY = ys[0];
		int maxY = ys[0];
		for (int i = 1; i < 4; ++i) {
			minX = std::min(minX, static_cast<int>(xs[i]));
			maxX = std::max(maxX, static_cast<int>(xs[i]));
			minY = std::min(minY, static_cast<int>(ys[i]));
			maxY = std::max(maxY, static_cast<int>(ys[i]));
		}
		if (x < minX || x >= maxX || y < minY || y >= maxY) return false;

		bool hasNegative = false;
		bool hasPositive = false;
		for (int i = 0; i < 4; ++i) {
			const int j = (i + 1) & 3;
			const std::int64_t edgeX = static_cast<std::int64_t>(xs[j]) - xs[i];
			const std::int64_t edgeY = static_cast<std::int64_t>(ys[j]) - ys[i];
			const std::int64_t pointX = static_cast<std::int64_t>(x) - xs[i];
			const std::int64_t pointY = static_cast<std::int64_t>(y) - ys[i];
			const std::int64_t cross = edgeX * pointY - edgeY * pointX;
			if (cross < 0) hasNegative = true;
			if (cross > 0) hasPositive = true;
			if (hasNegative && hasPositive) return false;
		}
		return true;
	}

	int hitTestNodeId(int id, int x, int y, bool clipped = false, bool groupRoot = true, bool includePositioned = true)
	{
		Node &node = Tree::instance().nodes()[id];
		if (node.style.display == 1 || isCollapsedFlexSubtree(node)) return -1;
		// CSS `pointer-events: none` — this node and its subtree are never the
		// target of a pointer event, so the hit-test falls through to whatever is
		// painted behind it. Without this, a decorative overlay image positioned
		// over interactive controls (e.g. the weather hero art over the city rail)
		// swallows their clicks. Matches the browser/DOM build, which honors it.
		if (node.style.pointer_events == 1) return -1;

		const bool inside = pointInsideNodeHitArea(node, x, y);
		if (LayoutEngine::isViewportFixed(node)) clipped = false;
		int clipX, clipY, clipW, clipH;
		overflowClipBounds(node, clipX, clipY, clipW, clipH);
		const bool outsideClip = x < clipX || y < clipY || x >= clipX + clipW || y >= clipY + clipH;
		const bool childClipped = clipped || (outsideClip && node.style.overflow != 0);
		if (childClipped && (!inside || clipped) && !LayoutEngine::containsViewportFixed(id)) return -1;

		const auto children = PaintOrder::collectChildren(id, groupRoot, includePositioned);
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			bool inheritedClip = childClipped, ignored = false;
			const Node *nodes = Tree::instance().nodes();
			for (int p = nodes[*child].parent; p >= 0 && p != id; p = nodes[p].parent) {
				const auto &ancestor = nodes[p];
				if (ancestor.style.pointer_events == 1) ignored = true;
				int ax, ay, aw, ah;
				overflowClipBounds(ancestor, ax, ay, aw, ah);
				if (ancestor.style.overflow && (x < ax || y < ay || x >= ax + aw || y >= ay + ah)) inheritedClip = true;
			}
			if (ignored) continue;
			const int result = hitTestNodeId(*child, x, y, inheritedClip, PaintOrder::isGroup(*child), PaintOrder::isContext(*child));
			if (result >= 0) return result;
		}

		if (!inside || clipped || node.style.visibility != 0) return -1;
		return id;
	}

	int findScrollNodeId(int id, int x, int y, bool clipped = false, bool groupRoot = true, bool includePositioned = true)
	{
		Node &node = Tree::instance().nodes()[id];
		if (node.style.display == 1 || isCollapsedFlexSubtree(node)) return -1;

		const bool inside = pointInsideNodeHitArea(node, x, y);
		if (LayoutEngine::isViewportFixed(node)) clipped = false;
		int clipX, clipY, clipW, clipH;
		overflowClipBounds(node, clipX, clipY, clipW, clipH);
		const bool outsideClip = x < clipX || y < clipY || x >= clipX + clipW || y >= clipY + clipH;
		const bool childClipped = clipped || (outsideClip && node.style.overflow != 0);
		if (childClipped && (!inside || clipped) && !LayoutEngine::containsViewportFixed(id)) return -1;

		const auto children = PaintOrder::collectChildren(id, groupRoot, includePositioned);
		for (auto child = children.rbegin(); child != children.rend(); ++child) {
			bool inheritedClip = childClipped, ignored = false;
			const Node *nodes = Tree::instance().nodes();
			for (int p = nodes[*child].parent; p >= 0 && p != id; p = nodes[p].parent) {
				const auto &ancestor = nodes[p];

				int ax, ay, aw, ah;
				overflowClipBounds(ancestor, ax, ay, aw, ah);
				if (ancestor.style.overflow && (x < ax || y < ay || x >= ax + aw || y >= ay + ah)) inheritedClip = true;
			}
			if (ignored) continue;
			const int result = findScrollNodeId(*child, x, y, inheritedClip, PaintOrder::isGroup(*child), PaintOrder::isContext(*child));
			if (result >= 0) return result;
		}

		if (!inside || clipped || node.style.visibility != 0) return -1;
		const bool scrollableVirtualList = node.type == NodeType::VirtualList && VirtualListRenderer::scrollMaxY(id) > 0;
		const bool scrollableView = node.type != NodeType::VirtualList && isViewLikeNodeType(node.type) &&
		                            (nodeCanScrollX(id, node) || nodeCanScrollY(id, node));
		return (scrollableVirtualList || scrollableView) ? id : -1;
	}

	static int scrollLeft(int nodeId)
	{
		Tree &tree = Tree::instance();
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return 0;
		return tree.node(nodeId).type == NodeType::VirtualList ? 0 : tree.node(nodeId).layout.scroll_x;
	}

	static int scrollTop(int nodeId)
	{
		Tree &tree = Tree::instance();
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return 0;
		return tree.node(nodeId).type == NodeType::VirtualList ? VirtualListRenderer::scrollTop(nodeId) : tree.node(nodeId).layout.scroll_y;
	}

	static int scrollMaxX(int nodeId, const Node &node)
	{
		(void)nodeId;
		return node.type == NodeType::VirtualList ? 0 : ViewRenderer::scrollMaxX(node);
	}

	static int scrollMaxY(int nodeId, const Node &node)
	{
		return node.type == NodeType::VirtualList ? VirtualListRenderer::scrollMaxY(nodeId) : ViewRenderer::scrollMaxY(node);
	}

	static bool nodeCanScrollX(int nodeId, const Node &node)
	{
		(void)nodeId;
		return node.type != NodeType::VirtualList && scrollsOverflowX(node.style) && ViewRenderer::scrollMaxX(node) > 0;
	}

	static bool nodeCanScrollY(int nodeId, const Node &node)
	{
		if (node.type == NodeType::VirtualList) return VirtualListRenderer::scrollMaxY(nodeId) > 0;
		return scrollsOverflowY(node.style) && ViewRenderer::scrollMaxY(node) > 0;
	}

	static bool nodeCanScrollAxis(int nodeId, const Node &node, ScrollAxis axis)
	{
		if (axis == ScrollAxis::X) return nodeCanScrollX(nodeId, node);
		if (axis == ScrollAxis::Y) return nodeCanScrollY(nodeId, node);
		return false;
	}

	static void setScrollLeft(int nodeId, int next)
	{
		Tree &tree = Tree::instance();
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return;
		Node &node = tree.node(nodeId);
		if (node.type == NodeType::VirtualList) return;
		if (next == node.layout.scroll_x) return;
		node.layout.scroll_x = static_cast<int32_t>(next);
		node.render.dirty = 1;
		node.render.layout_dirty = 1;
		// Horizontal scroll now has a scrollRect fast path (RootScrollOnlyRefresh
		// handles either axis), so it no longer forces the full clipped-replay
		// path via non_scroll_dirty — mirror the vertical setScrollTop path.
		tree.markScrollDirty(nodeId);
	}

	static void setScrollTop(int nodeId, int next)
	{
		Tree &tree = Tree::instance();
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return;
		if (tree.node(nodeId).type == NodeType::VirtualList) {
			VirtualListRenderer::setScrollTop(nodeId, next);
			return;
		}
		Node &node = tree.node(nodeId);
		if (next == node.layout.scroll_y) return;
		node.layout.scroll_y = static_cast<int32_t>(next);
		node.render.dirty = 1;
		node.render.layout_dirty = 1;
		tree.markScrollDirty(nodeId);
	}

	static std::uint16_t lightenRgb565(std::uint16_t color)
	{
		int r;
		int g;
		int b;
		gea::framework::graphics::pixel::unpackRgb565(color, &r, &g, &b);

		r += ((31 - r) * 6) / 32;
		g += ((63 - g) * 6) / 32;
		b += ((31 - b) * 6) / 32;

		return gea::framework::graphics::pixel::packRgb565Components(r, g, b);
	}

	static int findActiveTouchNodeId(int nodeId)
	{
		Tree &tree = Tree::instance();
		for (int current = nodeId; current >= 0 && current < tree.nodeCount(); current = tree.node(current).parent) {
			const Node &node = tree.node(current);
			if (node.style.has_active_bg || (node.style.has_bg && isNativeButtonNodeType(node.type))) return current;
		}
		return -1;
	}

	static int pressTargetForNode(int nodeId)
	{
		Tree &tree = Tree::instance();
		for (int current = nodeId; current >= 0 && current < tree.nodeCount(); current = tree.node(current).parent) {
			const int pressId = tree.pressId(current);
			if (pressId >= 0) return pressId;
		}
		return -1;
	}

	// A touched element (or any ancestor) that opts out of scrolling with a
	// `touch-action="none"` attribute claims the whole gesture — e.g. a
	// horizontal drag slider inside a vertical scroll list should set its value,
	// not scroll the list. Stops the scroll system from claiming this touch.
	static bool nodeChainDisablesScroll(int nodeId)
	{
		Tree &tree = Tree::instance();
		for (int current = nodeId; current >= 0 && current < tree.nodeCount(); current = tree.node(current).parent) {
			if (!tree.hasAttribute(current, "touch-action")) continue;
			const char *v = tree.getAttribute(current, "touch-action");
			if (v && std::strcmp(v, "none") == 0) return true;
		}
		return false;
	}

	bool clearActiveTouch()
	{
		if (activeTouchNode_ < 0) return false;

		Node &node = Tree::instance().node(activeTouchNode_);
		const bool ownsActiveBackground =
		    node.style.bg_color == activeBackgroundColor_ &&
		    node.style.has_bg == activeHasBackground_;
		if (ownsActiveBackground) {
			node.style.bg_color = savedBackgroundColor_;
			node.style.has_bg = savedHasBackground_;
			node.render.dirty = 1;  // paint-only: press highlight restore
			node.render.non_scroll_dirty = 1;
			Tree::instance().markNodeDisplayCommandsDirty(activeTouchNode_);
		}
		activeTouchNode_ = -1;
		activeBackgroundColor_ = 0;
		activeHasBackground_ = 0;
		return ownsActiveBackground;
	}

	// --- Momentum-scroll helpers ----------------------------------------

	void pushVelocitySample(int x, int y)
	{
		velocitySamples_[velocitySampleNext_] =
		    VelocitySample{gea::framework::services::FrameScheduler::nowMs(), x, y};
		velocitySampleNext_ = (velocitySampleNext_ + 1) % kVelocitySampleCount;
		if (velocitySampleCount_ < kVelocitySampleCount) velocitySampleCount_++;
	}

	void resetVelocitySamples()
	{
		velocitySampleCount_ = 0;
		velocitySampleNext_ = 0;
	}

	// Vertical row-snap opt-in: a positive integer `snap-step` attribute on the
	// scrollable is the row pitch in px. 0 = no snapping (normal live scroll).
	int nodeSnapStepY(int nodeId) const
	{
		Tree &tree = Tree::instance();
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return 0;
		if (!tree.hasAttribute(nodeId, "snap-step")) return 0;
		const char *attr = tree.getAttribute(nodeId, "snap-step");
		if (!attr) return 0;
		const int step = std::atoi(attr);
		return step > 0 ? step : 0;
	}

	bool nodeHasMomentumOptIn(int nodeId) const
	{
		Tree &tree = Tree::instance();
		// Momentum/fling is OPT-IN via the `momentum` attribute. The old
		// fling-by-default behavior generated long tails of post-release scroll
		// frames — on e-paper each one is a panel refresh, so an un-opted list
		// kept repainting seconds after the finger lifted.
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return false;
		if (!tree.hasAttribute(nodeId, "momentum")) return false;
		const char *attr = tree.getAttribute(nodeId, "momentum");
		if (!attr) return true;
		if (std::strcmp(attr, "false") == 0) return false;
		if (std::strcmp(attr, "0") == 0) return false;
		return true;
	}

	void startMomentum(int scrollNode, ScrollAxis axis)
	{
		if (velocitySampleCount_ < 2) return;
		if (axis == ScrollAxis::None) return;
		Tree &tree = Tree::instance();
		if (scrollNode < 0 || scrollNode >= tree.nodeCount()) return;
		if (!nodeCanScrollAxis(scrollNode, tree.node(scrollNode), axis)) return;

		// Average velocity from the oldest sample to the newest in the ring.
		// Using two endpoints rather than a per-sample derivative smooths
		// out the noisy last few touch reports right before lift-off.
		const int newestIdx =
		    (velocitySampleNext_ + kVelocitySampleCount - 1) % kVelocitySampleCount;
		const int oldestIdx =
		    velocitySampleCount_ < kVelocitySampleCount
		        ? 0
		        : velocitySampleNext_;
		const VelocitySample &newest = velocitySamples_[newestIdx];
		const VelocitySample &oldest = velocitySamples_[oldestIdx];
		const int dtMs = newest.timestampMs - oldest.timestampMs;
		if (dtMs <= 0) return;

		const int nowMs = gea::framework::services::FrameScheduler::nowMs();

		const int delta = axis == ScrollAxis::X ? newest.x - oldest.x : newest.y - oldest.y;
		float velocityPxPerMs = static_cast<float>(delta) / static_cast<float>(dtMs);

		// Below ~0.06 px/ms = 60 px/sec, the fling is so weak it'd stop in a
		// frame or two — not worth the tick overhead and visually a no-op.
		// A tap (no drag, weak velocity) also lands here, which is what we
		// want: a tap should NOT resume a stashed fling.
		if (std::fabs(velocityPxPerMs) < kMinFlingVelocity) {
			if (scrollTraceEnabled())
				std::fprintf(stderr, "[sc.start] t=%d reject v=%.3f win=%dms d=%d\n",
				             nowMs, velocityPxPerMs, dtMs, delta);
			carryOverVelocity_ = 0.0f;
			carryOverAxis_ = ScrollAxis::None;
			return;
		}

		const float carryAtLaunch = carryOverVelocity_;
		velocityPxPerMs *= kLaunchVelocityBoost;

		// iOS "re-flick to accelerate": if there was an in-flight fling and
		// the new flick goes in the same direction, add the leftover
		// velocity to the new one. Opposite direction → user is reversing
		// course, drop the stash.
		if (carryOverAxis_ == axis && carryOverVelocity_ != 0.0f &&
		    (carryOverVelocity_ > 0.0f) == (velocityPxPerMs > 0.0f)) {
			velocityPxPerMs += carryOverVelocity_ * kCarryOverBoost;
		}
		carryOverVelocity_ = 0.0f;
		carryOverAxis_ = ScrollAxis::None;

		// Cap absolute velocity so successive rapid flicks can't chain into
		// an absurd speed. iOS can hit very high projected speeds; allow
		// that headroom while still bounding pathological repeated flicks.
		if (velocityPxPerMs > kMaxMomentumVelocity) velocityPxPerMs = kMaxMomentumVelocity;
		else if (velocityPxPerMs < -kMaxMomentumVelocity) velocityPxPerMs = -kMaxMomentumVelocity;

		momentumNode_ = scrollNode;
		momentumAxis_ = axis;
		momentumVelocity_ = velocityPxPerMs;
		momentumLastTickMs_ = nowMs;
		momentumScrollAccum_ = 0.0f;
		treeState().inputTickRequired = true;
		if (scrollTraceEnabled())
			std::fprintf(stderr, "[sc.start] t=%d v=%.3f win=%dms d=%d carry=%.3f node=%d\n",
			             nowMs, velocityPxPerMs, dtMs, delta, carryAtLaunch, scrollNode);
	}

	void cancelMomentum()
	{
		momentumNode_ = -1;
		momentumAxis_ = ScrollAxis::None;
		momentumVelocity_ = 0.0f;
		momentumScrollAccum_ = 0.0f;
		treeState().inputTickRequired = false;
	}

	struct VelocitySample {
		int timestampMs;
		int x;
		int y;
	};
	static constexpr int kVelocitySampleCount = 5;
	static constexpr float kLaunchVelocityBoost = 1.45f;
	static constexpr float kCarryOverBoost = 2.0f;
	static constexpr float kMaxMomentumVelocity = 24.0f;  // px/ms = 24000 px/s
	static constexpr float kMinFlingVelocity = 0.06f;     // px/ms = 60 px/s
	static constexpr float kMomentumDecayMs = 680.0f;
	static constexpr float kStopVelocity = 0.012f;        // px/ms = 12 px/s

	int activeTouchNode_ = -1;
	std::uint16_t savedBackgroundColor_ = 0;
	std::int8_t savedHasBackground_ = 0;
	std::uint16_t activeBackgroundColor_ = 0;
	std::int8_t activeHasBackground_ = 0;
	int activeScrollNode_ = -1;
	int snapAccumDy_ = 0;
	ScrollAxis activeScrollAxis_ = ScrollAxis::None;
	int scrollLastX_ = 0;
	int scrollLastY_ = 0;

	// Momentum-scroll state.
	VelocitySample velocitySamples_[kVelocitySampleCount] = {};
	int velocitySampleCount_ = 0;
	int velocitySampleNext_ = 0;
	int momentumNode_ = -1;
	ScrollAxis momentumAxis_ = ScrollAxis::None;
	float momentumVelocity_ = 0.0f;   // px/ms in finger movement direction
	int momentumLastTickMs_ = 0;
	float momentumScrollAccum_ = 0.0f;

	// Leftover fling velocity at the moment a new pointerDown lands.
	// Folded into the next flick if same-direction so successive flicks
	// accelerate (iOS-style).
	float carryOverVelocity_ = 0.0f;
	ScrollAxis carryOverAxis_ = ScrollAxis::None;
};

}  // namespace

int Tree::hitTest(int x, int y)
{
	return InputController::instance().hitTest(x, y);
}

int Tree::hitTestNode(int x, int y)
{
	return InputController::instance().hitTestNode(x, y);
}

int Tree::pointerHover(int x, int y)
{
	auto &state = treeState();
	const int target = x < 0 || y < 0 ? -1 : hitTestNode(x, y);
	if (target != state.hoveredNodeId) {
		state.hoveredNodeId = target;
		StyleSheet::instance().hoverChanged();
	}
	return target;
}

bool Tree::isHovered(int node) const
{
	const auto &state = treeState();
	const int target = state.hoveredNodeId;
	return target >= 0 && target < state.nodeCount && state.nodeActive[target] &&
	       containsNode(mountedRoot(), target) && containsNode(node, target);
}

void Tree::pointerDown(int x, int y)
{
	InputController::instance().pointerDown(x, y);
}

int Tree::pointerMove(int x, int y)
{
	return InputController::instance().pointerMove(x, y);
}

int Tree::pointerUp()
{
	return InputController::instance().pointerUp();
}

bool Tree::scrollByKeyStep(int dy)
{
	return InputController::instance().scrollFirstContainerY(dy);
}

void Tree::tickInput(int timestampMs)
{
	InputController::instance().tick(timestampMs);
}

void Tree::resetInput()
{
	InputController::instance().reset();
}

int Tree::activeInputId() const
{
	return treeState().activeInputId;
}

void Tree::setActiveInput(int nodeId)
{
	auto &state = treeState();
	if (nodeId < -1 || nodeId >= state.nodeCount) nodeId = -1;
	if (state.activeInputId == nodeId) return;
	const int previousId = state.activeInputId;
	state.activeInputId = nodeId;
	state.caretVisible = true;
	state.caretLastFlipMs = state.lastFrameMs;
	// Both the old input (caret needs to disappear) and the new input
	// (caret needs to appear) must be re-recorded AND their screen rects
	// flushed. Without render.dirty + nodeCommandDirty the rebuilt
	// display list drops/adds the caret command but the dirty-region
	// tracker leaves the old pixels on the panel.
	auto markInputDirty = [&](int id) {
		if (id < 0 || id >= state.nodeCount) return;
		Node &n = state.nodes[id];
		n.render.dirty = 1;
		n.render.layout_dirty = 1;
		n.render.non_scroll_dirty = 1;
		state.nodeCommandDirty[id] = 1;
	};
	markInputDirty(previousId);
	markInputDirty(nodeId);
	// NOTE: previously this also flipped `inputTickRequired` to mirror the
	// keyboard-active state. That field belongs to the momentum-scroll
	// InputController — touching it from here was canceling in-flight scroll
	// ticks every time a Press landed on a non-input, which broke tap
	// responsiveness on ESP32. The caret has its own redraw signal via
	// `displayListDirty`; the keyboard subsystem decides activity in
	// notifyActiveInputChanged.
	state.displayListDirty = true;
}

bool Tree::activeInputCaretVisible() const
{
	return treeState().caretVisible;
}

void Tree::tickInputCaret(int timestampMs)
{
	auto &state = treeState();
	if (state.activeInputId < 0) {
		state.caretVisible = false;
		return;
	}
	constexpr int kCaretBlinkMs = 500;
	if (state.caretLastFlipMs == 0) state.caretLastFlipMs = timestampMs;
	if (timestampMs - state.caretLastFlipMs >= kCaretBlinkMs) {
		state.caretVisible = !state.caretVisible;
		state.caretLastFlipMs = timestampMs;
		// Three flags, all required:
		//   - nodeCommandDirty: tells the direct-replay path that this
		//     node needs re-recording on the next refresh
		//   - displayListDirty: forces a full rebuild (skipping direct-
		//     replay) — InputRenderer reads activeInputCaretVisible
		//     fresh on each record so we always get the latest blink
		//     phase in the output DrawText / FillRect commands
		//   - render.dirty + non_scroll_dirty: tells the per-frame
		//     dirty-region tracker that the input's rect needs to be
		//     pushed to the panel. Without this the commands change
		//     but the screen doesn't (the framework decides the
		//     bounding box hasn't moved so nothing needs to flush).
		state.nodeCommandDirty[state.activeInputId] = 1;
		state.displayListDirty = true;
		Node &n = state.nodes[state.activeInputId];
		n.render.dirty = 1;
		n.render.layout_dirty = 1;
		n.render.non_scroll_dirty = 1;
	}
}

}  // namespace gea::embedded::ui
