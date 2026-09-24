// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "canvas_store.h"
#include "css_atom.h"
#include "node_model.h"
#include "style.h"
#include "tree_events.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace gea::embedded::ui {

struct NodeClassList {
	static constexpr std::uint8_t kInlineTokenCount = 4;
	static constexpr std::uint16_t kNoOverflow = 0xFFFFu;

	CssAtomId inlineTokens[kInlineTokenCount]{};
	std::uint16_t overflowHandle = kNoOverflow;
	std::uint8_t count = 0;

	NodeClassList() = default;
	NodeClassList(const NodeClassList &other);
	NodeClassList &operator=(const NodeClassList &other);
	NodeClassList(NodeClassList &&other) noexcept;
	NodeClassList &operator=(NodeClassList &&other) noexcept;
	~NodeClassList();

	void clear();
	bool set(const char *className);
	bool set(const std::string &className);
	bool add(const std::string &token);
	bool remove(const std::string &token);
	bool toggle(const std::string &token);
	bool toggle(const std::string &token, bool force);
	bool contains(const std::string &token) const;
	bool containsAtom(CssAtomId token) const;
	std::size_t size() const { return count; }
	CssAtomId at(std::size_t index) const;
	std::string value() const;
	bool empty() const { return count == 0; }
};

struct NodeStyleOverride {
	Property property;
	int value;
};

struct NodeStyleOverrideStore {
	static constexpr std::uint8_t kInlineCount = 4;
	NodeStyleOverride inlineValues[kInlineCount]{};
	NodeStyleOverride *spillValues = nullptr;
	std::size_t spillCount = 0;
	std::size_t spillCapacity = 0;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	NodeStyleOverrideStore() = default;
	NodeStyleOverrideStore(const NodeStyleOverrideStore &other);
	NodeStyleOverrideStore &operator=(const NodeStyleOverrideStore &other);
	NodeStyleOverrideStore(NodeStyleOverrideStore &&other) noexcept;
	NodeStyleOverrideStore &operator=(NodeStyleOverrideStore &&other) noexcept;
	~NodeStyleOverrideStore();

	void clear();
	void set(Property property, int value);
	bool remove(Property property);
	std::size_t size() const { return spilled ? spillCount : inlineCount; }
	bool empty() const { return size() == 0; }
	const NodeStyleOverride &at(std::size_t index) const;
};

// CSS ::first-line painting is uncommon and line fragments are useful only
// for nodes inside a block with a first-line background. Keep this metadata in
// the node's rare allocation instead of charging every Node for it.
struct FirstLineBackground {
	style_color_t color = 0;
	std::uint8_t alpha = 0;
	bool hasColor = false;
	std::int16_t lineY = 0;
	std::int16_t lineHeight = 0;
	std::int16_t lineContextNode = -1;
	bool lineValid = false;
};

struct FirstLineFragment {
	std::int16_t x = 0;
	std::int16_t y = 0;
	std::int16_t width = 0;
	std::int16_t height = 0;
	std::int16_t contextNode = -1;
	bool valid = false;
};

struct InlineStaticPosition {
	std::int16_t x = 0;
	std::int16_t y = 0;
	bool continuationLine = false;
	bool valid = false;
};

struct NodeCustomProperty {
	CssAtomId nameId = kInvalidCssAtom;
	std::string value;
	CssAtomId valueAtom = kInvalidCssAtom;
	std::int32_t colorStyle = 0;
	std::int32_t colorNative = 0;
	float lengthValue = 0.0f;
	std::uint8_t colorAlpha = 255;
	std::uint8_t lengthUnit = 0;
	std::uint8_t flags = 0;

	bool hasColor() const { return (flags & 1u) != 0; }
	bool hasLength() const { return (flags & 2u) != 0; }
};

struct NodeCustomPropertyStore {
	std::vector<NodeCustomProperty> values;

	void clear();
	void set(const std::string &name, const std::string &value);
	void set(CssAtomId nameId, const std::string &value);
	void setColor(CssAtomId nameId,
	              const std::string &value,
	              std::int32_t styleColor,
	              std::int32_t nativeColor,
	              std::uint8_t alpha);
	void setLength(CssAtomId nameId,
	               const std::string &value,
	               float lengthValue,
	               std::uint8_t lengthUnit);
	const std::string *get(const std::string &name) const;
	const std::string *get(CssAtomId nameId) const;
	const NodeCustomProperty *getEntry(CssAtomId nameId) const;
};

// Generic windowing state for a <virtual-list>. The element renders a small
// pool of real child "slot" nodes (the row template, supplied by the app) and
// scrolls over a virtual content height of itemCount * rowHeight. It carries no
// app-specific row content: labels, fonts, colours and layout all come from the
// child slots' own CSS, exactly like any other node.
struct VirtualListNodeState {
	int32_t itemCount = 0;       // total virtual item count (item-count attribute)
	int32_t rowHeight = 0;       // derived from the first slot child's CSS layout height
	int32_t scrollTop = 0;       // kept in sync with layout.scroll_y
	int32_t previousScrollTop = 0;
	bool framebufferSynced = true;
};

// Blink-style "rare data" block: the cold/optional per-node state that the vast
// majority of nodes never have — event listeners, attributes, CSS custom
// properties, and virtual-list windowing state. Stored in a shared pool and
// referenced by a single int16 handle on the node (Node::rare_data) instead of
// inlining a fixed slot on every node, so a plain styled node allocates nothing.
struct GridTrackLayout {
	std::vector<int> columnStart, columnEnd, rowStart, rowEnd;
	int columnOrigin = 0, rowOrigin = 0;
};

struct NodeRareData {
	NodeAttributeStore attributes;
	NodeEventListeners listeners;
	NodeCustomPropertyStore customProperties;
	// Authored inline values survive rebuilding the computed custom-property map.
	NodeCustomPropertyStore inlineCustomProperties;
	NodeStyleOverrideStore defaultStyles;
	NodeStyleOverrideStore inlineStyles;
	CssAtomId inlineGridTemplates[2] = {kInvalidCssAtom, kInvalidCssAtom};
	uint8_t inlineGridShorthandMask = 0;
	std::unique_ptr<GridTrackLayout> gridLayout;
	VirtualListNodeState virtualList;
	FirstLineBackground firstLineBackground;
	FirstLineFragment firstLineFragment;
	InlineStaticPosition inlineStaticPosition;

	void clear()
	{
		attributes.clear();
		listeners.clear();
		customProperties.clear();
		inlineCustomProperties.clear();
		defaultStyles.clear();
		inlineStyles.clear();
		inlineGridTemplates[0] = inlineGridTemplates[1] = kInvalidCssAtom;
		inlineGridShorthandMask = 0;
		gridLayout.reset();
		virtualList = VirtualListNodeState{};
	firstLineBackground = FirstLineBackground{};
	firstLineFragment = FirstLineFragment{};
	inlineStaticPosition = InlineStaticPosition{};
	}
};

struct TreeState {
	Node nodes[kMaxNodes];
	// Attributes and event listeners are no longer dense per-node arrays — they
	// live in the shared NodeRareData pool, referenced by Node::rare_data. The
	// vast majority of nodes have neither (a class-only node allocates nothing),
	// so this reclaims the ~387 KB attribute + ~96 KB listener tables that were
	// inlined-but-empty on every node.
	NodeClassList classLists[kMaxNodes];
	// defaultStyles + inlineStyles now live in the NodeRareData pool too.
	CanvasStore canvases;
	// customProperties + virtualList now live in the shared NodeRareData pool
	// (Node::rare_data) — most nodes have neither.
	uint8_t nodeActive[kMaxNodes]{};
	int nodeCount = 0;
	int mountedRoot = -1;
	int mountedWidth = 0;
	int mountedHeight = 0;
	// Conservative fast-out for fixed-descendant clip/scroll checks. Set on
	// first use and retained until tree state is reset.
	bool fixedPositionUsed = false;
	int lastFrameMs = 0;
	// Monotonic counter bumped on every mount()/refresh() — i.e. whenever the
	// reactive tree is (re)built. Lets native renderers (macOS) skip an entire
	// re-sync when the tree hasn't changed since the last frame, instead of
	// diffing the whole view tree at 60Hz while idle.
	uint64_t refreshSerial = 0;
	uint64_t scrollDirtyNodes[kScrollDirtyWordCount]{};
	bool scrollDirtyAny = false;
	int pendingScrollIntoViewNode = -1;
	bool inputTickRequired = false;
	bool displayListDirty = true;
	// When a rebuild is pending (displayListDirty), this says whether it may have
	// changed draw order / which commands exist (node add/remove/reorder, z-index,
	// display toggle, pixel-spilling filters). If false, the rebuild is
	// appearance-only on existing nodes (e.g. a partial-opacity fade or an
	// absolute-leaf resize) — no draw-order change — so refresh can replay just the
	// dirty-node regions instead of repainting the whole viewport. Set true by
	// markDisplayListDirty(); left untouched by markDisplayListContentDirty().
	bool displayListRebuildStructural = true;
	int styleInvalidationSuppressionDepth = 0;
	uint8_t nodeCommandDirty[kMaxNodes]{};
	uint8_t nodeCommandDirtyBoundsValid[kMaxNodes]{};
	uint8_t nodeCommandDirtyCanOverpaint[kMaxNodes]{};
	// Static-backdrop dynamic-leaf set: frames remaining in which this node counts
	// as "recently changed" (a ticking text badge, a counter). While > 0 the node
	// is EXCLUDED from the backdrop bake, its commands stay recorded, settle
	// tolerates its dirtiness, and content changes on it skip the backdrop
	// invalidation — so a periodic HUD update no longer drops the whole cache.
	// Set saturating by Tree::setText on a real change; decremented once per
	// refresh by LayoutSnapshot::capture; expiry forces one backdrop invalidation
	// so the node folds back into the next bake consistently.
	uint8_t nodeBackdropCooldown[kMaxNodes]{};
	// Whether the last backdrop bake painted this node into the cache. Drives the
	// setText invalidation decision: changing a node whose pixels are NOT in the
	// cache never needs to drop the cache.
	uint8_t nodeInBackdrop[kMaxNodes]{};
	int16_t nodeCommandDirtyX0[kMaxNodes]{};
	int16_t nodeCommandDirtyY0[kMaxNodes]{};
	int16_t nodeCommandDirtyX1[kMaxNodes]{};
	int16_t nodeCommandDirtyY1[kMaxNodes]{};

	// Gate for the keyframe-3D transform record path. `transformPresent` is true
	// iff some node carries a non-identity transform/perspective this frame or
	// last frame; it is recomputed once per refresh (keyed on refreshSerial) by
	// ViewGeometry::anyTransformPresent(). When false, the per-node ancestor-chain
	// transform walks short-circuit to O(1) — non-3D trees (the common case) pay
	// nothing for the 3D machinery. Defaults coax a recompute before first use.
	uint64_t transformScanSerial = ~0ull;
	bool transformPresent = true;
	// Durable validity for a cached `transformPresent == false`. refreshSerial bumps
	// several times mid-refresh, so the serial-keyed cache above re-scans on each bump.
	// But a transform-free tree CANNOT gain a transform without a setStyle(transform),
	// and both such sites clear this flag — so once we scan and find no transforms, the
	// `false` holds across the bumps with no re-scan. This is what lets mode()/dirty/snap
	// short-circuit the per-node transform reads without paying a scan-per-phase (the
	// old mode-gate regression was that re-scan, not the gate itself). A cached `true`
	// is NOT held here — it still re-scans per refreshSerial so a removal is seen.
	bool transformScanValid = false;

	// Focus + caret state for `<input>` elements. -1 means no input is
	// focused. caretLastFlipMs/caretVisible drive the blink animation —
	// consulted by InputRenderer::record on the pixel target. The macOS
	// renderer ignores these (NSResponder owns its own caret).
	int activeInputId = -1;
	int hoveredNodeId = -1;
	int caretLastFlipMs = 0;
	bool caretVisible = true;
};

TreeState &treeState();

}  // namespace gea::embedded::ui
