// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pixel.h"

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>

namespace gea::embedded::ui {

// Style colours are stored in this board's native pixel format (pixel::native_t —
// RGB565 on esp32/geaos, RGBA8888 on iOS). Colours are born native: the CSS parser
// and geatsc fold both produce native_t, so nothing downstream converts.
using style_color_t = gea::framework::graphics::pixel::native_t;

// Per-node TreeState arrays scale linearly with this: sizeof(TreeState) is
// ~318 B/node, so 512 nodes = ~159 KB. That is designed to live in PSRAM (see
// treeState() in tree_state.cpp). A PSRAM-less board — the ESP32-C3 Xteink X3 —
// cannot host a 159 KB monolith in its largest ~113 KB SRAM bank, so it lowers
// this via -DGEA_EMBEDDED_MAX_NODES. The define is applied through the target's
// GEA_EMBEDDED_TARGET_COMPILE_DEFINITIONS, which reaches every framework TU (the
// framework is compiled per-app), so sizeof(TreeState) stays consistent across
// the whole link — an inconsistent value here would be silent memory corruption.
#ifndef GEA_EMBEDDED_MAX_NODES
#define GEA_EMBEDDED_MAX_NODES 512
#endif
inline constexpr int kMaxNodes = GEA_EMBEDDED_MAX_NODES;
inline constexpr int kUnset = -32768;
// Internal value carrier for CSS z-index:auto; numeric stack levels stay int16.
inline constexpr int kZIndexAuto = INT32_MIN;
// Dimension expression slots use nonnegative values for pooled expressions,
// -1 for no expression, and these tags for intrinsic sizing keywords.
inline constexpr int kSizeMinContent = -2;
inline constexpr int kSizeMaxContent = -3;
inline constexpr int kSizeFitContent = -4;

inline bool isIntrinsicSizeExpression(int value)
{
	return value >= kSizeFitContent && value <= kSizeMinContent;
}
inline constexpr int kScrollDirtyWordCount = (kMaxNodes + 63) / 64;
inline constexpr int kMaxGridTracks = 8;
// Layout-time track capacity: IMPLICIT rows (an auto-flowing grid of N
// children) may far exceed the 8-track template arrays above — a 3-column
// library of 30 books needs 10 rows. Children past the row capacity were
// never positioned and rendered at stale coordinates (stacked on row one).
// Template-declared tracks stay capped at kMaxGridTracks; rows beyond the
// template are implicit (auto-sized), so only the layout locals grow.
inline constexpr int kMaxGridLayoutTracks = 96;
inline constexpr int kGridLineSpan = 65536;

#ifndef GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES
#define GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES 0
#endif
// Compiler-driven hot-field placement: when the app (per geatsc's static knowledge of
// which style fields it uses) reads/writes transform/gradient/etc. every frame, set this
// so the "rare" fields live INLINE in ComputedStyle (a direct load, same cache line —
// spine's access pattern) instead of behind the rstyle() pool indirection (a call + branch
// + a separate cache line per node). For a transform+gradient-heavy app like css-3d-cube,
// the pool indirection is a measured per-node regression on every dirty/dlist/record walk.
#ifndef GEA_EMBEDDED_RARE_STYLE_INLINE
#define GEA_EMBEDDED_RARE_STYLE_INLINE 0
#endif
inline constexpr int8_t kDisplayBlock = 0;
inline constexpr int8_t kDisplayNone = 1;
inline constexpr int8_t kDisplayGrid = 2;
inline constexpr int8_t kDisplayFlex = 3;
inline constexpr int kPositionFixed = 3;
inline bool isOutOfFlowPosition(int position) { return position == 1 || position == kPositionFixed; }
// Existing alignment values occupy the low nibble. Overflow-position is
// independent of the alignment keyword and fits in the existing int8 fields.
inline constexpr int kAlignSafe = 16;
inline constexpr int kAlignUnsafe = 32;
inline constexpr int kAlignStart = 7;
inline constexpr int kAlignEnd = 8;
inline constexpr int kAlignLastBaseline = 9;
inline constexpr int kAlignSelfStart = 10;
inline constexpr int kAlignSelfEnd = 11;
inline constexpr int kAlignLeft = 12;
inline constexpr int kAlignRight = 13;
inline constexpr int kAlignSpaceEvenly = 14;
inline bool isDistributedAlignment(int alignment)
{
	const int keyword = alignment & 15;
	return keyword == 3 || keyword == 4 || keyword == kAlignSpaceEvenly;
}
inline int distributedAlignmentOffset(int alignment, int free, int index, int count)
{
	// Cumulative division keeps rounding error from accumulating across gaps.
	// Distributed alignment falls back to safe start/center on overflow.
	if (free <= 0 || count <= 0) return 0;
	switch (alignment & 15) {
	case 3: return count > 1 ? free * index / (count - 1) : 0;
	case 4: return free * (2 * index + 1) / (2 * count);
	case kAlignSpaceEvenly: return free * (index + 1) / (count + 1);
	default: return 0;
	}
}
inline int usedAlignment(int alignment, int freeSpace, bool reversed = false)
{
	if (alignment < 0) return alignment;
	const int keyword = (alignment & kAlignSafe) && freeSpace < 0 ? kAlignStart : alignment & 15;
	if (keyword == kAlignStart) return reversed ? 2 : 6;
	if (keyword == kAlignEnd) return reversed ? 6 : 2;
	return keyword;
}

enum class NodeType : int8_t {
	View = 0,
	Text = 1,
	Image = 2,
	Canvas = 3,
	Button = 4,
	VirtualList = 5,
	Camera = 6,
	Audio = 7
};

inline bool isViewLikeNodeType(NodeType type)
{
	return type == NodeType::View || type == NodeType::Button || type == NodeType::VirtualList;
}

inline bool isNativeButtonNodeType(NodeType type)
{
	return type == NodeType::Button;
}

// Cold/rare style fields. Pooled by default (referenced via ComputedStyle::rare_style),
// or embedded inline in ComputedStyle when GEA_EMBEDDED_RARE_STYLE_INLINE is set (defined
// BEFORE ComputedStyle so it can be a by-value member).
struct RareStyle {
	// Logical edges: block-start/end, inline-start/end.
	uint8_t margin_trim = 0;
	// IEEE float bits travel through the existing integer style-value transport.
	// Negative ratios mean `auto <ratio>` (prefer a replaced element's natural ratio).
	int32_t aspect_ratio = 0;
	int32_t flex_line_count = 1;
	int32_t flex_basis_expression = -1;
	int32_t line_height_expression = -1;
	// Individual grid item alignment; auto defers to the parent's justify-items.
	int8_t justify_self = -1;
	// Computed contain flags: size, inline-size, layout, style, paint.
	// Currently consumed by document background propagation.
	uint8_t containment = 0;
	uint16_t bg_clip = 0; // Interned clip list; zero is border-box.
	// row-start, column-start, row-end, column-end; zero is auto. Positive
	// spans carry kGridLineSpan, while signed integers retain explicit lines.
	int32_t grid_line[4] = {};
	// Deferred box edges; absent expressions do not allocate a rare-style record.
	int32_t margin_expression[4] = {-1, -1, -1, -1};
	int32_t padding_expression[4] = {-1, -1, -1, -1};
	// --- grid (display:grid track templates) ---
	int8_t grid_column_count = 0;
	int8_t grid_row_count = 0;
	int8_t grid_column_type[kMaxGridTracks] = {};
	int8_t grid_row_type[kMaxGridTracks] = {};
	int16_t grid_column_value[kMaxGridTracks] = {};
	int16_t grid_row_value[kMaxGridTracks] = {};

	// --- transform / perspective (defaults must match NodeLifecycle::init:
	//     scale 1000 = 1.0x, origins 500 = 50% center; the rest 0) ---
	// Authored identity effects still establish positioning containing blocks.
	uint8_t transform_present = 0;
	uint8_t transform_preserve_3d = 0;
	uint8_t rotate_present = 0;
	int16_t rotate_angle = 0;
	int32_t rotate_axis_x = 0, rotate_axis_y = 0, rotate_axis_z = 1000000;
	int16_t scale_x = 1000, scale_y = 1000, scale_z = 1000;
	uint8_t scale_present = 0;
	uint8_t translate_present = 0;
	uint8_t transform_translate_outer_axes = 0;
	int16_t translate_x = 0, translate_y = 0, translate_z = 0;
	int16_t translate_x_percent = 0, translate_y_percent = 0;
	uint8_t filter_present = 0;
	int16_t transform_rotate = 0;
	int16_t transform_rotate_x = 0;
	int16_t transform_rotate_y = 0;
	int16_t transform_translate_x = 0;
	int16_t transform_translate_y = 0;
	int16_t transform_translate_z = 0;
	int16_t transform_translate_x_percent = 0;
	int16_t transform_translate_y_percent = 0;
	int16_t transform_scale_x = 1000;
	int16_t transform_scale_y = 1000;
	int16_t transform_scale_z = 1000;
	int16_t transform_origin_x = 500;
	int16_t transform_origin_y = 500;
	int16_t perspective = 0;
	int16_t perspective_origin_x = 500;
	int16_t perspective_origin_y = 500;

	// --- filter / box-shadow (all default 0 = no effect) ---
	int16_t filter_blur_radius = 0;
	uint8_t box_shadow_inset = 0;
	int16_t box_shadow_offset_x = 0;
	int16_t box_shadow_offset_y = 0;
	int16_t box_shadow_blur_radius = 0;
	int16_t box_shadow_spread = 0;
	style_color_t box_shadow_color = 0;
	uint8_t box_shadow_alpha = 0;

	// --- per-side borders (default white/opaque, matching NodeLifecycle::init;
	//     only relevant when a side width > 0, which is rare) ---
	int16_t border_side_width[4] = {};
	// 0: flat; 1: groove; 2: ridge; 3: inset; 4: outset.
	uint8_t border_relief[4] = {};
	// Bits 0..3: explicit side colors; bit 4: literal common color;
	// bits 5..8: explicit side currentColor. Unspecified colors use currentColor.
	uint16_t border_color_flags = 0;
	style_color_t border_side_color[4] = {
		gea::framework::graphics::pixel::nativeColor(255, 255, 255),
		gea::framework::graphics::pixel::nativeColor(255, 255, 255),
		gea::framework::graphics::pixel::nativeColor(255, 255, 255),
		gea::framework::graphics::pixel::nativeColor(255, 255, 255),
	};

		uint8_t border_side_alpha[4] = {255, 255, 255, 255};

	// --- linear / overlay / radial gradients + background grid (defaults match
	//     NodeLifecycle::init: alphas 255, stops 500/1000, angle 1800, radial
	//     center/extent 500/1000) ---
	uint16_t bg_image_layer_count = 1; // Includes none layers.
	int32_t bg_size_list = -1, bg_position_list = -1, bg_repeat_list = -1;
	int32_t bg_attachment_list = -1, bg_origin_list = -1;
	uint16_t bg_gradient_layer = 0, bg_overlay_gradient_layer = 0, bg_radial_gradient_layer = 0;
	style_color_t bg_gradient_from_color = 0;
	style_color_t bg_gradient_mid_color = 0;
	style_color_t bg_gradient_to_color = 0;
	uint8_t bg_gradient_from_alpha = 255;
	uint8_t bg_gradient_mid_alpha = 255;
	uint8_t bg_gradient_to_alpha = 255;
	uint16_t bg_gradient_mid_stop = 500;
	uint16_t bg_gradient_to_stop = 1000;
	uint8_t bg_gradient_has_mid = 0;
	int16_t bg_gradient_angle = 1800;
	uint8_t bg_overlay_gradient = 0;
	style_color_t bg_overlay_gradient_from_color = 0;
	style_color_t bg_overlay_gradient_mid_color = 0;
	style_color_t bg_overlay_gradient_to_color = 0;
	uint8_t bg_overlay_gradient_from_alpha = 255;
	uint8_t bg_overlay_gradient_mid_alpha = 255;
	uint8_t bg_overlay_gradient_to_alpha = 255;
	uint16_t bg_overlay_gradient_mid_stop = 500;
	uint16_t bg_overlay_gradient_to_stop = 1000;
	uint8_t bg_overlay_gradient_has_mid = 0;
	int16_t bg_overlay_gradient_angle = 1800;
	uint8_t bg_radial_gradient = 0;
	style_color_t bg_radial_gradient_from_color = 0;
	style_color_t bg_radial_gradient_to_color = 0;
	uint8_t bg_radial_gradient_from_alpha = 255;
	uint8_t bg_radial_gradient_to_alpha = 255;
	uint16_t bg_radial_gradient_stop = 1000;
	int16_t bg_radial_gradient_cx = 500;
	int16_t bg_radial_gradient_cy = 500;
	int16_t bg_radial_gradient_rx = 1000;
	int16_t bg_radial_gradient_ry = 1000;
	uint8_t bg_grid_axes = 0;
	style_color_t bg_grid_color = 0;
	uint8_t bg_grid_alpha = 255;
	uint16_t bg_grid_step_x = 0;
	uint16_t bg_grid_step_y = 0;
	uint8_t bg_grid_line_x = 0;
		uint8_t bg_grid_line_y = 0;
	};

// Individual translation is applied outside the transform list. Its translation
// therefore adds to the list translation, while authored values stay independent.
inline bool hasIndividualLinearTransform(const RareStyle &s)
{
	return ((s.rotate_angle % 3600) != 0 && (s.rotate_axis_x || s.rotate_axis_y || s.rotate_axis_z)) ||
	       s.scale_x != 1000 || s.scale_y != 1000 || s.scale_z != 1000;
}
inline int composedTranslateX(const RareStyle &s) { return int(s.transform_translate_x) + s.translate_x; }
inline int composedTranslateY(const RareStyle &s) { return int(s.transform_translate_y) + s.translate_y; }
inline int composedTranslateZ(const RareStyle &s) { return int(s.transform_translate_z) + s.translate_z; }
inline int composedTranslateXPercent(const RareStyle &s) { return int(s.transform_translate_x_percent) + s.translate_x_percent; }
inline int composedTranslateYPercent(const RareStyle &s) { return int(s.transform_translate_y_percent) + s.translate_y_percent; }

struct ComputedStyle {
	int8_t display;
	// True iff the `display` property was explicitly authored (a CSS rule or
	// inline style set it), as opposed to the kDisplayBlock default. Lets the
	// inline-formatting heuristic distinguish an explicit `display:block` on an
	// inline-level tag (e.g. <span style="display:block">, which is block-level
	// and stacks) from a span's default inline behaviour. Mirrors
	// flex_direction_explicit.
	int8_t display_explicit;
	int8_t flex_direction;
	int8_t flex_direction_explicit;
	int8_t flex_wrap; // low bits: 0 nowrap, 1 wrap, 2 wrap-reverse; bit 2: balance
	int8_t justify_content;
	int8_t align_items;
	int8_t justify_items;
	int8_t align_content;
	int8_t align_self;
	int16_t gap;
	int8_t box_sizing; // 0: content-box (CSS initial), 1: border-box
	int8_t float_side; // 0: none, 1: left, 2: right
	int8_t clear_side; // 0: none, 1: left, 2: right, 3: both
	int8_t writing_mode; // -1: inherit, 0: horizontal-tb, 1: vertical-lr, 2: vertical-rl, 3: sideways-rl, 4: sideways-lr
	int8_t direction; // -1: inherit, 0: ltr, 1: rtl
	uint8_t margin_auto; // TRBL bit mask; numeric margins remain zero for auto
	int16_t row_gap, column_gap; // kUnset falls back to native gap
	int16_t row_gap_percent, column_gap_percent;
	int32_t order; // Stable ordering of flex/grid items; never changes tree order.
	// Inherited unitless line-height number as IEEE float bits; -1 means length/normal.
	// Keep this common inherited value out of the much larger rare-style allocation.
	int32_t line_height_multiplier;

	int32_t width_expression, height_expression;
	int16_t width, height;
	int16_t width_percent, height_percent;
	int16_t min_width, min_height;
	int16_t max_width, max_height;
	int16_t flex;
	int16_t flex_shrink;
	// CSS `flex-basis` as a definite main-axis length in px, or kUnset for `auto`
	// (the default — main size comes from content / flex-grow). When definite, the
	// flex item's main-axis size is set to this basis before grow/shrink, so
	// `flex: 0 0 26px` yields a 26px item instead of stretching to fill or sizing
	// to content. Percentage and `auto` bases resolve to kUnset (content-sized).
	int16_t flex_basis;

	int16_t padding[4];
	int16_t margin[4];

	int8_t position;
	int8_t z_index_auto;
	int16_t pos_offsets[4];
	int16_t pos_offset_percent[4];
	int16_t z_index;

	style_color_t bg_color;
	int8_t has_bg;
	uint8_t bg_alpha;
	int8_t bg_fill;
	// linear/overlay/radial gradients + background-grid moved to RareStyle (rare).
	style_color_t active_bg_color;
	int8_t has_active_bg;
	style_color_t text_color;
	uint8_t text_alpha;
	uint8_t opacity;
	int16_t blink_interval_ms;
	int32_t blink_started_ms;
	uint8_t blink_visible;

	int16_t border_width;
	style_color_t border_color;
	uint8_t border_alpha;
	// per-side border width/color/alpha moved to RareStyle (rare); border_radius
	// stays here (rounded rects are common).
	int16_t border_radius[4];
	int16_t border_radius_percent[4];

	// transform / perspective / filter / box-shadow moved to RareStyle (rare).

	int16_t font_id;
	int16_t font_size;
	int16_t font_weight;
	int16_t line_height;
	int8_t text_align;
	int8_t overflow;
	int8_t overflow_x;
	int8_t overflow_y;
	int16_t mask_right_fade_width;
	int8_t image_fit;
	// CSS `backface-visibility`: 0 = visible (default), 1 = hidden. When hidden, a
	// flat element's entire painted subtree is culled when its back faces the viewer.
	// A preserve-3d element keeps its descendants' individual visibility.
	int8_t backface_hidden;
	int8_t visibility; // 0 visible, 1 hidden, 2 collapse (inherited)
	// CSS `text-decoration` — 0 = none (default), 1 = underline, 2 =
	// line-through. The text renderer paints a font-size-relative line at
	// the appropriate y-offset for each decorated run.
	int8_t text_decoration;
	// CSS `text-transform` — 0 = none, 1 = uppercase, 2 = lowercase,
	// 3 = capitalize. Rendering applies this without mutating Node::text.
	int8_t text_transform;
	// CSS `white-space`: 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line,
	// 5 break-spaces. Inherited; space preservation and soft wrapping differ.
	int8_t white_space;
	// CSS `text-overflow` — 0 = clip (default), 1 = ellipsis. Only consulted
	// when the line cannot wrap (white-space: nowrap): an overflowing single
	// line is truncated at a glyph boundary and an ASCII "..." is appended so
	// the visible run fits within the content width. Not inherited.
	int8_t text_overflow;
	// CSS `pointer-events` — 0 = auto (default), 1 = none. A `none` node is never
	// the target of a pointer event and its subtree is skipped in hit-testing, so
	// clicks pass through to whatever is painted behind it (e.g. a decorative
	// overlay image over interactive controls). Default 0 via zero-init.
	int8_t pointer_events;

	// Handle into the shared RareStyle pool for cold/rarely-used style fields
	// (grid tracks, gradients, transforms, shadows, …) that most nodes never set.
	// -1 = none, so a plain styled node pays 2 bytes instead of inlining ~200 B.
	// Read via rstyle(style), write via rstyleMut(style). Aggregate-friendly: this
	// is the only member with a default initializer; the rest stay zero-init.
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	// Compiler-driven hot placement: the rare fields live inline (direct load, same cache
	// line — spine's access pattern), no rstyle() pool indirection per node. rare_style is
	// kept but vestigial (always -1) so pool-handle call sites compile as harmless no-ops;
	// the embedded `rare` is copied/freed with the ComputedStyle by struct assignment.
	RareStyle rare;
	int16_t rare_style = -1;
#else
	int16_t rare_style = -1;
#endif
};

	// (RareStyle is defined above ComputedStyle so it can be embedded inline when
	// GEA_EMBEDDED_RARE_STYLE_INLINE is set; the pool below is used only when it isn't.)
	class RareStylePool {
	public:
		RareStyle &operator[](std::size_t index)
		{
#if GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES > 0
			if (index < usedSram_) return sram_[index];
			return spill_[index - usedSram_];
#else
			return spill_[index];
#endif
		}

		const RareStyle &operator[](std::size_t index) const
		{
#if GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES > 0
			if (index < usedSram_) return sram_[index];
			return spill_[index - usedSram_];
#else
			return spill_[index];
#endif
		}

		std::size_t size() const
		{
#if GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES > 0
			return usedSram_ + spill_.size();
#else
			return spill_.size();
#endif
		}

		void emplace_back()
		{
#if GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES > 0
			if (usedSram_ < static_cast<std::size_t>(GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES)) {
				sram_[usedSram_++] = RareStyle{};
				return;
			}
#endif
			spill_.emplace_back();
		}

		void clear()
		{
#if GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES > 0
			for (std::size_t i = 0; i < usedSram_; ++i) sram_[i] = RareStyle{};
			usedSram_ = 0;
#endif
			spill_.clear();
		}

	private:
#if GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES > 0
		RareStyle sram_[GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES]{};
		std::size_t usedSram_ = 0;
#endif
		std::deque<RareStyle> spill_;
	};

	// RareStyle accessors. rstyle() returns a read-only view (a shared zero default
// when the node has no RareStyle), so reads work from a const ComputedStyle&
// with no node id. rstyleMut() allocates a pool entry on first write. The
// compiler flags every write site (can't assign to the const rstyle() result).
	// The shared cold-style pool. Defined inline HERE (not out-of-line in style.cpp) so
	// the hot read path rstyle() inlines to a branch + pool index at every call site.
// An out-of-line call per per-frame style-field read (transform projection, reproject
// patch, dirty scan) was a measured regression on the spinning css-3d-cube: the
// RareStyle split moved transform/gradient fields off the hot ComputedStyle struct, so
// every `style.transform_*` read became a function call. The static local is one shared
// instance across TUs (inline-function rule); rstyleMut/release/reset (style.cpp) use it.
#if GEA_EMBEDDED_RARE_STYLE_INLINE
// Inline-embedded rare fields: rstyle() is a direct member access — no pool, no branch, no
// separate cache line (spine's access pattern). rstyleMut/release/reset are still defined
// out-of-line in style.cpp (their bodies #if'd) because rstyleMut also invalidates the
// transform-scan cache, which this header can't reach.
inline const RareStyle &rstyle(const ComputedStyle &style) { return style.rare; }
#else
inline RareStylePool &rareStylePool()
{
	static RareStylePool pool;
	return pool;
}
// Shared zero default for nodes with no RareStyle. Namespace-scope inline (C++17)
// so the hot rstyle() read has no function-local-static init guard to check per call.
inline const RareStyle kRareStyleZero{};
inline const RareStyle &rstyle(const ComputedStyle &style)
{
	if (style.rare_style < 0) return kRareStyleZero;
	return rareStylePool()[static_cast<std::size_t>(style.rare_style)];
}
#endif
RareStyle &rstyleMut(ComputedStyle &style);
void releaseRareStyle(int16_t &handle);  // frees the entry (or no-op when embedded)
void resetRareStylePool();               // drops all entries (or no-op when embedded)

inline bool styleHasBackgroundImage(const ComputedStyle &style)
{
	const auto &rare = rstyle(style);
	return style.bg_fill != 0 || rare.bg_radial_gradient || rare.bg_overlay_gradient || rare.bg_grid_axes;
}

// Authored inheritance stays in the property override; ComputedStyle always
// contains resolved device-pixel widths, so descendants never re-evaluate em.
inline constexpr int kInheritedBorderWidth = -1;
inline int computedBorderWidth(const ComputedStyle &style, int side)
{
	const int specific = rstyle(style).border_side_width[side];
	return specific > style.border_width ? specific : style.border_width;
}

inline bool setComputedBorderWidth(ComputedStyle &style, int side, int value, const ComputedStyle *parent)
{
	int widths[4];
	for (int i = 0; i < 4; ++i) {
		widths[i] = computedBorderWidth(style, i);
		if (side < 0 || side == i)
			widths[i] = value == kInheritedBorderWidth ? (parent ? computedBorderWidth(*parent, i) : 0) : value;
	}
	const bool uniform = side < 0 && widths[0] == widths[1] && widths[0] == widths[2] && widths[0] == widths[3];
	// A side can override a wider common border with a narrower or zero edge.
	// Materialize the other sides before clearing that common width.
	const int common = side < 0 ? (uniform ? widths[0] : 0) : (widths[side] < style.border_width ? 0 : style.border_width);
	bool changed = common != style.border_width;
	int stored[4];
	for (int i = 0; i < 4; ++i) {
		stored[i] = side < 0 ? (uniform ? 0 : widths[i]) : (common != style.border_width || side == i ? widths[i] : rstyle(style).border_side_width[i]);
		changed |= stored[i] != rstyle(style).border_side_width[i];
	}
	if (!changed) return false;
	style.border_width = common;
	bool needsRare = false;
	for (int i = 0; i < 4; ++i) needsRare |= stored[i] != rstyle(style).border_side_width[i];
	if (needsRare) {
		auto &rare = rstyleMut(style);
		for (int i = 0; i < 4; ++i) rare.border_side_width[i] = stored[i];
	}
	return true;
}

inline bool setBorderColorBinding(ComputedStyle &style, int side, bool current)
{
	const unsigned previous = rstyle(style).border_color_flags;
	unsigned next;
	if (side < 0) next = current ? 0u : 16u; // shorthand resets every side
	else {
		next = previous | (1u << side);
		if (current) next |= 1u << (side + 5);
		else next &= ~(1u << (side + 5));
	}
	if (next == previous) return false;
	rstyleMut(style).border_color_flags = static_cast<uint16_t>(next);
	return true;
}

inline bool borderColorIsCurrent(const ComputedStyle &style, int side = -1)
{
	const unsigned flags = rstyle(style).border_color_flags;
	if (side >= 0 && (flags & (1u << side))) return flags & (1u << (side + 5));
	return !(flags & 16u);
}

inline style_color_t borderPaintColor(const ComputedStyle &style, int side = -1)
{
	if (borderColorIsCurrent(style, side)) return style.text_color;
	return side >= 0 && (rstyle(style).border_color_flags & (1u << side))
	    ? rstyle(style).border_side_color[side] : style.border_color;
}

inline uint8_t borderPaintAlpha(const ComputedStyle &style, int side = -1)
{
	if (borderColorIsCurrent(style, side)) return style.text_alpha;
	return side >= 0 && (rstyle(style).border_color_flags & (1u << side))
	    ? rstyle(style).border_side_alpha[side] : style.border_alpha;
}

inline bool borderColorsDiffer(const ComputedStyle &style)
{
	for (int side = 1; side < 4; ++side)
		if (borderPaintColor(style, side) != borderPaintColor(style, 0) ||
		    borderPaintAlpha(style, side) != borderPaintAlpha(style, 0)) return true;
	return false;
}

inline bool borderUsesCurrentColor(const ComputedStyle &style)
{
	for (int side = 0; side < 4; ++side)
		if ((style.border_width > 0 || rstyle(style).border_side_width[side] > 0) && borderColorIsCurrent(style, side)) return true;
	return false;
}

// Layout boxes include padding and borders. CSS width/height can select either
// the content box or border box, but child origins always use these insets.
inline int boxInset(const ComputedStyle &style, int side)
{
	const int sideWidth = rstyle(style).border_side_width[side];
	return style.padding[side] + (sideWidth > style.border_width ? sideWidth : style.border_width);
}
inline int boxInsets(const ComputedStyle &style, bool horizontal)
{
	return horizontal ? boxInset(style, 1) + boxInset(style, 3) : boxInset(style, 0) + boxInset(style, 2);
}
inline int contentSizeToBorderSize(const ComputedStyle &style, int size, bool horizontal)
{
	const int insets = boxInsets(style, horizontal);
	const int result = size + (style.box_sizing == 0 ? insets : 0);
	return result < insets ? insets : result;
}

inline int clampBorderBoxSize(const ComputedStyle &style, int size, bool horizontal)
{
	const int minSize = horizontal ? style.min_width : style.min_height;
	const int maxSize = horizontal ? style.max_width : style.max_height;
	if (maxSize != kUnset) {
		const int limit = contentSizeToBorderSize(style, maxSize, horizontal);
		if (size > limit) size = limit;
	}
	const int floor = contentSizeToBorderSize(style, minSize == kUnset ? 0 : minSize, horizontal);
	return size < floor ? floor : size;
}

inline bool isDisplayNone(const ComputedStyle &style)
{
	return style.display == kDisplayNone;
}

inline bool isDisplayGrid(const ComputedStyle &style)
{
	return style.display == kDisplayGrid;
}

inline bool usesRowLayout(const ComputedStyle &style)
{
	// `flex-direction` applies to FLEX CONTAINERS, and to nothing else --
	// CSS Flexbox Level 1 SS5.1 states its "Applies to:" as exactly that. On a
	// block container the property still computes, but it has no effect: block
	// boxes stack in the block direction whatever it says. A browser lays
	// `<div style="flex-direction: row">` out as a column, and so does this.
	//
	// Non-grid/non-none nodes (block included) all run through FlexLayoutPass
	// (LayoutEngine::layoutChildren), which is an implementation detail of how
	// block flow is computed here, not a licence to read a flex property on a
	// box that is not a flex container. Honouring it regardless of `display`
	// meant seven authored sites got a row a browser would never give them;
	// each now says `display: flex`, which is what it always meant.
	if (style.display != kDisplayFlex) return false;
	// Within a flex container the property means what it says, and its initial
	// value is `row` -- so an unstated direction on `display: flex` is a row.
	return style.flex_direction_explicit ? style.flex_direction == 1 : true;
}

// Keep specified axes separate: changing the other axis must undo CSS's
// visible->auto / clip->hidden computed-value conversion.
inline bool isScrollableOverflow(int value) { return value == 1 || value == 2; }
inline int usedOverflow(int axis, int other)
{
	return isScrollableOverflow(other) ? (axis == 0 ? 2 : axis == 3 ? 1 : axis) : axis;
}
inline int overflowX(const ComputedStyle &style) { return usedOverflow(style.overflow_x, style.overflow_y); }
inline int overflowY(const ComputedStyle &style) { return usedOverflow(style.overflow_y, style.overflow_x); }
inline int8_t aggregateOverflow(int8_t overflowX, int8_t overflowY)
{
	const int x = usedOverflow(overflowX, overflowY), y = usedOverflow(overflowY, overflowX);
	if (x == 2 || y == 2) return 2;
	if (x == 1 || y == 1) return 1;
	return x == 3 || y == 3 ? 3 : 0;
}
inline bool overflowEstablishesContext(const ComputedStyle &style)
{
	return isScrollableOverflow(overflowX(style)) || isScrollableOverflow(overflowY(style));
}
// Grouping effects force the used transform-style to flat.
inline bool preserves3D(const ComputedStyle &style)
{
	const auto &rare = rstyle(style);
	return rare.transform_preserve_3d && style.opacity == 255 &&
	    !isScrollableOverflow(overflowX(style)) && !isScrollableOverflow(overflowY(style)) &&
	    !rare.filter_present && !rare.filter_blur_radius && !(rare.containment & 16) &&
	    !style.mask_right_fade_width;
}
inline bool scrollsOverflowX(const ComputedStyle &style) { return overflowX(style) == 2; }
inline bool scrollsOverflowY(const ComputedStyle &style) { return overflowY(style) == 2; }

inline bool hasSideBorder(const ComputedStyle &style)
{
	if (style.rare_style < 0) return false;  // no RareStyle => no per-side borders
	const RareStyle &r = rstyle(style);
	return r.border_side_width[0] > 0 ||
	       r.border_side_width[1] > 0 ||
	       r.border_side_width[2] > 0 ||
	       r.border_side_width[3] > 0;
}

inline bool hasAnyBorder(const ComputedStyle &style)
{
	return style.border_width > 0 || hasSideBorder(style);
}

inline bool hasBorderRelief(const ComputedStyle &style)
{
	const auto &r = rstyle(style);
	return r.border_relief[0] || r.border_relief[1] || r.border_relief[2] || r.border_relief[3];
}

struct LayoutBox {
	int16_t x, y;
	int32_t width_expression, height_expression;
	int16_t width, height;

	// Inline formatting: x offset, inside this box's content area, at which the
	// run's FIRST line starts. A text run that begins part-way along a line box
	// (after a <strong>, say) and wraps keeps a full-content-width box so its
	// continuation lines are wrapped and drawn at the block's left edge, while
	// its first line is indented to the pen position it inherited. Zero for
	// every box that starts its own line, which is every box outside an inline
	// formatting row.
	int16_t inline_indent = 0;
	// The hypothetical block-start margin edge of an out-of-flow child,
	// relative to its static-position parent. Capture before relative offsets
	// and absolute-coordinate conversion; retained refresh reuses this anchor.
	int16_t static_block_start = 0;
	uint8_t static_block_axis = 0;  // 0: unavailable, 1: y, 2: x

	int16_t previous_x, previous_y;
	int16_t previous_width, previous_height;

	// Scroll geometry must be 32-bit: a <virtual-list> scrolls over a virtual
	// content height of itemCount * rowHeight (e.g. 5000 * 259 ≈ 1.29M px),
	// which overflows int16_t. Truncation made scroll_content_height wrap
	// negative, so scrollMaxY clamped to 0 and the list refused to scroll.
	int32_t scroll_x;
	int32_t scroll_y;
	int32_t scroll_content_width;
	int32_t scroll_content_height;
	int32_t previous_scroll_x;
	int32_t previous_scroll_y;

	// Intra-pass layout memo: the flex engine re-measures whole subtrees after
	// every parent resize (repositionChildren -> measureChildren -> layoutNode),
	// visiting nodes ~100x per pass on nested-flex trees. layoutNode() skips a
	// node already laid out THIS pass with the same available box. Two slots
	// (MRU order) because flex items alternate between the parent's measure
	// avail and their basis/grown avail — a single slot thrashes on exactly
	// that pattern. Scoped to a single pass (serial bumped by beginLayoutPass),
	// so no cross-frame invalidation is needed and the final
	// absolute-coordinate resolve is unaffected.
	// result_w/h: the node dims each slot's layout produced. The MRU slot's
	// result is the node's live state, so its hit is unconditional; the older
	// slot's layout has been overwritten in the tree, so its hit is only sound
	// when it produced exactly the current dims (the subtree is then
	// byte-identical — child avails derive from the node's own box).
	int16_t memo_avail_w;
	int16_t memo_avail_h;
	int16_t memo_result_w;
	int16_t memo_result_h;
	uint32_t memo_pass;
	int16_t memo2_avail_w;
	int16_t memo2_avail_h;
	int16_t memo2_result_w;
	int16_t memo2_result_h;
	uint32_t memo2_pass;
};

struct RenderState {
	int16_t previous_rotate_angle;
	int32_t previous_rotate_axis_x, previous_rotate_axis_y, previous_rotate_axis_z;
	int16_t previous_scale_x, previous_scale_y, previous_scale_z;
	int16_t previous_transform_rotate;
	int16_t previous_transform_rotate_x;
	int16_t previous_transform_rotate_y;
	int16_t previous_translate_x, previous_translate_y, previous_translate_z;
	int16_t previous_translate_x_percent, previous_translate_y_percent;
	uint8_t previous_transform_translate_outer_axes;
	int32_t previous_transform_translate_x;
	int32_t previous_transform_translate_y;
	int32_t previous_transform_translate_z;
	int32_t previous_transform_translate_x_percent;
	int32_t previous_transform_translate_y_percent;
	int16_t previous_transform_scale_x;
	int16_t previous_transform_scale_y;
	int16_t previous_transform_scale_z;
	int16_t previous_transform_origin_x;
	int16_t previous_transform_origin_y;
	int16_t previous_perspective;
	int16_t previous_perspective_origin_x;
	int16_t previous_perspective_origin_y;
	uint8_t previous_transformable_box;
	int16_t previous_filter_blur_radius;

	uint8_t dirty;
	uint8_t scroll_dirty;
	uint8_t non_scroll_dirty;
	// Set alongside `dirty` when the change can affect GEOMETRY (layout
	// property, box-changing text, structural mutation). Paint-only dirt (a
	// background/color class toggle, stable-box text) leaves it clear, letting
	// the refresh skip the relayout pass entirely and letting the scoped
	// relayout compute its LCA over geometry-relevant nodes only. Cleared with
	// `dirty`.
	uint8_t layout_dirty;
	uint8_t transform_dirty;
	uint8_t bg_recolor_pending;
	style_color_t bg_recolor_from;
	style_color_t bg_recolor_to;
	// Tree::setText pre-measures incoming text and sets this flag when the
	// update is safe for the retained/direct-replay path: absolute text may
	// update its measured bbox out of flow, while in-flow text must keep the
	// same layout box (e.g. fixed-width temperature readouts). Cleared on
	// LayoutSnapshot::capture() so it's always either freshly set by setText
	// in the current frame or absent.
	uint8_t text_layout_stable;
	// Glyph-incremental text dirty (set by Tree::setText): when a content-only text
	// change keeps the same layout box and only a middle run of glyphs differs
	// (e.g. a ticking counter "58"->"57", a clock), the dirty collector flushes only
	// [text_dirty_x0, text_dirty_x1] across the node's height instead of the whole
	// box. A 26vmin badge re-render then copies just the changed digits (~one glyph)
	// rather than ~the whole panel width, so it fits a TE-locked frame's slack.
	// Cleared with text_layout_stable each frame (LayoutSnapshot::capture).
	uint8_t text_partial_dirty;
	int16_t text_dirty_x0;
	int16_t text_dirty_x1;
	// This box SHARES a line box with other inline content, so it was placed on
	// that line's baseline (FlexLayoutPass::placeLineItems). TextRenderer::record
	// then draws the run at its baseline instead of centring the string's ink in
	// the line advance: ink centring shifts by however far that particular string's
	// glyphs reach, so runs of different words — or different fonts — on one line
	// end up at different heights. A run that owns its line keeps the centring.
	uint8_t inline_baseline;
};

inline bool hadIndividualLinearTransform(const RenderState &s)
{
	return ((s.previous_rotate_angle % 3600) != 0 && (s.previous_rotate_axis_x || s.previous_rotate_axis_y || s.previous_rotate_axis_z)) ||
	       s.previous_scale_x != 1000 || s.previous_scale_y != 1000 || s.previous_scale_z != 1000;
}

struct Node {
	NodeType type;
	ComputedStyle style;

	int16_t parent;
	int16_t first_child, last_child;
	int16_t next_sibling, prev_sibling;

	// Text content. std::string gives us dynamic sizing without the silent
	// truncation a fixed buffer caused, and on every modern libstdc++ /
	// libc++ build it has small-string optimization — strings up to ~22
	// chars stay inline in the std::string header with no heap allocation,
	// so the common short-label case (button text, counter values) doesn't
	// pay an allocation cost. Apps that put paragraph copy in a single
	// text node (e.g. typography sampler, ~160 chars) get the heap
	// allocation they actually need rather than being chopped to 63 chars.
	std::string text;
	int16_t image_id;
	// Interned tag id (index into the global tag table) rather than a 16-byte
	// inline string — the tag vocabulary is ~15 distinct strings, so this saves
	// 14 B/node. 0 = the empty tag. Use Tree::tagName(node) / tagFromId(tag_id)
	// to read the string, internTag(...) to write.
	int16_t tag_id = 0;

	LayoutBox layout;
	RenderState render;

	// Blink-style "rare data" handle: index into the shared NodeRareData pool for
	// the cold/optional per-node state most nodes never have (event listeners, and
	// later attributes/custom-properties/etc.). -1 = none, so the common node pays
	// 2 bytes instead of inlining those tables. Reset to -1 by Node{} / node reuse.
	int16_t rare_data = -1;
};

// Overflow clips the padding edge on each non-visible axis. Open axes span
// the positive display coordinate range and remain stationary during replay.
inline void overflowClipBounds(const Node &node, int &x, int &y, int &w, int &h)
{
	const auto &s = node.style;
	const int left = boxInset(s, 3) - s.padding[3], right = boxInset(s, 1) - s.padding[1];
	const int top = boxInset(s, 0) - s.padding[0], bottom = boxInset(s, 2) - s.padding[2];
	x = overflowX(s) ? node.layout.x + left : 0;
	y = overflowY(s) ? node.layout.y + top : 0;
	w = overflowX(s) ? std::max(0, node.layout.width - left - right) : 32767;
	h = overflowY(s) ? std::max(0, node.layout.height - top - bottom) : 32767;
}

}  // namespace gea::embedded::ui
