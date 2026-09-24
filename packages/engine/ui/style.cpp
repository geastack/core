// SPDX-License-Identifier: Apache-2.0
#include "style.h"
#include "state_init.h"

#include "css_atom.h"
#include "graphics/font.h"
#include "node.h"
#include "pixel.h"
#include "style_values.h"
#include "tree_state.h"
#include "tree_internal.h"
#include "internal.h"
#include "css/engine.h"
#include "refresh_perf.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#define GEA_STYLE_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#define GEA_STYLE_LOGW(tag, fmt, ...) std::fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

// Per-recompute-batch profiling that decomposes a recompute into per-node/per-parser
// sub-phases (dumped by endStyleMountBatch when a batch exceeds 50ms). The timers sit
// on hot paths (parseLengthForNode, applyPropertyWithSource, ...) so it's off by
// default; flip to 1 (ESP only — it uses esp_timer) to investigate recompute cost.
#ifndef GEA_RECPROF
#define GEA_RECPROF 0
#endif

// When 1, endStyleMountBatch runs the incremental recompute, then re-runs the full
// recompute and asserts every node's computed style is byte-identical — proving the
// incremental result on-device (where visual inspection over serial isn't possible).
// Costs the full recompute too, so it's a verification build, not a perf build.
#ifndef GEA_INCREMENTAL_VERIFY
#define GEA_INCREMENTAL_VERIFY 0
#endif

#ifndef GEA_CSS_CACHED_STYLE_APPLY_INLINE_OPS
#define GEA_CSS_CACHED_STYLE_APPLY_INLINE_OPS 64
#endif

#ifndef GEA_CSS_ACTIVE_RULE_PLAN_INLINE_CACHE_ENTRIES
#define GEA_CSS_ACTIVE_RULE_PLAN_INLINE_CACHE_ENTRIES 8
#endif

#ifndef GEA_CSS_RULE_CANDIDATE_INLINE_RULES
#define GEA_CSS_RULE_CANDIDATE_INLINE_RULES 32
#endif

#ifndef GEA_CSS_RULE_CANDIDATE_INLINE_CACHE_ENTRIES
#define GEA_CSS_RULE_CANDIDATE_INLINE_CACHE_ENTRIES 16
#endif

namespace gea::embedded::ui {

namespace {

#if GEA_RECPROF
// TEMP per-recompute-batch profiling: decompose a city/theme switch's ~400ms
// recompute into its per-node sub-phases. Reset + dumped by endStyleMountBatch.
int64_t g_profResetUs = 0, g_profCandUs = 0, g_profApplyUs = 0, g_profVarUs = 0,
        g_profMiscUs = 0, g_profPseudoUs = 0, g_profBodyUs = 0, g_profBgUs = 0, g_profXformUs = 0,
        g_profLenUs = 0, g_profColorUs = 0;
int g_profNodes = 0, g_profApplyCalls = 0, g_profBgCalls = 0, g_profXformCalls = 0,
    g_profLenCalls = 0, g_profColorCalls = 0;
inline int64_t recNow() { return gea::embedded::ui::refreshPerfNowUs(); }
// Last dump line, queryable after the fact (the mount-batch dump fires before a
// USB console can attach on embedded targets). See gea_recprof_last().
char g_recprofLast[420];
#endif

// --- Incremental class-style recompute: dependency tracking ---------------
// During a node's class-style recompute we record every custom-property name it
// resolves (via lookupCustomProperty, which is reached for var() in any property,
// including the node's pseudo-elements and transitively through var()-valued
// custom properties). On a later recompute triggered by a near-root class change
// (e.g. a weather theme switch flipping is-cloud->is-rain, which only mutates the
// shell's --base-*/--accent custom props), endStyleMountBatch walks the subtree and
// recomputes a node only if it was directly mutated, inherits a changed value, or
// references a custom property whose value changed. Nodes that depend on none of
// those keep their existing computed style (identical to a full recompute). A
// GEA_INCREMENTAL_VERIFY build additionally runs the full recompute afterwards and
// asserts every node's style matches, proving equivalence on-device.
std::vector<CssAtomId> g_nodeRefOverflow;

struct CssAtomSmallList {
	static constexpr std::uint8_t kInlineCount = 16;
	CssAtomId inlineValues[kInlineCount]{};
	std::vector<CssAtomId> spillValues;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	std::size_t size() const { return spilled ? spillValues.size() : inlineCount; }
	bool empty() const { return size() == 0; }

	CssAtomId at(std::size_t index) const
	{
		return spilled ? spillValues[index] : inlineValues[index];
	}

	bool contains(CssAtomId value) const
	{
		for (std::size_t i = 0, n = size(); i < n; ++i)
			if (at(i) == value) return true;
		return false;
	}

	void insertUnique(CssAtomId value)
	{
		if (value == kInvalidCssAtom || contains(value)) return;
		if (!spilled && inlineCount < kInlineCount) {
			inlineValues[inlineCount++] = value;
			return;
		}
		if (!spilled) {
			spillValues.assign(inlineValues, inlineValues + inlineCount);
			spilled = true;
		}
		spillValues.push_back(value);
	}

	void erase(CssAtomId value)
	{
		if (spilled) {
			for (auto it = spillValues.begin(); it != spillValues.end(); ++it)
				if (*it == value) {
					spillValues.erase(it);
					return;
				}
			return;
		}
		for (std::uint8_t i = 0; i < inlineCount; ++i) {
			if (inlineValues[i] != value) continue;
			for (std::uint8_t j = i + 1; j < inlineCount; ++j)
				inlineValues[j - 1] = inlineValues[j];
			--inlineCount;
			return;
		}
	}
};

struct NodeCustomPropRefs {
	static constexpr std::uint8_t kInlineCount = 4;
	static constexpr std::uint16_t kNoOverflow = 0xFFFFu;
	CssAtomId inlineRefs[kInlineCount]{};
	std::uint16_t overflowStart = kNoOverflow;
	std::uint16_t overflowCount = 0;
	std::uint8_t count = 0;
	bool tracked = false;

#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	// Runtime initialization keeps the overflow sentinel out of .data (see
	// state_init.h).
	NodeCustomPropRefs() { reset(); }
#endif

	void clearForRecompute()
	{
		tracked = true;
		count = 0;
		overflowStart = kNoOverflow;
		overflowCount = 0;
	}

	void reset()
	{
		tracked = false;
		count = 0;
		overflowStart = kNoOverflow;
		overflowCount = 0;
	}

	bool contains(CssAtomId name) const
	{
		for (std::uint8_t i = 0; i < count; ++i)
			if (inlineRefs[i] == name) return true;
		if (overflowStart != kNoOverflow) {
			const std::size_t start = overflowStart;
			const std::size_t end = start + overflowCount;
			for (std::size_t i = start; i < end && i < g_nodeRefOverflow.size(); ++i)
				if (g_nodeRefOverflow[i] == name) return true;
		}
		return false;
	}

	void insert(CssAtomId name)
	{
		if (name == kInvalidCssAtom || contains(name)) return;
		if (count < kInlineCount) {
			inlineRefs[count++] = name;
			return;
		}
		if (overflowStart == kNoOverflow) {
			const std::size_t start = g_nodeRefOverflow.size();
			if (start >= kNoOverflow) return;
			overflowStart = static_cast<std::uint16_t>(start);
			overflowCount = 0;
		}
		g_nodeRefOverflow.push_back(name);
		++overflowCount;
	}

	bool touches(const CssAtomSmallList &changed) const
	{
		if (!tracked || changed.empty()) return false;
		for (std::uint8_t i = 0; i < count; ++i)
			for (std::size_t c = 0, n = changed.size(); c < n; ++c) {
				const CssAtomId changedName = changed.at(c);
				if (changedName == inlineRefs[i]) return true;
			}
		if (overflowStart != kNoOverflow) {
			const std::size_t start = overflowStart;
			const std::size_t end = start + overflowCount;
			for (std::size_t i = start; i < end && i < g_nodeRefOverflow.size(); ++i)
				for (std::size_t c = 0, n = changed.size(); c < n; ++c) {
					const CssAtomId changedName = changed.at(c);
					if (changedName == g_nodeRefOverflow[i]) return true;
				}
		}
		return false;
	}

	std::string debugString() const
	{
		std::string out;
		for (std::uint8_t i = 0; i < count; ++i) {
			out += cssAtomText(inlineRefs[i]);
			out.push_back(' ');
		}
		if (overflowStart != kNoOverflow) {
			const std::size_t start = overflowStart;
			const std::size_t end = start + overflowCount;
			for (std::size_t i = start; i < end && i < g_nodeRefOverflow.size(); ++i) {
				out += cssAtomText(g_nodeRefOverflow[i]);
				out.push_back(' ');
			}
		}
		return out;
	}
};

NodeCustomPropRefs g_nodeRefs[kMaxNodes];
int g_recordingNode = -1;
struct CustomPropertyLookupCache {
	static constexpr std::uint8_t kCapacity = 16;
	int nodeIds[kCapacity]{};
	CssAtomId names[kCapacity]{};
	const NodeCustomProperty *entries[kCapacity]{};
	std::uint8_t hits[kCapacity]{};
	std::uint8_t count = 0;

	void clear()
	{
		count = 0;
	}

	bool lookup(int nodeId, CssAtomId name, const NodeCustomProperty *&out) const
	{
		for (std::uint8_t i = 0; i < count; ++i) {
			if (nodeIds[i] != nodeId || names[i] != name) continue;
			out = hits[i] ? entries[i] : nullptr;
			return true;
		}
		return false;
	}

	void store(int nodeId, CssAtomId name, const NodeCustomProperty *entry)
	{
		if (name == kInvalidCssAtom) return;
		for (std::uint8_t i = 0; i < count; ++i) {
			if (nodeIds[i] != nodeId || names[i] != name) continue;
			entries[i] = entry;
			hits[i] = entry ? 1 : 0;
			return;
		}
		if (count >= kCapacity) return;
		nodeIds[count] = nodeId;
		names[count] = name;
		entries[count] = entry;
		hits[count] = entry ? 1 : 0;
		++count;
	}
};

CustomPropertyLookupCache g_customPropertyLookupCache;

void clearDynamicLengthExpressionResolutionCache();

inline void clearCustomPropertyLookupCache()
{
	g_customPropertyLookupCache.clear();
	clearDynamicLengthExpressionResolutionCache();
}

struct DenseNodeMark {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	std::uint16_t marks[kMaxNodes];
	std::uint16_t serial;

	// Runtime initialization keeps the nonzero serial out of .data (see
	// state_init.h); noinline stops the compiler folding it back in.
	__attribute__((noinline)) DenseNodeMark()
	{
		for (std::uint16_t &mark : marks) mark = 0;
		serial = 1;
	}
#else
	std::uint16_t marks[kMaxNodes]{};
	std::uint16_t serial = 1;
#endif

	void clear()
	{
		++serial;
		if (serial == 0) {
			for (std::uint16_t &mark : marks) mark = 0;
			serial = 1;
		}
	}

	void insert(int node)
	{
		if (node < 0 || node >= kMaxNodes) return;
		marks[node] = serial;
	}

	bool contains(int node) const
	{
		return node >= 0 && node < kMaxNodes && marks[node] == serial;
	}
};

DenseNodeMark g_pendingRecomputeMark;
// Nodes whose class change touched a class/tag used as an ancestor matcher in some
// complex selector (e.g. toggling `.open` where `.open .panel {}` exists) — captured
// at setClassName time so both the OLD and NEW class sets are visible. Their entire
// subtree must be recomputed in full (descendant selector matches may have flipped),
// which a custom-property/inheritance diff alone wouldn't catch. Consumed + cleared
// by endStyleMountBatch.
DenseNodeMark g_forceFullSubtreeMark;
#if GEA_INCREMENTAL_VERIFY
std::unordered_set<int> g_incrRecomputedNodes;  // nodes the incremental pass actually recomputed
#endif

inline void recordCustomPropRef(CssAtomId name)
{
	if (g_recordingNode < 0 || g_recordingNode >= kMaxNodes || name == kInvalidCssAtom) return;
	g_nodeRefs[g_recordingNode].insert(name);
}

bool nodeParticipatesInMountedTree(const TreeState &state, int node)
{
	const int root = Tree::instance().mountedRoot();
	if (root < 0) return false;
	for (int current = node; current >= 0 && current < state.nodeCount; current = state.nodes[current].parent) {
		if (current == root) return true;
	}
	return false;
}

bool styleEqualExceptTextPaint(const ComputedStyle &a, const ComputedStyle &b)
{
	const RareStyle &ar = rstyle(a);
	const RareStyle &br = rstyle(b);
	if (a.visibility != b.visibility || a.display != b.display ||
	    a.flex_direction != b.flex_direction ||
	    a.flex_direction_explicit != b.flex_direction_explicit ||
	    a.display_explicit != b.display_explicit ||
	    a.box_sizing != b.box_sizing ||
	    a.float_side != b.float_side ||
	    a.clear_side != b.clear_side ||
	    ar.margin_trim != br.margin_trim ||
	    a.writing_mode != b.writing_mode ||
	    a.direction != b.direction ||
	    a.row_gap != b.row_gap ||
	    a.column_gap != b.column_gap ||
	    a.row_gap_percent != b.row_gap_percent ||
	    a.column_gap_percent != b.column_gap_percent ||
	    a.margin_auto != b.margin_auto ||
	    a.order != b.order ||
	    a.flex_wrap != b.flex_wrap ||
	    a.justify_content != b.justify_content ||
	    a.align_items != b.align_items ||
	    a.justify_items != b.justify_items ||
	    ar.aspect_ratio != br.aspect_ratio ||
	    ar.flex_line_count != br.flex_line_count ||
	    ar.justify_self != br.justify_self ||
	    ar.grid_line[0] != br.grid_line[0] ||
	    ar.grid_line[1] != br.grid_line[1] ||
	    ar.grid_line[2] != br.grid_line[2] ||
	    ar.grid_line[3] != br.grid_line[3] ||
	    a.align_content != b.align_content ||
	    a.align_self != b.align_self ||
	    a.gap != b.gap ||
	    ar.grid_column_count != br.grid_column_count ||
	    ar.grid_row_count != br.grid_row_count) return false;
	for (int i = 0; i < kMaxGridTracks; ++i) {
		if (ar.grid_column_type[i] != br.grid_column_type[i] ||
		    ar.grid_row_type[i] != br.grid_row_type[i] ||
		    ar.grid_column_value[i] != br.grid_column_value[i] ||
		    ar.grid_row_value[i] != br.grid_row_value[i]) return false;
	}
	if (a.width != b.width ||
	    a.height != b.height ||
	    a.width_expression != b.width_expression ||
	    a.height_expression != b.height_expression ||
	    a.width_percent != b.width_percent ||
	    a.height_percent != b.height_percent ||
	    a.min_width != b.min_width ||
	    a.min_height != b.min_height ||
	    a.max_width != b.max_width ||
	    a.max_height != b.max_height ||
	    a.flex != b.flex ||
	    a.flex_shrink != b.flex_shrink ||
	    a.flex_basis != b.flex_basis ||
	    ar.flex_basis_expression != br.flex_basis_expression ||
	    ar.line_height_expression != br.line_height_expression ||
	    a.line_height_multiplier != b.line_height_multiplier) return false;
	for (int i = 0; i < 4; ++i) {
		if (a.padding[i] != b.padding[i] ||
		    a.margin[i] != b.margin[i] ||
		    ar.margin_expression[i] != br.margin_expression[i] ||
		    ar.padding_expression[i] != br.padding_expression[i] ||
		    a.pos_offsets[i] != b.pos_offsets[i] ||
		    a.pos_offset_percent[i] != b.pos_offset_percent[i] ||
		    ar.border_side_width[i] != br.border_side_width[i] ||
		    ar.border_relief[i] != br.border_relief[i] ||
		    ar.border_color_flags != br.border_color_flags ||
		    ar.border_side_color[i] != br.border_side_color[i] ||
		    ar.border_side_alpha[i] != br.border_side_alpha[i] ||
		    a.border_radius[i] != b.border_radius[i] ||
		    a.border_radius_percent[i] != b.border_radius_percent[i]) return false;
	}
	if (a.position != b.position ||
	    a.z_index != b.z_index ||
	    a.z_index_auto != b.z_index_auto ||
	    a.bg_color != b.bg_color ||
	    a.has_bg != b.has_bg ||
	    a.bg_alpha != b.bg_alpha ||
	    a.bg_fill != b.bg_fill ||
	    ar.containment != br.containment ||
	    ar.bg_clip != br.bg_clip ||
	    ar.bg_size_list != br.bg_size_list ||
	    ar.bg_position_list != br.bg_position_list ||
	    ar.bg_repeat_list != br.bg_repeat_list ||
	    ar.bg_attachment_list != br.bg_attachment_list ||
	    ar.bg_origin_list != br.bg_origin_list ||
	    ar.bg_gradient_layer != br.bg_gradient_layer || ar.bg_overlay_gradient_layer != br.bg_overlay_gradient_layer || ar.bg_radial_gradient_layer != br.bg_radial_gradient_layer ||
	    ar.bg_image_layer_count != br.bg_image_layer_count ||
	    ar.bg_gradient_from_color != br.bg_gradient_from_color ||
	    ar.bg_gradient_mid_color != br.bg_gradient_mid_color ||
	    ar.bg_gradient_to_color != br.bg_gradient_to_color ||
	    ar.bg_gradient_from_alpha != br.bg_gradient_from_alpha ||
	    ar.bg_gradient_mid_alpha != br.bg_gradient_mid_alpha ||
	    ar.bg_gradient_to_alpha != br.bg_gradient_to_alpha ||
	    ar.bg_gradient_mid_stop != br.bg_gradient_mid_stop ||
	    ar.bg_gradient_to_stop != br.bg_gradient_to_stop ||
	    ar.bg_gradient_has_mid != br.bg_gradient_has_mid ||
	    ar.bg_gradient_angle != br.bg_gradient_angle ||
	    ar.bg_overlay_gradient != br.bg_overlay_gradient ||
	    ar.bg_overlay_gradient_from_color != br.bg_overlay_gradient_from_color ||
	    ar.bg_overlay_gradient_mid_color != br.bg_overlay_gradient_mid_color ||
	    ar.bg_overlay_gradient_to_color != br.bg_overlay_gradient_to_color ||
	    ar.bg_overlay_gradient_from_alpha != br.bg_overlay_gradient_from_alpha ||
	    ar.bg_overlay_gradient_mid_alpha != br.bg_overlay_gradient_mid_alpha ||
	    ar.bg_overlay_gradient_to_alpha != br.bg_overlay_gradient_to_alpha ||
	    ar.bg_overlay_gradient_mid_stop != br.bg_overlay_gradient_mid_stop ||
	    ar.bg_overlay_gradient_to_stop != br.bg_overlay_gradient_to_stop ||
	    ar.bg_overlay_gradient_has_mid != br.bg_overlay_gradient_has_mid ||
	    ar.bg_overlay_gradient_angle != br.bg_overlay_gradient_angle ||
	    ar.bg_radial_gradient != br.bg_radial_gradient ||
	    ar.bg_radial_gradient_from_color != br.bg_radial_gradient_from_color ||
	    ar.bg_radial_gradient_to_color != br.bg_radial_gradient_to_color ||
	    ar.bg_radial_gradient_from_alpha != br.bg_radial_gradient_from_alpha ||
	    ar.bg_radial_gradient_to_alpha != br.bg_radial_gradient_to_alpha ||
	    ar.bg_radial_gradient_stop != br.bg_radial_gradient_stop ||
	    ar.bg_radial_gradient_cx != br.bg_radial_gradient_cx ||
	    ar.bg_radial_gradient_cy != br.bg_radial_gradient_cy ||
	    ar.bg_radial_gradient_rx != br.bg_radial_gradient_rx ||
	    ar.bg_radial_gradient_ry != br.bg_radial_gradient_ry ||
	    ar.bg_grid_axes != br.bg_grid_axes ||
	    ar.bg_grid_color != br.bg_grid_color ||
	    ar.bg_grid_alpha != br.bg_grid_alpha ||
	    ar.bg_grid_step_x != br.bg_grid_step_x ||
	    ar.bg_grid_step_y != br.bg_grid_step_y ||
	    ar.bg_grid_line_x != br.bg_grid_line_x ||
	    ar.bg_grid_line_y != br.bg_grid_line_y ||
	    a.active_bg_color != b.active_bg_color ||
	    a.has_active_bg != b.has_active_bg ||
	    a.opacity != b.opacity ||
	    a.blink_interval_ms != b.blink_interval_ms ||
	    a.blink_started_ms != b.blink_started_ms ||
	    a.blink_visible != b.blink_visible ||
	    a.border_width != b.border_width ||
	    a.border_color != b.border_color ||
	    a.border_alpha != b.border_alpha ||
	    ar.transform_preserve_3d != br.transform_preserve_3d ||
	    ar.transform_present != br.transform_present ||
	    ar.translate_present != br.translate_present || ar.rotate_present != br.rotate_present ||
	    ar.scale_present != br.scale_present ||
	    ar.filter_present != br.filter_present ||
	    ar.transform_rotate != br.transform_rotate ||
	    ar.transform_translate_outer_axes != br.transform_translate_outer_axes ||
	    ar.rotate_angle != br.rotate_angle ||
	    ar.rotate_axis_x != br.rotate_axis_x ||
	    ar.rotate_axis_y != br.rotate_axis_y ||
	    ar.rotate_axis_z != br.rotate_axis_z ||
	    ar.scale_x != br.scale_x ||
	    ar.scale_y != br.scale_y ||
	    ar.scale_z != br.scale_z ||
	    ar.transform_rotate_x != br.transform_rotate_x ||
	    ar.transform_rotate_y != br.transform_rotate_y ||
	    ar.translate_x != br.translate_x ||
	    ar.transform_translate_x != br.transform_translate_x ||
	    ar.translate_y != br.translate_y ||
	    ar.transform_translate_y != br.transform_translate_y ||
	    ar.translate_z != br.translate_z ||
	    ar.transform_translate_z != br.transform_translate_z ||
	    ar.translate_x_percent != br.translate_x_percent ||
	    ar.transform_translate_x_percent != br.transform_translate_x_percent ||
	    ar.translate_y_percent != br.translate_y_percent ||
	    ar.transform_translate_y_percent != br.transform_translate_y_percent ||
	    ar.transform_scale_x != br.transform_scale_x ||
	    ar.transform_scale_y != br.transform_scale_y ||
	    ar.transform_scale_z != br.transform_scale_z ||
	    ar.transform_origin_x != br.transform_origin_x ||
	    ar.transform_origin_y != br.transform_origin_y ||
	    ar.perspective != br.perspective ||
	    ar.perspective_origin_x != br.perspective_origin_x ||
	    ar.perspective_origin_y != br.perspective_origin_y ||
	    ar.filter_blur_radius != br.filter_blur_radius ||
	    ar.box_shadow_inset != br.box_shadow_inset ||
	    ar.box_shadow_offset_x != br.box_shadow_offset_x ||
	    ar.box_shadow_offset_y != br.box_shadow_offset_y ||
	    ar.box_shadow_blur_radius != br.box_shadow_blur_radius ||
	    ar.box_shadow_spread != br.box_shadow_spread ||
	    ar.box_shadow_color != br.box_shadow_color ||
	    ar.box_shadow_alpha != br.box_shadow_alpha ||
	    a.font_id != b.font_id ||
	    a.font_size != b.font_size ||
	    a.font_weight != b.font_weight ||
	    a.line_height != b.line_height ||
	    a.text_align != b.text_align ||
	    a.overflow != b.overflow ||
	    a.overflow_x != b.overflow_x ||
	    a.overflow_y != b.overflow_y ||
	    a.mask_right_fade_width != b.mask_right_fade_width ||
	    a.image_fit != b.image_fit ||
	    a.backface_hidden != b.backface_hidden ||
	    a.text_decoration != b.text_decoration ||
	    a.text_transform != b.text_transform ||
	    a.white_space != b.white_space ||
	    a.text_overflow != b.text_overflow) return false;
	return true;
}

bool styleEqualExceptLocalDisplayCommands(const ComputedStyle &a, const ComputedStyle &b)
{
	const RareStyle &ar = rstyle(a);
	const RareStyle &br = rstyle(b);
	if (ar.transform_present != br.transform_present || ar.translate_present != br.translate_present || ar.rotate_present != br.rotate_present ||
	    ar.scale_present != br.scale_present || ar.filter_present != br.filter_present) return false;
	if (a.visibility != b.visibility || a.display != b.display ||
	    a.flex_direction != b.flex_direction ||
	    a.flex_direction_explicit != b.flex_direction_explicit ||
	    a.display_explicit != b.display_explicit ||
	    a.box_sizing != b.box_sizing ||
	    a.float_side != b.float_side ||
	    a.clear_side != b.clear_side ||
	    ar.margin_trim != br.margin_trim ||
	    a.writing_mode != b.writing_mode ||
	    a.direction != b.direction ||
	    a.row_gap != b.row_gap ||
	    a.column_gap != b.column_gap ||
	    a.row_gap_percent != b.row_gap_percent ||
	    a.column_gap_percent != b.column_gap_percent ||
	    a.margin_auto != b.margin_auto ||
	    a.order != b.order ||
	    a.flex_wrap != b.flex_wrap ||
	    a.justify_content != b.justify_content ||
	    a.align_items != b.align_items ||
	    a.justify_items != b.justify_items ||
	    ar.aspect_ratio != br.aspect_ratio ||
	    ar.flex_line_count != br.flex_line_count ||
	    ar.justify_self != br.justify_self ||
	    ar.grid_line[0] != br.grid_line[0] ||
	    ar.grid_line[1] != br.grid_line[1] ||
	    ar.grid_line[2] != br.grid_line[2] ||
	    ar.grid_line[3] != br.grid_line[3] ||
	    a.align_content != b.align_content ||
	    a.align_self != b.align_self ||
	    a.gap != b.gap ||
	    ar.grid_column_count != br.grid_column_count ||
	    ar.grid_row_count != br.grid_row_count) return false;
	for (int i = 0; i < kMaxGridTracks; ++i) {
		if (ar.grid_column_type[i] != br.grid_column_type[i] ||
		    ar.grid_row_type[i] != br.grid_row_type[i] ||
		    ar.grid_column_value[i] != br.grid_column_value[i] ||
		    ar.grid_row_value[i] != br.grid_row_value[i]) return false;
	}
	if (a.width != b.width ||
	    a.height != b.height ||
	    a.width_expression != b.width_expression ||
	    a.height_expression != b.height_expression ||
	    a.width_percent != b.width_percent ||
	    a.height_percent != b.height_percent ||
	    a.min_width != b.min_width ||
	    a.min_height != b.min_height ||
	    a.max_width != b.max_width ||
	    a.max_height != b.max_height ||
	    a.flex != b.flex ||
	    a.flex_shrink != b.flex_shrink ||
	    a.flex_basis != b.flex_basis ||
	    ar.flex_basis_expression != br.flex_basis_expression ||
	    ar.line_height_expression != br.line_height_expression ||
	    a.line_height_multiplier != b.line_height_multiplier) return false;
	for (int i = 0; i < 4; ++i) {
		if (boxInset(a, i) != boxInset(b, i) ||
		    a.padding[i] != b.padding[i] ||
		    a.margin[i] != b.margin[i] ||
		    ar.margin_expression[i] != br.margin_expression[i] ||
		    ar.padding_expression[i] != br.padding_expression[i] ||
		    a.pos_offsets[i] != b.pos_offsets[i] ||
		    a.pos_offset_percent[i] != b.pos_offset_percent[i]) return false;
	}
	if (a.position != b.position ||
	    a.z_index != b.z_index ||
	    a.z_index_auto != b.z_index_auto ||
	    a.opacity != b.opacity ||
	    a.blink_interval_ms != b.blink_interval_ms ||
	    a.blink_started_ms != b.blink_started_ms ||
	    a.blink_visible != b.blink_visible ||
	    ar.filter_blur_radius != br.filter_blur_radius ||
	    ar.box_shadow_inset != br.box_shadow_inset ||
	    ar.box_shadow_offset_x != br.box_shadow_offset_x ||
	    ar.box_shadow_offset_y != br.box_shadow_offset_y ||
	    ar.box_shadow_blur_radius != br.box_shadow_blur_radius ||
	    ar.box_shadow_spread != br.box_shadow_spread ||
	    ar.box_shadow_color != br.box_shadow_color ||
	    ar.box_shadow_alpha != br.box_shadow_alpha ||
	    a.font_id != b.font_id ||
	    a.font_size != b.font_size ||
	    a.font_weight != b.font_weight ||
	    a.line_height != b.line_height ||
	    a.text_align != b.text_align ||
	    a.overflow != b.overflow ||
	    a.overflow_x != b.overflow_x ||
	    a.overflow_y != b.overflow_y ||
	    a.mask_right_fade_width != b.mask_right_fade_width ||
	    a.image_fit != b.image_fit ||
	    a.text_decoration != b.text_decoration ||
	    a.text_transform != b.text_transform ||
	    a.white_space != b.white_space ||
	    a.text_overflow != b.text_overflow) return false;
	return true;
}

bool styleExactlyEqual(const ComputedStyle &a, const ComputedStyle &b)
{
	return a.text_color == b.text_color &&
	       a.text_alpha == b.text_alpha &&
	       styleEqualExceptTextPaint(a, b);
}

bool isPlainOpaqueBackground(const ComputedStyle &style)
{
	const RareStyle &rs = rstyle(style);
	return style.has_bg &&
	       style.bg_alpha == 255 &&
	       style.bg_fill == 0 &&
	       rs.bg_gradient_has_mid == 0 &&
	       rs.bg_overlay_gradient == 0 &&
	       rs.bg_radial_gradient == 0 &&
	       rs.bg_grid_axes == 0;
}

bool styleEqualExceptPlainBackgroundColor(const ComputedStyle &a, const ComputedStyle &b)
{
	if (a.bg_color == b.bg_color) return false;
	if (!isPlainOpaqueBackground(a) || !isPlainOpaqueBackground(b)) return false;
	ComputedStyle patched = b;
	patched.bg_color = a.bg_color;
	return styleExactlyEqual(a, patched);
}

void markClassRecomputeStyleDiff(int node, const ComputedStyle &beforeStyle, int beforeImageId, bool firstLineChanged)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	Node &target = state.nodes[node];
	if (beforeImageId == target.image_id && styleExactlyEqual(beforeStyle, target.style) && !firstLineChanged) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;
	if (firstLineChanged) {
		target.render.dirty = 1;
		target.render.layout_dirty = 1;
		target.render.non_scroll_dirty = 1;
		Tree::instance().markNodeDisplayCommandsDirty(node);
		// First-line backgrounds are recorded with descendant text fragments.
		// Rebuilding only the owner's box would retain their old fill commands.
		auto invalidateText = [&](auto &&self, int parent) -> void {
			for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
				if (state.nodes[child].type == NodeType::Text) {
					state.nodes[child].render.dirty = 1;
					state.nodes[child].render.non_scroll_dirty = 1;
					Tree::instance().markNodeDisplayCommandsDirty(child);
				}
				self(self, child);
			}
		};
		invalidateText(invalidateText, node);
	}
	if (beforeImageId == target.image_id && styleExactlyEqual(beforeStyle, target.style) && firstLineChanged) {
		return;
	}

	if (beforeImageId == target.image_id && styleEqualExceptTextPaint(beforeStyle, target.style)) {
		if (target.type == NodeType::Text || borderUsesCurrentColor(target.style) || borderUsesCurrentColor(beforeStyle)) {
			target.render.dirty = 1;  // paint-only: text color; geometry untouched
			target.render.non_scroll_dirty = 1;
			Tree::instance().markNodeDisplayCommandsDirty(node);
		}
		return;
	}

	if (beforeImageId == target.image_id && styleEqualExceptPlainBackgroundColor(beforeStyle, target.style)) {
		// Preserve `bg_recolor_from` across coalesced changes. The in-place recolor
		// rewrites framebuffer pixels equal to `from`. If several background-color
		// changes land between renders (e.g. fast rotary spins A->B->C), overwriting
		// `from` each time leaves it at an intermediate color (B/C) while the framebuffer
		// is still the original (A) from the last render — so almost nothing matches and
		// the background only partially recolors, leaving a patchwork that never
		// converges. Capture `from` only on the first change since the last render (when
		// the framebuffer still matches the current style), and just advance `to`.
		if (!target.render.bg_recolor_pending) {
			target.render.bg_recolor_pending = 1;
			target.render.bg_recolor_from = beforeStyle.bg_color;
		}
		target.render.bg_recolor_to = target.style.bg_color;
		target.render.dirty = 1;  // paint-only: background recolor; geometry untouched
		target.render.non_scroll_dirty = 1;
		Tree::instance().markNodeDisplayCommandsDirty(node);
		return;
	}

	target.render.dirty = 1;
	target.render.non_scroll_dirty = 1;
	if (preserves3D(beforeStyle) != preserves3D(target.style))
		Tree::instance().markDisplayListDirty();
	if (beforeImageId == target.image_id && styleEqualExceptLocalDisplayCommands(beforeStyle, target.style)) {
		// Local display-command diff: geometry equal by the predicate's
		// construction (transforms ride transform_dirty), so no layout pass.
		const RareStyle &beforeRare = rstyle(beforeStyle);
		const RareStyle &targetRare = rstyle(target.style);
		if (beforeRare.transform_rotate != targetRare.transform_rotate ||
		    beforeRare.transform_translate_outer_axes != targetRare.transform_translate_outer_axes ||
		    beforeRare.rotate_angle != targetRare.rotate_angle ||
		    beforeRare.rotate_axis_x != targetRare.rotate_axis_x ||
		    beforeRare.rotate_axis_y != targetRare.rotate_axis_y ||
		    beforeRare.rotate_axis_z != targetRare.rotate_axis_z ||
		    beforeRare.scale_x != targetRare.scale_x ||
		    beforeRare.scale_y != targetRare.scale_y ||
		    beforeRare.scale_z != targetRare.scale_z ||
		    beforeRare.transform_rotate_x != targetRare.transform_rotate_x ||
		    beforeRare.transform_rotate_y != targetRare.transform_rotate_y ||
		    beforeRare.translate_x != targetRare.translate_x ||
		    beforeRare.transform_translate_x != targetRare.transform_translate_x ||
		    beforeRare.translate_y != targetRare.translate_y ||
		    beforeRare.transform_translate_y != targetRare.transform_translate_y ||
		    beforeRare.translate_z != targetRare.translate_z ||
		    beforeRare.transform_translate_z != targetRare.transform_translate_z ||
		    beforeRare.translate_x_percent != targetRare.translate_x_percent ||
		    beforeRare.transform_translate_x_percent != targetRare.transform_translate_x_percent ||
		    beforeRare.translate_y_percent != targetRare.translate_y_percent ||
		    beforeRare.transform_translate_y_percent != targetRare.transform_translate_y_percent ||
		    beforeRare.transform_scale_x != targetRare.transform_scale_x ||
		    beforeRare.transform_scale_y != targetRare.transform_scale_y ||
		    beforeRare.transform_scale_z != targetRare.transform_scale_z ||
		    beforeRare.transform_origin_x != targetRare.transform_origin_x ||
		    beforeRare.transform_origin_y != targetRare.transform_origin_y ||
		    beforeRare.perspective != targetRare.perspective ||
		    beforeRare.perspective_origin_x != targetRare.perspective_origin_x ||
		    beforeRare.perspective_origin_y != targetRare.perspective_origin_y) {
			target.render.transform_dirty = 1;
			if (state.fixedPositionUsed) target.render.layout_dirty = 1;
			state.transformScanSerial = ~0ull;
			state.transformScanValid = false;  // a transform was added/changed → drop durable no-transform cache
		}
		Tree::instance().markNodeDisplayCommandsDirty(node);
		// Flattened backface visibility changes the whole descendant paint group.
		if (beforeStyle.backface_hidden != target.style.backface_hidden ||
		    rstyle(beforeStyle).transform_preserve_3d != rstyle(target.style).transform_preserve_3d)
			Tree::instance().markDisplayListDirty();
		return;
	}
	target.render.layout_dirty = 1;
	Tree::instance().markDisplayListDirty();
}

struct CssText {
	const char *data = "";
	std::uint16_t length = 0;
	bool owned = false;
	bool hasVar = false;
	bool trimClean = true;

	CssText() = default;

	~CssText()
	{
		reset();
	}

	CssText(const CssText &other)
	{
		assign(other.data, other.length, other.owned);
	}

	CssText &operator=(const CssText &other)
	{
		if (this == &other) return *this;
		reset();
		assign(other.data, other.length, other.owned);
		return *this;
	}

	CssText(CssText &&other) noexcept
	    : data(other.data),
	      length(other.length),
	      owned(other.owned),
	      hasVar(other.hasVar),
	      trimClean(other.trimClean)
	{
		other.data = "";
		other.length = 0;
		other.owned = false;
		other.hasVar = false;
		other.trimClean = true;
	}

	CssText &operator=(CssText &&other) noexcept
	{
		if (this == &other) return *this;
		reset();
		data = other.data;
		length = other.length;
		owned = other.owned;
		hasVar = other.hasVar;
		trimClean = other.trimClean;
		other.data = "";
		other.length = 0;
		other.owned = false;
		other.hasVar = false;
		other.trimClean = true;
		return *this;
	}

	static CssText literal(const char *text)
	{
		CssText out;
		out.data = text ? text : "";
		out.length = clampLength(std::strlen(out.data));
		out.owned = false;
		out.refreshFlags();
		return out;
	}

	static CssText copy(const std::string &text)
	{
		CssText out;
		out.assignCopy(text.c_str(), text.size());
		return out;
	}

	static CssText view(const std::string &text)
	{
		CssText out;
		out.data = text.c_str();
		out.length = clampLength(text.size());
		out.owned = false;
		out.refreshFlags();
		return out;
	}

	static CssText view(const char *text, std::size_t size)
	{
		CssText out;
		out.data = text ? text : "";
		out.length = clampLength(size);
		out.owned = false;
		out.refreshFlags();
		return out;
	}

	bool empty() const { return length == 0; }
	const char *c_str() const { return data ? data : ""; }
	std::string str() const { return std::string(c_str(), length); }
	bool hasVarReference() const { return hasVar; }

	bool equals(const char *text) const
	{
		const char *rhs = text ? text : "";
		const std::size_t rhsLen = std::strlen(rhs);
		return rhsLen == length && std::memcmp(c_str(), rhs, length) == 0;
	}

	bool contains(const char *needle) const
	{
		return needle && std::strstr(c_str(), needle) != nullptr;
	}

	bool startsWith(const char *prefix) const
	{
		if (!prefix) return false;
		const std::size_t prefixLen = std::strlen(prefix);
		return prefixLen <= length && std::memcmp(c_str(), prefix, prefixLen) == 0;
	}

	std::string trimmedStr() const
	{
		if (trimClean) return str();
		std::size_t b = 0;
		std::size_t e = length;
		const char *text = c_str();
		while (b < e && static_cast<unsigned char>(text[b]) <= ' ') ++b;
		while (e > b && static_cast<unsigned char>(text[e - 1]) <= ' ') --e;
		return std::string(text + b, e - b);
	}

private:
	static std::uint16_t clampLength(std::size_t size)
	{
		return size > 0xFFFFu ? static_cast<std::uint16_t>(0xFFFFu) : static_cast<std::uint16_t>(size);
	}

	void reset()
	{
		if (owned) delete[] const_cast<char *>(data);
		data = "";
		length = 0;
		owned = false;
		hasVar = false;
		trimClean = true;
	}

	void assign(const char *text, std::uint16_t size, bool copyText)
	{
		if (copyText)
			assignCopy(text, size);
		else {
			data = text ? text : "";
			length = size;
			owned = false;
			refreshFlags();
		}
	}

	void assignCopy(const char *text, std::size_t size)
	{
		length = clampLength(size);
		char *copy = new char[static_cast<std::size_t>(length) + 1];
		if (length > 0 && text) std::memcpy(copy, text, length);
		copy[length] = '\0';
		data = copy;
		owned = true;
		refreshFlags();
	}

	void refreshFlags()
	{
		const char *text = c_str();
		hasVar = false;
		for (std::uint16_t i = 0; i + 4 <= length; ++i) {
			if (std::memcmp(text + i, "var(", 4) == 0) {
				hasVar = true;
				break;
			}
		}
		trimClean = length == 0 ||
		            (static_cast<unsigned char>(text[0]) > ' ' &&
		             static_cast<unsigned char>(text[length - 1]) > ' ');
	}
};

CssAtomId atomForText(const CssText &text)
{
	return text.empty() ? kInvalidCssAtom : internCssAtom(text.c_str(), text.length);
}

enum class CssRuleProperty : std::uint8_t {
	Other,
	Custom,
	Animation
};

using CssDeclarationId = StyleDeclaration;

CssDeclarationId classifyDeclaration(const char *property)
{
	if (!property || !*property) return CssDeclarationId::Unknown;
	if (property[0] == '-' && property[1] == '-') return CssDeclarationId::Custom;
	if (std::strcmp(property, "color-scheme") == 0 ||
		    std::strcmp(property, "isolation") == 0 ||
	    std::strcmp(property, "letter-spacing") == 0 ||
	    std::strcmp(property, "outline") == 0 ||
	    std::strcmp(property, "scroll-snap-align") == 0 ||
	    std::strcmp(property, "scroll-snap-type") == 0 ||
	    std::strcmp(property, "scrollbar-width") == 0 ||
	    std::strcmp(property, "text-shadow") == 0 ||
	    std::strcmp(property, "transition") == 0 ||
	    std::strcmp(property, "cursor") == 0 ||
	    std::strcmp(property, "-webkit-tap-highlight-color") == 0)
		return CssDeclarationId::Ignored;
	if (std::strcmp(property, "animation") == 0) return CssDeclarationId::Animation;
	if (std::strcmp(property, "font") == 0) return CssDeclarationId::Font;
	if (std::strcmp(property, "display") == 0) return CssDeclarationId::Display;
	if (std::strcmp(property, "flex-direction") == 0) return CssDeclarationId::FlexDirection;
	if (std::strcmp(property, "flex-wrap") == 0) return CssDeclarationId::FlexWrap;
	if (std::strcmp(property, "flex-line-count") == 0) return CssDeclarationId::FlexLineCount;
	if (std::strcmp(property, "justify-content") == 0) return CssDeclarationId::JustifyContent;
	if (std::strcmp(property, "align-items") == 0) return CssDeclarationId::AlignItems;
	if (std::strcmp(property, "justify-items") == 0) return CssDeclarationId::JustifyItems;
	if (std::strcmp(property, "justify-self") == 0) return CssDeclarationId::JustifySelf;
	if (std::strcmp(property, "grid-row-start") == 0) return CssDeclarationId::GridRowStart;
	if (std::strcmp(property, "grid-column-start") == 0) return CssDeclarationId::GridColumnStart;
	if (std::strcmp(property, "grid-row-end") == 0) return CssDeclarationId::GridRowEnd;
	if (std::strcmp(property, "grid-column-end") == 0) return CssDeclarationId::GridColumnEnd;
	if (std::strcmp(property, "grid-row") == 0) return CssDeclarationId::GridRow;
	if (std::strcmp(property, "grid-column") == 0) return CssDeclarationId::GridColumn;
	if (std::strcmp(property, "grid-area") == 0) return CssDeclarationId::GridArea;
	if (std::strcmp(property, "grid-template") == 0) return CssDeclarationId::GridTemplate;
	if (std::strcmp(property, "grid") == 0) return CssDeclarationId::Grid;
	if (std::strcmp(property, "align-content") == 0) return CssDeclarationId::AlignContent;
	if (std::strcmp(property, "align-self") == 0) return CssDeclarationId::AlignSelf;
	if (std::strcmp(property, "place-items") == 0) return CssDeclarationId::PlaceItems;
	if (std::strcmp(property, "place-content") == 0) return CssDeclarationId::PlaceContent;
	if (std::strcmp(property, "place-self") == 0) return CssDeclarationId::PlaceSelf;
	if (std::strcmp(property, "grid-template-columns") == 0) return CssDeclarationId::GridTemplateColumns;
	if (std::strcmp(property, "grid-template-rows") == 0) return CssDeclarationId::GridTemplateRows;
	if (std::strcmp(property, "content") == 0) return CssDeclarationId::Content;
	if (std::strcmp(property, "gap") == 0) return CssDeclarationId::Gap;
	if (std::strcmp(property, "width") == 0) return CssDeclarationId::Width;
	if (std::strcmp(property, "height") == 0) return CssDeclarationId::Height;
	if (std::strcmp(property, "min-width") == 0) return CssDeclarationId::MinWidth;
	if (std::strcmp(property, "min-height") == 0) return CssDeclarationId::MinHeight;
	if (std::strcmp(property, "max-width") == 0) return CssDeclarationId::MaxWidth;
	if (std::strcmp(property, "max-height") == 0) return CssDeclarationId::MaxHeight;
	if (std::strcmp(property, "flex") == 0) return CssDeclarationId::Flex;
	if (std::strcmp(property, "flex-grow") == 0) return CssDeclarationId::FlexGrow;
	if (std::strcmp(property, "flex-shrink") == 0) return CssDeclarationId::FlexShrink;
	if (std::strcmp(property, "flex-basis") == 0) return CssDeclarationId::FlexBasis;
	if (std::strcmp(property, "padding") == 0) return CssDeclarationId::Padding;
	if (std::strcmp(property, "padding-top") == 0) return CssDeclarationId::PaddingTop;
	if (std::strcmp(property, "padding-right") == 0) return CssDeclarationId::PaddingRight;
	if (std::strcmp(property, "padding-bottom") == 0) return CssDeclarationId::PaddingBottom;
	if (std::strcmp(property, "padding-left") == 0) return CssDeclarationId::PaddingLeft;
	if (std::strcmp(property, "margin") == 0) return CssDeclarationId::Margin;
	if (std::strcmp(property, "margin-top") == 0) return CssDeclarationId::MarginTop;
	if (std::strcmp(property, "margin-right") == 0) return CssDeclarationId::MarginRight;
	if (std::strcmp(property, "margin-bottom") == 0) return CssDeclarationId::MarginBottom;
	if (std::strcmp(property, "margin-left") == 0) return CssDeclarationId::MarginLeft;
	if (std::strcmp(property, "position") == 0) return CssDeclarationId::Position;
	if (std::strcmp(property, "inset") == 0) return CssDeclarationId::Inset;
	if (std::strcmp(property, "top") == 0) return CssDeclarationId::Top;
	if (std::strcmp(property, "right") == 0) return CssDeclarationId::Right;
	if (std::strcmp(property, "bottom") == 0) return CssDeclarationId::Bottom;
	if (std::strcmp(property, "left") == 0) return CssDeclarationId::Left;
	if (std::strcmp(property, "box-sizing") == 0) return CssDeclarationId::BoxSizing;
	if (std::strcmp(property, "float") == 0) return CssDeclarationId::Float;
	if (std::strcmp(property, "aspect-ratio") == 0) return CssDeclarationId::AspectRatio;
	if (std::strcmp(property, "margin-trim") == 0) return CssDeclarationId::MarginTrim;
	if (std::strcmp(property, "clear") == 0) return CssDeclarationId::Clear;
	if (std::strcmp(property, "direction") == 0) return CssDeclarationId::Direction;
	if (std::strcmp(property, "writing-mode") == 0) return CssDeclarationId::WritingMode;
	if (std::strcmp(property, "flex-flow") == 0) return CssDeclarationId::FlexFlow;
	if (std::strcmp(property, "row-gap") == 0) return CssDeclarationId::RowGap;
	if (std::strcmp(property, "column-gap") == 0) return CssDeclarationId::ColumnGap;
	if (std::strcmp(property, "order") == 0) return CssDeclarationId::Order;
	if (std::strcmp(property, "z-index") == 0) return CssDeclarationId::ZIndex;
	if (std::strcmp(property, "active-background-color") == 0 || std::strcmp(property, "active-background") == 0) return CssDeclarationId::ActiveBackgroundColor;
	if (std::strcmp(property, "contain") == 0) return CssDeclarationId::Contain;
	if (std::strcmp(property, "background-color") == 0) return CssDeclarationId::BackgroundColor;
	if (std::strcmp(property, "background-clip") == 0) return CssDeclarationId::BackgroundClip;
	if (std::strcmp(property, "background-image") == 0) return CssDeclarationId::BackgroundImage;
	if (std::strcmp(property, "background") == 0) return CssDeclarationId::Background;
	if (std::strcmp(property, "background-size") == 0) return CssDeclarationId::BackgroundSize;
	if (std::strcmp(property, "background-position") == 0) return CssDeclarationId::BackgroundPosition;
	if (std::strcmp(property, "background-repeat") == 0) return CssDeclarationId::BackgroundRepeat;
	if (std::strcmp(property, "background-attachment") == 0) return CssDeclarationId::BackgroundAttachment;
	if (std::strcmp(property, "background-origin") == 0) return CssDeclarationId::BackgroundOrigin;
	if (std::strcmp(property, "object-fit") == 0) return CssDeclarationId::ObjectFit;
	if (std::strcmp(property, "color") == 0) return CssDeclarationId::Color;
	if (std::strcmp(property, "opacity") == 0) return CssDeclarationId::Opacity;
	if (std::strcmp(property, "border-color") == 0) return CssDeclarationId::BorderColor;
	if (std::strcmp(property, "border") == 0) return CssDeclarationId::Border;
	if (std::strcmp(property, "border-width") == 0) return CssDeclarationId::BorderWidth;
	if (std::strcmp(property, "border-top") == 0) return CssDeclarationId::BorderTop;
	if (std::strcmp(property, "border-right") == 0) return CssDeclarationId::BorderRight;
	if (std::strcmp(property, "border-bottom") == 0) return CssDeclarationId::BorderBottom;
	if (std::strcmp(property, "border-left") == 0) return CssDeclarationId::BorderLeft;
	if (std::strcmp(property, "border-top-width") == 0) return CssDeclarationId::BorderTopWidth;
	if (std::strcmp(property, "border-right-width") == 0) return CssDeclarationId::BorderRightWidth;
	if (std::strcmp(property, "border-bottom-width") == 0) return CssDeclarationId::BorderBottomWidth;
	if (std::strcmp(property, "border-left-width") == 0) return CssDeclarationId::BorderLeftWidth;
	if (std::strcmp(property, "border-top-color") == 0) return CssDeclarationId::BorderTopColor;
	if (std::strcmp(property, "border-right-color") == 0) return CssDeclarationId::BorderRightColor;
	if (std::strcmp(property, "border-bottom-color") == 0) return CssDeclarationId::BorderBottomColor;
	if (std::strcmp(property, "border-left-color") == 0) return CssDeclarationId::BorderLeftColor;
	if (std::strcmp(property, "border-radius") == 0) return CssDeclarationId::BorderRadius;
	if (std::strcmp(property, "border-top-left-radius") == 0) return CssDeclarationId::BorderTopLeftRadius;
	if (std::strcmp(property, "border-top-right-radius") == 0) return CssDeclarationId::BorderTopRightRadius;
	if (std::strcmp(property, "border-bottom-right-radius") == 0) return CssDeclarationId::BorderBottomRightRadius;
	if (std::strcmp(property, "border-bottom-left-radius") == 0) return CssDeclarationId::BorderBottomLeftRadius;
	if (std::strcmp(property, "font-family") == 0) return CssDeclarationId::FontFamily;
	if (std::strcmp(property, "font-size") == 0) return CssDeclarationId::FontSize;
	if (std::strcmp(property, "font-weight") == 0) return CssDeclarationId::FontWeight;
	if (std::strcmp(property, "line-height") == 0) return CssDeclarationId::LineHeight;
	if (std::strcmp(property, "text-align") == 0) return CssDeclarationId::TextAlign;
	if (std::strcmp(property, "text-decoration") == 0 || std::strcmp(property, "text-decoration-line") == 0) return CssDeclarationId::TextDecoration;
	if (std::strcmp(property, "text-transform") == 0) return CssDeclarationId::TextTransform;
	if (std::strcmp(property, "white-space") == 0) return CssDeclarationId::WhiteSpace;
	if (std::strcmp(property, "text-overflow") == 0) return CssDeclarationId::TextOverflow;
	if (std::strcmp(property, "transform-style") == 0) return CssDeclarationId::TransformStyle;
	if (std::strcmp(property, "visibility") == 0) return CssDeclarationId::Visibility;
	if (std::strcmp(property, "backface-visibility") == 0) return CssDeclarationId::BackfaceVisibility;
	if (std::strcmp(property, "pointer-events") == 0) return CssDeclarationId::PointerEvents;
	if (std::strcmp(property, "overflow") == 0) return CssDeclarationId::Overflow;
	if (std::strcmp(property, "overflow-x") == 0) return CssDeclarationId::OverflowX;
	if (std::strcmp(property, "overflow-y") == 0) return CssDeclarationId::OverflowY;
	if (std::strcmp(property, "mask-image") == 0 || std::strcmp(property, "-webkit-mask-image") == 0) return CssDeclarationId::MaskImage;
	if (std::strcmp(property, "transform") == 0) return CssDeclarationId::Transform;
	if (std::strcmp(property, "translate") == 0) return CssDeclarationId::Translate;
	if (std::strcmp(property, "rotate") == 0) return CssDeclarationId::Rotate;
	if (std::strcmp(property, "scale") == 0) return CssDeclarationId::Scale;
	if (std::strcmp(property, "filter") == 0) return CssDeclarationId::Filter;
	if (std::strcmp(property, "box-shadow") == 0) return CssDeclarationId::BoxShadow;
	if (std::strcmp(property, "transform-origin") == 0) return CssDeclarationId::TransformOrigin;
	if (std::strcmp(property, "perspective") == 0) return CssDeclarationId::Perspective;
	if (std::strcmp(property, "perspective-origin") == 0) return CssDeclarationId::PerspectiveOrigin;
	return CssDeclarationId::Unknown;
}

CssDeclarationId classifyDeclaration(const CssText &property)
{
	return classifyDeclaration(property.c_str());
}

CssDeclarationId classifyDeclaration(const std::string &property)
{
	return classifyDeclaration(property.c_str());
}

CssRuleProperty rulePropertyKind(CssDeclarationId declaration)
{
	if (declaration == CssDeclarationId::Custom) return CssRuleProperty::Custom;
	if (declaration == CssDeclarationId::Animation) return CssRuleProperty::Animation;
	return CssRuleProperty::Other;
}

constexpr std::uint16_t kNoCompiledCssValue = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssAnimationSpec = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssBackground = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssGridTemplate = 0xFFFFu;
constexpr std::uint16_t kNoCompiledCssLengthExpression = 0xFFFFu;
constexpr std::uint16_t kNoCssRuleText = 0xFFFFu;
constexpr std::uint16_t kNoSelectorPlan = 0xFFFFu;
constexpr std::uint16_t kNoMediaConditionPlan = 0xFFFFu;

#ifndef GEA_COMPILED_CSS_VALUE_MASK
#define GEA_COMPILED_CSS_VALUE_MASK \
	(kCssCompiledFeatureKeyword | kCssCompiledFeatureColor | kCssCompiledFeatureOpacity | \
	 kCssCompiledFeatureSize | kCssCompiledFeaturePosition | kCssCompiledFeatureBox | \
	 kCssCompiledFeatureLength | kCssCompiledFeatureOrigin | kCssCompiledFeatureTransformScalar | \
	 kCssCompiledFeatureTransform | kCssCompiledFeatureBackground | kCssCompiledFeatureEffects | \
	 kCssCompiledFeatureGridTemplate | kCssCompiledFeatureNumber)
#endif
#ifndef GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES
#define GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES 32
#endif

constexpr std::uint32_t kCssCompiledFeatureKeyword = 1u << 0;
constexpr std::uint32_t kCssCompiledFeatureColor = 1u << 1;
constexpr std::uint32_t kCssCompiledFeatureOpacity = 1u << 2;
constexpr std::uint32_t kCssCompiledFeatureSize = 1u << 3;
constexpr std::uint32_t kCssCompiledFeaturePosition = 1u << 4;
constexpr std::uint32_t kCssCompiledFeatureBox = 1u << 5;
constexpr std::uint32_t kCssCompiledFeatureLength = 1u << 6;
constexpr std::uint32_t kCssCompiledFeatureOrigin = 1u << 7;
constexpr std::uint32_t kCssCompiledFeatureTransformScalar = 1u << 8;
constexpr std::uint32_t kCssCompiledFeatureTransform = 1u << 9;
constexpr std::uint32_t kCssCompiledFeatureBackground = 1u << 10;
constexpr std::uint32_t kCssCompiledFeatureEffects = 1u << 11;
constexpr std::uint32_t kCssCompiledFeatureGridTemplate = 1u << 12;
constexpr std::uint32_t kCssCompiledFeatureNumber = 1u << 13;
constexpr int kInlineCompiledStyleCacheEntries = GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES;
static_assert(kInlineCompiledStyleCacheEntries >= 0);

constexpr bool compiledCssValueFeatureEnabled(std::uint32_t feature)
{
	return (static_cast<std::uint32_t>(GEA_COMPILED_CSS_VALUE_MASK) & feature) != 0u;
}

enum class CssCompiledKind : std::uint8_t {
	None,
	Noop,
	DirectProperty,
	DirectPropertyGroup,
	Keyword,
	Number,
	Length,
	Size,
	PositionOffset,
	Box,
	Color,
	ColorVar,
	Opacity,
	Rotate,
	Scale,
	OriginPair,
	Transform,
	LineHeight,
	Background,
	BackgroundSize,
	FilterBlur,
	BoxShadow,
	GridTemplate,
	Flex,
	FlexBasis,
	BorderShorthand,
	BorderSideShorthand,
	BorderRadius
};

enum class CssLengthUnit : std::uint8_t {
	Invalid,
	Raw,
	Px,
	Percent,
	Vw,
	Vh,
	Vmin,
	Vmax,
	Dvw,
	Dvh,
	Auto,
	Expression,
	Ch,
	Em,
	Rem,
	Lh,
	Rlh
};

struct CssLengthSpec {
	float value = 0.0f;
	CssLengthUnit unit = CssLengthUnit::Invalid;
};

enum class CssLengthExpressionKind : std::uint8_t {
	Add,
	Subtract,
	Multiply,
	Divide,
	Min,
	Max,
	Clamp,
	Var
};

struct CssLengthExpression {
	CssLengthExpressionKind kind = CssLengthExpressionKind::Add;
	CssLengthSpec a;
	CssLengthSpec b;
	CssLengthSpec c;
	float scalar = 0.0f;
	CssAtomId nameAtom = kInvalidCssAtom;
	std::uint8_t hasFallback = 0;
};

// Preserve fractions through expressions; each consuming property chooses its
// final device-pixel conversion (ordinary rounding or border-width snapping).
struct ResolvedCssLength {
	float value = 0;
	bool isPercent = false;
	bool isAuto = false;
	ResolvedCssLength() = default;
	ResolvedCssLength(double length, bool percent, bool automatic)
	    : value(static_cast<float>(length)), isPercent(percent), isAuto(automatic) {}
};

struct CachedCssColor {
	std::int32_t styleColor = 0;
	std::int32_t nativeColor = 0;
	std::uint8_t alpha = 255;
	bool valid = false;
};

struct CssBackgroundPair {
	CssLengthSpec x, y;
	int a = 0, b = 0;
};
std::vector<std::vector<CssBackgroundPair>> &backgroundPlacementLists()
{
	static std::vector<std::vector<CssBackgroundPair>> lists;
	return lists;
}
int storeBackgroundPlacementList(const std::vector<CssBackgroundPair> &list)
{
	auto &lists = backgroundPlacementLists();
	for (std::size_t i = 0; i < lists.size(); ++i) {
		if (lists[i].size() != list.size()) continue;
		bool equal = true;
		for (std::size_t j = 0; j < list.size(); ++j) {
			const auto &a = lists[i][j], &b = list[j];
			equal &= a.x.unit == b.x.unit && a.x.value == b.x.value && a.y.unit == b.y.unit && a.y.value == b.y.value && a.a == b.a && a.b == b.b;
		}
		if (equal) return static_cast<int>(i);
	}
	lists.push_back(list); return static_cast<int>(lists.size()-1);
}

struct CssCompiledValue {
	CssCompiledKind kind = CssCompiledKind::None;
	CssDeclarationId declaration = CssDeclarationId::Unknown;
	std::uint16_t flags = 0;
	std::uint8_t aux = 0;
	CssLengthSpec lengths[4];
	std::int32_t values[11]{};
};

CssLengthSpec cssLengthSpecForStatic(StaticStyleLengthSpec spec);
struct CssCompiledBackground;
std::vector<CssCompiledBackground> &compiledCssBackgrounds();

struct CssCompiledLinearGradient {
	std::int32_t fromStyleColor = 0;
	style_color_t fromNativeColor = 0;
	style_color_t midNativeColor = 0;
	style_color_t toNativeColor = 0;
	CssAtomId fromColorAtom = kInvalidCssAtom;
	CssAtomId midColorAtom = kInvalidCssAtom;
	CssAtomId toColorAtom = kInvalidCssAtom;
	std::uint8_t fromAlpha = 255;
	std::uint8_t midAlpha = 255;
	std::uint8_t toAlpha = 255;
	std::uint16_t midStopPermille = 500;
	std::uint16_t toStopPermille = 1000;
	std::uint8_t hasMid = 0;
	std::uint8_t fromColorHasFallback = 0;
	std::uint8_t midColorHasFallback = 0;
	std::uint8_t toColorHasFallback = 0;
	std::int16_t angleTenths = 1800;
};

struct CssCompiledRadialGradient {
	style_color_t fromNativeColor = 0;
	style_color_t toNativeColor = 0;
	CssAtomId fromColorAtom = kInvalidCssAtom;
	CssAtomId toColorAtom = kInvalidCssAtom;
	std::uint8_t fromAlpha = 255;
	std::uint8_t toAlpha = 255;
	std::uint8_t fromColorHasFallback = 0;
	std::uint8_t toColorHasFallback = 0;
	std::uint16_t stopPermille = 1000;
	std::int16_t cxPermille = 500;
	std::int16_t cyPermille = 500;
	std::int16_t rxPermille = 1000;
	std::int16_t ryPermille = 1000;
};

struct CssCompiledBackground {
	std::uint16_t gradientLayer = 0, overlayLayer = 0, radialLayer = 0;
	std::uint16_t layerCount = 1;
	std::uint16_t clip = 0;
	std::int32_t colorStyle = 0;
	std::uint8_t colorAlpha = 0;
	CssCompiledLinearGradient gradient;
	CssCompiledLinearGradient overlayGradient;
	CssCompiledRadialGradient radialGradient;
	CssLengthSpec gridLineX;
	CssLengthSpec gridLineY;
	style_color_t gridColor = 0;
	std::uint8_t gridAlpha = 255;
	std::uint8_t gridAxes = 0;
	std::uint8_t hasGridLineX = 0;
	std::uint8_t hasGridLineY = 0;
	std::uint8_t hasGradient = 0;
	std::uint8_t hasOverlayGradient = 0;
	std::uint8_t hasRadialGradient = 0;
};

struct CssCompiledGridTrack {
	std::int8_t type = 0;
	std::int16_t value = 0;
	CssLengthSpec length;
};

struct CssCompiledGridTemplate {
	std::uint8_t count = 0;
	CssCompiledGridTrack tracks[kMaxGridTracks];
};

std::uint16_t compileCssValue(CssDeclarationId declaration, const CssText &value);
int containmentValue(const std::string &value);

std::uint16_t compileCustomPropertyValue(const CssText &value);
std::uint16_t compileCssAnimationSpec(const CssText &value);
std::uint16_t compileSelectorPlan(const CssText &selector);
std::uint16_t compileMediaConditionPlan(const CssText &condition);
void clearCompiledCssBackgrounds();
void clearCompiledCssGridTemplates();
void clearCompiledCssLengthExpressions();
void clearStaticLengthExpressionResolutionCache();

struct CssRule {
	enum class SelectorType {
		Class,
		Element,
		Selector
	};

	enum class PseudoElement {
		None,
		Before,
		After,
		FirstLine,
		Unsupported
	};

	SelectorType selectorType;
	PseudoElement pseudoElement;
	CssRuleProperty propertyKind;
	CssDeclarationId declaration;
	std::uint16_t compiledValue;
	std::uint16_t compiledAnimationSpec;
	CssAtomId selectorAtom;
	CssAtomId propertyAtom;
	int16_t selectorTagId;
	std::uint16_t selectorPlan;
	std::uint16_t mediaPlan;
	std::uint16_t propertyText;
	std::uint16_t valueText;
	std::uint16_t mediaText;
	bool userAgent = false;
};

struct CssKeyframeRule {
	CssAtomId nameAtom;
	int offsetPermille;
	CssRuleProperty propertyKind;
	CssDeclarationId declaration;
	std::uint16_t compiledValue;
	std::uint16_t valueText;
};

struct SelectorTextSlice {
	const char *data = "";
	std::size_t length = 0;
	CssRule::PseudoElement pseudo = CssRule::PseudoElement::None;
};

bool asciiEqualsIgnoreCaseTrimmed(const char *text, std::size_t length, const char *expected)
{
	if (!text || !expected) return false;
	while (length > 0 && static_cast<unsigned char>(*text) <= ' ') {
		++text;
		--length;
	}
	while (length > 0 && static_cast<unsigned char>(text[length - 1]) <= ' ') --length;
	const std::size_t expectedLength = std::strlen(expected);
	if (length != expectedLength) return false;
	for (std::size_t i = 0; i < length; ++i) {
		const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
		if (lhs != expected[i]) return false;
	}
	return true;
}

SelectorTextSlice selectorTextWithoutPseudo(const char *selectorText, std::size_t selectorLength)
{
	SelectorTextSlice out;
	out.data = selectorText ? selectorText : "";
	out.length = selectorText ? selectorLength : 0;
	for (std::size_t i = 0; i + 1 < out.length; ++i) {
		if (out.data[i] != ':' || out.data[i + 1] != ':') continue;
		std::size_t selectorEnd = i;
		while (selectorEnd > 0 && static_cast<unsigned char>(out.data[selectorEnd - 1]) <= ' ') --selectorEnd;
		const char *name = out.data + i + 2;
		const std::size_t nameLength = out.length - i - 2;
		out.length = selectorEnd;
		if (asciiEqualsIgnoreCaseTrimmed(name, nameLength, "before")) {
			out.pseudo = CssRule::PseudoElement::Before;
		} else if (asciiEqualsIgnoreCaseTrimmed(name, nameLength, "after")) {
			out.pseudo = CssRule::PseudoElement::After;
		} else if (asciiEqualsIgnoreCaseTrimmed(name, nameLength, "first-line")) {
			out.pseudo = CssRule::PseudoElement::FirstLine;
		} else {
			out.pseudo = CssRule::PseudoElement::Unsupported;
		}
		return out;
	}
	return out;
}

enum class StyleApplicationSource {
	Inline,
	ClassRule
};

enum class LengthAxis {
	None,
	Horizontal,
	Vertical
};

bool propertyAffectsDescendantStyle(Property property);
void recomputeDescendantClassStyles(int node);
void recomputeSubtreeClassStyles(int node);
void setStyleValue(NodeHandle node, Property property, int value, StyleApplicationSource source);
struct ActiveRulePlan;
void primeCssAnimationsForNode(int node, const ActiveRulePlan *activePlan = nullptr);
int parseOriginPart(const std::string &part, int fallback);

// File-scope lazy pointer rather than a function-local static: the static-local
// guard is not inlined on this Xtensa toolchain, so a Meyers singleton pays a
// __cxa_guard_acquire CALL per access, and rules() is iterated per node during
// every class-style recompute. Single-threaded UI access, so no guard needed.
std::vector<CssRule> *g_rules = nullptr;

std::vector<CssRule> &rules()
{
	if (!g_rules) g_rules = new std::vector<CssRule>();
	return *g_rules;
}

std::vector<CssKeyframeRule> &keyframeRules()
{
	static std::vector<CssKeyframeRule> list;
	return list;
}

struct DenseRuleBucketSpan {
	const int *data = nullptr;
	std::size_t count = 0;

	bool empty() const { return count == 0; }
};

struct DenseRuleBuckets {
	struct PendingEntry {
		std::uint16_t key;
		int value;
	};

	std::vector<PendingEntry> pending;
	std::vector<int> values;
	std::vector<std::uint32_t> offsets;
	std::size_t nonEmpty = 0;
	std::uint16_t maxKey = 0;
	bool hasPending = false;

	void clear()
	{
		pending.clear();
		values.clear();
		offsets.clear();
		nonEmpty = 0;
		maxKey = 0;
		hasPending = false;
	}

	void add(int key, int value)
	{
		if (key < 0 || key > 0xFFFF) return;
		const std::size_t index = static_cast<std::size_t>(key);
		pending.push_back({static_cast<std::uint16_t>(index), value});
		if (index > maxKey) maxKey = static_cast<std::uint16_t>(index);
		hasPending = true;
	}

	void finalize()
	{
		values.clear();
		offsets.clear();
		nonEmpty = 0;
		if (pending.empty()) {
			hasPending = false;
			return;
		}
		offsets.assign(static_cast<std::size_t>(maxKey) + 2, 0);
		for (const PendingEntry &entry : pending)
			++offsets[static_cast<std::size_t>(entry.key) + 1];
		std::uint32_t running = 0;
		for (std::size_t key = 0; key + 1 < offsets.size(); ++key) {
			const std::uint32_t count = offsets[key + 1];
			if (count != 0) ++nonEmpty;
			offsets[key] = running;
			running += count;
		}
		offsets.back() = running;
		values.resize(pending.size());
		for (const PendingEntry &entry : pending) {
			const std::size_t index = static_cast<std::size_t>(entry.key);
			values[offsets[index]++] = entry.value;
		}
		for (std::size_t i = offsets.size() - 1; i > 0; --i)
			offsets[i] = offsets[i - 1];
		offsets[0] = 0;
		std::vector<PendingEntry>().swap(pending);
		hasPending = false;
	}

	DenseRuleBucketSpan get(int key) const
	{
		if (key < 0 || hasPending) return {};
		const std::size_t index = static_cast<std::size_t>(key);
		if (index + 1 >= offsets.size()) return {};
		const std::uint32_t begin = offsets[index];
		const std::uint32_t end = offsets[index + 1];
		if (begin == end) return {};
		return {values.data() + begin, static_cast<std::size_t>(end - begin)};
	}

	bool empty() const { return nonEmpty == 0; }
};

struct KeyframeRuleIndex {
	bool valid = false;
	DenseRuleBuckets byName;
};

KeyframeRuleIndex g_keyframeRuleIndex;

struct DenseIdSet {
	std::vector<std::uint8_t> bits;
	std::size_t count = 0;

	void clear()
	{
		std::fill(bits.begin(), bits.end(), 0);
		count = 0;
	}

	void insert(int key)
	{
		if (key < 0) return;
		const std::size_t index = static_cast<std::size_t>(key);
		if (index >= bits.size()) bits.resize(index + 1, 0);
		if (bits[index] == 0) {
			bits[index] = 1;
			++count;
		}
	}

	bool contains(int key) const
	{
		if (key < 0) return false;
		const std::size_t index = static_cast<std::size_t>(key);
		return index < bits.size() && bits[index] != 0;
	}

	bool empty() const { return count == 0; }
};

void invalidateKeyframeRuleIndex()
{
	g_keyframeRuleIndex.valid = false;
}

void clearKeyframeRuleIndex()
{
	g_keyframeRuleIndex.byName.clear();
	g_keyframeRuleIndex.valid = false;
}

void rebuildKeyframeRuleIndexIfNeeded()
{
	if (g_keyframeRuleIndex.valid) return;
	g_keyframeRuleIndex.byName.clear();
	const auto &list = keyframeRules();
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		if (list[i].nameAtom == kInvalidCssAtom) continue;
		g_keyframeRuleIndex.byName.add(list[i].nameAtom, i);
	}
	g_keyframeRuleIndex.byName.finalize();
	g_keyframeRuleIndex.valid = true;
}

DenseRuleBucketSpan keyframeRuleIndicesForName(CssAtomId name)
{
	if (name == kInvalidCssAtom) return {};
	rebuildKeyframeRuleIndexIfNeeded();
	return g_keyframeRuleIndex.byName.get(name);
}

std::vector<CssCompiledValue> &compiledCssValues()
{
	static std::vector<CssCompiledValue> list;
	return list;
}

std::vector<CssText> &cssRuleTexts()
{
	static std::vector<CssText> list;
	return list;
}

void clearCssRuleTexts()
{
	cssRuleTexts().clear();
}

std::uint16_t storeCssRuleText(CssText text)
{
	if (text.empty()) return kNoCssRuleText;
	auto &list = cssRuleTexts();
	if (list.size() >= kNoCssRuleText) return kNoCssRuleText;
	list.push_back(std::move(text));
	return static_cast<std::uint16_t>(list.size() - 1);
}

const CssText &cssRuleTextForHandle(std::uint16_t handle)
{
	static const CssText empty;
	const auto &list = cssRuleTexts();
	return handle < list.size() ? list[handle] : empty;
}

std::uint16_t storeDirectPropertyCompiledValue(Property property, int value)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::DirectProperty;
	compiled.values[0] = static_cast<int>(property);
	compiled.values[1] = value;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeDirectPropertyGroupCompiledValue(std::initializer_list<StaticStylePropertyValue> properties)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::DirectPropertyGroup;
	int count = 0;
	for (const StaticStylePropertyValue &entry : properties) {
		if (count >= 4) break;
		compiled.values[1 + count * 2] = static_cast<int>(entry.property);
		compiled.values[2 + count * 2] = entry.value;
		count++;
	}
	if (count == 0) return kNoCompiledCssValue;
	compiled.values[0] = count;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

int clampStaticCssColorChannel(int value)
{
	return std::clamp(value, 0, 255);
}

std::uint16_t storeStaticColorCompiledValue(CssDeclarationId declaration, int r, int g, int b, int a)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const int cr = clampStaticCssColorChannel(r);
	const int cg = clampStaticCssColorChannel(g);
	const int cb = clampStaticCssColorChannel(b);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Color;
	compiled.declaration = declaration;
	compiled.values[0] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(cr, cg, cb));
	compiled.values[1] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeColor(cr, cg, cb));
	compiled.values[2] = clampStaticCssColorChannel(a);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticColorVarCompiledValue(CssDeclarationId declaration,
                                               const char *name,
                                               bool hasFallback,
                                               int r,
                                               int g,
                                               int b,
                                               int a)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const int cr = clampStaticCssColorChannel(r);
	const int cg = clampStaticCssColorChannel(g);
	const int cb = clampStaticCssColorChannel(b);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::ColorVar;
	compiled.declaration = declaration;
	compiled.values[0] = static_cast<std::int32_t>(internCssAtom(name ? name : ""));
	compiled.aux = hasFallback ? 1 : 0;
	if (hasFallback) {
		compiled.values[1] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(cr, cg, cb));
		compiled.values[2] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeColor(cr, cg, cb));
		compiled.values[3] = clampStaticCssColorChannel(a);
	}
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticFontFamilyCompiledValue(const char *family)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Keyword;
	compiled.declaration = CssDeclarationId::FontFamily;
	compiled.values[0] = gea::framework::graphics::FontRegistry::familyId(family ? family : "");
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticLineHeightCompiledValue(StaticStyleLineHeightKind kind,
                                                 StaticStyleLengthSpec value)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::LineHeight;
	compiled.declaration = CssDeclarationId::LineHeight;
	compiled.lengths[0] = cssLengthSpecForStatic(value);
	switch (kind) {
	case StaticStyleLineHeightKind::Normal:
		compiled.aux = 0;
		break;
	case StaticStyleLineHeightKind::Scalar:
		compiled.aux = 1;
		compiled.lengths[0].unit = CssLengthUnit::Raw;
		break;
	case StaticStyleLineHeightKind::Percent:
		compiled.aux = 2;
		compiled.lengths[0].unit = CssLengthUnit::Percent;
		break;
	case StaticStyleLineHeightKind::Length:
		compiled.aux = 3;
		break;
	}
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticFlexCompiledValue(int grow, StaticStyleLengthSpec basis, bool hasBasis)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Flex;
	compiled.declaration = CssDeclarationId::Flex;
	compiled.values[0] = grow;
	compiled.values[1] = 1; // The compact API uses the initial flex-shrink value.
	compiled.aux = hasBasis ? 1 : 0;
	if (hasBasis) compiled.lengths[0] = cssLengthSpecForStatic(basis);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticBorderCompiledValue(StaticStyleLengthSpec width,
                                             int r,
                                             int g,
                                             int b,
                                             int a)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const int cr = clampStaticCssColorChannel(r);
	const int cg = clampStaticCssColorChannel(g);
	const int cb = clampStaticCssColorChannel(b);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BorderShorthand;
	compiled.declaration = CssDeclarationId::Border;
	compiled.aux = 1;
	compiled.lengths[0] = cssLengthSpecForStatic(width);
	compiled.values[0] = static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(cr, cg, cb));
	compiled.values[1] = clampStaticCssColorChannel(a);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

style_color_t staticNativeColor(StaticStyleColor color)
{
	return gea::framework::graphics::pixel::nativeColor(clampStaticCssColorChannel(color.r),
	                                                    clampStaticCssColorChannel(color.g),
	                                                    clampStaticCssColorChannel(color.b));
}

std::int32_t staticStyleColorValue(StaticStyleColor color)
{
	return static_cast<std::int32_t>(gea::framework::graphics::pixel::nativeStyleValue(
	    clampStaticCssColorChannel(color.r),
	    clampStaticCssColorChannel(color.g),
	    clampStaticCssColorChannel(color.b)));
}

std::uint16_t staticStopPermille(int value, int fallback, int maxValue)
{
	if (value < 0) value = fallback;
	if (value < 0) value = 0;
	if (value > maxValue) value = maxValue;
	return static_cast<std::uint16_t>(value);
}

CssCompiledLinearGradient staticLinearGradient(StaticStyleLinearGradient gradient)
{
	CssCompiledLinearGradient out;
	out.fromStyleColor = staticStyleColorValue(gradient.from);
	out.fromNativeColor = staticNativeColor(gradient.from);
	out.fromAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(gradient.from.a));
	out.toNativeColor = staticNativeColor(gradient.to);
	out.toAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(gradient.to.a));
	out.hasMid = gradient.hasMid ? 1 : 0;
	out.midStopPermille = staticStopPermille(gradient.midStopPermille, 500, 1000);
	int toStop = gradient.toStopPermille;
	if (toStop <= 0) toStop = 1;
	if (gradient.hasMid && toStop <= static_cast<int>(out.midStopPermille))
		toStop = static_cast<int>(out.midStopPermille) + 1;
	out.toStopPermille = staticStopPermille(toStop, 1000, 60000);
	if (gradient.hasMid) {
		out.midNativeColor = staticNativeColor(gradient.mid);
		out.midAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(gradient.mid.a));
	} else {
		out.midNativeColor = 0;
		out.midAlpha = 255;
	}
	out.angleTenths = static_cast<std::int16_t>(std::clamp(gradient.angleTenths, -32768, 32767));
	return out;
}

CssAtomId staticColorRefAtom(StaticStyleColorRef color)
{
	if (!color.varName || color.varName[0] == '\0') return kInvalidCssAtom;
	return internCssAtom(color.varName);
}

void applyStaticLinearGradientColorRef(CssCompiledLinearGradient &out,
                                       StaticStyleColorRef color,
                                       int slot)
{
	const CssAtomId atom = staticColorRefAtom(color);
	const style_color_t nativeColor = staticNativeColor(color.fallback);
	const std::uint8_t alpha = atom == kInvalidCssAtom || color.hasFallback
	    ? static_cast<std::uint8_t>(clampStaticCssColorChannel(color.fallback.a))
	    : 255;
	const std::uint8_t hasFallback = color.hasFallback ? 1 : 0;
	if (slot == 0) {
		out.fromStyleColor = staticStyleColorValue(color.fallback);
		out.fromNativeColor = nativeColor;
		out.fromAlpha = alpha;
		out.fromColorAtom = atom;
		out.fromColorHasFallback = hasFallback;
	} else if (slot == 1) {
		out.midNativeColor = nativeColor;
		out.midAlpha = alpha;
		out.midColorAtom = atom;
		out.midColorHasFallback = hasFallback;
	} else {
		out.toNativeColor = nativeColor;
		out.toAlpha = alpha;
		out.toColorAtom = atom;
		out.toColorHasFallback = hasFallback;
	}
}

CssCompiledLinearGradient staticLinearGradientRef(StaticStyleLinearGradientRef gradient)
{
	CssCompiledLinearGradient out;
	applyStaticLinearGradientColorRef(out, gradient.from, 0);
	applyStaticLinearGradientColorRef(out, gradient.to, 2);
	out.hasMid = gradient.hasMid ? 1 : 0;
	out.midStopPermille = staticStopPermille(gradient.midStopPermille, 500, 1000);
	int toStop = gradient.toStopPermille;
	if (toStop <= 0) toStop = 1;
	if (gradient.hasMid && toStop <= static_cast<int>(out.midStopPermille))
		toStop = static_cast<int>(out.midStopPermille) + 1;
	out.toStopPermille = staticStopPermille(toStop, 1000, 60000);
	if (gradient.hasMid) {
		applyStaticLinearGradientColorRef(out, gradient.mid, 1);
	} else {
		out.midNativeColor = 0;
		out.midAlpha = 255;
	}
	out.angleTenths = static_cast<std::int16_t>(std::clamp(gradient.angleTenths, -32768, 32767));
	return out;
}

void applyStaticRadialGradientColorRef(CssCompiledRadialGradient &out,
                                       StaticStyleColorRef color,
                                       bool from)
{
	const CssAtomId atom = staticColorRefAtom(color);
	const style_color_t nativeColor = staticNativeColor(color.fallback);
	const std::uint8_t alpha = atom == kInvalidCssAtom || color.hasFallback
	    ? static_cast<std::uint8_t>(clampStaticCssColorChannel(color.fallback.a))
	    : 255;
	if (from) {
		out.fromNativeColor = nativeColor;
		out.fromAlpha = alpha;
		out.fromColorAtom = atom;
		out.fromColorHasFallback = color.hasFallback ? 1 : 0;
	} else {
		out.toNativeColor = nativeColor;
		out.toAlpha = alpha;
		out.toColorAtom = atom;
		out.toColorHasFallback = color.hasFallback ? 1 : 0;
	}
}

CssCompiledRadialGradient staticRadialGradientRef(StaticStyleRadialGradientRef gradient)
{
	CssCompiledRadialGradient out;
	applyStaticRadialGradientColorRef(out, gradient.from, true);
	applyStaticRadialGradientColorRef(out, gradient.to, false);
	int stop = gradient.stopPermille;
	if (stop <= 0) stop = 1;
	out.stopPermille = staticStopPermille(stop, 1000, 1000);
	out.cxPermille = static_cast<std::int16_t>(std::clamp(gradient.cxPermille, -32768, 32767));
	out.cyPermille = static_cast<std::int16_t>(std::clamp(gradient.cyPermille, -32768, 32767));
	out.rxPermille = static_cast<std::int16_t>(std::clamp(gradient.rxPermille, -32768, 32767));
	out.ryPermille = static_cast<std::int16_t>(std::clamp(gradient.ryPermille, -32768, 32767));
	return out;
}

void applyStaticBackgroundGridLine(CssCompiledBackground &background,
                                   StaticStyleBackgroundGridLine line,
                                   bool xAxis)
{
	if (!line.enabled) return;
	background.gridAxes |= xAxis ? 1 : 2;
	background.gridColor = staticNativeColor(line.color);
	background.gridAlpha = static_cast<std::uint8_t>(clampStaticCssColorChannel(line.color.a));
	if (xAxis) {
		background.gridLineX = cssLengthSpecForStatic(line.width);
		background.hasGridLineX = 1;
	} else {
		background.gridLineY = cssLengthSpecForStatic(line.width);
		background.hasGridLineY = 1;
	}
}

std::uint16_t storeStaticBackgroundCompiledValue(StaticStyleLinearGradient gradient,
                                                 StaticStyleLinearGradient overlayGradient,
                                                 bool hasOverlayGradient,
                                                 StaticStyleBackgroundGridLine gridX,
                                                 StaticStyleBackgroundGridLine gridY, bool imageOnly)
{
	auto &values = compiledCssValues();
	auto &backgrounds = compiledCssBackgrounds();
	if (values.size() >= kNoCompiledCssValue || backgrounds.size() >= kNoCompiledCssBackground)
		return kNoCompiledCssValue;

	CssCompiledBackground background;
	background.gradient = staticLinearGradient(gradient);
	background.hasGradient = 1;
	if (hasOverlayGradient) {
		background.overlayGradient = staticLinearGradient(overlayGradient);
		background.hasOverlayGradient = 1;
	}
	applyStaticBackgroundGridLine(background, gridX, true);
	applyStaticBackgroundGridLine(background, gridY, false);

	background.layerCount = 1 + background.hasOverlayGradient + background.hasRadialGradient + background.hasGridLineX + background.hasGridLineY;
	background.gradientLayer = background.layerCount-1;
	background.overlayLayer = 0;
	background.radialLayer = background.hasOverlayGradient ? 1 : 0;
	backgrounds.push_back(background);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Background;
	compiled.declaration = imageOnly ? CssDeclarationId::BackgroundImage : CssDeclarationId::Background;
	compiled.values[0] = static_cast<std::int32_t>(backgrounds.size() - 1);
	values.push_back(compiled);
	return static_cast<std::uint16_t>(values.size() - 1);
}

std::uint16_t storeStaticBackgroundFullCompiledValue(StaticStyleLinearGradientRef gradient,
                                                     StaticStyleLinearGradientRef overlayGradient,
                                                     bool hasOverlayGradient,
                                                     StaticStyleRadialGradientRef radialGradient,
                                                     StaticStyleBackgroundGridLine gridX,
                                                     StaticStyleBackgroundGridLine gridY, bool imageOnly)
{
	auto &values = compiledCssValues();
	auto &backgrounds = compiledCssBackgrounds();
	if (values.size() >= kNoCompiledCssValue || backgrounds.size() >= kNoCompiledCssBackground)
		return kNoCompiledCssValue;

	CssCompiledBackground background;
	background.gradient = staticLinearGradientRef(gradient);
	background.hasGradient = 1;
	if (hasOverlayGradient) {
		background.overlayGradient = staticLinearGradientRef(overlayGradient);
		background.hasOverlayGradient = 1;
	}
	if (radialGradient.enabled) {
		background.radialGradient = staticRadialGradientRef(radialGradient);
		background.hasRadialGradient = 1;
	}
	applyStaticBackgroundGridLine(background, gridX, true);
	applyStaticBackgroundGridLine(background, gridY, false);

	background.layerCount = 1 + background.hasOverlayGradient + background.hasRadialGradient + background.hasGridLineX + background.hasGridLineY;
	background.gradientLayer = background.layerCount-1;
	background.overlayLayer = 0;
	background.radialLayer = background.hasOverlayGradient ? 1 : 0;
	backgrounds.push_back(background);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Background;
	compiled.declaration = imageOnly ? CssDeclarationId::BackgroundImage : CssDeclarationId::Background;
	compiled.values[0] = static_cast<std::int32_t>(backgrounds.size() - 1);
	values.push_back(compiled);
	return static_cast<std::uint16_t>(values.size() - 1);
}

std::uint16_t storeStaticBackgroundSizeCompiledValue(StaticStyleLengthSpec stepX,
                                                     StaticStyleLengthSpec stepY)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::DirectProperty;
	compiled.declaration = CssDeclarationId::BackgroundSize;
	compiled.values[0] = static_cast<int>(Property::BackgroundSizeList);
	compiled.values[1] = storeBackgroundPlacementList({{cssLengthSpecForStatic(stepX), cssLengthSpecForStatic(stepY)}});
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticCustomLengthCompiledValue(StaticStyleLengthSpec length)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Length;
	compiled.declaration = CssDeclarationId::Custom;
	compiled.lengths[0] = cssLengthSpecForStatic(length);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::vector<CssCompiledBackground> &compiledCssBackgrounds()
{
	static std::vector<CssCompiledBackground> list;
	return list;
}

std::vector<std::vector<std::uint8_t>> &backgroundClipLists()
{
	static std::vector<std::vector<std::uint8_t>> lists;
	return lists;
}

void clearCompiledCssBackgrounds()
{
	compiledCssBackgrounds().clear();
	backgroundClipLists().clear();
	backgroundPlacementLists().clear();
}

std::vector<CssCompiledGridTemplate> &compiledCssGridTemplates()
{
	static std::vector<CssCompiledGridTemplate> list;
	return list;
}

void clearCompiledCssGridTemplates()
{
	compiledCssGridTemplates().clear();
}

std::vector<std::pair<CssLengthSpec, int>> &boxLengthExpressionCache()
{
	static std::vector<std::pair<CssLengthSpec, int>> cache;
	return cache;
}

std::vector<CssLengthExpression> &compiledCssLengthExpressions()
{
	static std::vector<CssLengthExpression> list;
	return list;
}

struct StaticLengthExpressionResolution {
	ResolvedCssLength value;
	std::uint8_t status = 0;  // 0 unknown, 1 dynamic, 2 cacheable
	std::uint8_t valid = 0;
};

struct DynamicLengthExpressionResolution {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	ResolvedCssLength value;
	std::uint16_t handle;
	std::int16_t nodeId;
	int basis;
	std::uint8_t axis;
	std::uint8_t valid;

	// Runtime initialization keeps the nonzero sentinels out of .data (see
	// state_init.h); noinline stops the compiler folding them back in.
	__attribute__((noinline)) DynamicLengthExpressionResolution()
	{
		value = ResolvedCssLength{};
		handle = kNoCompiledCssLengthExpression;
		nodeId = -1;
		basis = 0;
		axis = 0;
		valid = 0;
	}
#else
	ResolvedCssLength value;
	std::uint16_t handle = kNoCompiledCssLengthExpression;
	std::int16_t nodeId = -1;
	int basis = 0;
	std::uint8_t axis = 0;
	std::uint8_t valid = 0;
#endif
};

constexpr std::uint8_t kDynamicLengthExpressionResolutionCacheSize = 64;

std::vector<StaticLengthExpressionResolution> &staticLengthExpressionResolutionCache()
{
	static std::vector<StaticLengthExpressionResolution> list;
	return list;
}

DynamicLengthExpressionResolution (&dynamicLengthExpressionResolutionCache())[kDynamicLengthExpressionResolutionCacheSize]
{
	static DynamicLengthExpressionResolution cache[kDynamicLengthExpressionResolutionCacheSize];
	return cache;
}

std::uint8_t &dynamicLengthExpressionResolutionCacheCursor()
{
	static std::uint8_t cursor = 0;
	return cursor;
}

void clearDynamicLengthExpressionResolutionCache()
{
	for (DynamicLengthExpressionResolution &entry : dynamicLengthExpressionResolutionCache())
		entry.valid = 0;
	dynamicLengthExpressionResolutionCacheCursor() = 0;
}

void clearStaticLengthExpressionResolutionCache()
{
	staticLengthExpressionResolutionCache().clear();
	clearDynamicLengthExpressionResolutionCache();
}

void clearCompiledCssLengthExpressions()
{
	compiledCssLengthExpressions().clear();
	boxLengthExpressionCache().clear();
	clearStaticLengthExpressionResolutionCache();
}

std::unordered_map<std::string, CssLengthSpec> &compiledCssLengthCache()
{
	static std::unordered_map<std::string, CssLengthSpec> cache;
	return cache;
}

void clearCompiledCssLengthCache()
{
	compiledCssLengthCache().clear();
}

std::unordered_map<std::string, CachedCssColor> &compiledCssColorCache()
{
	static std::unordered_map<std::string, CachedCssColor> cache;
	return cache;
}

void clearCompiledCssColorCache()
{
	compiledCssColorCache().clear();
}

bool compiledLinearGradientNeedsTextFallback(const CssCompiledLinearGradient &gradient)
{
	(void)gradient;
	return false;
}

bool compiledRadialGradientNeedsTextFallback(const CssCompiledRadialGradient &gradient)
{
	(void)gradient;
	return false;
}

bool compiledBackgroundNeedsTextFallback(const CssCompiledBackground &background)
{
	return compiledLinearGradientNeedsTextFallback(background.gradient) ||
	       (background.hasOverlayGradient &&
	        compiledLinearGradientNeedsTextFallback(background.overlayGradient)) ||
	       (background.hasRadialGradient &&
	        compiledRadialGradientNeedsTextFallback(background.radialGradient));
}

bool compiledCssValueCanSkipRuleText(std::uint16_t handle)
{
	const auto &values = compiledCssValues();
	if (handle >= values.size()) return false;
	const CssCompiledValue &compiled = values[handle];
	switch (compiled.kind) {
	case CssCompiledKind::None:
		return false;
	case CssCompiledKind::Background: {
		const auto &backgrounds = compiledCssBackgrounds();
		const std::uint16_t backgroundHandle = static_cast<std::uint16_t>(compiled.values[0]);
		if (backgroundHandle >= backgrounds.size()) return false;
		return !compiledBackgroundNeedsTextFallback(backgrounds[backgroundHandle]);
	}
	default:
		return true;
	}
}

bool compiledCssValueCanSkipKeyframeText(std::uint16_t handle)
{
	const auto &values = compiledCssValues();
	if (handle >= values.size()) return false;
	const CssCompiledValue &compiled = values[handle];
	switch (compiled.kind) {
	case CssCompiledKind::DirectProperty:
	case CssCompiledKind::Noop:
	case CssCompiledKind::Opacity:
	case CssCompiledKind::Transform:
	case CssCompiledKind::FilterBlur:
		return true;
	case CssCompiledKind::Rotate:
		return compiled.declaration == CssDeclarationId::Rotate;
	case CssCompiledKind::Scale:
		return compiled.declaration == CssDeclarationId::Scale;
	case CssCompiledKind::Color:
		return compiled.declaration == CssDeclarationId::Background ||
		       compiled.declaration == CssDeclarationId::BackgroundColor ||
		       compiled.declaration == CssDeclarationId::Color;
	case CssCompiledKind::ColorVar:
		return compiled.declaration == CssDeclarationId::Background ||
		       compiled.declaration == CssDeclarationId::BackgroundColor ||
		       compiled.declaration == CssDeclarationId::Color;
	case CssCompiledKind::Length:
	case CssCompiledKind::Size:
	case CssCompiledKind::PositionOffset:
		return compiled.declaration == CssDeclarationId::Width ||
		       compiled.declaration == CssDeclarationId::Height ||
		       compiled.declaration == CssDeclarationId::Left ||
		       compiled.declaration == CssDeclarationId::Top;
	default:
		return false;
	}
}

CssRule makeCssRule(CssRule::SelectorType selectorType,
                    CssRule::PseudoElement pseudoElement,
                    CssText selector,
                    CssText property,
	CssText value,
	CssText media)
{
	const CssDeclarationId declaration = classifyDeclaration(property);
	const CssAtomId selectorAtom = selectorType == CssRule::SelectorType::Class ? atomForText(selector) : kInvalidCssAtom;
	const int16_t selectorTagId = selectorType == CssRule::SelectorType::Element ? internTag(selector.c_str()) : -1;
	const std::uint16_t selectorPlan = selectorType == CssRule::SelectorType::Selector ? compileSelectorPlan(selector) : kNoSelectorPlan;
	const CssAtomId propertyAtom = declaration == CssDeclarationId::Custom
	    ? atomForText(property)
	    : kInvalidCssAtom;
	const std::uint16_t compiledValue = declaration == CssDeclarationId::Custom
	    ? compileCustomPropertyValue(value)
	    : compileCssValue(declaration, value);
	const std::uint16_t compiledAnimationSpec = declaration == CssDeclarationId::Animation
	    ? compileCssAnimationSpec(value)
	    : kNoCompiledCssAnimationSpec;
	const std::uint16_t mediaPlan = compileMediaConditionPlan(media);
	const bool keepValueText =
	    declaration == CssDeclarationId::Custom ||
	    (declaration == CssDeclarationId::Animation && compiledAnimationSpec == kNoCompiledCssAnimationSpec) ||
	    (declaration != CssDeclarationId::Animation &&
	     !compiledCssValueCanSkipRuleText(compiledValue));
	const bool keepPropertyText =
	    keepValueText &&
	    declaration != CssDeclarationId::Custom &&
	    declaration != CssDeclarationId::Animation;
	const std::uint16_t propertyText = keepPropertyText
	    ? storeCssRuleText(std::move(property))
	    : kNoCssRuleText;
	const std::uint16_t valueText = keepValueText
	    ? storeCssRuleText(std::move(value))
	    : kNoCssRuleText;
	const std::uint16_t mediaText = mediaPlan == kNoMediaConditionPlan
	    ? storeCssRuleText(std::move(media))
	    : kNoCssRuleText;
	CssRule rule{selectorType, pseudoElement, rulePropertyKind(declaration), declaration, compiledValue,
	             compiledAnimationSpec,
	             selectorAtom, propertyAtom, selectorTagId, selectorPlan, mediaPlan,
	             propertyText, valueText, mediaText};
	return rule;
}

CssRule::SelectorType cssRuleSelectorTypeForStaticKind(StaticStyleSelectorKind kind)
{
	switch (kind) {
	case StaticStyleSelectorKind::Class: return CssRule::SelectorType::Class;
	case StaticStyleSelectorKind::Element: return CssRule::SelectorType::Element;
	case StaticStyleSelectorKind::Selector: return CssRule::SelectorType::Selector;
	}
	return CssRule::SelectorType::Selector;
}

CssDeclarationId declarationForStaticColorProperty(StaticStyleColorProperty property)
{
	switch (property) {
	case StaticStyleColorProperty::Color: return CssDeclarationId::Color;
	case StaticStyleColorProperty::Background: return CssDeclarationId::Background;
	case StaticStyleColorProperty::BackgroundColor: return CssDeclarationId::BackgroundColor;
	case StaticStyleColorProperty::ActiveBackground: return CssDeclarationId::ActiveBackgroundColor;
	case StaticStyleColorProperty::Border: return CssDeclarationId::BorderColor;
	case StaticStyleColorProperty::BorderTop: return CssDeclarationId::BorderTopColor;
	case StaticStyleColorProperty::BorderRight: return CssDeclarationId::BorderRightColor;
	case StaticStyleColorProperty::BorderBottom: return CssDeclarationId::BorderBottomColor;
	case StaticStyleColorProperty::BorderLeft: return CssDeclarationId::BorderLeftColor;
	}
	return CssDeclarationId::Color;
}

CssLengthUnit cssLengthUnitForStatic(StaticStyleLengthUnit unit)
{
	switch (unit) {
	case StaticStyleLengthUnit::Raw: return CssLengthUnit::Raw;
	case StaticStyleLengthUnit::Px: return CssLengthUnit::Px;
	case StaticStyleLengthUnit::Percent: return CssLengthUnit::Percent;
	case StaticStyleLengthUnit::Vw: return CssLengthUnit::Vw;
	case StaticStyleLengthUnit::Vh: return CssLengthUnit::Vh;
	case StaticStyleLengthUnit::Vmin: return CssLengthUnit::Vmin;
	case StaticStyleLengthUnit::Vmax: return CssLengthUnit::Vmax;
	case StaticStyleLengthUnit::Dvw: return CssLengthUnit::Dvw;
	case StaticStyleLengthUnit::Dvh: return CssLengthUnit::Dvh;
	case StaticStyleLengthUnit::Auto: return CssLengthUnit::Auto;
	case StaticStyleLengthUnit::Expression: return CssLengthUnit::Expression;
	}
	return CssLengthUnit::Invalid;
}

CssLengthSpec cssLengthSpecForStatic(StaticStyleLengthSpec spec)
{
	CssLengthSpec out;
	out.unit = cssLengthUnitForStatic(spec.unit);
	out.value = spec.value;
	return out;
}

CssLengthExpressionKind cssLengthExpressionKindForStatic(StaticStyleLengthExpressionKind kind)
{
	switch (kind) {
	case StaticStyleLengthExpressionKind::Add: return CssLengthExpressionKind::Add;
	case StaticStyleLengthExpressionKind::Subtract: return CssLengthExpressionKind::Subtract;
	case StaticStyleLengthExpressionKind::Multiply: return CssLengthExpressionKind::Multiply;
	case StaticStyleLengthExpressionKind::Divide: return CssLengthExpressionKind::Divide;
	case StaticStyleLengthExpressionKind::Min: return CssLengthExpressionKind::Min;
	case StaticStyleLengthExpressionKind::Max: return CssLengthExpressionKind::Max;
	case StaticStyleLengthExpressionKind::Clamp: return CssLengthExpressionKind::Clamp;
	case StaticStyleLengthExpressionKind::Var: return CssLengthExpressionKind::Var;
	}
	return CssLengthExpressionKind::Add;
}

CssDeclarationId declarationForStaticLengthProperty(StaticStyleLengthProperty property)
{
	switch (property) {
	case StaticStyleLengthProperty::Gap: return CssDeclarationId::Gap;
	case StaticStyleLengthProperty::Width: return CssDeclarationId::Width;
	case StaticStyleLengthProperty::Height: return CssDeclarationId::Height;
	case StaticStyleLengthProperty::MinWidth: return CssDeclarationId::MinWidth;
	case StaticStyleLengthProperty::MinHeight: return CssDeclarationId::MinHeight;
	case StaticStyleLengthProperty::MaxWidth: return CssDeclarationId::MaxWidth;
	case StaticStyleLengthProperty::MaxHeight: return CssDeclarationId::MaxHeight;
	case StaticStyleLengthProperty::FlexBasis: return CssDeclarationId::FlexBasis;
	case StaticStyleLengthProperty::PaddingTop: return CssDeclarationId::PaddingTop;
	case StaticStyleLengthProperty::PaddingRight: return CssDeclarationId::PaddingRight;
	case StaticStyleLengthProperty::PaddingBottom: return CssDeclarationId::PaddingBottom;
	case StaticStyleLengthProperty::PaddingLeft: return CssDeclarationId::PaddingLeft;
	case StaticStyleLengthProperty::MarginTop: return CssDeclarationId::MarginTop;
	case StaticStyleLengthProperty::MarginRight: return CssDeclarationId::MarginRight;
	case StaticStyleLengthProperty::MarginBottom: return CssDeclarationId::MarginBottom;
	case StaticStyleLengthProperty::MarginLeft: return CssDeclarationId::MarginLeft;
	case StaticStyleLengthProperty::BorderWidth: return CssDeclarationId::BorderWidth;
	case StaticStyleLengthProperty::BorderTopWidth: return CssDeclarationId::BorderTopWidth;
	case StaticStyleLengthProperty::BorderRightWidth: return CssDeclarationId::BorderRightWidth;
	case StaticStyleLengthProperty::BorderBottomWidth: return CssDeclarationId::BorderBottomWidth;
	case StaticStyleLengthProperty::BorderLeftWidth: return CssDeclarationId::BorderLeftWidth;
	case StaticStyleLengthProperty::FontSize: return CssDeclarationId::FontSize;
	case StaticStyleLengthProperty::Perspective: return CssDeclarationId::Perspective;
	case StaticStyleLengthProperty::MaskImage: return CssDeclarationId::MaskImage;
	case StaticStyleLengthProperty::Top: return CssDeclarationId::Top;
	case StaticStyleLengthProperty::Right: return CssDeclarationId::Right;
	case StaticStyleLengthProperty::Bottom: return CssDeclarationId::Bottom;
	case StaticStyleLengthProperty::Left: return CssDeclarationId::Left;
	}
	return CssDeclarationId::Unknown;
}

CssDeclarationId declarationForStaticBorderRadiusCorner(StaticStyleBorderRadiusCorner corner)
{
	switch (corner) {
	case StaticStyleBorderRadiusCorner::TopLeft: return CssDeclarationId::BorderTopLeftRadius;
	case StaticStyleBorderRadiusCorner::TopRight: return CssDeclarationId::BorderTopRightRadius;
	case StaticStyleBorderRadiusCorner::BottomRight: return CssDeclarationId::BorderBottomRightRadius;
	case StaticStyleBorderRadiusCorner::BottomLeft: return CssDeclarationId::BorderBottomLeftRadius;
	}
	return CssDeclarationId::Unknown;
}

CssCompiledKind compiledKindForStaticLengthProperty(StaticStyleLengthProperty property)
{
	switch (property) {
	case StaticStyleLengthProperty::Width:
	case StaticStyleLengthProperty::Height:
		return CssCompiledKind::Size;
	case StaticStyleLengthProperty::Top:
	case StaticStyleLengthProperty::Right:
	case StaticStyleLengthProperty::Bottom:
	case StaticStyleLengthProperty::Left:
		return CssCompiledKind::PositionOffset;
	case StaticStyleLengthProperty::FlexBasis:
		return CssCompiledKind::FlexBasis;
	default:
		return CssCompiledKind::Length;
	}
}

std::uint16_t storeStaticLengthCompiledValue(StaticStyleLengthProperty property,
                                             StaticStyleLengthSpec length)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	const CssLengthUnit lengthUnit = cssLengthUnitForStatic(length.unit);
	if (declaration == CssDeclarationId::Unknown || lengthUnit == CssLengthUnit::Invalid)
		return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = compiledKindForStaticLengthProperty(property);
	compiled.declaration = declaration;
	compiled.lengths[0] = cssLengthSpecForStatic(length);
	if (compiled.kind == CssCompiledKind::FlexBasis)
		compiled.aux = lengthUnit == CssLengthUnit::Auto ? 0 : 1;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticBorderRadiusCompiledValue(CssDeclarationId declaration,
                                                   StaticStyleLengthSpec topLeft,
                                                   StaticStyleLengthSpec topRight,
                                                   StaticStyleLengthSpec bottomRight,
                                                   StaticStyleLengthSpec bottomLeft)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	if (declaration == CssDeclarationId::Unknown) return kNoCompiledCssValue;
	if (cssLengthUnitForStatic(topLeft.unit) == CssLengthUnit::Invalid ||
	    cssLengthUnitForStatic(topRight.unit) == CssLengthUnit::Invalid ||
	    cssLengthUnitForStatic(bottomRight.unit) == CssLengthUnit::Invalid ||
	    cssLengthUnitForStatic(bottomLeft.unit) == CssLengthUnit::Invalid)
		return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BorderRadius;
	compiled.declaration = declaration;
	compiled.lengths[0] = cssLengthSpecForStatic(topLeft);
	compiled.lengths[1] = cssLengthSpecForStatic(topRight);
	compiled.lengths[2] = cssLengthSpecForStatic(bottomRight);
	compiled.lengths[3] = cssLengthSpecForStatic(bottomLeft);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticLengthCompiledValue(StaticStyleLengthProperty property,
                                             StaticStyleLengthUnit unit,
                                             float value)
{
	return storeStaticLengthCompiledValue(property, StaticStyleLengthSpec{unit, value});
}

std::uint16_t storeStaticTransformCompiledValue(std::uint16_t flags,
                                                int rotateX,
                                                int rotateY,
                                                int rotateZ,
                                                StaticStyleLengthSpec translateX,
                                                StaticStyleLengthSpec translateY,
                                                StaticStyleLengthSpec translateZ,
                                                int scaleX,
                                                int scaleY,
                                                int scaleZ)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::Transform;
	compiled.declaration = CssDeclarationId::Transform;
	compiled.flags = flags;
	compiled.values[0] = rotateX;
	compiled.values[1] = rotateY;
	compiled.values[2] = rotateZ;
	compiled.lengths[0] = cssLengthSpecForStatic(translateX);
	compiled.lengths[1] = cssLengthSpecForStatic(translateY);
	compiled.lengths[2] = cssLengthSpecForStatic(translateZ);
	compiled.values[8] = scaleX;
	compiled.values[9] = scaleY;
	compiled.values[10] = scaleZ;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticFilterBlurCompiledValue(StaticStyleLengthSpec radius)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::FilterBlur;
	compiled.declaration = CssDeclarationId::Filter;
	compiled.aux = 1;
	compiled.lengths[0] = cssLengthSpecForStatic(radius);
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t storeStaticBoxShadowNoneCompiledValue()
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::BoxShadow;
	compiled.declaration = CssDeclarationId::BoxShadow;
	compiled.aux = 0;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

CssDeclarationId declarationForStaticOriginProperty(StaticStyleOriginProperty property)
{
	switch (property) {
	case StaticStyleOriginProperty::TransformOrigin: return CssDeclarationId::TransformOrigin;
	case StaticStyleOriginProperty::PerspectiveOrigin: return CssDeclarationId::PerspectiveOrigin;
	}
	return CssDeclarationId::Unknown;
}

std::uint16_t storeStaticOriginCompiledValue(CssDeclarationId declaration, int xPermille, int yPermille)
{
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	if (declaration == CssDeclarationId::Unknown) return kNoCompiledCssValue;
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::OriginPair;
	compiled.declaration = declaration;
	compiled.values[0] = xPermille;
	compiled.values[1] = yPermille;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

CssDeclarationId declarationForStaticGridTemplateProperty(StaticStyleGridTemplateProperty property)
{
	switch (property) {
	case StaticStyleGridTemplateProperty::Columns: return CssDeclarationId::GridTemplateColumns;
	case StaticStyleGridTemplateProperty::Rows: return CssDeclarationId::GridTemplateRows;
	}
	return CssDeclarationId::Unknown;
}

std::uint16_t storeStaticGridTemplateCompiledValue(CssDeclarationId declaration,
                                                   std::initializer_list<StaticStyleGridTemplateTrack> tracks)
{
	auto &values = compiledCssValues();
	auto &grids = compiledCssGridTemplates();
	if (values.size() >= kNoCompiledCssValue || grids.size() >= kNoCompiledCssGridTemplate)
		return kNoCompiledCssValue;
	if (declaration == CssDeclarationId::Unknown) return kNoCompiledCssValue;

	CssCompiledGridTemplate grid;
	for (const StaticStyleGridTemplateTrack &staticTrack : tracks) {
		if (grid.count >= kMaxGridTracks) break;
		CssCompiledGridTrack track;
		track.type = static_cast<std::int8_t>(std::clamp(staticTrack.type, 0, 2));
		track.value = static_cast<std::int16_t>(std::clamp(staticTrack.value, -32768, 32767));
		track.length = cssLengthSpecForStatic(staticTrack.length);
		grid.tracks[grid.count++] = track;
	}

	grids.push_back(grid);
	CssCompiledValue compiled;
	compiled.kind = CssCompiledKind::GridTemplate;
	compiled.declaration = declaration;
	compiled.values[0] = static_cast<std::int32_t>(grids.size() - 1);
	values.push_back(compiled);
	return static_cast<std::uint16_t>(values.size() - 1);
}

CssRule makeStaticCompiledCssRule(StaticStyleSelectorKind selectorKind,
                                  const char *selector,
                                  CssRuleProperty propertyKind,
                                  CssDeclarationId declaration,
                                  std::uint16_t compiledValue,
                                  const char *media)
{
	const CssRule::SelectorType selectorType = cssRuleSelectorTypeForStaticKind(selectorKind);
	CssRule::PseudoElement pseudo = CssRule::PseudoElement::None;
	CssText selectorText = CssText::literal(selector);
	if (selectorType == CssRule::SelectorType::Selector) {
		const char *raw = selector ? selector : "";
		const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(raw, std::strlen(raw));
		pseudo = selectorSlice.pseudo;
		selectorText = CssText::view(selectorSlice.data, selectorSlice.length);
	}
	const CssAtomId selectorAtom = selectorType == CssRule::SelectorType::Class
	    ? atomForText(selectorText)
	    : kInvalidCssAtom;
	const int16_t selectorTagId = selectorType == CssRule::SelectorType::Element
	    ? internTag(selectorText.c_str())
	    : -1;
	const std::uint16_t selectorPlan = selectorType == CssRule::SelectorType::Selector
	    ? compileSelectorPlan(selectorText)
	    : kNoSelectorPlan;
	CssText mediaTextValue = CssText::literal(media);
	const std::uint16_t mediaPlan = compileMediaConditionPlan(mediaTextValue);
	const std::uint16_t storedMediaText = mediaPlan == kNoMediaConditionPlan
	    ? storeCssRuleText(std::move(mediaTextValue))
	    : kNoCssRuleText;
	CssRule rule{selectorType,
	             pseudo,
	             propertyKind,
	             declaration,
	             compiledValue,
	             kNoCompiledCssAnimationSpec,
	             selectorAtom,
	             kInvalidCssAtom,
	             selectorTagId,
	             selectorPlan,
	             mediaPlan,
	             kNoCssRuleText,
	             kNoCssRuleText,
	             storedMediaText};
	return rule;
}

CssRule makeDirectPropertyCssRule(StaticStyleSelectorKind selectorKind,
                                  const char *selector,
                                  Property property,
                                  int value,
                                  const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 CssRuleProperty::Other,
	                                 CssDeclarationId::Ignored,
	                                 storeDirectPropertyCompiledValue(property, value),
	                                 media);
}

CssRule makeDirectPropertyGroupCssRule(StaticStyleSelectorKind selectorKind,
                                       const char *selector,
                                       std::initializer_list<StaticStylePropertyValue> properties,
                                       const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 CssRuleProperty::Other,
	                                 CssDeclarationId::Ignored,
	                                 storeDirectPropertyGroupCompiledValue(properties),
	                                 media);
}

CssRule makeStaticColorCssRule(StaticStyleSelectorKind selectorKind,
                               const char *selector,
                               StaticStyleColorProperty property,
                               int r,
                               int g,
                               int b,
                               int a,
                               const char *media)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticColorCompiledValue(declaration, r, g, b, a),
	                                 media);
}

CssRule makeStaticColorVarCssRule(StaticStyleSelectorKind selectorKind,
                                  const char *selector,
                                  StaticStyleColorProperty property,
                                  const char *name,
                                  bool hasFallback,
                                  int r,
                                  int g,
                                  int b,
                                  int a,
                                  const char *media)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticColorVarCompiledValue(declaration,
	                                                                  name,
	                                                                  hasFallback,
	                                                                  r,
	                                                                  g,
	                                                                  b,
	                                                                  a),
	                                 media);
}

CssRule makeStaticLengthCssRule(StaticStyleSelectorKind selectorKind,
                                const char *selector,
                                StaticStyleLengthProperty property,
                                StaticStyleLengthUnit unit,
                                float value,
                                const char *media)
{
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticLengthCompiledValue(property, unit, value),
	                                 media);
}

CssRule makeStaticLengthSpecCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLengthProperty property,
                                    StaticStyleLengthSpec length,
                                    const char *media)
{
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticLengthCompiledValue(property, length),
	                                 media);
}

CssRule makeStaticFontFamilyCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    const char *family,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::FontFamily),
	                                 CssDeclarationId::FontFamily,
	                                 storeStaticFontFamilyCompiledValue(family),
	                                 media);
}

CssRule makeStaticLineHeightCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLineHeightKind kind,
                                    StaticStyleLengthSpec value,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::LineHeight),
	                                 CssDeclarationId::LineHeight,
	                                 storeStaticLineHeightCompiledValue(kind, value),
	                                 media);
}

CssRule makeStaticFlexCssRule(StaticStyleSelectorKind selectorKind,
                              const char *selector,
                              int grow,
                              StaticStyleLengthSpec basis,
                              bool hasBasis,
                              const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Flex),
	                                 CssDeclarationId::Flex,
	                                 storeStaticFlexCompiledValue(grow, basis, hasBasis),
	                                 media);
}

CssRule makeStaticBorderCssRule(StaticStyleSelectorKind selectorKind,
                                const char *selector,
                                StaticStyleLengthSpec width,
                                int r,
                                int g,
                                int b,
                                int a,
                                const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Border),
	                                 CssDeclarationId::Border,
	                                 storeStaticBorderCompiledValue(width, r, g, b, a),
	                                 media);
}

CssRule makeStaticBorderRadiusCssRule(StaticStyleSelectorKind selectorKind,
                                      const char *selector,
                                      StaticStyleLengthSpec topLeft,
                                      StaticStyleLengthSpec topRight,
                                      StaticStyleLengthSpec bottomRight,
                                      StaticStyleLengthSpec bottomLeft,
                                      const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::BorderRadius),
	                                 CssDeclarationId::BorderRadius,
	                                 storeStaticBorderRadiusCompiledValue(CssDeclarationId::BorderRadius,
	                                                                      topLeft,
	                                                                      topRight,
	                                                                      bottomRight,
	                                                                      bottomLeft),
	                                 media);
}

CssRule makeStaticBorderRadiusCornerCssRule(StaticStyleSelectorKind selectorKind,
                                            const char *selector,
                                            StaticStyleBorderRadiusCorner corner,
                                            StaticStyleLengthSpec radius,
                                            const char *media)
{
	const CssDeclarationId declaration = declarationForStaticBorderRadiusCorner(corner);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticBorderRadiusCompiledValue(declaration,
	                                                                      radius,
	                                                                      radius,
	                                                                      radius,
	                                                                      radius),
	                                 media);
}

CssRule makeStaticFilterBlurCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLengthSpec radius,
                                    const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Filter),
	                                 CssDeclarationId::Filter,
	                                 storeStaticFilterBlurCompiledValue(radius),
	                                 media);
}

CssRule makeStaticBoxShadowNoneCssRule(StaticStyleSelectorKind selectorKind,
                                       const char *selector,
                                       const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::BoxShadow),
	                                 CssDeclarationId::BoxShadow,
	                                 storeStaticBoxShadowNoneCompiledValue(),
	                                 media);
}

CssRule makeStaticBackgroundCssRule(StaticStyleSelectorKind selectorKind,
                                    const char *selector,
                                    StaticStyleLinearGradient gradient,
                                    StaticStyleLinearGradient overlayGradient,
                                    bool hasOverlayGradient,
                                    StaticStyleBackgroundGridLine gridX,
                                    StaticStyleBackgroundGridLine gridY,
                                    const char *media, bool imageOnly)
{
	const auto declaration = imageOnly ? CssDeclarationId::BackgroundImage : CssDeclarationId::Background;
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticBackgroundCompiledValue(gradient,
	                                                                    overlayGradient,
	                                                                    hasOverlayGradient,
	                                                                    gridX,
	                                                                    gridY, imageOnly),
	                                 media);
}

CssRule makeStaticBackgroundFullCssRule(StaticStyleSelectorKind selectorKind,
                                        const char *selector,
                                        StaticStyleLinearGradientRef gradient,
                                        StaticStyleLinearGradientRef overlayGradient,
                                        bool hasOverlayGradient,
                                        StaticStyleRadialGradientRef radialGradient,
                                        StaticStyleBackgroundGridLine gridX,
                                        StaticStyleBackgroundGridLine gridY,
                                        const char *media, bool imageOnly)
{
	const auto declaration = imageOnly ? CssDeclarationId::BackgroundImage : CssDeclarationId::Background;
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticBackgroundFullCompiledValue(gradient,
	                                                                        overlayGradient,
	                                                                        hasOverlayGradient,
	                                                                        radialGradient,
	                                                                        gridX,
	                                                                        gridY, imageOnly),
	                                 media);
}

CssRule makeStaticBackgroundSizeCssRule(StaticStyleSelectorKind selectorKind,
                                        const char *selector,
                                        StaticStyleLengthSpec stepX,
                                        StaticStyleLengthSpec stepY,
                                        const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::BackgroundSize),
	                                 CssDeclarationId::BackgroundSize,
	                                 storeStaticBackgroundSizeCompiledValue(stepX, stepY),
	                                 media);
}

CssRule makeStaticCustomLengthCssRule(StaticStyleSelectorKind selectorKind,
                                      const char *selector,
                                      const char *name,
                                      StaticStyleLengthSpec length,
                                      const char *media)
{
	CssRule rule = makeStaticCompiledCssRule(selectorKind,
	                                        selector,
	                                        CssRuleProperty::Custom,
	                                        CssDeclarationId::Custom,
	                                        storeStaticCustomLengthCompiledValue(length),
	                                        media);
	rule.propertyAtom = internCssAtom(name ? name : "");
	return rule;
}

CssRule makeStaticCustomColorCssRule(StaticStyleSelectorKind selectorKind,
                                     const char *selector,
                                     const char *name,
                                     int r,
                                     int g,
                                     int b,
                                     int a,
                                     const char *media)
{
	CssRule rule = makeStaticCompiledCssRule(selectorKind,
	                                        selector,
	                                        CssRuleProperty::Custom,
	                                        CssDeclarationId::Custom,
	                                        storeStaticColorCompiledValue(CssDeclarationId::Custom, r, g, b, a),
	                                        media);
	rule.propertyAtom = internCssAtom(name ? name : "");
	return rule;
}

CssRule makeStaticTransformCssRule(StaticStyleSelectorKind selectorKind,
                                   const char *selector,
                                   std::uint16_t flags,
                                   int rotateX,
                                   int rotateY,
                                   int rotateZ,
                                   StaticStyleLengthSpec translateX,
                                   StaticStyleLengthSpec translateY,
                                   StaticStyleLengthSpec translateZ,
                                   int scaleX,
                                   int scaleY,
                                   int scaleZ,
                                   const char *media)
{
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(CssDeclarationId::Transform),
	                                 CssDeclarationId::Transform,
	                                 storeStaticTransformCompiledValue(flags,
	                                                                   rotateX,
	                                                                   rotateY,
	                                                                   rotateZ,
	                                                                   translateX,
	                                                                   translateY,
	                                                                   translateZ,
	                                                                   scaleX,
	                                                                   scaleY,
	                                                                   scaleZ),
	                                 media);
}

CssRule makeStaticOriginCssRule(StaticStyleSelectorKind selectorKind,
                                const char *selector,
                                StaticStyleOriginProperty property,
                                int xPermille,
                                int yPermille,
                                const char *media)
{
	const CssDeclarationId declaration = declarationForStaticOriginProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticOriginCompiledValue(declaration, xPermille, yPermille),
	                                 media);
}

CssRule makeStaticGridTemplateCssRule(StaticStyleSelectorKind selectorKind,
                                      const char *selector,
                                      StaticStyleGridTemplateProperty property,
                                      std::initializer_list<StaticStyleGridTemplateTrack> tracks,
                                      const char *media)
{
	const CssDeclarationId declaration = declarationForStaticGridTemplateProperty(property);
	return makeStaticCompiledCssRule(selectorKind,
	                                 selector,
	                                 rulePropertyKind(declaration),
	                                 declaration,
	                                 storeStaticGridTemplateCompiledValue(declaration, tracks),
	                                 media);
}

CssKeyframeRule makeCssKeyframeRule(CssText name, int offsetPermille, CssText property, CssText value)
{
	const CssDeclarationId declaration = classifyDeclaration(property);
	const CssAtomId nameAtom = atomForText(name);
	const std::uint16_t compiledValue = compileCssValue(declaration, value);
	const std::uint16_t valueText = compiledCssValueCanSkipKeyframeText(compiledValue)
	    ? kNoCssRuleText
	    : storeCssRuleText(std::move(value));
	CssKeyframeRule rule{nameAtom, offsetPermille, rulePropertyKind(declaration),
	                     declaration, compiledValue, valueText};
	return rule;
}

CssKeyframeRule makeStaticCompiledKeyframeRule(const char *name,
                                               int offsetPermille,
                                               CssRuleProperty propertyKind,
                                               CssDeclarationId declaration,
                                               std::uint16_t compiledValue)
{
	return CssKeyframeRule{internCssAtom(name ? name : ""),
	                       offsetPermille,
	                       propertyKind,
	                       declaration,
	                       compiledValue,
	                       kNoCssRuleText};
}

CssKeyframeRule makeStaticPropertyKeyframeRule(const char *name,
                                               int offsetPermille,
                                               Property property,
                                               int value)
{
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      CssRuleProperty::Other,
	                                      CssDeclarationId::Ignored,
	                                      storeDirectPropertyCompiledValue(property, value));
}

CssKeyframeRule makeStaticColorKeyframeRule(const char *name,
                                            int offsetPermille,
                                            StaticStyleColorProperty property,
                                            int r,
                                            int g,
                                            int b,
                                            int a)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      rulePropertyKind(declaration),
	                                      declaration,
	                                      storeStaticColorCompiledValue(declaration, r, g, b, a));
}

CssKeyframeRule makeStaticColorVarKeyframeRule(const char *name,
                                               int offsetPermille,
                                               StaticStyleColorProperty property,
                                               const char *varName,
                                               bool hasFallback,
                                               int r,
                                               int g,
                                               int b,
                                               int a)
{
	const CssDeclarationId declaration = declarationForStaticColorProperty(property);
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      rulePropertyKind(declaration),
	                                      declaration,
	                                      storeStaticColorVarCompiledValue(declaration,
	                                                                       varName,
	                                                                       hasFallback,
	                                                                       r,
	                                                                       g,
	                                                                       b,
	                                                                       a));
}

CssKeyframeRule makeStaticLengthKeyframeRule(const char *name,
                                             int offsetPermille,
                                             StaticStyleLengthProperty property,
                                             StaticStyleLengthSpec length)
{
	const CssDeclarationId declaration = declarationForStaticLengthProperty(property);
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      rulePropertyKind(declaration),
	                                      declaration,
	                                      storeStaticLengthCompiledValue(property, length));
}

CssKeyframeRule makeStaticFilterBlurKeyframeRule(const char *name,
                                                 int offsetPermille,
                                                 StaticStyleLengthSpec radius)
{
	return makeStaticCompiledKeyframeRule(name,
	                                      offsetPermille,
	                                      CssRuleProperty::Other,
	                                      CssDeclarationId::Filter,
	                                      storeStaticFilterBlurCompiledValue(radius));
}

CssKeyframeRule makeStaticTransformKeyframeRule(const char *name,
                                                int offsetPermille,
                                                std::uint16_t flags,
                                                int rotateX,
                                                int rotateY,
                                                int rotateZ,
                                                StaticStyleLengthSpec translateX,
                                                StaticStyleLengthSpec translateY,
                                                StaticStyleLengthSpec translateZ,
                                                int scaleX,
                                                int scaleY,
                                                int scaleZ)
{
	return CssKeyframeRule{internCssAtom(name ? name : ""),
	                       offsetPermille,
	                       rulePropertyKind(CssDeclarationId::Transform),
	                       CssDeclarationId::Transform,
	                       storeStaticTransformCompiledValue(flags,
	                                                        rotateX,
	                                                        rotateY,
	                                                        rotateZ,
	                                                        translateX,
	                                                        translateY,
	                                                        translateZ,
	                                                        scaleX,
	                                                        scaleY,
	                                                        scaleZ),
	                       kNoCssRuleText};
}

bool isCustomRuleProperty(const CssRule &rule)
{
	return rule.propertyKind == CssRuleProperty::Custom;
}

// Viewport dimensions are physical framebuffer pixels. CSS `vw`/`vh`
// resolve against those dimensions, while explicit CSS `px` lengths are
// logical pixels scaled by the device pixel ratio below. Unitless values
// remain physical pixels for compatibility with older embedded examples.
int g_viewport_width = 0;
int g_viewport_height = 0;
double g_device_pixel_ratio = 1.0;
// Physical-pixel height reserved at the bottom of the panel for a
// platform overlay drawn outside the document tree (e.g. the geaos
// home button). UI that wants to stay clear of that zone — the
// built-in virtual keyboard — reads this and offsets itself upward.
// Defaults to 0, so targets without such an overlay (ESP32) are
// unaffected.
int g_safe_area_inset_bottom = 0;

double sanitizedDevicePixelRatio(double value)
{
	return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

int roundToInt(double value)
{
	if (!std::isfinite(value)) return 0;
	return static_cast<int>(std::round(value));
}

int snapBorderWidth(double devicePixels)
{
	if (!std::isfinite(devicePixels) || devicePixels <= 0.0) return 0;
	return static_cast<int>(std::max(1.0, std::floor(std::min(devicePixels, 32767.0))));
}

bool isBorderWidthDeclaration(CssDeclarationId declaration)
{
	return declaration == CssDeclarationId::BorderWidth ||
	       declaration == CssDeclarationId::BorderTopWidth ||
	       declaration == CssDeclarationId::BorderRightWidth ||
	       declaration == CssDeclarationId::BorderBottomWidth ||
	       declaration == CssDeclarationId::BorderLeftWidth;
}

int cssPixelLength(double value)
{
	return roundToInt(value * g_device_pixel_ratio);
}

int rawNumber(double value)
{
	return roundToInt(value);
}

std::string trimCssValue(const std::string &value)
{
	std::size_t start = 0;
	while (start < value.size() && static_cast<unsigned char>(value[start]) <= ' ') ++start;
	std::size_t end = value.size();
	while (end > start && static_cast<unsigned char>(value[end - 1]) <= ' ') --end;
	return value.substr(start, end - start);
}

bool isTrimmedCssValue(const std::string &value)
{
	return value.empty() ||
	       (static_cast<unsigned char>(value.front()) > ' ' &&
	        static_cast<unsigned char>(value.back()) > ' ');
}

std::string toLowerAscii(std::string value)
{
	for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return value;
}

bool parseZIndex(const std::string &value, int &result)
{
	const auto text = toLowerAscii(trimCssValue(value));
	if (text == "auto" || text == "initial" || text == "unset") {
		result = kZIndexAuto;
		return true;
	}
	std::size_t i = !text.empty() && (text[0] == '+' || text[0] == '-') ? 1 : 0;
	if (i == text.size()) return false;
	for (; i < text.size(); ++i) if (text[i] < '0' || text[i] > '9') return false;
	result = static_cast<int>(std::clamp(std::strtod(text.c_str(), nullptr), -32768.0, 32767.0));
	return true;
}

bool startsWith(const std::string &value, const char *prefix)
{
	return value.rfind(prefix, 0) == 0;
}

std::string functionInner(const std::string &value, const char *name)
{
	const std::string text = trimCssValue(value);
	const std::string prefix = std::string(name) + "(";
	if (!startsWith(text, prefix.c_str()) || text.empty() || text.back() != ')') return std::string();
	return text.substr(prefix.size(), text.size() - prefix.size() - 1);
}

std::vector<std::string> splitTopLevel(const std::string &value, char delimiter)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	int depth = 0;
	for (std::size_t i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if (c == '(') depth++;
		else if (c == ')' && depth > 0) depth--;
		else if (c == delimiter && depth == 0) {
			out.push_back(trimCssValue(value.substr(start, i - start)));
			start = i + 1;
		}
	}
	out.push_back(trimCssValue(value.substr(start)));
	return out;
}

const NodeCustomProperty *lookupCustomPropertyEntry(int nodeId, CssAtomId name)
{
	// Record the dependency against the node currently being recomputed (not nodeId:
	// pseudo-element resolution looks up from the pseudo node but the dependency
	// belongs to its owner, which is g_recordingNode). Record on every lookup, hit or
	// miss — a miss that later becomes a hit is also a dependency.
	recordCustomPropRef(name);
	if (name == kInvalidCssAtom) return nullptr;
	const NodeCustomProperty *cached = nullptr;
	if (g_customPropertyLookupCache.lookup(nodeId, name, cached)) return cached;
	auto &state = treeState();
	int visited[CustomPropertyLookupCache::kCapacity]{};
	std::uint8_t visitedCount = 0;
	auto rememberVisited = [&](int id) {
		if (visitedCount >= CustomPropertyLookupCache::kCapacity) return;
		visited[visitedCount++] = id;
	};
	auto storeVisited = [&](const NodeCustomProperty *entry) {
		for (std::uint8_t i = 0; i < visitedCount; ++i)
			g_customPropertyLookupCache.store(visited[i], name, entry);
	};
	for (int id = nodeId; id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
		if (id != nodeId && g_customPropertyLookupCache.lookup(id, name, cached)) {
			storeVisited(cached);
			return cached;
		}
		rememberVisited(id);
		if (const NodeRareData *rd = rareDataFor(id)) {
			if (const auto *entry = rd->customProperties.getEntry(name)) {
				storeVisited(entry);
				return entry;
			}
		}
	}
	storeVisited(nullptr);
	return nullptr;
}

const std::string *lookupCustomProperty(int nodeId, CssAtomId name)
{
	if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, name)) return &entry->value;
	return nullptr;
}

const std::string *lookupCustomProperty(int nodeId, const std::string &name)
{
	return lookupCustomProperty(nodeId, internCssAtom(name));
}

std::string resolveCssVarsForNode(const std::string &rawValue, int nodeId, int depth = 0)
{
	if (depth > 8) return rawValue;
	// Common case: no var() reference. Skip the working-copy allocation and the
	// scan loop; just hand back the trimmed value.
	if (rawValue.find("var(") == std::string::npos) return trimCssValue(rawValue);
	std::string value = rawValue;
	std::size_t search = 0;
	while (true) {
		const std::size_t start = value.find("var(", search);
		if (start == std::string::npos) break;
		std::size_t i = start + 4;
		int parenDepth = 1;
		while (i < value.size() && parenDepth > 0) {
			if (value[i] == '(') parenDepth++;
			else if (value[i] == ')') parenDepth--;
			++i;
		}
		if (parenDepth != 0) break;
		const std::string inner = value.substr(start + 4, i - start - 5);
		const auto parts = splitTopLevel(inner, ',');
		std::string replacement;
		if (!parts.empty()) {
			const std::string name = trimCssValue(parts[0]);
			if (const auto *resolved = lookupCustomProperty(nodeId, name)) {
				replacement = resolveCssVarsForNode(*resolved, nodeId, depth + 1);
			} else if (parts.size() > 1) {
				replacement = resolveCssVarsForNode(parts[1], nodeId, depth + 1);
			}
		}
		value.replace(start, i - start, replacement);
		search = start + replacement.size();
	}
	return trimCssValue(value);
}

std::string resolveCssVarsForNode(const CssText &rawValue, int nodeId, int depth = 0)
{
	if (depth > 8) return rawValue.trimmedStr();
	if (!rawValue.hasVarReference()) return rawValue.trimmedStr();
	return resolveCssVarsForNode(rawValue.str(), nodeId, depth);
}

int g_boxPercentageBasis = -1;

int percentBasisForNode(int nodeId, LengthAxis axis)
{
	if (g_boxPercentageBasis >= 0) return g_boxPercentageBasis;
	auto &state = treeState();
	int parent = nodeId >= 0 && nodeId < state.nodeCount ? state.nodes[nodeId].parent : -1;
	if (parent >= 0 && parent < state.nodeCount) {
		const auto &p = state.nodes[parent];
		const int padding = boxInsets(p.style, axis != LengthAxis::Vertical);
		auto contentBasis = [padding](int value) {
			value -= padding;
			return value < 0 ? 0 : value;
		};
		if (axis == LengthAxis::Vertical) {
			if (p.layout.height > 0) return contentBasis(p.layout.height);
			if (p.style.height != kUnset) return p.style.box_sizing == 0 ? p.style.height : contentBasis(p.style.height);
		} else {
			if (p.layout.width > 0) return contentBasis(p.layout.width);
			if (p.style.width != kUnset) return p.style.box_sizing == 0 ? p.style.width : contentBasis(p.style.width);
		}
	}
	return axis == LengthAxis::Vertical ? g_viewport_height : g_viewport_width;
}

int currentFontSizeForNode(int nodeId);
const CssLengthSpec *cachedCompiledCssLengthSpec(const std::string &raw);
int resolveCompiledLengthForNode(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth);

int resolveFontSizeLength(const CssLengthSpec &length, int nodeId);
int g_fontSizeBasisNode = -1;
int g_lineHeightBasisNode = -1;
struct LineHeightBasisScope {
	int previous;
	explicit LineHeightBasisScope(int node) : previous(g_lineHeightBasisNode) { g_lineHeightBasisNode = node; }
	~LineHeightBasisScope() { g_lineHeightBasisNode = previous; }
};

int fontMetricBasisNode(int nodeId)
{
	const auto &state = treeState();
	return g_fontSizeBasisNode >= 0 && g_fontSizeBasisNode < state.nodeCount
	    ? state.nodes[g_fontSizeBasisNode].parent : nodeId;
}

int rootFontSizeForNode(int nodeId)
{
	const auto &state = treeState();
	int root = nodeId;
	while (root >= 0 && root < state.nodeCount && state.nodes[root].parent >= 0) root = state.nodes[root].parent;
	return currentFontSizeForNode(root == g_fontSizeBasisNode ? -1 : root);
}

int lineHeightForLength(int nodeId, bool rootRelative)
{
	const auto &state = treeState();
	int basis = nodeId;
	if (rootRelative) {
		while (basis >= 0 && basis < state.nodeCount && state.nodes[basis].parent >= 0)
			basis = state.nodes[basis].parent;
	}
	// Self-referential font-size/line-height units use parent metrics. A root
	// self-reference (or an element-free query) uses initial font metrics.
	if (basis >= 0 && basis < state.nodeCount &&
	    (basis == g_fontSizeBasisNode || basis == g_lineHeightBasisNode)) basis = state.nodes[basis].parent;
	if (basis >= 0 && basis < state.nodeCount) {
		const auto &style = state.nodes[basis].style;
		if (style.line_height > 0) return style.line_height;
		return TextRenderer::measureHeight(" ", style.font_id, currentFontSizeForNode(basis));
	}
	return TextRenderer::measureHeight(" ", gea::framework::graphics::FontRegistry::familyId("serif"),
	                                   currentFontSizeForNode(-1));
}

int zeroAdvanceForNode(int nodeId)
{
	const auto &state = treeState();
	nodeId = fontMetricBasisNode(nodeId);
	const int fontId = nodeId >= 0 && nodeId < state.nodeCount ? state.nodes[nodeId].style.font_id : -1;
	return TextRenderer::measureWidth("0", fontId, currentFontSizeForNode(nodeId));
}

int parseLengthForNode(const std::string &rawValue, int nodeId, LengthAxis axis);
std::vector<std::string> splitWords(const std::string &value);

int containmentValue(const std::string &value)
{
	const auto lower = toLowerAscii(trimCssValue(value));
	if (lower == "none" || lower == "initial" || lower == "unset") return 0;
	if (lower == "strict") return 1 | 4 | 8 | 16;
	if (lower == "content") return 4 | 8 | 16;
	int flags = 0;
	for (const auto &word : splitWords(lower)) {
		const int flag = word == "size" ? 1 : word == "inline-size" ? 2 :
		    word == "layout" ? 4 : word == "style" ? 8 : word == "paint" ? 16 : 0;
		if (!flag || (flags & flag)) return -1;
		flags |= flag;
	}
	return !flags || (flags & 3) == 3 ? -1 : flags;
}
std::vector<std::string> splitFunctionAwareWords(const std::string &value);
int parseOriginPart(const std::string &part, int fallback);

int parseCalcExpression(const std::string &expr, int nodeId, LengthAxis axis)
{
	const std::string text = trimCssValue(expr);
	for (char op : {'/', '*', '+', '-'}) {
		int depth = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (c == op && depth == 0 && i > 0) {
				const int left = parseLengthForNode(text.substr(0, i), nodeId, axis);
				const double right = std::strtod(text.substr(i + 1).c_str(), nullptr);
				if (op == '/') return right == 0.0 ? 0 : roundToInt(static_cast<double>(left) / right);
				if (op == '*') return roundToInt(static_cast<double>(left) * right);
				const int rightLength = parseLengthForNode(text.substr(i + 1), nodeId, axis);
				return op == '+' ? left + rightLength : left - rightLength;
			}
		}
	}
	return parseLengthForNode(text, nodeId, axis);
}

int parseLengthForNode(const std::string &rawValue, int nodeId, LengthAxis axis)
{
#if GEA_RECPROF
	g_profLenCalls++;
	const int64_t _lt = recNow();
	struct LenTimer { int64_t s; ~LenTimer() { g_profLenUs += recNow() - s; } } _lenTimer{_lt};
#endif
	// Fast path: a plain number/length (NOT a var()/calc()/min()/max()/clamp()
	// function) parses without the trimmed + lowercased std::string temporaries the
	// general path below allocates. Functions start with a letter; numeric lengths
	// start with a digit, sign, or dot — so the cheap first-char test routes them.
	// This is the hot case during a full style recompute (px/%/vw values).
	{
		std::size_t b = 0, e = rawValue.size();
		while (b < e && static_cast<unsigned char>(rawValue[b]) <= ' ') ++b;
		while (e > b && static_cast<unsigned char>(rawValue[e - 1]) <= ' ') --e;
		if (b >= e) return 0;
		const char c0 = rawValue[b];
		const bool maybeFunction = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
		if (!maybeFunction) {
			char *end = nullptr;
			const double v = std::strtod(rawValue.c_str() + b, &end);
			if (end) {
				while (*end == ' ') ++end;
				if (*end == '\0') return rawNumber(v);
				if (end[0] == 'p' && end[1] == 'x') return cssPixelLength(v);
				if ((end[0] == 'c' || end[0] == 'C') && (end[1] == 'h' || end[1] == 'H') && trimCssValue(end + 2).empty()) return roundToInt(v * zeroAdvanceForNode(nodeId));
				if ((end[0] == 'e' || end[0] == 'E') && toLowerAscii(trimCssValue(end)) == "em") return roundToInt(v * currentFontSizeForNode(fontMetricBasisNode(nodeId)));
				if ((end[0] == 'r' || end[0] == 'R') && toLowerAscii(trimCssValue(end)) == "rem") return roundToInt(v * rootFontSizeForNode(nodeId));
				if ((end[0] == 'l' || end[0] == 'L') && toLowerAscii(trimCssValue(end)) == "lh") return roundToInt(v * lineHeightForLength(nodeId, false));
				if ((end[0] == 'r' || end[0] == 'R') && toLowerAscii(trimCssValue(end)) == "rlh") return roundToInt(v * lineHeightForLength(nodeId, true));
				if (end[0] == '%') {
					const int basis = percentBasisForNode(nodeId, axis);
					return roundToInt(v * basis / 100.0);
				}
				if (end[0] == 'v' && end[1] == 'm' && end[2] == 'i' && end[3] == 'n' && g_viewport_width > 0 && g_viewport_height > 0) return roundToInt(v * std::min(g_viewport_width, g_viewport_height) / 100.0);
				if (end[0] == 'v' && end[1] == 'm' && end[2] == 'a' && end[3] == 'x' && g_viewport_width > 0 && g_viewport_height > 0) return roundToInt(v * std::max(g_viewport_width, g_viewport_height) / 100.0);
				if (end[0] == 'v' && end[1] == 'w' && g_viewport_width > 0) return roundToInt(v * g_viewport_width / 100.0);
				if (end[0] == 'v' && end[1] == 'h' && g_viewport_height > 0) return roundToInt(v * g_viewport_height / 100.0);
				if (end[0] == 'd' && end[1] == 'v' && end[2] == 'w' && g_viewport_width > 0) return roundToInt(v * g_viewport_width / 100.0);
				if (end[0] == 'd' && end[1] == 'v' && end[2] == 'h' && g_viewport_height > 0) return roundToInt(v * g_viewport_height / 100.0);
			}
			return rawNumber(v);
		}
	}
	const std::string value = trimCssValue(rawValue);
	if (value.empty()) return 0;
	const std::string lower = toLowerAscii(value);
	// Inline expressions and class declarations must use the same fractional
	// evaluator. The numeric fast path above still avoids expression allocation.
	if (const CssLengthSpec *compiled = cachedCompiledCssLengthSpec(value))
		return resolveCompiledLengthForNode(*compiled, nodeId, axis, 0);

	if (startsWith(lower, "var(")) {
		const auto inner = splitTopLevel(functionInner(value, "var"), ',');
		if (!inner.empty()) {
			if (const auto *resolved = lookupCustomProperty(nodeId, trimCssValue(inner[0])))
				return parseLengthForNode(*resolved, nodeId, axis);
			if (inner.size() > 1) return parseLengthForNode(inner[1], nodeId, axis);
		}
		return 0;
	}
	if (startsWith(lower, "calc(")) return parseCalcExpression(functionInner(value, "calc"), nodeId, axis);
	if (startsWith(lower, "min(") || startsWith(lower, "max(")) {
		const bool isMin = startsWith(lower, "min(");
		const auto parts = splitTopLevel(functionInner(value, isMin ? "min" : "max"), ',');
		if (parts.empty()) return 0;
		int result = parseLengthForNode(parts[0], nodeId, axis);
		for (std::size_t i = 1; i < parts.size(); ++i) {
			const int next = parseLengthForNode(parts[i], nodeId, axis);
			result = isMin ? std::min(result, next) : std::max(result, next);
		}
		return result;
	}
	if (startsWith(lower, "clamp(")) {
		const auto parts = splitTopLevel(functionInner(value, "clamp"), ',');
		if (parts.size() < 3) return parts.empty() ? 0 : parseLengthForNode(parts[0], nodeId, axis);
		const int minValue = parseLengthForNode(parts[0], nodeId, axis);
		const int preferred = parseLengthForNode(parts[1], nodeId, axis);
		const int maxValue = parseLengthForNode(parts[2], nodeId, axis);
		return std::max(minValue, std::min(preferred, maxValue));
	}
	char *end = nullptr;
	double v = std::strtod(value.c_str(), &end);
	if (end) {
		while (*end == ' ') ++end;
		if (*end == '\0') return rawNumber(v);
		if (end[0] == 'p' && end[1] == 'x') return cssPixelLength(v);
		if ((end[0] == 'c' || end[0] == 'C') && (end[1] == 'h' || end[1] == 'H') && trimCssValue(end + 2).empty()) return roundToInt(v * zeroAdvanceForNode(nodeId));
		if (end[0] == '%') {
			const int basis = percentBasisForNode(nodeId, axis);
			return roundToInt(v * basis / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'm' && end[2] == 'i' && end[3] == 'n' && g_viewport_width > 0 && g_viewport_height > 0) {
			return roundToInt(v * std::min(g_viewport_width, g_viewport_height) / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'm' && end[2] == 'a' && end[3] == 'x' && g_viewport_width > 0 && g_viewport_height > 0) {
			return roundToInt(v * std::max(g_viewport_width, g_viewport_height) / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'w' && g_viewport_width > 0) {
			return roundToInt(v * g_viewport_width / 100.0);
		}
		if (end[0] == 'v' && end[1] == 'h' && g_viewport_height > 0) {
			return roundToInt(v * g_viewport_height / 100.0);
		}
		if (end[0] == 'd' && end[1] == 'v' && end[2] == 'w' && g_viewport_width > 0) {
			return roundToInt(v * g_viewport_width / 100.0);
		}
		if (end[0] == 'd' && end[1] == 'v' && end[2] == 'h' && g_viewport_height > 0) {
			return roundToInt(v * g_viewport_height / 100.0);
		}
	}
	return rawNumber(v);
}

int parseRightFadeMaskWidth(const std::string &rawValue, int nodeId)
{
	const std::string value = trimCssValue(rawValue);
	const std::string lower = toLowerAscii(value);
	if (lower.empty() || lower == "none") return 0;
	if (lower.find("linear-gradient") == std::string::npos ||
	    lower.find("to right") == std::string::npos ||
	    lower.find("transparent") == std::string::npos) {
		return 0;
	}

	const std::size_t calcStart = lower.find("calc(");
	if (calcStart == std::string::npos) return 0;
	const std::size_t innerStart = calcStart + 5;
	int depth = 1;
	std::size_t end = innerStart;
	for (; end < value.size(); ++end) {
		if (value[end] == '(') depth++;
		else if (value[end] == ')') {
			if (--depth == 0) break;
		}
	}
	if (end <= innerStart || end >= value.size()) return 0;

	const std::string inner = trimCssValue(value.substr(innerStart, end - innerStart));
	const std::string innerLower = toLowerAscii(inner);
	if (innerLower.rfind("100%", 0) != 0) return 0;
	depth = 0;
	for (std::size_t i = 0; i < inner.size(); ++i) {
		const char c = inner[i];
		if (c == '(') depth++;
		else if (c == ')' && depth > 0) depth--;
		else if (c == '-' && depth == 0) {
			return std::max(0, parseLengthForNode(inner.substr(i + 1), nodeId, LengthAxis::Horizontal));
		}
	}
	return 0;
}

int parseLength(const std::string &value)
{
	return parseLengthForNode(value, -1, LengthAxis::None);
}

int currentFontSizeForNode(int nodeId)
{
	auto &state = treeState();
	if (nodeId >= 0 && nodeId < state.nodeCount && state.nodes[nodeId].style.font_size > 0)
		return state.nodes[nodeId].style.font_size;
	return cssPixelLength(16.0);
}

int parseLineHeightForNode(const std::string &rawValue, int nodeId)
{
	LineHeightBasisScope basisScope(nodeId);
	const std::string value = trimCssValue(rawValue);
	if (value.empty()) return 0;
	const std::string lower = toLowerAscii(value);
	if (lower == "normal") return 0;
	if (startsWith(lower, "var(")) {
		const auto inner = splitTopLevel(functionInner(value, "var"), ',');
		if (!inner.empty()) {
			if (const auto *resolved = lookupCustomProperty(nodeId, trimCssValue(inner[0])))
				return parseLineHeightForNode(*resolved, nodeId);
			if (inner.size() > 1) return parseLineHeightForNode(inner[1], nodeId);
		}
		return 0;
	}

	char *end = nullptr;
	const double scalar = std::strtod(value.c_str(), &end);
	if (end) {
		while (*end == ' ') ++end;
		if (*end == '\0') return roundToInt(static_cast<double>(currentFontSizeForNode(nodeId)) * scalar);
		if (end[0] == '%') return roundToInt(static_cast<double>(currentFontSizeForNode(nodeId)) * scalar / 100.0);
		const std::string unit = toLowerAscii(trimCssValue(end));
		if (unit == "em") return roundToInt(currentFontSizeForNode(nodeId) * scalar);
		if (unit == "rem") {
			int root = nodeId;
			const auto &state = treeState();
			while (root >= 0 && root < state.nodeCount && state.nodes[root].parent >= 0) root = state.nodes[root].parent;
			return roundToInt(currentFontSizeForNode(root) * scalar);
		}
		if (unit == "pt") return cssPixelLength(scalar * 4.0 / 3.0);
	}
	return parseLengthForNode(value, nodeId, LengthAxis::Vertical);
}

std::string primaryFontFamily(const std::string &value)
{
	std::string text = trimCssValue(value);
	if (text.empty()) return text;
	const char quote = text[0];
	if (quote == '\'' || quote == '"') {
		const std::size_t end = text.find(quote, 1);
		return end == std::string::npos ? std::string() : text.substr(1, end - 1);
	}
	const std::size_t comma = text.find(',');
	if (comma != std::string::npos) text = text.substr(0, comma);
	return trimCssValue(text);
}

int fontFamilyValue(const std::string &value)
{
	for (const auto &entry : splitTopLevel(value, ',')) {
		const auto family = primaryFontFamily(entry);
		const int id = gea::framework::graphics::FontRegistry::familyId(family.c_str());
		if (id >= 0) return id;
	}
	return gea::framework::graphics::FontRegistry::familyId("serif");
}

struct ParsedFontShorthand {
	std::string size, lineHeight = "normal", family;
	int weight = 400;
};

bool parseFontShorthand(const std::string &value, ParsedFontShorthand &font)
{
	std::size_t cursor = 0;
	auto skipSpace = [&] { while (cursor < value.size() && static_cast<unsigned char>(value[cursor]) <= ' ') ++cursor; };
	auto word = [&] {
		skipSpace();
		const auto start = cursor;
		int depth = 0;
		while (cursor < value.size()) {
			const char c = value[cursor];
			if (depth == 0 && (c == '/' || static_cast<unsigned char>(c) <= ' ')) break;
			if (c == '(') ++depth;
			if (c == ')') --depth;
			++cursor;
		}
		return value.substr(start, cursor - start);
	};
	bool weightSeen = false;
	for (;;) {
		const auto token = word();
		if (token.empty()) return false;
		if (token == "normal") continue;
		char *end = nullptr;
		const double number = std::strtod(token.c_str(), &end);
		const bool numeric = end != token.c_str() && std::isfinite(number);
		if (token == "bold" || (numeric && *end == '\0' && number >= 100 && number <= 900 && std::fmod(number, 100) == 0)) {
			if (weightSeen) return false;
			font.weight = token == "bold" ? 700 : static_cast<int>(number);
			weightSeen = true;
			continue;
		}
		// Other face variants and system-font keywords have no native face
		// representation yet. Reject the whole declaration rather than apply
		// an incomplete size/family with a silently different face.
		const std::string unit = numeric ? std::string(end) : std::string();
		if (!numeric || number < 0 || !(unit == "px" || unit == "pt" || unit == "em" || unit == "rem" || unit == "ch" || unit == "lh" || unit == "rlh" || unit == "%" || (unit.empty() && number == 0))) return false;
		font.size = token;
		break;
	}
	skipSpace();
	if (cursor < value.size() && value[cursor] == '/') {
		++cursor;
		font.lineHeight = word();
		if (font.lineHeight.empty()) return false;
		if (font.lineHeight != "normal") {
			char *end = nullptr;
			const double number = std::strtod(font.lineHeight.c_str(), &end);
			if (end == font.lineHeight.c_str() || !std::isfinite(number) || number < 0) return false;
			const std::string unit(end);
			if (!(unit.empty() || unit == "px" || unit == "pt" || unit == "em" || unit == "rem" || unit == "ch" || unit == "lh" || unit == "rlh" || unit == "%")) return false;
		}
	}
	skipSpace();
	font.family = value.substr(cursor);
	return !font.family.empty() && font.family.find('/') == std::string::npos;
}

int fontSizeValue(const std::string &value, int nodeId)
{
	if (const CssLengthSpec *length = cachedCompiledCssLengthSpec(value))
		return resolveFontSizeLength(*length, nodeId);
	char *end = nullptr;
	const double number = std::strtod(value.c_str(), &end);
	const auto &state = treeState();
	const int parent = nodeId >= 0 && nodeId < state.nodeCount ? state.nodes[nodeId].parent : -1;
	if (end != value.c_str() && end) {
		const std::string unit = toLowerAscii(trimCssValue(end));
		if (unit == "%" || unit == "em") return roundToInt(number * currentFontSizeForNode(parent) / (unit == "%" ? 100.0 : 1.0));
		if (unit == "ch") return roundToInt(number * zeroAdvanceForNode(parent));
		if (unit == "rem") {
			int root = nodeId;
			while (root >= 0 && state.nodes[root].parent >= 0) root = state.nodes[root].parent;
			return roundToInt(number * (root == nodeId ? cssPixelLength(16) : currentFontSizeForNode(root)));
		}
		if (unit == "pt") return cssPixelLength(number * 4.0 / 3.0);
	}
	return parseLengthForNode(value, nodeId, LengthAxis::Vertical);
}

int fontWeightValue(const std::string &rawValue)
{
	const std::string value = toLowerAscii(trimCssValue(rawValue));
	if (value.empty() || value == "normal") return 400;
	if (value == "bold" || value == "bolder") return 700;
	if (value == "lighter") return 300;
	char *end = nullptr;
	const double number = std::strtod(value.c_str(), &end);
	if (!end || end == value.c_str()) return 400;
	while (*end == ' ') ++end;
	if (*end != '\0') return 400;
	return std::clamp(roundToInt(number), 1, 1000);
}

int clampColorChannel(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return value;
}

double parseAngleDegrees(const std::string &value);
int numericRotateTenths(double degrees);

int clampAlphaChannel(double value)
{
	if (value <= 1.0) value *= 255.0;
	if (value < 0.0) return 0;
	if (value > 255.0) return 255;
	return static_cast<int>(value + 0.5);
}

struct ParsedCssColor {
	int r = 255;
	int g = 255;
	int b = 255;
	int a = 255;
	bool valid = false;
};

int hexDigitValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool parseHexPair(const std::string &value, std::size_t index, int &out)
{
	if (index + 1 >= value.size()) return false;
	const int hi = hexDigitValue(value[index]);
	const int lo = hexDigitValue(value[index + 1]);
	if (hi < 0 || lo < 0) return false;
	out = (hi << 4) | lo;
	return true;
}

bool parseHexCssColor(const std::string &value, ParsedCssColor &out)
{
	if (value.empty() || value[0] != '#') return false;
	if (value.size() == 4 || value.size() == 5) {
		const int r = hexDigitValue(value[1]);
		const int g = hexDigitValue(value[2]);
		const int b = hexDigitValue(value[3]);
		const int a = value.size() == 5 ? hexDigitValue(value[4]) : 15;
		if (r < 0 || g < 0 || b < 0 || a < 0) return false;
		out.r = (r << 4) | r;
		out.g = (g << 4) | g;
		out.b = (b << 4) | b;
		out.a = (a << 4) | a;
		out.valid = true;
		return true;
	}
	if (value.size() == 7 || value.size() == 9) {
		if (!parseHexPair(value, 1, out.r) ||
		    !parseHexPair(value, 3, out.g) ||
		    !parseHexPair(value, 5, out.b))
			return false;
		if (value.size() == 9 && !parseHexPair(value, 7, out.a)) return false;
		if (value.size() == 7) out.a = 255;
		out.valid = true;
		return true;
	}
	return false;
}

bool parseRgbFunction(const std::string &value, ParsedCssColor &out)
{
	const std::size_t open = value.find('(');
	const std::size_t close = value.find(')', open == std::string::npos ? 0 : open);
	if (open == std::string::npos || close == std::string::npos) return false;
	const std::string name = toLowerAscii(trimCssValue(value.substr(0, open)));
	if (name != "rgb" && name != "rgba") return false;
	const auto parts = splitTopLevel(value.substr(open + 1, close - open - 1), ',');
	if (parts.size() < 3) return false;
	out.r = clampColorChannel(static_cast<int>(std::strtod(parts[0].c_str(), nullptr)));
	out.g = clampColorChannel(static_cast<int>(std::strtod(parts[1].c_str(), nullptr)));
	out.b = clampColorChannel(static_cast<int>(std::strtod(parts[2].c_str(), nullptr)));
	out.a = parts.size() >= 4 ? clampAlphaChannel(std::strtod(parts[3].c_str(), nullptr)) : 255;
	out.valid = true;
	return true;
}

std::string firstColorToken(const std::string &value)
{
	std::size_t rgb = value.find("rgb(");
	std::size_t rgba = value.find("rgba(");
	if (rgba != std::string::npos && (rgb == std::string::npos || rgba < rgb)) rgb = rgba;
	std::size_t hex = value.find('#');
	if (rgb != std::string::npos && (hex == std::string::npos || rgb < hex)) {
		int depth = 0;
		for (std::size_t i = rgb; i < value.size(); ++i) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')' && --depth == 0) return value.substr(rgb, i - rgb + 1);
		}
		return value.substr(rgb);
	}
	if (hex != std::string::npos) {
		std::size_t end = hex + 1;
		while (end < value.size() && std::isxdigit(static_cast<unsigned char>(value[end]))) ++end;
		return value.substr(hex, end - hex);
	}
	return trimCssValue(value);
}

// Parse to a RAW (pre-panel-swap) native style-value int. Callers route through
// setStyleValue/pixelFromStyleValue (border fallback, keyframe colours), which
// applies the panel byte-swap on 16-bit boards; identity on full-colour boards.
int parseColorStyleValue(const std::string &value)
{
#if GEA_RECPROF
	g_profColorCalls++;
	const int64_t _ct = recNow();
	struct ColorTimer { int64_t s; ~ColorTimer() { g_profColorUs += recNow() - s; } } _colorTimer{_ct};
#endif
	const std::string colorValue = firstColorToken(value);
	ParsedCssColor parsed;
	if (parseRgbFunction(colorValue, parsed)) return static_cast<int>(gea::framework::graphics::pixel::nativeStyleValue(parsed.r, parsed.g, parsed.b));
	if (parseHexCssColor(colorValue, parsed)) return static_cast<int>(gea::framework::graphics::pixel::nativeStyleValue(parsed.r, parsed.g, parsed.b));
	return static_cast<int>(gea::framework::graphics::pixel::nativeStyleValue(255, 255, 255));
}

ParsedCssColor parseCssColor(const std::string &value)
{
	const std::string colorValue = trimCssValue(value);
	if (toLowerAscii(colorValue) == "transparent") {
		ParsedCssColor parsed;
		parsed.r = 0;
		parsed.g = 0;
		parsed.b = 0;
		parsed.a = 0;
		parsed.valid = true;
		return parsed;
	}
	ParsedCssColor parsed;
	if (parseRgbFunction(colorValue, parsed)) return parsed;
	if (parseHexCssColor(colorValue, parsed)) return parsed;
	return parsed;
}

// RAW (pre-panel-swap) native style-value for colours applied via setStyleValue()
// -> Tree::setStyle() -> StyleValues::pixelFromStyleValue(), which applies the
// panel byte-swap once on 16-bit boards (must be raw here or colours double-swap,
// e.g. cream text rendered periwinkle). Colours written straight into node.style
// (gradient stops, grid lines) bypass that swap and use cssColorNative() instead.
// On full-colour boards there is no panel concept, so both are identical RGBA8888.
gea::framework::graphics::pixel::native_t cssColorStyleValue(const ParsedCssColor &color)
{
	return gea::framework::graphics::pixel::nativeStyleValue(color.r, color.g, color.b);
}

// Final native pixel for colours written DIRECTLY into node.style (gradient stops,
// grid colours) which skip setStyleValue/pixelFromStyleValue and so must already be
// in the board's framebuffer form (panel-order RGB565 / RGBA8888).
gea::framework::graphics::pixel::native_t cssColorNative(const ParsedCssColor &color)
{
	return gea::framework::graphics::pixel::nativeColor(color.r, color.g, color.b);
}

CachedCssColor cachedCssColorForValue(const std::string &raw)
{
	const std::string key = trimCssValue(raw);
	if (key.empty()) return {};
	auto &cache = compiledCssColorCache();
	const auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	CachedCssColor out;
	const ParsedCssColor color = parseCssColor(firstColorToken(key));
	if (color.valid) {
		out.styleColor = static_cast<std::int32_t>(cssColorStyleValue(color));
		out.nativeColor = static_cast<std::int32_t>(cssColorNative(color));
		out.alpha = static_cast<std::uint8_t>(color.a);
		out.valid = true;
	}
	cache.emplace(key, out);
	return out;
}

const CssLengthSpec *cachedCompiledCssLengthSpec(const std::string &raw);

bool tryPreResolveStaticCustomLengthSpec(const CssLengthSpec &length, int nodeId, CssLengthSpec &out);

void setCustomPropertyValue(NodeCustomPropertyStore &store, CssAtomId name, const std::string &value)
{
	clearCustomPropertyLookupCache();
	const CachedCssColor color = cachedCssColorForValue(value);
	if (color.valid) {
		store.setColor(name,
		               value,
		               color.styleColor,
		               color.nativeColor,
		               color.alpha);
		return;
	}
	if (const CssLengthSpec *length = cachedCompiledCssLengthSpec(value)) {
		store.setLength(name, value, length->value, static_cast<std::uint8_t>(length->unit));
		return;
	}
	store.set(name, value);
}

void setCustomPropertyRuleValue(NodeCustomPropertyStore &store,
                                CssAtomId name,
                                const CssText &text,
                                int nodeId,
                                std::uint16_t compiledHandle)
{
	clearCustomPropertyLookupCache();
	const std::string value = text.str();
	const auto &compiledValues = compiledCssValues();
	if (compiledHandle < compiledValues.size()) {
		const CssCompiledValue &compiled = compiledValues[compiledHandle];
		if (compiled.declaration == CssDeclarationId::Custom) {
			if (compiled.kind == CssCompiledKind::Color) {
				store.setColor(name,
				               value,
				               compiled.values[0],
				               compiled.values[1],
				               static_cast<std::uint8_t>(compiled.values[2]));
				return;
			}
			if (compiled.kind == CssCompiledKind::Length) {
				CssLengthSpec storedLength;
				if (tryPreResolveStaticCustomLengthSpec(compiled.lengths[0], nodeId, storedLength)) {
					store.setLength(name,
					                value,
					                storedLength.value,
					                static_cast<std::uint8_t>(storedLength.unit));
					return;
				}
				store.setLength(name,
				                value,
				                compiled.lengths[0].value,
				                static_cast<std::uint8_t>(compiled.lengths[0].unit));
				return;
			}
		}
	}
	setCustomPropertyValue(store, name, value);
}

ParsedCssColor parseGradientColorStop(const std::string &value)
{
	const std::string token = trimCssValue(value);
	if (token.rfind("rgb", 0) == 0 || token.rfind("rgba", 0) == 0 || token.find('#') != std::string::npos)
		return parseCssColor(firstColorToken(token));
	const std::size_t end = token.find_first_of(" \t\r\n");
	return parseCssColor(end == std::string::npos ? token : token.substr(0, end));
}

std::string gradientColorTokenForStop(const std::string &stop)
{
	const auto words = splitFunctionAwareWords(stop);
	if (!words.empty()) return words[0];
	const std::string token = firstColorToken(stop);
	const std::string trimmed = trimCssValue(stop);
	if (token == trimmed) {
		const std::size_t end = token.find_first_of(" \t\r\n");
		if (end != std::string::npos) return token.substr(0, end);
	}
	return token;
}

struct ParsedLinearGradient {
	bool valid = false;
	int angleTenths = 1800;
	ParsedCssColor from;
	ParsedCssColor mid;
	ParsedCssColor to;
	int midStopPermille = 500;
	int toStopPermille = 1000;
	bool hasMid = false;
};

struct ParsedRadialGradient {
	bool valid = false;
	ParsedCssColor from;
	ParsedCssColor to;
	int stopPermille = 1000;
	int cxPermille = 500;
	int cyPermille = 500;
	int rxPermille = 1000;
	int ryPermille = 1000;
};

struct ParsedGradientLineLayer {
	bool valid = false;
	bool vertical = false;
	ParsedCssColor color;
	int lineWidth = 1;
};

std::string lastFunctionCall(const std::string &value, const std::string &name)
{
	const std::string needle = name + "(";
	std::string found;
	std::size_t search = 0;
	while (true) {
		const std::size_t start = value.find(needle, search);
		if (start == std::string::npos) break;
		std::size_t i = start + needle.size();
		int depth = 1;
		while (i < value.size() && depth > 0) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')') depth--;
			++i;
		}
		if (depth == 0) found = value.substr(start, i - start);
		search = start + needle.size();
	}
	return found;
}

int colorStopLength(const std::string &stop, int nodeId)
{
	const std::string token = gradientColorTokenForStop(stop);
	const std::size_t tokenStart = stop.find(token);
	const std::string rest = tokenStart == std::string::npos
	    ? std::string()
	    : trimCssValue(stop.substr(tokenStart + token.size()));
	if (rest.empty()) return 0;
	const auto parts = splitWords(rest);
	if (parts.empty()) return 0;
	return parseLengthForNode(parts[0], nodeId, LengthAxis::None);
}

int colorStopPermilleWithLimit(const std::string &stop, int fallback, bool clampToGradientBox)
{
	const std::string token = gradientColorTokenForStop(stop);
	const std::size_t tokenStart = stop.find(token);
	const std::string rest = tokenStart == std::string::npos
	    ? std::string()
	    : trimCssValue(stop.substr(tokenStart + token.size()));
	if (rest.empty()) return fallback;
	const auto parts = splitWords(rest);
	if (parts.empty()) return fallback;
	const std::string first = trimCssValue(parts[0]);
	if (first.find('%') == std::string::npos) return fallback;
	const double percent = std::strtod(first.c_str(), nullptr);
	if (!std::isfinite(percent)) return fallback;
	int permille = static_cast<int>(percent * 10.0 + (percent >= 0.0 ? 0.5 : -0.5));
	if (permille < 0) permille = 0;
	if (clampToGradientBox && permille > 1000) permille = 1000;
	else if (!clampToGradientBox && permille > 60000) permille = 60000;
	return permille;
}

int colorStopPermille(const std::string &stop, int fallback)
{
	return colorStopPermilleWithLimit(stop, fallback, true);
}

int colorStopPermilleUnclamped(const std::string &stop, int fallback)
{
	return colorStopPermilleWithLimit(stop, fallback, false);
}

int parsePercentPermille(const std::string &part, int fallback)
{
	const std::string value = trimCssValue(part);
	if (value.find('%') == std::string::npos) return fallback;
	const double percent = std::strtod(value.c_str(), nullptr);
	if (!std::isfinite(percent)) return fallback;
	return static_cast<int>(percent * 10.0 + (percent >= 0.0 ? 0.5 : -0.5));
}

bool gradientLineIsVertical(int angleTenths)
{
	const double angleRadians = (static_cast<double>(angleTenths) * 3.14159265358979323846) / 1800.0;
	const double dx = std::sin(angleRadians);
	const double dy = -std::cos(angleRadians);
	return std::fabs(dx) >= std::fabs(dy);
}

ParsedGradientLineLayer parseGradientLineLayer(const std::string &value, int nodeId)
{
	ParsedGradientLineLayer layer;
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return layer;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return layer;

	std::size_t colorStart = 0;
	int angleTenths = 1800;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) angleTenths = 900;
		else if (direction.find("left") != std::string::npos) angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) angleTenths = 0;
		else angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart + 1 >= parts.size()) return layer;

	const ParsedCssColor firstColor = parseGradientColorStop(parts[colorStart]);
	const ParsedCssColor secondColor = parseGradientColorStop(parts[colorStart + 1]);
	const int firstLength = colorStopLength(parts[colorStart], nodeId);
	const int secondLength = colorStopLength(parts[colorStart + 1], nodeId);
	if (!firstColor.valid || !secondColor.valid || firstColor.a == 0 || secondColor.a != 0 || firstLength <= 0) return layer;
	if (secondLength > 0 && secondLength != firstLength) return layer;

	layer.valid = true;
	layer.vertical = gradientLineIsVertical(angleTenths);
	layer.color = firstColor;
	layer.lineWidth = firstLength;
	return layer;
}

ParsedLinearGradient parseLinearGradient(const std::string &value)
{
	ParsedLinearGradient gradient;
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return gradient;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return gradient;

	std::size_t colorStart = 0;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		gradient.angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) gradient.angleTenths = 900;
		else if (direction.find("left") != std::string::npos) gradient.angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) gradient.angleTenths = 0;
		else gradient.angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart >= parts.size()) return gradient;

	gradient.from = parseGradientColorStop(parts[colorStart]);
	if (parts.size() == colorStart + 2) {
		const int fromStop = colorStopPermille(parts[colorStart], -1);
		if (fromStop > 0 && fromStop < 1000) {
			gradient.mid = gradient.from;
			gradient.midStopPermille = fromStop;
			gradient.hasMid = gradient.mid.valid;
		}
	} else if (parts.size() > colorStart + 2) {
		gradient.mid = parseGradientColorStop(parts[colorStart + 1]);
		gradient.midStopPermille = colorStopPermille(parts[colorStart + 1], 500);
		gradient.hasMid = gradient.mid.valid;
	}
	gradient.to = parseGradientColorStop(parts.back());
	gradient.toStopPermille = colorStopPermilleUnclamped(parts.back(), 1000);
	if (gradient.toStopPermille <= 0) gradient.toStopPermille = 1;
	if (gradient.hasMid && gradient.toStopPermille <= gradient.midStopPermille)
		gradient.toStopPermille = gradient.midStopPermille + 1;
	gradient.valid = gradient.from.valid && gradient.to.valid;
	return gradient;
}

ParsedRadialGradient parseRadialGradient(const std::string &value)
{
	ParsedRadialGradient gradient;
	const std::string call = lastFunctionCall(value, "radial-gradient");
	if (call.empty()) return gradient;
	const std::string inner = functionInner(call, "radial-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return gradient;

	std::size_t colorStart = 0;
	const ParsedCssColor firstMaybeColor = parseGradientColorStop(parts[0]);
	if (!firstMaybeColor.valid) {
		colorStart = 1;
		const auto words = splitWords(parts[0]);
		std::size_t atIndex = words.size();
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (toLowerAscii(words[i]) == "at") {
				atIndex = i;
				break;
			}
		}

		std::vector<std::string> sizeWords;
		for (std::size_t i = 0; i < atIndex; ++i) {
			const std::string lower = toLowerAscii(words[i]);
			if (lower == "circle" || lower == "ellipse" || lower == "closest-side" ||
			    lower == "closest-corner" || lower == "farthest-side" || lower == "farthest-corner")
				continue;
			sizeWords.push_back(words[i]);
		}
		if (!sizeWords.empty()) gradient.rxPermille = parsePercentPermille(sizeWords[0], gradient.rxPermille);
		if (sizeWords.size() > 1) gradient.ryPermille = parsePercentPermille(sizeWords[1], gradient.ryPermille);
		else if (!sizeWords.empty()) gradient.ryPermille = gradient.rxPermille;

		if (atIndex < words.size()) {
			std::vector<std::string> centerWords;
			for (std::size_t i = atIndex + 1; i < words.size(); ++i) centerWords.push_back(words[i]);
			if (!centerWords.empty()) gradient.cxPermille = parseOriginPart(centerWords[0], gradient.cxPermille);
			if (centerWords.size() > 1) gradient.cyPermille = parseOriginPart(centerWords[1], gradient.cyPermille);
		}
	}
	if (colorStart + 1 >= parts.size()) return gradient;

	gradient.from = parseGradientColorStop(parts[colorStart]);
	gradient.to = parseGradientColorStop(parts.back());
	gradient.stopPermille = colorStopPermille(parts.back(), 1000);
	if (gradient.stopPermille <= 0) gradient.stopPermille = 1;
	gradient.valid = gradient.from.valid && gradient.to.valid;
	return gradient;
}

// CSS <integer>, with saturation at the engine's signed 32-bit range.
// Reject units, fractions and trailing garbage instead of accepting a prefix.
bool parseOrder(const std::string &raw, int &result)
{
	const std::string value = trimCssValue(raw);
	std::size_t i = 0;
	bool negative = false;
	if (!value.empty() && (value[0] == '+' || value[0] == '-')) {
		negative = value[0] == '-';
		++i;
	}
	if (i == value.size()) return false;
	const std::uint64_t limit = negative ? 2147483648ull : 2147483647ull;
	std::uint64_t number = 0;
	for (; i < value.size(); ++i) {
		if (value[i] < '0' || value[i] > '9') return false;
		number = std::min(limit, number * 10 + static_cast<unsigned>(value[i] - '0'));
	}
	result = static_cast<int>(negative ? -static_cast<std::int64_t>(number) : static_cast<std::int64_t>(number));
	return true;
}

bool parseGridLine(const std::string &raw, int &out)
{
	const auto words = splitFunctionAwareWords(toLowerAscii(trimCssValue(raw)));
	if (words.size() == 1 && words[0] == "auto") { out = 0; return true; }
	bool span = words.size() == 2 && (words[0] == "span" || words[1] == "span");
	if (words.size() != 1 && !span) return false;
	const auto &number = span && words[0] == "span" ? words[1] : words[0];
	int value;
	if (!parseOrder(number, value) || value == 0 || (span && value < 0)) return false;
	value = std::max(-32767, std::min(32767, value));
	out = span ? kGridLineSpan + value : value;
	return true;
}

int flexAlignValue(const std::string &raw)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	const auto words = splitFunctionAwareWords(value);
	if (words.size() == 2 && ((words[0] == "first" && words[1] == "baseline") ||
	    (words[0] == "baseline" && words[1] == "first"))) return 5;
	if (words.size() == 2 && ((words[0] == "last" && words[1] == "baseline") ||
	    (words[0] == "baseline" && words[1] == "last"))) return kAlignLastBaseline;
	if (words.size() == 2 && (words[0] == "safe" || words[0] == "unsafe")) {
		const auto &position = words[1];
		int align = position == "center" ? 1 : position == "end" ? kAlignEnd : position == "flex-end" ? 2 :
		            position == "start" ? kAlignStart : position == "flex-start" ? 6 : -1;
		if (align >= 0) return align | (words[0] == "safe" ? kAlignSafe : kAlignUnsafe);
		return -2;
	}
	if (!words.empty() && (words[0] == "safe" || words[0] == "unsafe")) return -2;
	if (value == "center") return 1;
	if (value == "flex-end") return 2;
	if (value == "end") return kAlignEnd;
	if (value == "space-between") return 3;
	if (value == "space-around") return 4;
	if (value == "space-evenly") return kAlignSpaceEvenly;
	if (value == "baseline") return 5;
	if (value == "flex-start") return 6;
	if (value == "start") return kAlignStart;
	if (value == "normal" || value == "stretch" || value == "initial" || value == "unset") return 0;
	return -2;
}

int displayValue(const std::string &value)
{
	if (value == "none") return kDisplayNone;
	if (value == "grid" || value == "inline-grid") return kDisplayGrid;
	if (value == "flex" || value == "inline-flex") return kDisplayFlex;
	return kDisplayBlock;
}

int imageFitValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "contain") return 1;
	if (lower == "cover") return 2;
	if (lower == "none") return 3;
	if (lower == "scale-down") return 4;
	return 0;
}

int flexDirectionValue(const std::string &value) { return value == "row" ? 1 : value == "row-reverse" ? 3 : value == "column-reverse" ? 2 : 0; }
int flexWrapValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "nowrap" || lower == "initial" || lower == "unset") return 0;
	int wrap = 0, balance = 0;
	for (const auto &word : splitFunctionAwareWords(lower)) {
		if (word == "balance" && !balance) balance = 4;
		else if ((word == "wrap" || word == "wrap-reverse") && !wrap) wrap = word == "wrap" ? 1 : 2;
		else return -1;
	}
	return wrap || balance ? (wrap ? wrap : 1) | balance : -1;
}
int selfAlignValue(const std::string &raw, bool physical = false)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	const auto words = splitFunctionAwareWords(value);
	int overflow = 0;
	std::string position = value;
	if (words.size() == 2 && (words[0] == "safe" || words[0] == "unsafe")) {
		overflow = words[0] == "safe" ? kAlignSafe : kAlignUnsafe;
		position = words[1];
	}
	if (position == "self-start") return kAlignSelfStart | overflow;
	if (position == "self-end") return kAlignSelfEnd | overflow;
	if (physical && position == "left") return kAlignLeft | overflow;
	if (physical && position == "right") return kAlignRight | overflow;
	const int alignment = flexAlignValue(value);
	return isDistributedAlignment(alignment) ? -2 : alignment;
}

int alignSelfValue(const std::string &value) { return toLowerAscii(trimCssValue(value)) == "auto" ? -1 : selfAlignValue(value); }
// Only accept implemented self-alignment values. In particular, distribution
// keywords such as space-between must not reset an earlier valid declaration.
int justifySelfValue(const std::string &raw)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	if (value == "auto") return -1;
	if (value == "normal" || value == "stretch") return 0;
	const int baseline = flexAlignValue(value);
	if (baseline == 5 || baseline == kAlignLastBaseline) return baseline;
	const auto words = splitFunctionAwareWords(value);
	std::string position = value;
	if (words.size() == 2 && (words[0] == "safe" || words[0] == "unsafe")) position = words[1];
	if (position != "center" && position != "start" && position != "end" &&
	    position != "flex-start" && position != "flex-end" && position != "self-start" && position != "self-end" &&
	    position != "left" && position != "right") return -2;
	return selfAlignValue(value, true);
}

bool alignmentShorthandProperties(CssDeclarationId declaration, Property &align, Property &justify)
{
	switch (declaration) {
	case CssDeclarationId::PlaceItems: align = Property::AlignItems; justify = Property::JustifyItems; return true;
	case CssDeclarationId::PlaceContent: align = Property::AlignContent; justify = Property::JustifyContent; return true;
	case CssDeclarationId::PlaceSelf: align = Property::AlignSelf; justify = Property::JustifySelf; return true;
	default: return false;
	}
}

bool parseAlignmentShorthand(CssDeclarationId declaration, const std::string &raw, int &align, int &justify)
{
	const auto words = splitFunctionAwareWords(toLowerAscii(trimCssValue(raw)));
	if (words.empty() || words.size() > 4) return false;
	const bool content = declaration == CssDeclarationId::PlaceContent;
	const bool self = declaration == CssDeclarationId::PlaceSelf;
	if (words.size() == 1 && (words[0] == "initial" || words[0] == "unset")) {
		align = justify = self ? -1 : 0;
		return true;
	}
	for (const auto &word : words)
		if (word == "initial" || word == "unset" || word == "inherit" || word == "revert" || word == "revert-layer") return false;
	auto parse = [&](const std::string &value, bool second) {
		if (content) {
			const int v = flexAlignValue(value);
			return second && (v == 5 || v == kAlignLastBaseline) ? -2 : v;
		}
		if (second) {
			const int v = justifySelfValue(value);
			return !self && v == -1 ? -2 : v;
		}
		return self ? alignSelfValue(value) : selfAlignValue(value);
	};
	auto join = [&](std::size_t start, std::size_t end) {
		std::string value;
		for (auto i = start; i < end; ++i) { if (!value.empty()) value += ' '; value += words[i]; }
		return value;
	};
	for (std::size_t split = words.size(); split > 0; --split) {
		const auto first = join(0, split);
		const int a = parse(first, false);
		if (a < -1) continue;
		const auto second = split < words.size() ? join(split, words.size())
		    : content && (a == 5 || a == kAlignLastBaseline) ? "start" : first;
		const int b = parse(second, true);
		if (b < -1) continue;
		align = a; justify = b;
		return true;
	}
	return false;
}

int positionValue(const std::string &value)
{
	if (value == "absolute") return 1;
	if (value == "fixed") return kPositionFixed;
	if (value == "relative") return 2;
	return 0;
}

int textAlignValue(const std::string &value)
{
	if (value == "center") return 1;
	if (value == "right" || value == "end") return 2;
	return 0;
}

// CSS `text-decoration` — only the values relevant to embedded UI are
// modeled: `none` (default), `underline`, and `line-through`. The
// renderer paints a 1-pixel horizontal line at a y-offset chosen per
// decoration kind; the value enum is what TextRenderer consults.
int textDecorationValue(const std::string &value)
{
	if (value == "line-through" || value == "strikethrough") return 2;
	if (value == "underline") return 1;
	return 0;
}

int textTransformValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "uppercase") return 1;
	if (lower == "lowercase") return 2;
	if (lower == "capitalize") return 3;
	return 0;
}

int visibilityValue(const std::string &value)
{
	const auto lower = toLowerAscii(trimCssValue(value));
	if (lower == "visible") return 0;
	if (lower == "hidden") return 1;
	if (lower == "collapse") return 2;
	return -1;
}

// CSS `backface-visibility`: hidden => 1 (cull a face/text whose projected winding
// points away from the viewer), visible/default => 0.
int backfaceValue(const std::string &value)
{
	return toLowerAscii(trimCssValue(value)) == "hidden" ? 1 : 0;
}

int overflowValue(const std::string &value)
{
	const auto lower = toLowerAscii(trimCssValue(value));
	if (lower == "visible") return 0;
	if (lower == "hidden") return 1;
	if (lower == "scroll" || lower == "auto" || lower == "overlay") return 2;
	if (lower == "clip") return 3;
	return -1;
}

// Preserve the distinction between collapsing spaces, forced line breaks and
// soft wrapping. In particular, pre is not a single-line nowrap value.
int whiteSpaceValue(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	if (lower == "nowrap") return 1;
	if (lower == "pre") return 2;
	if (lower == "pre-wrap") return 3;
	if (lower == "pre-line") return 4;
	if (lower == "break-spaces") return 5;
	return 0;
}

// CSS `text-overflow`: 1 (ellipsis) when an overflowing nowrap line should be
// truncated with a trailing "..."; 0 (clip — the default) otherwise.
int textOverflowValue(const std::string &value)
{
	return toLowerAscii(trimCssValue(value)) == "ellipsis" ? 1 : 0;
}

// CSS `pointer-events`: 1 (none — the node and its subtree are skipped during
// hit-testing so clicks fall through to whatever is behind) when "none";
// 0 (auto — the default) otherwise.
int pointerEventsValue(const std::string &value)
{
	return toLowerAscii(trimCssValue(value)) == "none" ? 1 : 0;
}

int parseOpacity(const std::string &value)
{
	double v = std::strtod(value.c_str(), nullptr);
	if (v <= 1.0) v *= 255.0;
	if (v < 0.0) v = 0.0;
	if (v > 255.0) v = 255.0;
	return static_cast<int>(v + 0.5);
}

int parseFilterBlurRadius(const std::string &value, int nodeId)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (lower.empty() || lower == "none") return 0;
	const std::size_t blur = lower.find("blur(");
	if (blur == std::string::npos) return 0;
	std::size_t i = blur + 5;
	int depth = 1;
	const std::size_t argStart = i;
	while (i < text.size() && depth > 0) {
		if (text[i] == '(') depth++;
		else if (text[i] == ')') depth--;
		++i;
	}
	if (depth != 0 || i <= argStart) return 0;
	int radius = parseLengthForNode(text.substr(argStart, i - argStart - 1), nodeId, LengthAxis::None);
	if (radius < 0) radius = 0;
	if (radius > 64) radius = 64;
	return radius;
}

std::vector<std::string> splitFunctionAwareWords(const std::string &value)
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t start = i;
		int depth = 0;
		while (i < value.size()) {
			const char c = value[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (static_cast<unsigned char>(c) <= ' ' && depth == 0) break;
			++i;
		}
		if (i > start) out.push_back(value.substr(start, i - start));
	}
	return out;
}

bool isBoxShadowColorToken(const std::string &token)
{
	const std::string lower = toLowerAscii(trimCssValue(token));
	return lower == "transparent" ||
	       lower.rfind("rgb(", 0) == 0 ||
	       lower.rfind("rgba(", 0) == 0 ||
	       (!lower.empty() && lower[0] == '#');
}

struct ParsedBoxShadow {
	bool valid = false;
	bool inset = false;
	int offsetX = 0;
	int offsetY = 0;
	int blur = 0;
	int spread = 0;
	ParsedCssColor color;
};

ParsedBoxShadow parseInsetBoxShadow(const std::string &value, int nodeId)
{
	ParsedBoxShadow out;
	out.color.r = 0;
	out.color.g = 0;
	out.color.b = 0;
	out.color.a = 255;
	out.color.valid = true;

	const std::string text = trimCssValue(value);
	const std::string lowerText = toLowerAscii(text);
	if (text.empty() || lowerText == "none") {
		out.valid = true;
		out.inset = false;
		out.color.a = 0;
		return out;
	}

	for (const auto &rawLayer : splitTopLevel(text, ',')) {
		const auto tokens = splitFunctionAwareWords(rawLayer);
		bool inset = false;
		std::vector<std::string> lengths;
		ParsedCssColor color = out.color;
		for (const auto &token : tokens) {
			const std::string lower = toLowerAscii(trimCssValue(token));
			if (lower == "inset") {
				inset = true;
				continue;
			}
			if (isBoxShadowColorToken(token)) {
				const ParsedCssColor parsed = parseCssColor(firstColorToken(token));
				if (parsed.valid) color = parsed;
				continue;
			}
			if (lower == "outset") continue;
			lengths.push_back(token);
		}
		if (!inset) continue;
		out.valid = true;
		out.inset = true;
		out.color = color;
		if (!lengths.empty()) out.offsetX = parseLengthForNode(lengths[0], nodeId, LengthAxis::Horizontal);
		if (lengths.size() > 1) out.offsetY = parseLengthForNode(lengths[1], nodeId, LengthAxis::Vertical);
		if (lengths.size() > 2) out.blur = parseLengthForNode(lengths[2], nodeId, LengthAxis::None);
		if (lengths.size() > 3) out.spread = parseLengthForNode(lengths[3], nodeId, LengthAxis::None);
		if (out.blur < 0) out.blur = 0;
		if (out.blur > 96) out.blur = 96;
		return out;
	}

	out.valid = true;
	out.inset = false;
	out.color.a = 0;
	return out;
}

void applyBoxShadowValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	const ParsedBoxShadow shadow = parseInsetBoxShadow(value, node.id());
	if (!shadow.valid || !shadow.inset || shadow.color.a == 0) {
		setStyleValue(node, Property::BoxShadowInset, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetX, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetY, 0, source);
		setStyleValue(node, Property::BoxShadowBlur, 0, source);
		setStyleValue(node, Property::BoxShadowSpread, 0, source);
		setStyleValue(node, Property::BoxShadowColor, 0, source);
		setStyleValue(node, Property::BoxShadowAlpha, 0, source);
		return;
	}
	setStyleValue(node, Property::BoxShadowOffsetX, shadow.offsetX, source);
	setStyleValue(node, Property::BoxShadowOffsetY, shadow.offsetY, source);
	setStyleValue(node, Property::BoxShadowBlur, shadow.blur, source);
	setStyleValue(node, Property::BoxShadowSpread, shadow.spread, source);
	setStyleValue(node, Property::BoxShadowColor, cssColorStyleValue(shadow.color), source);
	setStyleValue(node, Property::BoxShadowAlpha, shadow.color.a, source);
	setStyleValue(node, Property::BoxShadowInset, 1, source);
}

int numericLength(double value)
{
	return rawNumber(value);
}

int numericOpacity(double value)
{
	if (!std::isfinite(value)) return 0;
	if (value <= 1.0) value *= 255.0;
	if (value < 0.0) value = 0.0;
	if (value > 255.0) value = 255.0;
	return static_cast<int>(value + 0.5);
}

int numericRotateTenths(double degrees)
{
	if (!std::isfinite(degrees)) return 0;
	return static_cast<int>(degrees * 10.0 + (degrees >= 0 ? 0.5 : -0.5));
}

int numericScalePermille(double value)
{
	if (!std::isfinite(value)) return 1000;
	if (value < 0.0) value = 0.0;
	if (value > 16.0) value = 16.0;
	return static_cast<int>(value * 1000.0 + 0.5);
}

int parseRotateTenths(const std::string &value)
{
	const auto rotate = value.find("rotate(");
	const char *start = value.c_str();
	if (rotate != std::string::npos) start = value.c_str() + rotate + 7;
	double degrees = std::strtod(start, nullptr);
	if (value.find("rad", static_cast<std::size_t>(start - value.c_str())) != std::string::npos) {
		degrees = degrees * 180.0 / 3.14159265358979323846;
	}
	return numericRotateTenths(degrees);
}

double parseAngleDegrees(const std::string &value)
{
	char *end = nullptr;
	double degrees = std::strtod(value.c_str(), &end);
	const std::string unit = end ? trimCssValue(std::string(end)) : std::string();
	if (unit.rfind("rad", 0) == 0) degrees = degrees * 180.0 / 3.14159265358979323846;
	if (unit.rfind("turn", 0) == 0) degrees = degrees * 360.0;
	return degrees;
}

int parseScalePermille(const std::string &value)
{
	if (toLowerAscii(trimCssValue(value)) == "none") return 1000;
	const double scale = std::strtod(value.c_str(), nullptr);
	return roundToInt(scale * 1000.0);
}

struct IndividualRotation {
	int angle = 0;
	int x = 0, y = 0, z = 1000000;
};

bool parseFiniteCssNumber(const std::string &value, double &number, std::string &unit)
{
	char *end = nullptr;
	number = std::strtod(value.c_str(), &end);
	if (!end || end == value.c_str() || !std::isfinite(number)) return false;
	unit = toLowerAscii(trimCssValue(end));
	return true;
}

bool parseIndividualRotation(const std::string &raw, IndividualRotation &out)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	if (value == "none") return true;
	auto parts = splitWords(value);
	if (parts.size() != 1 && parts.size() != 2 && parts.size() != 4) return false;
	double degrees; std::string unit;
	// The axis and angle use CSS && grammar: either may come first.
	auto isAngle = [&](const std::string &token) {
		return parseFiniteCssNumber(token, degrees, unit) &&
		    (unit == "deg" || unit == "rad" || unit == "turn" || unit == "grad" || (unit.empty() && degrees == 0));
	};
	if (!isAngle(parts.back())) {
		if (parts.size() == 1 || !isAngle(parts.front())) return false;
		std::rotate(parts.begin(), parts.begin() + 1, parts.end());
	}
	if (unit == "rad") degrees *= 180.0 / 3.14159265358979323846;
	else if (unit == "turn") degrees *= 360;
	else if (unit == "grad") degrees *= 0.9;
	else if (unit != "deg" && !(unit.empty() && degrees == 0)) return false;
	if (!std::isfinite(degrees)) return false;
	if (std::abs(degrees) > 3276) degrees = std::fmod(degrees, 360.0);
	out.angle = numericRotateTenths(degrees);
	if (parts.size() == 2) {
		out.z = 0;
		if (parts[0] == "x") out.x = 1000000;
		else if (parts[0] == "y") out.y = 1000000;
		else if (parts[0] == "z") out.z = 1000000;
		else return false;
	} else if (parts.size() == 4) {
		double axis[3];
		for (int i = 0; i < 3; ++i)
			if (!parseFiniteCssNumber(parts[i], axis[i], unit) || !unit.empty()) return false;
		const double magnitude = std::max({std::abs(axis[0]), std::abs(axis[1]), std::abs(axis[2])});
		if (magnitude == 0) { out.angle = 0; return true; }
		out.x = roundToInt(axis[0] / magnitude * 1000000);
		out.y = roundToInt(axis[1] / magnitude * 1000000);
		out.z = roundToInt(axis[2] / magnitude * 1000000);
	}
	return true;
}

bool parseIndividualScale(const std::string &raw, int (&out)[3])
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	out[0] = out[1] = out[2] = 1000;
	if (value == "none") return true;
	const auto parts = splitWords(value);
	if (parts.empty() || parts.size() > 3) return false;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		double factor; std::string unit;
		if (!parseFiniteCssNumber(parts[i], factor, unit)) return false;
		if (unit == "%") factor /= 100;
		else if (!unit.empty()) return false;
		out[i] = roundToInt(std::clamp(factor * 1000, -32768.0, 32767.0));
	}
	if (parts.size() == 1) out[1] = out[0];
	return true;
}

// Accumulate rotations in CSS source order, then express the result in the
// renderer's existing Rx * Ry * Rz storage convention. No new per-node matrix
// allocation is needed. Canonical axis sequences retain authored full turns
// so existing per-axis animation tracks do not collapse equivalent endpoints.
struct TransformRotationProduct {
	double matrix[9] = {1,0,0,0,1,0,0,0,1};
	int count = 0;
	bool canonical = true;
	int lastAxis = -1;
	int canonicalAngles[3] = {};

	bool append(const std::string &name, const std::string &arg)
	{
		IndividualRotation rotation;
		std::string value = arg;
		if (name == "rotate3d") {
			const auto args = splitTopLevel(arg, ',');
			if (args.size() != 4) return false;
			for (int i = 0; i < 3; ++i) {
				double number; std::string unit;
				if (!parseFiniteCssNumber(trimCssValue(args[i]), number, unit) || !unit.empty()) return false;
			}
			value = args[0] + " " + args[1] + " " + args[2] + " " + args[3];
		} else {
			if (splitWords(arg).size() != 1) return false;
			if (name == "rotatex") value = "x " + arg;
			else if (name == "rotatey") value = "y " + arg;
			else if (name != "rotate" && name != "rotatez") return false;
		}
		if (!parseIndividualRotation(value, rotation)) return false;
		++count;
		const int axisCount = int(rotation.x != 0) + int(rotation.y != 0) + int(rotation.z != 0);
		const int axis = axisCount != 1 ? -1 : rotation.x ? 0 : rotation.y ? 1 : 2;
		if (axis < lastAxis || axis < 0) canonical = false;
		if (canonical) {
			const int sign = (axis == 0 ? rotation.x : axis == 1 ? rotation.y : rotation.z) < 0 ? -1 : 1;
			canonicalAngles[axis] += sign * rotation.angle;
			// The stored angles are signed 16-bit tenths. Reduce only when the
			// composed value exceeds that range, before narrowing can wrap it.
			if (canonicalAngles[axis] < -32768 || canonicalAngles[axis] > 32767)
				canonicalAngles[axis] %= 3600;
			lastAxis = axis;
		}
		double x = rotation.x, y = rotation.y, z = rotation.z;
		const double norm = std::sqrt(x*x + y*y + z*z);
		if (!norm || rotation.angle % 3600 == 0) return true;
		x /= norm; y /= norm; z /= norm;
		const double angle = rotation.angle * 3.14159265358979323846 / 1800;
		const double c = std::cos(angle), sine = std::sin(angle), t = 1-c;
		const double r[9] = {c+x*x*t, x*y*t-z*sine, x*z*t+y*sine,
		                    y*x*t+z*sine, c+y*y*t, y*z*t-x*sine,
		                    z*x*t-y*sine, z*y*t+x*sine, c+z*z*t};
		double product[9] = {};
		for (int row=0; row<3; ++row) for (int col=0; col<3; ++col)
			for (int k=0; k<3; ++k) product[row*3+col] += matrix[row*3+k]*r[k*3+col];
		std::copy(product, product+9, matrix);
		return true;
	}

	void angles(int &x, int &y, int &z) const
	{
		if (!count) return;
		if (canonical) {
			x = canonicalAngles[0]; y = canonicalAngles[1]; z = canonicalAngles[2];
			return;
		}
		const double sy = std::clamp(matrix[2], -1.0, 1.0);
		const double ry = std::asin(sy);
		const bool singular = std::abs(std::cos(ry)) < 1e-7;
		const double rx = singular ? std::atan2(matrix[7], matrix[4]) : std::atan2(-matrix[5], matrix[8]);
		const double rz = singular ? 0 : std::atan2(-matrix[1], matrix[0]);
		constexpr double degrees = 180 / 3.14159265358979323846;
		x = numericRotateTenths(rx*degrees); y = numericRotateTenths(ry*degrees); z = numericRotateTenths(rz*degrees);
	}
};

struct TransformComponents {
	int translateOuterAxes = 0;
	int rotateX = 0;
	int rotateY = 0;
	int rotateZ = 0;
	int translateX = 0;
	int translateY = 0;
	int translateZ = 0;
	int translateXPercent = 0;
	int translateYPercent = 0;
	int scaleX = 1000;
	int scaleY = 1000;
	int scaleZ = 1000;
	bool hasRotateX = false;
	bool hasRotateY = false;
	bool hasRotateZ = false;
	bool hasTranslateX = false;
	bool hasTranslateY = false;
	bool hasTranslateZ = false;
	bool hasScaleX = false;
	bool hasScaleY = false;
	bool hasScaleZ = false;
};

int multiplyScalePermille(int left, int right)
{
	const long long product = static_cast<long long>(left) * right;
	const long long rounded = (product + (product >= 0 ? 500 : -500)) / 1000;
	return static_cast<int>(std::clamp<long long>(rounded, -32768, 32767));
}

bool parseSimplePercentPermille(const std::string &raw, int &out)
{
	const std::string value = trimCssValue(raw);
	if (value.empty()) return false;
	char *end = nullptr;
	const double percent = std::strtod(value.c_str(), &end);
	if (end == value.c_str() || !end || *end != '%') return false;
	++end;
	while (*end != '\0' && static_cast<unsigned char>(*end) <= ' ') ++end;
	if (*end != '\0') return false;
	int permille = roundToInt(percent * 10.0);
	if (permille < -32768) permille = -32768;
	if (permille > 32767) permille = 32767;
	out = permille;
	return true;
}

void parseTranslateComponent(const std::string &value,
                             int nodeId,
                             LengthAxis axis,
                             int &length,
                             int &percent,
                             bool &hasValue)
{
	hasValue = true;
	if (parseSimplePercentPermille(value, percent)) {
		length = 0;
		return;
	}
	length = parseLengthForNode(value, nodeId, axis);
	percent = 0;
}

TransformComponents parseTransformComponents(const std::string &value, int nodeId)
{
#if GEA_RECPROF
	g_profXformCalls++;
	const int64_t _xt = recNow();
	struct XformTimer { int64_t s; ~XformTimer() { g_profXformUs += recNow() - s; } } _xformTimer{_xt};
#endif
	TransformComponents out;
	TransformRotationProduct rotations;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t nameStart = i;
		while (i < value.size() && (std::isalpha(static_cast<unsigned char>(value[i])) || value[i] == '3')) ++i;
		if (i == nameStart || i >= value.size() || value[i] != '(') {
			++i;
			continue;
		}
		const std::string name = toLowerAscii(value.substr(nameStart, i - nameStart));
		const std::size_t argStart = ++i;
		int depth = 1;
		while (i < value.size() && depth > 0) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')') depth--;
			++i;
		}
		const std::string arg = value.substr(argStart, depth == 0 ? i - argStart - 1 : i - argStart);
		const auto args = splitTopLevel(arg, ',');
		const int axes = name == "translatex" ? 1 : name == "translatey" ? 2 : name == "translatez" ? 4 :
		    (name == "translate" || name == "translate3d") ? ((1 << std::min<std::size_t>(3, args.size())) - 1) : 0;
		out.translateOuterAxes = (out.translateOuterAxes & ~axes) |
		    (out.hasRotateX || out.hasRotateY || out.hasRotateZ ? 0 : axes);
		if (name == "rotate" || name == "rotatez" || name == "rotatex" || name == "rotatey" || name == "rotate3d") {
			if (rotations.append(name, arg)) {
				rotations.angles(out.rotateX, out.rotateY, out.rotateZ);
				out.hasRotateX = out.hasRotateY = out.hasRotateZ = true;
			}
		} else if (name == "translatex") {
			parseTranslateComponent(arg, nodeId, LengthAxis::Horizontal, out.translateX, out.translateXPercent, out.hasTranslateX);
		} else if (name == "translatey") {
			parseTranslateComponent(arg, nodeId, LengthAxis::Vertical, out.translateY, out.translateYPercent, out.hasTranslateY);
		} else if (name == "translatez") {
			out.translateZ = parseLengthForNode(arg, nodeId, LengthAxis::Horizontal);
			out.hasTranslateZ = true;
		} else if (name == "translate" || name == "translate3d") {
			if (!args.empty()) {
				parseTranslateComponent(args[0], nodeId, LengthAxis::Horizontal, out.translateX, out.translateXPercent, out.hasTranslateX);
			}
			if (args.size() > 1) {
				parseTranslateComponent(args[1], nodeId, LengthAxis::Vertical, out.translateY, out.translateYPercent, out.hasTranslateY);
			}
			if (args.size() > 2) {
				out.translateZ = parseLengthForNode(args[2], nodeId, LengthAxis::Horizontal);
				out.hasTranslateZ = true;
			}
		} else if (name == "scale") {
			const int sx = parseScalePermille(args.empty() ? arg : args[0]);
			const int sy = parseScalePermille(args.size() > 1 ? args[1] : (args.empty() ? arg : args[0]));
			out.scaleX = multiplyScalePermille(out.scaleX, sx);
			out.scaleY = multiplyScalePermille(out.scaleY, sy);
			out.hasScaleX = true;
			out.hasScaleY = true;
		} else if (name == "scale3d") {
			if (args.size() == 3) {
				out.scaleX = multiplyScalePermille(out.scaleX, parseScalePermille(args[0]));
				out.scaleY = multiplyScalePermille(out.scaleY, parseScalePermille(args[1]));
				out.scaleZ = multiplyScalePermille(out.scaleZ, parseScalePermille(args[2]));
				out.hasScaleX = out.hasScaleY = out.hasScaleZ = true;
			}
		} else if (name == "scalex") {
			out.scaleX = multiplyScalePermille(out.scaleX, parseScalePermille(arg));
			out.hasScaleX = true;
		} else if (name == "scaley") {
			out.scaleY = multiplyScalePermille(out.scaleY, parseScalePermille(arg));
			out.hasScaleY = true;
		} else if (name == "scalez") {
			out.scaleZ = multiplyScalePermille(out.scaleZ, parseScalePermille(arg));
			out.hasScaleZ = true;
		}
	}
	return out;
}

bool parseCompiledLengthSpec(const std::string &raw, CssLengthSpec &out, bool allowAuto);

int parseOriginPart(const std::string &part, int fallback)
{
	if (part.empty()) return fallback;
	if (part == "left" || part == "top") return 0;
	if (part == "center") return 500;
	if (part == "right" || part == "bottom") return 1000;
	char *end = nullptr;
	double v = std::strtod(part.c_str(), &end);
	if (part.find('%') != std::string::npos) return static_cast<int>(v * 10.0 + (v >= 0.0 ? 0.5 : -0.5));
	// Zero lengths, including unitless CSS zero, have the same origin as 0%.
	// Validate the length suffix so an unrecognized token is not treated as 0.
	if (v == 0.0 && end != part.c_str()) {
		CssLengthSpec zero;
		if (parseCompiledLengthSpec(part, zero, false)) return 0;
	}
	return fallback;
}

std::vector<std::string> splitWords(const std::string &value)
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t start = i;
		int depth = 0;
		while (i < value.size()) {
			const char c = value[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (static_cast<unsigned char>(c) <= ' ' && depth == 0) break;
			++i;
		}
		if (i > start) out.push_back(value.substr(start, i - start));
	}
	return out;
}

struct BoxLengths {
	int top = 0;
	int right = 0;
	int bottom = 0;
	int left = 0;
};

BoxLengths parseBoxLengths(const std::string &value, int nodeId = -1)
{
	const auto parts = splitWords(value);
	if (parts.empty()) return {};
	const int first = parseLengthForNode(parts[0], nodeId, LengthAxis::Vertical);
	const int second = parts.size() > 1 ? parseLengthForNode(parts[1], nodeId, LengthAxis::Horizontal) : first;
	const int third = parts.size() > 2 ? parseLengthForNode(parts[2], nodeId, LengthAxis::Vertical) : first;
	const int fourth = parts.size() > 3 ? parseLengthForNode(parts[3], nodeId, LengthAxis::Horizontal) : second;
	return {first, second, third, fourth};
}

bool hasDynamicCssValue(const std::string &value)
{
	const std::string lower = toLowerAscii(value);
	return lower.find("var(") != std::string::npos ||
	       lower.find("calc(") != std::string::npos ||
	       lower.find("min(") != std::string::npos ||
	       lower.find("max(") != std::string::npos ||
	       lower.find("clamp(") != std::string::npos;
}

bool parseCompiledLengthSpec(const std::string &raw, CssLengthSpec &out, bool allowAuto = false);
bool compileLengthExpressionSpec(const std::string &raw, CssLengthSpec &out);
int resolveCompiledLengthForNode(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth = 0);
ResolvedCssLength resolveCompiledLengthForNodeDetailed(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth = 0);
double resolveCompiledLengthPixels(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth = 0);

std::uint16_t storeCompiledCssLengthExpression(const CssLengthExpression &expression)
{
	auto &list = compiledCssLengthExpressions();
	if (list.size() >= kNoCompiledCssLengthExpression) return kNoCompiledCssLengthExpression;
	list.push_back(expression);
	return static_cast<std::uint16_t>(list.size() - 1);
}

bool storeCompiledCssLengthExpressionSpec(const CssLengthExpression &expression, CssLengthSpec &out)
{
	const std::uint16_t handle = storeCompiledCssLengthExpression(expression);
	if (handle == kNoCompiledCssLengthExpression) return false;
	out.unit = CssLengthUnit::Expression;
	out.value = static_cast<float>(handle);
	return true;
}

bool compileVarLengthSpec(const std::string &raw, CssLengthSpec &out)
{
	const std::string text = trimCssValue(raw);
	const std::string lower = toLowerAscii(text);
	if (!startsWith(lower, "var(") || text.empty() || text.back() != ')') return false;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || open + 1 >= text.size()) return false;
	const std::string inner = text.substr(open + 1, text.size() - open - 2);
	const auto parts = splitTopLevel(inner, ',');
	if (parts.empty()) return false;
	const std::string name = trimCssValue(parts[0]);
	if (name.rfind("--", 0) != 0) return false;
	CssLengthExpression expression;
	expression.kind = CssLengthExpressionKind::Var;
	expression.nameAtom = internCssAtom(name);
	if (parts.size() > 1) {
		if (!parseCompiledLengthSpec(parts[1], expression.a)) return false;
		expression.hasFallback = 1;
	}
	return storeCompiledCssLengthExpressionSpec(expression, out);
}

bool parseCompiledLengthSpec(const std::string &raw, CssLengthSpec &out, bool allowAuto)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	if (value.empty()) return false;
	if (value == "auto") {
		if (!allowAuto) return false;
		out.unit = CssLengthUnit::Auto;
		out.value = 0.0f;
		return true;
	}
	if (startsWith(value, "var(")) return compileVarLengthSpec(raw, out);
	if (startsWith(value, "calc(") || startsWith(value, "min(") ||
	    startsWith(value, "max(") || startsWith(value, "clamp("))
		return compileLengthExpressionSpec(raw, out);
	char *end = nullptr;
	const double parsed = std::strtod(value.c_str(), &end);
	if (end == value.c_str()) return false;
	const std::string unit = end ? trimCssValue(std::string(end)) : std::string();
	if (unit.empty()) out.unit = CssLengthUnit::Raw;
	else if (unit == "px") out.unit = CssLengthUnit::Px;
	else if (unit == "%") out.unit = CssLengthUnit::Percent;
	else if (unit == "vw") out.unit = CssLengthUnit::Vw;
	else if (unit == "vh") out.unit = CssLengthUnit::Vh;
	else if (unit == "vmin") out.unit = CssLengthUnit::Vmin;
	else if (unit == "vmax") out.unit = CssLengthUnit::Vmax;
	else if (unit == "dvw") out.unit = CssLengthUnit::Dvw;
	else if (unit == "dvh") out.unit = CssLengthUnit::Dvh;
	else if (unit == "ch" || unit == "em" || unit == "rem" || unit == "lh" || unit == "rlh") {
		// Font-relative dimensions retain a compiled expression through layout:
		// font declarations may occur later and can change after mounting.
		CssLengthExpression expression;
		expression.a = {static_cast<float>(parsed), unit == "ch" ? CssLengthUnit::Ch : unit == "em" ? CssLengthUnit::Em :
		    unit == "rem" ? CssLengthUnit::Rem : unit == "lh" ? CssLengthUnit::Lh : CssLengthUnit::Rlh};
		expression.b = {0.0f, CssLengthUnit::Px};
		return storeCompiledCssLengthExpressionSpec(expression, out);
	}
	else return false;
	out.value = static_cast<float>(parsed);
	return true;
}

bool parseCompiledCalcExpression(const std::string &expr, CssLengthSpec &out)
{
	const std::string text = trimCssValue(expr);
	for (char op : {'/', '*', '+', '-'}) {
		int depth = 0;
		for (std::size_t i = 0; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (c == op && depth == 0 && i > 0) {
				CssLengthExpression expression;
				if (!parseCompiledLengthSpec(text.substr(0, i), expression.a)) return false;
				if (op == '/' || op == '*') {
					const std::string right = trimCssValue(text.substr(i + 1));
					char *end = nullptr;
					const double scalar = std::strtod(right.c_str(), &end);
					if (end == right.c_str()) return false;
					while (*end == ' ') ++end;
					if (*end != '\0') return false;
					expression.scalar = static_cast<float>(scalar);
					expression.kind = op == '/'
					    ? CssLengthExpressionKind::Divide
					    : CssLengthExpressionKind::Multiply;
				} else {
					if (!parseCompiledLengthSpec(text.substr(i + 1), expression.b)) return false;
					expression.kind = op == '+'
					    ? CssLengthExpressionKind::Add
					    : CssLengthExpressionKind::Subtract;
				}
				return storeCompiledCssLengthExpressionSpec(expression, out);
			}
		}
	}
	return parseCompiledLengthSpec(text, out);
}

bool compileLengthExpressionSpec(const std::string &raw, CssLengthSpec &out)
{
	const std::string text = trimCssValue(raw);
	const std::string lower = toLowerAscii(text);
	if (startsWith(lower, "calc(")) return parseCompiledCalcExpression(functionInner(text, "calc"), out);
	if (startsWith(lower, "min(") || startsWith(lower, "max(")) {
		const bool isMin = startsWith(lower, "min(");
		const auto parts = splitTopLevel(functionInner(text, isMin ? "min" : "max"), ',');
		if (parts.empty()) return false;
		CssLengthSpec current;
		if (!parseCompiledLengthSpec(parts[0], current)) return false;
		for (std::size_t i = 1; i < parts.size(); ++i) {
			CssLengthExpression expression;
			expression.kind = isMin ? CssLengthExpressionKind::Min : CssLengthExpressionKind::Max;
			expression.a = current;
			if (!parseCompiledLengthSpec(parts[i], expression.b)) return false;
			if (!storeCompiledCssLengthExpressionSpec(expression, current)) return false;
		}
		out = current;
		return true;
	}
	if (startsWith(lower, "clamp(")) {
		const auto parts = splitTopLevel(functionInner(text, "clamp"), ',');
		if (parts.size() < 3) return false;
		CssLengthExpression expression;
		expression.kind = CssLengthExpressionKind::Clamp;
		if (!parseCompiledLengthSpec(parts[0], expression.a)) return false;
		if (!parseCompiledLengthSpec(parts[1], expression.b)) return false;
		if (!parseCompiledLengthSpec(parts[2], expression.c)) return false;
		return storeCompiledCssLengthExpressionSpec(expression, out);
	}
	return false;
}

const CssLengthSpec *cachedCompiledCssLengthSpec(const std::string &raw)
{
	const std::string key = trimCssValue(raw);
	if (key.empty()) return nullptr;
	auto &cache = compiledCssLengthCache();
	const auto it = cache.find(key);
	if (it != cache.end()) return &it->second;
	CssLengthSpec spec;
	if (!parseCompiledLengthSpec(key, spec)) return nullptr;
	const auto inserted = cache.emplace(key, spec);
	return &inserted.first->second;
}

ResolvedCssLength resolveCustomPropertyLengthForNode(const std::string &raw, int nodeId, LengthAxis axis, int depth)
{
	if (depth > 8) return {0, false, false};
	if (const CssLengthSpec *compiled = cachedCompiledCssLengthSpec(raw))
		return resolveCompiledLengthForNodeDetailed(*compiled, nodeId, axis, depth + 1);
	return {static_cast<double>(parseLengthForNode(raw, nodeId, axis)), false, false};
}

bool compiledLengthSpecHasCustomRuntimeInputs(const CssLengthSpec &length, int depth);

bool compiledLengthExpressionHasCustomRuntimeInputs(const CssLengthExpression &expression, int depth)
{
	if (depth > 8) return true;
	if (expression.kind == CssLengthExpressionKind::Var) return true;
	if (compiledLengthSpecHasCustomRuntimeInputs(expression.a, depth + 1)) return true;
	switch (expression.kind) {
	case CssLengthExpressionKind::Add:
	case CssLengthExpressionKind::Subtract:
	case CssLengthExpressionKind::Min:
	case CssLengthExpressionKind::Max:
		return compiledLengthSpecHasCustomRuntimeInputs(expression.b, depth + 1);
	case CssLengthExpressionKind::Clamp:
		return compiledLengthSpecHasCustomRuntimeInputs(expression.b, depth + 1) ||
		       compiledLengthSpecHasCustomRuntimeInputs(expression.c, depth + 1);
	case CssLengthExpressionKind::Multiply:
	case CssLengthExpressionKind::Divide:
		return false;
	case CssLengthExpressionKind::Var:
		return true;
	}
	return true;
}

bool compiledLengthSpecHasCustomRuntimeInputs(const CssLengthSpec &length, int depth)
{
	if (depth > 8) return true;
	if (length.unit == CssLengthUnit::Percent || length.unit == CssLengthUnit::Ch || length.unit == CssLengthUnit::Em || length.unit == CssLengthUnit::Rem ||
	    length.unit == CssLengthUnit::Lh || length.unit == CssLengthUnit::Rlh) return true;
	if (length.unit != CssLengthUnit::Expression) return false;
	const auto &list = compiledCssLengthExpressions();
	const std::uint16_t handle = static_cast<std::uint16_t>(length.value);
	if (handle >= list.size()) return true;
	return compiledLengthExpressionHasCustomRuntimeInputs(list[handle], depth + 1);
}

// Trace late inputs through expressions and custom-property substitutions.
// Percentage dimensions need their containing block; ch dimensions need the
// final font. Pure percentages still use the dedicated percentage slot.
bool lengthDependsOnInput(const CssLengthSpec &length, int nodeId, CssLengthUnit input, int depth = 0)
{
	if (depth > 8) return false;
	if (length.unit == input) return true;
	if (length.unit != CssLengthUnit::Expression) return false;
	const auto &list = compiledCssLengthExpressions();
	const auto handle = static_cast<std::uint16_t>(length.value);
	if (handle >= list.size()) return false;
	const CssLengthExpression expression = list[handle];
	if (expression.kind == CssLengthExpressionKind::Var) {
		if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, expression.nameAtom)) {
			CssLengthSpec spec;
			if (entry->hasLength()) {
				spec.value = entry->lengthValue;
				spec.unit = static_cast<CssLengthUnit>(entry->lengthUnit);
			} else if (!parseCompiledLengthSpec(entry->value, spec)) return false;
			return lengthDependsOnInput(spec, nodeId, input, depth + 1);
		}
		return expression.hasFallback && lengthDependsOnInput(expression.a, nodeId, input, depth + 1);
	}
	if (lengthDependsOnInput(expression.a, nodeId, input, depth + 1)) return true;
	if (expression.kind == CssLengthExpressionKind::Multiply || expression.kind == CssLengthExpressionKind::Divide)
		return false;
	return lengthDependsOnInput(expression.b, nodeId, input, depth + 1) ||
	       (expression.kind == CssLengthExpressionKind::Clamp &&
	        lengthDependsOnInput(expression.c, nodeId, input, depth + 1));
}

bool lengthDependsOnFont(const CssLengthSpec &length, int nodeId)
{
	return lengthDependsOnInput(length, nodeId, CssLengthUnit::Ch) ||
	       lengthDependsOnInput(length, nodeId, CssLengthUnit::Em) ||
	       lengthDependsOnInput(length, nodeId, CssLengthUnit::Rem) ||
	       lengthDependsOnInput(length, nodeId, CssLengthUnit::Lh) ||
	       lengthDependsOnInput(length, nodeId, CssLengthUnit::Rlh);
}

bool lengthNeedsLayout(const CssLengthSpec &length, int nodeId)
{
	return lengthDependsOnInput(length, nodeId, CssLengthUnit::Percent) ||
	       lengthDependsOnFont(length, nodeId);
}

bool tryPreResolveStaticCustomLengthSpec(const CssLengthSpec &length, int nodeId, CssLengthSpec &out)
{
	if (length.unit != CssLengthUnit::Expression) return false;
	if (compiledLengthSpecHasCustomRuntimeInputs(length, 0)) return false;
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, nodeId, LengthAxis::None);
	if (resolved.isPercent || resolved.isAuto) return false;
	out.unit = CssLengthUnit::Raw;
	out.value = static_cast<float>(resolved.value);
	return true;
}

ResolvedCssLength resolveCompiledLengthExpressionForNode(const CssLengthExpression &expression,
                                                         int nodeId,
                                                         LengthAxis axis,
                                                         int depth);

bool tryResolveStaticLengthExpressionCached(std::uint16_t handle,
                                            const CssLengthExpression &expression,
                                            int nodeId,
                                            LengthAxis axis,
                                            int depth,
                                            ResolvedCssLength &out)
{
	auto &cache = staticLengthExpressionResolutionCache();
	if (cache.size() <= handle) cache.resize(static_cast<std::size_t>(handle) + 1);
	StaticLengthExpressionResolution &entry = cache[handle];
	if (entry.status == 0) {
		CssLengthSpec spec;
		spec.unit = CssLengthUnit::Expression;
		spec.value = static_cast<float>(handle);
		entry.status = compiledLengthSpecHasCustomRuntimeInputs(spec, 0) ? 1 : 2;
	}
	if (entry.status != 2) return false;
	if (entry.valid) {
		out = entry.value;
		return true;
	}
	entry.value = resolveCompiledLengthExpressionForNode(expression, nodeId, axis, depth);
	entry.valid = 1;
	out = entry.value;
	return true;
}

bool tryResolveDynamicLengthExpressionCached(std::uint16_t handle,
                                             int nodeId,
                                             LengthAxis axis,
                                             ResolvedCssLength &out)
{
	for (const DynamicLengthExpressionResolution &entry : dynamicLengthExpressionResolutionCache()) {
		if (!entry.valid ||
		    entry.handle != handle ||
		    entry.nodeId != nodeId ||
		    entry.basis != percentBasisForNode(nodeId, axis) ||
		    entry.axis != static_cast<std::uint8_t>(axis))
			continue;
		out = entry.value;
		return true;
	}
	return false;
}

void storeDynamicLengthExpressionCached(std::uint16_t handle,
                                        int nodeId,
                                        LengthAxis axis,
                                        ResolvedCssLength value)
{
	if (handle == kNoCompiledCssLengthExpression || nodeId < 0) return;
	std::uint8_t &cursor = dynamicLengthExpressionResolutionCacheCursor();
	DynamicLengthExpressionResolution &entry = dynamicLengthExpressionResolutionCache()[cursor];
	entry.value = value;
	entry.handle = handle;
	entry.nodeId = static_cast<std::int16_t>(std::min(nodeId, 32767));
	entry.basis = percentBasisForNode(nodeId, axis);
	entry.axis = static_cast<std::uint8_t>(axis);
	entry.valid = 1;
	cursor = static_cast<std::uint8_t>((cursor + 1) % kDynamicLengthExpressionResolutionCacheSize);
}

ResolvedCssLength resolveCompiledLengthExpressionForNode(const CssLengthExpression &expression,
                                                         int nodeId,
                                                         LengthAxis axis,
                                                         int depth)
{
	if (expression.kind == CssLengthExpressionKind::Var) {
		if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, expression.nameAtom)) {
			if (entry->hasLength()) {
				CssLengthSpec spec;
				spec.value = entry->lengthValue;
				spec.unit = static_cast<CssLengthUnit>(entry->lengthUnit);
				return resolveCompiledLengthForNodeDetailed(spec, nodeId, axis, depth + 1);
			}
			return resolveCustomPropertyLengthForNode(entry->value, nodeId, axis, depth + 1);
		}
		if (expression.hasFallback)
			return resolveCompiledLengthForNodeDetailed(expression.a, nodeId, axis, depth + 1);
		return {0, false, false};
	}
	// A percent-homogeneous calc() -- `calc(50% - 1.2%)` -- IS a percentage. Resolving
	// it to px here uses percentBasisForNode() at style-apply time, which falls back to
	// the VIEWPORT whenever the node has no parent yet. Report it as a percent (permille)
	// so the caller can store it in the *Percent companion and let LAYOUT resolve it
	// against the real containing block, exactly as a bare `50%` already does.
	{
		const ResolvedCssLength da = resolveCompiledLengthForNodeDetailed(expression.a, nodeId, axis, depth + 1);
		if (da.isPercent && (expression.kind == CssLengthExpressionKind::Add ||
		                     expression.kind == CssLengthExpressionKind::Subtract)) {
			const ResolvedCssLength db =
			    resolveCompiledLengthForNodeDetailed(expression.b, nodeId, axis, depth + 1);
			if (db.isPercent)
				return {expression.kind == CssLengthExpressionKind::Add ? da.value + db.value
				                                                       : da.value - db.value,
				        true, false};
		}
	}
	const double a = resolveCompiledLengthPixels(expression.a, nodeId, axis, depth + 1);
	switch (expression.kind) {
	case CssLengthExpressionKind::Add:
		return {a + resolveCompiledLengthPixels(expression.b, nodeId, axis, depth + 1), false, false};
	case CssLengthExpressionKind::Subtract:
		return {a - resolveCompiledLengthPixels(expression.b, nodeId, axis, depth + 1), false, false};
	case CssLengthExpressionKind::Multiply:
		return {a * static_cast<double>(expression.scalar), false, false};
	case CssLengthExpressionKind::Divide:
		return {expression.scalar == 0.0f
		            ? 0
		            : a / static_cast<double>(expression.scalar),
		        false,
		        false};
	case CssLengthExpressionKind::Min:
		return {std::min(a, resolveCompiledLengthPixels(expression.b, nodeId, axis, depth + 1)), false, false};
	case CssLengthExpressionKind::Max:
		return {std::max(a, resolveCompiledLengthPixels(expression.b, nodeId, axis, depth + 1)), false, false};
	case CssLengthExpressionKind::Clamp: {
		const double preferred = resolveCompiledLengthPixels(expression.b, nodeId, axis, depth + 1);
		const double maxValue = resolveCompiledLengthPixels(expression.c, nodeId, axis, depth + 1);
		return {std::max(a, std::min(preferred, maxValue)), false, false};
	}
	case CssLengthExpressionKind::Var:
		return {0, false, false};
	}
	return {0, false, false};
}

ResolvedCssLength resolveCompiledLengthForNodeDetailed(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth)
{
	const double value = static_cast<double>(length.value);
	switch (length.unit) {
	case CssLengthUnit::Raw:
		return {value, false, false};
	case CssLengthUnit::Px:
		return {value * g_device_pixel_ratio, false, false};
	case CssLengthUnit::Ch:
		return {value * zeroAdvanceForNode(nodeId), false, false};
	case CssLengthUnit::Em:
		return {value * currentFontSizeForNode(fontMetricBasisNode(nodeId)), false, false};
	case CssLengthUnit::Rem:
		return {value * rootFontSizeForNode(nodeId), false, false};
	case CssLengthUnit::Lh:
		return {value * lineHeightForLength(nodeId, false), false, false};
	case CssLengthUnit::Rlh:
		return {value * lineHeightForLength(nodeId, true), false, false};
	case CssLengthUnit::Percent:
		return {value * 10.0, true, false};
	case CssLengthUnit::Vw:
	case CssLengthUnit::Dvw:
		return {g_viewport_width > 0 ? value * g_viewport_width / 100.0 : 0, false, false};
	case CssLengthUnit::Vh:
	case CssLengthUnit::Dvh:
		return {g_viewport_height > 0 ? value * g_viewport_height / 100.0 : 0, false, false};
	case CssLengthUnit::Vmin:
		return {g_viewport_width > 0 && g_viewport_height > 0
		            ? value * std::min(g_viewport_width, g_viewport_height) / 100.0
		            : 0,
		        false,
		        false};
	case CssLengthUnit::Vmax:
		return {g_viewport_width > 0 && g_viewport_height > 0
		            ? value * std::max(g_viewport_width, g_viewport_height) / 100.0
		            : 0,
		        false,
		        false};
	case CssLengthUnit::Auto:
		return {kUnset, false, true};
	case CssLengthUnit::Expression:
		break;
	case CssLengthUnit::Invalid:
		return {0, false, false};
	}

	if (depth > 8) return {0, false, false};
	const auto &list = compiledCssLengthExpressions();
	const std::uint16_t handle = static_cast<std::uint16_t>(value);
	if (handle >= list.size()) return {0, false, false};
	const CssLengthExpression &expression = list[handle];
	ResolvedCssLength cached;
	if (tryResolveStaticLengthExpressionCached(handle, expression, nodeId, axis, depth, cached)) return cached;
	// The ordinary dynamic cache keys containing-block dimensions. Font metrics
	// are another dependency, so never reuse that cache for font-relative input.
	const bool fontRelative = g_fontSizeBasisNode >= 0 || lengthDependsOnFont(length, nodeId);
	if (!fontRelative && tryResolveDynamicLengthExpressionCached(handle, nodeId, axis, cached)) return cached;
	ResolvedCssLength resolved = resolveCompiledLengthExpressionForNode(expression, nodeId, axis, depth);
	if (!fontRelative) storeDynamicLengthExpressionCached(handle, nodeId, axis, resolved);
	return resolved;
}

double resolveCompiledLengthPixels(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, nodeId, axis, depth);
	if (resolved.isPercent) {
		const int basis = g_fontSizeBasisNode >= 0 ? currentFontSizeForNode(fontMetricBasisNode(nodeId)) : percentBasisForNode(nodeId, axis);
		return resolved.value * basis / 1000.0;
	}
	return resolved.value;
}

int resolveCompiledLengthForNode(const CssLengthSpec &length, int nodeId, LengthAxis axis, int depth)
{
	return roundToInt(resolveCompiledLengthPixels(length, nodeId, axis, depth));
}

bool isNegativeLengthLiteral(const std::string &value)
{
	// strtod accepts leading whitespace and a signed numeric token, but not a
	// function name. Negative results of calc() are clamped, not rejected here.
	return std::strtod(value.c_str(), nullptr) < 0.0;
}

int resolveBorderWidth(const CssLengthSpec &length, int nodeId)
{
	// Negative literals are invalid declarations; negative calculations clamp to
	// the property's nonnegative range before device-pixel snapping.
	if ((length.unit != CssLengthUnit::Expression && length.value < 0) ||
	    lengthDependsOnInput(length, nodeId, CssLengthUnit::Percent)) return -1;
	return snapBorderWidth(resolveCompiledLengthPixels(length, nodeId, LengthAxis::None));
}

int parseBorderWidth(const std::string &raw, int nodeId)
{
	if (isNegativeLengthLiteral(raw)) return -1;
	const CssLengthSpec *length = cachedCompiledCssLengthSpec(raw);
	return length ? resolveBorderWidth(*length, nodeId) : -1;
}

int resolveFontSizeLength(const CssLengthSpec &length, int nodeId)
{
	const int previous = g_fontSizeBasisNode;
	g_fontSizeBasisNode = nodeId;
	const int value = resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical);
	g_fontSizeBasisNode = previous;
	return value;
}

bool compileBoxLengthSpecs(const std::string &value, CssCompiledValue &compiled, bool allowAuto = false)
{
	const auto parts = splitWords(value);
	if (parts.empty() || parts.size() > 4) return false;
	CssLengthSpec first;
	if (!parseCompiledLengthSpec(parts[0], first, allowAuto)) return false;
	CssLengthSpec second = first;
	CssLengthSpec third = first;
	CssLengthSpec fourth = second;
	if (parts.size() > 1 && !parseCompiledLengthSpec(parts[1], second, allowAuto)) return false;
	if (parts.size() > 2 && !parseCompiledLengthSpec(parts[2], third, allowAuto)) return false;
	if (parts.size() > 3 && !parseCompiledLengthSpec(parts[3], fourth, allowAuto)) return false;
	if (parts.size() <= 2) third = first;
	if (parts.size() <= 3) fourth = second;
	compiled.lengths[0] = first;
	compiled.lengths[1] = second;
	compiled.lengths[2] = third;
	compiled.lengths[3] = fourth;
	return true;
}

bool compileSimpleColor(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (lower.empty() || lower.find("gradient(") != std::string::npos || lower.find("var(") != std::string::npos)
		return false;
	const ParsedCssColor color = parseCssColor(firstColorToken(text));
	if (!color.valid) return false;
	compiled.values[0] = static_cast<std::int32_t>(cssColorStyleValue(color));
	compiled.values[1] = static_cast<std::int32_t>(cssColorNative(color));
	compiled.values[2] = color.a;
	return true;
}

bool compileColorVarValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (!startsWith(lower, "var(") || text.empty() || text.back() != ')') return false;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || open + 1 >= text.size()) return false;
	const auto parts = splitTopLevel(text.substr(open + 1, text.size() - open - 2), ',');
	if (parts.empty()) return false;
	const std::string name = trimCssValue(parts[0]);
	if (name.rfind("--", 0) != 0) return false;
	compiled.values[0] = static_cast<std::int32_t>(internCssAtom(name));
	compiled.aux = 0;
	if (parts.size() > 1) {
		const CachedCssColor fallback = cachedCssColorForValue(parts[1]);
		if (!fallback.valid) return false;
		compiled.values[1] = fallback.styleColor;
		compiled.values[2] = fallback.nativeColor;
		compiled.values[3] = fallback.alpha;
		compiled.aux = 1;
	}
	return true;
}

struct CompiledCssColorRef {
	CachedCssColor color;
	CssAtomId atom = kInvalidCssAtom;
	std::uint8_t hasFallback = 0;
	bool valid = false;
};

bool compileDirectColorVarRef(const std::string &value, CssAtomId &atom, CachedCssColor &fallback, std::uint8_t &hasFallback)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (!startsWith(lower, "var(") || text.empty() || text.back() != ')') return false;
	const std::size_t open = text.find('(');
	if (open == std::string::npos || open + 1 >= text.size()) return false;
	const auto parts = splitTopLevel(text.substr(open + 1, text.size() - open - 2), ',');
	if (parts.empty()) return false;
	const std::string name = trimCssValue(parts[0]);
	if (name.rfind("--", 0) != 0) return false;
	atom = internCssAtom(name);
	hasFallback = 0;
	fallback = {};
	if (parts.size() > 1) {
		fallback = cachedCssColorForValue(parts[1]);
		if (!fallback.valid) return false;
		hasFallback = 1;
	}
	return true;
}

bool compileGradientColorStopRef(const std::string &stop, CompiledCssColorRef &out)
{
	const std::string token = gradientColorTokenForStop(stop);
	if (token.empty()) return false;
	const std::string lower = toLowerAscii(trimCssValue(token));
	if (startsWith(lower, "var(")) {
		if (!compileDirectColorVarRef(token, out.atom, out.color, out.hasFallback)) return false;
		out.valid = true;
		return true;
	}
	const CachedCssColor color = cachedCssColorForValue(token);
	if (!color.valid) return false;
	out.color = color;
	out.valid = true;
	return true;
}

void setCompiledLinearGradientFrom(CssCompiledLinearGradient &gradient, const CompiledCssColorRef &color)
{
	gradient.fromStyleColor = color.color.styleColor;
	gradient.fromNativeColor = static_cast<style_color_t>(color.color.nativeColor);
	gradient.fromAlpha = color.hasFallback || color.atom == kInvalidCssAtom ? color.color.alpha : 255;
	gradient.fromColorAtom = color.atom;
	gradient.fromColorHasFallback = color.hasFallback;
}

void setCompiledLinearGradientMid(CssCompiledLinearGradient &gradient, const CompiledCssColorRef &color)
{
	gradient.midNativeColor = static_cast<style_color_t>(color.color.nativeColor);
	gradient.midAlpha = color.hasFallback || color.atom == kInvalidCssAtom ? color.color.alpha : 255;
	gradient.midColorAtom = color.atom;
	gradient.midColorHasFallback = color.hasFallback;
}

void setCompiledLinearGradientTo(CssCompiledLinearGradient &gradient, const CompiledCssColorRef &color)
{
	gradient.toNativeColor = static_cast<style_color_t>(color.color.nativeColor);
	gradient.toAlpha = color.hasFallback || color.atom == kInvalidCssAtom ? color.color.alpha : 255;
	gradient.toColorAtom = color.atom;
	gradient.toColorHasFallback = color.hasFallback;
}

bool compileLineHeightValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	if (text.empty()) return false;
	const std::string lower = toLowerAscii(text);
	if (lower == "normal") {
		compiled.aux = 0;
		return true;
	}
	char *end = nullptr;
	const double scalar = std::strtod(text.c_str(), &end);
	if (end && end != text.c_str()) {
		while (*end == ' ') ++end;
		if (*end == '\0') {
			compiled.aux = 1;
			compiled.lengths[0].value = static_cast<float>(scalar);
			compiled.lengths[0].unit = CssLengthUnit::Raw;
			return true;
		}
		if (end[0] == '%' && end[1] == '\0') {
			compiled.aux = 2;
			compiled.lengths[0].value = static_cast<float>(scalar);
			compiled.lengths[0].unit = CssLengthUnit::Percent;
			return true;
		}
	}
	if (!parseCompiledLengthSpec(text, compiled.lengths[0])) return false;
	compiled.aux = 3;
	return true;
}

bool compileRightFadeMaskWidthValue(const std::string &rawValue, CssCompiledValue &compiled)
{
	const std::string value = trimCssValue(rawValue);
	const std::string lower = toLowerAscii(value);
	if (lower.empty() || lower == "none") {
		compiled.lengths[0].unit = CssLengthUnit::Raw;
		compiled.lengths[0].value = 0.0f;
		return true;
	}
	if (lower.find("linear-gradient") == std::string::npos ||
	    lower.find("to right") == std::string::npos ||
	    lower.find("transparent") == std::string::npos) {
		return false;
	}

	const std::size_t calcStart = lower.find("calc(");
	if (calcStart == std::string::npos) return false;
	const std::size_t innerStart = calcStart + 5;
	int depth = 1;
	std::size_t end = innerStart;
	for (; end < value.size(); ++end) {
		if (value[end] == '(') depth++;
		else if (value[end] == ')') {
			if (--depth == 0) break;
		}
	}
	if (end <= innerStart || end >= value.size()) return false;

	const std::string inner = trimCssValue(value.substr(innerStart, end - innerStart));
	const std::string innerLower = toLowerAscii(inner);
	if (innerLower.rfind("100%", 0) != 0) return false;
	depth = 0;
	for (std::size_t i = 0; i < inner.size(); ++i) {
		const char c = inner[i];
		if (c == '(') depth++;
		else if (c == ')' && depth > 0) depth--;
		else if (c == '-' && depth == 0)
			return parseCompiledLengthSpec(inner.substr(i + 1), compiled.lengths[0]);
	}
	return false;
}

bool isUnsetFlexBasisToken(const std::string &value)
{
	const std::string lower = toLowerAscii(trimCssValue(value));
	return lower == "auto" ||
	       lower == "content" ||
	       lower == "max-content" ||
	       lower == "min-content" ||
	       lower == "fit-content";
}

bool compileFlexBasisValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	if (text.empty()) return false;
	compiled.aux = 0;
	if (isUnsetFlexBasisToken(text)) return true;
	if (!parseCompiledLengthSpec(text, compiled.lengths[0])) return false;
	if (compiled.lengths[0].unit != CssLengthUnit::Expression && compiled.lengths[0].value < 0) return false;
	compiled.aux = 1;
	return true;
}

bool compileFlexShorthandValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	if (text.empty()) return false;
	double grow = 0.0;
	double shrink = 1.0;
	int numberIndex = 0;
	CssLengthSpec basis;
	bool hasBasis = false;

	for (const auto &tokenRaw : splitFunctionAwareWords(text)) {
		const std::string token = trimCssValue(tokenRaw);
		const std::string lower = toLowerAscii(token);
		if (token.empty()) continue;
		if (lower == "none") {
			grow = 0.0;
			shrink = 0.0;
			hasBasis = false;
			continue;
		}
		if (isUnsetFlexBasisToken(token)) {
			hasBasis = false;
			continue;
		}
		char *endp = nullptr;
		const double num = std::strtod(token.c_str(), &endp);
		const bool parsedNumber = endp && endp != token.c_str();
		const bool hasUnit = parsedNumber && *endp != '\0';
		if (hasUnit || !parsedNumber) {
			CssLengthSpec candidate;
			if (!parseCompiledLengthSpec(token, candidate)) return false;
			if (candidate.unit != CssLengthUnit::Expression && candidate.value < 0) return false;
			basis = candidate;
			hasBasis = true;
			continue;
		}
		if (parsedNumber) {
			if (numberIndex == 0) grow = num;
			else if (numberIndex == 1) shrink = num;
			numberIndex++;
			continue;
		}
		return false;
	}

	compiled.values[0] = rawNumber(grow);
	compiled.values[1] = rawNumber(shrink);
	compiled.aux = hasBasis ? 1 : 0;
	if (hasBasis) compiled.lengths[0] = basis;
	return true;
}

bool compileFilterBlurValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lower = toLowerAscii(text);
	if (lower.empty() || lower == "none") {
		compiled.aux = 0;
		return true;
	}
	const std::size_t blur = lower.find("blur(");
	if (blur == std::string::npos) {
		compiled.aux = 0;
		return true;
	}
	std::size_t i = blur + 5;
	int depth = 1;
	const std::size_t argStart = i;
	while (i < text.size() && depth > 0) {
		if (text[i] == '(') depth++;
		else if (text[i] == ')') depth--;
		++i;
	}
	if (depth != 0 || i <= argStart) return false;
	if (!parseCompiledLengthSpec(text.substr(argStart, i - argStart - 1), compiled.lengths[0])) return false;
	compiled.aux = 1;
	return true;
}

bool compileBoxShadowValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string text = trimCssValue(value);
	const std::string lowerText = toLowerAscii(text);
	compiled.aux = 0;
	compiled.values[0] = 0;
	compiled.values[1] = 0;
	if (text.empty() || lowerText == "none") return true;

	for (const auto &rawLayer : splitTopLevel(text, ',')) {
		const auto tokens = splitFunctionAwareWords(rawLayer);
		bool inset = false;
		std::vector<CssLengthSpec> lengths;
		ParsedCssColor color;
		color.r = 0;
		color.g = 0;
		color.b = 0;
		color.a = 255;
		color.valid = true;
		for (const auto &token : tokens) {
			const std::string lower = toLowerAscii(trimCssValue(token));
			if (lower == "inset") {
				inset = true;
				continue;
			}
			if (isBoxShadowColorToken(token)) {
				const ParsedCssColor parsed = parseCssColor(firstColorToken(token));
				if (parsed.valid) color = parsed;
				continue;
			}
			if (lower == "outset") continue;
			CssLengthSpec length;
			if (!parseCompiledLengthSpec(token, length)) return false;
			if (lengths.size() < 4) lengths.push_back(length);
		}
		if (!inset) continue;
		compiled.aux = 1;
		for (std::size_t i = 0; i < lengths.size(); ++i) compiled.lengths[i] = lengths[i];
		compiled.values[0] = static_cast<std::int32_t>(cssColorStyleValue(color));
		compiled.values[1] = color.a;
		return true;
	}
	return true;
}

bool boxShadowValueHasInsetLayer(const std::string &value)
{
	for (const auto &rawLayer : splitTopLevel(value, ',')) {
		for (const auto &token : splitFunctionAwareWords(rawLayer)) {
			if (toLowerAscii(trimCssValue(token)) == "inset") return true;
		}
	}
	return false;
}

bool compileBorderWidthBox(const std::string &value, CssCompiledValue &compiled)
{
	const auto parts = splitFunctionAwareWords(value);
	if (parts.empty() || parts.size() > 4) return false;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (isNegativeLengthLiteral(parts[i]) || !parseCompiledLengthSpec(parts[i], compiled.lengths[i])) return false;
	}
	if (parts.size() < 2) compiled.lengths[1] = compiled.lengths[0];
	if (parts.size() < 3) compiled.lengths[2] = compiled.lengths[0];
	if (parts.size() < 4) compiled.lengths[3] = compiled.lengths[1];
	return true;
}

bool compileBorderShorthandValue(const std::string &value, CssCompiledValue &compiled)
{
	const auto parts = splitFunctionAwareWords(value);
	if (parts.empty()) return false;
	compiled.lengths[0] = {3.0f, CssLengthUnit::Px};
	compiled.values[2] = 0;
	bool foundWidth = false;
	bool noStroke = false;
	for (const auto &part : parts) {
		const std::string token = toLowerAscii(part);
		if (token == "currentcolor") continue;
		if (token == "none" || token == "hidden") { noStroke = true; continue; }
		if (token == "groove" || token == "ridge" || token == "inset" || token == "outset") {
			compiled.values[2] = token == "groove" ? 1 : token == "ridge" ? 2 : token == "inset" ? 3 : 4;
			continue;
		}
		if (token == "solid" || token == "dashed" || token == "dotted" || token == "double") continue;
		if (part[0] == '#' || token.find("rgb") != std::string::npos || token == "transparent") continue;
		if (foundWidth || isNegativeLengthLiteral(part)) return false;
		if (token == "thin" || token == "medium" || token == "thick")
			compiled.lengths[0] = {token == "thin" ? 1.0f : token == "medium" ? 3.0f : 5.0f, CssLengthUnit::Px};
		else if (!parseCompiledLengthSpec(part, compiled.lengths[0])) return false;
		foundWidth = true;
	}
	if (noStroke) compiled.lengths[0] = {0.0f, CssLengthUnit::Px};
	compiled.aux = 0;
	for (const auto &part : parts) {
		if (part.empty()) continue;
		if (part[0] == '#' || part.find("rgb") != std::string::npos || toLowerAscii(part) == "transparent") {
			const ParsedCssColor color = parseCssColor(part);
			if (!color.valid) return false;
			compiled.aux = 1;
			compiled.values[0] = static_cast<std::int32_t>(cssColorStyleValue(color));
			compiled.values[1] = color.a;
			return true;
		}
	}
	return true;
}

int borderSideForDeclaration(CssDeclarationId declaration)
{
	switch (declaration) {
	case CssDeclarationId::BorderTop: return 0;
	case CssDeclarationId::BorderRight: return 1;
	case CssDeclarationId::BorderBottom: return 2;
	case CssDeclarationId::BorderLeft: return 3;
	default: return -1;
	}
}

bool compileBorderRadiusValue(const std::string &value, CssCompiledValue &compiled)
{
	const auto axes = splitTopLevel(value, '/');
	const auto parts = splitWords(axes.empty() ? value : axes[0]);
	if (parts.empty() || parts.size() > 4) return false;
	if (!parseCompiledLengthSpec(parts[0], compiled.lengths[0])) return false;
	compiled.lengths[1] = compiled.lengths[0];
	compiled.lengths[2] = compiled.lengths[0];
	compiled.lengths[3] = compiled.lengths[1];
	if (parts.size() > 1 && !parseCompiledLengthSpec(parts[1], compiled.lengths[1])) return false;
	if (parts.size() > 2 && !parseCompiledLengthSpec(parts[2], compiled.lengths[2])) return false;
	if (parts.size() > 3 && !parseCompiledLengthSpec(parts[3], compiled.lengths[3])) return false;
	if (parts.size() <= 2) compiled.lengths[2] = compiled.lengths[0];
	if (parts.size() <= 3) compiled.lengths[3] = compiled.lengths[1];
	return true;
}

bool compileGridTrackSpec(const std::string &rawToken, CssCompiledGridTrack &track)
{
	const std::string token = trimCssValue(rawToken);
	const std::string lower = toLowerAscii(token);
	if (token.empty()) return false;
	if (lower == "auto") {
		track.type = 0;
		track.value = 0;
		track.length = {};
		return true;
	}
	if (startsWith(lower, "minmax(")) {
		const auto parts = splitTopLevel(functionInner(token, "minmax"), ',');
		if (parts.empty()) return false;
		const std::string preferred = parts.size() > 1 ? parts[1] : parts[0];
		return compileGridTrackSpec(preferred, track);
	}
	if (lower.size() > 2 && lower.substr(lower.size() - 2) == "fr") {
		char *end = nullptr;
		const double fr = std::strtod(lower.c_str(), &end);
		if (end == lower.c_str()) return false;
		while (*end == ' ') ++end;
		if (end[0] != 'f' || end[1] != 'r') return false;
		track.type = 2;
		track.value = static_cast<std::int16_t>(std::max(1, rawNumber(fr)));
		track.length = {};
		return true;
	}
	CssLengthSpec length;
	if (!parseCompiledLengthSpec(token, length)) return false;
	track.type = 1;
	track.value = 0;
	track.length = length;
	return true;
}

bool appendCompiledGridTrackToken(const std::string &token, CssCompiledGridTemplate &grid)
{
	CssCompiledGridTrack track;
	if (!compileGridTrackSpec(token, track)) return false;
	if (grid.count < kMaxGridTracks) grid.tracks[grid.count++] = track;
	return true;
}

std::uint16_t storeCompiledCssGridTemplate(const CssCompiledGridTemplate &grid)
{
	auto &list = compiledCssGridTemplates();
	if (list.size() >= kNoCompiledCssGridTemplate) return kNoCompiledCssGridTemplate;
	list.push_back(grid);
	return static_cast<std::uint16_t>(list.size() - 1);
}

bool parseGridTemplateSpec(const std::string &value, CssCompiledGridTemplate &grid)
{
	if (toLowerAscii(trimCssValue(value)) == "none") return true;
	if (trimCssValue(value).empty()) return false;
	for (const auto &token : splitFunctionAwareWords(value)) {
		const std::string lower = toLowerAscii(trimCssValue(token));
		if (startsWith(lower, "repeat(")) {
			if (lower.back() != ')') return false;
			const auto parts = splitTopLevel(functionInner(token, "repeat"), ',');
			int count;
			if (parts.size() != 2 || !parseOrder(parts[0], count) || count <= 0) return false;
			const auto tracks = splitFunctionAwareWords(parts[1]);
			if (tracks.empty()) return false;
			for (int i = 0; i < std::min(count, kMaxGridTracks); ++i)
				for (const auto &track : tracks)
					if (!appendCompiledGridTrackToken(track, grid)) return false;
		} else if (!appendCompiledGridTrackToken(token, grid)) return false;
	}
	return true;
}

bool splitGridTemplate(const std::string &value, std::string &rows, std::string &columns)
{
	if (toLowerAscii(trimCssValue(value)) == "none") { rows = columns = "none"; return true; }
	const auto parts = splitTopLevel(value, '/');
	if (parts.size() != 2) return false;
	rows = trimCssValue(parts[0]); columns = trimCssValue(parts[1]);
	CssCompiledGridTemplate r, c;
	return parseGridTemplateSpec(rows, r) && parseGridTemplateSpec(columns, c);
}

bool compileGridTemplateValue(const std::string &value, CssCompiledValue &compiled)
{
	CssCompiledGridTemplate grid;
	if (!parseGridTemplateSpec(value, grid)) return false;
	const std::uint16_t handle = storeCompiledCssGridTemplate(grid);
	if (handle == kNoCompiledCssGridTemplate) return false;
	compiled.values[0] = static_cast<std::int32_t>(handle);
	return true;
}

bool sameCompiledLengthSpec(const CssLengthSpec &a, const CssLengthSpec &b)
{
	return a.unit == b.unit && std::fabs(static_cast<double>(a.value) - static_cast<double>(b.value)) < 0.0001;
}

bool compileColorStopLengthSpec(const std::string &stop, CssLengthSpec &out)
{
	const std::string token = gradientColorTokenForStop(stop);
	const std::size_t tokenStart = stop.find(token);
	const std::string rest = tokenStart == std::string::npos
	    ? std::string()
	    : trimCssValue(stop.substr(tokenStart + token.size()));
	if (rest.empty()) return false;
	const auto parts = splitWords(rest);
	if (parts.empty()) return false;
	return parseCompiledLengthSpec(parts[0], out);
}

bool compiledGridLineLengthIsSafe(const CssLengthSpec &length)
{
	if (!std::isfinite(static_cast<double>(length.value)) || length.value < 1.0f) return false;
	return length.unit == CssLengthUnit::Raw || length.unit == CssLengthUnit::Px;
}

CssCompiledLinearGradient compileLinearGradientLayer(const ParsedLinearGradient &gradient)
{
	CssCompiledLinearGradient out;
	out.fromStyleColor = static_cast<std::int32_t>(cssColorStyleValue(gradient.from));
	out.fromNativeColor = cssColorNative(gradient.from);
	out.midNativeColor = gradient.hasMid ? cssColorNative(gradient.mid) : 0;
	out.toNativeColor = cssColorNative(gradient.to);
	out.fromAlpha = static_cast<std::uint8_t>(gradient.from.a);
	out.midAlpha = static_cast<std::uint8_t>(gradient.hasMid ? gradient.mid.a : 255);
	out.toAlpha = static_cast<std::uint8_t>(gradient.to.a);
	out.midStopPermille = static_cast<std::uint16_t>(gradient.midStopPermille);
	out.toStopPermille = static_cast<std::uint16_t>(gradient.toStopPermille);
	out.hasMid = gradient.hasMid ? 1 : 0;
	out.angleTenths = static_cast<std::int16_t>(gradient.angleTenths);
	return out;
}

bool compileLinearGradientLayerValue(const std::string &value, CssCompiledLinearGradient &out)
{
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return false;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return false;

	std::size_t colorStart = 0;
	int angleTenths = 1800;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) angleTenths = 900;
		else if (direction.find("left") != std::string::npos) angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) angleTenths = 0;
		else angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart >= parts.size()) return false;

	CompiledCssColorRef from;
	if (!compileGradientColorStopRef(parts[colorStart], from)) return false;
	setCompiledLinearGradientFrom(out, from);

	if (parts.size() == colorStart + 2) {
		const int fromStop = colorStopPermille(parts[colorStart], -1);
		if (fromStop > 0 && fromStop < 1000) {
			setCompiledLinearGradientMid(out, from);
			out.midStopPermille = static_cast<std::uint16_t>(fromStop);
			out.hasMid = 1;
		}
	} else if (parts.size() > colorStart + 2) {
		CompiledCssColorRef mid;
		if (!compileGradientColorStopRef(parts[colorStart + 1], mid)) return false;
		setCompiledLinearGradientMid(out, mid);
		out.midStopPermille = static_cast<std::uint16_t>(colorStopPermille(parts[colorStart + 1], 500));
		out.hasMid = 1;
	}

	CompiledCssColorRef to;
	if (!compileGradientColorStopRef(parts.back(), to)) return false;
	setCompiledLinearGradientTo(out, to);
	int toStopPermille = colorStopPermilleUnclamped(parts.back(), 1000);
	if (toStopPermille <= 0) toStopPermille = 1;
	if (out.hasMid && toStopPermille <= out.midStopPermille) toStopPermille = out.midStopPermille + 1;
	out.toStopPermille = static_cast<std::uint16_t>(toStopPermille);
	out.angleTenths = static_cast<std::int16_t>(angleTenths);
	return true;
}

CssCompiledRadialGradient compileRadialGradientLayer(const ParsedRadialGradient &gradient)
{
	CssCompiledRadialGradient out;
	out.fromNativeColor = cssColorNative(gradient.from);
	out.toNativeColor = cssColorNative(gradient.to);
	out.fromAlpha = static_cast<std::uint8_t>(gradient.from.a);
	out.toAlpha = static_cast<std::uint8_t>(gradient.to.a);
	out.stopPermille = static_cast<std::uint16_t>(gradient.stopPermille);
	out.cxPermille = static_cast<std::int16_t>(gradient.cxPermille);
	out.cyPermille = static_cast<std::int16_t>(gradient.cyPermille);
	out.rxPermille = static_cast<std::int16_t>(gradient.rxPermille);
	out.ryPermille = static_cast<std::int16_t>(gradient.ryPermille);
	return out;
}

bool compileGradientLineLayer(const std::string &value, CssCompiledBackground &background, bool &matchedLineLayer)
{
	matchedLineLayer = false;
	const std::string call = lastFunctionCall(value, "linear-gradient");
	if (call.empty()) return false;
	const std::string inner = functionInner(call, "linear-gradient");
	const auto parts = splitTopLevel(inner, ',');
	if (parts.size() < 2) return false;

	std::size_t colorStart = 0;
	int angleTenths = 1800;
	const std::string first = trimCssValue(parts[0]);
	if (first.find("deg") != std::string::npos || first.find("turn") != std::string::npos || first.find("rad") != std::string::npos) {
		angleTenths = numericRotateTenths(parseAngleDegrees(first));
		colorStart = 1;
	} else if (first.rfind("to ", 0) == 0) {
		const std::string direction = toLowerAscii(first);
		if (direction.find("right") != std::string::npos) angleTenths = 900;
		else if (direction.find("left") != std::string::npos) angleTenths = 2700;
		else if (direction.find("top") != std::string::npos) angleTenths = 0;
		else angleTenths = 1800;
		colorStart = 1;
	}
	if (colorStart + 1 >= parts.size()) return false;

	const ParsedCssColor firstColor = parseGradientColorStop(parts[colorStart]);
	const ParsedCssColor secondColor = parseGradientColorStop(parts[colorStart + 1]);
	CssLengthSpec firstLength;
	const bool hasFirstLength = compileColorStopLengthSpec(parts[colorStart], firstLength);
	if (hasFirstLength && secondColor.valid && secondColor.a == 0) matchedLineLayer = true;
	if (!firstColor.valid || !secondColor.valid || firstColor.a == 0 || secondColor.a != 0 ||
	    !hasFirstLength)
		return false;

	matchedLineLayer = true;
	if (!compiledGridLineLengthIsSafe(firstLength)) return false;
	CssLengthSpec secondLength;
	if (compileColorStopLengthSpec(parts[colorStart + 1], secondLength)) {
		if (!compiledGridLineLengthIsSafe(secondLength)) return false;
		if (!sameCompiledLengthSpec(firstLength, secondLength)) return false;
	}

	const bool vertical = gradientLineIsVertical(angleTenths);
	background.gridAxes |= vertical ? 1 : 2;
	background.gridColor = cssColorNative(firstColor);
	background.gridAlpha = static_cast<std::uint8_t>(firstColor.a);
	if (vertical) {
		background.gridLineX = firstLength;
		background.hasGridLineX = 1;
	} else {
		background.gridLineY = firstLength;
		background.hasGridLineY = 1;
	}
	return true;
}

std::uint16_t storeCompiledCssBackground(const CssCompiledBackground &background)
{
	auto &list = compiledCssBackgrounds();
	if (list.size() >= kNoCompiledCssBackground) return kNoCompiledCssBackground;
	list.push_back(background);
	return static_cast<std::uint16_t>(list.size() - 1);
}

int storeBackgroundClipList(const std::vector<std::uint8_t> &clips)
{
	if (clips.empty()) return -1;
	if (std::all_of(clips.begin(), clips.end(), [](auto clip) { return clip == 0; })) return 0;
	auto &lists = backgroundClipLists();
	for (std::size_t i = 0; i < lists.size(); ++i) if (lists[i] == clips) return static_cast<int>(i + 1);
	if (lists.size() >= 65535) return -1;
	lists.push_back(clips);
	return static_cast<int>(lists.size());
}

int backgroundBoxValue(const std::string &word)
{
	return word == "border-box" ? 0 : word == "padding-box" ? 1 : word == "content-box" ? 2 : -1;
}

int compileBackgroundClip(const std::string &value, bool shorthand = false)
{
	const auto lower = toLowerAscii(trimCssValue(value));
	if (lower == "initial" || lower == "unset") return 0;
	std::vector<std::uint8_t> clips;
	for (const auto &layer : splitTopLevel(lower, ',')) {
		if (!shorthand) {
			const auto word = trimCssValue(layer);
			const int clip = word == "text" ? 3 : backgroundBoxValue(word);
			if (clip < 0) return -1;
			clips.push_back(clip);
		} else {
			int clip = 0, boxes = 0;
			bool text = false;
			for (const auto &word : splitWords(layer)) {
				if (word == "text") { if (text) return -1; text = true; continue; }
				const int box = backgroundBoxValue(word);
				if (box >= 0) { clip = box; if (++boxes > 2) return -1; }
			}
			if (text) { if (boxes > 1) return -1; clip = 3; }
			clips.push_back(clip);
		}
	}
	return storeBackgroundClipList(clips);
}

ParsedCssColor backgroundBaseColor(const std::string &value)
{
	ParsedCssColor color;
	for (const auto &layer : splitTopLevel(value, ','))
		for (const auto &word : splitWords(layer)) {
			const ParsedCssColor candidate = parseCssColor(word);
			if (candidate.valid) color = candidate;
		}
	return color;
}

const CssCompiledBackground *compiledCssBackgroundForHandle(std::uint16_t handle);
bool backgroundImageIsValid(int handle, int nodeId);

int marginTrimValue(const std::string &raw)
{
	const auto value = toLowerAscii(trimCssValue(raw));
	if (value == "none" || value == "initial" || value == "unset") return 0;
	int flags = 0, grammar = 0;
	for (const auto &word : splitWords(value)) {
		const int bits = word == "block" ? 3 : word == "inline" ? 12 :
		    word == "block-start" ? 1 : word == "block-end" ? 2 :
		    word == "inline-start" ? 4 : word == "inline-end" ? 8 : 0;
		const int group = word == "block" || word == "inline" ? 1 : 2;
		if (!bits || (flags & bits) || (grammar && grammar != group)) return -1;
		flags |= bits; grammar = group;
	}
	return flags ? flags : -1;
}

Property backgroundPlacementProperty(CssDeclarationId declaration)
{
	switch (declaration) {
	case CssDeclarationId::BackgroundSize: return Property::BackgroundSizeList;
	case CssDeclarationId::BackgroundPosition: return Property::BackgroundPositionList;
	case CssDeclarationId::BackgroundRepeat: return Property::BackgroundRepeatList;
	case CssDeclarationId::BackgroundAttachment: return Property::BackgroundAttachmentList;
	case CssDeclarationId::BackgroundOrigin: return Property::BackgroundOriginList;
	default: return Property::Count;
	}
}

int backgroundPlacementHandle(const ComputedStyle &style, Property property)
{
	const auto &r = rstyle(style);
	switch (property) {
	case Property::BackgroundSizeList: return r.bg_size_list;
	case Property::BackgroundPositionList: return r.bg_position_list;
	case Property::BackgroundRepeatList: return r.bg_repeat_list;
	case Property::BackgroundAttachmentList: return r.bg_attachment_list;
	case Property::BackgroundOriginList: return r.bg_origin_list;
	default: return -1;
	}
}

int compileBackgroundPlacement(CssDeclarationId declaration, const std::string &raw)
{
	const auto value = toLowerAscii(trimCssValue(raw));
	if (value == "initial" || value == "unset") return -1;
	std::vector<CssBackgroundPair> list;
	for (const auto &layer : splitTopLevel(value, ',')) {
		auto parts = splitWords(layer);
		if (parts.empty()) return -2;
		CssBackgroundPair pair;
		auto length = [&](const std::string &word, CssLengthSpec &out, bool allowAuto) {
			// Percent expressions need an explicit image-area basis, not the
			// containing-block basis used by the general expression evaluator.
			if (word.find('(') != std::string::npos && word.find('%') != std::string::npos) return false;
			return parseCompiledLengthSpec(word, out, allowAuto);
		};
		if (declaration == CssDeclarationId::BackgroundSize) {
			if (parts.size() == 1 && (parts[0] == "cover" || parts[0] == "contain"))
				pair.x.unit = pair.y.unit = CssLengthUnit::Auto; // Gradients have no intrinsic ratio.
			else {
				if (parts.size() > 2 || !length(parts[0], pair.x, true)) return -2;
				if (parts.size() == 1) pair.y.unit = CssLengthUnit::Auto;
				else if (!length(parts[1], pair.y, true)) return -2;
				if ((pair.x.unit != CssLengthUnit::Auto && pair.x.value < 0) || (pair.y.unit != CssLengthUnit::Auto && pair.y.value < 0)) return -2;
			}
		} else if (declaration == CssDeclarationId::BackgroundPosition) {
			if (parts.size() > 2) return -2;
			if (parts.size() == 1) {
				if (parts[0] == "top" || parts[0] == "bottom") parts.insert(parts.begin(), "center");
				else parts.push_back("center");
			}
			if (parts[0] == "top" || parts[0] == "bottom" || parts[1] == "left" || parts[1] == "right") std::swap(parts[0], parts[1]);
			for (int axis = 0; axis < 2; ++axis) {
				auto &spec = axis ? pair.y : pair.x;
				const auto &word = parts[axis];
				if (word == "center" || word == (axis ? "top" : "left") || word == (axis ? "bottom" : "right")) {
					spec.unit = CssLengthUnit::Percent;
					spec.value = word == "center" ? 50 : word == (axis ? "top" : "left") ? 0 : 100;
				} else if (!length(word, spec, false)) return -2;
			}
		} else if (declaration == CssDeclarationId::BackgroundRepeat) {
			if (parts.size() == 1 && (parts[0] == "repeat-x" || parts[0] == "repeat-y")) {
				pair.a = parts[0] == "repeat-y"; pair.b = parts[0] == "repeat-x";
			} else {
				if (parts.size() > 2) return -2;
				auto keyword = [](const std::string &s) { return s == "repeat" ? 0 : s == "no-repeat" ? 1 : s == "round" ? 2 : s == "space" ? 3 : -1; };
				pair.a = keyword(parts[0]); pair.b = keyword(parts.size() == 1 ? parts[0] : parts[1]);
				if (pair.a < 0 || pair.b < 0) return -2;
			}
		} else if (declaration == CssDeclarationId::BackgroundAttachment) {
			if (parts.size() != 1) return -2;
			pair.a = parts[0] == "scroll" ? 0 : parts[0] == "fixed" ? 1 : parts[0] == "local" ? 2 : -1;
			if (pair.a < 0) return -2;
		} else if (declaration == CssDeclarationId::BackgroundOrigin) {
			if (parts.size() != 1 || (pair.a = backgroundBoxValue(parts[0])) < 0) return -2;
		} else return -2;
		list.push_back(pair);
	}
	return list.empty() ? -2 : storeBackgroundPlacementList(list);
}

void applyBackgroundHandle(NodeHandle node, int handle, bool shorthand, StyleApplicationSource source)
{
	if (!node || !backgroundImageIsValid(handle, node.id())) return;
	if (shorthand) {
		const auto *background = handle >= 0 ? compiledCssBackgroundForHandle(handle) : nullptr;
		setStyleValue(node, Property::BackgroundColor, background ? background->colorStyle : 0, source);
		setStyleValue(node, Property::BackgroundAlpha, background ? background->colorAlpha : 0, source);
		setStyleValue(node, Property::BackgroundClip, background ? background->clip : 0, source);
		for (Property p : {Property::BackgroundSizeList, Property::BackgroundPositionList, Property::BackgroundRepeatList,
		                   Property::BackgroundAttachmentList, Property::BackgroundOriginList}) setStyleValue(node, p, -1, source);
	}
	setStyleValue(node, Property::BackgroundImage, handle, source);
	setStyleValue(node, Property::HasBackground, 1, source);
}

bool compileBackgroundValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string lower = toLowerAscii(value);
	if (lower.find("calc(") != std::string::npos ||
	    lower.find("min(") != std::string::npos ||
	    lower.find("max(") != std::string::npos ||
	    lower.find("clamp(") != std::string::npos)
		return false;

	CssCompiledBackground background;
	const auto color = backgroundBaseColor(value);
	if (compiled.declaration == CssDeclarationId::BackgroundImage && color.valid) return false;
	if (color.valid) { background.colorStyle = cssColorStyleValue(color); background.colorAlpha = color.a; }
	const auto layers = splitTopLevel(value, ',');
	if (layers.empty() || layers.size() > 65535) return false;
	background.layerCount = layers.size();
	if (compiled.declaration == CssDeclarationId::Background) {
		const int clip = compileBackgroundClip(value, true);
		if (clip < 0) return false;
		background.clip = clip;
	}
	for (std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
		const auto &layerValue = layers[layerIndex];
		bool matchedLineLayer = false;
		if (compileGradientLineLayer(layerValue, background, matchedLineLayer)) continue;
		if (matchedLineLayer) return false;

		CssCompiledLinearGradient layerGradient;
		if (compileLinearGradientLayerValue(layerValue, layerGradient)) {
			if (background.hasGradient) {
				background.overlayLayer = background.gradientLayer;
				background.overlayGradient = background.gradient;
				background.hasOverlayGradient = 1;
			}
			background.gradientLayer = layerIndex;
			background.gradient = layerGradient;
			background.hasGradient = 1;
			continue;
		}
		const ParsedRadialGradient layerRadial = parseRadialGradient(layerValue);
		if (layerRadial.valid) {
			background.radialLayer = layerIndex;
			background.radialGradient = compileRadialGradientLayer(layerRadial);
			background.hasRadialGradient = 1;
			continue;
		}
		if (toLowerAscii(layerValue).find("var(") != std::string::npos) return false;
	}
	if (!background.hasGradient && !background.hasRadialGradient && !background.gridAxes &&
	    !color.valid && !(compiled.declaration == CssDeclarationId::Background &&
	        std::all_of(layers.begin(), layers.end(), [](const auto &layer) {
		        const auto words = splitWords(layer);
		        return !words.empty() && std::all_of(words.begin(), words.end(), [](const auto &word) {
			        return word == "none" || backgroundBoxValue(toLowerAscii(word)) >= 0;
		        });
	        })) && lower != "none" && lower != "initial" && lower != "unset" &&
	    !std::all_of(layers.begin(), layers.end(), [](const std::string &layer) { return toLowerAscii(trimCssValue(layer)) == "none"; })) return false;

	const std::uint16_t handle = storeCompiledCssBackground(background);
	if (handle == kNoCompiledCssBackground) return false;
	compiled.values[0] = handle;
	return true;
}

bool compileBackgroundSizeValue(const std::string &value, CssCompiledValue &compiled)
{
	const std::string lower = toLowerAscii(value);
	if (hasDynamicCssValue(lower)) return false;
	const auto layers = splitTopLevel(value, ',');
	if (layers.empty()) return false;
	const auto parts = splitWords(layers[0]);
	if (parts.empty()) return false;
	if (!parseCompiledLengthSpec(parts[0], compiled.lengths[0])) return false;
	if (parts.size() > 1) {
		if (!parseCompiledLengthSpec(parts[1], compiled.lengths[1])) return false;
	} else {
		compiled.lengths[1] = compiled.lengths[0];
	}
	return true;
}

bool compileKeywordValue(CssDeclarationId declaration, const std::string &value, CssCompiledValue &compiled)
{
	switch (declaration) {
	case CssDeclarationId::Display:
		compiled.values[0] = displayValue(value);
		return true;
	case CssDeclarationId::ObjectFit:
		compiled.values[0] = imageFitValue(value);
		return true;
	case CssDeclarationId::FlexDirection:
		compiled.values[0] = flexDirectionValue(value);
		return true;
	case CssDeclarationId::FlexWrap:
		compiled.values[0] = flexWrapValue(value);
		return compiled.values[0] >= 0;
	case CssDeclarationId::AlignItems:
	case CssDeclarationId::JustifyItems:
		compiled.values[0] = selfAlignValue(value, declaration == CssDeclarationId::JustifyItems);
		return compiled.values[0] >= 0;
	case CssDeclarationId::JustifyContent:
	case CssDeclarationId::AlignContent:
		compiled.values[0] = flexAlignValue(value);
		return compiled.values[0] >= 0;
	case CssDeclarationId::GridRowStart:
	case CssDeclarationId::GridColumnStart:
	case CssDeclarationId::GridRowEnd:
	case CssDeclarationId::GridColumnEnd: {
		// int32_t is `long` on arm-none-eabi, so the slot cannot bind to int&.
		int line = 0;
		if (!parseGridLine(value, line)) return false;
		compiled.values[0] = line;
		return true;
	}
	case CssDeclarationId::JustifySelf:
		compiled.values[0] = justifySelfValue(value);
		return compiled.values[0] >= -1;
	case CssDeclarationId::AlignSelf:
		compiled.values[0] = alignSelfValue(value);
		return compiled.values[0] >= -1;
	case CssDeclarationId::Position:
		compiled.values[0] = positionValue(value);
		return true;
	case CssDeclarationId::TextAlign:
		compiled.values[0] = textAlignValue(value);
		return true;
	case CssDeclarationId::TextDecoration:
		compiled.values[0] = textDecorationValue(value);
		return true;
	case CssDeclarationId::TextTransform:
		compiled.values[0] = textTransformValue(value);
		return true;
	case CssDeclarationId::WhiteSpace:
		compiled.values[0] = whiteSpaceValue(value);
		return true;
	case CssDeclarationId::TextOverflow:
		compiled.values[0] = textOverflowValue(value);
		return true;
	case CssDeclarationId::TransformStyle: {
		const auto lower = toLowerAscii(trimCssValue(value));
		if (lower != "flat" && lower != "preserve-3d") return false;
		compiled.values[0] = lower == "preserve-3d";
		return true;
	}
	case CssDeclarationId::Visibility:
		compiled.values[0] = visibilityValue(value);
		return compiled.values[0] >= 0;
	case CssDeclarationId::BackfaceVisibility:
		compiled.values[0] = backfaceValue(value);
		return true;
	case CssDeclarationId::PointerEvents:
		compiled.values[0] = pointerEventsValue(value);
		return true;
	case CssDeclarationId::Overflow:
	case CssDeclarationId::OverflowX:
	case CssDeclarationId::OverflowY:
		compiled.values[0] = overflowValue(value);
		return compiled.values[0] >= 0;
	case CssDeclarationId::FontFamily: {
		compiled.values[0] = fontFamilyValue(value);
		return true;
	}
	case CssDeclarationId::FontWeight:
		compiled.values[0] = fontWeightValue(value);
		return true;
	default:
		return false;
	}
}

bool compileTransformLength(const std::string &value, CssLengthSpec &out)
{
	return parseCompiledLengthSpec(value, out);
}

bool compileTransformValue(const std::string &value, CssCompiledValue &compiled)
{
	TransformRotationProduct rotations;
	compiled.values[8] = compiled.values[9] = compiled.values[10] = 1000;
	std::size_t i = 0;
	bool sawTransform = false;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t nameStart = i;
		while (i < value.size() && (std::isalpha(static_cast<unsigned char>(value[i])) || value[i] == '3')) ++i;
		if (i == nameStart) {
			while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
			if (i >= value.size()) break;
			return false;
		}
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		if (i >= value.size() || value[i] != '(') return false;
		const std::string name = toLowerAscii(value.substr(nameStart, i - nameStart));
		const std::size_t argStart = ++i;
		int depth = 1;
		while (i < value.size() && depth > 0) {
			if (value[i] == '(') depth++;
			else if (value[i] == ')') depth--;
			++i;
		}
		if (depth != 0) return false;
		const std::string arg = value.substr(argStart, i - argStart - 1);
		const auto args = splitTopLevel(arg, ',');
		const int axes = name == "translatex" ? 1 : name == "translatey" ? 2 : name == "translatez" ? 4 :
		    (name == "translate" || name == "translate3d") ? ((1 << std::min<std::size_t>(3, args.size())) - 1) : 0;
		compiled.flags = (compiled.flags & ~(axes << 10)) | ((compiled.flags & 7) ? 0 : (axes << 10));
		sawTransform = true;

		if (name == "rotate" || name == "rotatez" || name == "rotatex" || name == "rotatey" || name == "rotate3d") {
			if (!rotations.append(name, arg)) return false;
			int x=0, y=0, z=0;
			rotations.angles(x, y, z);
			compiled.values[0] = x; compiled.values[1] = y; compiled.values[2] = z;
			compiled.flags |= 7;
		} else if (name == "translatex") {
			if (!compileTransformLength(arg, compiled.lengths[0])) return false;
			compiled.flags |= 1u << 3;
		} else if (name == "translatey") {
			if (!compileTransformLength(arg, compiled.lengths[1])) return false;
			compiled.flags |= 1u << 4;
		} else if (name == "translatez") {
			if (!compileTransformLength(arg, compiled.lengths[2])) return false;
			compiled.flags |= 1u << 5;
		} else if (name == "translate" || name == "translate3d") {
			if (args.empty()) return false;
			if (!compileTransformLength(args[0], compiled.lengths[0])) return false;
			compiled.flags |= 1u << 3;
			if (args.size() > 1) {
				if (!compileTransformLength(args[1], compiled.lengths[1])) return false;
				compiled.flags |= 1u << 4;
			}
			if (args.size() > 2) {
				if (!compileTransformLength(args[2], compiled.lengths[2])) return false;
				compiled.flags |= 1u << 5;
			}
		} else if (name == "scale") {
			if (hasDynamicCssValue(arg)) return false;
			const int sx = parseScalePermille(args.empty() ? arg : args[0]);
			const int sy = parseScalePermille(args.size() > 1 ? args[1] : (args.empty() ? arg : args[0]));
			compiled.values[8] = multiplyScalePermille(compiled.values[8], sx);
			compiled.values[9] = multiplyScalePermille(compiled.values[9], sy);
			compiled.flags |= (1u << 8) | (1u << 9);
		} else if (name == "scale3d") {
			if (hasDynamicCssValue(arg) || args.size() != 3) return false;
			compiled.values[8] = multiplyScalePermille(compiled.values[8], parseScalePermille(args[0]));
			compiled.values[9] = multiplyScalePermille(compiled.values[9], parseScalePermille(args[1]));
			compiled.values[10] = multiplyScalePermille(compiled.values[10], parseScalePermille(args[2]));
			compiled.flags |= (1u << 8) | (1u << 9) | (1u << 13);
		} else if (name == "scalex") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[8] = multiplyScalePermille(compiled.values[8], parseScalePermille(arg));
			compiled.flags |= 1u << 8;
		} else if (name == "scaley") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[9] = multiplyScalePermille(compiled.values[9], parseScalePermille(arg));
			compiled.flags |= 1u << 9;
		} else if (name == "scalez") {
			if (hasDynamicCssValue(arg)) return false;
			compiled.values[10] = multiplyScalePermille(compiled.values[10], parseScalePermille(arg));
			compiled.flags |= 1u << 13;
		} else {
			return false;
		}
	}
	return sawTransform;
}

bool individualTranslateFunction(const std::string &raw, std::string &function)
{
	const std::string value = toLowerAscii(trimCssValue(raw));
	if (value == "none") { function = "none"; return true; }
	const auto parts = splitWords(value);
	if (parts.empty() || parts.size() > 3) return false;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		CssLengthSpec length;
		if (!parseCompiledLengthSpec(parts[i], length) || length.unit == CssLengthUnit::Auto ||
		    (length.unit == CssLengthUnit::Raw && length.value != 0) ||
		    (i == 2 && length.unit == CssLengthUnit::Percent)) return false;
		// Expression evaluation currently uses the containing block as its percent
		// basis. Do not accept an expression that would silently translate against
		// that wrong box; bare percentages retain their own-box percentage slots.
		if (length.unit == CssLengthUnit::Expression &&
		    lengthDependsOnInput(length, -1, CssLengthUnit::Percent)) return false;
	}
	function = "translate3d(" + parts[0] + "," + (parts.size() > 1 ? parts[1] : "0") + "," + (parts.size() > 2 ? parts[2] : "0") + ")";
	return true;
}

std::uint16_t storeCompiledCssValue(const CssCompiledValue &compiled)
{
	if (compiled.kind == CssCompiledKind::None) return kNoCompiledCssValue;
	auto &list = compiledCssValues();
	if (list.size() >= kNoCompiledCssValue) return kNoCompiledCssValue;
	list.push_back(compiled);
	return static_cast<std::uint16_t>(list.size() - 1);
}

std::uint16_t compileCustomPropertyValue(const CssText &rawValue)
{
	if (rawValue.empty()) return kNoCompiledCssValue;
	const std::string value = rawValue.trimmedStr();
	if (value.empty()) return kNoCompiledCssValue;

	CssCompiledValue compiled;
	compiled.declaration = CssDeclarationId::Custom;

	const CachedCssColor color = cachedCssColorForValue(value);
	if (color.valid) {
		compiled.kind = CssCompiledKind::Color;
		compiled.values[0] = color.styleColor;
		compiled.values[1] = color.nativeColor;
		compiled.values[2] = color.alpha;
		return storeCompiledCssValue(compiled);
	}

	CssLengthSpec length;
	if (parseCompiledLengthSpec(value, length)) {
		compiled.kind = CssCompiledKind::Length;
		compiled.lengths[0] = length;
		return storeCompiledCssValue(compiled);
	}

	return kNoCompiledCssValue;
}

std::uint16_t compileCssValue(CssDeclarationId declaration, const CssText &rawValue)
{
	if (declaration == CssDeclarationId::Unknown || declaration == CssDeclarationId::Custom || rawValue.empty())
		return kNoCompiledCssValue;
	if (static_cast<std::uint32_t>(GEA_COMPILED_CSS_VALUE_MASK) == 0u) return kNoCompiledCssValue;

	const std::string value = rawValue.trimmedStr();
	if (value.empty()) return kNoCompiledCssValue;
	const bool hasVar = rawValue.hasVarReference();

	CssCompiledValue compiled;
	compiled.declaration = declaration;

	if (backgroundPlacementProperty(declaration) != Property::Count && !hasVar) {
		const int handle = compileBackgroundPlacement(declaration, value);
		if (handle < -1) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::DirectProperty;
		compiled.values[0] = static_cast<int>(backgroundPlacementProperty(declaration));
		compiled.values[1] = handle;
		return storeCompiledCssValue(compiled);
	}

	if (declaration == CssDeclarationId::Ignored ||
	    declaration == CssDeclarationId::Content ||
	    declaration == CssDeclarationId::Animation) {
		compiled.kind = CssCompiledKind::Noop;
		return storeCompiledCssValue(compiled);
	}

	if ((declaration == CssDeclarationId::Margin || declaration == CssDeclarationId::MarginTop ||
	     declaration == CssDeclarationId::MarginRight || declaration == CssDeclarationId::MarginBottom ||
	     declaration == CssDeclarationId::MarginLeft) && value.find("auto") != std::string::npos)
		return kNoCompiledCssValue;

	if (declaration == CssDeclarationId::BackgroundClip && !hasVar) {
		const int clip = compileBackgroundClip(value);
		if (clip < 0) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::DirectProperty;
		compiled.values[0] = static_cast<int>(Property::BackgroundClip);
		compiled.values[1] = clip;
		return storeCompiledCssValue(compiled);
	}

	if (declaration == CssDeclarationId::MarginTrim && !hasVar) {
		const int flags = marginTrimValue(value);
		if (flags < 0) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::DirectProperty;
		compiled.values[0] = static_cast<int>(Property::MarginTrim);
		compiled.values[1] = flags;
		return storeCompiledCssValue(compiled);
	}

	if (declaration == CssDeclarationId::Contain && !hasVar) {
		const int flags = containmentValue(value);
		if (flags < 0) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::DirectProperty;
		compiled.values[0] = static_cast<int>(Property::Containment);
		compiled.values[1] = flags;
		return storeCompiledCssValue(compiled);
	}

	if ((declaration == CssDeclarationId::Order || declaration == CssDeclarationId::FlexLineCount) && !hasVar) {
		int order = 0;
		if (!parseOrder(value, order) || (declaration == CssDeclarationId::FlexLineCount && order < 1)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::DirectProperty;
		compiled.values[0] = static_cast<int>(declaration == CssDeclarationId::Order ? Property::Order : Property::FlexLineCount);
		compiled.values[1] = order;
		return storeCompiledCssValue(compiled);
	}

	Property alignProperty, justifyProperty;
	if (!hasVar && alignmentShorthandProperties(declaration, alignProperty, justifyProperty)) {
		int align, justify;
		if (!parseAlignmentShorthand(declaration, value, align, justify)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::DirectPropertyGroup;
		compiled.values[0] = 2;
		compiled.values[1] = static_cast<int>(alignProperty); compiled.values[2] = align;
		compiled.values[3] = static_cast<int>(justifyProperty); compiled.values[4] = justify;
		return storeCompiledCssValue(compiled);
	}

	if (!hasVar && compiledCssValueFeatureEnabled(kCssCompiledFeatureKeyword) && compileKeywordValue(declaration, value, compiled)) {
		compiled.kind = CssCompiledKind::Keyword;
		return storeCompiledCssValue(compiled);
	}

	switch (declaration) {
	case CssDeclarationId::Opacity:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureOpacity)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Opacity;
		compiled.values[0] = parseOpacity(value);
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::ZIndex:
		if (hasVar || !compiledCssValueFeatureEnabled(kCssCompiledFeatureNumber)) return kNoCompiledCssValue;
	{
		// int32_t is `long` on arm-none-eabi, so the slot cannot bind to int&.
		int zIndex = 0;
		if (!parseZIndex(value, zIndex)) return kNoCompiledCssValue;
		compiled.values[0] = zIndex;
	}
		compiled.kind = CssCompiledKind::Number;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::FlexGrow:
	case CssDeclarationId::FlexShrink:
	case CssDeclarationId::FontWeight:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureNumber)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Number;
		compiled.values[0] = declaration == CssDeclarationId::FontWeight
		                         ? fontWeightValue(value)
		                         : rawNumber(std::strtod(value.c_str(), nullptr));
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Flex:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureNumber) ||
		    !compiledCssValueFeatureEnabled(kCssCompiledFeatureLength))
			return kNoCompiledCssValue;
		if (!compileFlexShorthandValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Flex;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::FlexBasis:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileFlexBasisValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::FlexBasis;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Color:
	case CssDeclarationId::BackgroundColor:
	case CssDeclarationId::ActiveBackgroundColor:
	case CssDeclarationId::BorderColor:
	case CssDeclarationId::BorderTopColor:
	case CssDeclarationId::BorderRightColor:
	case CssDeclarationId::BorderBottomColor:
	case CssDeclarationId::BorderLeftColor:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureColor)) return kNoCompiledCssValue;
		if (hasVar) {
			if (!compileColorVarValue(value, compiled)) return kNoCompiledCssValue;
			compiled.kind = CssCompiledKind::ColorVar;
		} else {
			if (!compileSimpleColor(value, compiled)) return kNoCompiledCssValue;
			compiled.kind = CssCompiledKind::Color;
		}
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Background:
		// The color-only shortcut resets every other background longhand. It
		// applies only to a complete color token, never a shorthand whose next
		// token supplies a clip/origin/repeat value.
		if (compiledCssValueFeatureEnabled(kCssCompiledFeatureColor) && splitWords(value).size() == 1) {
			if (hasVar) {
				if (compileColorVarValue(value, compiled)) {
					compiled.kind = CssCompiledKind::ColorVar;
					return storeCompiledCssValue(compiled);
				}
			} else if (compileSimpleColor(value, compiled)) {
				compiled.kind = CssCompiledKind::Color;
				return storeCompiledCssValue(compiled);
			}
		}
		[[fallthrough]];
	case CssDeclarationId::BackgroundImage:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureBackground)) return kNoCompiledCssValue;
		if (hasVar && !compiledCssValueFeatureEnabled(kCssCompiledFeatureColor)) return kNoCompiledCssValue;
		if (!compileBackgroundValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Background;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BackgroundSize:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureBackground)) return kNoCompiledCssValue;
		if (!compileBackgroundSizeValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BackgroundSize;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::MaskImage:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileRightFadeMaskWidthValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Length;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Border:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderShorthandValue(value, compiled)) return kNoCompiledCssValue;
		if (compiled.aux != 0 && !compiledCssValueFeatureEnabled(kCssCompiledFeatureColor))
			return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderShorthand;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderTop:
	case CssDeclarationId::BorderRight:
	case CssDeclarationId::BorderBottom:
	case CssDeclarationId::BorderLeft:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderShorthandValue(value, compiled)) return kNoCompiledCssValue;
		if (compiled.aux != 0 && !compiledCssValueFeatureEnabled(kCssCompiledFeatureColor))
			return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderSideShorthand;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderRadius:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderRadiusValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderRadius;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderTopLeftRadius:
	case CssDeclarationId::BorderTopRightRadius:
	case CssDeclarationId::BorderBottomRightRadius:
	case CssDeclarationId::BorderBottomLeftRadius:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0])) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BorderRadius;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Width:
	case CssDeclarationId::Height:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureSize)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0], true)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Size;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Top:
	case CssDeclarationId::Right:
	case CssDeclarationId::Bottom:
	case CssDeclarationId::Left:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeaturePosition)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0])) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::PositionOffset;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::Padding:
	case CssDeclarationId::Margin:
	case CssDeclarationId::Inset:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureBox)) return kNoCompiledCssValue;
		if (!compileBoxLengthSpecs(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Box;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BorderWidth:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileBorderWidthBox(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Box;
		return storeCompiledCssValue(compiled);

	case CssDeclarationId::MaxWidth:
	case CssDeclarationId::MaxHeight:
		if (value == "none") {
			compiled.kind = CssCompiledKind::Length;
			compiled.lengths[0].unit = CssLengthUnit::Raw;
			compiled.lengths[0].value = kUnset;
			return storeCompiledCssValue(compiled);
		}
		[[fallthrough]];
	case CssDeclarationId::MinWidth:
	case CssDeclarationId::MinHeight:
	case CssDeclarationId::PaddingTop:
	case CssDeclarationId::PaddingRight:
	case CssDeclarationId::PaddingBottom:
	case CssDeclarationId::PaddingLeft:
	case CssDeclarationId::MarginTop:
	case CssDeclarationId::MarginRight:
	case CssDeclarationId::MarginBottom:
	case CssDeclarationId::MarginLeft:
	case CssDeclarationId::BorderTopWidth:
	case CssDeclarationId::BorderRightWidth:
	case CssDeclarationId::BorderBottomWidth:
	case CssDeclarationId::BorderLeftWidth:
	case CssDeclarationId::FontSize:
	case CssDeclarationId::Perspective:
		if (isBorderWidthDeclaration(declaration) && isNegativeLengthLiteral(value)) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!parseCompiledLengthSpec(value, compiled.lengths[0])) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Length;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::LineHeight:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureLength)) return kNoCompiledCssValue;
		if (!compileLineHeightValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::LineHeight;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::TransformOrigin:
	case CssDeclarationId::PerspectiveOrigin: {
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureOrigin)) return kNoCompiledCssValue;
		const auto parts = splitWords(value);
		compiled.kind = CssCompiledKind::OriginPair;
		compiled.values[0] = parseOriginPart(parts.empty() ? "" : parts[0], 500);
		compiled.values[1] = parseOriginPart(parts.size() < 2 ? "" : parts[1], 500);
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Rotate: {
		if (hasVar || !compiledCssValueFeatureEnabled(kCssCompiledFeatureTransformScalar)) return kNoCompiledCssValue;
		IndividualRotation rotation;
		if (!parseIndividualRotation(value, rotation)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Rotate;
		compiled.aux = toLowerAscii(trimCssValue(value)) != "none";
		compiled.values[0] = rotation.angle;
		compiled.values[1] = rotation.x; compiled.values[2] = rotation.y; compiled.values[3] = rotation.z;
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Scale: {
		if (hasVar || !compiledCssValueFeatureEnabled(kCssCompiledFeatureTransformScalar)) return kNoCompiledCssValue;
		int scale[3]; if (!parseIndividualScale(value, scale)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Scale;
		compiled.aux = toLowerAscii(trimCssValue(value)) != "none";
		for (int i = 0; i < 3; ++i) compiled.values[i] = scale[i];
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Translate: {
		if (hasVar || !compiledCssValueFeatureEnabled(kCssCompiledFeatureTransform)) return kNoCompiledCssValue;
		std::string function;
		if (!individualTranslateFunction(value, function)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Transform;
		compiled.values[8] = compiled.values[9] = 1000;
		if (function != "none" && !compileTransformValue(function, compiled)) return kNoCompiledCssValue;
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Transform: {
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureTransform)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::Transform;
		compiled.values[8] = 1000;
		compiled.values[9] = 1000;
		if (!compileTransformValue(value, compiled)) return kNoCompiledCssValue;
		return storeCompiledCssValue(compiled);
	}
	case CssDeclarationId::Filter:
		if (hasVar && toLowerAscii(value).find("blur(") == std::string::npos) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureEffects)) return kNoCompiledCssValue;
		if (!compileFilterBlurValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::FilterBlur;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::BoxShadow:
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureEffects)) return kNoCompiledCssValue;
		if (hasDynamicCssValue(value)) {
			if (boxShadowValueHasInsetLayer(value)) return kNoCompiledCssValue;
			compiled.kind = CssCompiledKind::BoxShadow;
			compiled.aux = 0;
			return storeCompiledCssValue(compiled);
		}
		if (!compileBoxShadowValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::BoxShadow;
		return storeCompiledCssValue(compiled);
	case CssDeclarationId::GridTemplateColumns:
	case CssDeclarationId::GridTemplateRows:
		if (hasVar) return kNoCompiledCssValue;
		if (!compiledCssValueFeatureEnabled(kCssCompiledFeatureGridTemplate)) return kNoCompiledCssValue;
		if (!compileGridTemplateValue(value, compiled)) return kNoCompiledCssValue;
		compiled.kind = CssCompiledKind::GridTemplate;
		return storeCompiledCssValue(compiled);
	default:
		return kNoCompiledCssValue;
	}
}

bool styleApplyInvalidationSuppressed()
{
	return treeState().styleInvalidationSuppressionDepth > 0;
}

void markNodeDisplayCommandsDirtyForStyleApply(int nodeId)
{
	if (styleApplyInvalidationSuppressed()) return;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
}

void markDisplayListDirtyForStyleApply()
{
	if (styleApplyInvalidationSuppressed()) return;
	Tree::instance().markDisplayListDirty();
}

void applyCompiledGridTemplateValue(NodeHandle node, const CssCompiledGridTemplate &grid, bool columns)
{
	if (!node) return;
	const int nodeId = node.id();
	Node &target = treeState().nodes[nodeId];
	const LengthAxis axis = columns ? LengthAxis::Horizontal : LengthAxis::Vertical;
	RareStyle &rs = rstyleMut(target.style);
	int8_t *types = columns ? rs.grid_column_type : rs.grid_row_type;
	int16_t *values = columns ? rs.grid_column_value : rs.grid_row_value;
	int8_t &count = columns ? rs.grid_column_count : rs.grid_row_count;
	for (int i = 0; i < kMaxGridTracks; ++i) {
		types[i] = 0;
		values[i] = 0;
	}
	const int trackCount = std::min<int>(grid.count, kMaxGridTracks);
	for (int i = 0; i < trackCount; ++i) {
		const CssCompiledGridTrack &track = grid.tracks[i];
		types[i] = track.type;
		values[i] = track.type == 1
		    ? static_cast<std::int16_t>(resolveCompiledLengthForNode(track.length, nodeId, axis))
		    : track.value;
	}
	count = static_cast<std::int8_t>(trackCount);
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	markDisplayListDirtyForStyleApply();
}

int g_resolvedFontNode = -1;
int g_resolvedLineHeightNode = -1;
bool isLineHeightProperty(Property property)
{
	return property == Property::LineHeight || property == Property::LineHeightExpression || property == Property::LineHeightMultiplier;
}

bool isFontMetricProperty(Property property)
{
	return property == Property::FontId || property == Property::FontSize || property == Property::FontWeight;
}

void applyGridTemplateValue(NodeHandle node, const std::string &value, bool columns)
{
	CssCompiledGridTemplate grid;
	if (parseGridTemplateSpec(value, grid)) applyCompiledGridTemplateValue(node, grid, columns);
}

bool setClassRuleValueFastUnchecked(Node &target, Property property, int value)
{
	if (g_resolvedFontNode >= 0 && isFontMetricProperty(property) &&
	    &target == &treeState().nodes[g_resolvedFontNode]) return true;
	if (g_resolvedLineHeightNode >= 0 && isLineHeightProperty(property) &&
	    &target == &treeState().nodes[g_resolvedLineHeightNode]) return true;
	ComputedStyle &style = target.style;
	switch (property) {
	case Property::Display:
		style.display_explicit = 1;
		style.display = value;
		return true;
	case Property::FlexDirection:
		style.flex_direction_explicit = 1;
		style.flex_direction = value;
		return true;
	case Property::FlexWrap: style.flex_wrap = value; return true;
	case Property::FlexLineCount: rstyleMut(style).flex_line_count = value; return true;
	case Property::BackgroundClip: rstyleMut(style).bg_clip = value; return true;
	case Property::BackgroundSizeList: rstyleMut(style).bg_size_list = value; return true;
	case Property::BackgroundPositionList: rstyleMut(style).bg_position_list = value; return true;
	case Property::BackgroundRepeatList: rstyleMut(style).bg_repeat_list = value; return true;
	case Property::BackgroundAttachmentList: rstyleMut(style).bg_attachment_list = value; return true;
	case Property::BackgroundOriginList: rstyleMut(style).bg_origin_list = value; return true;

	case Property::Containment: rstyleMut(style).containment = value; return true;
	case Property::JustifyContent: style.justify_content = value; return true;
	case Property::AlignItems: style.align_items = value; return true;
	case Property::JustifyItems: style.justify_items = value; return true;
	case Property::AlignContent: style.align_content = value; return true;
	case Property::AlignSelf: style.align_self = value; return true;
	case Property::JustifySelf: rstyleMut(style).justify_self = value; return true;
	case Property::GridRowStart: rstyleMut(style).grid_line[0] = value; return true;
	case Property::GridColumnStart: rstyleMut(style).grid_line[1] = value; return true;
	case Property::GridRowEnd: rstyleMut(style).grid_line[2] = value; return true;
	case Property::GridColumnEnd: rstyleMut(style).grid_line[3] = value; return true;
	case Property::BoxSizing: style.box_sizing = value; return true;
	case Property::Float: style.float_side = value; return true;
	case Property::AspectRatio: rstyleMut(style).aspect_ratio = value; return true;
	case Property::MarginTrim: rstyleMut(style).margin_trim = value; return true;
	case Property::Clear: style.clear_side = value; return true;
	case Property::WritingMode: style.writing_mode = value; return true;
	case Property::Direction: style.direction = value; return true;
	case Property::RowGap: style.row_gap = value; return true;
	case Property::ColumnGap: style.column_gap = value; return true;
	case Property::RowGapPercent: style.row_gap_percent = value; return true;
	case Property::ColumnGapPercent: style.column_gap_percent = value; return true;
	case Property::MarginTopAuto: style.margin_auto = (style.margin_auto & ~1) | (value ? 1 : 0); return true;
	case Property::MarginRightAuto: style.margin_auto = (style.margin_auto & ~2) | (value ? 2 : 0); return true;
	case Property::MarginBottomAuto: style.margin_auto = (style.margin_auto & ~4) | (value ? 4 : 0); return true;
	case Property::MarginLeftAuto: style.margin_auto = (style.margin_auto & ~8) | (value ? 8 : 0); return true;
	case Property::MarginTopExpression: rstyleMut(style).margin_expression[0] = value; return true;
	case Property::MarginRightExpression: rstyleMut(style).margin_expression[1] = value; return true;
	case Property::MarginBottomExpression: rstyleMut(style).margin_expression[2] = value; return true;
	case Property::MarginLeftExpression: rstyleMut(style).margin_expression[3] = value; return true;
	case Property::PaddingTopExpression: rstyleMut(style).padding_expression[0] = value; return true;
	case Property::PaddingRightExpression: rstyleMut(style).padding_expression[1] = value; return true;
	case Property::PaddingBottomExpression: rstyleMut(style).padding_expression[2] = value; return true;
	case Property::PaddingLeftExpression: rstyleMut(style).padding_expression[3] = value; return true;
	case Property::WidthExpression: style.width_expression = value; style.width = style.width_percent = kUnset; return true;
	case Property::HeightExpression: style.height_expression = value; style.height = style.height_percent = kUnset; return true;
	case Property::Order: style.order = value; return true;
	case Property::Gap: style.gap = value; style.row_gap = style.column_gap = style.row_gap_percent = style.column_gap_percent = kUnset; return true;
	case Property::Width:
		style.width_expression = -1;
		style.width = value;
		style.width_percent = kUnset;
		return true;
	case Property::Height:
		style.height_expression = -1;
		style.height = value;
		style.height_percent = kUnset;
		return true;
	case Property::WidthPercent:
		style.width_expression = -1;
		style.width_percent = value;
		style.width = kUnset;
		return true;
	case Property::HeightPercent:
		style.height_expression = -1;
		style.height_percent = value;
		style.height = kUnset;
		return true;
	case Property::MinWidth: style.min_width = value; return true;
	case Property::MinHeight: style.min_height = value; return true;
	case Property::MaxWidth: style.max_width = value; return true;
	case Property::MaxHeight: style.max_height = value; return true;
	case Property::Flex: style.flex = value; return true;
	case Property::FlexShrink: style.flex_shrink = value; return true;
	case Property::FlexBasis:
		if (rstyle(style).flex_basis_expression >= 0) rstyleMut(style).flex_basis_expression = -1;
		style.flex_basis = value; return true;
	case Property::FlexBasisExpression:
		rstyleMut(style).flex_basis_expression = value; style.flex_basis = kUnset; return true;
	case Property::PaddingTop: if (rstyle(style).padding_expression[0] >= 0) rstyleMut(style).padding_expression[0] = -1; style.padding[0] = value; return true;
	case Property::PaddingRight: if (rstyle(style).padding_expression[1] >= 0) rstyleMut(style).padding_expression[1] = -1; style.padding[1] = value; return true;
	case Property::PaddingBottom: if (rstyle(style).padding_expression[2] >= 0) rstyleMut(style).padding_expression[2] = -1; style.padding[2] = value; return true;
	case Property::PaddingLeft: if (rstyle(style).padding_expression[3] >= 0) rstyleMut(style).padding_expression[3] = -1; style.padding[3] = value; return true;
	case Property::MarginTop: if (rstyle(style).margin_expression[0] >= 0) rstyleMut(style).margin_expression[0] = -1; style.margin_auto &= ~1; style.margin[0] = value; return true;
	case Property::MarginRight: if (rstyle(style).margin_expression[1] >= 0) rstyleMut(style).margin_expression[1] = -1; style.margin_auto &= ~2; style.margin[1] = value; return true;
	case Property::MarginBottom: if (rstyle(style).margin_expression[2] >= 0) rstyleMut(style).margin_expression[2] = -1; style.margin_auto &= ~4; style.margin[2] = value; return true;
	case Property::MarginLeft: if (rstyle(style).margin_expression[3] >= 0) rstyleMut(style).margin_expression[3] = -1; style.margin_auto &= ~8; style.margin[3] = value; return true;
	case Property::Position: if (value == kPositionFixed) treeState().fixedPositionUsed = true; style.position = value; return true;
	case Property::Top:
		style.pos_offsets[0] = value;
		style.pos_offset_percent[0] = kUnset;
		return true;
	case Property::Right:
		style.pos_offsets[1] = value;
		style.pos_offset_percent[1] = kUnset;
		return true;
	case Property::Bottom:
		style.pos_offsets[2] = value;
		style.pos_offset_percent[2] = kUnset;
		return true;
	case Property::Left:
		style.pos_offsets[3] = value;
		style.pos_offset_percent[3] = kUnset;
		return true;
	case Property::TopPercent:
		style.pos_offset_percent[0] = value;
		style.pos_offsets[0] = kUnset;
		return true;
	case Property::RightPercent:
		style.pos_offset_percent[1] = value;
		style.pos_offsets[1] = kUnset;
		return true;
	case Property::BottomPercent:
		style.pos_offset_percent[2] = value;
		style.pos_offsets[2] = kUnset;
		return true;
	case Property::LeftPercent:
		style.pos_offset_percent[3] = value;
		style.pos_offsets[3] = kUnset;
		return true;
	case Property::ZIndex:
		style.z_index_auto = value == kZIndexAuto;
		style.z_index = style.z_index_auto ? 0 : std::clamp(value, -32768, 32767);
		return true;
	case Property::BackgroundColor: {
		style.bg_color = StyleValues::pixelFromStyleValue(value);
		style.bg_alpha = 255;
		return true;
	}
	case Property::BackgroundAlpha: style.bg_alpha = static_cast<std::uint8_t>(std::clamp(value, 0, 255)); return true;
	case Property::BackgroundImage:
		StyleValues::applyBackgroundImage(style, value, static_cast<int>(&target - treeState().nodes));
		return true;
	case Property::HasBackground: style.has_bg = value; return true;
	case Property::ActiveBackgroundColor:
		style.active_bg_color = StyleValues::pixelFromStyleValue(value);
		return true;
	case Property::HasActiveBackground: style.has_active_bg = value; return true;
	case Property::Color:
		style.text_color = StyleValues::pixelFromStyleValue(value);
		style.text_alpha = 255;
		return true;
	case Property::Opacity:
		style.opacity = static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		return true;
	case Property::ColorAlpha: style.text_alpha = static_cast<std::uint8_t>(std::clamp(value, 0, 255)); return true;
	case Property::BorderAlpha: style.border_alpha = static_cast<std::uint8_t>(std::clamp(value, 0, 255)); return true;
	case Property::BorderTopAlpha:
	case Property::BorderRightAlpha:
	case Property::BorderBottomAlpha:
	case Property::BorderLeftAlpha:
		rstyleMut(style).border_side_alpha[static_cast<int>(property) - static_cast<int>(Property::BorderTopAlpha)] = static_cast<std::uint8_t>(std::clamp(value, 0, 255));
		return true;
	case Property::BorderColorCurrent: setBorderColorBinding(style, -1, value != 0); return true;
	case Property::BorderTopColorCurrent:
	case Property::BorderRightColorCurrent:
	case Property::BorderBottomColorCurrent:
	case Property::BorderLeftColorCurrent:
		setBorderColorBinding(style, static_cast<int>(property) - static_cast<int>(Property::BorderTopColorCurrent), value != 0);
		return true;
	case Property::BorderWidth:
		setComputedBorderWidth(style, -1, value, target.parent >= 0 ? &treeState().nodes[target.parent].style : nullptr); return true;
	case Property::BorderColor:
		setBorderColorBinding(style, -1, false);
		style.border_color = StyleValues::pixelFromStyleValue(value);
		style.border_alpha = 255;
		return true;
	case Property::BorderTopRelief:
	case Property::BorderRightRelief:
	case Property::BorderBottomRelief:
	case Property::BorderLeftRelief: {
		const int side = static_cast<int>(property) - static_cast<int>(Property::BorderTopRelief);
		if (rstyle(style).border_relief[side] != value) rstyleMut(style).border_relief[side] = static_cast<uint8_t>(value);
		return true;
	}
	case Property::BorderRelief:
		if (value == 0 && !hasBorderRelief(style)) return true;
		for (int side = 0; side < 4; ++side) rstyleMut(style).border_relief[side] = static_cast<uint8_t>(value);
		return true;
	case Property::BorderTopWidth: setComputedBorderWidth(style, 0, value, target.parent >= 0 ? &treeState().nodes[target.parent].style : nullptr); return true;
	case Property::BorderRightWidth: setComputedBorderWidth(style, 1, value, target.parent >= 0 ? &treeState().nodes[target.parent].style : nullptr); return true;
	case Property::BorderBottomWidth: setComputedBorderWidth(style, 2, value, target.parent >= 0 ? &treeState().nodes[target.parent].style : nullptr); return true;
	case Property::BorderLeftWidth: setComputedBorderWidth(style, 3, value, target.parent >= 0 ? &treeState().nodes[target.parent].style : nullptr); return true;
	case Property::BorderTopColor:
		setBorderColorBinding(style, 0, false);
		rstyleMut(style).border_side_color[0] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[0] = 255;
		return true;
	case Property::BorderRightColor:
		setBorderColorBinding(style, 1, false);
		rstyleMut(style).border_side_color[1] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[1] = 255;
		return true;
	case Property::BorderBottomColor:
		setBorderColorBinding(style, 2, false);
		rstyleMut(style).border_side_color[2] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[2] = 255;
		return true;
	case Property::BorderLeftColor:
		setBorderColorBinding(style, 3, false);
		rstyleMut(style).border_side_color[3] = StyleValues::pixelFromStyleValue(value);
		rstyleMut(style).border_side_alpha[3] = 255;
		return true;
	case Property::BorderRadiusTopLeft:
		style.border_radius[0] = value;
		style.border_radius_percent[0] = kUnset;
		return true;
	case Property::BorderRadiusTopRight:
		style.border_radius[1] = value;
		style.border_radius_percent[1] = kUnset;
		return true;
	case Property::BorderRadiusBottomRight:
		style.border_radius[2] = value;
		style.border_radius_percent[2] = kUnset;
		return true;
	case Property::BorderRadiusBottomLeft:
		style.border_radius[3] = value;
		style.border_radius_percent[3] = kUnset;
		return true;
	case Property::BorderRadiusTopLeftPercent:
		style.border_radius_percent[0] = value;
		style.border_radius[0] = 0;
		return true;
	case Property::BorderRadiusTopRightPercent:
		style.border_radius_percent[1] = value;
		style.border_radius[1] = 0;
		return true;
	case Property::BorderRadiusBottomRightPercent:
		style.border_radius_percent[2] = value;
		style.border_radius[2] = 0;
		return true;
	case Property::BorderRadiusBottomLeftPercent:
		style.border_radius_percent[3] = value;
		style.border_radius[3] = 0;
		return true;
	case Property::FontId: style.font_id = value; return true;
	case Property::FontSize: style.font_size = value; return true;
	case Property::FontWeight: style.font_weight = value; return true;
	case Property::LineHeight:
		if (style.line_height_multiplier >= 0) style.line_height_multiplier = -1;
		if (rstyle(style).line_height_expression >= 0) rstyleMut(style).line_height_expression = -1;
		style.line_height = value; return true;
	case Property::LineHeightExpression:
		if (style.line_height_multiplier >= 0) style.line_height_multiplier = -1;
		rstyleMut(style).line_height_expression = value;
		style.line_height = resolveLineHeightExpression(static_cast<int>(&target - treeState().nodes), value); return true;
	case Property::LineHeightMultiplier:
		style.line_height_multiplier = value;
		if (rstyle(style).line_height_expression >= 0) rstyleMut(style).line_height_expression = -1;
		style.line_height = resolveLineHeightMultiplier(static_cast<int>(&target - treeState().nodes), value); return true;
	case Property::TextAlign: style.text_align = value; return true;
	case Property::TextDecoration: style.text_decoration = value; return true;
	case Property::TextTransform: style.text_transform = value; return true;
	case Property::WhiteSpace: style.white_space = static_cast<std::int8_t>(value); return true;
	case Property::TextOverflow: style.text_overflow = static_cast<std::int8_t>(value); return true;
	case Property::TransformStyle: rstyleMut(style).transform_preserve_3d = value != 0; return true;
	case Property::Visibility: style.visibility = static_cast<std::int8_t>(value); return true;
	case Property::Backface: style.backface_hidden = static_cast<std::int8_t>(value); return true;
	case Property::PointerEvents: style.pointer_events = static_cast<std::int8_t>(value); return true;
	case Property::Overflow: {
		const auto next = static_cast<std::int8_t>(value);
		style.overflow = next;
		style.overflow_x = next;
		style.overflow_y = next;
		return true;
	}
	case Property::OverflowX: {
		const auto next = static_cast<std::int8_t>(value);
		style.overflow_x = next;
		style.overflow = aggregateOverflow(next, style.overflow_y);
		return true;
	}
	case Property::OverflowY: {
		const auto next = static_cast<std::int8_t>(value);
		style.overflow_y = next;
		style.overflow = aggregateOverflow(style.overflow_x, next);
		return true;
	}
	case Property::MaskRightFadeWidth:
		style.mask_right_fade_width = static_cast<std::int16_t>(value < 0 ? 0 : value > 32767 ? 32767 : value);
		return true;
	case Property::ImageId: target.image_id = value; return true;
	case Property::ImageFit: style.image_fit = value; return true;
	case Property::TransformTranslateOuterAxes: rstyleMut(style).transform_translate_outer_axes = value; return true;
	case Property::RotateAngle: rstyleMut(style).rotate_angle = value; return true;
	case Property::RotateAxisX: rstyleMut(style).rotate_axis_x = value; return true;
	case Property::RotateAxisY: rstyleMut(style).rotate_axis_y = value; return true;
	case Property::RotateAxisZ: rstyleMut(style).rotate_axis_z = value; return true;
	case Property::ScaleX: rstyleMut(style).scale_x = value; return true;
	case Property::ScaleY: rstyleMut(style).scale_y = value; return true;
	case Property::ScaleZ: rstyleMut(style).scale_z = value; return true;
	case Property::TranslatePresent: rstyleMut(style).translate_present = value != 0; return true;
	case Property::TranslateX: rstyleMut(style).translate_x = value; return true;
	case Property::TranslateY: rstyleMut(style).translate_y = value; return true;
	case Property::TranslateZ: rstyleMut(style).translate_z = value; return true;
	case Property::TranslateXPercent: rstyleMut(style).translate_x_percent = value; return true;
	case Property::TranslateYPercent: rstyleMut(style).translate_y_percent = value; return true;
	case Property::TransformPresent: rstyleMut(style).transform_present = value != 0; return true;
	case Property::RotatePresent: rstyleMut(style).rotate_present = value != 0; return true;
	case Property::ScalePresent: rstyleMut(style).scale_present = value != 0; return true;
	case Property::FilterPresent: rstyleMut(style).filter_present = value != 0; return true;
	case Property::TransformRotate: rstyleMut(style).transform_rotate = value; return true;
	case Property::TransformRotateX: rstyleMut(style).transform_rotate_x = value; return true;
	case Property::TransformRotateY: rstyleMut(style).transform_rotate_y = value; return true;
	case Property::TransformTranslateX: rstyleMut(style).transform_translate_x = value; return true;
	case Property::TransformTranslateY: rstyleMut(style).transform_translate_y = value; return true;
	case Property::TransformTranslateZ: rstyleMut(style).transform_translate_z = value; return true;
	case Property::TransformTranslateXPercent: rstyleMut(style).transform_translate_x_percent = value; return true;
	case Property::TransformTranslateYPercent: rstyleMut(style).transform_translate_y_percent = value; return true;
	case Property::TransformScaleX: rstyleMut(style).transform_scale_x = value; return true;
	case Property::TransformScaleY: rstyleMut(style).transform_scale_y = value; return true;
	case Property::TransformScaleZ: rstyleMut(style).transform_scale_z = value; return true;
	case Property::TransformOriginX: rstyleMut(style).transform_origin_x = value; return true;
	case Property::TransformOriginY: rstyleMut(style).transform_origin_y = value; return true;
	case Property::Perspective: rstyleMut(style).perspective = value; return true;
	case Property::PerspectiveOriginX: rstyleMut(style).perspective_origin_x = value; return true;
	case Property::PerspectiveOriginY: rstyleMut(style).perspective_origin_y = value; return true;
	case Property::FilterBlur: rstyleMut(style).filter_blur_radius = value; return true;
	case Property::BoxShadowInset:
		rstyleMut(style).box_shadow_inset = value != 0 ? 1 : 0;
		return true;
	case Property::BoxShadowOffsetX: rstyleMut(style).box_shadow_offset_x = value; return true;
	case Property::BoxShadowOffsetY: rstyleMut(style).box_shadow_offset_y = value; return true;
	case Property::BoxShadowBlur: rstyleMut(style).box_shadow_blur_radius = value; return true;
	case Property::BoxShadowSpread: rstyleMut(style).box_shadow_spread = value; return true;
	case Property::BoxShadowColor:
		rstyleMut(style).box_shadow_color = StyleValues::pixelFromStyleValue(value);
		return true;
	case Property::BoxShadowAlpha:
		rstyleMut(style).box_shadow_alpha = static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		return true;
	default:
		return false;
	}
}

bool setClassRuleValueFast(NodeHandle node, Property property, int value)
{
	if (!node) return true;
	auto &state = treeState();
	if (state.styleInvalidationSuppressionDepth <= 0) return false;
	const int nodeId = node.id();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	return setClassRuleValueFastUnchecked(target, property, value);
}

void setStyleValue(NodeHandle node, Property property, int value, StyleApplicationSource source)
{
	if (!node) return;
	if (source == StyleApplicationSource::ClassRule) {
		if (node.id() == g_resolvedFontNode && isFontMetricProperty(property)) return;
		if (node.id() == g_resolvedLineHeightNode && isLineHeightProperty(property)) return;
		if (setClassRuleValueFast(node, property, value)) return;
		Tree::instance().setStyleFromClass(node.id(), property, value);
	} else {
		node.style().set(property, value);
	}
}

void setStyleValueKnownTarget(NodeHandle node,
                              Node &target,
                              Property property,
                              int value,
                              StyleApplicationSource source)
{
	if (source == StyleApplicationSource::ClassRule &&
	    treeState().styleInvalidationSuppressionDepth > 0 &&
	    setClassRuleValueFastUnchecked(target, property, value))
		return;
	setStyleValue(node, property, value, source);
}

void setPositionOffsetValue(NodeHandle node,
                            Property lengthProperty,
                            Property percentProperty,
                            const std::string &value,
                            LengthAxis axis,
                            StyleApplicationSource source)
{
	if (toLowerAscii(trimCssValue(value)) == "auto") {
		setStyleValue(node, lengthProperty, kUnset, source);
		return;
	}
	int percent = 0;
	if (parseSimplePercentPermille(value, percent)) {
		setStyleValue(node, percentProperty, percent, source);
		return;
	}
	// parseSimplePercentPermille only recognises a BARE `NN%`. An inline
	// `left: calc(50% - 1.2%)` fell through to parseLengthForNode, which froze it to px
	// against percentBasisForNode() -- the VIEWPORT, because JSX styles a child before
	// its parent element exists -- and nothing ever re-resolved it after the append.
	if (const CssLengthSpec *compiled = cachedCompiledCssLengthSpec(value)) {
		const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(*compiled, node.id(), axis);
		if (resolved.isPercent) {
			setStyleValue(node, percentProperty, roundToInt(resolved.value), source);
			return;
		}
	}
	setStyleValue(node, lengthProperty, parseLengthForNode(value, node.id(), axis), source);
}

int deferredLengthExpression(const CssLengthSpec &length)
{
	CssLengthSpec expression = length;
	if (expression.unit != CssLengthUnit::Expression) {
		auto &cache = boxLengthExpressionCache();
		int handle = -1;
		for (const auto &entry : cache)
			if (entry.first.unit == length.unit && entry.first.value == length.value) { handle = entry.second; break; }
		if (handle < 0) {
			CssLengthExpression wrapper;
			wrapper.a = length;
			wrapper.b = CssLengthSpec{0, CssLengthUnit::Px};
			if (!storeCompiledCssLengthExpressionSpec(wrapper, expression)) return -1;
			handle = static_cast<int>(expression.value);
			cache.push_back({length, handle});
		}
		expression = CssLengthSpec{static_cast<float>(handle), CssLengthUnit::Expression};
	}
	return static_cast<int>(expression.value);
}

void setFlexBasisValue(NodeHandle node, bool hasBasis, const CssLengthSpec &length, StyleApplicationSource source)
{
	if (hasBasis && (length.unit == CssLengthUnit::Expression || lengthNeedsLayout(length, node.id()))) {
		const int expression = deferredLengthExpression(length);
		if (expression >= 0) setStyleValue(node, Property::FlexBasisExpression, expression, source);
	} else {
		setStyleValue(node, Property::FlexBasis, hasBasis ? resolveCompiledLengthForNode(length, node.id(), LengthAxis::Horizontal) : kUnset, source);
	}
}

void setBoxLengthValue(NodeHandle node, bool padding, int side, const CssLengthSpec &length, StyleApplicationSource source)
{
	const Property pixel = static_cast<Property>(static_cast<int>(padding ? Property::PaddingTop : Property::MarginTop) + side);
	const int value = length.unit == CssLengthUnit::Auto ? 0 : resolveCompiledLengthForNode(length, node.id(), LengthAxis::Horizontal);
	setStyleValue(node, pixel, padding ? std::max(0, value) : value, source);
	if (!padding)
		setStyleValue(node, static_cast<Property>(static_cast<int>(Property::MarginTopAuto) + side), length.unit == CssLengthUnit::Auto, source);
	// A var() can acquire a percentage or font-relative value later, even
	// when its current fallback is a constant. Keep that expression too.
	if (length.unit != CssLengthUnit::Expression && !lengthNeedsLayout(length, node.id())) return;
	const int expression = deferredLengthExpression(length);
	if (expression < 0) return;
	setStyleValue(node, static_cast<Property>(static_cast<int>(padding ? Property::PaddingTopExpression : Property::MarginTopExpression) + side),
	              expression, source);
}

void setSizeValue(NodeHandle node,
                  Property lengthProperty,
                  Property percentProperty,
                  const std::string &value,
                  LengthAxis axis,
                  StyleApplicationSource source)
{
	const std::string keyword = toLowerAscii(trimCssValue(value));
	if ((lengthProperty == Property::Width || lengthProperty == Property::Height) &&
	    (keyword == "min-content" || keyword == "max-content" || keyword == "fit-content")) {
		setStyleValue(node, lengthProperty == Property::Width ? Property::WidthExpression : Property::HeightExpression,
		              keyword == "min-content" ? kSizeMinContent : keyword == "max-content" ? kSizeMaxContent : kSizeFitContent,
		              source);
		return;
	}
	CssLengthSpec spec;
	if (parseCompiledLengthSpec(value, spec, true) && spec.unit == CssLengthUnit::Expression &&
	    lengthNeedsLayout(spec, node.id()) &&
	    !resolveCompiledLengthForNodeDetailed(spec, node.id(), axis).isPercent) {
		setStyleValue(node, lengthProperty == Property::Width ? Property::WidthExpression : Property::HeightExpression,
		              static_cast<int>(spec.value), source);
		return;
	}
	// `width: auto` / `height: auto` is the CSS initial value: size the box to its
	// content (the same as not declaring width/height at all). Map it to kUnset on
	// BOTH the length and percent companions so the flex pass autosizes the box.
	// Without this, parseLengthForNode("auto") falls through to its 0 keyword
	// fallback, so an explicit `width: auto` collapsed the box to a 0px width
	// (e.g. topbar buttons rendered 0-wide, overlapping their labels) — diverging
	// from the browser, which content-sizes it.
	if (toLowerAscii(trimCssValue(value)) == "auto") {
		setStyleValue(node, lengthProperty, kUnset, source);
		setStyleValue(node, percentProperty, kUnset, source);
		return;
	}
	int percent = 0;
	if (parseSimplePercentPermille(value, percent))
		setStyleValue(node, percentProperty, percent, source);
	else
		setStyleValue(node, lengthProperty, parseLengthForNode(value, node.id(), axis), source);
}

void setAllPadding(NodeHandle node, int value, StyleApplicationSource source)
{
	setStyleValue(node, Property::PaddingTop, value, source);
	setStyleValue(node, Property::PaddingRight, value, source);
	setStyleValue(node, Property::PaddingBottom, value, source);
	setStyleValue(node, Property::PaddingLeft, value, source);
}

void setPaddingBox(NodeHandle node, const BoxLengths &box, StyleApplicationSource source)
{
	setStyleValue(node, Property::PaddingTop, box.top, source);
	setStyleValue(node, Property::PaddingRight, box.right, source);
	setStyleValue(node, Property::PaddingBottom, box.bottom, source);
	setStyleValue(node, Property::PaddingLeft, box.left, source);
}

void setAllMargin(NodeHandle node, int value, StyleApplicationSource source)
{
	setStyleValue(node, Property::MarginTop, value, source);
	setStyleValue(node, Property::MarginRight, value, source);
	setStyleValue(node, Property::MarginBottom, value, source);
	setStyleValue(node, Property::MarginLeft, value, source);
}

void setMarginBox(NodeHandle node, const BoxLengths &box, StyleApplicationSource source)
{
	setStyleValue(node, Property::MarginTop, box.top, source);
	setStyleValue(node, Property::MarginRight, box.right, source);
	setStyleValue(node, Property::MarginBottom, box.bottom, source);
	setStyleValue(node, Property::MarginLeft, box.left, source);
}

void setAllBorderRadius(NodeHandle node, int value, StyleApplicationSource source)
{
	setStyleValue(node, Property::BorderRadiusTopLeft, value, source);
	setStyleValue(node, Property::BorderRadiusTopRight, value, source);
	setStyleValue(node, Property::BorderRadiusBottomRight, value, source);
	setStyleValue(node, Property::BorderRadiusBottomLeft, value, source);
}

Property borderRadiusLengthProperty(int corner)
{
	switch (corner) {
	case 0: return Property::BorderRadiusTopLeft;
	case 1: return Property::BorderRadiusTopRight;
	case 2: return Property::BorderRadiusBottomRight;
	default: return Property::BorderRadiusBottomLeft;
	}
}

Property borderRadiusPercentProperty(int corner)
{
	switch (corner) {
	case 0: return Property::BorderRadiusTopLeftPercent;
	case 1: return Property::BorderRadiusTopRightPercent;
	case 2: return Property::BorderRadiusBottomRightPercent;
	default: return Property::BorderRadiusBottomLeftPercent;
	}
}

void setBorderRadiusCornerValue(NodeHandle node,
                                int corner,
                                const std::string &value,
                                StyleApplicationSource source)
{
	int percent = 0;
	if (parseSimplePercentPermille(value, percent))
		setStyleValue(node, borderRadiusPercentProperty(corner), percent, source);
	else
		setStyleValue(node, borderRadiusLengthProperty(corner), parseLengthForNode(value, node.id(), LengthAxis::None), source);
}

void applyBorderRadiusValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
	const auto axes = splitTopLevel(value, '/');
	const auto parts = splitWords(axes.empty() ? value : axes[0]);
	if (parts.empty()) return;
	const std::string &tl = parts[0];
	const std::string &tr = parts.size() > 1 ? parts[1] : tl;
	const std::string &br = parts.size() > 2 ? parts[2] : tl;
	const std::string &bl = parts.size() > 3 ? parts[3] : tr;
	setBorderRadiusCornerValue(node, 0, tl, source);
	setBorderRadiusCornerValue(node, 1, tr, source);
	setBorderRadiusCornerValue(node, 2, br, source);
	setBorderRadiusCornerValue(node, 3, bl, source);
}

void setIndividualRotation(NodeHandle node, const IndividualRotation &r, bool present, StyleApplicationSource source)
{
	setStyleValue(node, Property::RotatePresent, present, source);
	setStyleValue(node, Property::RotateAngle, r.angle, source);
	setStyleValue(node, Property::RotateAxisX, r.x, source);
	setStyleValue(node, Property::RotateAxisY, r.y, source);
	setStyleValue(node, Property::RotateAxisZ, r.z, source);
}

void setIndividualScale(NodeHandle node, const int (&scale)[3], bool present, StyleApplicationSource source)
{
	setStyleValue(node, Property::ScalePresent, present, source);
	setStyleValue(node, Property::ScaleX, scale[0], source);
	setStyleValue(node, Property::ScaleY, scale[1], source);
	setStyleValue(node, Property::ScaleZ, scale[2], source);
}

void setIndividualTranslation(NodeHandle node, const TransformComponents &t, StyleApplicationSource source)
{
	setStyleValue(node, Property::TranslatePresent, t.hasTranslateX || t.hasTranslateY || t.hasTranslateZ, source);
	setStyleValue(node, Property::TranslateX, t.translateX, source);
	setStyleValue(node, Property::TranslateY, t.translateY, source);
	setStyleValue(node, Property::TranslateZ, t.translateZ, source);
	setStyleValue(node, Property::TranslateXPercent, t.translateXPercent, source);
	setStyleValue(node, Property::TranslateYPercent, t.translateYPercent, source);
}

void setTransformComponents(NodeHandle node, const TransformComponents &t, StyleApplicationSource source)
{
	setStyleValue(node, Property::TransformTranslateOuterAxes, t.translateOuterAxes, source);
	setStyleValue(node, Property::TransformPresent, t.hasRotateX || t.hasRotateY || t.hasRotateZ || t.hasTranslateX || t.hasTranslateY || t.hasTranslateZ || t.hasScaleX || t.hasScaleY || t.hasScaleZ, source);
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] setTransformComponents node=%d src=%d rx=%d ry=%d rz=%d tz=%d sx=%d sy=%d sz=%d\n",
			            node.id(), static_cast<int>(source), t.rotateX, t.rotateY, t.rotateZ, t.translateZ, t.scaleX,
			            t.scaleY, t.scaleZ);
	}
	setStyleValue(node, Property::TransformRotateX, t.rotateX, source);
	setStyleValue(node, Property::TransformRotateY, t.rotateY, source);
	setStyleValue(node, Property::TransformRotate, t.rotateZ, source);
	setStyleValue(node, Property::TransformTranslateX, t.translateX, source);
	setStyleValue(node, Property::TransformTranslateY, t.translateY, source);
	setStyleValue(node, Property::TransformTranslateZ, t.translateZ, source);
	setStyleValue(node, Property::TransformTranslateXPercent, t.translateXPercent, source);
	setStyleValue(node, Property::TransformTranslateYPercent, t.translateYPercent, source);
	setStyleValue(node, Property::TransformScaleX, t.scaleX, source);
	setStyleValue(node, Property::TransformScaleY, t.scaleY, source);
	setStyleValue(node, Property::TransformScaleZ, t.scaleZ, source);
}

bool applyTransformComponentsFast(NodeHandle node, const TransformComponents &t, StyleApplicationSource source)
{
	setStyleValue(node, Property::TransformTranslateOuterAxes, t.translateOuterAxes, source);
	setStyleValue(node, Property::TransformPresent, t.hasRotateX || t.hasRotateY || t.hasRotateZ || t.hasTranslateX || t.hasTranslateY || t.hasTranslateZ || t.hasScaleX || t.hasScaleY || t.hasScaleZ, source);
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] applyTransformComponentsFast node=%d src=%d rx=%d ry=%d rz=%d tz=%d sx=%d sy=%d sz=%d\n",
			            node.id(), static_cast<int>(source), t.rotateX, t.rotateY, t.rotateZ, t.translateZ, t.scaleX,
			            t.scaleY, t.scaleZ);
	}
	if (!node) return true;
	if (source != StyleApplicationSource::ClassRule) return false;
	auto &state = treeState();
	const int nodeId = node.id();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const RareStyle &current = rstyle(target.style);
	if (current.transform_rotate_x == static_cast<int16_t>(t.rotateX) &&
	    current.transform_rotate_y == static_cast<int16_t>(t.rotateY) &&
	    current.transform_rotate == static_cast<int16_t>(t.rotateZ) &&
	    current.transform_translate_x == static_cast<int16_t>(t.translateX) &&
	    current.transform_translate_y == static_cast<int16_t>(t.translateY) &&
	    current.transform_translate_z == static_cast<int16_t>(t.translateZ) &&
	    current.transform_translate_x_percent == static_cast<int16_t>(t.translateXPercent) &&
	    current.transform_translate_y_percent == static_cast<int16_t>(t.translateYPercent) &&
	    current.transform_scale_x == static_cast<int16_t>(t.scaleX) &&
	    current.transform_scale_y == static_cast<int16_t>(t.scaleY) &&
	    current.transform_scale_z == static_cast<int16_t>(t.scaleZ))
		return true;
	RareStyle &rs = rstyleMut(target.style);
	rs.transform_rotate_x = static_cast<int16_t>(t.rotateX);
	rs.transform_rotate_y = static_cast<int16_t>(t.rotateY);
	rs.transform_rotate = static_cast<int16_t>(t.rotateZ);
	rs.transform_translate_x = static_cast<int16_t>(t.translateX);
	rs.transform_translate_y = static_cast<int16_t>(t.translateY);
	rs.transform_translate_z = static_cast<int16_t>(t.translateZ);
	rs.transform_translate_x_percent = static_cast<int16_t>(t.translateXPercent);
	rs.transform_translate_y_percent = static_cast<int16_t>(t.translateYPercent);
	rs.transform_scale_x = static_cast<int16_t>(t.scaleX);
	rs.transform_scale_y = static_cast<int16_t>(t.scaleY);
	rs.transform_scale_z = static_cast<int16_t>(t.scaleZ);
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube]   stored node=%d suppress=%d mounted=%d\n", nodeId,
			            state.styleInvalidationSuppressionDepth,
			            nodeParticipatesInMountedTree(state, nodeId) ? 1 : 0);
	}
	if (state.styleInvalidationSuppressionDepth > 0) return true;
	if (!nodeParticipatesInMountedTree(state, nodeId)) return true;
	target.render.dirty = 1;
	if (state.fixedPositionUsed) target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.transform_dirty = 1;
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
	return true;
}

bool applyTransformSlotsFast(NodeHandle node, const std::int16_t *slots, StyleApplicationSource source)
{
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] applyTransformSlotsFast node=%d src=%d rx=%d ry=%d rz=%d tz=%d sx=%d sy=%d\n", node.id(),
			            static_cast<int>(source), slots[0], slots[1], slots[2], slots[5], slots[8], slots[9]);
	}
	if (!node) return true;
	if (source != StyleApplicationSource::ClassRule) return false;
	auto &state = treeState();
	const int nodeId = node.id();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const RareStyle &current = rstyle(target.style);
	if (current.transform_rotate_x == slots[0] &&
	    current.transform_rotate_y == slots[1] &&
	    current.transform_rotate == slots[2] &&
	    current.transform_translate_x == slots[3] &&
	    current.transform_translate_y == slots[4] &&
	    current.transform_translate_z == slots[5] &&
	    current.transform_translate_x_percent == slots[6] &&
	    current.transform_translate_y_percent == slots[7] &&
	    current.transform_scale_x == slots[8] &&
	    current.transform_scale_y == slots[9] &&
	    current.transform_scale_z == slots[10])
		return true;
	RareStyle &rs = rstyleMut(target.style);
	rs.transform_rotate_x = slots[0];
	rs.transform_rotate_y = slots[1];
	rs.transform_rotate = slots[2];
	rs.transform_translate_x = slots[3];
	rs.transform_translate_y = slots[4];
	rs.transform_translate_z = slots[5];
	rs.transform_translate_x_percent = slots[6];
	rs.transform_translate_y_percent = slots[7];
	rs.transform_scale_x = slots[8];
	rs.transform_scale_y = slots[9];
	rs.transform_scale_z = slots[10];
	if (state.styleInvalidationSuppressionDepth > 0) return true;
	if (!nodeParticipatesInMountedTree(state, nodeId)) return true;
	target.render.dirty = 1;
	if (state.fixedPositionUsed) target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.transform_dirty = 1;
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
	return true;
}

Property borderSideWidthProperty(int side);

void setUniformBorderWidth(NodeHandle node, int width, StyleApplicationSource source)
{
	setStyleValue(node, Property::BorderWidth, width, source);
}

bool applyBorderWidthBox(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	int widths[4];
	for (int side = 0; side < 4; ++side) {
		widths[side] = resolveBorderWidth(compiled.lengths[side], node.id());
		if (widths[side] < 0) return true;
	}
	const bool uniform = widths[0] == widths[1] && widths[0] == widths[2] && widths[0] == widths[3];
	setUniformBorderWidth(node, uniform ? widths[0] : 0, source);
	if (!uniform)
		for (int side = 0; side < 4; ++side) setStyleValue(node, borderSideWidthProperty(side), widths[side], source);
	return true;
}

void applyBorderShorthand(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	CssCompiledValue compiled;
	if (!compileBorderShorthandValue(value, compiled)) return;
	const auto parts = splitFunctionAwareWords(value);
	const int width = resolveBorderWidth(compiled.lengths[0], node.id());
	if (width < 0) return;
	setUniformBorderWidth(node, width, source);
	for (int side = 0; side < 4; ++side)
		setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side), compiled.values[2], source);
	for (const auto &part : parts) {
		if (part.empty()) continue;
		if (part[0] == '#' || part.find("rgb") != std::string::npos || toLowerAscii(part) == "transparent") {
			const ParsedCssColor color = parseCssColor(part);
			setStyleValue(node, Property::BorderColor, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
			if (color.valid) {
				setStyleValue(node, Property::BorderAlpha, color.a, source);
				markNodeDisplayCommandsDirtyForStyleApply(node.id());
			}
			return;
		}
	}
	setStyleValue(node, Property::BorderColorCurrent, 1, source);
}

Property borderSideWidthProperty(int side)
{
	switch (side) {
	case 0: return Property::BorderTopWidth;
	case 1: return Property::BorderRightWidth;
	case 2: return Property::BorderBottomWidth;
	default: return Property::BorderLeftWidth;
	}
}

Property borderSideColorProperty(int side)
{
	switch (side) {
	case 0: return Property::BorderTopColor;
	case 1: return Property::BorderRightColor;
	case 2: return Property::BorderBottomColor;
	default: return Property::BorderLeftColor;
	}
}

int borderSideForProperty(const std::string &property, const char *suffix = "")
{
	const std::string tail = suffix ? suffix : "";
	if (property == std::string("border-top") + tail) return 0;
	if (property == std::string("border-right") + tail) return 1;
	if (property == std::string("border-bottom") + tail) return 2;
	if (property == std::string("border-left") + tail) return 3;
	return -1;
}

void applyBorderSideColorValue(NodeHandle node, int side, const std::string &value, StyleApplicationSource source)
{
	if (!node || side < 0 || side > 3) return;
	if (toLowerAscii(trimCssValue(value)) == "currentcolor") {
		setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopColorCurrent) + side), 1, source);
		return;
	}
	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, borderSideColorProperty(side), color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopAlpha) + side), color.valid ? color.a : 255, source);
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyBorderSideShorthand(NodeHandle node, int side, const std::string &value, StyleApplicationSource source)
{
	if (!node || side < 0 || side > 3) return;
	CssCompiledValue compiled;
	if (!compileBorderShorthandValue(value, compiled)) return;
	const auto parts = splitFunctionAwareWords(value);
	const int width = resolveBorderWidth(compiled.lengths[0], node.id());
	if (width < 0) return;
	setStyleValue(node, borderSideWidthProperty(side), width, source);
	setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side), compiled.values[2], source);
	for (const auto &part : parts) {
		if (part.empty()) continue;
		if (part[0] == '#' || part.find("rgb") != std::string::npos || toLowerAscii(part) == "transparent") {
			applyBorderSideColorValue(node, side, part, source);
			return;
		}
	}
	setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopColorCurrent) + side), 1, source);
}

void applyBackgroundValue(NodeHandle node, const std::string &value, StyleApplicationSource source, bool shorthand = true)
{
	if (!node) return;
	CssCompiledValue compiled;
	compiled.declaration = shorthand ? CssDeclarationId::Background : CssDeclarationId::BackgroundImage;
	if (compileBackgroundValue(value, compiled)) {
		applyBackgroundHandle(node, compiled.values[0], shorthand, source);
		return;
	}
	CssCompiledBackground background;
	const ParsedCssColor color = backgroundBaseColor(value);
	if (!shorthand && color.valid) return;
	if (color.valid) { background.colorStyle = cssColorStyleValue(color); background.colorAlpha = color.a; }
	const auto layers = splitTopLevel(value, ',');
	if (layers.empty() || layers.size() > 65535) return;
	background.layerCount = layers.size();
	if (shorthand) { const int clip = compileBackgroundClip(value, true); if (clip < 0) return; background.clip = clip; }
	for (std::size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
		const auto &layer = layers[layerIndex];
		const auto line = parseGradientLineLayer(layer, node.id());
		if (line.valid) {
			background.gridAxes |= line.vertical ? 1 : 2;
			background.gridColor = cssColorNative(line.color);
			background.gridAlpha = line.color.a;
			CssLengthSpec &length = line.vertical ? background.gridLineX : background.gridLineY;
			length.unit = CssLengthUnit::Px;
			length.value = line.lineWidth;
			if (line.vertical) background.hasGridLineX = 1; else background.hasGridLineY = 1;
			continue;
		}
		const auto gradient = parseLinearGradient(layer);
		if (gradient.valid) {
			if (background.hasGradient) { background.overlayLayer = background.gradientLayer; background.overlayGradient = background.gradient; background.hasOverlayGradient = 1; }
			background.gradientLayer = layerIndex;
			background.gradient = compileLinearGradientLayer(gradient);
			background.hasGradient = 1;
			continue;
		}
		const auto radial = parseRadialGradient(layer);
		if (radial.valid) { background.radialLayer = layerIndex; background.radialGradient = compileRadialGradientLayer(radial); background.hasRadialGradient = 1; }
	}
	if (!color.valid && !background.hasGradient && !background.hasRadialGradient && !background.gridAxes) return;
	const auto handle = storeCompiledCssBackground(background);
	if (handle != kNoCompiledCssBackground) applyBackgroundHandle(node, handle, shorthand, source);
}

void applyBackgroundSizeValue(NodeHandle node, const std::string &value)
{
	if (!node) return;
	Node &target = treeState().nodes[node.id()];
	if (rstyle(target.style).bg_grid_axes == 0) return;
	const auto layers = splitTopLevel(value, ',');
	if (layers.empty()) return;
	const auto parts = splitWords(layers[0]);
	if (parts.empty()) return;
	const int stepX = parseLengthForNode(parts[0], node.id(), LengthAxis::Horizontal);
	const int stepY = parseLengthForNode(parts.size() > 1 ? parts[1] : parts[0], node.id(), LengthAxis::Vertical);
	if (stepX > 0) rstyleMut(target.style).bg_grid_step_x = static_cast<uint16_t>(std::min(stepX, 65535));
	if (stepY > 0) rstyleMut(target.style).bg_grid_step_y = static_cast<uint16_t>(std::min(stepY, 65535));
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyTextColorValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, Property::Color, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	setStyleValue(node, Property::ColorAlpha, color.valid ? color.a : 255, source);
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

void applyBorderColorValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	if (!node) return;
	if (toLowerAscii(trimCssValue(value)) == "currentcolor") {
		setStyleValue(node, Property::BorderColorCurrent, 1, source);
		return;
	}
	const ParsedCssColor color = parseCssColor(firstColorToken(value));
	setStyleValue(node, Property::BorderColor, color.valid ? cssColorStyleValue(color) : parseColorStyleValue(value), source);
	setStyleValue(node, Property::BorderAlpha, color.valid ? color.a : 255, source);
	markNodeDisplayCommandsDirtyForStyleApply(node.id());
}

int16_t *animatedTransformSlot(RareStyle &rs, Property property)
{
	switch (property) {
	case Property::RotateAngle: return &rs.rotate_angle;
	case Property::ScaleX: return &rs.scale_x;
	case Property::ScaleY: return &rs.scale_y;
	case Property::ScaleZ: return &rs.scale_z;
	case Property::TranslateX: return &rs.translate_x;
	case Property::TranslateY: return &rs.translate_y;
	case Property::TranslateZ: return &rs.translate_z;
	case Property::TranslateXPercent: return &rs.translate_x_percent;
	case Property::TranslateYPercent: return &rs.translate_y_percent;
	case Property::TransformRotate: return &rs.transform_rotate;
	case Property::TransformRotateX: return &rs.transform_rotate_x;
	case Property::TransformRotateY: return &rs.transform_rotate_y;
	case Property::TransformTranslateX: return &rs.transform_translate_x;
	case Property::TransformTranslateY: return &rs.transform_translate_y;
	case Property::TransformTranslateZ: return &rs.transform_translate_z;
	case Property::TransformTranslateXPercent: return &rs.transform_translate_x_percent;
	case Property::TransformTranslateYPercent: return &rs.transform_translate_y_percent;
	case Property::TransformScaleX: return &rs.transform_scale_x;
	case Property::TransformScaleY: return &rs.transform_scale_y;
	case Property::TransformScaleZ: return &rs.transform_scale_z;
	default: return nullptr;
	}
}

bool applyAnimatedTransformStyleValueFast(int nodeId, Property property, int value)
{
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	RareStyle &rs = rstyleMut(target.style);
	int16_t *slot = animatedTransformSlot(rs, property);
	if (!slot) return false;
	const int16_t next = static_cast<int16_t>(value);
	if (*slot == next) return true;
	*slot = next;
	if (state.styleInvalidationSuppressionDepth > 0) return true;
	if (!nodeParticipatesInMountedTree(state, nodeId)) return true;
	target.render.dirty = 1;
	if (state.fixedPositionUsed) target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.transform_dirty = 1;
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
	Tree::instance().markNodeDisplayCommandsDirty(nodeId);
	return true;
}

}  // namespace

bool layoutSizeExpressionUsesPercentage(int nodeId, int expression)
{
	return expression >= 0 && lengthDependsOnInput(
	    CssLengthSpec{static_cast<float>(expression), CssLengthUnit::Expression}, nodeId, CssLengthUnit::Percent);
}

int resolveLayoutSizeExpression(int nodeId, int expression, bool horizontal)
{
	return resolveCompiledLengthForNode(CssLengthSpec{static_cast<float>(expression), CssLengthUnit::Expression},
	                                    nodeId, horizontal ? LengthAxis::Horizontal : LengthAxis::Vertical);
}

int resolveLineHeightMultiplier(int nodeId, int bits)
{
	float multiplier = 0;
	std::memcpy(&multiplier, &bits, sizeof(multiplier));
	return roundToInt(std::clamp(static_cast<double>(currentFontSizeForNode(nodeId)) * multiplier, 0.0, 32767.0));
}

int resolveLineHeightExpression(int nodeId, int expression)
{
	LineHeightBasisScope scope(nodeId);
	return std::max(0, resolveLayoutSizeExpression(nodeId, expression, false));
}

int resolveLayoutFlexBasis(int nodeId, int percentageBasis, bool horizontal)
{
	const auto &style = treeState().nodes[nodeId].style;
	const int expression = rstyle(style).flex_basis_expression;
	if (expression < 0) return style.flex_basis;
	if (percentageBasis < 0 && layoutSizeExpressionUsesPercentage(nodeId, expression)) return kUnset;
	const int previous = g_boxPercentageBasis;
	g_boxPercentageBasis = std::max(0, percentageBasis);
	const int result = std::max(0, resolveLayoutSizeExpression(nodeId, expression, horizontal));
	g_boxPercentageBasis = previous;
	return result;
}

bool resolveLayoutBoxLengths(int nodeId, int percentageBasis)
{
	auto &node = treeState().nodes[nodeId];
#if !GEA_EMBEDDED_RARE_STYLE_INLINE
	if (node.style.rare_style < 0) return false;
#endif
	const RareStyle &rare = rstyle(node.style);
	const int previous = g_boxPercentageBasis;
	g_boxPercentageBasis = std::max(0, percentageBasis);
	bool changed = false;
	for (int side = 0; side < 4; ++side) {
		for (bool padding : {false, true}) {
			const int expression = padding ? rare.padding_expression[side] : rare.margin_expression[side];
			if (expression < 0) continue;
			const int raw = resolveLayoutSizeExpression(nodeId, expression, true);
			const int value = std::max(padding ? 0 : -32768, std::min(32767, raw));
			auto &target = padding ? node.style.padding[side] : node.style.margin[side];
			if (target != value) { target = static_cast<int16_t>(value); changed = true; }
		}
	}
	g_boxPercentageBasis = previous;
	return changed;
}


void Style::set(Property property, int value) const
{
	if (nodeId_ < 0) return;
	Tree::instance().setStyle(nodeId_, property, value);
	// This element's cascaded lengths can depend on its own font too. Updating
	// descendants alone leaves, for example, class padding in ch at old metrics.
	if (isFontMetricProperty(property) || isLineHeightProperty(property)) recomputeSubtreeClassStyles(nodeId_);
	else if (propertyAffectsDescendantStyle(property)) recomputeDescendantClassStyles(nodeId_);
}

void Style::backgroundColor(int rgb565) const
{
	set(Property::BackgroundColor, rgb565);
	set(Property::HasBackground, 1);
}

void Style::setProperty(const std::string &property, const std::string &value) const
{
	if (nodeId_ < 0) return;
	StyleSheet::instance().applyProperty(NodeHandle(nodeId_), property, value);
}

bool Style::removeProperty(const std::string &property) const
{
	if (nodeId_ < 0) return false;
	return StyleSheet::instance().removeProperty(NodeHandle(nodeId_), property);
}

void applyAnimatedStyleValue(int nodeId, Property property, int value)
{
	if (nodeId < 0) return;
	if (applyAnimatedTransformStyleValueFast(nodeId, property, value)) return;
	Tree::instance().setStyleFromClass(nodeId, property, value);
	if (propertyAffectsDescendantStyle(property)) recomputeDescendantClassStyles(nodeId);
}

namespace {

bool propertyAffectsDescendantStyle(Property property)
{
	return property == Property::Color || property == Property::ColorAlpha ||
	       property == Property::FontId ||
	       property == Property::FontSize ||
	       property == Property::FontWeight ||
	       property == Property::LineHeight || property == Property::LineHeightExpression || property == Property::LineHeightMultiplier ||
	       property == Property::TextAlign ||
	       property == Property::TextTransform ||
	       property == Property::WhiteSpace || property == Property::Visibility || property == Property::BorderWidth ||
	       (property >= Property::BorderTopWidth && property <= Property::BorderLeftWidth);
}

void applyInheritedStyleDefaults(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	const int parent = state.nodes[node].parent;
	if (parent < 0 || parent >= state.nodeCount) return;

	const auto &parentStyle = state.nodes[parent].style;
	auto &style = state.nodes[node].style;
	style.text_color = parentStyle.text_color;
	style.text_alpha = parentStyle.text_alpha;
	style.font_id = parentStyle.font_id;
	style.font_size = parentStyle.font_size;
	style.font_weight = parentStyle.font_weight;
	style.line_height = parentStyle.line_height;
	if (parentStyle.line_height_multiplier >= 0)
		style.line_height_multiplier = parentStyle.line_height_multiplier;
	style.text_align = parentStyle.text_align;
	style.text_transform = parentStyle.text_transform;
	style.white_space = parentStyle.white_space;
	style.visibility = parentStyle.visibility;
}

bool applyNumberDeclarationWithSource(NodeHandle node,
                                      CssDeclarationId declaration,
                                      double value,
                                      StyleApplicationSource source)
{
	if (!node) return false;
	const int length = numericLength(value);
	switch (declaration) {
	case CssDeclarationId::Order:
		if (std::isfinite(value) && std::trunc(value) == value)
			setStyleValue(node, Property::Order, static_cast<int>(std::clamp(value, -2147483648.0, 2147483647.0)), source);
		return true;
	case CssDeclarationId::Gap:
		setStyleValue(node, Property::Gap, length, source);
		return true;
	case CssDeclarationId::Width:
		setStyleValue(node, Property::Width, length, source);
		return true;
	case CssDeclarationId::Height:
		setStyleValue(node, Property::Height, length, source);
		return true;
	case CssDeclarationId::MinWidth:
		setStyleValue(node, Property::MinWidth, length, source);
		return true;
	case CssDeclarationId::MinHeight:
		setStyleValue(node, Property::MinHeight, length, source);
		return true;
	case CssDeclarationId::MaxWidth:
		setStyleValue(node, Property::MaxWidth, length, source);
		return true;
	case CssDeclarationId::MaxHeight:
		setStyleValue(node, Property::MaxHeight, length, source);
		return true;
	case CssDeclarationId::Flex:
		setStyleValue(node, Property::Flex, rawNumber(value), source);
		setStyleValue(node, Property::FlexShrink, rawNumber(1.0), source);
		setStyleValue(node, Property::FlexBasis, kUnset, source);  // numeric `flex: N` has no explicit basis
		return true;
	case CssDeclarationId::Padding:
		setAllPadding(node, length, source);
		return true;
	case CssDeclarationId::PaddingTop:
		setStyleValue(node, Property::PaddingTop, length, source);
		return true;
	case CssDeclarationId::PaddingRight:
		setStyleValue(node, Property::PaddingRight, length, source);
		return true;
	case CssDeclarationId::PaddingBottom:
		setStyleValue(node, Property::PaddingBottom, length, source);
		return true;
	case CssDeclarationId::PaddingLeft:
		setStyleValue(node, Property::PaddingLeft, length, source);
		return true;
	case CssDeclarationId::Margin:
		setAllMargin(node, length, source);
		return true;
	case CssDeclarationId::MarginTop:
		setStyleValue(node, Property::MarginTop, length, source);
		return true;
	case CssDeclarationId::MarginRight:
		setStyleValue(node, Property::MarginRight, length, source);
		return true;
	case CssDeclarationId::MarginBottom:
		setStyleValue(node, Property::MarginBottom, length, source);
		return true;
	case CssDeclarationId::MarginLeft:
		setStyleValue(node, Property::MarginLeft, length, source);
		return true;
	case CssDeclarationId::Top:
		setStyleValue(node, Property::Top, length, source);
		return true;
	case CssDeclarationId::Right:
		setStyleValue(node, Property::Right, length, source);
		return true;
	case CssDeclarationId::Bottom:
		setStyleValue(node, Property::Bottom, length, source);
		return true;
	case CssDeclarationId::Left:
		setStyleValue(node, Property::Left, length, source);
		return true;
	case CssDeclarationId::ZIndex:
		setStyleValue(node, Property::ZIndex, rawNumber(value), source);
		return true;
	case CssDeclarationId::Opacity:
		setStyleValue(node, Property::Opacity, numericOpacity(value), source);
		return true;
	case CssDeclarationId::BorderWidth:
		if (value >= 0.0 && std::isfinite(value)) setUniformBorderWidth(node, snapBorderWidth(value), source);
		return true;
	case CssDeclarationId::BorderTopWidth:
		if (value >= 0.0 && std::isfinite(value)) setStyleValue(node, Property::BorderTopWidth, snapBorderWidth(value), source);
		return true;
	case CssDeclarationId::BorderRightWidth:
		if (value >= 0.0 && std::isfinite(value)) setStyleValue(node, Property::BorderRightWidth, snapBorderWidth(value), source);
		return true;
	case CssDeclarationId::BorderBottomWidth:
		if (value >= 0.0 && std::isfinite(value)) setStyleValue(node, Property::BorderBottomWidth, snapBorderWidth(value), source);
		return true;
	case CssDeclarationId::BorderLeftWidth:
		if (value >= 0.0 && std::isfinite(value)) setStyleValue(node, Property::BorderLeftWidth, snapBorderWidth(value), source);
		return true;
	case CssDeclarationId::BorderRadius:
		setAllBorderRadius(node, length, source);
		return true;
	case CssDeclarationId::BorderTopLeftRadius:
		setStyleValue(node, Property::BorderRadiusTopLeft, length, source);
		return true;
	case CssDeclarationId::BorderTopRightRadius:
		setStyleValue(node, Property::BorderRadiusTopRight, length, source);
		return true;
	case CssDeclarationId::BorderBottomRightRadius:
		setStyleValue(node, Property::BorderRadiusBottomRight, length, source);
		return true;
	case CssDeclarationId::BorderBottomLeftRadius:
		setStyleValue(node, Property::BorderRadiusBottomLeft, length, source);
		return true;
	case CssDeclarationId::FontSize:
		setStyleValue(node, Property::FontSize, length, source);
		return true;
	case CssDeclarationId::FontWeight:
		setStyleValue(node, Property::FontWeight, std::clamp(roundToInt(value), 1, 1000), source);
		return true;
	case CssDeclarationId::LineHeight: {
		const float multiplier = static_cast<float>(value);
		if (!std::isfinite(multiplier) || multiplier < 0) return false;
		int bits = 0; if (multiplier != 0) std::memcpy(&bits, &multiplier, sizeof(bits));
		setStyleValue(node, Property::LineHeightMultiplier, bits, source);
		return true;
	}
	case CssDeclarationId::Transform:
		setStyleValue(node, Property::TransformPresent, 1, source);
		setStyleValue(node, Property::TransformRotate, numericRotateTenths(value), source);
		return true;
	case CssDeclarationId::Rotate: {
		IndividualRotation rotation; rotation.angle = numericRotateTenths(value);
		setIndividualRotation(node, rotation, true, source); return true;
	}
	case CssDeclarationId::Scale: {
		const int factor = roundToInt(std::clamp(value * 1000, -32768.0, 32767.0));
		const int scale[3] = {factor, factor, 1000};
		setIndividualScale(node, scale, true, source); return true;
	}
	default:
		return false;
	}
}

bool applyNumberPropertyWithSource(NodeHandle node, const char *property, double value, StyleApplicationSource source)
{
	if (!node || !property) return false;
	return applyNumberDeclarationWithSource(node, classifyDeclaration(property), value, source);
}

bool applyRuntimeLineHeightValue(NodeHandle node, std::uint8_t kind, const CssLengthSpec &length, StyleApplicationSource source);
void setAuthoredLineHeightValue(NodeHandle node, const std::string &value, StyleApplicationSource source)
{
	CssCompiledValue compiled;
	if (compileLineHeightValue(value, compiled)) applyRuntimeLineHeightValue(node, compiled.aux, compiled.lengths[0], source);
	else setStyleValue(node, Property::LineHeight, parseLineHeightForNode(value, node.id()), source);
}

bool applyKnownResolvedPropertyWithSource(NodeHandle node, CssDeclarationId declaration, const std::string &value, StyleApplicationSource source)
{
	if (!node) return true;
	const int nodeId = node.id();
	const auto placement = backgroundPlacementProperty(declaration);
	if (placement != Property::Count) {
		const int parent = Tree::instance().node(nodeId).parent;
		const int handle = toLowerAscii(trimCssValue(value)) == "inherit"
		    ? (parent >= 0 ? backgroundPlacementHandle(Tree::instance().node(parent).style, placement) : -1)
		    : compileBackgroundPlacement(declaration, value);
		if (handle >= -1) setStyleValue(node, placement, handle, source);
		return true;
	}
	if (isBorderWidthDeclaration(declaration) && toLowerAscii(trimCssValue(value)) == "inherit") {
		const int side = declaration == CssDeclarationId::BorderWidth ? -1 : static_cast<int>(declaration) - static_cast<int>(CssDeclarationId::BorderTopWidth);
		setStyleValue(node, side < 0 ? Property::BorderWidth : borderSideWidthProperty(side), kInheritedBorderWidth, source);
		return true;
	}

	switch (declaration) {
	case CssDeclarationId::BackgroundClip: {
		const int parent = Tree::instance().node(nodeId).parent;
		const int clip = toLowerAscii(trimCssValue(value)) == "inherit"
		    ? (parent >= 0 ? rstyle(Tree::instance().node(parent).style).bg_clip : 0)
		    : compileBackgroundClip(value);
		if (clip >= 0) setStyleValue(node, Property::BackgroundClip, clip, source);
		return true;
	}
	case CssDeclarationId::Contain: {
		const int parent = Tree::instance().node(nodeId).parent;
		const int flags = toLowerAscii(trimCssValue(value)) == "inherit"
		    ? (parent >= 0 ? rstyle(Tree::instance().node(parent).style).containment : 0)
		    : containmentValue(value);
		if (flags >= 0) setStyleValue(node, Property::Containment, flags, source);
		return true;
	}
	case CssDeclarationId::AspectRatio: {
		std::string ratio = toLowerAscii(trimCssValue(value));
		bool natural = false;
		if (ratio == "auto" || ratio == "initial" || ratio == "unset") {
			setStyleValue(node, Property::AspectRatio, 0, source);
			return true;
		}
		if (ratio == "inherit") {
			const int parent = Tree::instance().node(nodeId).parent;
			setStyleValue(node, Property::AspectRatio, parent >= 0 ? rstyle(Tree::instance().node(parent).style).aspect_ratio : 0, source);
			return true;
		}
		const auto words = splitFunctionAwareWords(ratio);
		ratio.clear();
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (words[i] == "auto") {
				if (natural || (i != 0 && i + 1 != words.size())) return true;
				natural = true;
			} else {
				if (!ratio.empty()) ratio += ' ';
				ratio += words[i];
			}
		}
		const auto slash = ratio.find('/');
		const std::string numerator = trimCssValue(ratio.substr(0, slash));
		const std::string denominator = slash == std::string::npos ? "1" : trimCssValue(ratio.substr(slash + 1));
		auto number = [](const std::string &text, double &out) {
			if (text.empty()) return false;
			if (text.find_first_not_of("0123456789+-.eE") != std::string::npos) return false;
			char *end = nullptr;
			out = std::strtod(text.c_str(), &end);
			return end == text.c_str() + text.size() && std::isfinite(out) && out >= 0;
		};
		double a, b;
		if (!number(numerator, a) || !number(denominator, b)) return true;
		float encoded = 0;
		if (a > 0 && b > 0) {
			encoded = static_cast<float>(std::max(1e-37, std::min(1e37, a / b)));
			if (natural) encoded = -encoded;
		}
		int bits = 0;
		static_assert(sizeof(bits) == sizeof(encoded));
		std::memcpy(&bits, &encoded, sizeof(bits));
		setStyleValue(node, Property::AspectRatio, bits, source);
		return true;
	}
	case CssDeclarationId::BoxSizing:
		if (value == "content-box") setStyleValue(node, Property::BoxSizing, 0, source);
		if (value == "border-box") setStyleValue(node, Property::BoxSizing, 1, source);
		return true;
	case CssDeclarationId::Float:
		if (value == "none") setStyleValue(node, Property::Float, 0, source);
		if (value == "left") setStyleValue(node, Property::Float, 1, source);
		if (value == "right") setStyleValue(node, Property::Float, 2, source);
		return true;
	case CssDeclarationId::MarginTrim: {
		const int parent = Tree::instance().node(nodeId).parent;
		const int flags = toLowerAscii(trimCssValue(value)) == "inherit"
		    ? (parent >= 0 ? rstyle(Tree::instance().node(parent).style).margin_trim : 0)
		    : marginTrimValue(value);
		if (flags >= 0) setStyleValue(node, Property::MarginTrim, flags, source);
		return true;
	}
	case CssDeclarationId::Clear:
		if (value == "none") setStyleValue(node, Property::Clear, 0, source);
		if (value == "left") setStyleValue(node, Property::Clear, 1, source);
		if (value == "right") setStyleValue(node, Property::Clear, 2, source);
		if (value == "both") setStyleValue(node, Property::Clear, 3, source);
		return true;
	case CssDeclarationId::Direction:
		if (value == "ltr" || value == "initial") setStyleValue(node, Property::Direction, 0, source);
		if (value == "rtl") setStyleValue(node, Property::Direction, 1, source);
		if (value == "inherit" || value == "unset") setStyleValue(node, Property::Direction, -1, source);
		return true;
	case CssDeclarationId::WritingMode:
		if (value == "horizontal-tb") setStyleValue(node, Property::WritingMode, 0, source);
		if (value == "vertical-lr") setStyleValue(node, Property::WritingMode, 1, source);
		if (value == "vertical-rl") setStyleValue(node, Property::WritingMode, 2, source);
		if (value == "sideways-rl") setStyleValue(node, Property::WritingMode, 3, source);
		if (value == "sideways-lr") setStyleValue(node, Property::WritingMode, 4, source);
		if (value == "inherit") setStyleValue(node, Property::WritingMode, -1, source);
		return true;
	case CssDeclarationId::FlexFlow: {
		int direction = 1;
		bool hasDirection = false;
		std::string wrapWords;
		for (const auto &part : splitFunctionAwareWords(value)) {
			if (part == "row" || part == "column" || part == "row-reverse" || part == "column-reverse") {
				if (hasDirection) return true;
				direction = flexDirectionValue(part); hasDirection = true;
			} else if (part == "wrap" || part == "nowrap" || part == "wrap-reverse" || part == "balance") {
				if (!wrapWords.empty()) wrapWords += ' ';
				wrapWords += part;
			} else return true;
		}
		const int wrap = wrapWords.empty() ? 0 : flexWrapValue(wrapWords);
		if (wrap >= 0 && (hasDirection || !wrapWords.empty())) {
			setStyleValue(node, Property::FlexDirection, direction, source);
			setStyleValue(node, Property::FlexWrap, wrap, source);
		}
		return true;
	}
	case CssDeclarationId::Gap:
	case CssDeclarationId::RowGap:
	case CssDeclarationId::ColumnGap: {
		const auto parts = splitFunctionAwareWords(value);
		if (parts.empty() || parts.size() > (declaration == CssDeclarationId::Gap ? 2u : 1u)) return true;
		CssLengthSpec specs[2];
		for (std::size_t i = 0; i < parts.size(); ++i) {
			if (parts[i] == "normal") specs[i] = CssLengthSpec{0, CssLengthUnit::Px};
			else if (!parseCompiledLengthSpec(parts[i], specs[i]) || specs[i].value < 0) return true;
		}
		if (parts.size() == 1) specs[1] = specs[0];
		if (declaration == CssDeclarationId::Gap)
			setStyleValue(node, Property::Gap, specs[0].unit == CssLengthUnit::Percent ? 0 : resolveCompiledLengthForNode(specs[0], nodeId, LengthAxis::Vertical), source);
		for (int axis = 0; axis < 2; ++axis) {
			if (declaration == CssDeclarationId::RowGap && axis != 0) continue;
			if (declaration == CssDeclarationId::ColumnGap && axis != 1) continue;
			const auto &spec = specs[declaration == CssDeclarationId::Gap ? axis : 0];
			const bool percent = spec.unit == CssLengthUnit::Percent;
			setStyleValue(node, axis == 0 ? Property::RowGap : Property::ColumnGap,
			              percent ? 0 : resolveCompiledLengthForNode(spec, nodeId, axis == 0 ? LengthAxis::Vertical : LengthAxis::Horizontal), source);
			setStyleValue(node, axis == 0 ? Property::RowGapPercent : Property::ColumnGapPercent,
			              percent ? roundToInt(spec.value * 10.0) : kUnset, source);
		}
		return true;
	}
	case CssDeclarationId::Order: {
		int order = 0;
		if (parseOrder(value, order)) setStyleValue(node, Property::Order, order, source);
		return true;
	}
	case CssDeclarationId::Display:
		setStyleValue(node, Property::Display, displayValue(value), source);
		return true;
	case CssDeclarationId::FlexDirection:
		setStyleValue(node, Property::FlexDirection, flexDirectionValue(value), source);
		return true;
	case CssDeclarationId::FlexWrap:
		if (value == "inherit") {
			const int parent = Tree::instance().node(nodeId).parent;
			setStyleValue(node, Property::FlexWrap, parent >= 0 ? Tree::instance().node(parent).style.flex_wrap : 0, source);
		} else if (flexWrapValue(value) >= 0) setStyleValue(node, Property::FlexWrap, flexWrapValue(value), source);
		return true;
	case CssDeclarationId::FlexLineCount: {
		int count = 1;
		if (value == "initial" || value == "unset") setStyleValue(node, Property::FlexLineCount, 1, source);
		else if (value == "inherit") {
			const int parent = Tree::instance().node(nodeId).parent;
			setStyleValue(node, Property::FlexLineCount, parent >= 0 ? rstyle(Tree::instance().node(parent).style).flex_line_count : 1, source);
		} else if (parseOrder(value, count) && count >= 1) setStyleValue(node, Property::FlexLineCount, count, source);
		return true;
	}
	case CssDeclarationId::JustifyContent:
		if (flexAlignValue(value) < 0) return true;
		setStyleValue(node, Property::JustifyContent, flexAlignValue(value), source);
		return true;
	case CssDeclarationId::AlignItems:
		if (selfAlignValue(value, false) < 0) return true;
		setStyleValue(node, Property::AlignItems, selfAlignValue(value, false), source);
		return true;
	case CssDeclarationId::JustifyItems:
		if (selfAlignValue(value, true) < 0) return true;
		setStyleValue(node, Property::JustifyItems, selfAlignValue(value, true), source);
		return true;
	case CssDeclarationId::AlignContent:
		if (flexAlignValue(value) < 0) return true;
		setStyleValue(node, Property::AlignContent, flexAlignValue(value), source);
		return true;
	case CssDeclarationId::JustifySelf:
		if (justifySelfValue(value) < -1) return true;
		setStyleValue(node, Property::JustifySelf, justifySelfValue(value), source);
		return true;
	case CssDeclarationId::AlignSelf:
		if (alignSelfValue(value) < -1) return true;
		setStyleValue(node, Property::AlignSelf, alignSelfValue(value), source);
		return true;
	case CssDeclarationId::PlaceItems:
	case CssDeclarationId::PlaceContent:
	case CssDeclarationId::PlaceSelf: {
		Property alignProperty, justifyProperty;
		alignmentShorthandProperties(declaration, alignProperty, justifyProperty);
		int align, justify;
		if (toLowerAscii(trimCssValue(value)) == "inherit") {
			const int parent = Tree::instance().node(nodeId).parent;
			if (parent < 0) {
				align = justify = declaration == CssDeclarationId::PlaceSelf ? -1 : 0;
			} else {
				const auto &inherited = Tree::instance().node(parent).style;
				align = declaration == CssDeclarationId::PlaceItems ? inherited.align_items : declaration == CssDeclarationId::PlaceContent ? inherited.align_content : inherited.align_self;
				justify = declaration == CssDeclarationId::PlaceItems ? inherited.justify_items : declaration == CssDeclarationId::PlaceContent ? inherited.justify_content : rstyle(inherited).justify_self;
			}
		} else if (!parseAlignmentShorthand(declaration, value, align, justify)) return true;
		setStyleValue(node, alignProperty, align, source);
		setStyleValue(node, justifyProperty, justify, source);
		return true;
	}
	case CssDeclarationId::GridRowStart:
	case CssDeclarationId::GridColumnStart:
	case CssDeclarationId::GridRowEnd:
	case CssDeclarationId::GridColumnEnd:
	{
		int line;
		if (parseGridLine(value, line)) setStyleValue(node, static_cast<Property>(static_cast<int>(Property::GridRowStart) + static_cast<int>(declaration) - static_cast<int>(CssDeclarationId::GridRowStart)), line, source);
		return true;
	}
	case CssDeclarationId::GridRow:
	case CssDeclarationId::GridColumn:
	case CssDeclarationId::GridArea: {
		const auto parts = splitTopLevel(value, '/');
		const bool area = declaration == CssDeclarationId::GridArea;
		if (parts.empty() || parts.size() > (area ? 4u : 2u)) return true;
		int lines[4] = {};
		for (std::size_t i = 0; i < parts.size(); ++i) if (!parseGridLine(parts[i], lines[i])) return true;
		if (area) {
			for (int i = 0; i < 4; ++i) setStyleValue(node, static_cast<Property>(static_cast<int>(Property::GridRowStart) + i), lines[i], source);
		} else {
			const bool column = declaration == CssDeclarationId::GridColumn;
			setStyleValue(node, column ? Property::GridColumnStart : Property::GridRowStart, lines[0], source);
			setStyleValue(node, column ? Property::GridColumnEnd : Property::GridRowEnd, lines[1], source);
		}
		return true;
	}
	case CssDeclarationId::Grid:
	case CssDeclarationId::GridTemplate: {
		std::string rows, columns;
		if (!splitGridTemplate(value, rows, columns)) return true;
		applyGridTemplateValue(node, rows, false);
		applyGridTemplateValue(node, columns, true);
		return true;
	}
	case CssDeclarationId::GridTemplateColumns:
		applyGridTemplateValue(node, value, true);
		return true;
	case CssDeclarationId::GridTemplateRows:
		applyGridTemplateValue(node, value, false);
		return true;
	case CssDeclarationId::Ignored:
	case CssDeclarationId::Content:
	case CssDeclarationId::Animation:
		return true;
	case CssDeclarationId::Width:
		setSizeValue(node, Property::Width, Property::WidthPercent, value, LengthAxis::Horizontal, source);
		return true;
	case CssDeclarationId::Height:
		setSizeValue(node, Property::Height, Property::HeightPercent, value, LengthAxis::Vertical, source);
		return true;
	case CssDeclarationId::MinWidth:
		setStyleValue(node, Property::MinWidth, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::MinHeight:
		setStyleValue(node, Property::MinHeight, parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::MaxWidth:
		setStyleValue(node, Property::MaxWidth, value == "none" ? kUnset : parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::MaxHeight:
		setStyleValue(node, Property::MaxHeight, value == "none" ? kUnset : parseLengthForNode(value, nodeId, LengthAxis::Vertical), source);
		return true;
	case CssDeclarationId::Flex: {
		CssCompiledValue compiled;
		if (!compileFlexShorthandValue(value, compiled)) return true;
		setStyleValue(node, Property::Flex, compiled.values[0], source);
		setStyleValue(node, Property::FlexShrink, compiled.values[1], source);
		setFlexBasisValue(node, compiled.aux != 0, compiled.lengths[0], source);
		return true;
	}
	case CssDeclarationId::FlexGrow:
		setStyleValue(node, Property::Flex, rawNumber(std::strtod(value.c_str(), nullptr)), source);
		return true;
	case CssDeclarationId::FlexShrink:
		setStyleValue(node, Property::FlexShrink, rawNumber(std::strtod(value.c_str(), nullptr)), source);
		return true;
	case CssDeclarationId::FlexBasis: {
		CssCompiledValue compiled;
		if (compileFlexBasisValue(value, compiled)) setFlexBasisValue(node, compiled.aux != 0, compiled.lengths[0], source);
		return true;
	}
	case CssDeclarationId::Padding:
	case CssDeclarationId::PaddingTop:
	case CssDeclarationId::PaddingRight:
	case CssDeclarationId::PaddingBottom:
	case CssDeclarationId::PaddingLeft:
	case CssDeclarationId::Margin:
	case CssDeclarationId::MarginTop:
	case CssDeclarationId::MarginRight:
	case CssDeclarationId::MarginBottom:
	case CssDeclarationId::MarginLeft: {
		const bool padding = declaration >= CssDeclarationId::Padding && declaration <= CssDeclarationId::PaddingLeft;
		const bool all = declaration == CssDeclarationId::Padding || declaration == CssDeclarationId::Margin;
		const auto parts = splitFunctionAwareWords(value);
		if (parts.empty() || parts.size() > (all ? 4u : 1u)) return true;
		CssLengthSpec lengths[4];
		for (std::size_t i = 0; i < parts.size(); ++i)
			if (!parseCompiledLengthSpec(parts[i], lengths[i], !padding)) return true;
		for (int side = 0; side < 4; ++side) {
			if (!all && side != static_cast<int>(declaration) - static_cast<int>(padding ? CssDeclarationId::PaddingTop : CssDeclarationId::MarginTop)) continue;
			const int index = all ? (side == 0 ? 0 : side == 1 ? (parts.size() > 1 ? 1 : 0) : side == 2 ? (parts.size() > 2 ? 2 : 0) : (parts.size() > 3 ? 3 : parts.size() > 1 ? 1 : 0)) : 0;
			setBoxLengthValue(node, padding, side, lengths[index], source);
		}
		return true;
	}
	case CssDeclarationId::Position:
		setStyleValue(node, Property::Position, positionValue(value), source);
		return true;
	case CssDeclarationId::Inset: {
		const BoxLengths box = parseBoxLengths(value, nodeId);
		setStyleValue(node, Property::Top, box.top, source);
		setStyleValue(node, Property::Right, box.right, source);
		setStyleValue(node, Property::Bottom, box.bottom, source);
		setStyleValue(node, Property::Left, box.left, source);
		return true;
	}
	case CssDeclarationId::Top:
		setPositionOffsetValue(node, Property::Top, Property::TopPercent, value, LengthAxis::Vertical, source);
		return true;
	case CssDeclarationId::Right:
		setPositionOffsetValue(node, Property::Right, Property::RightPercent, value, LengthAxis::Horizontal, source);
		return true;
	case CssDeclarationId::Bottom:
		setPositionOffsetValue(node, Property::Bottom, Property::BottomPercent, value, LengthAxis::Vertical, source);
		return true;
	case CssDeclarationId::Left:
		setPositionOffsetValue(node, Property::Left, Property::LeftPercent, value, LengthAxis::Horizontal, source);
		return true;
	case CssDeclarationId::ZIndex: {
		int level;
		if (parseZIndex(value, level)) setStyleValue(node, Property::ZIndex, level, source);
		return true;
	}
	case CssDeclarationId::ActiveBackgroundColor:
		setStyleValue(node, Property::ActiveBackgroundColor, parseColorStyleValue(value), source);
		setStyleValue(node, Property::HasActiveBackground, 1, source);
		return true;
	case CssDeclarationId::BackgroundColor: {
		const auto lower = toLowerAscii(trimCssValue(value));
		const ParsedCssColor color = parseCssColor(lower == "initial" || lower == "unset" ? "transparent" : value);
		if (color.valid) {
			setStyleValue(node, Property::BackgroundColor, cssColorStyleValue(color), source);
			setStyleValue(node, Property::BackgroundAlpha, color.a, source);
			setStyleValue(node, Property::HasBackground, 1, source);
		}
		return true;
	}
	case CssDeclarationId::BackgroundImage:
		applyBackgroundValue(node, value, source, false);
		return true;
	case CssDeclarationId::Background:
		applyBackgroundValue(node, value, source);
		return true;
	case CssDeclarationId::BackgroundSize:
		applyBackgroundSizeValue(node, value);
		return true;
	case CssDeclarationId::BackgroundPosition:
	case CssDeclarationId::BackgroundRepeat:
	case CssDeclarationId::BackgroundAttachment:
	case CssDeclarationId::BackgroundOrigin:
		return false; // Placement declarations are handled before this switch.
	case CssDeclarationId::ObjectFit:
		setStyleValue(node, Property::ImageFit, imageFitValue(value), source);
		return true;
	case CssDeclarationId::Color:
		applyTextColorValue(node, value, source);
		return true;
	case CssDeclarationId::Opacity:
		setStyleValue(node, Property::Opacity, parseOpacity(value), source);
		return true;
	case CssDeclarationId::BorderColor:
		applyBorderColorValue(node, value, source);
		return true;
	case CssDeclarationId::Border:
		applyBorderShorthand(node, value, source);
		return true;
	case CssDeclarationId::BorderWidth: {
		CssCompiledValue compiled;
		if (compileBorderWidthBox(value, compiled)) applyBorderWidthBox(node, compiled, source);
		return true;
	}
	case CssDeclarationId::BorderTop:
		applyBorderSideShorthand(node, 0, value, source);
		return true;
	case CssDeclarationId::BorderRight:
		applyBorderSideShorthand(node, 1, value, source);
		return true;
	case CssDeclarationId::BorderBottom:
		applyBorderSideShorthand(node, 2, value, source);
		return true;
	case CssDeclarationId::BorderLeft:
		applyBorderSideShorthand(node, 3, value, source);
		return true;
	case CssDeclarationId::BorderTopWidth:
		if (const int width = parseBorderWidth(value, nodeId); width >= 0) setStyleValue(node, Property::BorderTopWidth, width, source);
		return true;
	case CssDeclarationId::BorderRightWidth:
		if (const int width = parseBorderWidth(value, nodeId); width >= 0) setStyleValue(node, Property::BorderRightWidth, width, source);
		return true;
	case CssDeclarationId::BorderBottomWidth:
		if (const int width = parseBorderWidth(value, nodeId); width >= 0) setStyleValue(node, Property::BorderBottomWidth, width, source);
		return true;
	case CssDeclarationId::BorderLeftWidth:
		if (const int width = parseBorderWidth(value, nodeId); width >= 0) setStyleValue(node, Property::BorderLeftWidth, width, source);
		return true;
	case CssDeclarationId::BorderTopColor:
		applyBorderSideColorValue(node, 0, value, source);
		return true;
	case CssDeclarationId::BorderRightColor:
		applyBorderSideColorValue(node, 1, value, source);
		return true;
	case CssDeclarationId::BorderBottomColor:
		applyBorderSideColorValue(node, 2, value, source);
		return true;
	case CssDeclarationId::BorderLeftColor:
		applyBorderSideColorValue(node, 3, value, source);
		return true;
	case CssDeclarationId::BorderRadius:
		applyBorderRadiusValue(node, value, source);
		return true;
	case CssDeclarationId::BorderTopLeftRadius:
		setBorderRadiusCornerValue(node, 0, value, source);
		return true;
	case CssDeclarationId::BorderTopRightRadius:
		setBorderRadiusCornerValue(node, 1, value, source);
		return true;
	case CssDeclarationId::BorderBottomRightRadius:
		setBorderRadiusCornerValue(node, 2, value, source);
		return true;
	case CssDeclarationId::BorderBottomLeftRadius:
		setBorderRadiusCornerValue(node, 3, value, source);
		return true;
	case CssDeclarationId::FontFamily: {
		setStyleValue(node, Property::FontId, fontFamilyValue(value), source);
		return true;
	}
	case CssDeclarationId::Font: {
		ParsedFontShorthand font;
		if (!parseFontShorthand(value, font)) return true;
		setStyleValue(node, Property::FontWeight, font.weight, source);
		setStyleValue(node, Property::FontId, fontFamilyValue(font.family), source);
		setStyleValue(node, Property::FontSize, fontSizeValue(font.size, nodeId), source);
		setAuthoredLineHeightValue(node, font.lineHeight, source);
		return true;
	}
	case CssDeclarationId::FontSize:
		setStyleValue(node, Property::FontSize, fontSizeValue(value, nodeId), source);
		return true;
	case CssDeclarationId::FontWeight:
		setStyleValue(node, Property::FontWeight, fontWeightValue(value), source);
		return true;
	case CssDeclarationId::LineHeight:
		setAuthoredLineHeightValue(node, value, source);
		return true;
	case CssDeclarationId::TextAlign:
		setStyleValue(node, Property::TextAlign, textAlignValue(value), source);
		return true;
	case CssDeclarationId::TextDecoration:
		setStyleValue(node, Property::TextDecoration, textDecorationValue(value), source);
		return true;
	case CssDeclarationId::TextTransform:
		setStyleValue(node, Property::TextTransform, textTransformValue(value), source);
		return true;
	case CssDeclarationId::WhiteSpace:
		setStyleValue(node, Property::WhiteSpace, whiteSpaceValue(value), source);
		return true;
	case CssDeclarationId::TextOverflow:
		setStyleValue(node, Property::TextOverflow, textOverflowValue(value), source);
		return true;
	case CssDeclarationId::TransformStyle: {
		const auto lower = toLowerAscii(trimCssValue(value));
		if (lower == "flat" || lower == "preserve-3d" || lower == "initial" || lower == "unset")
			setStyleValue(node, Property::TransformStyle, lower == "preserve-3d", source);
		return true;
	}
	case CssDeclarationId::Visibility: {
		const int visibility = visibilityValue(value);
		if (visibility >= 0) setStyleValue(node, Property::Visibility, visibility, source);
		return true;
	}
	case CssDeclarationId::BackfaceVisibility:
		setStyleValue(node, Property::Backface, backfaceValue(value), source);
		return true;
	case CssDeclarationId::PointerEvents:
		setStyleValue(node, Property::PointerEvents, pointerEventsValue(value), source);
		return true;
	case CssDeclarationId::Overflow: {
		const auto raw = trimCssValue(value);
		const auto split = raw.find_first_of(" \t\r\n\f");
		const int x = overflowValue(raw.substr(0, split));
		const int y = split == std::string::npos ? x : overflowValue(raw.substr(split));
		if (x < 0 || y < 0) return true;
		setStyleValue(node, Property::OverflowX, x, source);
		setStyleValue(node, Property::OverflowY, y, source);
		return true;
	}
	case CssDeclarationId::OverflowX:
	case CssDeclarationId::OverflowY: {
		const int parsed = overflowValue(value);
		if (parsed >= 0) setStyleValue(node, declaration == CssDeclarationId::OverflowX ? Property::OverflowX : Property::OverflowY, parsed, source);
		return true;
	}
	case CssDeclarationId::MaskImage:
		setStyleValue(node, Property::MaskRightFadeWidth, parseRightFadeMaskWidth(value, nodeId), source);
		return true;
	case CssDeclarationId::Translate: {
		std::string function;
		if (individualTranslateFunction(value, function)) setIndividualTranslation(node, parseTransformComponents(function, nodeId), source);
		return true;
	}
	case CssDeclarationId::Transform:
		setTransformComponents(node, parseTransformComponents(value, nodeId), source);
		return true;
	case CssDeclarationId::Rotate: {
		IndividualRotation rotation;
		if (parseIndividualRotation(value, rotation))
			setIndividualRotation(node, rotation, toLowerAscii(trimCssValue(value)) != "none", source);
		return true;
	}
	case CssDeclarationId::Scale: {
		int scale[3];
		if (parseIndividualScale(value, scale))
			setIndividualScale(node, scale, toLowerAscii(trimCssValue(value)) != "none", source);
		return true;
	}
	case CssDeclarationId::Filter:
		setStyleValue(node, Property::FilterPresent, toLowerAscii(value).find("blur(") != std::string::npos, source);
		setStyleValue(node, Property::FilterBlur, parseFilterBlurRadius(value, nodeId), source);
		return true;
	case CssDeclarationId::BoxShadow:
		applyBoxShadowValue(node, value, source);
		return true;
	case CssDeclarationId::TransformOrigin: {
		const auto parts = splitWords(value);
		setStyleValue(node, Property::TransformOriginX, parseOriginPart(parts.empty() ? "" : parts[0], 500), source);
		setStyleValue(node, Property::TransformOriginY, parseOriginPart(parts.size() < 2 ? "" : parts[1], 500), source);
		return true;
	}
	case CssDeclarationId::Perspective:
		setStyleValue(node, Property::Perspective, parseLengthForNode(value, nodeId, LengthAxis::Horizontal), source);
		return true;
	case CssDeclarationId::PerspectiveOrigin: {
		const auto parts = splitWords(value);
		setStyleValue(node, Property::PerspectiveOriginX, parseOriginPart(parts.empty() ? "" : parts[0], 500), source);
		setStyleValue(node, Property::PerspectiveOriginY, parseOriginPart(parts.size() < 2 ? "" : parts[1], 500), source);
		return true;
	}
	case CssDeclarationId::Unknown:
	case CssDeclarationId::Custom:
		return false;
	}
	return false;
}

const CssCompiledValue *compiledCssValueForHandle(std::uint16_t handle)
{
	const auto &list = compiledCssValues();
	return handle < list.size() ? &list[handle] : nullptr;
}

const CssCompiledBackground *compiledCssBackgroundForHandle(std::uint16_t handle)
{
	const auto &list = compiledCssBackgrounds();
	return handle < list.size() ? &list[handle] : nullptr;
}

const CssCompiledGridTemplate *compiledCssGridTemplateForHandle(std::uint16_t handle)
{
	const auto &list = compiledCssGridTemplates();
	return handle < list.size() ? &list[handle] : nullptr;
}

int percentPermille(const CssLengthSpec &length)
{
	return roundToInt(static_cast<double>(length.value) * 10.0);
}

void setCompiledSizeValue(NodeHandle node,
                          Property lengthProperty,
                          Property percentProperty,
                          const CssLengthSpec &length,
                          LengthAxis axis,
                          StyleApplicationSource source)
{
	if (length.unit == CssLengthUnit::Expression && lengthNeedsLayout(length, node.id()) &&
	    !resolveCompiledLengthForNodeDetailed(length, node.id(), axis).isPercent) {
		setStyleValue(node, lengthProperty == Property::Width ? Property::WidthExpression : Property::HeightExpression,
		              static_cast<int>(length.value), source);
		return;
	}
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, node.id(), axis);
	if (resolved.isAuto) {
		setStyleValue(node, lengthProperty, kUnset, source);
		setStyleValue(node, percentProperty, kUnset, source);
		return;
	}
	if (resolved.isPercent)
		setStyleValue(node, percentProperty, roundToInt(resolved.value), source);
	else
		setStyleValue(node, lengthProperty, roundToInt(resolved.value), source);
}

void setCompiledPositionOffsetValue(NodeHandle node,
                                    Property lengthProperty,
                                    Property percentProperty,
                                    const CssLengthSpec &length,
                                    LengthAxis axis,
                                    StyleApplicationSource source)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, node.id(), axis);
	if (resolved.isPercent)
		setStyleValue(node, percentProperty, roundToInt(resolved.value), source);
	else
		setStyleValue(node, lengthProperty, roundToInt(resolved.value), source);
}

void setCompiledBorderRadiusCornerValue(NodeHandle node,
                                        int corner,
                                        const CssLengthSpec &length,
                                        StyleApplicationSource source)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, node.id(), LengthAxis::None);
	if (resolved.isPercent)
		setStyleValue(node, borderRadiusPercentProperty(corner), roundToInt(resolved.value), source);
	else
		setStyleValue(node, borderRadiusLengthProperty(corner), roundToInt(resolved.value), source);
}

void resolveCompiledTranslate(const CssLengthSpec &length,
                              int nodeId,
                              LengthAxis axis,
                              int &px,
                              int &percent)
{
	const ResolvedCssLength resolved = resolveCompiledLengthForNodeDetailed(length, nodeId, axis);
	if (resolved.isPercent) {
		px = 0;
		percent = roundToInt(resolved.value);
		return;
	}
	px = roundToInt(resolved.value);
	percent = 0;
}

TransformComponents transformFromCompiled(const CssCompiledValue &compiled, int nodeId)
{
	TransformComponents t;
	t.translateOuterAxes = (compiled.flags >> 10) & 7;
	t.rotateX = compiled.values[0];
	t.rotateY = compiled.values[1];
	t.rotateZ = compiled.values[2];
	t.scaleX = compiled.values[8];
	t.scaleY = compiled.values[9];
	t.scaleZ = compiled.values[10];
	t.hasRotateX = (compiled.flags & (1u << 0)) != 0;
	t.hasRotateY = (compiled.flags & (1u << 1)) != 0;
	t.hasRotateZ = (compiled.flags & (1u << 2)) != 0;
	t.hasTranslateX = (compiled.flags & (1u << 3)) != 0;
	t.hasTranslateY = (compiled.flags & (1u << 4)) != 0;
	t.hasTranslateZ = (compiled.flags & (1u << 5)) != 0;
	t.hasScaleX = (compiled.flags & (1u << 8)) != 0;
	t.hasScaleY = (compiled.flags & (1u << 9)) != 0;
	t.hasScaleZ = (compiled.flags & (1u << 13)) != 0;
	if (t.hasTranslateX)
		resolveCompiledTranslate(compiled.lengths[0], nodeId, LengthAxis::Horizontal, t.translateX, t.translateXPercent);
	if (t.hasTranslateY)
		resolveCompiledTranslate(compiled.lengths[1], nodeId, LengthAxis::Vertical, t.translateY, t.translateYPercent);
	if (t.hasTranslateZ) {
		t.translateZ = resolveCompiledLengthForNode(compiled.lengths[2], nodeId, LengthAxis::Horizontal);
	}
	return t;
}

bool applyCompiledColorValue(NodeHandle node,
                             CssDeclarationId declaration,
                             int styleColor,
                             int nativeColor,
                             int alpha,
                             StyleApplicationSource source)
{
	if (!node) return true;
	const int nodeId = node.id();
	Node &target = treeState().nodes[nodeId];
	switch (declaration) {
	case CssDeclarationId::Color:
		if (source == StyleApplicationSource::ClassRule && target.style.text_color == static_cast<style_color_t>(nativeColor) &&
		    target.style.text_alpha == static_cast<std::uint8_t>(alpha))
			return true;
		setStyleValueKnownTarget(node, target, Property::Color, styleColor, source);
		setStyleValueKnownTarget(node, target, Property::ColorAlpha, alpha, source);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
		case CssDeclarationId::ActiveBackgroundColor:
			setStyleValueKnownTarget(node, target, Property::ActiveBackgroundColor, styleColor, source);
			setStyleValueKnownTarget(node, target, Property::HasActiveBackground, 1, source);
			return true;
	case CssDeclarationId::Background:
	case CssDeclarationId::BackgroundColor:
		if (declaration == CssDeclarationId::Background) {
			setStyleValue(node, Property::BackgroundImage, -1, source);
			setStyleValue(node, Property::BackgroundClip, 0, source);
			for (Property p : {Property::BackgroundSizeList, Property::BackgroundPositionList, Property::BackgroundRepeatList,
			                   Property::BackgroundAttachmentList, Property::BackgroundOriginList}) setStyleValue(node, p, -1, source);
		}
		setStyleValueKnownTarget(node, target, Property::BackgroundColor, styleColor, source);
		setStyleValueKnownTarget(node, target, Property::BackgroundAlpha, alpha, source);
		setStyleValueKnownTarget(node, target, Property::HasBackground, 1, source);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
	case CssDeclarationId::BorderColor:
		if (source == StyleApplicationSource::ClassRule && rstyle(target.style).border_color_flags == 16u && target.style.border_color == static_cast<style_color_t>(nativeColor) &&
		    target.style.border_alpha == static_cast<std::uint8_t>(alpha))
			return true;
		setStyleValueKnownTarget(node, target, Property::BorderColor, styleColor, source);
		setStyleValue(node, Property::BorderAlpha, alpha, source);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
	case CssDeclarationId::BorderTopColor:
	case CssDeclarationId::BorderRightColor:
	case CssDeclarationId::BorderBottomColor:
	case CssDeclarationId::BorderLeftColor: {
			const int side = declaration == CssDeclarationId::BorderTopColor ? 0 :
			    declaration == CssDeclarationId::BorderRightColor ? 1 :
			    declaration == CssDeclarationId::BorderBottomColor ? 2 : 3;
			const RareStyle &current = rstyle(target.style);
			if (source == StyleApplicationSource::ClassRule && (current.border_color_flags & (1u << side)) && !borderColorIsCurrent(target.style, side) && current.border_side_color[side] == static_cast<style_color_t>(nativeColor) &&
			    current.border_side_alpha[side] == static_cast<std::uint8_t>(alpha))
				return true;
			setStyleValueKnownTarget(node, target, borderSideColorProperty(side), styleColor, source);
			setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopAlpha) + side), alpha, source);
			markNodeDisplayCommandsDirtyForStyleApply(nodeId);
			return true;
		}
	default:
		return false;
	}
}

struct ResolvedCompiledCssColor {
	std::int32_t styleColor = 0;
	style_color_t nativeColor = 0;
	std::uint8_t alpha = 255;
};

bool resolveCompiledColorRef(int nodeId,
                             CssAtomId atom,
                             std::uint8_t hasFallback,
                             std::int32_t fallbackStyleColor,
                             style_color_t fallbackNativeColor,
                             std::uint8_t fallbackAlpha,
                             ResolvedCompiledCssColor &out)
{
	if (atom == kInvalidCssAtom) {
		out.styleColor = fallbackStyleColor;
		out.nativeColor = fallbackNativeColor;
		out.alpha = fallbackAlpha;
		return true;
	}
	if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, atom)) {
		if (entry->hasColor()) {
			out.styleColor = entry->colorStyle;
			out.nativeColor = static_cast<style_color_t>(entry->colorNative);
			out.alpha = entry->colorAlpha;
			return true;
		}
		const CachedCssColor color = cachedCssColorForValue(entry->value);
		if (!color.valid) return false;
		out.styleColor = color.styleColor;
		out.nativeColor = static_cast<style_color_t>(color.nativeColor);
		out.alpha = color.alpha;
		return true;
	}
	if (hasFallback == 0) return false;
	out.styleColor = fallbackStyleColor;
	out.nativeColor = fallbackNativeColor;
	out.alpha = fallbackAlpha;
	return true;
}

bool resolveCompiledLinearGradientColors(const CssCompiledLinearGradient &input,
                                         int nodeId,
                                         CssCompiledLinearGradient &out)
{
	out = input;
	ResolvedCompiledCssColor color;
	if (!resolveCompiledColorRef(nodeId,
	                             input.fromColorAtom,
	                             input.fromColorHasFallback,
	                             input.fromStyleColor,
	                             input.fromNativeColor,
	                             input.fromAlpha,
	                             color))
		return false;
	out.fromStyleColor = color.styleColor;
	out.fromNativeColor = color.nativeColor;
	out.fromAlpha = color.alpha;

	if (input.hasMid) {
		if (!resolveCompiledColorRef(nodeId,
		                             input.midColorAtom,
		                             input.midColorHasFallback,
		                             0,
		                             input.midNativeColor,
		                             input.midAlpha,
		                             color))
			return false;
		out.midNativeColor = color.nativeColor;
		out.midAlpha = color.alpha;
	}

	if (!resolveCompiledColorRef(nodeId,
	                             input.toColorAtom,
	                             input.toColorHasFallback,
	                             0,
	                             input.toNativeColor,
	                             input.toAlpha,
	                             color))
		return false;
	out.toNativeColor = color.nativeColor;
	out.toAlpha = color.alpha;
	return true;
}

bool resolveCompiledRadialGradientColors(const CssCompiledRadialGradient &input,
                                         int nodeId,
                                         CssCompiledRadialGradient &out)
{
	out = input;
	ResolvedCompiledCssColor color;
	if (!resolveCompiledColorRef(nodeId,
	                             input.fromColorAtom,
	                             input.fromColorHasFallback,
	                             0,
	                             input.fromNativeColor,
	                             input.fromAlpha,
	                             color))
		return false;
	out.fromNativeColor = color.nativeColor;
	out.fromAlpha = color.alpha;
	if (!resolveCompiledColorRef(nodeId,
	                             input.toColorAtom,
	                             input.toColorHasFallback,
	                             0,
	                             input.toNativeColor,
	                             input.toAlpha,
	                             color))
		return false;
	out.toNativeColor = color.nativeColor;
	out.toAlpha = color.alpha;
	return true;
}

void applyCompiledLinearGradient(RareStyle &rs, const CssCompiledLinearGradient &gradient)
{
	rs.bg_gradient_from_color = gradient.fromNativeColor;
	rs.bg_gradient_mid_color = gradient.midNativeColor;
	rs.bg_gradient_to_color = gradient.toNativeColor;
	rs.bg_gradient_from_alpha = gradient.fromAlpha;
	rs.bg_gradient_mid_alpha = gradient.midAlpha;
	rs.bg_gradient_to_alpha = gradient.toAlpha;
	rs.bg_gradient_mid_stop = gradient.midStopPermille;
	rs.bg_gradient_to_stop = gradient.toStopPermille;
	rs.bg_gradient_has_mid = gradient.hasMid;
	rs.bg_gradient_angle = gradient.angleTenths;
}

void applyCompiledOverlayGradient(RareStyle &rs, const CssCompiledLinearGradient &gradient)
{
	rs.bg_overlay_gradient = 1;
	rs.bg_overlay_gradient_from_color = gradient.fromNativeColor;
	rs.bg_overlay_gradient_mid_color = gradient.midNativeColor;
	rs.bg_overlay_gradient_to_color = gradient.toNativeColor;
	rs.bg_overlay_gradient_from_alpha = gradient.fromAlpha;
	rs.bg_overlay_gradient_mid_alpha = gradient.midAlpha;
	rs.bg_overlay_gradient_to_alpha = gradient.toAlpha;
	rs.bg_overlay_gradient_mid_stop = gradient.midStopPermille;
	rs.bg_overlay_gradient_to_stop = gradient.toStopPermille;
	rs.bg_overlay_gradient_has_mid = gradient.hasMid;
	rs.bg_overlay_gradient_angle = gradient.angleTenths;
}

void applyCompiledRadialGradient(RareStyle &rs, const CssCompiledRadialGradient &gradient)
{
	rs.bg_radial_gradient = 1;
	rs.bg_radial_gradient_from_color = gradient.fromNativeColor;
	rs.bg_radial_gradient_to_color = gradient.toNativeColor;
	rs.bg_radial_gradient_from_alpha = gradient.fromAlpha;
	rs.bg_radial_gradient_to_alpha = gradient.toAlpha;
	rs.bg_radial_gradient_stop = gradient.stopPermille;
	rs.bg_radial_gradient_cx = gradient.cxPermille;
	rs.bg_radial_gradient_cy = gradient.cyPermille;
	rs.bg_radial_gradient_rx = gradient.rxPermille;
	rs.bg_radial_gradient_ry = gradient.ryPermille;
}

bool compiledLinearGradientMatches(const RareStyle &rs, const CssCompiledLinearGradient &gradient)
{
	return rs.bg_gradient_from_color == gradient.fromNativeColor &&
	       rs.bg_gradient_mid_color == gradient.midNativeColor &&
	       rs.bg_gradient_to_color == gradient.toNativeColor &&
	       rs.bg_gradient_from_alpha == gradient.fromAlpha &&
	       rs.bg_gradient_mid_alpha == gradient.midAlpha &&
	       rs.bg_gradient_to_alpha == gradient.toAlpha &&
	       rs.bg_gradient_mid_stop == gradient.midStopPermille &&
	       rs.bg_gradient_to_stop == gradient.toStopPermille &&
	       rs.bg_gradient_has_mid == gradient.hasMid &&
	       rs.bg_gradient_angle == gradient.angleTenths;
}

bool compiledOverlayGradientMatches(const RareStyle &rs,
                                    const CssCompiledLinearGradient &gradient,
                                    bool present)
{
	if (!present) return rs.bg_overlay_gradient == 0;
	return rs.bg_overlay_gradient == 1 &&
	       rs.bg_overlay_gradient_from_color == gradient.fromNativeColor &&
	       rs.bg_overlay_gradient_mid_color == gradient.midNativeColor &&
	       rs.bg_overlay_gradient_to_color == gradient.toNativeColor &&
	       rs.bg_overlay_gradient_from_alpha == gradient.fromAlpha &&
	       rs.bg_overlay_gradient_mid_alpha == gradient.midAlpha &&
	       rs.bg_overlay_gradient_to_alpha == gradient.toAlpha &&
	       rs.bg_overlay_gradient_mid_stop == gradient.midStopPermille &&
	       rs.bg_overlay_gradient_to_stop == gradient.toStopPermille &&
	       rs.bg_overlay_gradient_has_mid == gradient.hasMid &&
	       rs.bg_overlay_gradient_angle == gradient.angleTenths;
}

bool compiledRadialGradientMatches(const RareStyle &rs,
                                   const CssCompiledRadialGradient &gradient,
                                   bool present)
{
	if (!present) return rs.bg_radial_gradient == 0;
	return rs.bg_radial_gradient == 1 &&
	       rs.bg_radial_gradient_from_color == gradient.fromNativeColor &&
	       rs.bg_radial_gradient_to_color == gradient.toNativeColor &&
	       rs.bg_radial_gradient_from_alpha == gradient.fromAlpha &&
	       rs.bg_radial_gradient_to_alpha == gradient.toAlpha &&
	       rs.bg_radial_gradient_stop == gradient.stopPermille &&
	       rs.bg_radial_gradient_cx == gradient.cxPermille &&
	       rs.bg_radial_gradient_cy == gradient.cyPermille &&
	       rs.bg_radial_gradient_rx == gradient.rxPermille &&
	       rs.bg_radial_gradient_ry == gradient.ryPermille;
}

std::uint8_t resolveCompiledGridLineWidth(const CssLengthSpec &length, int nodeId, LengthAxis axis)
{
	const int px = resolveCompiledLengthForNode(length, nodeId, axis);
	if (px <= 0) return 0;
	return static_cast<std::uint8_t>(std::max(1, std::min(px, 255)));
}

bool backgroundImageIsValid(int handle, int nodeId)
{
	if (handle < 0) return true;
	const auto *background = compiledCssBackgroundForHandle(handle);
	if (!background) return false;
	CssCompiledLinearGradient gradient;
	CssCompiledRadialGradient radial;
	return (!background->hasGradient || resolveCompiledLinearGradientColors(background->gradient, nodeId, gradient)) &&
	       (!background->hasOverlayGradient || resolveCompiledLinearGradientColors(background->overlayGradient, nodeId, gradient)) &&
	       (!background->hasRadialGradient || resolveCompiledRadialGradientColors(background->radialGradient, nodeId, radial));
}

bool applyBackgroundImageToStyle(ComputedStyle &style, int handle, int nodeId)
{
	const auto *background = handle >= 0 ? compiledCssBackgroundForHandle(handle) : nullptr;
	if (handle >= 0 && !background) return false;
	CssCompiledLinearGradient gradient, overlay;
	CssCompiledRadialGradient radial;
	if (background) {
		if (background->hasGradient && !resolveCompiledLinearGradientColors(background->gradient, nodeId, gradient)) return false;
		if (background->hasOverlayGradient && !resolveCompiledLinearGradientColors(background->overlayGradient, nodeId, overlay)) return false;
		if (background->hasRadialGradient && !resolveCompiledRadialGradientColors(background->radialGradient, nodeId, radial)) return false;
	}
	std::uint8_t axes = background ? background->gridAxes : 0;
	const std::uint8_t lineX = background && background->hasGridLineX ? resolveCompiledGridLineWidth(background->gridLineX, nodeId, LengthAxis::None) : 0;
	const std::uint8_t lineY = background && background->hasGridLineY ? resolveCompiledGridLineWidth(background->gridLineY, nodeId, LengthAxis::None) : 0;
	if (!lineX) axes &= ~1u;
	if (!lineY) axes &= ~2u;
	const RareStyle &current = rstyle(style);
	const bool hasGradient = background && background->hasGradient;
	if (current.bg_gradient_layer == (background ? background->gradientLayer : 0) &&
	    current.bg_overlay_gradient_layer == (background ? background->overlayLayer : 0) &&
	    current.bg_radial_gradient_layer == (background ? background->radialLayer : 0) &&
	    current.bg_image_layer_count == (background ? background->layerCount : 1) &&
	    style.bg_fill == (hasGradient ? 1 : 0) &&
	    (!hasGradient || compiledLinearGradientMatches(current, gradient)) &&
	    compiledOverlayGradientMatches(current, overlay, background && background->hasOverlayGradient) &&
	    compiledRadialGradientMatches(current, radial, background && background->hasRadialGradient) &&
	    current.bg_grid_axes == axes &&
	    (!axes || (current.bg_grid_color == background->gridColor && current.bg_grid_alpha == background->gridAlpha &&
	               current.bg_grid_line_x == lineX && current.bg_grid_line_y == lineY))) return false;
	RareStyle &rs = rstyleMut(style);
	rs.bg_image_layer_count = background ? background->layerCount : 1;
	rs.bg_gradient_layer = background ? background->gradientLayer : 0;
	rs.bg_overlay_gradient_layer = background ? background->overlayLayer : 0;
	rs.bg_radial_gradient_layer = background ? background->radialLayer : 0;
	style.bg_fill = background && background->hasGradient ? 1 : 0;
	rs.bg_gradient_has_mid = 0;
	rs.bg_overlay_gradient = 0;
	rs.bg_radial_gradient = 0;
	rs.bg_grid_axes = 0;
	rs.bg_grid_color = 0;
	rs.bg_grid_alpha = 255;
	rs.bg_grid_line_x = rs.bg_grid_line_y = 0;
	if (background) {
		if (background->hasGradient) applyCompiledLinearGradient(rs, gradient);
		if (background->hasOverlayGradient) applyCompiledOverlayGradient(rs, overlay);
		if (background->hasRadialGradient) applyCompiledRadialGradient(rs, radial);
		rs.bg_grid_axes = background->gridAxes;
		rs.bg_grid_color = background->gridColor;
		rs.bg_grid_alpha = background->gridAlpha;
		rs.bg_grid_axes = axes;
		rs.bg_grid_line_x = lineX;
		rs.bg_grid_line_y = lineY;
	}
	return true;
}

bool applyCompiledBackgroundValue(NodeHandle node, int handle, CssDeclarationId declaration, StyleApplicationSource source)
{
	if (!compiledCssBackgroundForHandle(handle)) return false;
	applyBackgroundHandle(node, handle, declaration == CssDeclarationId::Background, source);
	return true;
}

bool applyRuntimeFlexValue(NodeHandle node,
                           int grow,
                           int shrink,
                           std::uint8_t hasBasis,
                           const CssLengthSpec &basis,
                           StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	setStyleValue(node, Property::Flex, grow, source);
	setStyleValue(node, Property::FlexShrink, shrink, source);
	setFlexBasisValue(node, hasBasis != 0, basis, source);
	return true;
}

bool applyCompiledFlexValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::Flex) return false;
	return applyRuntimeFlexValue(node,
	                             compiled.values[0],
	                             compiled.values[1],
	                             compiled.aux,
	                             compiled.lengths[0],
	                             source);
}

bool applyRuntimeFlexBasisValue(NodeHandle node,
                                std::uint8_t hasBasis,
                                const CssLengthSpec &basis,
                                StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	setFlexBasisValue(node, hasBasis != 0, basis, source);
	return true;
}

bool applyCompiledFlexBasisValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::FlexBasis) return false;
	return applyRuntimeFlexBasisValue(node, compiled.aux, compiled.lengths[0], source);
}

bool applyRuntimeLengthValue(NodeHandle node,
                             CssDeclarationId declaration,
                             const CssLengthSpec &length,
                             StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	if ((declaration >= CssDeclarationId::MarginTop && declaration <= CssDeclarationId::MarginLeft) ||
	    (declaration >= CssDeclarationId::PaddingTop && declaration <= CssDeclarationId::PaddingLeft)) {
		const bool padding = declaration >= CssDeclarationId::PaddingTop && declaration <= CssDeclarationId::PaddingLeft;
		const int side = static_cast<int>(declaration) - static_cast<int>(padding ? CssDeclarationId::PaddingTop : CssDeclarationId::MarginTop);
		setBoxLengthValue(node, padding, side, length, source);
		return true;
	}
	switch (declaration) {
	case CssDeclarationId::Gap:
		setStyleValue(node, Property::Gap, length.unit == CssLengthUnit::Percent ? 0 : resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source);
		if (length.unit == CssLengthUnit::Percent) {
			setStyleValue(node, Property::RowGapPercent, roundToInt(length.value * 10.0), source);
			setStyleValue(node, Property::ColumnGapPercent, roundToInt(length.value * 10.0), source);
		}
		return true;
	case CssDeclarationId::MinWidth: setStyleValue(node, Property::MinWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MinHeight: setStyleValue(node, Property::MinHeight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::MaxWidth: setStyleValue(node, Property::MaxWidth, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MaxHeight: setStyleValue(node, Property::MaxHeight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::PaddingTop: setStyleValue(node, Property::PaddingTop, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::PaddingRight: setStyleValue(node, Property::PaddingRight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::PaddingBottom: setStyleValue(node, Property::PaddingBottom, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::PaddingLeft: setStyleValue(node, Property::PaddingLeft, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MarginTop: setStyleValue(node, Property::MarginTop, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::MarginRight: setStyleValue(node, Property::MarginRight, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MarginBottom: setStyleValue(node, Property::MarginBottom, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical), source); return true;
	case CssDeclarationId::MarginLeft: setStyleValue(node, Property::MarginLeft, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::BorderWidth: if (const int width = resolveBorderWidth(length, nodeId); width >= 0) setUniformBorderWidth(node, width, source); return true;
	case CssDeclarationId::BorderTopWidth: if (const int width = resolveBorderWidth(length, nodeId); width >= 0) setStyleValue(node, Property::BorderTopWidth, width, source); return true;
	case CssDeclarationId::BorderRightWidth: if (const int width = resolveBorderWidth(length, nodeId); width >= 0) setStyleValue(node, Property::BorderRightWidth, width, source); return true;
	case CssDeclarationId::BorderBottomWidth: if (const int width = resolveBorderWidth(length, nodeId); width >= 0) setStyleValue(node, Property::BorderBottomWidth, width, source); return true;
	case CssDeclarationId::BorderLeftWidth: if (const int width = resolveBorderWidth(length, nodeId); width >= 0) setStyleValue(node, Property::BorderLeftWidth, width, source); return true;
	case CssDeclarationId::FontSize:
		setStyleValue(node, Property::FontSize, resolveFontSizeLength(length, nodeId), source);
		return true;
	case CssDeclarationId::Perspective: setStyleValue(node, Property::Perspective, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal), source); return true;
	case CssDeclarationId::MaskImage:
		setStyleValue(node,
		              Property::MaskRightFadeWidth,
		              std::max(0, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal)),
		              source);
		return true;
	default:
		return false;
	}
}

bool applyCompiledLengthValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::Length) return false;
	return applyRuntimeLengthValue(node, compiled.declaration, compiled.lengths[0], source);
}

bool applyRuntimeSizeValue(NodeHandle node,
                           CssDeclarationId declaration,
                           const CssLengthSpec &length,
                           StyleApplicationSource source)
{
	if (!node) return false;
	if (declaration == CssDeclarationId::Width) {
		setCompiledSizeValue(node, Property::Width, Property::WidthPercent, length, LengthAxis::Horizontal, source);
		return true;
	}
	if (declaration == CssDeclarationId::Height) {
		setCompiledSizeValue(node, Property::Height, Property::HeightPercent, length, LengthAxis::Vertical, source);
		return true;
	}
	return false;
}

bool applyCompiledSizeValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::Size) return false;
	return applyRuntimeSizeValue(node, compiled.declaration, compiled.lengths[0], source);
}

bool applyRuntimePositionOffsetValue(NodeHandle node,
                                     CssDeclarationId declaration,
                                     const CssLengthSpec &length,
                                     StyleApplicationSource source)
{
	if (!node) return false;
	if (declaration == CssDeclarationId::Top) {
		setCompiledPositionOffsetValue(node, Property::Top, Property::TopPercent, length, LengthAxis::Vertical, source);
		return true;
	}
	if (declaration == CssDeclarationId::Right) {
		setCompiledPositionOffsetValue(node, Property::Right, Property::RightPercent, length, LengthAxis::Horizontal, source);
		return true;
	}
	if (declaration == CssDeclarationId::Bottom) {
		setCompiledPositionOffsetValue(node, Property::Bottom, Property::BottomPercent, length, LengthAxis::Vertical, source);
		return true;
	}
	if (declaration == CssDeclarationId::Left) {
		setCompiledPositionOffsetValue(node, Property::Left, Property::LeftPercent, length, LengthAxis::Horizontal, source);
		return true;
	}
	return false;
}

bool applyCompiledPositionOffsetValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::PositionOffset) return false;
	return applyRuntimePositionOffsetValue(node, compiled.declaration, compiled.lengths[0], source);
}

bool applyCompiledBoxValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node || compiled.kind != CssCompiledKind::Box) return false;
	if (compiled.declaration == CssDeclarationId::BorderWidth) return applyBorderWidthBox(node, compiled, source);
	const int nodeId = node.id();
	const BoxLengths box{
	    resolveCompiledLengthForNode(compiled.lengths[0], nodeId, LengthAxis::Vertical),
	    resolveCompiledLengthForNode(compiled.lengths[1], nodeId, LengthAxis::Horizontal),
	    resolveCompiledLengthForNode(compiled.lengths[2], nodeId, LengthAxis::Vertical),
	    resolveCompiledLengthForNode(compiled.lengths[3], nodeId, LengthAxis::Horizontal)};
	if (compiled.declaration == CssDeclarationId::Padding || compiled.declaration == CssDeclarationId::Margin) {
		for (int side = 0; side < 4; ++side)
			setBoxLengthValue(node, compiled.declaration == CssDeclarationId::Padding, side, compiled.lengths[side], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::Inset) {
		setStyleValue(node, Property::Top, box.top, source);
		setStyleValue(node, Property::Right, box.right, source);
		setStyleValue(node, Property::Bottom, box.bottom, source);
		setStyleValue(node, Property::Left, box.left, source);
		return true;
	}
	return false;
}

bool applyRuntimeBackgroundSizeValue(NodeHandle node,
                                     const CssLengthSpec &stepXLength,
                                     const CssLengthSpec &stepYLength)
{
	if (!node) return false;
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	if (rstyle(target.style).bg_grid_axes == 0) return true;
	const int stepX = resolveCompiledLengthForNode(stepXLength, nodeId, LengthAxis::Horizontal);
	const int stepY = resolveCompiledLengthForNode(stepYLength, nodeId, LengthAxis::Vertical);
	RareStyle &rs = rstyleMut(target.style);
	if (stepX > 0) rs.bg_grid_step_x = static_cast<std::uint16_t>(std::min(stepX, 65535));
	if (stepY > 0) rs.bg_grid_step_y = static_cast<std::uint16_t>(std::min(stepY, 65535));
	markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	return true;
}

bool applyCompiledBackgroundSizeValue(NodeHandle node, const CssCompiledValue &compiled)
{
	if (compiled.kind != CssCompiledKind::BackgroundSize) return false;
	return applyRuntimeBackgroundSizeValue(node, compiled.lengths[0], compiled.lengths[1]);
}

bool applyRuntimeBorderShorthandValue(NodeHandle node,
                                      const CssLengthSpec &width,
                                      int color,
                                      int alpha,
                                      int relief,
                                      StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const int snappedWidth = resolveBorderWidth(width, nodeId);
	if (snappedWidth < 0) return true;
	setUniformBorderWidth(node, snappedWidth, source);
	for (int side = 0; side < 4; ++side)
		setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side), relief, source);
	if (alpha >= 0) {
		setStyleValue(node, Property::BorderColor, color, source);
		setStyleValue(node, Property::BorderAlpha, alpha, source);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	} else setStyleValue(node, Property::BorderColorCurrent, 1, source);
	return true;
}

bool applyCompiledBorderShorthandValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::BorderShorthand) return false;
	return applyRuntimeBorderShorthandValue(node,
	                                       compiled.lengths[0],
	                                       compiled.values[0],
	                                       compiled.aux != 0 ? compiled.values[1] : -1,
	                                       compiled.values[2],
	                                       source);
}

bool applyRuntimeBorderSideShorthandValue(NodeHandle node,
                                          CssDeclarationId declaration,
                                          const CssLengthSpec &width,
                                          int color,
                                          int alpha,
                                          int relief,
                                          StyleApplicationSource source)
{
	if (!node) return false;
	const int side = borderSideForDeclaration(declaration);
	if (side < 0) return false;
	const int nodeId = node.id();
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return true;
	Node &target = state.nodes[nodeId];
	const int snappedWidth = resolveBorderWidth(width, nodeId);
	if (snappedWidth < 0) return true;
	setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side), relief, source);
	setStyleValue(node,
	              borderSideWidthProperty(side),
	              snappedWidth,
	              source);
	if (alpha >= 0) {
		setStyleValue(node, borderSideColorProperty(side), color, source);
		setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopAlpha) + side), alpha, source);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
	} else setStyleValue(node, static_cast<Property>(static_cast<int>(Property::BorderTopColorCurrent) + side), 1, source);
	return true;
}

bool applyCompiledBorderSideShorthandValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::BorderSideShorthand) return false;
	return applyRuntimeBorderSideShorthandValue(node,
	                                           compiled.declaration,
	                                           compiled.lengths[0],
	                                           compiled.values[0],
	                                           compiled.aux != 0 ? compiled.values[1] : -1,
	                                           compiled.values[2],
	                                           source);
}

bool applyCompiledBorderRadiusValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node || compiled.kind != CssCompiledKind::BorderRadius) return false;
	if (compiled.declaration == CssDeclarationId::BorderRadius) {
		for (int corner = 0; corner < 4; ++corner)
			setCompiledBorderRadiusCornerValue(node, corner, compiled.lengths[corner], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderTopLeftRadius) {
		setCompiledBorderRadiusCornerValue(node, 0, compiled.lengths[0], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderTopRightRadius) {
		setCompiledBorderRadiusCornerValue(node, 1, compiled.lengths[0], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderBottomRightRadius) {
		setCompiledBorderRadiusCornerValue(node, 2, compiled.lengths[0], source);
		return true;
	}
	if (compiled.declaration == CssDeclarationId::BorderBottomLeftRadius) {
		setCompiledBorderRadiusCornerValue(node, 3, compiled.lengths[0], source);
		return true;
	}
	return false;
}

bool applyRuntimeFilterBlurValue(NodeHandle node,
                                 std::uint8_t hasRadius,
                                 const CssLengthSpec &length,
                                 StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	int radius = hasRadius == 0 ? 0 : resolveCompiledLengthForNode(length, nodeId, LengthAxis::None);
	if (radius < 0) radius = 0;
	if (radius > 64) radius = 64;
	setStyleValue(node, Property::FilterPresent, hasRadius != 0, source);
	setStyleValue(node, Property::FilterBlur, radius, source);
	return true;
}

bool applyCompiledFilterBlurValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::FilterBlur) return false;
	return applyRuntimeFilterBlurValue(node, compiled.aux, compiled.lengths[0], source);
}

bool applyCompiledBoxShadowValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node || compiled.kind != CssCompiledKind::BoxShadow) return false;
	const int nodeId = node.id();
	if (compiled.aux == 0 || compiled.values[1] == 0) {
		setStyleValue(node, Property::BoxShadowInset, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetX, 0, source);
		setStyleValue(node, Property::BoxShadowOffsetY, 0, source);
		setStyleValue(node, Property::BoxShadowBlur, 0, source);
		setStyleValue(node, Property::BoxShadowSpread, 0, source);
		setStyleValue(node, Property::BoxShadowColor, 0, source);
		setStyleValue(node, Property::BoxShadowAlpha, 0, source);
		return true;
	}
	int blur = resolveCompiledLengthForNode(compiled.lengths[2], nodeId, LengthAxis::None);
	if (blur < 0) blur = 0;
	if (blur > 96) blur = 96;
	setStyleValue(node,
	              Property::BoxShadowOffsetX,
	              resolveCompiledLengthForNode(compiled.lengths[0], nodeId, LengthAxis::Horizontal),
	              source);
	setStyleValue(node,
	              Property::BoxShadowOffsetY,
	              resolveCompiledLengthForNode(compiled.lengths[1], nodeId, LengthAxis::Vertical),
	              source);
	setStyleValue(node, Property::BoxShadowBlur, blur, source);
	setStyleValue(node,
	              Property::BoxShadowSpread,
	              resolveCompiledLengthForNode(compiled.lengths[3], nodeId, LengthAxis::None),
	              source);
	setStyleValue(node, Property::BoxShadowColor, compiled.values[0], source);
	setStyleValue(node, Property::BoxShadowAlpha, compiled.values[1], source);
	setStyleValue(node, Property::BoxShadowInset, 1, source);
	return true;
}

bool applyRuntimeLineHeightValue(NodeHandle node,
                                 std::uint8_t kind,
                                 const CssLengthSpec &length,
                                 StyleApplicationSource source)
{
	if (!node) return false;
	const int nodeId = node.id();
	if (kind == 0) {
		setStyleValue(node, Property::LineHeight, 0, source);
		return true;
	}
	if (kind == 1) {
		if (!std::isfinite(length.value) || length.value < 0) return false;
		int bits = 0; if (length.value != 0) std::memcpy(&bits, &length.value, sizeof(bits));
		setStyleValue(node, Property::LineHeightMultiplier, bits, source);
		return true;
	}
	if (kind == 2) {
		// Percentages compute to a length, but authored inline percentages must
		// be reevaluated when this element's font changes before inheritance.
		const int expression = deferredLengthExpression(CssLengthSpec{length.value / 100.0f, CssLengthUnit::Em});
		if (expression >= 0) setStyleValue(node, Property::LineHeightExpression, expression, source);
		return true;
	}
	if (kind == 3) {
		if (length.unit == CssLengthUnit::Expression || lengthDependsOnFont(length, nodeId)) {
			const int expression = deferredLengthExpression(length);
			if (expression >= 0) setStyleValue(node, Property::LineHeightExpression, expression, source);
			return true;
		}
		setStyleValue(node,
		              Property::LineHeight,
		              resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical),
		              source);
		return true;
	}
	return false;
}

bool applyCompiledLineHeightValue(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (compiled.kind != CssCompiledKind::LineHeight) return false;
	return applyRuntimeLineHeightValue(node, compiled.aux, compiled.lengths[0], source);
}

bool applyCompiledCssValueWithSource(NodeHandle node, const CssCompiledValue &compiled, StyleApplicationSource source)
{
	if (!node) return true;
	const int nodeId = node.id();
	switch (compiled.kind) {
	case CssCompiledKind::Noop:
		return true;
		case CssCompiledKind::DirectProperty: {
			const int propertyIndex = compiled.values[0];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			setStyleValue(node, static_cast<Property>(propertyIndex), compiled.values[1], source);
			return true;
		}
		case CssCompiledKind::DirectPropertyGroup: {
			int count = compiled.values[0];
			if (count < 0) count = 0;
			if (count > 4) count = 4;
			for (int i = 0; i < count; ++i) {
				const int propertyIndex = compiled.values[1 + i * 2];
				if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
				setStyleValue(node, static_cast<Property>(propertyIndex), compiled.values[2 + i * 2], source);
			}
			return true;
		}
		case CssCompiledKind::Keyword:
		switch (compiled.declaration) {
		case CssDeclarationId::Display: setStyleValue(node, Property::Display, compiled.values[0], source); return true;
		case CssDeclarationId::ObjectFit: setStyleValue(node, Property::ImageFit, compiled.values[0], source); return true;
		case CssDeclarationId::FlexDirection: setStyleValue(node, Property::FlexDirection, compiled.values[0], source); return true;
		case CssDeclarationId::FlexWrap: setStyleValue(node, Property::FlexWrap, compiled.values[0], source); return true;
		case CssDeclarationId::JustifyContent: setStyleValue(node, Property::JustifyContent, compiled.values[0], source); return true;
		case CssDeclarationId::AlignItems: setStyleValue(node, Property::AlignItems, compiled.values[0], source); return true;
		case CssDeclarationId::JustifyItems: setStyleValue(node, Property::JustifyItems, compiled.values[0], source); return true;
		case CssDeclarationId::AlignContent: setStyleValue(node, Property::AlignContent, compiled.values[0], source); return true;
		case CssDeclarationId::JustifySelf: setStyleValue(node, Property::JustifySelf, compiled.values[0], source); return true;
		case CssDeclarationId::GridRowStart: setStyleValue(node, Property::GridRowStart, compiled.values[0], source); return true;
		case CssDeclarationId::GridColumnStart: setStyleValue(node, Property::GridColumnStart, compiled.values[0], source); return true;
		case CssDeclarationId::GridRowEnd: setStyleValue(node, Property::GridRowEnd, compiled.values[0], source); return true;
		case CssDeclarationId::GridColumnEnd: setStyleValue(node, Property::GridColumnEnd, compiled.values[0], source); return true;
		case CssDeclarationId::AlignSelf: setStyleValue(node, Property::AlignSelf, compiled.values[0], source); return true;
		case CssDeclarationId::Position: setStyleValue(node, Property::Position, compiled.values[0], source); return true;
		case CssDeclarationId::TextAlign: setStyleValue(node, Property::TextAlign, compiled.values[0], source); return true;
		case CssDeclarationId::TextDecoration: setStyleValue(node, Property::TextDecoration, compiled.values[0], source); return true;
		case CssDeclarationId::TextTransform: setStyleValue(node, Property::TextTransform, compiled.values[0], source); return true;
		case CssDeclarationId::WhiteSpace: setStyleValue(node, Property::WhiteSpace, compiled.values[0], source); return true;
		case CssDeclarationId::TextOverflow: setStyleValue(node, Property::TextOverflow, compiled.values[0], source); return true;
		case CssDeclarationId::TransformStyle: setStyleValue(node, Property::TransformStyle, compiled.values[0], source); return true;
		case CssDeclarationId::Visibility: setStyleValue(node, Property::Visibility, compiled.values[0], source); return true;
		case CssDeclarationId::BackfaceVisibility: setStyleValue(node, Property::Backface, compiled.values[0], source); return true;
		case CssDeclarationId::PointerEvents: setStyleValue(node, Property::PointerEvents, compiled.values[0], source); return true;
		case CssDeclarationId::Overflow: setStyleValue(node, Property::Overflow, compiled.values[0], source); return true;
		case CssDeclarationId::OverflowX: setStyleValue(node, Property::OverflowX, compiled.values[0], source); return true;
		case CssDeclarationId::OverflowY: setStyleValue(node, Property::OverflowY, compiled.values[0], source); return true;
		case CssDeclarationId::FontFamily: setStyleValue(node, Property::FontId, compiled.values[0], source); return true;
		case CssDeclarationId::FontWeight: setStyleValue(node, Property::FontWeight, compiled.values[0], source); return true;
		default: return false;
		}
	case CssCompiledKind::Opacity:
		setStyleValue(node, Property::Opacity, compiled.values[0], source);
		return true;
	case CssCompiledKind::Number:
		if (compiled.declaration == CssDeclarationId::ZIndex) {
			setStyleValue(node, Property::ZIndex, compiled.values[0], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexGrow) {
			setStyleValue(node, Property::Flex, compiled.values[0], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexShrink) {
			setStyleValue(node, Property::FlexShrink, compiled.values[0], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FontWeight) {
			setStyleValue(node, Property::FontWeight, compiled.values[0], source);
			return true;
		}
		return false;
	case CssCompiledKind::Flex:
		return applyCompiledFlexValue(node, compiled, source);
	case CssCompiledKind::FlexBasis:
		return applyCompiledFlexBasisValue(node, compiled, source);
	case CssCompiledKind::Length:
		return applyCompiledLengthValue(node, compiled, source);
	case CssCompiledKind::Size:
		return applyCompiledSizeValue(node, compiled, source);
	case CssCompiledKind::PositionOffset:
		return applyCompiledPositionOffsetValue(node, compiled, source);
	case CssCompiledKind::Box:
		return applyCompiledBoxValue(node, compiled, source);
	case CssCompiledKind::Color:
		return applyCompiledColorValue(node,
		                               compiled.declaration,
		                               compiled.values[0],
		                               compiled.values[1],
		                               compiled.values[2],
		                               source);
	case CssCompiledKind::ColorVar: {
		const CssAtomId atom = static_cast<CssAtomId>(compiled.values[0]);
		if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(nodeId, atom)) {
			CachedCssColor color;
			if (entry->hasColor()) {
				color.styleColor = entry->colorStyle;
				color.nativeColor = entry->colorNative;
				color.alpha = entry->colorAlpha;
				color.valid = true;
			} else {
				color = cachedCssColorForValue(entry->value);
			}
			if (color.valid)
				return applyCompiledColorValue(node,
				                               compiled.declaration,
				                               color.styleColor,
				                               color.nativeColor,
				                               color.alpha,
				                               source);
			return false;
		}
		if (compiled.aux == 0) return false;
		return applyCompiledColorValue(node,
		                               compiled.declaration,
		                               compiled.values[1],
		                               compiled.values[2],
		                               compiled.values[3],
		                               source);
	}
	case CssCompiledKind::Rotate: {
		const IndividualRotation rotation{compiled.values[0], compiled.values[1], compiled.values[2], compiled.values[3]};
		setIndividualRotation(node, rotation, compiled.aux, source); return true;
	}
	case CssCompiledKind::Scale: {
		const int scale[3] = {compiled.values[0], compiled.values[1], compiled.values[2]};
		setIndividualScale(node, scale, compiled.aux, source); return true;
	}
	case CssCompiledKind::OriginPair:
		if (compiled.declaration == CssDeclarationId::TransformOrigin) {
			setStyleValue(node, Property::TransformOriginX, compiled.values[0], source);
			setStyleValue(node, Property::TransformOriginY, compiled.values[1], source);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::PerspectiveOrigin) {
			setStyleValue(node, Property::PerspectiveOriginX, compiled.values[0], source);
			setStyleValue(node, Property::PerspectiveOriginY, compiled.values[1], source);
			return true;
		}
		return false;
	case CssCompiledKind::Transform: {
		const TransformComponents transform = transformFromCompiled(compiled, nodeId);
		if (compiled.declaration == CssDeclarationId::Translate) { setIndividualTranslation(node, transform, source); return true; }
		if (!applyTransformComponentsFast(node, transform, source))
			setTransformComponents(node, transform, source);
		return true;
	}
	case CssCompiledKind::Background: {
		const CssCompiledBackground *background = compiledCssBackgroundForHandle(static_cast<std::uint16_t>(compiled.values[0]));
		return background ? applyCompiledBackgroundValue(node, compiled.values[0], compiled.declaration, source) : false;
	}
	case CssCompiledKind::BackgroundSize:
		return applyCompiledBackgroundSizeValue(node, compiled);
	case CssCompiledKind::BorderShorthand:
		return applyCompiledBorderShorthandValue(node, compiled, source);
	case CssCompiledKind::BorderSideShorthand:
		return applyCompiledBorderSideShorthandValue(node, compiled, source);
	case CssCompiledKind::BorderRadius:
		return applyCompiledBorderRadiusValue(node, compiled, source);
	case CssCompiledKind::FilterBlur:
		return applyCompiledFilterBlurValue(node, compiled, source);
	case CssCompiledKind::BoxShadow:
		return applyCompiledBoxShadowValue(node, compiled, source);
	case CssCompiledKind::GridTemplate: {
		const CssCompiledGridTemplate *grid =
		    compiledCssGridTemplateForHandle(static_cast<std::uint16_t>(compiled.values[0]));
		if (!grid) return false;
		if (compiled.declaration == CssDeclarationId::GridTemplateColumns) {
			applyCompiledGridTemplateValue(node, *grid, true);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::GridTemplateRows) {
			applyCompiledGridTemplateValue(node, *grid, false);
			return true;
		}
		return false;
	}
	case CssCompiledKind::LineHeight:
		return applyCompiledLineHeightValue(node, compiled, source);
	case CssCompiledKind::None:
		return false;
	}
	return false;
}

struct InlineCompiledStyleCacheEntry {
	CssDeclarationId declaration = CssDeclarationId::Unknown;
	std::uint16_t compiledValue = kNoCompiledCssValue;
	bool valid = false;
	bool compileAttempted = false;
	std::string value;
};

#if GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES > 0
InlineCompiledStyleCacheEntry g_inlineCompiledStyleCache[kInlineCompiledStyleCacheEntries];
int g_inlineCompiledStyleCacheNext = 0;
#endif

void clearInlineCompiledStyleCache()
{
#if GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES > 0
	for (InlineCompiledStyleCacheEntry &entry : g_inlineCompiledStyleCache)
		entry = InlineCompiledStyleCacheEntry{};
	g_inlineCompiledStyleCacheNext = 0;
#endif
}

const CssCompiledValue *inlineCompiledStyleValueFor(CssDeclarationId declaration, const std::string &value)
{
#if GEA_INLINE_COMPILED_STYLE_CACHE_ENTRIES <= 0
	(void)declaration;
	(void)value;
	return nullptr;
#else
	if (static_cast<std::uint32_t>(GEA_COMPILED_CSS_VALUE_MASK) == 0u) return nullptr;
	if (declaration == CssDeclarationId::Unknown || declaration == CssDeclarationId::Custom || value.empty())
		return nullptr;

	for (InlineCompiledStyleCacheEntry &entry : g_inlineCompiledStyleCache) {
		if (!entry.valid || entry.declaration != declaration || entry.value != value) continue;
		if (entry.compiledValue != kNoCompiledCssValue)
			return compiledCssValueForHandle(entry.compiledValue);
		if (entry.compileAttempted) return nullptr;
		entry.compileAttempted = true;
		entry.compiledValue = compileCssValue(declaration, CssText::view(value));
		return compiledCssValueForHandle(entry.compiledValue);
	}

	int slot = -1;
	for (int i = 0; i < kInlineCompiledStyleCacheEntries; ++i) {
		if (g_inlineCompiledStyleCache[i].valid) continue;
		slot = i;
		break;
	}
	if (slot < 0) {
		for (int offset = 0; offset < kInlineCompiledStyleCacheEntries; ++offset) {
			const int candidate = (g_inlineCompiledStyleCacheNext + offset) % kInlineCompiledStyleCacheEntries;
			if (g_inlineCompiledStyleCache[candidate].compiledValue != kNoCompiledCssValue) continue;
			slot = candidate;
			g_inlineCompiledStyleCacheNext = (candidate + 1) % kInlineCompiledStyleCacheEntries;
			break;
		}
	}
	if (slot < 0) return nullptr;
	InlineCompiledStyleCacheEntry &entry = g_inlineCompiledStyleCache[slot];
	entry = InlineCompiledStyleCacheEntry{};
	entry.valid = true;
	entry.declaration = declaration;
	entry.value = value;
	return nullptr;
#endif
}

void applyPropertyWithSource(NodeHandle node,
                             CssDeclarationId declaration,
                             const std::string &rawValue,
                             StyleApplicationSource source)
{
	if (!node) return;
	const int nodeId = node.id();
	if (declaration == CssDeclarationId::Unknown) return;
	if (source == StyleApplicationSource::Inline &&
	    (declaration == CssDeclarationId::GridTemplateColumns || declaration == CssDeclarationId::GridTemplateRows ||
	     declaration == CssDeclarationId::Grid || declaration == CssDeclarationId::GridTemplate)) {
		const std::string resolved = resolveCssVarsForNode(rawValue, nodeId);
		const bool shorthand = declaration == CssDeclarationId::Grid || declaration == CssDeclarationId::GridTemplate;
		std::string rows, columns;
		CssCompiledGridTemplate parsed;
		if (shorthand ? !splitGridTemplate(resolved, rows, columns) : !parseGridTemplateSpec(resolved, parsed)) return;
		auto &rare = ensureRareData(nodeId);
		const int mask = shorthand ? 3 : declaration == CssDeclarationId::GridTemplateRows ? 2 : 1;
		for (int axis = 0; axis < 2; ++axis)
			if (mask & (1 << axis)) rare.inlineGridTemplates[axis] = internCssAtom(rawValue);
		rare.inlineGridShorthandMask = (rare.inlineGridShorthandMask & ~mask) | (shorthand ? mask : 0);
	}
	if (declaration == CssDeclarationId::FlexBasis && rawValue.find("var(") != std::string::npos) {
		if (const CssLengthSpec *length = cachedCompiledCssLengthSpec(rawValue)) {
			setFlexBasisValue(node, true, *length, source);
			return;
		}
	}
	if (rawValue.find("var(") != std::string::npos &&
	    ((declaration >= CssDeclarationId::MarginTop && declaration <= CssDeclarationId::MarginLeft) ||
	     (declaration >= CssDeclarationId::PaddingTop && declaration <= CssDeclarationId::PaddingLeft))) {
		// A length-valued longhand can retain var() as an expression instead
		// of permanently replacing it with today's fallback before parsing.
		if (const CssLengthSpec *length = cachedCompiledCssLengthSpec(rawValue)) {
			const bool padding = declaration >= CssDeclarationId::PaddingTop && declaration <= CssDeclarationId::PaddingLeft;
			const int side = static_cast<int>(declaration) - static_cast<int>(padding ? CssDeclarationId::PaddingTop : CssDeclarationId::MarginTop);
			setBoxLengthValue(node, padding, side, *length, source);
			return;
		}
	}
#if GEA_RECPROF
	g_profApplyCalls++;
	const int64_t _vt = recNow();
#endif
	const bool directValue = rawValue.find("var(") == std::string::npos && isTrimmedCssValue(rawValue);
	const CssCompiledValue *compiledValue = directValue
	    ? inlineCompiledStyleValueFor(declaration, rawValue)
	    : nullptr;
	std::string resolvedValue;
	if (!directValue)
		resolvedValue = resolveCssVarsForNode(rawValue, nodeId);
#if GEA_RECPROF
	g_profVarUs += recNow() - _vt;
	const int64_t _bt = recNow();
	struct BodyTimer { int64_t s; ~BodyTimer() { g_profBodyUs += recNow() - s; } } _bodyTimer{_bt};
#endif
	if (compiledValue && applyCompiledCssValueWithSource(node, *compiledValue, source)) return;
	const std::string &value = directValue ? rawValue : resolvedValue;
	(void)applyKnownResolvedPropertyWithSource(node, declaration, value, source);
}

void applyPropertyWithSource(NodeHandle node, const char *property, const std::string &rawValue, StyleApplicationSource source)
{
	if (!node || !property) return;
	const CssDeclarationId declaration = classifyDeclaration(property);
	if (declaration == CssDeclarationId::Custom) {
		auto &rare = ensureRareData(node.id());
		const CssAtomId name = internCssAtom(property);
		if (source == StyleApplicationSource::Inline) {
			setCustomPropertyValue(rare.inlineCustomProperties, name, rawValue);
			recomputeSubtreeClassStyles(node.id());
		} else {
			setCustomPropertyValue(rare.customProperties, name, rawValue);
		}
		return;
	}
	applyPropertyWithSource(node, declaration, rawValue, source);
}

void applyPropertyWithSource(NodeHandle node, const std::string &property, const std::string &rawValue, StyleApplicationSource source)
{
	applyPropertyWithSource(node, property.c_str(), rawValue, source);
}

void applyRulePropertyWithCompiledValue(NodeHandle node,
                                        const CssRule &rule,
                                        const CssCompiledValue *compiled,
                                        StyleApplicationSource source)
{
	if (!node) return;
	const int nodeId = node.id();
	if (rule.declaration == CssDeclarationId::Custom) {
		if (const NodeRareData *rare = rareDataFor(nodeId))
			if (rare->inlineCustomProperties.getEntry(rule.propertyAtom)) return;
		setCustomPropertyRuleValue(ensureRareData(nodeId).customProperties,
		                           rule.propertyAtom,
		                           cssRuleTextForHandle(rule.valueText),
		                           nodeId,
		                           rule.compiledValue);
		return;
	}
#if GEA_RECPROF
	g_profApplyCalls++;
	const int64_t _vt = recNow();
#endif
	if (compiled) {
		if (applyCompiledCssValueWithSource(node, *compiled, source)) return;
		if (rule.valueText == kNoCssRuleText) return;
	}
	const CssText &rawValue = cssRuleTextForHandle(rule.valueText);
	const std::string value = resolveCssVarsForNode(rawValue, nodeId);
#if GEA_RECPROF
	g_profVarUs += recNow() - _vt;
	const int64_t _bt = recNow();
	struct BodyTimer { int64_t s; ~BodyTimer() { g_profBodyUs += recNow() - s; } } _bodyTimer{_bt};
#endif
	if (applyKnownResolvedPropertyWithSource(node, rule.declaration, value, source)) return;
	applyPropertyWithSource(node,
	                        cssRuleTextForHandle(rule.propertyText).str(),
	                        rawValue.str(),
	                        source);
}

void applyRulePropertyWithSource(NodeHandle node, const CssRule &rule, StyleApplicationSource source)
{
	applyRulePropertyWithCompiledValue(node, rule, compiledCssValueForHandle(rule.compiledValue), source);
}

bool removeInlineStyleProperties(int node, std::initializer_list<Property> properties)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	bool changed = false;
	if (NodeRareData *rd = rareDataFor(node))
		for (const Property property : properties) changed = rd->inlineStyles.remove(property) || changed;
	if (changed) recomputeSubtreeClassStyles(node);
	return changed;
}

bool removeInlineStyleProperty(NodeHandle node, const std::string &property)
{
	if (!node) return false;
	if (property.rfind("--", 0) == 0) {
		NodeRareData *rare = rareDataFor(node.id());
		if (!rare) return false;
		auto &values = rare->inlineCustomProperties.values;
		const CssAtomId name = findCssAtom(property);
		const auto end = std::remove_if(values.begin(), values.end(), [name](const NodeCustomProperty &entry) { return entry.nameId == name; });
		if (end == values.end()) return false;
		values.erase(end, values.end());
		recomputeSubtreeClassStyles(node.id());
		return true;
	}
	const int id = node.id();
	if (property == "grid" || property == "grid-template") {
		NodeRareData *rare = rareDataFor(id);
		if (!rare || (rare->inlineGridTemplates[0] == kInvalidCssAtom && rare->inlineGridTemplates[1] == kInvalidCssAtom)) return false;
		rare->inlineGridTemplates[0] = rare->inlineGridTemplates[1] = kInvalidCssAtom;
		rare->inlineGridShorthandMask = 0;
		recomputeSubtreeClassStyles(id);
		return true;
	}
	if (property == "grid-template-columns" || property == "grid-template-rows") {
		NodeRareData *rare = rareDataFor(id);
		const int axis = property == "grid-template-rows" ? 1 : 0;
		if (!rare || rare->inlineGridTemplates[axis] == kInvalidCssAtom) return false;
		rare->inlineGridTemplates[axis] = kInvalidCssAtom;
		rare->inlineGridShorthandMask &= ~(1 << axis);
		recomputeSubtreeClassStyles(id);
		return true;
	}
	if (property == "font") return removeInlineStyleProperties(id, {Property::FontId, Property::FontSize, Property::FontWeight, Property::LineHeight, Property::LineHeightExpression, Property::LineHeightMultiplier});
	if (property == "display") return removeInlineStyleProperties(id, {Property::Display});
	if (property == "contain") return removeInlineStyleProperties(id, {Property::Containment});
	if (property == "aspect-ratio") return removeInlineStyleProperties(id, {Property::AspectRatio});
	if (property == "flex-direction") return removeInlineStyleProperties(id, {Property::FlexDirection});
	if (property == "flex-wrap") return removeInlineStyleProperties(id, {Property::FlexWrap});
	if (property == "flex-line-count") return removeInlineStyleProperties(id, {Property::FlexLineCount});
	if (property == "justify-content") return removeInlineStyleProperties(id, {Property::JustifyContent});
	if (property == "align-items") return removeInlineStyleProperties(id, {Property::AlignItems});
	if (property == "justify-items") return removeInlineStyleProperties(id, {Property::JustifyItems});
	if (property == "align-content") return removeInlineStyleProperties(id, {Property::AlignContent});
	if (property == "justify-self") return removeInlineStyleProperties(id, {Property::JustifySelf});
	if (property == "grid-row-start") return removeInlineStyleProperties(id, {Property::GridRowStart});
	if (property == "grid-column-start") return removeInlineStyleProperties(id, {Property::GridColumnStart});
	if (property == "grid-row-end") return removeInlineStyleProperties(id, {Property::GridRowEnd});
	if (property == "grid-column-end") return removeInlineStyleProperties(id, {Property::GridColumnEnd});
	if (property == "grid-row") return removeInlineStyleProperties(id, {Property::GridRowStart, Property::GridRowEnd});
	if (property == "grid-column") return removeInlineStyleProperties(id, {Property::GridColumnStart, Property::GridColumnEnd});
	if (property == "grid-area") return removeInlineStyleProperties(id, {Property::GridRowStart, Property::GridColumnStart, Property::GridRowEnd, Property::GridColumnEnd});
	if (property == "align-self") return removeInlineStyleProperties(id, {Property::AlignSelf});
	if (property == "place-items") return removeInlineStyleProperties(id, {Property::AlignItems, Property::JustifyItems});
	if (property == "place-content") return removeInlineStyleProperties(id, {Property::AlignContent, Property::JustifyContent});
	if (property == "place-self") return removeInlineStyleProperties(id, {Property::AlignSelf, Property::JustifySelf});
	if (property == "gap") return removeInlineStyleProperties(id, {Property::Gap, Property::RowGap, Property::ColumnGap, Property::RowGapPercent, Property::ColumnGapPercent});
	if (property == "width") return removeInlineStyleProperties(id, {Property::Width, Property::WidthPercent, Property::WidthExpression});
	if (property == "height") return removeInlineStyleProperties(id, {Property::Height, Property::HeightPercent, Property::HeightExpression});
	if (property == "min-width") return removeInlineStyleProperties(id, {Property::MinWidth});
	if (property == "min-height") return removeInlineStyleProperties(id, {Property::MinHeight});
	if (property == "max-width") return removeInlineStyleProperties(id, {Property::MaxWidth});
	if (property == "max-height") return removeInlineStyleProperties(id, {Property::MaxHeight});
	if (property == "flex") return removeInlineStyleProperties(id, {Property::Flex, Property::FlexShrink, Property::FlexBasis, Property::FlexBasisExpression});
	if (property == "flex-grow") return removeInlineStyleProperties(id, {Property::Flex});
	if (property == "flex-shrink") return removeInlineStyleProperties(id, {Property::FlexShrink});
	if (property == "flex-basis") return removeInlineStyleProperties(id, {Property::FlexBasis, Property::FlexBasisExpression});
	if (property == "padding")
		return removeInlineStyleProperties(id, {Property::PaddingTop, Property::PaddingTopExpression, Property::PaddingRight, Property::PaddingRightExpression, Property::PaddingBottom, Property::PaddingBottomExpression, Property::PaddingLeft, Property::PaddingLeftExpression});
	if (property == "padding-top") return removeInlineStyleProperties(id, {Property::PaddingTop, Property::PaddingTopExpression});
	if (property == "padding-right") return removeInlineStyleProperties(id, {Property::PaddingRight, Property::PaddingRightExpression});
	if (property == "padding-bottom") return removeInlineStyleProperties(id, {Property::PaddingBottom, Property::PaddingBottomExpression});
	if (property == "padding-left") return removeInlineStyleProperties(id, {Property::PaddingLeft, Property::PaddingLeftExpression});
	if (property == "margin")
		return removeInlineStyleProperties(id, {Property::MarginTop, Property::MarginTopExpression, Property::MarginRight, Property::MarginRightExpression, Property::MarginBottom, Property::MarginBottomExpression, Property::MarginLeft, Property::MarginLeftExpression, Property::MarginTopAuto, Property::MarginRightAuto, Property::MarginBottomAuto, Property::MarginLeftAuto});
	if (property == "margin-top") return removeInlineStyleProperties(id, {Property::MarginTop, Property::MarginTopExpression, Property::MarginTopAuto});
	if (property == "margin-right") return removeInlineStyleProperties(id, {Property::MarginRight, Property::MarginRightExpression, Property::MarginRightAuto});
	if (property == "margin-bottom") return removeInlineStyleProperties(id, {Property::MarginBottom, Property::MarginBottomExpression, Property::MarginBottomAuto});
	if (property == "margin-left") return removeInlineStyleProperties(id, {Property::MarginLeft, Property::MarginLeftExpression, Property::MarginLeftAuto});
	if (property == "position") return removeInlineStyleProperties(id, {Property::Position});
	if (property == "top") return removeInlineStyleProperties(id, {Property::Top, Property::TopPercent});
	if (property == "right") return removeInlineStyleProperties(id, {Property::Right, Property::RightPercent});
	if (property == "bottom") return removeInlineStyleProperties(id, {Property::Bottom, Property::BottomPercent});
	if (property == "left") return removeInlineStyleProperties(id, {Property::Left, Property::LeftPercent});
	if (property == "inset")
		return removeInlineStyleProperties(id,
		                                   {Property::Top,
		                                    Property::Right,
		                                    Property::Bottom,
		                                    Property::Left,
		                                    Property::TopPercent,
		                                    Property::RightPercent,
		                                    Property::BottomPercent,
		                                    Property::LeftPercent});
	if (property == "box-sizing") return removeInlineStyleProperties(id, {Property::BoxSizing});
	if (property == "float") return removeInlineStyleProperties(id, {Property::Float});
	if (property == "margin-trim") return removeInlineStyleProperties(id, {Property::MarginTrim});
	if (property == "clear") return removeInlineStyleProperties(id, {Property::Clear});
	if (property == "direction") return removeInlineStyleProperties(id, {Property::Direction});
	if (property == "writing-mode") return removeInlineStyleProperties(id, {Property::WritingMode});
	if (property == "flex-flow") return removeInlineStyleProperties(id, {Property::FlexDirection, Property::FlexWrap});
	if (property == "row-gap") return removeInlineStyleProperties(id, {Property::RowGap, Property::RowGapPercent});
	if (property == "column-gap") return removeInlineStyleProperties(id, {Property::ColumnGap, Property::ColumnGapPercent});
	if (property == "order") return removeInlineStyleProperties(id, {Property::Order});
	if (property == "z-index") return removeInlineStyleProperties(id, {Property::ZIndex});
	if (property == "background")
		return removeInlineStyleProperties(id, {Property::BackgroundColor, Property::BackgroundAlpha, Property::BackgroundImage, Property::BackgroundClip, Property::HasBackground, Property::BackgroundSizeList, Property::BackgroundPositionList, Property::BackgroundRepeatList, Property::BackgroundAttachmentList, Property::BackgroundOriginList});
	if (property == "background-clip") return removeInlineStyleProperties(id, {Property::BackgroundClip});
	if (property == "background-size") return removeInlineStyleProperties(id, {Property::BackgroundSizeList});
	if (property == "background-position") return removeInlineStyleProperties(id, {Property::BackgroundPositionList});
	if (property == "background-repeat") return removeInlineStyleProperties(id, {Property::BackgroundRepeatList});
	if (property == "background-attachment") return removeInlineStyleProperties(id, {Property::BackgroundAttachmentList});
	if (property == "background-origin") return removeInlineStyleProperties(id, {Property::BackgroundOriginList});

	if (property == "background-color")
		return removeInlineStyleProperties(id, {Property::BackgroundColor, Property::BackgroundAlpha});
	if (property == "background-image")
		return removeInlineStyleProperties(id, {Property::BackgroundImage});
	if (property == "color") return removeInlineStyleProperties(id, {Property::Color, Property::ColorAlpha});
	if (property == "opacity") return removeInlineStyleProperties(id, {Property::Opacity});
	if (property == "border-color") return removeInlineStyleProperties(id, {Property::BorderColor, Property::BorderColorCurrent, Property::BorderAlpha, Property::BorderTopColor, Property::BorderRightColor, Property::BorderBottomColor, Property::BorderLeftColor, Property::BorderTopAlpha, Property::BorderTopColorCurrent, Property::BorderRightAlpha, Property::BorderRightColorCurrent, Property::BorderBottomAlpha, Property::BorderBottomColorCurrent, Property::BorderLeftAlpha, Property::BorderLeftColorCurrent});
	if (property == "border-width") return removeInlineStyleProperties(id, {Property::BorderWidth, Property::BorderTopWidth, Property::BorderRightWidth, Property::BorderBottomWidth, Property::BorderLeftWidth});
	if (property == "border") return removeInlineStyleProperties(id, {Property::BorderRelief, Property::BorderTopRelief, Property::BorderRightRelief, Property::BorderBottomRelief, Property::BorderLeftRelief, Property::BorderWidth, Property::BorderTopWidth, Property::BorderRightWidth, Property::BorderBottomWidth, Property::BorderLeftWidth, Property::BorderColor, Property::BorderColorCurrent, Property::BorderAlpha, Property::BorderTopColor, Property::BorderRightColor, Property::BorderBottomColor, Property::BorderLeftColor, Property::BorderTopAlpha, Property::BorderTopColorCurrent, Property::BorderRightAlpha, Property::BorderRightColorCurrent, Property::BorderBottomAlpha, Property::BorderBottomColorCurrent, Property::BorderLeftAlpha, Property::BorderLeftColorCurrent});
	if (property == "border-top") return removeInlineStyleProperties(id, {Property::BorderTopRelief, Property::BorderTopWidth, Property::BorderTopColor, Property::BorderTopAlpha, Property::BorderTopColorCurrent});
	if (property == "border-right") return removeInlineStyleProperties(id, {Property::BorderRightRelief, Property::BorderRightWidth, Property::BorderRightColor, Property::BorderRightAlpha, Property::BorderRightColorCurrent});
	if (property == "border-bottom") return removeInlineStyleProperties(id, {Property::BorderBottomRelief, Property::BorderBottomWidth, Property::BorderBottomColor, Property::BorderBottomAlpha, Property::BorderBottomColorCurrent});
	if (property == "border-left") return removeInlineStyleProperties(id, {Property::BorderLeftRelief, Property::BorderLeftWidth, Property::BorderLeftColor, Property::BorderLeftAlpha, Property::BorderLeftColorCurrent});
	if (property == "border-top-width") return removeInlineStyleProperties(id, {Property::BorderTopWidth});
	if (property == "border-right-width") return removeInlineStyleProperties(id, {Property::BorderRightWidth});
	if (property == "border-bottom-width") return removeInlineStyleProperties(id, {Property::BorderBottomWidth});
	if (property == "border-left-width") return removeInlineStyleProperties(id, {Property::BorderLeftWidth});
	if (property == "border-top-color") return removeInlineStyleProperties(id, {Property::BorderTopColor, Property::BorderTopAlpha, Property::BorderTopColorCurrent});
	if (property == "border-right-color") return removeInlineStyleProperties(id, {Property::BorderRightColor, Property::BorderRightAlpha, Property::BorderRightColorCurrent});
	if (property == "border-bottom-color") return removeInlineStyleProperties(id, {Property::BorderBottomColor, Property::BorderBottomAlpha, Property::BorderBottomColorCurrent});
	if (property == "border-left-color") return removeInlineStyleProperties(id, {Property::BorderLeftColor, Property::BorderLeftAlpha, Property::BorderLeftColorCurrent});
	if (property == "border-radius")
		return removeInlineStyleProperties(id,
		                                   {Property::BorderRadiusTopLeft,
		                                    Property::BorderRadiusTopRight,
		                                    Property::BorderRadiusBottomRight,
		                                    Property::BorderRadiusBottomLeft,
		                                    Property::BorderRadiusTopLeftPercent,
		                                    Property::BorderRadiusTopRightPercent,
		                                    Property::BorderRadiusBottomRightPercent,
		                                    Property::BorderRadiusBottomLeftPercent});
	if (property == "border-top-left-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusTopLeft, Property::BorderRadiusTopLeftPercent});
	if (property == "border-top-right-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusTopRight, Property::BorderRadiusTopRightPercent});
	if (property == "border-bottom-right-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusBottomRight, Property::BorderRadiusBottomRightPercent});
	if (property == "border-bottom-left-radius")
		return removeInlineStyleProperties(id, {Property::BorderRadiusBottomLeft, Property::BorderRadiusBottomLeftPercent});
	if (property == "font-family") return removeInlineStyleProperties(id, {Property::FontId});
	if (property == "font-size") return removeInlineStyleProperties(id, {Property::FontSize});
	if (property == "font-weight") return removeInlineStyleProperties(id, {Property::FontWeight});
	if (property == "line-height") return removeInlineStyleProperties(id, {Property::LineHeight, Property::LineHeightExpression, Property::LineHeightMultiplier});
	if (property == "text-align") return removeInlineStyleProperties(id, {Property::TextAlign});
	if (property == "text-decoration" || property == "text-decoration-line")
		return removeInlineStyleProperties(id, {Property::TextDecoration});
	if (property == "text-transform") return removeInlineStyleProperties(id, {Property::TextTransform});
	if (property == "white-space") return removeInlineStyleProperties(id, {Property::WhiteSpace});
	if (property == "text-overflow") return removeInlineStyleProperties(id, {Property::TextOverflow});
	if (property == "transform-style") return removeInlineStyleProperties(id, {Property::TransformStyle});
	if (property == "visibility") return removeInlineStyleProperties(id, {Property::Visibility});
	if (property == "backface-visibility") return removeInlineStyleProperties(id, {Property::Backface});
	if (property == "pointer-events") return removeInlineStyleProperties(id, {Property::PointerEvents});
	if (property == "overflow") return removeInlineStyleProperties(id, {Property::Overflow, Property::OverflowX, Property::OverflowY});
	if (property == "overflow-x") return removeInlineStyleProperties(id, {Property::OverflowX});
	if (property == "overflow-y") return removeInlineStyleProperties(id, {Property::OverflowY});
	if (property == "mask-image" || property == "-webkit-mask-image")
		return removeInlineStyleProperties(id, {Property::MaskRightFadeWidth});
	if (property == "transform")
		return removeInlineStyleProperties(id,
		                                   {Property::TransformPresent, Property::TransformTranslateOuterAxes, Property::TransformRotate,
		                                    Property::TransformRotateX,
		                                    Property::TransformRotateY,
		                                    Property::TransformTranslateX,
		                                    Property::TransformTranslateY,
		                                    Property::TransformTranslateZ,
		                                    Property::TransformTranslateXPercent,
		                                    Property::TransformTranslateYPercent,
	                                    Property::TransformScaleX,
	                                    Property::TransformScaleY,
	                                    Property::TransformScaleZ});
	if (property == "rotate") return removeInlineStyleProperties(id, {Property::RotatePresent, Property::RotateAngle, Property::RotateAxisX, Property::RotateAxisY, Property::RotateAxisZ});
	if (property == "translate") return removeInlineStyleProperties(id, {Property::TranslatePresent, Property::TranslateX, Property::TranslateY, Property::TranslateZ, Property::TranslateXPercent, Property::TranslateYPercent});
	if (property == "scale") return removeInlineStyleProperties(id, {Property::ScalePresent, Property::ScaleX, Property::ScaleY, Property::ScaleZ});
	if (property == "filter") return removeInlineStyleProperties(id, {Property::FilterPresent, Property::FilterBlur});
	if (property == "box-shadow")
		return removeInlineStyleProperties(id,
		                                   {Property::BoxShadowInset,
		                                    Property::BoxShadowOffsetX,
		                                    Property::BoxShadowOffsetY,
		                                    Property::BoxShadowBlur,
		                                    Property::BoxShadowSpread,
		                                    Property::BoxShadowColor,
		                                    Property::BoxShadowAlpha});
	if (property == "transform-origin")
		return removeInlineStyleProperties(id, {Property::TransformOriginX, Property::TransformOriginY});
	if (property == "perspective") return removeInlineStyleProperties(id, {Property::Perspective});
	if (property == "perspective-origin")
		return removeInlineStyleProperties(id, {Property::PerspectiveOriginX, Property::PerspectiveOriginY});
	return false;
}

void replayInlineStyles(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (const NodeRareData *rd = rareDataFor(node))
		for (std::size_t i = 0, n = rd->inlineStyles.size(); i < n; ++i) {
			const NodeStyleOverride &entry = rd->inlineStyles.at(i);
			Tree::instance().setStyleFromClass(node, entry.property, entry.value);
		}
	if (const NodeRareData *rd = rareDataFor(node)) {
		// Copy the atoms before replay: resolving authored values can touch
		// pooled style storage. Resolve variables and font-relative tracks anew.
		const CssAtomId templates[2] = {rd->inlineGridTemplates[0], rd->inlineGridTemplates[1]};
		const int shorthandMask = rd->inlineGridShorthandMask;
		for (int axis = 0; axis < 2; ++axis) {
			if (templates[axis] == kInvalidCssAtom) continue;
			std::string value = cssAtomText(templates[axis]);
			if (shorthandMask & (1 << axis)) {
				std::string rows, columns;
				if (!splitGridTemplate(resolveCssVarsForNode(value, node), rows, columns)) continue;
				value = axis == 0 ? columns : rows;
			}
			applyPropertyWithSource(NodeHandle(node), axis == 0 ? CssDeclarationId::GridTemplateColumns : CssDeclarationId::GridTemplateRows,
			                        value, StyleApplicationSource::ClassRule);
		}
	}
}

void applyDefaultStyleOverrides(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (const NodeRareData *rd = rareDataFor(node))
		for (std::size_t i = 0, n = rd->defaultStyles.size(); i < n; ++i) {
			const NodeStyleOverride &entry = rd->defaultStyles.at(i);
			Tree::instance().setStyleFromClass(node, entry.property, entry.value);
		}
}

int16_t g_pseudoBeforeTagId = -1;
int16_t g_pseudoAfterTagId = -1;

int16_t pseudoBeforeTagId()
{
	if (g_pseudoBeforeTagId < 0) g_pseudoBeforeTagId = internTag("::before");
	return g_pseudoBeforeTagId;
}

int16_t pseudoAfterTagId()
{
	if (g_pseudoAfterTagId < 0) g_pseudoAfterTagId = internTag("::after");
	return g_pseudoAfterTagId;
}

int16_t pseudoTagId(CssRule::PseudoElement pseudo)
{
	return pseudo == CssRule::PseudoElement::Before ? pseudoBeforeTagId() : pseudoAfterTagId();
}

bool isGeneratedPseudoNode(const Node &node)
{
	const int16_t tag = node.tag_id;
	return tag == pseudoBeforeTagId() || tag == pseudoAfterTagId();
}

std::string normalizeSelectorText(const std::string &selector)
{
	std::string out = trimCssValue(selector);
	std::size_t write = 0;
	bool previousSpace = false;
	for (std::size_t read = 0; read < out.size(); ++read) {
		const unsigned char c = static_cast<unsigned char>(out[read]);
		if (c <= ' ') {
			if (!previousSpace) out[write++] = ' ';
			previousSpace = true;
		} else {
			out[write++] = out[read];
			previousSpace = false;
		}
	}
	out.resize(write);
	if (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

bool isRootNode(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const int mountedRoot = Tree::instance().mountedRoot();
	if (mountedRoot >= 0) return node == mountedRoot;
	return state.nodes[node].parent < 0;
}

bool isFirstElementChild(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const int parent = state.nodes[node].parent;
	if (parent < 0 || parent >= state.nodeCount) return false;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child]) || isAnonymousTextNode(state.nodes[child])) continue;
		return child == node;
	}
	return false;
}

bool isLastElementChild(int node)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const int parent = state.nodes[node].parent;
	if (parent < 0 || parent >= state.nodeCount) return false;
	for (int child = state.nodes[parent].last_child; child >= 0; child = state.nodes[child].prev_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child]) || isAnonymousTextNode(state.nodes[child])) continue;
		return child == node;
	}
	return false;
}

template <typename T, std::size_t InlineCount>
struct SmallSelectorList {
	T inlineValues[InlineCount]{};
	std::vector<T> spillValues;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	std::size_t size() const { return spilled ? spillValues.size() : inlineCount; }
	bool empty() const { return size() == 0; }

	T &at(std::size_t index)
	{
		return spilled ? spillValues[index] : inlineValues[index];
	}

	const T &at(std::size_t index) const
	{
		return spilled ? spillValues[index] : inlineValues[index];
	}

	T &front() { return at(0); }
	const T &front() const { return at(0); }
	T &back() { return at(size() - 1); }
	const T &back() const { return at(size() - 1); }

	void push_back(const T &value)
	{
		if (!spilled && inlineCount < InlineCount) {
			inlineValues[inlineCount++] = value;
			return;
		}
		ensureSpilled();
		spillValues.push_back(value);
	}

	void push_back(T &&value)
	{
		if (!spilled && inlineCount < InlineCount) {
			inlineValues[inlineCount++] = std::move(value);
			return;
		}
		ensureSpilled();
		spillValues.push_back(std::move(value));
	}

private:
	void ensureSpilled()
	{
		if (spilled) return;
		spillValues.assign(inlineValues, inlineValues + inlineCount);
		spilled = true;
	}
};

bool equalsLiteral(const char *text, std::size_t length, const char *literal)
{
	const std::size_t literalLength = std::strlen(literal ? literal : "");
	return length == literalLength && std::memcmp(text ? text : "", literal ? literal : "", length) == 0;
}

// A simple (compound) selector parsed into its constituents. `valid` is false for
// syntactically unmatchable inputs (empty, contains "::", unknown pseudo, bad
// marker). The match against a specific node stays per-call; only the parse is
// cached, since parsing the compound string each call dominated selector matching
// during recompute.
struct ParsedSimpleSelector {
	int16_t tagId = -1;
	CssAtomId idAtom = kInvalidCssAtom;
	SmallSelectorList<CssAtomId, 4> classIds;
	bool hasTag = false;
	bool wantsRoot = false;
	bool wantsFirstChild = false;
	bool wantsLastChild = false;
	bool wantsHover = false;
	bool rootTag = false;
	bool hasMatcher = false;
	bool valid = true;
};

ParsedSimpleSelector parseSimpleSelector(const char *rawSimple, std::size_t rawLength)
{
	ParsedSimpleSelector p;
	const char *simple = rawSimple ? rawSimple : "";
	std::size_t startOffset = 0;
	std::size_t endOffset = rawLength;
	while (startOffset < endOffset && static_cast<unsigned char>(simple[startOffset]) <= ' ') ++startOffset;
	while (endOffset > startOffset && static_cast<unsigned char>(simple[endOffset - 1]) <= ' ') --endOffset;
	if (startOffset == endOffset) {
		p.valid = false;
		return p;
	}
	for (std::size_t j = startOffset; j + 1 < endOffset; ++j) {
		if (simple[j] == ':' && simple[j + 1] == ':') {
			p.valid = false;
			return p;
		}
	}
	std::size_t i = startOffset;
	if (simple[i] != '.' && simple[i] != '#' && simple[i] != ':') {
		const std::size_t start = i;
		while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
		const std::string tag = toLowerAscii(std::string(simple + start, i - start));
		p.hasTag = tag != "*";
		p.hasMatcher = tag == "*";
		if (tag == "body" || tag == "html")
			p.rootTag = true;
		else
			p.tagId = internTag(tag.c_str());
	}
	while (i < endOffset) {
		const char marker = simple[i++];
		const std::size_t start = i;
		if (marker == '.') {
			while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
			p.classIds.push_back(internCssAtom(simple + start, i - start));
		} else if (marker == '#') {
			while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
			p.idAtom = internCssAtom(simple + start, i - start);
		} else if (marker == ':') {
			while (i < endOffset && simple[i] != '.' && simple[i] != '#' && simple[i] != ':') ++i;
			const std::size_t pseudoLength = i - start;
			if (equalsLiteral(simple + start, pseudoLength, "root")) p.wantsRoot = true;
			else if (equalsLiteral(simple + start, pseudoLength, "first-child")) p.wantsFirstChild = true;
			else if (equalsLiteral(simple + start, pseudoLength, "last-child")) p.wantsLastChild = true;
			else if (equalsLiteral(simple + start, pseudoLength, "hover")) p.wantsHover = true;
			else { p.valid = false; return p; }
		} else {
			p.valid = false;
			return p;
		}
	}
	p.hasMatcher = p.hasMatcher || p.wantsRoot || p.wantsFirstChild || p.wantsLastChild || p.wantsHover ||
	               p.hasTag || p.idAtom != kInvalidCssAtom || !p.classIds.empty();
	return p;
}

struct SelectorPart {
	ParsedSimpleSelector simple;
	bool directParent = false;
};

struct SelectorPlan {
	SmallSelectorList<SelectorPart, 4> parts;
	SmallSelectorList<CssAtomId, 4> ancestorClasses;
	SmallSelectorList<int16_t, 4> ancestorTags;
	CssAtomId rightmostId = kInvalidCssAtom;
	CssAtomId rightmostClass = kInvalidCssAtom;
	int16_t rightmostTag = -1;
	int specificity = 0;
	bool rightmostRoot = false;
	bool valid = true;
};

template <typename T>
void appendUnique(SmallSelectorList<T, 4> &values, T value)
{
	for (std::size_t i = 0, n = values.size(); i < n; ++i)
		if (values.at(i) == value) return;
	values.push_back(value);
}

int simpleSelectorSpecificity(const ParsedSimpleSelector &simple)
{
	int ids = simple.idAtom == kInvalidCssAtom ? 0 : 1;
	int classes = static_cast<int>(simple.classIds.size());
	if (simple.wantsRoot) classes += 1;
	if (simple.wantsFirstChild) classes += 1;
	if (simple.wantsLastChild) classes += 1;
	if (simple.wantsHover) classes += 1;
	const int elements = simple.hasTag ? 1 : 0;
	return ids * 10000 + classes * 100 + elements;
}

void finishSelectorPlan(SelectorPlan &plan)
{
	if (plan.parts.empty()) {
		plan.valid = false;
		return;
	}

	const ParsedSimpleSelector &rightmost = plan.parts.back().simple;
	if (rightmost.idAtom != kInvalidCssAtom) {
		plan.rightmostId = rightmost.idAtom;
	} else if (!rightmost.classIds.empty() && rightmost.classIds.front() != kInvalidCssAtom) {
		plan.rightmostClass = rightmost.classIds.front();
	} else if (rightmost.hasTag && !rightmost.rootTag && rightmost.tagId >= 0) {
		plan.rightmostTag = rightmost.tagId;
	} else if (rightmost.wantsRoot || rightmost.rootTag) {
		plan.rightmostRoot = true;
	}

	for (std::size_t p = 0; p + 1 < plan.parts.size(); ++p) {
		const ParsedSimpleSelector &ancestor = plan.parts.at(p).simple;
		for (std::size_t c = 0, n = ancestor.classIds.size(); c < n; ++c) {
			const CssAtomId cls = ancestor.classIds.at(c);
			if (cls != kInvalidCssAtom) appendUnique(plan.ancestorClasses, cls);
		}
		if (ancestor.hasTag && !ancestor.rootTag && ancestor.tagId >= 0)
			appendUnique(plan.ancestorTags, ancestor.tagId);
	}
}

SelectorPlan parseNormalizedSelectorPlan(const char *selectorText, std::size_t selectorLength)
{
	SelectorPlan plan;
	const char *selector = selectorText ? selectorText : "";
	bool nextDirect = false;
	std::size_t i = 0;
	while (i < selectorLength) {
		while (i < selectorLength && static_cast<unsigned char>(selector[i]) <= ' ') ++i;
		if (i >= selectorLength) break;
		if (selector[i] == '>') {
			nextDirect = true;
			++i;
			continue;
		}
		const std::size_t start = i;
		while (i < selectorLength && static_cast<unsigned char>(selector[i]) > ' ' && selector[i] != '>') ++i;
		if (i <= start) continue;
		SelectorPart part;
		part.simple = parseSimpleSelector(selector + start, i - start);
		part.directParent = nextDirect;
		nextDirect = false;
		if (!part.simple.valid) plan.valid = false;
		plan.specificity += simpleSelectorSpecificity(part.simple);
		plan.parts.push_back(std::move(part));
	}
	finishSelectorPlan(plan);
	return plan;
}

SelectorPlan parseSelectorPlan(const std::string &rawSelector)
{
	const std::string selector = normalizeSelectorText(rawSelector);
	return parseNormalizedSelectorPlan(selector.c_str(), selector.size());
}

std::vector<SelectorPlan> &selectorPlans()
{
	static std::vector<SelectorPlan> plans;
	return plans;
}

std::unordered_map<std::string, std::uint16_t> &selectorPlanCache()
{
	static std::unordered_map<std::string, std::uint16_t> cache;
	return cache;
}

std::uint16_t compileSelectorPlan(const CssText &selector)
{
	if (selector.empty()) return kNoSelectorPlan;
	const std::string key(selector.c_str(), selector.length);
	auto &cache = selectorPlanCache();
	const auto cached = cache.find(key);
	if (cached != cache.end()) return cached->second;
	auto &plans = selectorPlans();
	if (plans.size() >= 0xFFFFu) return kNoSelectorPlan;
	plans.push_back(parseNormalizedSelectorPlan(key.c_str(), key.size()));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(key, handle);
	return handle;
}

ParsedSimpleSelector staticSimpleSelectorForSpec(const StaticStyleSimpleSelectorSpec &spec)
{
	ParsedSimpleSelector p;
	if (spec.tag && spec.tag[0] != '\0') {
		const std::string tag = toLowerAscii(spec.tag);
		p.hasTag = true;
		if (tag == "body" || tag == "html")
			p.rootTag = true;
		else
			p.tagId = internTag(tag.c_str());
	}
	if (spec.id && spec.id[0] != '\0')
		p.idAtom = internCssAtom(spec.id);
	for (const char *className : spec.classes) {
		if (!className || className[0] == '\0') continue;
		p.classIds.push_back(internCssAtom(className));
	}
	p.wantsRoot = spec.wantsRoot;
	p.wantsFirstChild = spec.wantsFirstChild;
	p.wantsLastChild = spec.wantsLastChild;
	p.wantsHover = spec.wantsHover;
	p.hasMatcher = p.wantsRoot || p.wantsFirstChild || p.wantsLastChild || p.wantsHover ||
	               p.hasTag || p.idAtom != kInvalidCssAtom || !p.classIds.empty();
	p.valid = p.hasMatcher;
	return p;
}

std::uint16_t storeStaticSelectorPlan(const char *selector,
                                      std::initializer_list<StaticStyleSelectorPartSpec> parts)
{
	const char *selectorText = selector ? selector : "";
	const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(selectorText, std::strlen(selectorText));
	if (!selectorSlice.data || selectorSlice.length == 0) return kNoSelectorPlan;
	const std::string key(selectorSlice.data, selectorSlice.length);
	auto &cache = selectorPlanCache();
	const auto cached = cache.find(key);
	if (cached != cache.end()) return cached->second;
	auto &plans = selectorPlans();
	if (plans.size() >= 0xFFFFu) return kNoSelectorPlan;

	SelectorPlan plan;
	for (const StaticStyleSelectorPartSpec &spec : parts) {
		SelectorPart part;
		part.simple = staticSimpleSelectorForSpec(spec.simple);
		part.directParent = spec.directParent;
		if (!part.simple.valid) plan.valid = false;
		plan.specificity += simpleSelectorSpecificity(part.simple);
		plan.parts.push_back(std::move(part));
	}
	finishSelectorPlan(plan);
	plans.push_back(std::move(plan));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(key, handle);
	return handle;
}

const SelectorPlan *selectorPlanForHandle(std::uint16_t handle)
{
	const auto &plans = selectorPlans();
	if (handle == kNoSelectorPlan || static_cast<std::size_t>(handle) >= plans.size()) return nullptr;
	return &plans[static_cast<std::size_t>(handle)];
}

CssAtomId nodeIdAttributeAtom(int node);

bool matchSimpleSelector(int node, const ParsedSimpleSelector &parsed)
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	if (isAnonymousTextNode(state.nodes[node])) return false;
	if (!parsed.valid) return false;
	const bool wantsRoot = parsed.wantsRoot;
	const bool wantsFirstChild = parsed.wantsFirstChild;
	const bool wantsLastChild = parsed.wantsLastChild;

	if (wantsRoot && !isRootNode(node)) return false;
	if (wantsFirstChild && !isFirstElementChild(node)) return false;
	if (wantsLastChild && !isLastElementChild(node)) return false;
	if (parsed.wantsHover && !Tree::instance().isHovered(node)) return false;
	if (parsed.hasTag) {
		if (parsed.rootTag) {
			if (!isRootNode(node)) return false;
		} else if (state.nodes[node].tag_id != parsed.tagId) {
			return false;
		}
	}
	if (parsed.idAtom != kInvalidCssAtom && nodeIdAttributeAtom(node) != parsed.idAtom) return false;
	for (std::size_t i = 0, n = parsed.classIds.size(); i < n; ++i) {
		const CssAtomId classId = parsed.classIds.at(i);
		if (classId == kInvalidCssAtom || !state.classLists[node].containsAtom(classId)) return false;
	}
	return parsed.hasMatcher;
}

void clearSelectorPartsCache()
{
	selectorPlans().clear();
	selectorPlanCache().clear();
}

bool selectorPlanMatchesNode(const SelectorPlan &plan, int node)
{
	const auto &parts = plan.parts;
	if (!plan.valid) return false;
	if (parts.empty()) return false;
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;

	int current = node;
	const int last = static_cast<int>(parts.size()) - 1;
	if (!matchSimpleSelector(current, parts.at(static_cast<std::size_t>(last)).simple)) return false;

	for (int target = last - 1; target >= 0; --target) {
		const bool direct = parts.at(static_cast<std::size_t>(target + 1)).directParent;
		const int parent = current >= 0 && current < state.nodeCount ? state.nodes[current].parent : -1;
		if (direct) {
			current = parent;
			if (current < 0) return false;
			if (!matchSimpleSelector(current, parts.at(static_cast<std::size_t>(target)).simple)) return false;
			continue;
		}

		bool found = false;
		for (int ancestor = parent; ancestor >= 0 && ancestor < state.nodeCount; ancestor = state.nodes[ancestor].parent) {
			if (matchSimpleSelector(ancestor, parts.at(static_cast<std::size_t>(target)).simple)) {
				current = ancestor;
				found = true;
				break;
			}
		}
		if (!found) return false;
	}
	return true;
}

bool selectorMatchesNode(const std::string &selector, int node)
{
	const SelectorPlan plan = parseSelectorPlan(selector);
	return selectorPlanMatchesNode(plan, node);
}

bool selectorMatchesNode(const CssRule &rule, int node)
{
	const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
	return plan && selectorPlanMatchesNode(*plan, node);
}

bool ruleMatchesNode(const CssRule &rule, int node)
{
	if (rule.pseudoElement != CssRule::PseudoElement::None) return false;
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	switch (rule.selectorType) {
	case CssRule::SelectorType::Class:
		return state.classLists[node].containsAtom(rule.selectorAtom);
	case CssRule::SelectorType::Element:
		return state.nodes[node].tag_id == rule.selectorTagId;
	case CssRule::SelectorType::Selector:
		return selectorMatchesNode(rule, node);
	}
	return false;
}

// --- @media condition evaluation -------------------------------------------
// Conditions are evaluated live against the current viewport. Lengths resolve
// to logical CSS px (physical / DPR) to match CSS author intent; orientation
// and aspect-ratio use the raw dimension ratio (DPR-independent); resolution /
// device-pixel-ratio compare against DPR (in dppx). Empty condition always
// applies. recomputeAllClassStyles() re-runs on resize, so breakpoints reflow.

double mediaLogicalWidth()
{
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	return g_viewport_width / dpr;
}

double mediaLogicalHeight()
{
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	return g_viewport_height / dpr;
}

// Parse a media-feature value: ratio "W/H" -> W/H; "Ndpi" -> N/96 dppx;
// "Ndppx"/"Nx" -> N; px / unitless / dpr -> N.
bool parseMediaNumber(const std::string &raw, double &out)
{
	const std::string s = trimCssValue(raw);
	if (s.empty()) return false;
	const std::size_t slash = s.find('/');
	if (slash != std::string::npos) {
		const double w = std::strtod(s.c_str(), nullptr);
		const double h = std::strtod(s.c_str() + slash + 1, nullptr);
		if (h == 0.0) return false;
		out = w / h;
		return true;
	}
	char *end = nullptr;
	const double v = std::strtod(s.c_str(), &end);
	if (end == s.c_str()) return false;
	const std::string unit = trimCssValue(std::string(end));
	if (unit == "dpi") { out = v / 96.0; return true; }
	if (unit == "dppx" || unit == "x") { out = v; return true; }
	out = v;
	return true;
}

enum class CssMediaFeatureKind : std::uint8_t {
	AlwaysFalse,
	Orientation,
	Monochrome,
	Width,
	Height,
	AspectRatio,
	Resolution
};

enum class CssMediaCompare : std::uint8_t {
	Equal,
	Min,
	Max,
	Boolean
};

struct CssMediaTerm {
	CssMediaFeatureKind kind = CssMediaFeatureKind::AlwaysFalse;
	CssMediaCompare compare = CssMediaCompare::Equal;
	double value = 0.0;
	std::uint8_t orientation = 0;  // 1 = portrait, 2 = landscape
};

struct CssMediaQueryPlan {
	bool valid = false;
	std::vector<CssMediaTerm> terms;
};

struct CssMediaConditionPlan {
	std::vector<CssMediaQueryPlan> queries;
};

std::vector<CssMediaConditionPlan> &compiledMediaConditionPlans()
{
	static std::vector<CssMediaConditionPlan> plans;
	return plans;
}

std::unordered_map<std::string, std::uint16_t> &mediaConditionPlanCache()
{
	static std::unordered_map<std::string, std::uint16_t> cache;
	return cache;
}

CssMediaFeatureKind mediaFeatureKindForStatic(StaticStyleMediaFeatureKind kind)
{
	switch (kind) {
	case StaticStyleMediaFeatureKind::Orientation: return CssMediaFeatureKind::Orientation;
	case StaticStyleMediaFeatureKind::Monochrome: return CssMediaFeatureKind::Monochrome;
	case StaticStyleMediaFeatureKind::Width: return CssMediaFeatureKind::Width;
	case StaticStyleMediaFeatureKind::Height: return CssMediaFeatureKind::Height;
	case StaticStyleMediaFeatureKind::AspectRatio: return CssMediaFeatureKind::AspectRatio;
	case StaticStyleMediaFeatureKind::Resolution: return CssMediaFeatureKind::Resolution;
	case StaticStyleMediaFeatureKind::AlwaysFalse: return CssMediaFeatureKind::AlwaysFalse;
	}
	return CssMediaFeatureKind::AlwaysFalse;
}

CssMediaCompare mediaCompareForStatic(StaticStyleMediaCompare compare)
{
	switch (compare) {
	case StaticStyleMediaCompare::Equal: return CssMediaCompare::Equal;
	case StaticStyleMediaCompare::Min: return CssMediaCompare::Min;
	case StaticStyleMediaCompare::Max: return CssMediaCompare::Max;
	case StaticStyleMediaCompare::Boolean: return CssMediaCompare::Boolean;
	}
	return CssMediaCompare::Equal;
}

std::uint16_t storeStaticMediaConditionPlan(const char *condition,
                                            std::initializer_list<StaticStyleMediaQuerySpec> queries)
{
	const char *conditionText = condition ? condition : "";
	if (conditionText[0] == '\0') return kNoMediaConditionPlan;
	const std::string key(conditionText);
	auto &cache = mediaConditionPlanCache();
	const auto cached = cache.find(key);
	if (cached != cache.end()) return cached->second;
	auto &plans = compiledMediaConditionPlans();
	if (plans.size() >= kNoMediaConditionPlan) return kNoMediaConditionPlan;

	CssMediaConditionPlan plan;
	for (const StaticStyleMediaQuerySpec &querySpec : queries) {
		CssMediaQueryPlan query;
		query.valid = querySpec.valid;
		for (const StaticStyleMediaTermSpec &termSpec : querySpec.terms) {
			CssMediaTerm term;
			term.kind = mediaFeatureKindForStatic(termSpec.kind);
			term.compare = mediaCompareForStatic(termSpec.compare);
			term.value = termSpec.value;
			term.orientation = termSpec.orientation;
			query.terms.push_back(term);
		}
		plan.queries.push_back(std::move(query));
	}
	plans.push_back(std::move(plan));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(key, handle);
	return handle;
}

void clearMediaConditionPlans()
{
	compiledMediaConditionPlans().clear();
	mediaConditionPlanCache().clear();
}

CssMediaTerm alwaysFalseMediaTerm()
{
	return {};
}

bool mediaFeatureKindForName(const std::string &name, CssMediaFeatureKind &kind, CssMediaCompare &compare)
{
	compare = CssMediaCompare::Equal;
	if (name == "orientation") {
		kind = CssMediaFeatureKind::Orientation;
		return true;
	}
	if (name == "monochrome") {
		kind = CssMediaFeatureKind::Monochrome;
		return true;
	}
	if (name == "min-monochrome") {
		kind = CssMediaFeatureKind::Monochrome;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-monochrome") {
		kind = CssMediaFeatureKind::Monochrome;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "width") {
		kind = CssMediaFeatureKind::Width;
		return true;
	}
	if (name == "min-width") {
		kind = CssMediaFeatureKind::Width;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-width") {
		kind = CssMediaFeatureKind::Width;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "height") {
		kind = CssMediaFeatureKind::Height;
		return true;
	}
	if (name == "min-height") {
		kind = CssMediaFeatureKind::Height;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-height") {
		kind = CssMediaFeatureKind::Height;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "aspect-ratio") {
		kind = CssMediaFeatureKind::AspectRatio;
		return true;
	}
	if (name == "min-aspect-ratio") {
		kind = CssMediaFeatureKind::AspectRatio;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-aspect-ratio") {
		kind = CssMediaFeatureKind::AspectRatio;
		compare = CssMediaCompare::Max;
		return true;
	}
	if (name == "resolution" || name == "device-pixel-ratio") {
		kind = CssMediaFeatureKind::Resolution;
		return true;
	}
	if (name == "min-resolution" || name == "min-device-pixel-ratio") {
		kind = CssMediaFeatureKind::Resolution;
		compare = CssMediaCompare::Min;
		return true;
	}
	if (name == "max-resolution" || name == "max-device-pixel-ratio") {
		kind = CssMediaFeatureKind::Resolution;
		compare = CssMediaCompare::Max;
		return true;
	}
	return false;
}

CssMediaTerm compileMediaFeatureTerm(const std::string &name, const std::string &value)
{
	CssMediaFeatureKind kind = CssMediaFeatureKind::AlwaysFalse;
	CssMediaCompare compare = CssMediaCompare::Equal;
	if (!mediaFeatureKindForName(name, kind, compare)) return alwaysFalseMediaTerm();

	CssMediaTerm term;
	term.kind = kind;
	term.compare = compare;
	if (kind == CssMediaFeatureKind::Orientation) {
		const std::string orientation = trimCssValue(value);
		if (orientation == "portrait") term.orientation = 1;
		else if (orientation == "landscape") term.orientation = 2;
		else return alwaysFalseMediaTerm();
		return term;
	}
	double parsed = 0.0;
	if (!parseMediaNumber(value, parsed)) return alwaysFalseMediaTerm();
	term.value = parsed;
	return term;
}

CssMediaQueryPlan compileMediaQueryPlan(const std::string &query)
{
	CssMediaQueryPlan plan;
	const std::string q = trimCssValue(query);
	if (q.empty()) return plan;
	plan.valid = true;
	std::size_t i = 0;
	while (true) {
		const std::size_t open = q.find('(', i);
		if (open == std::string::npos) break;
		const std::size_t close = q.find(')', open);
		if (close == std::string::npos) {
			plan.valid = false;
			plan.terms.clear();
			return plan;
		}
		const std::string feature = q.substr(open + 1, close - open - 1);
		const std::size_t colon = feature.find(':');
		if (colon != std::string::npos) {
			const std::string name = trimCssValue(feature.substr(0, colon));
			const std::string val = trimCssValue(feature.substr(colon + 1));
			plan.terms.push_back(compileMediaFeatureTerm(name, val));
		} else if (trimCssValue(feature) == "monochrome") {
			CssMediaTerm term;
			term.kind = CssMediaFeatureKind::Monochrome;
			term.compare = CssMediaCompare::Boolean;
			plan.terms.push_back(term);
		}
		i = close + 1;
	}
	return plan;
}

std::uint16_t compileMediaConditionPlan(const CssText &condition)
{
	if (condition.empty()) return kNoMediaConditionPlan;
	const std::string raw = condition.str();
	auto &cache = mediaConditionPlanCache();
	const auto cached = cache.find(raw);
	if (cached != cache.end()) return cached->second;
	auto &plans = compiledMediaConditionPlans();
	if (plans.size() >= kNoMediaConditionPlan) return kNoMediaConditionPlan;
	CssMediaConditionPlan plan;
	std::size_t start = 0;
	while (true) {
		const std::size_t comma = raw.find(',', start);
		plan.queries.push_back(compileMediaQueryPlan(raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
	plans.push_back(std::move(plan));
	const auto handle = static_cast<std::uint16_t>(plans.size() - 1);
	cache.emplace(raw, handle);
	return handle;
}

// Monochrome bits-per-pixel of the board's panel: 0 on color displays, 1 on
// 1-bit panels (e-paper). Boards set GEA_EMBEDDED_DISPLAY_MONOCHROME=1 in
// their target CMakeLists; this TU compiles per-target so no runtime plumbing
// is needed. Lets stylesheets theme e-ink boards via `@media (monochrome)`.
#ifndef GEA_EMBEDDED_DISPLAY_MONOCHROME
#define GEA_EMBEDDED_DISPLAY_MONOCHROME 0
#endif

bool mediaFeatureMatches(const std::string &name, const std::string &value)
{
	const double w = mediaLogicalWidth();
	const double h = mediaLogicalHeight();
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	if (name == "orientation") {
		const std::string v = trimCssValue(value);
		if (v == "portrait") return h >= w;
		if (v == "landscape") return w > h;
		return false;
	}
	double n = 0.0;
	if (!parseMediaNumber(value, n)) return false;
	constexpr double mono = GEA_EMBEDDED_DISPLAY_MONOCHROME;
	if (name == "monochrome") return mono == n;
	if (name == "min-monochrome") return mono >= n;
	if (name == "max-monochrome") return mono <= n;
	if (name == "min-width") return w >= n;
	if (name == "max-width") return w <= n;
	if (name == "width") return w == n;
	if (name == "min-height") return h >= n;
	if (name == "max-height") return h <= n;
	if (name == "height") return h == n;
	const double aspect = h != 0.0 ? w / h : 0.0;
	if (name == "min-aspect-ratio") return aspect >= n;
	if (name == "max-aspect-ratio") return aspect <= n;
	if (name == "aspect-ratio") return aspect == n;
	if (name == "min-resolution" || name == "min-device-pixel-ratio") return dpr >= n;
	if (name == "max-resolution" || name == "max-device-pixel-ratio") return dpr <= n;
	if (name == "resolution" || name == "device-pixel-ratio") return dpr == n;
	return false;  // unknown feature -> this query cannot match (CSS semantics)
}

// One query: parenthesized `(feature: value)` terms (AND), with media-type /
// `and` keywords ignored. All present features must match.
bool mediaQueryMatches(const std::string &query)
{
	const std::string q = trimCssValue(query);
	if (q.empty()) return false;
	std::size_t i = 0;
	while (true) {
		const std::size_t open = q.find('(', i);
		if (open == std::string::npos) break;
		const std::size_t close = q.find(')', open);
		if (close == std::string::npos) return false;
		const std::string feature = q.substr(open + 1, close - open - 1);
		const std::size_t colon = feature.find(':');
		if (colon != std::string::npos) {
			const std::string name = trimCssValue(feature.substr(0, colon));
			const std::string val = trimCssValue(feature.substr(colon + 1));
			if (!mediaFeatureMatches(name, val)) return false;
		} else if (trimCssValue(feature) == "monochrome") {
			// Boolean form `(monochrome)`: matches when the panel has any
			// monochrome bits. Other value-less terms stay ignored (the
			// pre-existing behavior) so legacy queries keep matching.
			if (GEA_EMBEDDED_DISPLAY_MONOCHROME <= 0) return false;
		}
		i = close + 1;
	}
	return true;
}

// Full condition: comma-separated queries are OR'd.
bool mediaConditionMatches(const std::string &condition)
{
	if (condition.empty()) return true;
	std::size_t start = 0;
	while (true) {
		const std::size_t comma = condition.find(',', start);
		const std::string query = condition.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		if (mediaQueryMatches(query)) return true;
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
	return false;
}

bool mediaConditionMatches(const CssText &condition)
{
	if (condition.empty()) return true;
	return mediaConditionMatches(condition.str());
}

bool compareMediaNumber(double actual, CssMediaCompare compare, double expected)
{
	switch (compare) {
	case CssMediaCompare::Equal: return actual == expected;
	case CssMediaCompare::Min: return actual >= expected;
	case CssMediaCompare::Max: return actual <= expected;
	case CssMediaCompare::Boolean: return actual > 0.0;
	}
	return false;
}

bool compiledMediaTermMatches(const CssMediaTerm &term)
{
	const double w = mediaLogicalWidth();
	const double h = mediaLogicalHeight();
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	constexpr double mono = GEA_EMBEDDED_DISPLAY_MONOCHROME;
	switch (term.kind) {
	case CssMediaFeatureKind::AlwaysFalse:
		return false;
	case CssMediaFeatureKind::Orientation:
		if (term.orientation == 1) return h >= w;
		if (term.orientation == 2) return w > h;
		return false;
	case CssMediaFeatureKind::Monochrome:
		return compareMediaNumber(mono, term.compare, term.value);
	case CssMediaFeatureKind::Width:
		return compareMediaNumber(w, term.compare, term.value);
	case CssMediaFeatureKind::Height:
		return compareMediaNumber(h, term.compare, term.value);
	case CssMediaFeatureKind::AspectRatio:
		return compareMediaNumber(h != 0.0 ? w / h : 0.0, term.compare, term.value);
	case CssMediaFeatureKind::Resolution:
		return compareMediaNumber(dpr, term.compare, term.value);
	}
	return false;
}

bool compiledMediaQueryMatches(const CssMediaQueryPlan &query)
{
	if (!query.valid) return false;
	for (const auto &term : query.terms)
		if (!compiledMediaTermMatches(term)) return false;
	return true;
}

bool compiledMediaConditionMatches(std::uint16_t handle)
{
	const auto &plans = compiledMediaConditionPlans();
	if (handle >= plans.size()) return false;
	const auto &condition = plans[handle];
	for (const auto &query : condition.queries)
		if (compiledMediaQueryMatches(query)) return true;
	return false;
}

bool ruleMediaMatchesUncached(const CssRule &rule)
{
	if (rule.mediaPlan != kNoMediaConditionPlan && rule.mediaPlan < compiledMediaConditionPlans().size())
		return compiledMediaConditionMatches(rule.mediaPlan);
	return mediaConditionMatches(cssRuleTextForHandle(rule.mediaText));
}

int findPseudoChild(int parent, CssRule::PseudoElement pseudo)
{
	const int16_t tag = pseudoTagId(pseudo);
	auto &state = treeState();
	if (parent < 0 || parent >= state.nodeCount) return -1;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (state.nodes[child].tag_id == tag) return child;
	}
	return -1;
}

int firstNonPseudoChild(int parent)
{
	auto &state = treeState();
	if (parent < 0 || parent >= state.nodeCount) return -1;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (!isGeneratedPseudoNode(state.nodes[child])) return child;
	}
	return -1;
}

bool nodeHasGeneratedPseudoChild(int parent)
{
	auto &state = treeState();
	if (parent < 0 || parent >= state.nodeCount) return false;
	for (int child = state.nodes[parent].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) return true;
	}
	return false;
}

int ensurePseudoChild(int parent, CssRule::PseudoElement pseudo)
{
	int existing = findPseudoChild(parent, pseudo);
	if (existing >= 0) return existing;
	Tree &tree = Tree::instance();
	const int child = tree.createView();
	if (child < 0) return -1;
	tree.setTagName(child, pseudo == CssRule::PseudoElement::Before ? "::before" : "::after");
	if (pseudo == CssRule::PseudoElement::Before)
		tree.insertBefore(child, parent, firstNonPseudoChild(parent));
	else
		tree.setParent(child, parent);
	return child;
}

static constexpr std::size_t kRuleCandidateCacheClassCapacity = 6;

struct RuleCandidateSignature {
	CssAtomId classes[kRuleCandidateCacheClassCapacity]{};
	CssAtomId idAtom = kInvalidCssAtom;
	int16_t tagId = -1;
	std::uint8_t classCount = 0;
	bool root = false;
};

class RuleCandidateList {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }
	bool empty() const { return size() == 0; }

	int &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const int &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) inline_[i] = 0;
		inlineCount_ = 0;
		spill_.clear();
	}

	void push_back(int rule)
	{
		if (inlineCount_ < kInlineCapacity) {
			inline_[inlineCount_++] = rule;
			return;
		}
		spill_.push_back(rule);
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_RULE_CANDIDATE_INLINE_RULES);
	std::array<int, kInlineCapacity> inline_{};
	std::vector<int> spill_;
	std::size_t inlineCount_ = 0;
};

struct RuleCandidateCacheEntry {
	CssAtomId classes[kRuleCandidateCacheClassCapacity]{};
	CssAtomId idAtom = kInvalidCssAtom;
	int16_t tagId = -1;
	std::uint8_t classCount = 0;
	bool root = false;
	bool signatureLocal = false;
	RuleCandidateList rules;
};

void resetRuleCandidateCacheEntry(RuleCandidateCacheEntry &entry)
{
	for (CssAtomId &cls : entry.classes) cls = kInvalidCssAtom;
	entry.idAtom = kInvalidCssAtom;
	entry.tagId = -1;
	entry.classCount = 0;
	entry.root = false;
	entry.signatureLocal = false;
	entry.rules.clear();
}

class RuleCandidateCacheStore {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }

	RuleCandidateCacheEntry &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const RuleCandidateCacheEntry &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	RuleCandidateCacheEntry &emplace_back()
	{
		if (inlineCount_ < kInlineCapacity) {
			RuleCandidateCacheEntry &entry = inline_[inlineCount_++];
			resetRuleCandidateCacheEntry(entry);
			return entry;
		}
		spill_.emplace_back();
		RuleCandidateCacheEntry &entry = spill_.back();
		resetRuleCandidateCacheEntry(entry);
		return entry;
	}

	RuleCandidateCacheEntry &back()
	{
		return spill_.empty() ? inline_[inlineCount_ - 1] : spill_.back();
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) resetRuleCandidateCacheEntry(inline_[i]);
		inlineCount_ = 0;
		spill_.clear();
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_RULE_CANDIDATE_INLINE_CACHE_ENTRIES);
	std::array<RuleCandidateCacheEntry, kInlineCapacity> inline_{};
	std::vector<RuleCandidateCacheEntry> spill_;
	std::size_t inlineCount_ = 0;
};

void sortRuleCandidateSignatureClasses(RuleCandidateSignature &signature)
{
	for (std::uint8_t i = 1; i < signature.classCount; ++i) {
		const CssAtomId cls = signature.classes[i];
		std::uint8_t j = i;
		while (j > 0 && signature.classes[j - 1] > cls) {
			signature.classes[j] = signature.classes[j - 1];
			--j;
		}
		signature.classes[j] = cls;
	}
}

constexpr int kVirtualBackgroundSizeWrite = static_cast<int>(Property::Count);
constexpr int kVirtualGridTemplateColumnsWrite = kVirtualBackgroundSizeWrite + 1;
constexpr int kVirtualGridTemplateRowsWrite = kVirtualBackgroundSizeWrite + 2;
constexpr int kPropertyWriteWords = (kVirtualGridTemplateRowsWrite + 64) / 64;
struct PropertyWriteMask {
	std::array<std::uint64_t, kPropertyWriteWords> words{};
	bool empty() const { for (auto word : words) if (word) return false; return true; }
	void addIndex(int index) {
		if (index < 0 || index >= kPropertyWriteWords * 64) return;
		words[index / 64] |= std::uint64_t{1} << (index % 64);
	}
	void add(Property property) { addIndex(static_cast<int>(property)); }
	void addAll(const PropertyWriteMask &other) {
		for (int i = 0; i < kPropertyWriteWords; ++i) words[i] |= other.words[i];
	}
	bool containsAll(const PropertyWriteMask &other) const {
		for (int i = 0; i < kPropertyWriteWords; ++i) if (other.words[i] & ~words[i]) return false;
		return true;
	}
};
static_assert(kVirtualGridTemplateRowsWrite < kPropertyWriteWords * 64, "PropertyWriteMask virtual bits overflow");

constexpr std::uint16_t kNoCachedStyleApplyOp = 0xFFFFu;

bool ruleWriteMask(const CssRule &rule, PropertyWriteMask &mask);

// Rule-matching index. A full class-style recompute is O(nodes x rules), and
// with a large stylesheet (the weather app registers ~686 rules) scanning every
// rule for every node dominates mount/recompute time. The index buckets rules by
// their selector key (class name / element tag) so a node only examines the rules
// that could apply to it (its classes + its tag) plus the complex-selector rules
// (which must always be checked via selectorMatchesNode). Candidates are emitted
// in cascade order: specificity first, then source order for ties.
// pseudoElement is NOT filtered here — each caller filters for the pseudo it wants.
struct RuleIndex {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	// Runtime initialization keeps the nonzero sentinels out of .data (see
	// state_init.h); noinline stops the compiler folding them back in.
	__attribute__((noinline)) RuleIndex() { valid = false; }
#endif

	DenseRuleBuckets byClass;
	DenseRuleBuckets byTag;
	// Complex (Selector-type) rules, bucketed by their RIGHTMOST simple selector's
	// primary key: a node can only match the full selector if it matches the
	// rightmost part, so a node only needs to test rules keyed by its own id,
	// classes, tag, or root-ness (plus selAlways for universal rightmosts).
	// selectorMatchesNode
	// still verifies the ancestor chain. This is what makes a near-root class change
	// (theme switch) cheap: a node tests ~its-own rules, not all ~136 selector rules.
	DenseRuleBuckets selById;
	DenseRuleBuckets selByClass;
	DenseRuleBuckets selByTag;
	std::vector<int> selRoot;
	std::vector<int> selAlways;
	std::vector<int> animationRules;  // rules with property == "animation" (any selector type)
	RuleCandidateCacheStore candidateCache;
	// Classes/tags that appear in a NON-rightmost simple selector of any complex rule
	// (i.e. as an ancestor matcher, like `.theme-night` in `.theme-night .icon`).
	// Used by the incremental recompute: a node whose class change touches one of
	// these keys may alter its DESCENDANTS' selector matches, so its subtree must be
	// recomputed in full — a custom-prop/inheritance diff alone wouldn't catch it.
	DenseIdSet ancestorClasses;
	DenseIdSet ancestorTags;
	// CSS specificity per rule (indexed by rule index), packed as
	// ids*10000 + (classes+pseudo-classes)*100 + (elements+pseudo-elements). Used to
	// order the cascade: a more-specific rule wins over a less-specific one
	// regardless of source order (real CSS), instead of pure source-order last-wins.
	std::vector<int> specificity;
	std::vector<PropertyWriteMask> writeMasks;
	std::vector<std::uint16_t> styleOps;
	// Scratch used while building an active rule plan. Reused across nodes so the
	// hot path avoids allocating a set just to remove rules reached through more
	// than one selector key. Serial 0 means "not seen".
	std::vector<std::uint16_t> candidateSeen;
	std::uint16_t candidateSerial = 0;
	// Media-query results are viewport-global. Cache one active bit per rule so a
	// recompute over many nodes does not re-evaluate the same @media expressions.
	std::vector<std::uint8_t> mediaMatches;
	int mediaWidth = -1;
	int mediaHeight = -1;
	int mediaDprMilli = -1;
	bool hasMediaConditions = false;
	bool hasPseudoElementRules = false;
	bool hasHoverRules = false;
	bool mediaCacheValid = false;
	bool valid = false;
};

RuleIndex g_ruleIndex;

std::size_t &ruleCandidateCacheLastHit()
{
	static std::size_t index = static_cast<std::size_t>(-1);
	return index;
}

void clearActiveRulePlanCache();
void clearCachedStyleApplyOps();
std::uint16_t cachedStyleApplyOpForCompiledValue(const CssCompiledValue *compiled);

// CSS specificity of a single rule's selector. Class -> one class; Element -> one
// element; a complex Selector sums its parts (each compound contributes its
// classes/ids/pseudo-classes/tag). Pseudo-elements (::before/::after) add an
// element-level unit. Inline styles and UA defaults are applied outside the
// cascade loop, so they keep their natural (highest / lowest) priority.
int computeRuleSpecificity(const CssRule &rule)
{
	int ids = 0, classes = 0, elements = 0;
	switch (rule.selectorType) {
	case CssRule::SelectorType::Class:
		classes = 1;
		break;
	case CssRule::SelectorType::Element:
		elements = 1;
		break;
	case CssRule::SelectorType::Selector: {
		const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
		if (plan) return plan->specificity +
		                  ((rule.pseudoElement == CssRule::PseudoElement::Before ||
		                    rule.pseudoElement == CssRule::PseudoElement::After ||
		                    rule.pseudoElement == CssRule::PseudoElement::FirstLine) ? 1 : 0);
		break;
	}
	}
	if (rule.pseudoElement == CssRule::PseudoElement::Before ||
	    rule.pseudoElement == CssRule::PseudoElement::After ||
	    rule.pseudoElement == CssRule::PseudoElement::FirstLine)
		elements += 1;
	return ids * 10000 + classes * 100 + elements;
}

void invalidateRuleIndex()
{
	g_ruleIndex.valid = false;
}

void rebuildRuleIndexIfNeeded()
{
	if (g_ruleIndex.valid) return;
	g_ruleIndex.byClass.clear();
	g_ruleIndex.byTag.clear();
	g_ruleIndex.selById.clear();
	g_ruleIndex.selByClass.clear();
	g_ruleIndex.selByTag.clear();
	g_ruleIndex.selRoot.clear();
	g_ruleIndex.selAlways.clear();
	g_ruleIndex.animationRules.clear();
	g_ruleIndex.candidateCache.clear();
	ruleCandidateCacheLastHit() = static_cast<std::size_t>(-1);
	clearActiveRulePlanCache();
	clearCachedStyleApplyOps();
	g_ruleIndex.ancestorClasses.clear();
	g_ruleIndex.ancestorTags.clear();
	const auto &list = rules();
	g_ruleIndex.specificity.assign(list.size(), 0);
	g_ruleIndex.writeMasks.assign(list.size(), {});
	g_ruleIndex.styleOps.assign(list.size(), kNoCachedStyleApplyOp);
	g_ruleIndex.candidateSeen.assign(list.size(), 0);
	g_ruleIndex.candidateSerial = 0;
	g_ruleIndex.mediaMatches.assign(list.size(), 0);
	g_ruleIndex.hasMediaConditions = false;
	g_ruleIndex.hasPseudoElementRules = false;
	g_ruleIndex.hasHoverRules = false;
	g_ruleIndex.mediaCacheValid = false;
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		g_ruleIndex.specificity[i] = computeRuleSpecificity(list[i]);
		PropertyWriteMask writeMask;
		if (ruleWriteMask(list[i], writeMask))
			g_ruleIndex.writeMasks[static_cast<std::size_t>(i)] = writeMask;
		g_ruleIndex.styleOps[static_cast<std::size_t>(i)] =
		    cachedStyleApplyOpForCompiledValue(compiledCssValueForHandle(list[i].compiledValue));
		switch (list[i].selectorType) {
		case CssRule::SelectorType::Class:
			if (list[i].selectorAtom != kInvalidCssAtom) g_ruleIndex.byClass.add(list[i].selectorAtom, i);
			break;
		case CssRule::SelectorType::Element:
			if (list[i].selectorTagId >= 0) g_ruleIndex.byTag.add(list[i].selectorTagId, i);
			break;
		case CssRule::SelectorType::Selector: {
			// Key by the rightmost simple selector's first class, else its tag, else
			// "always" (:root / universal / pseudo-only) — the node must match the
			// rightmost for the whole selector to match.
			const SelectorPlan *plan = selectorPlanForHandle(list[i].selectorPlan);
			if (plan && plan->valid) for (std::size_t p = 0; p < plan->parts.size(); ++p)
				if (plan->parts.at(p).simple.wantsHover) g_ruleIndex.hasHoverRules = true;
			if (!plan || !plan->valid || plan->parts.empty()) {
				g_ruleIndex.selAlways.push_back(i);
			} else {
				if (plan->rightmostId != kInvalidCssAtom)
					g_ruleIndex.selById.add(plan->rightmostId, i);
				else if (plan->rightmostClass != kInvalidCssAtom)
					g_ruleIndex.selByClass.add(plan->rightmostClass, i);
				else if (plan->rightmostTag >= 0)
					g_ruleIndex.selByTag.add(plan->rightmostTag, i);
				else if (plan->rightmostRoot)
					g_ruleIndex.selRoot.push_back(i);
				else g_ruleIndex.selAlways.push_back(i);
				// Every non-rightmost simple selector is an ancestor matcher: record its
				// classes/tag so the incremental recompute can detect when a node's class
				// change could flip a descendant's match.
				for (std::size_t ancestor = 0, count = plan->ancestorClasses.size(); ancestor < count; ++ancestor) {
					const CssAtomId cls = plan->ancestorClasses.at(ancestor);
					if (cls != kInvalidCssAtom) g_ruleIndex.ancestorClasses.insert(cls);
				}
				for (std::size_t ancestor = 0, count = plan->ancestorTags.size(); ancestor < count; ++ancestor) {
					const int16_t tag = plan->ancestorTags.at(ancestor);
					if (tag >= 0) g_ruleIndex.ancestorTags.insert(tag);
				}
			}
			break;
		}
		}
		if (list[i].mediaPlan != kNoMediaConditionPlan || list[i].mediaText != kNoCssRuleText)
			g_ruleIndex.hasMediaConditions = true;
		if (list[i].pseudoElement == CssRule::PseudoElement::Before ||
		    list[i].pseudoElement == CssRule::PseudoElement::After ||
		    list[i].pseudoElement == CssRule::PseudoElement::FirstLine)
			g_ruleIndex.hasPseudoElementRules = true;
		if (list[i].propertyKind == CssRuleProperty::Animation) g_ruleIndex.animationRules.push_back(i);
	}
	g_ruleIndex.byClass.finalize();
	g_ruleIndex.byTag.finalize();
	g_ruleIndex.selById.finalize();
	g_ruleIndex.selByClass.finalize();
	g_ruleIndex.selByTag.finalize();
	g_ruleIndex.valid = true;
}

int mediaDprMilliKey()
{
	const double dpr = g_device_pixel_ratio > 0.0 ? g_device_pixel_ratio : 1.0;
	return static_cast<int>(std::llround(dpr * 1000.0));
}

void rebuildRuleMediaCacheIfNeeded()
{
	rebuildRuleIndexIfNeeded();
	if (!g_ruleIndex.hasMediaConditions) return;
	const int dpr = mediaDprMilliKey();
	if (g_ruleIndex.mediaCacheValid &&
	    g_ruleIndex.mediaWidth == g_viewport_width &&
	    g_ruleIndex.mediaHeight == g_viewport_height &&
	    g_ruleIndex.mediaDprMilli == dpr)
		return;
	const auto &list = rules();
	if (g_ruleIndex.mediaMatches.size() != list.size())
		g_ruleIndex.mediaMatches.assign(list.size(), 0);
	for (int i = 0; i < static_cast<int>(list.size()); ++i)
		g_ruleIndex.mediaMatches[static_cast<std::size_t>(i)] =
		    ruleMediaMatchesUncached(list[static_cast<std::size_t>(i)]) ? 1u : 0u;
	clearActiveRulePlanCache();
	g_ruleIndex.mediaWidth = g_viewport_width;
	g_ruleIndex.mediaHeight = g_viewport_height;
	g_ruleIndex.mediaDprMilli = dpr;
	g_ruleIndex.mediaCacheValid = true;
}

bool ruleMediaMatchesIndex(int ruleIndex)
{
	rebuildRuleIndexIfNeeded();
	if (!g_ruleIndex.hasMediaConditions) return true;
	rebuildRuleMediaCacheIfNeeded();
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= g_ruleIndex.mediaMatches.size()) return false;
	return g_ruleIndex.mediaMatches[static_cast<std::size_t>(ruleIndex)] != 0;
}

std::uint16_t nextCandidateCollectSerial()
{
	std::uint16_t next = static_cast<std::uint16_t>(g_ruleIndex.candidateSerial + 1u);
	if (next == 0) {
		std::fill(g_ruleIndex.candidateSeen.begin(), g_ruleIndex.candidateSeen.end(), 0);
		next = 1;
	}
	g_ruleIndex.candidateSerial = next;
	return next;
}

CssAtomId nodeIdAttributeAtom(int node)
{
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->attributes.idAtom : kInvalidCssAtom;
}

enum ActiveRuleBucket : std::uint8_t {
	kActiveMainCustom,
	kActiveMainRule,
	kActiveBeforeCustom,
	kActiveBeforeRule,
	kActiveAfterCustom,
	kActiveAfterRule,
	kActiveFirstLineCustom,
	kActiveFirstLineRule,
	kActiveAnimation,
	kActiveRuleBucketCount
};

struct ActiveRulePlanCacheEntry;

struct ActiveRulePlan {
	struct Bucket {
		static constexpr std::size_t kInlineRuleCapacity = 24;
		int inlineRules[kInlineRuleCapacity];
		int *spillRules = nullptr;
		std::size_t spillCount = 0;
		std::size_t spillCapacity = 0;
		std::uint8_t count = 0;
		bool spilled = false;

		~Bucket()
		{
			delete[] spillRules;
		}

		void clear()
		{
			if (spilled)
				spillCount = 0;
			else
				count = 0;
		}

			std::size_t size() const { return spilled ? spillCount : count; }
			bool empty() const { return size() == 0; }
			const int *data() const { return spilled ? spillRules : inlineRules; }

			void push(int rule)
		{
			if (!spilled && count < kInlineRuleCapacity) {
				inlineRules[count++] = rule;
				return;
			}
			if (!spilled) {
				spillCapacity = kInlineRuleCapacity * 2;
				spillRules = new int[spillCapacity];
				for (std::size_t i = 0; i < count; ++i)
					spillRules[i] = inlineRules[i];
				spillCount = count;
				spilled = true;
			}
			if (spillCount >= spillCapacity) {
				const std::size_t nextCapacity = spillCapacity ? spillCapacity * 2 : kInlineRuleCapacity * 2;
				int *next = new int[nextCapacity];
				for (std::size_t i = 0; i < spillCount; ++i)
					next[i] = spillRules[i];
				delete[] spillRules;
				spillRules = next;
				spillCapacity = nextCapacity;
			}
			spillRules[spillCount++] = rule;
		}

		int at(std::size_t index) const
		{
			return spilled ? spillRules[index] : inlineRules[index];
		}

		void set(std::size_t index, int rule)
		{
			if (spilled)
				spillRules[index] = rule;
			else
				inlineRules[index] = rule;
		}
	};

	Bucket buckets[kActiveRuleBucketCount];
	const ActiveRulePlanCacheEntry *cachedEntry = nullptr;

	void clear()
	{
		cachedEntry = nullptr;
		for (auto &bucket : buckets) bucket.clear();
	}

	bool has(int bucket) const;

	void push(int bucket, int rule)
	{
		if (bucket < 0 || bucket >= kActiveRuleBucketCount) return;
		cachedEntry = nullptr;
		buckets[bucket].push(rule);
	}
};

int activeRuleBucketFor(const CssRule &rule);

constexpr std::uint16_t kNoCachedRuleIndex = 0xFFFFu;
constexpr std::uint8_t kCachedStyleApplyOpPropertyCapacity = 4;
constexpr std::uint8_t kCachedTransformValueCount = 11;
constexpr std::uint8_t kActiveRulePlanCacheRuleCapacity = 96;

enum class CachedStyleApplyOpKind : std::uint8_t {
	DirectProperties,
	Noop,
	Transform,
	CompiledTransform,
	Color,
	ColorVar,
	Background,
	StaticBackground,
	BackgroundSize,
	RuntimeLength,
	RuntimeSize,
	RuntimePositionOffset,
	RuntimeBackgroundSize,
	RuntimeFlex,
	RuntimeFlexBasis,
	RuntimeBorderShorthand,
	RuntimeBorderSideShorthand,
	RuntimeFilterBlur,
	RuntimeLineHeight,
	CompiledLength,
	CompiledSize,
	CompiledPositionOffset,
	CompiledBox,
	CompiledBackgroundSize,
	CompiledFlex,
	CompiledFlexBasis,
	CompiledBorderShorthand,
	CompiledBorderSideShorthand,
	CompiledBorderRadius,
	CompiledFilterBlur,
	CompiledBoxShadow,
	CompiledLineHeight,
	GridTemplate
};

struct CachedStyleApplyOp {
	CachedStyleApplyOpKind kind = CachedStyleApplyOpKind::DirectProperties;
	std::uint8_t propertyCount = 0;
	std::uint8_t declaration = 0;
	std::uint8_t borderRelief = 0;
	std::uint8_t properties[kCachedStyleApplyOpPropertyCapacity]{};
	std::int32_t values[kCachedStyleApplyOpPropertyCapacity]{};
	std::int16_t transform[kCachedTransformValueCount]{};
};

class CachedStyleApplyOpStore {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }

	CachedStyleApplyOp &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const CachedStyleApplyOp &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	void push_back(const CachedStyleApplyOp &op)
	{
		if (inlineCount_ < kInlineCapacity) {
			inline_[inlineCount_++] = op;
			return;
		}
		spill_.push_back(op);
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) inline_[i] = CachedStyleApplyOp{};
		inlineCount_ = 0;
		spill_.clear();
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_CACHED_STYLE_APPLY_INLINE_OPS);
	std::array<CachedStyleApplyOp, kInlineCapacity> inline_{};
	std::vector<CachedStyleApplyOp> spill_;
	std::size_t inlineCount_ = 0;
};

struct CachedRuleApply {
	std::uint16_t styleOp = kNoCachedStyleApplyOp;
	std::uint16_t ruleIndex = kNoCachedRuleIndex;
	std::uint16_t compiledValue = kNoCompiledCssValue;
};

struct ActiveRulePlanCacheEntry {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	CssAtomId classes[kRuleCandidateCacheClassCapacity];
	CssAtomId idAtom;
	int16_t tagId;
	std::uint8_t classCount;
	bool root;
	std::uint8_t bucketCounts[kActiveRuleBucketCount];
	std::uint8_t bucketOffsets[kActiveRuleBucketCount];
	CachedRuleApply rules[kActiveRulePlanCacheRuleCapacity];

	// Runtime initialization keeps the nonzero sentinels out of .data (see
	// state_init.h). Defined below resetActiveRulePlanCacheEntry, which it uses.
	ActiveRulePlanCacheEntry();
#else
	CssAtomId classes[kRuleCandidateCacheClassCapacity]{};
	CssAtomId idAtom = kInvalidCssAtom;
	int16_t tagId = -1;
	std::uint8_t classCount = 0;
	bool root = false;
	std::uint8_t bucketCounts[kActiveRuleBucketCount]{};
	std::uint8_t bucketOffsets[kActiveRuleBucketCount]{};
	CachedRuleApply rules[kActiveRulePlanCacheRuleCapacity]{};
#endif
};

void resetActiveRulePlanCacheEntry(ActiveRulePlanCacheEntry &entry)
{
	for (CssAtomId &cls : entry.classes) cls = kInvalidCssAtom;
	entry.idAtom = kInvalidCssAtom;
	entry.tagId = -1;
	entry.classCount = 0;
	entry.root = false;
	for (std::uint8_t &count : entry.bucketCounts) count = 0;
	for (std::uint8_t &offset : entry.bucketOffsets) offset = 0;
	for (CachedRuleApply &rule : entry.rules) {
		rule.styleOp = kNoCachedStyleApplyOp;
		rule.ruleIndex = kNoCachedRuleIndex;
		rule.compiledValue = kNoCompiledCssValue;
	}
}

#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
// noinline stops the compiler folding the reset back into a .data image.
__attribute__((noinline)) ActiveRulePlanCacheEntry::ActiveRulePlanCacheEntry()
{
	resetActiveRulePlanCacheEntry(*this);
}
#endif

class ActiveRulePlanCacheStore {
public:
	std::size_t size() const { return inlineCount_ + spill_.size(); }

	ActiveRulePlanCacheEntry &operator[](std::size_t index)
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	const ActiveRulePlanCacheEntry &operator[](std::size_t index) const
	{
		return index < inlineCount_ ? inline_[index] : spill_[index - inlineCount_];
	}

	ActiveRulePlanCacheEntry &emplace_back()
	{
		if (inlineCount_ < kInlineCapacity) {
			ActiveRulePlanCacheEntry &entry = inline_[inlineCount_++];
			resetActiveRulePlanCacheEntry(entry);
			return entry;
		}
		spill_.emplace_back();
		ActiveRulePlanCacheEntry &entry = spill_.back();
		resetActiveRulePlanCacheEntry(entry);
		return entry;
	}

	ActiveRulePlanCacheEntry &back()
	{
		return spill_.empty() ? inline_[inlineCount_ - 1] : spill_.back();
	}

	void pop_back()
	{
		if (!spill_.empty()) {
			spill_.pop_back();
			return;
		}
		if (inlineCount_ == 0) return;
		resetActiveRulePlanCacheEntry(inline_[--inlineCount_]);
	}

	void clear()
	{
		for (std::size_t i = 0; i < inlineCount_; ++i) resetActiveRulePlanCacheEntry(inline_[i]);
		inlineCount_ = 0;
		spill_.clear();
	}

private:
	static constexpr std::size_t kInlineCapacity =
	    static_cast<std::size_t>(GEA_CSS_ACTIVE_RULE_PLAN_INLINE_CACHE_ENTRIES);
	std::array<ActiveRulePlanCacheEntry, kInlineCapacity> inline_{};
	std::vector<ActiveRulePlanCacheEntry> spill_;
	std::size_t inlineCount_ = 0;
};

ActiveRulePlanCacheStore &activeRulePlanCache()
{
	static ActiveRulePlanCacheStore cache;
	return cache;
}

std::size_t &activeRulePlanCacheLastHit()
{
	static std::size_t index = static_cast<std::size_t>(-1);
	return index;
}

CachedStyleApplyOpStore &cachedStyleApplyOps()
{
	static CachedStyleApplyOpStore ops;
	return ops;
}

void clearCachedStyleApplyOps()
{
	cachedStyleApplyOps().clear();
}

void clearActiveRulePlanCache()
{
	activeRulePlanCache().clear();
	activeRulePlanCacheLastHit() = static_cast<std::size_t>(-1);
}

bool activeRulePlanCacheEntryMatches(const ActiveRulePlanCacheEntry &entry,
                                     const RuleCandidateSignature &signature)
{
	if (entry.idAtom != signature.idAtom ||
	    entry.tagId != signature.tagId ||
	    entry.classCount != signature.classCount ||
	    entry.root != signature.root)
		return false;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		if (entry.classes[i] != signature.classes[i]) return false;
	return true;
}

const ActiveRulePlanCacheEntry *cachedActiveRulePlanForSignature(const RuleCandidateSignature &signature)
{
	const auto &cache = activeRulePlanCache();
	std::size_t &lastHit = activeRulePlanCacheLastHit();
	if (lastHit < cache.size() && activeRulePlanCacheEntryMatches(cache[lastHit], signature))
		return &cache[lastHit];
	for (std::size_t i = 0; i < cache.size(); ++i) {
		if (!activeRulePlanCacheEntryMatches(cache[i], signature)) continue;
		lastHit = i;
		return &cache[i];
	}
	return nullptr;
}

void setActiveRulePlanFromCache(ActiveRulePlan &plan, const ActiveRulePlanCacheEntry &entry)
{
	plan.clear();
	plan.cachedEntry = &entry;
}

std::size_t activeRulePlanBucketSize(const ActiveRulePlan &plan, int bucket)
{
	if (bucket < 0 || bucket >= kActiveRuleBucketCount) return 0;
	if (plan.cachedEntry) return plan.cachedEntry->bucketCounts[bucket];
	return plan.buckets[bucket].size();
}

bool ActiveRulePlan::has(int bucket) const
{
	return activeRulePlanBucketSize(*this, bucket) != 0;
}

DenseRuleBucketSpan activeRulePlanBucketSpan(const ActiveRulePlan &plan, int bucket)
{
	if (bucket < 0 || bucket >= kActiveRuleBucketCount) return {};
	if (plan.cachedEntry) return {};
	const ActiveRulePlan::Bucket &local = plan.buckets[bucket];
	const std::size_t count = local.size();
	return count == 0 ? DenseRuleBucketSpan{} : DenseRuleBucketSpan{local.data(), count};
}

struct CachedRuleApplyBucketSpan {
	const CachedRuleApply *data = nullptr;
	std::size_t count = 0;
};

CachedRuleApplyBucketSpan activeRulePlanCachedRuleSpan(const ActiveRulePlan &plan, int bucket)
{
	if (!plan.cachedEntry || bucket < 0 || bucket >= kActiveRuleBucketCount) return {};
	const std::size_t count = plan.cachedEntry->bucketCounts[bucket];
	return count == 0
	    ? CachedRuleApplyBucketSpan{}
	    : CachedRuleApplyBucketSpan{plan.cachedEntry->rules + plan.cachedEntry->bucketOffsets[bucket], count};
}

bool activeRulePlanFitsCache(const ActiveRulePlan &plan)
{
	std::size_t total = 0;
	for (const auto &bucket : plan.buckets) total += bucket.size();
	return total <= kActiveRulePlanCacheRuleCapacity;
}

void copyActiveRulePlanSignature(ActiveRulePlanCacheEntry &entry,
                                 const RuleCandidateSignature &signature)
{
	entry.idAtom = signature.idAtom;
	entry.tagId = signature.tagId;
	entry.classCount = signature.classCount;
	entry.root = signature.root;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		entry.classes[i] = signature.classes[i];
}

bool addCachedStyleApplyProperty(CachedStyleApplyOp &op, Property property, int value)
{
	const int propertyIndex = static_cast<int>(property);
	if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
	if (op.propertyCount >= kCachedStyleApplyOpPropertyCapacity) return false;
	op.properties[op.propertyCount] = static_cast<std::uint8_t>(propertyIndex);
	op.values[op.propertyCount] = value;
	op.propertyCount++;
	return true;
}

std::int32_t cachedLengthValueBits(float value)
{
	std::int32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

float cachedLengthValueFromBits(std::int32_t bits)
{
	float value = 0.0f;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

void storeCachedLengthSpec(CachedStyleApplyOp &op, int valueSlot, const CssLengthSpec &length)
{
	if (valueSlot < 0 || valueSlot + 1 >= static_cast<int>(kCachedStyleApplyOpPropertyCapacity)) return;
	op.values[valueSlot] = cachedLengthValueBits(length.value);
	op.values[valueSlot + 1] = static_cast<std::int32_t>(length.unit);
}

CssLengthSpec loadCachedLengthSpec(const CachedStyleApplyOp &op, int valueSlot)
{
	CssLengthSpec length;
	if (valueSlot < 0 || valueSlot + 1 >= static_cast<int>(kCachedStyleApplyOpPropertyCapacity)) return length;
	length.value = cachedLengthValueFromBits(op.values[valueSlot]);
	length.unit = static_cast<CssLengthUnit>(op.values[valueSlot + 1]);
	return length;
}

bool buildRuntimeLengthApplyOp(CachedStyleApplyOp &op,
                               CachedStyleApplyOpKind kind,
                               CssDeclarationId declaration,
                               const CssLengthSpec &length)
{
	op = CachedStyleApplyOp{};
	op.kind = kind;
	op.declaration = static_cast<std::uint8_t>(declaration);
	storeCachedLengthSpec(op, 0, length);
	return true;
}

bool fixedCachedLengthValue(const CssLengthSpec &length, int &value)
{
	switch (length.unit) {
	case CssLengthUnit::Raw:
		value = rawNumber(length.value);
		return true;
	case CssLengthUnit::Px:
		value = cssPixelLength(length.value);
		return true;
	default:
		return false;
	}
}

bool fixedCachedBorderWidth(const CssLengthSpec &length, int &value)
{
	if (length.unit != CssLengthUnit::Raw && length.unit != CssLengthUnit::Px) return false;
	value = resolveBorderWidth(length, -1);
	return value >= 0;
}

bool percentCachedLengthValue(const CssLengthSpec &length, int &value)
{
	if (length.unit != CssLengthUnit::Percent) return false;
	value = roundToInt(static_cast<double>(length.value) * 10.0);
	return true;
}

bool addCachedUniformBorderWidth(CachedStyleApplyOp &op, int width)
{
	if (!addCachedStyleApplyProperty(op, Property::BorderWidth, width)) return false;
	return true;
}

bool addCachedLengthDeclaration(CachedStyleApplyOp &op, CssDeclarationId declaration, const CssLengthSpec &length)
{
	int value = 0;
	if (isBorderWidthDeclaration(declaration)) {
		if (!fixedCachedBorderWidth(length, value)) return false;
	} else if (!fixedCachedLengthValue(length, value)) return false;
	switch (declaration) {
	case CssDeclarationId::Gap: return addCachedStyleApplyProperty(op, Property::Gap, value);
	case CssDeclarationId::MinWidth: return addCachedStyleApplyProperty(op, Property::MinWidth, value);
	case CssDeclarationId::MinHeight: return addCachedStyleApplyProperty(op, Property::MinHeight, value);
	case CssDeclarationId::MaxWidth: return addCachedStyleApplyProperty(op, Property::MaxWidth, value);
	case CssDeclarationId::MaxHeight: return addCachedStyleApplyProperty(op, Property::MaxHeight, value);
	case CssDeclarationId::PaddingTop: return addCachedStyleApplyProperty(op, Property::PaddingTop, value);
	case CssDeclarationId::PaddingRight: return addCachedStyleApplyProperty(op, Property::PaddingRight, value);
	case CssDeclarationId::PaddingBottom: return addCachedStyleApplyProperty(op, Property::PaddingBottom, value);
	case CssDeclarationId::PaddingLeft: return addCachedStyleApplyProperty(op, Property::PaddingLeft, value);
	case CssDeclarationId::MarginTop: return addCachedStyleApplyProperty(op, Property::MarginTop, value);
	case CssDeclarationId::MarginRight: return addCachedStyleApplyProperty(op, Property::MarginRight, value);
	case CssDeclarationId::MarginBottom: return addCachedStyleApplyProperty(op, Property::MarginBottom, value);
	case CssDeclarationId::MarginLeft: return addCachedStyleApplyProperty(op, Property::MarginLeft, value);
	case CssDeclarationId::BorderWidth: return addCachedUniformBorderWidth(op, value);
	case CssDeclarationId::BorderTopWidth: return addCachedStyleApplyProperty(op, Property::BorderTopWidth, value);
	case CssDeclarationId::BorderRightWidth: return addCachedStyleApplyProperty(op, Property::BorderRightWidth, value);
	case CssDeclarationId::BorderBottomWidth: return addCachedStyleApplyProperty(op, Property::BorderBottomWidth, value);
	case CssDeclarationId::BorderLeftWidth: return addCachedStyleApplyProperty(op, Property::BorderLeftWidth, value);
	case CssDeclarationId::FontSize: return addCachedStyleApplyProperty(op, Property::FontSize, value);
	case CssDeclarationId::Perspective: return addCachedStyleApplyProperty(op, Property::Perspective, value);
	case CssDeclarationId::MaskImage:
		return addCachedStyleApplyProperty(op, Property::MaskRightFadeWidth, std::max(0, value));
	default:
		return false;
	}
}

bool addCachedSizeDeclaration(CachedStyleApplyOp &op,
                              CssDeclarationId declaration,
                              const CssLengthSpec &length)
{
	const bool width = declaration == CssDeclarationId::Width;
	const bool height = declaration == CssDeclarationId::Height;
	if (!width && !height) return false;
	int value = 0;
	if (length.unit == CssLengthUnit::Auto) {
		return addCachedStyleApplyProperty(op, width ? Property::Width : Property::Height, kUnset) &&
		       addCachedStyleApplyProperty(op, width ? Property::WidthPercent : Property::HeightPercent, kUnset);
	}
	if (percentCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, width ? Property::WidthPercent : Property::HeightPercent, value);
	if (fixedCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, width ? Property::Width : Property::Height, value);
	return false;
}

bool addCachedPositionDeclaration(CachedStyleApplyOp &op,
                                  CssDeclarationId declaration,
                                  const CssLengthSpec &length)
{
	Property lengthProperty = Property::Top;
	Property percentProperty = Property::TopPercent;
	switch (declaration) {
	case CssDeclarationId::Top: lengthProperty = Property::Top; percentProperty = Property::TopPercent; break;
	case CssDeclarationId::Right: lengthProperty = Property::Right; percentProperty = Property::RightPercent; break;
	case CssDeclarationId::Bottom: lengthProperty = Property::Bottom; percentProperty = Property::BottomPercent; break;
	case CssDeclarationId::Left: lengthProperty = Property::Left; percentProperty = Property::LeftPercent; break;
	default: return false;
	}
	int value = 0;
	if (percentCachedLengthValue(length, value)) return addCachedStyleApplyProperty(op, percentProperty, value);
	if (fixedCachedLengthValue(length, value)) return addCachedStyleApplyProperty(op, lengthProperty, value);
	return false;
}

bool addCachedBorderRadiusCorner(CachedStyleApplyOp &op, int corner, const CssLengthSpec &length)
{
	int value = 0;
	if (percentCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, borderRadiusPercentProperty(corner), value);
	if (fixedCachedLengthValue(length, value))
		return addCachedStyleApplyProperty(op, borderRadiusLengthProperty(corner), value);
	return false;
}

bool addCachedBorderRadiusDeclaration(CachedStyleApplyOp &op,
                                      CssDeclarationId declaration,
                                      const CssCompiledValue &compiled)
{
	if (declaration == CssDeclarationId::BorderRadius) {
		for (int corner = 0; corner < 4; ++corner)
			if (!addCachedBorderRadiusCorner(op, corner, compiled.lengths[corner])) return false;
		return true;
	}
	if (declaration == CssDeclarationId::BorderTopLeftRadius)
		return addCachedBorderRadiusCorner(op, 0, compiled.lengths[0]);
	if (declaration == CssDeclarationId::BorderTopRightRadius)
		return addCachedBorderRadiusCorner(op, 1, compiled.lengths[0]);
	if (declaration == CssDeclarationId::BorderBottomRightRadius)
		return addCachedBorderRadiusCorner(op, 2, compiled.lengths[0]);
	if (declaration == CssDeclarationId::BorderBottomLeftRadius)
		return addCachedBorderRadiusCorner(op, 3, compiled.lengths[0]);
	return false;
}

bool addCachedBoxDeclaration(CachedStyleApplyOp &op,
                             CssDeclarationId declaration,
                             const CssCompiledValue &compiled)
{
	Property properties[4]{};
	switch (declaration) {
	case CssDeclarationId::Padding:
		properties[0] = Property::PaddingTop;
		properties[1] = Property::PaddingRight;
		properties[2] = Property::PaddingBottom;
		properties[3] = Property::PaddingLeft;
		break;
	case CssDeclarationId::Margin:
		properties[0] = Property::MarginTop;
		properties[1] = Property::MarginRight;
		properties[2] = Property::MarginBottom;
		properties[3] = Property::MarginLeft;
		break;
	case CssDeclarationId::Inset:
		properties[0] = Property::Top;
		properties[1] = Property::Right;
		properties[2] = Property::Bottom;
		properties[3] = Property::Left;
		break;
	default:
		return false;
	}
	int values[4]{};
	for (int i = 0; i < 4; ++i)
		if (!fixedCachedLengthValue(compiled.lengths[i], values[i])) return false;
	for (int i = 0; i < 4; ++i)
		if (!addCachedStyleApplyProperty(op, properties[i], values[i])) return false;
	return true;
}

bool addCachedKeywordDeclaration(CachedStyleApplyOp &op, CssDeclarationId declaration, int value)
{
	switch (declaration) {
	case CssDeclarationId::Display: return addCachedStyleApplyProperty(op, Property::Display, value);
	case CssDeclarationId::ObjectFit: return addCachedStyleApplyProperty(op, Property::ImageFit, value);
	case CssDeclarationId::FlexDirection: return addCachedStyleApplyProperty(op, Property::FlexDirection, value);
	case CssDeclarationId::FlexWrap: return addCachedStyleApplyProperty(op, Property::FlexWrap, value);
	case CssDeclarationId::JustifyContent: return addCachedStyleApplyProperty(op, Property::JustifyContent, value);
	case CssDeclarationId::AlignItems: return addCachedStyleApplyProperty(op, Property::AlignItems, value);
	case CssDeclarationId::JustifyItems: return addCachedStyleApplyProperty(op, Property::JustifyItems, value);
	case CssDeclarationId::AlignContent: return addCachedStyleApplyProperty(op, Property::AlignContent, value);
	case CssDeclarationId::JustifySelf: return addCachedStyleApplyProperty(op, Property::JustifySelf, value);
	case CssDeclarationId::GridRowStart: return addCachedStyleApplyProperty(op, Property::GridRowStart, value);
	case CssDeclarationId::GridColumnStart: return addCachedStyleApplyProperty(op, Property::GridColumnStart, value);
	case CssDeclarationId::GridRowEnd: return addCachedStyleApplyProperty(op, Property::GridRowEnd, value);
	case CssDeclarationId::GridColumnEnd: return addCachedStyleApplyProperty(op, Property::GridColumnEnd, value);
	case CssDeclarationId::AlignSelf: return addCachedStyleApplyProperty(op, Property::AlignSelf, value);
	case CssDeclarationId::Position: return addCachedStyleApplyProperty(op, Property::Position, value);
	case CssDeclarationId::TextAlign: return addCachedStyleApplyProperty(op, Property::TextAlign, value);
	case CssDeclarationId::TextDecoration: return addCachedStyleApplyProperty(op, Property::TextDecoration, value);
	case CssDeclarationId::TextTransform: return addCachedStyleApplyProperty(op, Property::TextTransform, value);
	case CssDeclarationId::WhiteSpace: return addCachedStyleApplyProperty(op, Property::WhiteSpace, value);
	case CssDeclarationId::TextOverflow: return addCachedStyleApplyProperty(op, Property::TextOverflow, value);
	case CssDeclarationId::TransformStyle: return addCachedStyleApplyProperty(op, Property::TransformStyle, value);
	case CssDeclarationId::Visibility: return addCachedStyleApplyProperty(op, Property::Visibility, value);
	case CssDeclarationId::BackfaceVisibility: return addCachedStyleApplyProperty(op, Property::Backface, value);
	case CssDeclarationId::PointerEvents: return addCachedStyleApplyProperty(op, Property::PointerEvents, value);
	case CssDeclarationId::Overflow: return addCachedStyleApplyProperty(op, Property::Overflow, value);
	case CssDeclarationId::OverflowX: return addCachedStyleApplyProperty(op, Property::OverflowX, value);
	case CssDeclarationId::OverflowY: return addCachedStyleApplyProperty(op, Property::OverflowY, value);
	case CssDeclarationId::FontFamily: return addCachedStyleApplyProperty(op, Property::FontId, value);
	case CssDeclarationId::FontWeight: return addCachedStyleApplyProperty(op, Property::FontWeight, value);
	default: return false;
	}
}

std::int16_t cachedInt16Value(int value)
{
	return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

bool cachedTransformTranslateValue(const CssLengthSpec &length,
                                   bool allowPercent,
                                   int &px,
                                   int &percent)
{
	px = 0;
	percent = 0;
	if (allowPercent && percentCachedLengthValue(length, percent)) return true;
	return fixedCachedLengthValue(length, px);
}

bool buildCachedTransformApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Transform) return false;
	TransformComponents transform;
	transform.rotateX = compiled.values[0];
	transform.rotateY = compiled.values[1];
	transform.rotateZ = compiled.values[2];
	transform.scaleX = compiled.values[8];
	transform.scaleY = compiled.values[9];
	transform.scaleZ = compiled.values[10];
	if ((compiled.flags & (1u << 3)) != 0 &&
	    !cachedTransformTranslateValue(compiled.lengths[0], true, transform.translateX, transform.translateXPercent))
		return false;
	if ((compiled.flags & (1u << 4)) != 0 &&
	    !cachedTransformTranslateValue(compiled.lengths[1], true, transform.translateY, transform.translateYPercent))
		return false;
	if ((compiled.flags & (1u << 5)) != 0) {
		int unusedPercent = 0;
		if (!cachedTransformTranslateValue(compiled.lengths[2], false, transform.translateZ, unusedPercent)) return false;
	}

	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::Transform;
	op.values[0] = compiled.flags != 0;
	op.values[1] = (compiled.flags >> 10) & 7;
	op.transform[0] = cachedInt16Value(transform.rotateX);
	op.transform[1] = cachedInt16Value(transform.rotateY);
	op.transform[2] = cachedInt16Value(transform.rotateZ);
	op.transform[3] = cachedInt16Value(transform.translateX);
	op.transform[4] = cachedInt16Value(transform.translateY);
	op.transform[5] = cachedInt16Value(transform.translateZ);
	op.transform[6] = cachedInt16Value(transform.translateXPercent);
	op.transform[7] = cachedInt16Value(transform.translateYPercent);
	op.transform[8] = cachedInt16Value(transform.scaleX);
	op.transform[9] = cachedInt16Value(transform.scaleY);
	op.transform[10] = cachedInt16Value(transform.scaleZ);
	return true;
}

bool buildCachedCompiledTransformApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Transform) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::CompiledTransform;
	return true;
}

bool buildCachedColorApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Color) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::Color;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = compiled.values[0];
	op.values[1] = compiled.values[1];
	op.values[2] = compiled.values[2];
	return true;
}

bool buildCachedColorVarApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::ColorVar) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::ColorVar;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = compiled.values[0];
	op.values[1] = compiled.values[1];
	op.values[2] = compiled.values[2];
	op.values[3] = compiled.aux == 0 ? -1 : compiled.values[3];
	return true;
}

bool compiledLinearGradientIsStaticLiteral(const CssCompiledLinearGradient &gradient)
{
	return gradient.fromColorAtom == kInvalidCssAtom &&
	       gradient.midColorAtom == kInvalidCssAtom &&
	       gradient.toColorAtom == kInvalidCssAtom &&
	       gradient.fromColorHasFallback == 0 &&
	       gradient.midColorHasFallback == 0 &&
	       gradient.toColorHasFallback == 0;
}

bool compiledRadialGradientIsStaticLiteral(const CssCompiledRadialGradient &gradient)
{
	return gradient.fromColorAtom == kInvalidCssAtom &&
	       gradient.toColorAtom == kInvalidCssAtom &&
	       gradient.fromColorHasFallback == 0 &&
	       gradient.toColorHasFallback == 0;
}

bool cachedStaticGridLineWidth(const CssLengthSpec &length, std::uint8_t &out)
{
	int px = 0;
	if (!fixedCachedLengthValue(length, px)) return false;
	if (px <= 0) {
		out = 0;
		return true;
	}
	out = static_cast<std::uint8_t>(std::max(1, std::min(px, 255)));
	return true;
}

bool buildCachedStaticBackgroundApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Background) return false;
	const std::uint16_t handle = static_cast<std::uint16_t>(compiled.values[0]);
	const CssCompiledBackground *background = compiledCssBackgroundForHandle(handle);
	if (!background || !background->hasGradient) return false;
	if (!compiledLinearGradientIsStaticLiteral(background->gradient)) return false;
	if (background->hasOverlayGradient &&
	    !compiledLinearGradientIsStaticLiteral(background->overlayGradient))
		return false;
	if (background->hasRadialGradient &&
	    !compiledRadialGradientIsStaticLiteral(background->radialGradient))
		return false;

	std::uint8_t gridAxes = background->gridAxes;
	std::uint8_t gridLineX = 0;
	std::uint8_t gridLineY = 0;
	if (background->hasGridLineX) {
		if (!cachedStaticGridLineWidth(background->gridLineX, gridLineX)) return false;
		if (gridLineX == 0) gridAxes &= static_cast<std::uint8_t>(~1u);
	}
	if (background->hasGridLineY) {
		if (!cachedStaticGridLineWidth(background->gridLineY, gridLineY)) return false;
		if (gridLineY == 0) gridAxes &= static_cast<std::uint8_t>(~2u);
	}

	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::StaticBackground;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = handle;
	op.values[1] = gridAxes;
	op.values[2] = gridLineX;
	op.values[3] = gridLineY;
	return true;
}

bool buildCachedBackgroundApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::Background) return false;
	if (buildCachedStaticBackgroundApplyOp(compiled, op)) return true;
	const std::uint16_t handle = static_cast<std::uint16_t>(compiled.values[0]);
	if (!compiledCssBackgroundForHandle(handle)) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::Background;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = handle;
	return true;
}

bool buildCachedGridTemplateApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::GridTemplate) return false;
	if (compiled.declaration != CssDeclarationId::GridTemplateColumns &&
	    compiled.declaration != CssDeclarationId::GridTemplateRows)
		return false;
	const std::uint16_t handle = static_cast<std::uint16_t>(compiled.values[0]);
	if (!compiledCssGridTemplateForHandle(handle)) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::GridTemplate;
	op.declaration = static_cast<std::uint8_t>(compiled.declaration);
	op.values[0] = handle;
	return true;
}

bool buildCachedBackgroundSizeApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::BackgroundSize) return false;
	int stepX = 0;
	int stepY = 0;
	if (!fixedCachedLengthValue(compiled.lengths[0], stepX) ||
	    !fixedCachedLengthValue(compiled.lengths[1], stepY))
		return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::BackgroundSize;
	op.values[0] = std::clamp(stepX, 0, 65535);
	op.values[1] = std::clamp(stepY, 0, 65535);
	return true;
}

bool buildRuntimeBackgroundSizeApplyOp(const CssCompiledValue &compiled, CachedStyleApplyOp &op)
{
	if (compiled.kind != CssCompiledKind::BackgroundSize) return false;
	op = CachedStyleApplyOp{};
	op.kind = CachedStyleApplyOpKind::RuntimeBackgroundSize;
	storeCachedLengthSpec(op, 0, compiled.lengths[0]);
	storeCachedLengthSpec(op, 2, compiled.lengths[1]);
	return true;
}

bool buildCachedStyleApplyOp(const CssCompiledValue *compiled, CachedStyleApplyOp &op)
{
	op = CachedStyleApplyOp{};
	if (!compiled) return false;
	switch (compiled->kind) {
	case CssCompiledKind::Noop:
		op.kind = CachedStyleApplyOpKind::Noop;
		return true;
	case CssCompiledKind::DirectProperty: {
		const int propertyIndex = compiled->values[0];
		if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
		return addCachedStyleApplyProperty(op, static_cast<Property>(propertyIndex), compiled->values[1]);
	}
	case CssCompiledKind::DirectPropertyGroup: {
		int count = compiled->values[0];
		if (count < 0 || count > static_cast<int>(kCachedStyleApplyOpPropertyCapacity)) return false;
		for (int i = 0; i < count; ++i) {
			const int propertyIndex = compiled->values[1 + i * 2];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			if (!addCachedStyleApplyProperty(op,
			                                 static_cast<Property>(propertyIndex),
			                                 compiled->values[2 + i * 2])) return false;
		}
		return op.propertyCount != 0;
	}
	case CssCompiledKind::Keyword:
		return addCachedKeywordDeclaration(op, compiled->declaration, compiled->values[0]);
	case CssCompiledKind::Opacity:
		return addCachedStyleApplyProperty(op, Property::Opacity, compiled->values[0]);
	case CssCompiledKind::Number:
		if (compiled->declaration == CssDeclarationId::ZIndex)
			return addCachedStyleApplyProperty(op, Property::ZIndex, compiled->values[0]);
		if (compiled->declaration == CssDeclarationId::FlexGrow)
			return addCachedStyleApplyProperty(op, Property::Flex, compiled->values[0]);
		if (compiled->declaration == CssDeclarationId::FlexShrink)
			return addCachedStyleApplyProperty(op, Property::FlexShrink, compiled->values[0]);
		if (compiled->declaration == CssDeclarationId::FontWeight)
			return addCachedStyleApplyProperty(op, Property::FontWeight, compiled->values[0]);
		return false;
	case CssCompiledKind::Length:
		if (addCachedLengthDeclaration(op, compiled->declaration, compiled->lengths[0])) return true;
		return buildRuntimeLengthApplyOp(op,
		                                 CachedStyleApplyOpKind::RuntimeLength,
		                                 compiled->declaration,
		                                 compiled->lengths[0]);
	case CssCompiledKind::Size:
		if (addCachedSizeDeclaration(op, compiled->declaration, compiled->lengths[0])) return true;
		return buildRuntimeLengthApplyOp(op,
		                                 CachedStyleApplyOpKind::RuntimeSize,
		                                 compiled->declaration,
		                                 compiled->lengths[0]);
	case CssCompiledKind::PositionOffset:
		if (addCachedPositionDeclaration(op, compiled->declaration, compiled->lengths[0])) return true;
		return buildRuntimeLengthApplyOp(op,
		                                 CachedStyleApplyOpKind::RuntimePositionOffset,
		                                 compiled->declaration,
		                                 compiled->lengths[0]);
	case CssCompiledKind::Box:
		if (addCachedBoxDeclaration(op, compiled->declaration, *compiled)) return true;
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::CompiledBox;
		return true;
	case CssCompiledKind::Color:
		return buildCachedColorApplyOp(*compiled, op);
	case CssCompiledKind::ColorVar:
		return buildCachedColorVarApplyOp(*compiled, op);
	case CssCompiledKind::Rotate:
		return addCachedStyleApplyProperty(op, Property::RotatePresent, compiled->aux) &&
		       addCachedStyleApplyProperty(op, Property::RotateAngle, compiled->values[0]) &&
		       addCachedStyleApplyProperty(op, Property::RotateAxisX, compiled->values[1]) &&
		       addCachedStyleApplyProperty(op, Property::RotateAxisY, compiled->values[2]) &&
		       addCachedStyleApplyProperty(op, Property::RotateAxisZ, compiled->values[3]);
	case CssCompiledKind::Scale:
		return addCachedStyleApplyProperty(op, Property::ScalePresent, compiled->aux) &&
		       addCachedStyleApplyProperty(op, Property::ScaleX, compiled->values[0]) &&
		       addCachedStyleApplyProperty(op, Property::ScaleY, compiled->values[1]) &&
		       addCachedStyleApplyProperty(op, Property::ScaleZ, compiled->values[2]);
	case CssCompiledKind::OriginPair:
		if (compiled->declaration == CssDeclarationId::TransformOrigin)
			return addCachedStyleApplyProperty(op, Property::TransformOriginX, compiled->values[0]) &&
			       addCachedStyleApplyProperty(op, Property::TransformOriginY, compiled->values[1]);
		if (compiled->declaration == CssDeclarationId::PerspectiveOrigin)
			return addCachedStyleApplyProperty(op, Property::PerspectiveOriginX, compiled->values[0]) &&
			       addCachedStyleApplyProperty(op, Property::PerspectiveOriginY, compiled->values[1]);
		return false;
	case CssCompiledKind::Transform:
		if (compiled->declaration == CssDeclarationId::Translate) { op.kind = CachedStyleApplyOpKind::CompiledTransform; return true; }
		return buildCachedTransformApplyOp(*compiled, op) ||
		       buildCachedCompiledTransformApplyOp(*compiled, op);
	case CssCompiledKind::Background:
		return buildCachedBackgroundApplyOp(*compiled, op);
	case CssCompiledKind::BackgroundSize:
		if (buildCachedBackgroundSizeApplyOp(*compiled, op)) return true;
		return buildRuntimeBackgroundSizeApplyOp(*compiled, op);
	case CssCompiledKind::GridTemplate:
		return buildCachedGridTemplateApplyOp(*compiled, op);
	case CssCompiledKind::LineHeight:
		if (compiled->aux == 0 && addCachedStyleApplyProperty(op, Property::LineHeight, 0)) return true;
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::RuntimeLineHeight;
		op.values[0] = compiled->aux;
		storeCachedLengthSpec(op, 1, compiled->lengths[0]);
		return true;
	case CssCompiledKind::FilterBlur: {
		int value = 0;
		if (compiled->aux != 0 && !fixedCachedLengthValue(compiled->lengths[0], value)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeFilterBlur;
			op.values[0] = compiled->aux;
			storeCachedLengthSpec(op, 1, compiled->lengths[0]);
			return true;
		}
		value = std::clamp(value, 0, 64);
		return addCachedStyleApplyProperty(op, Property::FilterPresent, compiled->aux != 0) &&
		       addCachedStyleApplyProperty(op, Property::FilterBlur, value);
	}
	case CssCompiledKind::Flex: {
		if (!addCachedStyleApplyProperty(op, Property::Flex, compiled->values[0])) return false;
		if (!addCachedStyleApplyProperty(op, Property::FlexShrink, compiled->values[1])) return false;
		if (compiled->aux == 0)
			return addCachedStyleApplyProperty(op, Property::FlexBasis, kUnset);
		int basis = 0;
		if (!fixedCachedLengthValue(compiled->lengths[0], basis)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeFlex;
			op.values[0] = compiled->values[0];
			op.values[1] = compiled->values[1];
			storeCachedLengthSpec(op, 2, compiled->lengths[0]);
			return true;
		}
		return addCachedStyleApplyProperty(op, Property::FlexBasis, basis);
	}
	case CssCompiledKind::FlexBasis: {
		if (compiled->aux == 0) return addCachedStyleApplyProperty(op, Property::FlexBasis, kUnset);
		int basis = 0;
		if (fixedCachedLengthValue(compiled->lengths[0], basis))
			return addCachedStyleApplyProperty(op, Property::FlexBasis, basis);
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::RuntimeFlexBasis;
		storeCachedLengthSpec(op, 0, compiled->lengths[0]);
		return true;
	}
	case CssCompiledKind::BorderShorthand: {
		int width = 0;
		if (!fixedCachedBorderWidth(compiled->lengths[0], width) ||
		    (compiled->aux != 0 && compiled->values[1] != 255)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeBorderShorthand;
			op.borderRelief = static_cast<uint8_t>(compiled->values[2]);
			storeCachedLengthSpec(op, 0, compiled->lengths[0]);
			op.values[2] = compiled->aux != 0 ? compiled->values[0] : 0;
			op.values[3] = compiled->aux != 0 ? compiled->values[1] : -1;
			return true;
		}
		if (!addCachedUniformBorderWidth(op, width)) return false;
		if (!addCachedStyleApplyProperty(op, Property::BorderRelief, compiled->values[2])) return false;
		return compiled->aux == 0 ? addCachedStyleApplyProperty(op, Property::BorderColorCurrent, 1) : addCachedStyleApplyProperty(op, Property::BorderColor, compiled->values[0]);
	}
	case CssCompiledKind::BorderSideShorthand: {
		const int side = borderSideForDeclaration(compiled->declaration);
		if (side < 0) return false;
		int width = 0;
		if (!fixedCachedBorderWidth(compiled->lengths[0], width) ||
		    (compiled->aux != 0 && compiled->values[1] != 255)) {
			op = CachedStyleApplyOp{};
			op.kind = CachedStyleApplyOpKind::RuntimeBorderSideShorthand;
			op.borderRelief = static_cast<uint8_t>(compiled->values[2]);
			op.declaration = static_cast<std::uint8_t>(compiled->declaration);
			storeCachedLengthSpec(op, 0, compiled->lengths[0]);
			op.values[2] = compiled->aux != 0 ? compiled->values[0] : 0;
			op.values[3] = compiled->aux != 0 ? compiled->values[1] : -1;
			return true;
		}
		if (!addCachedStyleApplyProperty(op, borderSideWidthProperty(side), width)) return false;
		if (!addCachedStyleApplyProperty(op, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side), compiled->values[2])) return false;
		return compiled->aux == 0 ? addCachedStyleApplyProperty(op, static_cast<Property>(static_cast<int>(Property::BorderTopColorCurrent) + side), 1) : addCachedStyleApplyProperty(op, borderSideColorProperty(side), compiled->values[0]);
	}
	case CssCompiledKind::BorderRadius:
		if (addCachedBorderRadiusDeclaration(op, compiled->declaration, *compiled)) return true;
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::CompiledBorderRadius;
		return true;
	case CssCompiledKind::BoxShadow:
		op = CachedStyleApplyOp{};
		op.kind = CachedStyleApplyOpKind::CompiledBoxShadow;
		return true;
	default:
		return false;
	}
}

bool cachedStyleApplyOpsEqual(const CachedStyleApplyOp &a, const CachedStyleApplyOp &b)
{
	if (a.kind != b.kind) return false;
	if (a.kind == CachedStyleApplyOpKind::Noop)
		return true;
	if (a.kind == CachedStyleApplyOpKind::Transform) {
		if (a.values[0] != b.values[0] || a.values[1] != b.values[1]) return false;
		for (std::uint8_t i = 0; i < kCachedTransformValueCount; ++i)
			if (a.transform[i] != b.transform[i]) return false;
		return true;
	}
	if (a.kind == CachedStyleApplyOpKind::CompiledTransform)
		return true;
	if (a.kind == CachedStyleApplyOpKind::RuntimeLength ||
	    a.kind == CachedStyleApplyOpKind::RuntimeSize ||
	    a.kind == CachedStyleApplyOpKind::RuntimePositionOffset)
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1];
	if (a.kind == CachedStyleApplyOpKind::RuntimeBackgroundSize)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeFlex)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeFlexBasis)
		return a.values[0] == b.values[0] && a.values[1] == b.values[1];
	if (a.kind == CachedStyleApplyOpKind::RuntimeBorderShorthand)
		return a.borderRelief == b.borderRelief && a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeBorderSideShorthand)
		return a.borderRelief == b.borderRelief && a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::RuntimeFilterBlur ||
	    a.kind == CachedStyleApplyOpKind::RuntimeLineHeight)
		return a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2];
	if (a.kind == CachedStyleApplyOpKind::CompiledLength ||
	    a.kind == CachedStyleApplyOpKind::CompiledSize ||
	    a.kind == CachedStyleApplyOpKind::CompiledPositionOffset ||
	    a.kind == CachedStyleApplyOpKind::CompiledBox ||
	    a.kind == CachedStyleApplyOpKind::CompiledBackgroundSize ||
	    a.kind == CachedStyleApplyOpKind::CompiledFlex ||
	    a.kind == CachedStyleApplyOpKind::CompiledFlexBasis ||
	    a.kind == CachedStyleApplyOpKind::CompiledBorderShorthand ||
	    a.kind == CachedStyleApplyOpKind::CompiledBorderSideShorthand ||
	    a.kind == CachedStyleApplyOpKind::CompiledBorderRadius ||
	    a.kind == CachedStyleApplyOpKind::CompiledFilterBlur ||
	    a.kind == CachedStyleApplyOpKind::CompiledBoxShadow ||
	    a.kind == CachedStyleApplyOpKind::CompiledLineHeight)
		return true;
	if (a.kind == CachedStyleApplyOpKind::Color) {
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2];
	}
	if (a.kind == CachedStyleApplyOpKind::ColorVar) {
		return a.declaration == b.declaration &&
		       a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	}
	if (a.kind == CachedStyleApplyOpKind::Background)
		return a.declaration == b.declaration && a.values[0] == b.values[0];
	if (a.kind == CachedStyleApplyOpKind::StaticBackground)
		return a.declaration == b.declaration && a.values[0] == b.values[0] &&
		       a.values[1] == b.values[1] &&
		       a.values[2] == b.values[2] &&
		       a.values[3] == b.values[3];
	if (a.kind == CachedStyleApplyOpKind::BackgroundSize)
		return a.values[0] == b.values[0] && a.values[1] == b.values[1];
	if (a.kind == CachedStyleApplyOpKind::GridTemplate)
		return a.declaration == b.declaration && a.values[0] == b.values[0];
	if (a.propertyCount != b.propertyCount) return false;
	for (std::uint8_t i = 0; i < a.propertyCount; ++i)
		if (a.properties[i] != b.properties[i] || a.values[i] != b.values[i]) return false;
	return true;
}

std::uint16_t cachedStyleApplyOpForCompiledValue(const CssCompiledValue *compiled)
{
	CachedStyleApplyOp op;
	if (!buildCachedStyleApplyOp(compiled, op)) return kNoCachedStyleApplyOp;
	auto &ops = cachedStyleApplyOps();
	for (std::size_t i = 0; i < ops.size(); ++i)
		if (cachedStyleApplyOpsEqual(ops[i], op)) return static_cast<std::uint16_t>(i);
	if (ops.size() >= kNoCachedStyleApplyOp) return kNoCachedStyleApplyOp;
	ops.push_back(op);
	return static_cast<std::uint16_t>(ops.size() - 1);
}

void storeActiveRulePlanForSignature(const RuleCandidateSignature &signature, const ActiveRulePlan &plan)
{
	static constexpr std::size_t kActiveRulePlanCacheLimit = 64;
	if (!activeRulePlanFitsCache(plan)) return;
	auto &cache = activeRulePlanCache();
	if (cache.size() >= kActiveRulePlanCacheLimit) return;
	cache.emplace_back();
	ActiveRulePlanCacheEntry &entry = cache.back();
	activeRulePlanCacheLastHit() = cache.size() - 1;
	copyActiveRulePlanSignature(entry, signature);
	const auto &ruleList = rules();
	std::size_t write = 0;
	for (int bucket = 0; bucket < kActiveRuleBucketCount; ++bucket) {
		const auto &src = plan.buckets[bucket];
		const std::size_t count = src.size();
		entry.bucketCounts[bucket] = static_cast<std::uint8_t>(count);
		entry.bucketOffsets[bucket] = static_cast<std::uint8_t>(write);
		for (std::size_t i = 0; i < count; ++i) {
			if (write >= kActiveRulePlanCacheRuleCapacity) {
				cache.pop_back();
				return;
			}
			const int ri = src.at(i);
			if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) {
				entry.rules[write++] = {};
				continue;
			}
			const CssRule &rule = ruleList[static_cast<std::size_t>(ri)];
			CachedRuleApply &cachedRule = entry.rules[write++];
			if (ri >= static_cast<int>(kNoCachedRuleIndex)) {
				cachedRule = {};
				continue;
			}
			cachedRule.ruleIndex = static_cast<std::uint16_t>(ri);
			cachedRule.compiledValue = rule.compiledValue;
			cachedRule.styleOp =
			    static_cast<std::size_t>(ri) < g_ruleIndex.styleOps.size()
			        ? g_ruleIndex.styleOps[static_cast<std::size_t>(ri)]
			        : cachedStyleApplyOpForCompiledValue(compiledCssValueForHandle(rule.compiledValue));
		}
	}
}

bool selectorRuleIsSignatureLocal(const CssRule &rule)
{
	if (rule.selectorType != CssRule::SelectorType::Selector) return true;
	const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
	if (!plan || !plan->valid || plan->parts.size() != 1) return false;
	const ParsedSimpleSelector &simple = plan->parts.at(0).simple;
	return !simple.wantsFirstChild && !simple.wantsLastChild && !simple.wantsHover;
}

bool signatureContainsClass(const RuleCandidateSignature &signature, CssAtomId classId)
{
	if (classId == kInvalidCssAtom) return false;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		if (signature.classes[i] == classId) return true;
	return false;
}

bool simpleSelectorMatchesSignature(const ParsedSimpleSelector &simple,
                                    const RuleCandidateSignature &signature)
{
	if (!simple.valid || simple.wantsFirstChild || simple.wantsLastChild || simple.wantsHover) return false;
	if (simple.wantsRoot && !signature.root) return false;
	if (simple.hasTag) {
		if (simple.rootTag) {
			if (!signature.root) return false;
		} else if (signature.tagId != simple.tagId) {
			return false;
		}
	}
	if (simple.idAtom != kInvalidCssAtom && signature.idAtom != simple.idAtom) return false;
	for (std::size_t i = 0, n = simple.classIds.size(); i < n; ++i)
		if (!signatureContainsClass(signature, simple.classIds.at(i))) return false;
	return simple.hasMatcher;
}

bool selectorRuleMatchesSignature(const CssRule &rule, const RuleCandidateSignature &signature)
{
	if (rule.selectorType != CssRule::SelectorType::Selector) return true;
	const SelectorPlan *plan = selectorPlanForHandle(rule.selectorPlan);
	if (!plan || !plan->valid || plan->parts.size() != 1) return false;
	return simpleSelectorMatchesSignature(plan->parts.at(0).simple, signature);
}

bool candidateRulesAreSignatureLocal(const RuleCandidateList &candidates,
                                     const std::vector<CssRule> &ruleList)
{
	for (std::size_t i = 0, n = candidates.size(); i < n; ++i) {
		const int ri = candidates[i];
		if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) return false;
		if (!selectorRuleIsSignatureLocal(ruleList[static_cast<std::size_t>(ri)])) return false;
	}
	return true;
}

bool cascadeRuleBefore(int a, int b)
{
	const std::vector<int> &spec = g_ruleIndex.specificity;
	if (a < 0 || b < 0 ||
	    static_cast<std::size_t>(a) >= spec.size() ||
	    static_cast<std::size_t>(b) >= spec.size())
		return a < b;
	if (rules()[a].userAgent != rules()[b].userAgent) return rules()[a].userAgent;
	return spec[static_cast<std::size_t>(a)] != spec[static_cast<std::size_t>(b)]
	    ? spec[static_cast<std::size_t>(a)] < spec[static_cast<std::size_t>(b)]
	    : a < b;
}

void sortActiveRuleBucketByCascade(ActiveRulePlan::Bucket &bucket)
{
	for (std::size_t i = 1, n = bucket.size(); i < n; ++i) {
		const int rule = bucket.at(i);
		std::size_t j = i;
		while (j > 0 && cascadeRuleBefore(rule, bucket.at(j - 1))) {
			bucket.set(j, bucket.at(j - 1));
			--j;
		}
		bucket.set(j, rule);
	}
}

void sortActiveRulePlanByCascade(ActiveRulePlan &plan)
{
	for (auto &bucket : plan.buckets)
		sortActiveRuleBucketByCascade(bucket);
}

void compactActiveRuleBucket(ActiveRulePlan::Bucket &bucket, const std::uint8_t *skip)
{
	std::size_t write = 0;
	const std::size_t bucketSize = bucket.size();
	for (std::size_t read = 0; read < bucketSize; ++read) {
		if (skip[read]) continue;
		if (write != read) bucket.set(write, bucket.at(read));
		++write;
	}
	if (bucket.spilled)
		bucket.spillCount = write;
	else
		bucket.count = static_cast<std::uint8_t>(write);
}

void addPropertyWrite(PropertyWriteMask &mask, Property property)
{
	if (property == Property::BackgroundColor) { mask.add(Property::BackgroundColor); mask.add(Property::BackgroundAlpha); return; }
	if (property == Property::Color) { mask.add(Property::Color); mask.add(Property::ColorAlpha); return; }
	if (property == Property::BorderColor || property == Property::BorderColorCurrent) {
		for (Property color : {Property::BorderColor, Property::BorderColorCurrent,
		     Property::BorderTopColor, Property::BorderRightColor, Property::BorderBottomColor, Property::BorderLeftColor,
		     Property::BorderTopColorCurrent, Property::BorderRightColorCurrent, Property::BorderBottomColorCurrent, Property::BorderLeftColorCurrent, Property::BorderAlpha, Property::BorderTopAlpha, Property::BorderRightAlpha, Property::BorderBottomAlpha, Property::BorderLeftAlpha}) mask.add(color);
		return;
	}
	for (int side = 0; side < 4; ++side) {
		const Property current = static_cast<Property>(static_cast<int>(Property::BorderTopColorCurrent) + side);
		if (property == borderSideColorProperty(side) || property == current) { mask.add(current); mask.add(borderSideColorProperty(side)); mask.add(static_cast<Property>(static_cast<int>(Property::BorderTopAlpha) + side)); return; }
	}
	switch (property) {
	case Property::BorderRelief:
		for (int side = 0; side < 4; ++side) mask.add(static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side));
		return;
	case Property::BorderWidth:
		mask.add(property);
		for (int side = 0; side < 4; ++side) mask.add(borderSideWidthProperty(side));
		return;

	case Property::PaddingLeft:
	case Property::PaddingLeftExpression: mask.add(Property::PaddingLeft); mask.add(Property::PaddingLeftExpression); return;
	case Property::PaddingBottom:
	case Property::PaddingBottomExpression: mask.add(Property::PaddingBottom); mask.add(Property::PaddingBottomExpression); return;
	case Property::PaddingRight:
	case Property::PaddingRightExpression: mask.add(Property::PaddingRight); mask.add(Property::PaddingRightExpression); return;
	case Property::PaddingTop:
	case Property::PaddingTopExpression: mask.add(Property::PaddingTop); mask.add(Property::PaddingTopExpression); return;
	case Property::Gap:
		mask.add(property); mask.add(Property::RowGap); mask.add(Property::ColumnGap);
		mask.add(Property::RowGapPercent); mask.add(Property::ColumnGapPercent); return;
	case Property::MarginTop:
	case Property::MarginTopExpression: mask.add(Property::MarginTop); mask.add(Property::MarginTopExpression); mask.add(Property::MarginTopAuto); return;
	case Property::MarginRight:
	case Property::MarginRightExpression: mask.add(Property::MarginRight); mask.add(Property::MarginRightExpression); mask.add(Property::MarginRightAuto); return;
	case Property::MarginBottom:
	case Property::MarginBottomExpression: mask.add(Property::MarginBottom); mask.add(Property::MarginBottomExpression); mask.add(Property::MarginBottomAuto); return;
	case Property::MarginLeft:
	case Property::MarginLeftExpression: mask.add(Property::MarginLeft); mask.add(Property::MarginLeftExpression); mask.add(Property::MarginLeftAuto); return;
	case Property::LineHeight:
	case Property::LineHeightExpression:
	case Property::LineHeightMultiplier:
		mask.add(Property::LineHeight); mask.add(Property::LineHeightExpression); mask.add(Property::LineHeightMultiplier); return;
	case Property::FlexBasis:
	case Property::FlexBasisExpression:
		mask.add(Property::FlexBasis); mask.add(Property::FlexBasisExpression); return;
	case Property::WidthExpression:
	case Property::Width:
	case Property::WidthPercent:
		mask.add(Property::WidthExpression);
		mask.add(Property::Width);
		mask.add(Property::WidthPercent);
		return;
	case Property::HeightExpression:
	case Property::Height:
	case Property::HeightPercent:
		mask.add(Property::HeightExpression);
		mask.add(Property::Height);
		mask.add(Property::HeightPercent);
		return;
	case Property::Top:
	case Property::TopPercent:
		mask.add(Property::Top);
		mask.add(Property::TopPercent);
		return;
	case Property::Right:
	case Property::RightPercent:
		mask.add(Property::Right);
		mask.add(Property::RightPercent);
		return;
	case Property::Bottom:
	case Property::BottomPercent:
		mask.add(Property::Bottom);
		mask.add(Property::BottomPercent);
		return;
	case Property::Left:
	case Property::LeftPercent:
		mask.add(Property::Left);
		mask.add(Property::LeftPercent);
		return;
	case Property::BorderRadiusTopLeft:
	case Property::BorderRadiusTopLeftPercent:
		mask.add(Property::BorderRadiusTopLeft);
		mask.add(Property::BorderRadiusTopLeftPercent);
		return;
	case Property::BorderRadiusTopRight:
	case Property::BorderRadiusTopRightPercent:
		mask.add(Property::BorderRadiusTopRight);
		mask.add(Property::BorderRadiusTopRightPercent);
		return;
	case Property::BorderRadiusBottomRight:
	case Property::BorderRadiusBottomRightPercent:
		mask.add(Property::BorderRadiusBottomRight);
		mask.add(Property::BorderRadiusBottomRightPercent);
		return;
	case Property::BorderRadiusBottomLeft:
	case Property::BorderRadiusBottomLeftPercent:
		mask.add(Property::BorderRadiusBottomLeft);
		mask.add(Property::BorderRadiusBottomLeftPercent);
		return;
	case Property::Overflow:
		mask.add(Property::Overflow);
		mask.add(Property::OverflowX);
		mask.add(Property::OverflowY);
		return;
	case Property::OverflowX:
		mask.add(Property::Overflow);
		mask.add(Property::OverflowX);
		return;
	case Property::OverflowY:
		mask.add(Property::Overflow);
		mask.add(Property::OverflowY);
		return;
	default:
		mask.add(property);
		return;
	}
}

void addTransformPropertyWrites(PropertyWriteMask &mask)
{
	mask.add(Property::TransformPresent);
	mask.add(Property::TransformTranslateOuterAxes);
	mask.add(Property::TransformRotate);
	mask.add(Property::TransformRotateX);
	mask.add(Property::TransformRotateY);
	mask.add(Property::TransformTranslateX);
	mask.add(Property::TransformTranslateY);
	mask.add(Property::TransformTranslateZ);
	mask.add(Property::TransformTranslateXPercent);
	mask.add(Property::TransformTranslateYPercent);
	mask.add(Property::TransformScaleX);
	mask.add(Property::TransformScaleY);
	mask.add(Property::TransformScaleZ);
}

void addBoxShadowPropertyWrites(PropertyWriteMask &mask)
{
	mask.add(Property::BoxShadowInset);
	mask.add(Property::BoxShadowOffsetX);
	mask.add(Property::BoxShadowOffsetY);
	mask.add(Property::BoxShadowBlur);
	mask.add(Property::BoxShadowSpread);
	mask.add(Property::BoxShadowColor);
	mask.add(Property::BoxShadowAlpha);
}

bool addColorDeclarationWrites(CssDeclarationId declaration, PropertyWriteMask &mask)
{
	switch (declaration) {
	case CssDeclarationId::Color:
		addPropertyWrite(mask, Property::Color);
		return true;
	case CssDeclarationId::ActiveBackgroundColor:
		mask.add(Property::ActiveBackgroundColor);
		mask.add(Property::HasActiveBackground);
		return true;
	case CssDeclarationId::Background:
		mask.add(Property::BackgroundImage);
		mask.add(Property::BackgroundClip);
		mask.add(Property::BackgroundSizeList);
		mask.add(Property::BackgroundPositionList);
		mask.add(Property::BackgroundRepeatList);
		mask.add(Property::BackgroundAttachmentList);
		mask.add(Property::BackgroundOriginList);
		[[fallthrough]];
	case CssDeclarationId::BackgroundColor:
		mask.add(Property::BackgroundAlpha);
		mask.add(Property::BackgroundColor);
		mask.add(Property::HasBackground);
		return true;
	case CssDeclarationId::BorderColor:
		addPropertyWrite(mask, Property::BorderColor);
		return true;
	case CssDeclarationId::BorderTopColor:
	case CssDeclarationId::BorderRightColor:
	case CssDeclarationId::BorderBottomColor:
	case CssDeclarationId::BorderLeftColor: {
		const int side = declaration == CssDeclarationId::BorderTopColor ? 0 :
		    declaration == CssDeclarationId::BorderRightColor ? 1 :
		    declaration == CssDeclarationId::BorderBottomColor ? 2 : 3;
		addPropertyWrite(mask, borderSideColorProperty(side));
		return true;
	}
	default:
		return false;
	}
}

bool addCompiledValueWrites(const CssCompiledValue &compiled, PropertyWriteMask &mask)
{
	switch (compiled.kind) {
	case CssCompiledKind::Noop:
		return true;
	case CssCompiledKind::DirectProperty: {
		const int propertyIndex = compiled.values[0];
		if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
		addPropertyWrite(mask, static_cast<Property>(propertyIndex));
		return true;
	}
	case CssCompiledKind::DirectPropertyGroup: {
		int count = compiled.values[0];
		if (count < 0) count = 0;
		if (count > 4) count = 4;
		for (int i = 0; i < count; ++i) {
			const int propertyIndex = compiled.values[1 + i * 2];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			addPropertyWrite(mask, static_cast<Property>(propertyIndex));
		}
		return true;
	}
	case CssCompiledKind::Keyword:
		switch (compiled.declaration) {
		case CssDeclarationId::Display: addPropertyWrite(mask, Property::Display); return true;
		case CssDeclarationId::ObjectFit: addPropertyWrite(mask, Property::ImageFit); return true;
		case CssDeclarationId::FlexDirection: addPropertyWrite(mask, Property::FlexDirection); return true;
		case CssDeclarationId::FlexWrap: addPropertyWrite(mask, Property::FlexWrap); return true;
		case CssDeclarationId::JustifyContent: addPropertyWrite(mask, Property::JustifyContent); return true;
		case CssDeclarationId::AlignItems: addPropertyWrite(mask, Property::AlignItems); return true;
		case CssDeclarationId::JustifyItems: addPropertyWrite(mask, Property::JustifyItems); return true;
		case CssDeclarationId::AlignContent: addPropertyWrite(mask, Property::AlignContent); return true;
		case CssDeclarationId::JustifySelf: addPropertyWrite(mask, Property::JustifySelf); return true;
		case CssDeclarationId::GridRowStart: addPropertyWrite(mask, Property::GridRowStart); return true;
		case CssDeclarationId::GridColumnStart: addPropertyWrite(mask, Property::GridColumnStart); return true;
		case CssDeclarationId::GridRowEnd: addPropertyWrite(mask, Property::GridRowEnd); return true;
		case CssDeclarationId::GridColumnEnd: addPropertyWrite(mask, Property::GridColumnEnd); return true;
		case CssDeclarationId::AlignSelf: addPropertyWrite(mask, Property::AlignSelf); return true;
		case CssDeclarationId::Position: addPropertyWrite(mask, Property::Position); return true;
		case CssDeclarationId::TextAlign: addPropertyWrite(mask, Property::TextAlign); return true;
		case CssDeclarationId::TextDecoration: addPropertyWrite(mask, Property::TextDecoration); return true;
		case CssDeclarationId::TextTransform: addPropertyWrite(mask, Property::TextTransform); return true;
		case CssDeclarationId::WhiteSpace: addPropertyWrite(mask, Property::WhiteSpace); return true;
		case CssDeclarationId::TextOverflow: addPropertyWrite(mask, Property::TextOverflow); return true;
		case CssDeclarationId::TransformStyle: addPropertyWrite(mask, Property::TransformStyle); return true;
		case CssDeclarationId::Visibility: addPropertyWrite(mask, Property::Visibility); return true;
		case CssDeclarationId::BackfaceVisibility: addPropertyWrite(mask, Property::Backface); return true;
		case CssDeclarationId::PointerEvents: addPropertyWrite(mask, Property::PointerEvents); return true;
		case CssDeclarationId::Overflow: addPropertyWrite(mask, Property::Overflow); return true;
		case CssDeclarationId::OverflowX: addPropertyWrite(mask, Property::OverflowX); return true;
		case CssDeclarationId::OverflowY: addPropertyWrite(mask, Property::OverflowY); return true;
		case CssDeclarationId::FontFamily: addPropertyWrite(mask, Property::FontId); return true;
		case CssDeclarationId::FontWeight: addPropertyWrite(mask, Property::FontWeight); return true;
		default: return false;
		}
	case CssCompiledKind::Opacity:
		addPropertyWrite(mask, Property::Opacity);
		return true;
	case CssCompiledKind::Number:
		if (compiled.declaration == CssDeclarationId::ZIndex) {
			addPropertyWrite(mask, Property::ZIndex);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexGrow) {
			addPropertyWrite(mask, Property::Flex);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FlexShrink) {
			addPropertyWrite(mask, Property::FlexShrink);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::FontWeight) {
			addPropertyWrite(mask, Property::FontWeight);
			return true;
		}
		return false;
	case CssCompiledKind::Flex:
		addPropertyWrite(mask, Property::Flex);
		addPropertyWrite(mask, Property::FlexShrink);
		addPropertyWrite(mask, Property::FlexBasis);
		return true;
	case CssCompiledKind::FlexBasis:
		addPropertyWrite(mask, Property::FlexBasis);
		return true;
	case CssCompiledKind::Length:
		switch (compiled.declaration) {
		case CssDeclarationId::Gap: addPropertyWrite(mask, Property::Gap); return true;
		case CssDeclarationId::MinWidth: addPropertyWrite(mask, Property::MinWidth); return true;
		case CssDeclarationId::MinHeight: addPropertyWrite(mask, Property::MinHeight); return true;
		case CssDeclarationId::MaxWidth: addPropertyWrite(mask, Property::MaxWidth); return true;
		case CssDeclarationId::MaxHeight: addPropertyWrite(mask, Property::MaxHeight); return true;
		case CssDeclarationId::PaddingTop: addPropertyWrite(mask, Property::PaddingTop); return true;
		case CssDeclarationId::PaddingRight: addPropertyWrite(mask, Property::PaddingRight); return true;
		case CssDeclarationId::PaddingBottom: addPropertyWrite(mask, Property::PaddingBottom); return true;
		case CssDeclarationId::PaddingLeft: addPropertyWrite(mask, Property::PaddingLeft); return true;
		case CssDeclarationId::MarginTop: addPropertyWrite(mask, Property::MarginTop); return true;
		case CssDeclarationId::MarginRight: addPropertyWrite(mask, Property::MarginRight); return true;
		case CssDeclarationId::MarginBottom: addPropertyWrite(mask, Property::MarginBottom); return true;
		case CssDeclarationId::MarginLeft: addPropertyWrite(mask, Property::MarginLeft); return true;
		case CssDeclarationId::BorderWidth:
			addPropertyWrite(mask, Property::BorderWidth);
			for (int side = 0; side < 4; ++side) addPropertyWrite(mask, borderSideWidthProperty(side));
			return true;
		case CssDeclarationId::BorderTopWidth: addPropertyWrite(mask, Property::BorderTopWidth); return true;
		case CssDeclarationId::BorderRightWidth: addPropertyWrite(mask, Property::BorderRightWidth); return true;
		case CssDeclarationId::BorderBottomWidth: addPropertyWrite(mask, Property::BorderBottomWidth); return true;
		case CssDeclarationId::BorderLeftWidth: addPropertyWrite(mask, Property::BorderLeftWidth); return true;
		case CssDeclarationId::FontSize: addPropertyWrite(mask, Property::FontSize); return true;
		case CssDeclarationId::Perspective: addPropertyWrite(mask, Property::Perspective); return true;
		case CssDeclarationId::MaskImage: addPropertyWrite(mask, Property::MaskRightFadeWidth); return true;
		default: return false;
		}
	case CssCompiledKind::Size:
		if (compiled.declaration == CssDeclarationId::Width) {
			addPropertyWrite(mask, Property::Width);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Height) {
			addPropertyWrite(mask, Property::Height);
			return true;
		}
		return false;
	case CssCompiledKind::PositionOffset:
		if (compiled.declaration == CssDeclarationId::Top) {
			addPropertyWrite(mask, Property::Top);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Right) {
			addPropertyWrite(mask, Property::Right);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Bottom) {
			addPropertyWrite(mask, Property::Bottom);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Left) {
			addPropertyWrite(mask, Property::Left);
			return true;
		}
		return false;
	case CssCompiledKind::Box:
		if (compiled.declaration == CssDeclarationId::BorderWidth) {
			addPropertyWrite(mask, Property::BorderWidth);
			for (int side = 0; side < 4; ++side) addPropertyWrite(mask, borderSideWidthProperty(side));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Padding) {
			addPropertyWrite(mask, Property::PaddingTop);
			addPropertyWrite(mask, Property::PaddingRight);
			addPropertyWrite(mask, Property::PaddingBottom);
			addPropertyWrite(mask, Property::PaddingLeft);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Margin) {
			addPropertyWrite(mask, Property::MarginTop);
			addPropertyWrite(mask, Property::MarginRight);
			addPropertyWrite(mask, Property::MarginBottom);
			addPropertyWrite(mask, Property::MarginLeft);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Inset) {
			mask.add(Property::Top);
			mask.add(Property::Right);
			mask.add(Property::Bottom);
			mask.add(Property::Left);
			return true;
		}
		return false;
	case CssCompiledKind::Color:
		return addColorDeclarationWrites(compiled.declaration, mask);
	case CssCompiledKind::ColorVar:
		return false;
	case CssCompiledKind::Rotate:
		for (Property p : {Property::RotatePresent, Property::RotateAngle, Property::RotateAxisX, Property::RotateAxisY, Property::RotateAxisZ}) addPropertyWrite(mask, p);
		return true;
	case CssCompiledKind::Scale:
		for (Property p : {Property::ScalePresent, Property::ScaleX, Property::ScaleY, Property::ScaleZ}) addPropertyWrite(mask, p);
		return true;
	case CssCompiledKind::OriginPair:
		if (compiled.declaration == CssDeclarationId::TransformOrigin) {
			addPropertyWrite(mask, Property::TransformOriginX);
			addPropertyWrite(mask, Property::TransformOriginY);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::PerspectiveOrigin) {
			addPropertyWrite(mask, Property::PerspectiveOriginX);
			addPropertyWrite(mask, Property::PerspectiveOriginY);
			return true;
		}
		return false;
	case CssCompiledKind::Transform:
		if (compiled.declaration == CssDeclarationId::Translate) {
			for (Property p : {Property::TranslatePresent, Property::TranslateX, Property::TranslateY, Property::TranslateZ, Property::TranslateXPercent, Property::TranslateYPercent}) addPropertyWrite(mask, p);
			return true;
		}
		addTransformPropertyWrites(mask);
		return true;
	case CssCompiledKind::Background:
		return false;
	case CssCompiledKind::BackgroundSize:
		mask.addIndex(kVirtualBackgroundSizeWrite);
		return true;
	case CssCompiledKind::BorderShorthand:
		addPropertyWrite(mask, Property::BorderWidth);
		for (int side = 0; side < 4; ++side) addPropertyWrite(mask, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side));
		for (int side = 0; side < 4; ++side) addPropertyWrite(mask, borderSideWidthProperty(side));
		addPropertyWrite(mask, Property::BorderColor);
		return true;
	case CssCompiledKind::BorderSideShorthand: {
		const int side = borderSideForDeclaration(compiled.declaration);
		if (side < 0) return false;
		addPropertyWrite(mask, static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side));
		addPropertyWrite(mask, borderSideWidthProperty(side));
		addPropertyWrite(mask, borderSideColorProperty(side));
		return true;
	}
	case CssCompiledKind::BorderRadius:
		if (compiled.declaration == CssDeclarationId::BorderRadius) {
			for (int corner = 0; corner < 4; ++corner)
				addPropertyWrite(mask, borderRadiusLengthProperty(corner));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderTopLeftRadius) {
			addPropertyWrite(mask, Property::BorderRadiusTopLeft);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderTopRightRadius) {
			addPropertyWrite(mask, Property::BorderRadiusTopRight);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderBottomRightRadius) {
			addPropertyWrite(mask, Property::BorderRadiusBottomRight);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::BorderBottomLeftRadius) {
			addPropertyWrite(mask, Property::BorderRadiusBottomLeft);
			return true;
		}
		return false;
	case CssCompiledKind::FilterBlur:
		addPropertyWrite(mask, Property::FilterPresent);
		addPropertyWrite(mask, Property::FilterBlur);
		return true;
	case CssCompiledKind::BoxShadow:
		addBoxShadowPropertyWrites(mask);
		return true;
	case CssCompiledKind::GridTemplate:
		if (compiled.declaration == CssDeclarationId::GridTemplateColumns) {
			mask.addIndex(kVirtualGridTemplateColumnsWrite);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::GridTemplateRows) {
			mask.addIndex(kVirtualGridTemplateRowsWrite);
			return true;
		}
		return false;
	case CssCompiledKind::LineHeight:
		addPropertyWrite(mask, Property::LineHeight);
		return true;
	case CssCompiledKind::None:
		return false;
	}
	return false;
}

bool ruleWriteMask(const CssRule &rule, PropertyWriteMask &mask)
{
	if (rule.declaration == CssDeclarationId::Custom) return false;
	const CssCompiledValue *compiled = compiledCssValueForHandle(rule.compiledValue);
	if (!compiled) return false;
	return addCompiledValueWrites(*compiled, mask);
}

void collapseShadowedActiveRuleBucket(ActiveRulePlan::Bucket &bucket)
{
	const auto &ruleList = rules();
	const std::size_t bucketSize = bucket.size();
	if (bucketSize <= 1) return;
	std::uint8_t inlineSkip[64]{};
	std::vector<std::uint8_t> spillSkip;
	std::uint8_t *skip = inlineSkip;
	if (bucketSize > sizeof(inlineSkip) / sizeof(inlineSkip[0])) {
		spillSkip.assign(bucketSize, 0);
		skip = spillSkip.data();
	}

	bool anySkip = false;
	PropertyWriteMask laterWrites;
	for (std::size_t reverseIndex = bucketSize; reverseIndex > 0; --reverseIndex) {
		const std::size_t i = reverseIndex - 1;
		const int ri = bucket.at(i);
		if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) continue;
		if (static_cast<std::size_t>(ri) >= g_ruleIndex.writeMasks.size()) continue;
		const PropertyWriteMask &writes = g_ruleIndex.writeMasks[static_cast<std::size_t>(ri)];
		if (writes.empty()) continue;
		if (laterWrites.containsAll(writes)) {
			skip[i] = 1;
			anySkip = true;
		} else {
			laterWrites.addAll(writes);
		}
	}
	if (anySkip) compactActiveRuleBucket(bucket, skip);
}

void collapseShadowedActiveRules(ActiveRulePlan &plan)
{
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveMainRule]);
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveBeforeRule]);
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveAfterRule]);
	collapseShadowedActiveRuleBucket(plan.buckets[kActiveFirstLineRule]);
}

int activeRuleBucketFor(const CssRule &rule)
{
	if (rule.pseudoElement == CssRule::PseudoElement::Unsupported) return -1;
	const bool custom = isCustomRuleProperty(rule);
	switch (rule.pseudoElement) {
	case CssRule::PseudoElement::None:
		return custom ? kActiveMainCustom : kActiveMainRule;
	case CssRule::PseudoElement::Before:
		return custom ? kActiveBeforeCustom : kActiveBeforeRule;
	case CssRule::PseudoElement::After:
		return custom ? kActiveAfterCustom : kActiveAfterRule;
	case CssRule::PseudoElement::FirstLine:
		return custom ? kActiveFirstLineCustom : kActiveFirstLineRule;
	case CssRule::PseudoElement::Unsupported:
		return -1;
	}
	return -1;
}

bool captureRuleCandidateSignature(int selectorNode, RuleCandidateSignature &signature)
{
	const auto &state = treeState();
	if (selectorNode < 0 || selectorNode >= state.nodeCount) return false;
	const NodeClassList &classes = state.classLists[selectorNode];
	if (classes.size() > kRuleCandidateCacheClassCapacity) return false;
	signature = RuleCandidateSignature{};
	signature.classCount = static_cast<std::uint8_t>(classes.size());
	for (std::size_t i = 0; i < classes.size(); ++i)
		signature.classes[i] = classes.at(i);
	sortRuleCandidateSignatureClasses(signature);
	signature.idAtom = nodeIdAttributeAtom(selectorNode);
	signature.tagId = state.nodes[selectorNode].tag_id;
	signature.root = isRootNode(selectorNode);
	return true;
}

bool candidateSignatureMatches(const RuleCandidateCacheEntry &entry, const RuleCandidateSignature &signature)
{
	if (entry.idAtom != signature.idAtom ||
	    entry.tagId != signature.tagId ||
	    entry.classCount != signature.classCount ||
	    entry.root != signature.root)
		return false;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		if (entry.classes[i] != signature.classes[i]) return false;
	return true;
}

void copyCandidateSignature(RuleCandidateCacheEntry &entry, const RuleCandidateSignature &signature)
{
	entry.idAtom = signature.idAtom;
	entry.tagId = signature.tagId;
	entry.classCount = signature.classCount;
	entry.root = signature.root;
	for (std::size_t i = 0; i < signature.classCount; ++i)
		entry.classes[i] = signature.classes[i];
}

void addRuleCandidateIndex(RuleCandidateList &out,
                           int ruleIndex,
                           std::uint16_t serial,
                           const std::vector<CssRule> &ruleList)
{
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= g_ruleIndex.candidateSeen.size()) return;
	std::uint16_t &seen = g_ruleIndex.candidateSeen[static_cast<std::size_t>(ruleIndex)];
	if (seen == serial) return;
	seen = serial;
	if (static_cast<std::size_t>(ruleIndex) >= ruleList.size()) return;
	out.push_back(ruleIndex);
}

void addRuleCandidateBucket(RuleCandidateList &out,
                            DenseRuleBucketSpan bucket,
                            std::uint16_t serial,
                            const std::vector<CssRule> &ruleList)
{
	if (bucket.empty()) return;
	for (std::size_t i = 0; i < bucket.count; ++i)
		addRuleCandidateIndex(out, bucket.data[i], serial, ruleList);
}

void addRuleCandidateBucket(RuleCandidateList &out,
                            const std::vector<int> &bucket,
                            std::uint16_t serial,
                            const std::vector<CssRule> &ruleList)
{
	for (const int ri : bucket)
		addRuleCandidateIndex(out, ri, serial, ruleList);
}

void sortRuleCandidateListByCascade(RuleCandidateList &candidates)
{
	for (std::size_t i = 1, n = candidates.size(); i < n; ++i) {
		const int rule = candidates[i];
		std::size_t j = i;
		while (j > 0 && cascadeRuleBefore(rule, candidates[j - 1])) {
			candidates[j] = candidates[j - 1];
			--j;
		}
		candidates[j] = rule;
	}
}

void buildRuleCandidatesForSignature(const RuleCandidateSignature &signature, RuleCandidateList &out)
{
	out.clear();
	const auto &ruleList = rules();
	const std::uint16_t serial = nextCandidateCollectSerial();
	for (std::size_t classIndex = 0; classIndex < signature.classCount; ++classIndex) {
		const CssAtomId cls = signature.classes[classIndex];
		addRuleCandidateBucket(out, g_ruleIndex.byClass.get(cls), serial, ruleList);
		addRuleCandidateBucket(out, g_ruleIndex.selByClass.get(cls), serial, ruleList);
	}
	if (signature.idAtom != kInvalidCssAtom)
		addRuleCandidateBucket(out, g_ruleIndex.selById.get(signature.idAtom), serial, ruleList);
	addRuleCandidateBucket(out, g_ruleIndex.byTag.get(signature.tagId), serial, ruleList);
	addRuleCandidateBucket(out, g_ruleIndex.selByTag.get(signature.tagId), serial, ruleList);
	if (signature.root)
		addRuleCandidateBucket(out, g_ruleIndex.selRoot, serial, ruleList);
	addRuleCandidateBucket(out, g_ruleIndex.selAlways, serial, ruleList);
	sortRuleCandidateListByCascade(out);
}

const RuleCandidateCacheEntry *cachedRuleCandidateEntryForSignature(const RuleCandidateSignature &signature)
{
	static constexpr std::size_t kRuleCandidateCacheLimit = 64;
	std::size_t &lastHit = ruleCandidateCacheLastHit();
	if (lastHit < g_ruleIndex.candidateCache.size() &&
	    candidateSignatureMatches(g_ruleIndex.candidateCache[lastHit], signature))
		return &g_ruleIndex.candidateCache[lastHit];
	for (std::size_t i = 0; i < g_ruleIndex.candidateCache.size(); ++i) {
		if (!candidateSignatureMatches(g_ruleIndex.candidateCache[i], signature)) continue;
		lastHit = i;
		return &g_ruleIndex.candidateCache[i];
	}
	if (g_ruleIndex.candidateCache.size() >= kRuleCandidateCacheLimit) return nullptr;
	g_ruleIndex.candidateCache.emplace_back();
	RuleCandidateCacheEntry &entry = g_ruleIndex.candidateCache.back();
	copyCandidateSignature(entry, signature);
	buildRuleCandidatesForSignature(signature, entry.rules);
	entry.signatureLocal = candidateRulesAreSignatureLocal(entry.rules, rules());
	lastHit = g_ruleIndex.candidateCache.size() - 1;
	return &entry;
}

void addCachedActiveCandidateRule(ActiveRulePlan &plan,
                                  int selectorNode,
                                  int ruleIndex,
                                  const std::vector<CssRule> &ruleList,
                                  const std::vector<std::uint8_t> *mediaMatches,
                                  const RuleCandidateSignature *signature = nullptr)
{
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= ruleList.size()) return;
	if (mediaMatches &&
	    (static_cast<std::size_t>(ruleIndex) >= mediaMatches->size() ||
	     (*mediaMatches)[static_cast<std::size_t>(ruleIndex)] == 0))
		return;
	const CssRule &rule = ruleList[static_cast<std::size_t>(ruleIndex)];
	if (rule.selectorType == CssRule::SelectorType::Selector) {
		if (signature && selectorRuleIsSignatureLocal(rule)) {
			if (!selectorRuleMatchesSignature(rule, *signature)) return;
		} else if (!selectorMatchesNode(rule, selectorNode)) {
			return;
		}
	}
	const int bucket = activeRuleBucketFor(rule);
	if (bucket >= 0) plan.push(bucket, ruleIndex);
	if (rule.propertyKind == CssRuleProperty::Animation &&
	    rule.pseudoElement == CssRule::PseudoElement::None)
		plan.push(kActiveAnimation, ruleIndex);
}

void addActiveCandidateRule(ActiveRulePlan &plan,
                            int selectorNode,
                            int ruleIndex,
                            std::uint16_t serial,
                            const std::vector<CssRule> &ruleList,
                            const std::vector<std::uint8_t> *mediaMatches)
{
	if (ruleIndex < 0 || static_cast<std::size_t>(ruleIndex) >= g_ruleIndex.candidateSeen.size()) return;
	std::uint16_t &seen = g_ruleIndex.candidateSeen[static_cast<std::size_t>(ruleIndex)];
	if (seen == serial) return;
	seen = serial;
	if (static_cast<std::size_t>(ruleIndex) >= ruleList.size()) return;
	if (mediaMatches &&
	    (static_cast<std::size_t>(ruleIndex) >= mediaMatches->size() ||
	     (*mediaMatches)[static_cast<std::size_t>(ruleIndex)] == 0))
		return;
	const CssRule &rule = ruleList[static_cast<std::size_t>(ruleIndex)];
	if (rule.selectorType == CssRule::SelectorType::Selector && !selectorMatchesNode(rule, selectorNode)) return;
	const int bucket = activeRuleBucketFor(rule);
	if (bucket >= 0) plan.push(bucket, ruleIndex);
	if (rule.propertyKind == CssRuleProperty::Animation &&
	    rule.pseudoElement == CssRule::PseudoElement::None)
		plan.push(kActiveAnimation, ruleIndex);
}

void addActiveCandidateRuleBucket(ActiveRulePlan &plan,
                                  int selectorNode,
                                  DenseRuleBucketSpan bucket,
                                  std::uint16_t serial,
                                  const std::vector<CssRule> &ruleList,
                                  const std::vector<std::uint8_t> *mediaMatches)
{
	if (bucket.empty()) return;
	for (std::size_t i = 0; i < bucket.count; ++i)
		addActiveCandidateRule(plan, selectorNode, bucket.data[i], serial, ruleList, mediaMatches);
}

void addActiveCandidateRuleBucket(ActiveRulePlan &plan,
                                  int selectorNode,
                                  const std::vector<int> &bucket,
                                  std::uint16_t serial,
                                  const std::vector<CssRule> &ruleList,
                                  const std::vector<std::uint8_t> *mediaMatches)
{
	if (bucket.empty()) return;
	for (const int ri : bucket)
		addActiveCandidateRule(plan, selectorNode, ri, serial, ruleList, mediaMatches);
}

void buildActiveRulePlanForNode(int selectorNode, ActiveRulePlan &plan)
{
	plan.clear();
	rebuildRuleIndexIfNeeded();
	if (g_ruleIndex.hasMediaConditions) rebuildRuleMediaCacheIfNeeded();
	const auto &state = treeState();
	if (selectorNode < 0 || selectorNode >= state.nodeCount) return;
	if (isAnonymousTextNode(state.nodes[selectorNode])) return;
	const auto &ruleList = rules();
	const std::vector<std::uint8_t> *mediaMatches =
	    g_ruleIndex.hasMediaConditions ? &g_ruleIndex.mediaMatches : nullptr;
	RuleCandidateSignature signature;
	if (captureRuleCandidateSignature(selectorNode, signature)) {
		if (const ActiveRulePlanCacheEntry *cachedPlan = cachedActiveRulePlanForSignature(signature)) {
			setActiveRulePlanFromCache(plan, *cachedPlan);
			return;
		}
		if (const RuleCandidateCacheEntry *candidateEntry = cachedRuleCandidateEntryForSignature(signature)) {
			for (std::size_t i = 0, n = candidateEntry->rules.size(); i < n; ++i) {
				const int ri = candidateEntry->rules[i];
				addCachedActiveCandidateRule(plan, selectorNode, ri, ruleList, mediaMatches, &signature);
			}
			collapseShadowedActiveRules(plan);
			if (candidateEntry->signatureLocal)
				storeActiveRulePlanForSignature(signature, plan);
			return;
		}
	}
	const std::uint16_t serial = nextCandidateCollectSerial();
	const NodeClassList &classes = state.classLists[selectorNode];
	for (std::size_t classIndex = 0, classCount = classes.size(); classIndex < classCount; ++classIndex) {
		const CssAtomId cls = classes.at(classIndex);
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.byClass.get(cls), serial, ruleList, mediaMatches);
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selByClass.get(cls), serial, ruleList, mediaMatches);
	}
	const CssAtomId idAtom = nodeIdAttributeAtom(selectorNode);
	if (idAtom != kInvalidCssAtom)
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selById.get(idAtom), serial, ruleList, mediaMatches);
	const int16_t nodeTag = state.nodes[selectorNode].tag_id;
	addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.byTag.get(nodeTag), serial, ruleList, mediaMatches);
	addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selByTag.get(nodeTag), serial, ruleList, mediaMatches);
	if (isRootNode(selectorNode))
		addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selRoot, serial, ruleList, mediaMatches);
	addActiveCandidateRuleBucket(plan, selectorNode, g_ruleIndex.selAlways, serial, ruleList, mediaMatches);
	sortActiveRulePlanByCascade(plan);
	collapseShadowedActiveRules(plan);
}

bool applyCachedStyleApplyOpWithSource(NodeHandle node,
                                       std::uint16_t handle,
                                       std::uint16_t compiledValue,
                                       StyleApplicationSource source)
{
	if (!node || handle == kNoCachedStyleApplyOp) return false;
	const auto &ops = cachedStyleApplyOps();
	if (handle >= ops.size()) return false;
	const CachedStyleApplyOp &op = ops[handle];
	switch (op.kind) {
	case CachedStyleApplyOpKind::Noop:
		return true;
	case CachedStyleApplyOpKind::Transform: {
		setStyleValue(node, Property::TransformTranslateOuterAxes, op.values[1], source);
		setStyleValue(node, Property::TransformPresent, op.values[0], source);
		if (applyTransformSlotsFast(node, op.transform, source)) return true;
		TransformComponents transform;
		transform.hasRotateX = op.values[0] != 0;
		transform.translateOuterAxes = op.values[1];
		transform.rotateX = op.transform[0];
		transform.rotateY = op.transform[1];
		transform.rotateZ = op.transform[2];
		transform.translateX = op.transform[3];
		transform.translateY = op.transform[4];
		transform.translateZ = op.transform[5];
		transform.translateXPercent = op.transform[6];
		transform.translateYPercent = op.transform[7];
		transform.scaleX = op.transform[8];
		transform.scaleY = op.transform[9];
		transform.scaleZ = op.transform[10];
		if (!applyTransformComponentsFast(node, transform, source))
			setTransformComponents(node, transform, source);
		return true;
	}
	case CachedStyleApplyOpKind::CompiledTransform: {
		const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue);
		if (!compiled || compiled->kind != CssCompiledKind::Transform) return false;
		const TransformComponents transform = transformFromCompiled(*compiled, node.id());
		if (compiled->declaration == CssDeclarationId::Translate) { setIndividualTranslation(node, transform, source); return true; }
		if (!applyTransformComponentsFast(node, transform, source))
			setTransformComponents(node, transform, source);
		return true;
	}
	case CachedStyleApplyOpKind::Color:
		return applyCompiledColorValue(node,
		                               static_cast<CssDeclarationId>(op.declaration),
		                               op.values[0],
		                               op.values[1],
		                               op.values[2],
		                               source);
	case CachedStyleApplyOpKind::ColorVar: {
		ResolvedCompiledCssColor color;
		const bool hasFallback = op.values[3] >= 0;
		if (!resolveCompiledColorRef(node.id(),
		                             static_cast<CssAtomId>(op.values[0]),
		                             hasFallback ? 1 : 0,
		                             op.values[1],
		                             static_cast<style_color_t>(op.values[2]),
		                             static_cast<std::uint8_t>(hasFallback ? op.values[3] : 0),
		                             color))
			return false;
		return applyCompiledColorValue(node,
		                               static_cast<CssDeclarationId>(op.declaration),
		                               color.styleColor,
		                               color.nativeColor,
		                               color.alpha,
		                               source);
	}
	case CachedStyleApplyOpKind::Background:
	case CachedStyleApplyOpKind::StaticBackground:
		return applyCompiledBackgroundValue(node, op.values[0], static_cast<CssDeclarationId>(op.declaration), source);
	case CachedStyleApplyOpKind::BackgroundSize: {
		const int nodeId = node.id();
		auto &state = treeState();
		if (nodeId < 0 || nodeId >= state.nodeCount) return true;
		Node &target = state.nodes[nodeId];
		if (rstyle(target.style).bg_grid_axes == 0) return true;
		RareStyle &rs = rstyleMut(target.style);
		if (op.values[0] > 0) rs.bg_grid_step_x = static_cast<std::uint16_t>(op.values[0]);
		if (op.values[1] > 0) rs.bg_grid_step_y = static_cast<std::uint16_t>(op.values[1]);
		markNodeDisplayCommandsDirtyForStyleApply(nodeId);
		return true;
	}
	case CachedStyleApplyOpKind::GridTemplate: {
		const CssCompiledGridTemplate *grid =
		    compiledCssGridTemplateForHandle(static_cast<std::uint16_t>(op.values[0]));
		if (!grid) return false;
		const auto declaration = static_cast<CssDeclarationId>(op.declaration);
		if (declaration == CssDeclarationId::GridTemplateColumns) {
			applyCompiledGridTemplateValue(node, *grid, true);
			return true;
		}
		if (declaration == CssDeclarationId::GridTemplateRows) {
			applyCompiledGridTemplateValue(node, *grid, false);
			return true;
		}
		return false;
	}
	case CachedStyleApplyOpKind::RuntimeLength:
		return applyRuntimeLengthValue(node,
		                               static_cast<CssDeclarationId>(op.declaration),
		                               loadCachedLengthSpec(op, 0),
		                               source);
	case CachedStyleApplyOpKind::RuntimeSize:
		return applyRuntimeSizeValue(node,
		                             static_cast<CssDeclarationId>(op.declaration),
		                             loadCachedLengthSpec(op, 0),
		                             source);
	case CachedStyleApplyOpKind::RuntimePositionOffset:
		return applyRuntimePositionOffsetValue(node,
		                                       static_cast<CssDeclarationId>(op.declaration),
		                                       loadCachedLengthSpec(op, 0),
		                                       source);
	case CachedStyleApplyOpKind::RuntimeBackgroundSize:
		return applyRuntimeBackgroundSizeValue(node,
		                                       loadCachedLengthSpec(op, 0),
		                                       loadCachedLengthSpec(op, 2));
	case CachedStyleApplyOpKind::RuntimeFlex:
		return applyRuntimeFlexValue(node,
		                             op.values[0],
		                             op.values[1],
		                             1, // RuntimeFlex is emitted only for an authored basis; slot 2 holds its float bits.
		                             loadCachedLengthSpec(op, 2),
		                             source);
	case CachedStyleApplyOpKind::RuntimeFlexBasis:
		return applyRuntimeFlexBasisValue(node, 1, loadCachedLengthSpec(op, 0), source);
	case CachedStyleApplyOpKind::RuntimeBorderShorthand:
		return applyRuntimeBorderShorthandValue(node,
		                                        loadCachedLengthSpec(op, 0),
		                                        op.values[2],
		                                        op.values[3],
		                                        op.borderRelief,
		                                        source);
	case CachedStyleApplyOpKind::RuntimeBorderSideShorthand:
		return applyRuntimeBorderSideShorthandValue(node,
		                                            static_cast<CssDeclarationId>(op.declaration),
		                                            loadCachedLengthSpec(op, 0),
		                                            op.values[2],
		                                            op.values[3],
		                                            op.borderRelief,
		                                            source);
	case CachedStyleApplyOpKind::RuntimeFilterBlur:
		return applyRuntimeFilterBlurValue(node,
		                                   static_cast<std::uint8_t>(op.values[0]),
		                                   loadCachedLengthSpec(op, 1),
		                                   source);
	case CachedStyleApplyOpKind::RuntimeLineHeight:
		return applyRuntimeLineHeightValue(node,
		                                   static_cast<std::uint8_t>(op.values[0]),
		                                   loadCachedLengthSpec(op, 1),
		                                   source);
	case CachedStyleApplyOpKind::CompiledLength:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledLengthValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledSize:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledSizeValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledPositionOffset:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledPositionOffsetValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBox:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBoxValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBackgroundSize:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBackgroundSizeValue(node, *compiled);
		return false;
	case CachedStyleApplyOpKind::CompiledFlex:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledFlexValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledFlexBasis:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledFlexBasisValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBorderShorthand:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBorderShorthandValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBorderSideShorthand:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBorderSideShorthandValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBorderRadius:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBorderRadiusValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledFilterBlur:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledFilterBlurValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledBoxShadow:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledBoxShadowValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::CompiledLineHeight:
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(compiledValue))
			return applyCompiledLineHeightValue(node, *compiled, source);
		return false;
	case CachedStyleApplyOpKind::DirectProperties:
		if (source == StyleApplicationSource::ClassRule) {
			auto &state = treeState();
			if (state.styleInvalidationSuppressionDepth > 0) {
				const int nodeId = node.id();
				if (nodeId < 0 || nodeId >= state.nodeCount) return true;
				Node &target = state.nodes[nodeId];
				for (std::uint8_t i = 0; i < op.propertyCount; ++i) {
					const int propertyIndex = op.properties[i];
					if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
					if (!setClassRuleValueFastUnchecked(target, static_cast<Property>(propertyIndex), op.values[i])) return false;
				}
				return true;
			}
		}
		for (std::uint8_t i = 0; i < op.propertyCount; ++i) {
			const int propertyIndex = op.properties[i];
			if (propertyIndex < 0 || propertyIndex >= static_cast<int>(Property::Count)) return false;
			setStyleValue(node, static_cast<Property>(propertyIndex), op.values[i], source);
		}
		return true;
	}
	return false;
}

void applyCachedRuleWithSource(NodeHandle node, const CachedRuleApply &op, StyleApplicationSource source)
{
	if (applyCachedStyleApplyOpWithSource(node, op.styleOp, op.compiledValue, source)) return;
	if (!node || op.ruleIndex == kNoCachedRuleIndex) return;
	const auto &ruleList = rules();
	if (op.ruleIndex >= ruleList.size()) return;
	const CssRule &rule = ruleList[op.ruleIndex];
	const CssCompiledValue *compiled = compiledCssValueForHandle(op.compiledValue);
	applyRulePropertyWithCompiledValue(node, rule, compiled, source);
}

bool setsFontMetrics(CssDeclarationId declaration)
{
	return declaration == CssDeclarationId::Font || declaration == CssDeclarationId::FontFamily ||
	       declaration == CssDeclarationId::FontSize || declaration == CssDeclarationId::FontWeight;
}

void applyActiveRuleSpanToNode(int node, const ActiveRulePlan &plan, int bucketId, int metricsOnly = 0)
{
	auto matchesMetrics = [metricsOnly](CssDeclarationId declaration) {
		return metricsOnly == 1 ? setsFontMetrics(declaration)
		    : declaration == CssDeclarationId::Font || declaration == CssDeclarationId::LineHeight;
	};
	const NodeHandle nodeHandle(node);
	if (!nodeHandle) return;
	if (plan.cachedEntry) {
		const CachedRuleApplyBucketSpan bucket = activeRulePlanCachedRuleSpan(plan, bucketId);
		for (std::size_t i = 0; i < bucket.count; ++i) {
			if (metricsOnly && (bucket.data[i].ruleIndex >= rules().size() ||
			    !matchesMetrics(rules()[bucket.data[i].ruleIndex].declaration))) continue;
			applyCachedRuleWithSource(nodeHandle, bucket.data[i], StyleApplicationSource::ClassRule);
		}
		return;
	}

	const auto &ruleList = rules();
	const DenseRuleBucketSpan bucket = activeRulePlanBucketSpan(plan, bucketId);
	for (std::size_t i = 0; i < bucket.count; ++i) {
		const int ri = bucket.data[i];
		if (ri < 0 || static_cast<std::size_t>(ri) >= ruleList.size()) continue;
		const CssRule &rule = ruleList[static_cast<std::size_t>(ri)];
		if (metricsOnly && !matchesMetrics(rule.declaration)) continue;
		const std::uint16_t styleOp =
		    static_cast<std::size_t>(ri) < g_ruleIndex.styleOps.size()
		        ? g_ruleIndex.styleOps[static_cast<std::size_t>(ri)]
		        : cachedStyleApplyOpForCompiledValue(compiledCssValueForHandle(rule.compiledValue));
		if (applyCachedStyleApplyOpWithSource(nodeHandle, styleOp, rule.compiledValue, StyleApplicationSource::ClassRule)) continue;
		const CssCompiledValue *compiled = compiledCssValueForHandle(rule.compiledValue);
		applyRulePropertyWithCompiledValue(nodeHandle, rule, compiled, StyleApplicationSource::ClassRule);
	}
}

void applyActiveRuleSpansToNode(int node, const ActiveRulePlan &plan, int customBucket, int ruleBucket)
{
	applyActiveRuleSpanToNode(node, plan, customBucket);
	// Resolve the font cascade before any property consumes its metrics. Keep
	// the winning font fixed while replaying the ordinary cascade so intervening
	// font declarations cannot change the basis of a padding/line-height/length.
	applyActiveRuleSpanToNode(node, plan, ruleBucket, true);
	if (const NodeRareData *rd = rareDataFor(node))
		for (std::size_t i = 0; i < rd->inlineStyles.size(); ++i) {
			const auto &entry = rd->inlineStyles.at(i);
			if (isFontMetricProperty(entry.property))
				Tree::instance().setStyleFromClass(node, entry.property, entry.value);
		}
	const int previousFontNode = g_resolvedFontNode;
	g_resolvedFontNode = node;
	// Inherited unitless numbers are resolved against this element's final font.
	// Lengths and percentages inherit their already-computed pixel value.
	auto &style = treeState().nodes[node].style;
	if (style.line_height_multiplier >= 0)
		style.line_height = resolveLineHeightMultiplier(node, style.line_height_multiplier);
	// lh consumers need the winning line-height, evaluated using the final font.
	applyActiveRuleSpanToNode(node, plan, ruleBucket, 2);
	if (const NodeRareData *rd = rareDataFor(node))
		for (std::size_t i = 0; i < rd->inlineStyles.size(); ++i) {
			const auto &entry = rd->inlineStyles.at(i);
			if (isLineHeightProperty(entry.property)) Tree::instance().setStyleFromClass(node, entry.property, entry.value);
		}
	const int previousLineHeightNode = g_resolvedLineHeightNode;
	g_resolvedLineHeightNode = node;
	applyActiveRuleSpanToNode(node, plan, ruleBucket);
	g_resolvedLineHeightNode = previousLineHeightNode;
	g_resolvedFontNode = previousFontNode;
}

void applyFirstLineBackground(int node, const ActiveRulePlan &plan)
{
	NodeRareData *rare = rareDataFor(node);
	const FirstLineBackground previous = rare ? rare->firstLineBackground : FirstLineBackground{};
	if (rare) rare->firstLineBackground = FirstLineBackground{};
	const Node &target = Tree::instance().node(node);
	if (LayoutEngine::isCssInlineLevelBox(target)) return;
	const DenseRuleBucketSpan bucket = activeRulePlanBucketSpan(plan, kActiveFirstLineRule);
	const CachedRuleApplyBucketSpan cachedBucket = plan.cachedEntry
	    ? activeRulePlanCachedRuleSpan(plan, kActiveFirstLineRule)
	    : CachedRuleApplyBucketSpan{};
	if ((!plan.cachedEntry && bucket.empty()) || (plan.cachedEntry && cachedBucket.count == 0)) return;
	if (!rare) rare = &ensureRareData(node);
	auto applyRule = [&](int ri, std::uint16_t compiledHandle) {
		if (ri < 0 || static_cast<std::size_t>(ri) >= rules().size()) return;
		const CssRule &rule = rules()[static_cast<std::size_t>(ri)];
		if (rule.declaration != CssDeclarationId::Background &&
		    rule.declaration != CssDeclarationId::BackgroundColor) return;
		const CssCompiledValue *compiled = compiledCssValueForHandle(compiledHandle);
		if (!compiled) return;
		if (compiled->kind == CssCompiledKind::Color) {
			rare->firstLineBackground.color = static_cast<style_color_t>(compiled->values[1]);
			rare->firstLineBackground.alpha = static_cast<std::uint8_t>(compiled->values[2]);
			rare->firstLineBackground.hasColor = true;
		} else if (compiled->kind == CssCompiledKind::ColorVar) {
			const CssAtomId atom = static_cast<CssAtomId>(compiled->values[0]);
			if (const NodeCustomProperty *entry = lookupCustomPropertyEntry(node, atom)) {
				CachedCssColor color;
				if (entry->hasColor()) {
					color.nativeColor = entry->colorNative;
					color.alpha = entry->colorAlpha;
					color.valid = true;
				} else {
					color = cachedCssColorForValue(entry->value);
				}
				if (color.valid) {
					rare->firstLineBackground.color = color.nativeColor;
					rare->firstLineBackground.alpha = color.alpha;
					rare->firstLineBackground.hasColor = true;
				}
			} else if (compiled->aux != 0) {
				rare->firstLineBackground.color = static_cast<style_color_t>(compiled->values[2]);
				rare->firstLineBackground.alpha = static_cast<std::uint8_t>(compiled->values[3]);
				rare->firstLineBackground.hasColor = true;
			}
		} else if (compiled->kind == CssCompiledKind::Background) {
			const CssCompiledBackground *background = compiledCssBackgroundForHandle(
			    static_cast<std::uint16_t>(compiled->values[0]));
			if (background) {
				rare->firstLineBackground.color = StyleValues::pixelFromStyleValue(background->colorStyle);
				rare->firstLineBackground.alpha = background->colorAlpha;
				rare->firstLineBackground.hasColor = background->colorAlpha != 0;
			}
		}
	};
	if (plan.cachedEntry) {
		for (std::size_t i = 0; i < cachedBucket.count; ++i)
			applyRule(cachedBucket.data[i].ruleIndex, cachedBucket.data[i].compiledValue);
	} else {
		for (std::size_t i = 0; i < bucket.count; ++i) {
			const int ri = bucket.data[i];
			if (ri < 0 || static_cast<std::size_t>(ri) >= rules().size()) continue;
			applyRule(ri, rules()[static_cast<std::size_t>(ri)].compiledValue);
		}
	}
	if (previous.hasColor == rare->firstLineBackground.hasColor &&
	    previous.color == rare->firstLineBackground.color &&
	    previous.alpha == rare->firstLineBackground.alpha) {
		rare->firstLineBackground.lineY = previous.lineY;
		rare->firstLineBackground.lineHeight = previous.lineHeight;
		rare->firstLineBackground.lineContextNode = previous.lineContextNode;
		rare->firstLineBackground.lineValid = previous.lineValid;
	}
}

void syncPseudoElementsForNode(int node, const ActiveRulePlan *existingPlan = nullptr)
{
	ActiveRulePlan ownedPlan;
	const ActiveRulePlan *plan = existingPlan;
	if (!plan) {
		buildActiveRulePlanForNode(node, ownedPlan);
		plan = &ownedPlan;
	}

	const struct {
		CssRule::PseudoElement pseudo;
		int customBucket;
		int ruleBucket;
	} pseudoBuckets[] = {
	    {CssRule::PseudoElement::Before, kActiveBeforeCustom, kActiveBeforeRule},
	    {CssRule::PseudoElement::After, kActiveAfterCustom, kActiveAfterRule},
	};
	for (const auto &entry : pseudoBuckets) {
		const bool hasMatchingRule = plan->has(entry.customBucket) || plan->has(entry.ruleBucket);
		if (!hasMatchingRule) {
			const int stalePseudoNode = findPseudoChild(node, entry.pseudo);
			if (stalePseudoNode >= 0) Tree::instance().removeNode(stalePseudoNode);
			continue;
		}
		const int pseudoNode = ensurePseudoChild(node, entry.pseudo);
		if (pseudoNode < 0) continue;
		if (NodeRareData *rd = rareDataFor(pseudoNode)) rd->customProperties.clear();
		clearCustomPropertyLookupCache();
		int16_t staleRareStyle = treeState().nodes[pseudoNode].style.rare_style;
		Tree::instance().resetStyleForClassRecompute(pseudoNode);
		if (staleRareStyle >= 0 && treeState().nodes[pseudoNode].style.rare_style != staleRareStyle)
			releaseRareStyle(staleRareStyle);
		applyInheritedStyleDefaults(pseudoNode);
		applyDefaultStyleOverrides(pseudoNode);
		applyActiveRuleSpansToNode(pseudoNode, *plan, entry.customBucket, entry.ruleBucket);
		replayInlineStyles(pseudoNode);
	}
}

void recomputeNodeClassStyles(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;

	const ComputedStyle beforeStyle = state.nodes[node].style;
	const FirstLineBackground beforeFirstLine = rareDataFor(node)
	    ? rareDataFor(node)->firstLineBackground : FirstLineBackground{};
	int16_t staleRareStyle = beforeStyle.rare_style;
	const int beforeImageId = state.nodes[node].image_id;
	state.styleInvalidationSuppressionDepth++;
	// Record this node's custom-property dependencies fresh (lookupCustomProperty
	// appends into g_nodeRefs[g_recordingNode] for every var() resolved here,
	// including the node's pseudo-elements via syncPseudoElementsForNode below).
	const int prevRecordingNode = g_recordingNode;
	g_recordingNode = node;
	if (node >= 0 && node < kMaxNodes) g_nodeRefs[node].clearForRecompute();
#if GEA_RECPROF
	g_profNodes++;
	int64_t _t = recNow();
#endif
	if (NodeRareData *rd = rareDataFor(node)) rd->customProperties = rd->inlineCustomProperties;
	clearCustomPropertyLookupCache();
	Tree::instance().resetStyleForClassRecompute(node);
	applyInheritedStyleDefaults(node);
	applyDefaultStyleOverrides(node);
#if GEA_RECPROF
	g_profResetUs += recNow() - _t;
	_t = recNow();
#endif
	ActiveRulePlan activePlan;
	buildActiveRulePlanForNode(node, activePlan);
#if GEA_RECPROF
	g_profCandUs += recNow() - _t;
	_t = recNow();
#endif
	applyActiveRuleSpansToNode(node, activePlan, kActiveMainCustom, kActiveMainRule);
#if GEA_RECPROF
	g_profApplyUs += recNow() - _t;
	_t = recNow();
#endif
	replayInlineStyles(node);
	applyFirstLineBackground(node, activePlan);
	const FirstLineBackground afterFirstLine = rareDataFor(node)
	    ? rareDataFor(node)->firstLineBackground : FirstLineBackground{};
	const bool firstLineChanged = beforeFirstLine.hasColor != afterFirstLine.hasColor ||
	                              beforeFirstLine.color != afterFirstLine.color ||
	                              beforeFirstLine.alpha != afterFirstLine.alpha;
	// Inline overrides store expression handles, not a newly evaluated pixel
	// value. Seed used edges before diffing so custom/font changes schedule
	// layout even when the expression handle itself is unchanged.
	resolveLayoutBoxLengths(node, percentBasisForNode(node, LengthAxis::Horizontal));
	primeCssAnimationsForNode(node, &activePlan);
	// A runtime `src` attribute's image id is NOT class-derived, so the reset
	// above must not lose it: restore it unless a class rule supplied its own
	// image. (Re-resolving from the attribute instead would re-read and
	// re-decode the file — e.g. a full-page EPUB cover from the SD card — on
	// every recompute of the node or any ancestor.)
	if (state.nodes[node].type == NodeType::Image && state.nodes[node].image_id < 0 &&
	    beforeImageId >= 0 && Tree::instance().hasAttribute(node, "src"))
		state.nodes[node].image_id = beforeImageId;
	state.styleInvalidationSuppressionDepth--;
	markClassRecomputeStyleDiff(node, beforeStyle, beforeImageId, firstLineChanged);
	if (staleRareStyle >= 0 && state.nodes[node].style.rare_style != staleRareStyle)
		releaseRareStyle(staleRareStyle);
#if GEA_RECPROF
	g_profMiscUs += recNow() - _t;
	_t = recNow();
#endif
	if (!isGeneratedPseudoNode(state.nodes[node]) &&
	    (g_ruleIndex.hasPseudoElementRules || nodeHasGeneratedPseudoChild(node)))
		syncPseudoElementsForNode(node, &activePlan);
#if GEA_RECPROF
	g_profPseudoUs += recNow() - _t;
#endif
	g_recordingNode = prevRecordingNode;
}

// When set (during the initial app mount), per-op class-style recomputes are
// deferred. Otherwise every setTagName / setClassName / appendChild re-walks the
// growing subtree against every CSS rule, which is O(nodes^2 x rules) over a
// mount — pathological for a large tree + large stylesheet (e.g. the weather
// app: 34s of CPU). endStyleMountBatch() clears this and runs a single
// recomputeAllClassStyles() pass, which produces identical final styles (it is
// the same pass the resize path already uses).
bool g_styleMountBatchActive = false;
// Roots whose subtree recompute was deferred while a batch is active. Processed
// (deduped + ancestor-subsumed) by endStyleMountBatch into the minimum set of
// subtree recomputes. This coalesces a burst of class changes — an app mount, or
// a reactive update like a city switch that flips a near-root class plus a few
// descendant classes — from N full/overlapping recomputes into one pass.
std::vector<int> g_pendingRecomputeRoots;

void recomputeSubtreeClassStyles(int node)
{
	if (g_styleMountBatchActive) {
		g_pendingRecomputeRoots.push_back(node);
		return;
	}
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	recomputeNodeClassStyles(node);
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		recomputeSubtreeClassStyles(child);
	}
}

void recomputeDescendantClassStyles(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		recomputeSubtreeClassStyles(child);
	}
}

// --- Incremental subtree recompute ----------------------------------------
// Computed parent values consumed by descendants, either implicitly (see
// applyInheritedStyleDefaults) or via explicit border-width inheritance.
struct ParentStyleSnapshot {
	style_color_t text_color;
	std::uint8_t text_alpha;
	std::int16_t font_id;
	std::int16_t font_size;
	std::int16_t font_weight;
	std::int16_t line_height;
	std::int32_t line_height_multiplier;
	std::uint8_t text_align;
	std::uint8_t text_transform;
	std::uint8_t white_space;
	std::uint8_t visibility;
	std::array<int, 4> border_widths;
};

ParentStyleSnapshot snapshotParentStyle(const ComputedStyle &s)
{
	return ParentStyleSnapshot{s.text_color, s.text_alpha, static_cast<std::int16_t>(s.font_id),
	                       static_cast<std::int16_t>(s.font_size), static_cast<std::int16_t>(s.font_weight),
	                       static_cast<std::int16_t>(s.line_height), s.line_height_multiplier,
	                       static_cast<std::uint8_t>(s.text_align), static_cast<std::uint8_t>(s.text_transform),
	                       static_cast<std::uint8_t>(s.white_space), static_cast<std::uint8_t>(s.visibility),
	                       {computedBorderWidth(s, 0), computedBorderWidth(s, 1), computedBorderWidth(s, 2), computedBorderWidth(s, 3)}};
}

bool parentStylesDiffer(const ParentStyleSnapshot &a, const ParentStyleSnapshot &b)
{
	return a.text_color != b.text_color || a.text_alpha != b.text_alpha || a.font_id != b.font_id || a.font_size != b.font_size ||
	       a.font_weight != b.font_weight || a.line_height != b.line_height || a.line_height_multiplier != b.line_height_multiplier ||
	       a.text_align != b.text_align || a.text_transform != b.text_transform ||
	       a.white_space != b.white_space || a.visibility != b.visibility || a.border_widths != b.border_widths;
}

inline void listInsertUnique(CssAtomSmallList &v, CssAtomId s)
{
	v.insertUnique(s);
}

inline void listErase(CssAtomSmallList &v, CssAtomId s)
{
	v.erase(s);
}

// True if any of the node's current classes (or its tag) is used as an ancestor
// matcher in some complex selector — so a change to this node's class set could
// flip which rules its DESCENDANTS match.
bool classChangeAffectsDescendants(int node)
{
	if (g_ruleIndex.ancestorClasses.empty() && g_ruleIndex.ancestorTags.empty()) return false;
	const auto &state = treeState();
	const NodeClassList &classes = state.classLists[node];
	for (std::size_t classIndex = 0, classCount = classes.size(); classIndex < classCount; ++classIndex) {
		const CssAtomId cls = classes.at(classIndex);
		if (g_ruleIndex.ancestorClasses.contains(cls)) return true;
	}
	return g_ruleIndex.ancestorTags.contains(state.nodes[node].tag_id);
}

struct NodeClassSnapshot {
	static constexpr std::size_t kInlineCount = 8;

	CssAtomId inlineTokens[kInlineCount]{};
	CssAtomId *spillTokens = nullptr;
	std::size_t count = 0;

	NodeClassSnapshot() = default;
	explicit NodeClassSnapshot(const NodeClassList &classes) { capture(classes); }
	NodeClassSnapshot(const NodeClassSnapshot &) = delete;
	NodeClassSnapshot &operator=(const NodeClassSnapshot &) = delete;

	~NodeClassSnapshot()
	{
		delete[] spillTokens;
	}

	void capture(const NodeClassList &classes)
	{
		delete[] spillTokens;
		spillTokens = nullptr;
		count = classes.size();
		CssAtomId *out = count > kInlineCount ? (spillTokens = new CssAtomId[count]) : inlineTokens;
		for (std::size_t i = 0; i < count; ++i)
			out[i] = classes.at(i);
	}

	CssAtomId at(std::size_t index) const
	{
		return spillTokens ? spillTokens[index] : inlineTokens[index];
	}
};

bool classTokensTouchAncestorSelectors(const NodeClassSnapshot &oldTokens, const NodeClassList &current)
{
	if (g_ruleIndex.ancestorClasses.empty()) return false;
	for (std::size_t i = 0; i < oldTokens.count; ++i) {
		const CssAtomId token = oldTokens.at(i);
		if (g_ruleIndex.ancestorClasses.contains(token)) return true;
	}
	for (std::size_t classIndex = 0, classCount = current.size(); classIndex < classCount; ++classIndex) {
		const CssAtomId token = current.at(classIndex);
		if (g_ruleIndex.ancestorClasses.contains(token)) return true;
	}
	return false;
}

struct CustomPropertyFingerprint {
	CssAtomId nameId = kInvalidCssAtom;
	CssAtomId valueAtom = kInvalidCssAtom;
	std::int32_t colorStyle = 0;
	std::int32_t colorNative = 0;
	float lengthValue = 0.0f;
	std::uint8_t colorAlpha = 255;
	std::uint8_t lengthUnit = 0;
	std::uint8_t flags = 0;
	bool valueEmpty = true;
	bool exact = true;
};

CustomPropertyFingerprint customPropertyFingerprint(const NodeCustomProperty &property)
{
	CustomPropertyFingerprint out;
	out.nameId = property.nameId;
	out.valueEmpty = property.value.empty();
	out.valueAtom = property.valueAtom;
	out.colorStyle = property.colorStyle;
	out.colorNative = property.colorNative;
	out.lengthValue = property.lengthValue;
	out.colorAlpha = property.colorAlpha;
	out.lengthUnit = property.lengthUnit;
	out.flags = property.flags;
	out.exact = out.valueEmpty || out.valueAtom != kInvalidCssAtom;
	return out;
}

bool customPropertyFingerprintEqualsValue(const CustomPropertyFingerprint &before,
                                          const NodeCustomProperty &after)
{
	if (before.nameId != after.nameId) return false;
	if (before.flags != after.flags) return false;
	if ((before.flags & 1u) != 0 &&
	    (before.colorStyle != after.colorStyle ||
	     before.colorNative != after.colorNative ||
	     before.colorAlpha != after.colorAlpha))
		return false;
	if ((before.flags & 2u) != 0 &&
	    (before.lengthUnit != after.lengthUnit ||
	     std::fabs(static_cast<double>(before.lengthValue) -
	               static_cast<double>(after.lengthValue)) >= 0.0001))
		return false;
	if (!before.exact) return false;  // conservative: recompute descendants rather than risk a stale var().
	if (before.valueEmpty) return after.value.empty();
	if (after.value.empty()) return false;
	return after.valueAtom == before.valueAtom && after.valueAtom != kInvalidCssAtom;
}

struct CustomPropertySnapshot {
	static constexpr std::uint8_t kInlineCount = 4;
	CustomPropertyFingerprint inlineEntries[kInlineCount]{};
	CustomPropertyFingerprint *spillEntries = nullptr;
	std::size_t spillCount = 0;
	std::size_t spillCapacity = 0;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	CustomPropertySnapshot() = default;
	CustomPropertySnapshot(const CustomPropertySnapshot &) = delete;
	CustomPropertySnapshot &operator=(const CustomPropertySnapshot &) = delete;

	~CustomPropertySnapshot()
	{
		delete[] spillEntries;
	}

	void add(const NodeCustomProperty &property)
	{
		const CustomPropertyFingerprint fingerprint = customPropertyFingerprint(property);
		if (!spilled && inlineCount < kInlineCount) {
			inlineEntries[inlineCount++] = fingerprint;
			return;
		}
		if (!spilled) {
			spillCapacity = kInlineCount * 2;
			spillEntries = new CustomPropertyFingerprint[spillCapacity];
			for (std::size_t i = 0; i < inlineCount; ++i)
				spillEntries[i] = inlineEntries[i];
			spillCount = inlineCount;
			spilled = true;
		}
		if (spillCount >= spillCapacity) {
			const std::size_t nextCapacity = spillCapacity ? spillCapacity * 2 : kInlineCount * 2;
			auto *next = new CustomPropertyFingerprint[nextCapacity];
			for (std::size_t i = 0; i < spillCount; ++i)
				next[i] = spillEntries[i];
			delete[] spillEntries;
			spillEntries = next;
			spillCapacity = nextCapacity;
		}
		spillEntries[spillCount++] = fingerprint;
	}

	std::size_t size() const { return spilled ? spillCount : inlineCount; }
	const CustomPropertyFingerprint &at(std::size_t index) const
	{
		return spilled ? spillEntries[index] : inlineEntries[index];
	}

	const CustomPropertyFingerprint *find(CssAtomId nameId) const
	{
		for (std::size_t i = 0, n = size(); i < n; ++i) {
			const auto &entry = at(i);
			if (entry.nameId == nameId) return &entry;
		}
		return nullptr;
	}
};

void snapshotCustomProperties(const NodeRareData *rareData, CustomPropertySnapshot &snapshot)
{
	if (!rareData) return;
	for (const auto &property : rareData->customProperties.values)
		snapshot.add(property);
}

void noteClassMutationForIncremental(int node, const NodeClassSnapshot &oldTokens)
{
	if (!g_styleMountBatchActive) return;
	rebuildRuleIndexIfNeeded();
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (classTokensTouchAncestorSelectors(oldTokens, state.classLists[node]))
		g_forceFullSubtreeMark.insert(node);
}

// Recompute `node` only if it is directly changed, inherits a changed value, or
// references a custom property whose resolved value changed; otherwise keep its
// existing (still-correct) computed style. `changedAbove` carries the names of
// custom properties whose value differs for this node vs the previous recompute;
// `forceByParent` is set when an ancestor's inheritable values changed;
// `forceSubtree` is set (and stays set for all descendants) when an ancestor's
// class change could alter descendant selector matches.
void recomputeNodeIncremental(int node, const CssAtomSmallList &changedAbove, bool forceByParent,
                              bool forceSubtree, const DenseNodeMark &pending)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (isGeneratedPseudoNode(state.nodes[node])) return;

	const NodeCustomPropRefs &refs = g_nodeRefs[node];
	const bool tracked = refs.tracked;
	const bool referencesChanged = refs.touches(changedAbove);
	const bool directlyChanged = pending.contains(node);
	// A never-recorded node (e.g. freshly created this batch) must recompute — we
	// have no dependency info to justify skipping it.
	const bool forceFull = g_forceFullSubtreeMark.contains(node);
	const bool mustRecompute = forceByParent || forceSubtree || forceFull || directlyChanged || referencesChanged || !tracked;
	// A node whose class change could flip a descendant-combinator match forces its
	// whole subtree (descendant selector matches may have changed). g_forceFullSubtreeMark
	// is the precise signal (captured at setClassName with both old+new classes);
	// classChangeAffectsDescendants is a current-class fallback for other paths.
	const bool childForceSubtree =
	    forceSubtree || forceFull || (directlyChanged && classChangeAffectsDescendants(node));

	CssAtomSmallList changedForChildren = changedAbove;
	bool inheritablesChanged = false;

	if (mustRecompute) {
#if GEA_INCREMENTAL_VERIFY
		g_incrRecomputedNodes.insert(node);
#endif
		const ParentStyleSnapshot before = snapshotParentStyle(state.nodes[node].style);
		CustomPropertySnapshot beforeCustom;
		snapshotCustomProperties(rareDataFor(node), beforeCustom);
		recomputeNodeClassStyles(node);
		inheritablesChanged = parentStylesDiffer(before, snapshotParentStyle(state.nodes[node].style));
		// Reconcile the cascaded custom-property set for descendants: a (re)defined
		// property whose value changed is now changed for them; one whose value is
		// unchanged shadows any same-named change from above; a removed one un-shadows.
		static const std::vector<NodeCustomProperty> kNoCustomProps;
		const NodeRareData *rdCustomAfter = rareDataFor(node);
		const auto &afterCustom = rdCustomAfter ? rdCustomAfter->customProperties.values : kNoCustomProps;
		for (const auto &kv : afterCustom) {
			const CustomPropertyFingerprint *old = beforeCustom.find(kv.nameId);
			if (!old || !customPropertyFingerprintEqualsValue(*old, kv)) listInsertUnique(changedForChildren, kv.nameId);
			else listErase(changedForChildren, kv.nameId);
		}
		for (std::size_t i = 0, n = beforeCustom.size(); i < n; ++i) {
			const CustomPropertyFingerprint &b = beforeCustom.at(i);
			bool stillDefined = false;
			for (const auto &kv : afterCustom)
				if (kv.nameId == b.nameId) { stillDefined = true; break; }
			if (!stillDefined) listInsertUnique(changedForChildren, b.nameId);
		}
	} else {
		// Skipped: style + custom properties unchanged. Any property this node defines
		// shadows a same-named change from above with its (unchanged) value.
		if (const NodeRareData *rd = rareDataFor(node))
			for (const auto &kv : rd->customProperties.values)
				listErase(changedForChildren, kv.nameId);
	}

	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling) {
		if (isGeneratedPseudoNode(state.nodes[child])) continue;
		recomputeNodeIncremental(child, changedForChildren, inheritablesChanged, childForceSubtree, pending);
	}
}

void recomputeSubtreeIncremental(int root, const DenseNodeMark &pending)
{
	// The root is a directly-changed node, so it always recomputes; its custom-prop
	// and inheritable diffs seed the descendant walk.
	recomputeNodeIncremental(root, CssAtomSmallList{}, /*forceByParent=*/true, /*forceSubtree=*/false, pending);
}

#if GEA_INCREMENTAL_VERIFY
void collectSubtreeStyles(int node, std::vector<int> &ids, std::vector<ComputedStyle> &styles, std::vector<int> &imageIds)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	ids.push_back(node);
	styles.push_back(state.nodes[node].style);
	imageIds.push_back(state.nodes[node].image_id);
	for (int child = state.nodes[node].first_child; child >= 0; child = state.nodes[child].next_sibling)
		collectSubtreeStyles(child, ids, styles, imageIds);
}
#endif

void syncNodeClassAttribute(int node)
{
	// No-op: the class is no longer mirrored into the attribute table — doing so
	// would force a rare-data block onto every styled node, defeating the sparse
	// attribute storage. getAttribute("class") reads the dense NodeClassList
	// directly (see Tree::getAttribute). Kept as a no-op so call sites are intact.
	(void)node;
}

void recomputeAllClassStyles()
{
	auto &state = treeState();
	g_nodeRefOverflow.clear();
	clearCustomPropertyLookupCache();
	for (auto &refs : g_nodeRefs) refs.reset();
	for (int i = 0; i < state.nodeCount; i++) {
		if (state.nodes[i].parent < 0) recomputeSubtreeClassStyles(i);
	}
}

int g_ruleRegistrationBatchDepth = 0;
bool g_ruleRegistrationRulesChanged = false;
bool g_ruleRegistrationKeyframesChanged = false;

void flushRuleRegistrationBatch()
{
	if (g_ruleRegistrationKeyframesChanged) {
		invalidateKeyframeRuleIndex();
		g_ruleRegistrationKeyframesChanged = false;
	}
	if (g_ruleRegistrationRulesChanged) {
		invalidateRuleIndex();
		recomputeAllClassStyles();
		g_ruleRegistrationRulesChanged = false;
	}
}

void noteStyleRuleRegistrationChanged()
{
	if (g_ruleRegistrationBatchDepth > 0) {
		g_ruleRegistrationRulesChanged = true;
		return;
	}
	invalidateRuleIndex();
	recomputeAllClassStyles();
}

void noteKeyframeRuleRegistrationChanged()
{
	if (g_ruleRegistrationBatchDepth > 0) {
		g_ruleRegistrationKeyframesChanged = true;
		return;
	}
	invalidateKeyframeRuleIndex();
}

std::vector<std::string> splitCssTokens(const std::string &value)
{
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < value.size()) {
		while (i < value.size() && static_cast<unsigned char>(value[i]) <= ' ') ++i;
		const std::size_t start = i;
		int depth = 0;
		while (i < value.size()) {
			const char c = value[i];
			if (c == '(') depth++;
			else if (c == ')' && depth > 0) depth--;
			else if (static_cast<unsigned char>(c) <= ' ' && depth == 0) break;
			++i;
		}
		if (i > start) out.push_back(value.substr(start, i - start));
	}
	return out;
}

bool parseTimeMs(const std::string &token, std::uint32_t &out)
{
	const std::string lower = toLowerAscii(trimCssValue(token));
	if (lower.size() < 2) return false;
	char *end = nullptr;
	const double amount = std::strtod(lower.c_str(), &end);
	if (end == lower.c_str()) return false;
	const std::string unit = trimCssValue(std::string(end));
	if (unit == "ms") {
		out = static_cast<std::uint32_t>(std::max(0, roundToInt(amount)));
		return true;
	}
	if (unit == "s") {
		out = static_cast<std::uint32_t>(std::max(0, roundToInt(amount * 1000.0)));
		return true;
	}
	return false;
}

gea::css::Easing easingFromCss(const std::string &token)
{
	const std::string name = toLowerAscii(trimCssValue(token));
	if (name == "linear") return gea::css::Easing::linear();
	if (name == "ease") return gea::css::Easing::ease();
	if (name == "ease-in") return gea::css::Easing::easeIn();
	if (name == "ease-out") return gea::css::Easing::easeOut();
	if (name == "ease-in-out") return gea::css::Easing::easeInOut();
	if (startsWith(name, "steps(")) {
		const int n = static_cast<int>(std::strtol(functionInner(name, "steps").c_str(), nullptr, 10));
		return gea::css::Easing::steps(n > 0 ? n : 1);
	}
	if (startsWith(name, "cubic-bezier(")) {
		const auto parts = splitTopLevel(functionInner(name, "cubic-bezier"), ',');
		if (parts.size() == 4) {
			return gea::css::Easing::cubicBezier(std::strtod(parts[0].c_str(), nullptr),
			                                    std::strtod(parts[1].c_str(), nullptr),
			                                    std::strtod(parts[2].c_str(), nullptr),
			                                    std::strtod(parts[3].c_str(), nullptr));
		}
	}
	return gea::css::Easing::ease();
}

struct CssAnimationSpec {
	CssAtomId nameAtom = kInvalidCssAtom;
	std::uint32_t durationMs = 0;
	std::uint32_t delayMs = 0;
	int iterations = 1;
	gea::css::Direction direction = gea::css::Direction::Normal;
	gea::css::Fill fill = gea::css::Fill::None;
	gea::css::Easing easing = gea::css::Easing::ease();
	bool valid = false;
};

CssAnimationSpec parseAnimationShorthand(const std::string &value)
{
	CssAnimationSpec spec;
	bool sawDuration = false;
	bool sawName = false;
	for (const auto &tokenRaw : splitCssTokens(value)) {
		const std::string token = trimCssValue(tokenRaw);
		const std::string lower = toLowerAscii(token);
		std::uint32_t timeMs = 0;
		if (parseTimeMs(lower, timeMs)) {
			if (!sawDuration) {
				spec.durationMs = timeMs;
				sawDuration = true;
			} else {
				spec.delayMs = timeMs;
			}
			continue;
		}
		if (lower == "infinite") {
			spec.iterations = -1;
			continue;
		}
		char *end = nullptr;
		const long count = std::strtol(lower.c_str(), &end, 10);
		if (end && *end == '\0' && count >= 0) {
			spec.iterations = static_cast<int>(count);
			continue;
		}
		if (lower == "reverse") {
			spec.direction = gea::css::Direction::Reverse;
			continue;
		}
		if (lower == "alternate") {
			spec.direction = gea::css::Direction::Alternate;
			continue;
		}
		if (lower == "alternate-reverse") {
			spec.direction = gea::css::Direction::AlternateReverse;
			continue;
		}
		if (lower == "forwards") {
			spec.fill = gea::css::Fill::Forwards;
			continue;
		}
		if (lower == "backwards") {
			spec.fill = gea::css::Fill::Backwards;
			continue;
		}
		if (lower == "both") {
			spec.fill = gea::css::Fill::Both;
			continue;
		}
		if (lower == "linear" || lower == "ease" || lower == "ease-in" ||
		    lower == "ease-out" || lower == "ease-in-out" ||
		    startsWith(lower, "steps(") || startsWith(lower, "cubic-bezier(")) {
			spec.easing = easingFromCss(token);
			continue;
		}
		if (lower == "normal" || lower == "running" || lower == "none") continue;
		if (!sawName) {
			sawName = true;
			spec.nameAtom = internCssAtom(token);
		}
	}
	spec.valid = spec.nameAtom != kInvalidCssAtom && spec.durationMs > 0;
	return spec;
}

std::vector<CssAnimationSpec> &compiledCssAnimationSpecs()
{
	static std::vector<CssAnimationSpec> specs;
	return specs;
}

gea::css::Direction staticAnimationDirection(StaticStyleAnimationDirection direction)
{
	switch (direction) {
	case StaticStyleAnimationDirection::Normal: return gea::css::Direction::Normal;
	case StaticStyleAnimationDirection::Reverse: return gea::css::Direction::Reverse;
	case StaticStyleAnimationDirection::Alternate: return gea::css::Direction::Alternate;
	case StaticStyleAnimationDirection::AlternateReverse: return gea::css::Direction::AlternateReverse;
	}
	return gea::css::Direction::Normal;
}

gea::css::Fill staticAnimationFill(StaticStyleAnimationFill fill)
{
	switch (fill) {
	case StaticStyleAnimationFill::None: return gea::css::Fill::None;
	case StaticStyleAnimationFill::Forwards: return gea::css::Fill::Forwards;
	case StaticStyleAnimationFill::Backwards: return gea::css::Fill::Backwards;
	case StaticStyleAnimationFill::Both: return gea::css::Fill::Both;
	}
	return gea::css::Fill::None;
}

gea::css::Easing staticAnimationEasing(StaticStyleAnimationEasing easing)
{
	switch (easing.kind) {
	case StaticStyleAnimationEasingKind::Linear: return gea::css::Easing::linear();
	case StaticStyleAnimationEasingKind::Ease: return gea::css::Easing::ease();
	case StaticStyleAnimationEasingKind::EaseIn: return gea::css::Easing::easeIn();
	case StaticStyleAnimationEasingKind::EaseOut: return gea::css::Easing::easeOut();
	case StaticStyleAnimationEasingKind::EaseInOut: return gea::css::Easing::easeInOut();
	case StaticStyleAnimationEasingKind::CubicBezier:
		return gea::css::Easing::cubicBezier(easing.x1, easing.y1, easing.x2, easing.y2);
	case StaticStyleAnimationEasingKind::Steps:
		return gea::css::Easing::steps(easing.steps > 0 ? easing.steps : 1);
	}
	return gea::css::Easing::ease();
}

std::uint16_t storeStaticAnimationSpec(const char *name,
                                       std::uint32_t durationMs,
                                       std::uint32_t delayMs,
                                       int iterations,
                                       StaticStyleAnimationDirection direction,
                                       StaticStyleAnimationFill fill,
                                       StaticStyleAnimationEasing easing)
{
	if (!name || !*name || durationMs == 0) return kNoCompiledCssAnimationSpec;
	auto &specs = compiledCssAnimationSpecs();
	if (specs.size() >= kNoCompiledCssAnimationSpec) return kNoCompiledCssAnimationSpec;
	CssAnimationSpec spec;
	spec.nameAtom = internCssAtom(name);
	spec.durationMs = durationMs;
	spec.delayMs = delayMs;
	spec.iterations = iterations;
	spec.direction = staticAnimationDirection(direction);
	spec.fill = staticAnimationFill(fill);
	spec.easing = staticAnimationEasing(easing);
	spec.valid = true;
	specs.push_back(spec);
	return static_cast<std::uint16_t>(specs.size() - 1);
}

CssRule makeStaticAnimationCssRule(StaticStyleSelectorKind selectorKind,
                                   const char *selector,
                                   const char *name,
                                   std::uint32_t durationMs,
                                   std::uint32_t delayMs,
                                   int iterations,
                                   StaticStyleAnimationDirection direction,
                                   StaticStyleAnimationFill fill,
                                   StaticStyleAnimationEasing easing,
                                   const char *media)
{
	CssRule rule = makeStaticCompiledCssRule(selectorKind,
	                                        selector,
	                                        CssRuleProperty::Animation,
	                                        CssDeclarationId::Animation,
	                                        kNoCompiledCssValue,
	                                        media);
	rule.compiledAnimationSpec = storeStaticAnimationSpec(name,
	                                                     durationMs,
	                                                     delayMs,
	                                                     iterations,
	                                                     direction,
	                                                     fill,
	                                                     easing);
	return rule;
}

std::uint16_t compileCssAnimationSpec(const CssText &rawValue)
{
	if (rawValue.empty() || rawValue.hasVarReference()) return kNoCompiledCssAnimationSpec;
	const CssAnimationSpec spec = parseAnimationShorthand(rawValue.trimmedStr());
	if (!spec.valid) return kNoCompiledCssAnimationSpec;
	auto &specs = compiledCssAnimationSpecs();
	if (specs.size() >= kNoCompiledCssAnimationSpec) return kNoCompiledCssAnimationSpec;
	specs.push_back(spec);
	return static_cast<std::uint16_t>(specs.size() - 1);
}

const CssAnimationSpec *compiledCssAnimationSpecForHandle(std::uint16_t handle)
{
	const auto &specs = compiledCssAnimationSpecs();
	return handle < specs.size() ? &specs[handle] : nullptr;
}

CssAnimationSpec animationSpecForNode(int node)
{
	CssAnimationSpec spec;
	rebuildRuleIndexIfNeeded();
	const auto &list = rules();
	for (const int ri : g_ruleIndex.animationRules) {
		const CssRule &rule = list[ri];
		if (!ruleMediaMatchesIndex(ri) || !ruleMatchesNode(rule, node)) continue;
		if (const CssAnimationSpec *compiled = compiledCssAnimationSpecForHandle(rule.compiledAnimationSpec))
			spec = *compiled;
		else
			spec = parseAnimationShorthand(cssRuleTextForHandle(rule.valueText).str());
	}
	return spec;
}

CssAnimationSpec animationSpecForNodeFromActivePlan(const ActiveRulePlan &plan)
{
	CssAnimationSpec spec;
	if (plan.cachedEntry) {
		const CachedRuleApplyBucketSpan bucket = activeRulePlanCachedRuleSpan(plan, kActiveAnimation);
		const auto &list = rules();
		for (std::size_t i = 0; i < bucket.count; ++i) {
			const std::uint16_t ri = bucket.data[i].ruleIndex;
			if (ri == kNoCachedRuleIndex || ri >= list.size()) continue;
			const CssRule &rule = list[ri];
			if (const CssAnimationSpec *compiled = compiledCssAnimationSpecForHandle(rule.compiledAnimationSpec))
				spec = *compiled;
			else
				spec = parseAnimationShorthand(cssRuleTextForHandle(rule.valueText).str());
		}
		return spec;
	}

	const auto &list = rules();
	const DenseRuleBucketSpan bucket = activeRulePlanBucketSpan(plan, kActiveAnimation);
	for (std::size_t i = 0; i < bucket.count; ++i) {
		const int ri = bucket.data[i];
		if (ri < 0 || static_cast<std::size_t>(ri) >= list.size()) continue;
		const CssRule &rule = list[ri];
		if (const CssAnimationSpec *compiled = compiledCssAnimationSpecForHandle(rule.compiledAnimationSpec))
			spec = *compiled;
		else
			spec = parseAnimationShorthand(cssRuleTextForHandle(rule.valueText).str());
	}
	return spec;
}

double currentStyleValue(const Node &node, Property property)
{
	switch (property) {
	case Property::TransformRotate: return static_cast<double>(rstyle(node.style).transform_rotate) / 10.0;
	case Property::TransformRotateX: return static_cast<double>(rstyle(node.style).transform_rotate_x) / 10.0;
	case Property::TransformRotateY: return static_cast<double>(rstyle(node.style).transform_rotate_y) / 10.0;
	case Property::TransformTranslateOuterAxes: return rstyle(node.style).transform_translate_outer_axes;
	case Property::RotateAngle: return static_cast<double>(rstyle(node.style).rotate_angle) / 10.0;
	case Property::RotateAxisX: return rstyle(node.style).rotate_axis_x;
	case Property::RotateAxisY: return rstyle(node.style).rotate_axis_y;
	case Property::RotateAxisZ: return rstyle(node.style).rotate_axis_z;
	case Property::ScaleX: return rstyle(node.style).scale_x;
	case Property::ScaleY: return rstyle(node.style).scale_y;
	case Property::ScaleZ: return rstyle(node.style).scale_z;
	case Property::RotatePresent: return rstyle(node.style).rotate_present;
	case Property::ScalePresent: return rstyle(node.style).scale_present;
	case Property::TranslatePresent: return rstyle(node.style).translate_present;
	case Property::TranslateX: return rstyle(node.style).translate_x;
	case Property::TranslateY: return rstyle(node.style).translate_y;
	case Property::TranslateZ: return rstyle(node.style).translate_z;
	case Property::TranslateXPercent: return rstyle(node.style).translate_x_percent;
	case Property::TranslateYPercent: return rstyle(node.style).translate_y_percent;
	case Property::TransformTranslateX: return rstyle(node.style).transform_translate_x;
	case Property::TransformTranslateY: return rstyle(node.style).transform_translate_y;
	case Property::TransformTranslateZ: return rstyle(node.style).transform_translate_z;
	case Property::TransformTranslateXPercent: return rstyle(node.style).transform_translate_x_percent;
	case Property::TransformTranslateYPercent: return rstyle(node.style).transform_translate_y_percent;
	case Property::TransformScaleX: return rstyle(node.style).transform_scale_x;
	case Property::TransformScaleY: return rstyle(node.style).transform_scale_y;
	case Property::TransformScaleZ: return rstyle(node.style).transform_scale_z;
	case Property::FilterBlur: return rstyle(node.style).filter_blur_radius;
	case Property::Opacity: return node.style.opacity;
	case Property::Width: return node.style.width;
	case Property::Height: return node.style.height;
	case Property::WidthPercent: return node.style.width_percent == kUnset ? 0 : node.style.width_percent;
	case Property::HeightPercent: return node.style.height_percent == kUnset ? 0 : node.style.height_percent;
	case Property::LineHeight: return node.style.line_height;
	case Property::FontWeight: return node.style.font_weight;
	case Property::Top: return node.style.pos_offsets[0] == kUnset ? 0 : node.style.pos_offsets[0];
	case Property::Right: return node.style.pos_offsets[1] == kUnset ? 0 : node.style.pos_offsets[1];
	case Property::Bottom: return node.style.pos_offsets[2] == kUnset ? 0 : node.style.pos_offsets[2];
	case Property::Left: return node.style.pos_offsets[3] == kUnset ? 0 : node.style.pos_offsets[3];
	case Property::TopPercent: return node.style.pos_offset_percent[0] == kUnset ? 0 : node.style.pos_offset_percent[0];
	case Property::RightPercent: return node.style.pos_offset_percent[1] == kUnset ? 0 : node.style.pos_offset_percent[1];
	case Property::BottomPercent: return node.style.pos_offset_percent[2] == kUnset ? 0 : node.style.pos_offset_percent[2];
	case Property::LeftPercent: return node.style.pos_offset_percent[3] == kUnset ? 0 : node.style.pos_offset_percent[3];
	case Property::BackgroundColor: return node.style.bg_color;
	case Property::Color: return node.style.text_color;
	default: return 0;
	}
}

struct CssAnimationTrack {
	Property property;
	gea::css::KeyframeList keyframes;
};

struct CssAnimationTrackList {
	static constexpr std::size_t kInlineTrackCapacity = 8;

	CssAnimationTrack inlineTracks[kInlineTrackCapacity]{};
	CssAnimationTrack *spillTracks = nullptr;
	std::size_t spillCount = 0;
	std::size_t spillCapacity = 0;
	std::uint8_t inlineCount = 0;
	bool spilled = false;

	CssAnimationTrackList() = default;
	CssAnimationTrackList(const CssAnimationTrackList &) = delete;
	CssAnimationTrackList &operator=(const CssAnimationTrackList &) = delete;

	~CssAnimationTrackList()
	{
		delete[] spillTracks;
	}

	std::size_t size() const { return spilled ? spillCount : inlineCount; }
	bool empty() const { return size() == 0; }

	CssAnimationTrack &at(std::size_t index)
	{
		return spilled ? spillTracks[index] : inlineTracks[index];
	}

	const CssAnimationTrack &at(std::size_t index) const
	{
		return spilled ? spillTracks[index] : inlineTracks[index];
	}

	CssAnimationTrack &push(Property property)
	{
		if (!spilled && inlineCount < kInlineTrackCapacity) {
			CssAnimationTrack &track = inlineTracks[inlineCount++];
			track.property = property;
			track.keyframes.clear();
			return track;
		}
		if (!spilled) {
			spillCapacity = kInlineTrackCapacity * 2;
			spillTracks = new CssAnimationTrack[spillCapacity];
			for (std::size_t i = 0; i < inlineCount; ++i)
				spillTracks[i] = inlineTracks[i];
			spillCount = inlineCount;
			spilled = true;
		}
		if (spillCount >= spillCapacity) {
			const std::size_t nextCapacity = spillCapacity ? spillCapacity * 2 : kInlineTrackCapacity * 2;
			CssAnimationTrack *next = new CssAnimationTrack[nextCapacity];
			for (std::size_t i = 0; i < spillCount; ++i)
				next[i] = spillTracks[i];
			delete[] spillTracks;
			spillTracks = next;
			spillCapacity = nextCapacity;
		}
		CssAnimationTrack &track = spillTracks[spillCount++];
		track.property = property;
		track.keyframes.clear();
		return track;
	}

	void removeShortTracks()
	{
		std::size_t write = 0;
		const std::size_t n = size();
		for (std::size_t read = 0; read < n; ++read) {
			CssAnimationTrack &track = at(read);
			if (track.keyframes.size() < 2) continue;
			if (write != read) at(write) = track;
			++write;
		}
		if (spilled)
			spillCount = write;
		else
			inlineCount = static_cast<std::uint8_t>(write);
	}
};

void addTrackKeyframe(CssAnimationTrackList &tracks, Property property, double offset, double value)
{
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		CssAnimationTrack &track = tracks.at(i);
		if (track.property != property) continue;
		track.keyframes.push_back({offset, value});
		return;
	}
	CssAnimationTrack &track = tracks.push(property);
	track.keyframes.push_back({offset, value});
}

void addTransformComponentKeyframes(CssAnimationTrackList &tracks, double offset, const TransformComponents &transform)
{
	if (transform.hasTranslateX || transform.hasTranslateY || transform.hasTranslateZ)
		addTrackKeyframe(tracks, Property::TransformTranslateOuterAxes, offset, transform.translateOuterAxes);
	if (transform.hasRotateX) addTrackKeyframe(tracks, Property::TransformRotateX, offset, transform.rotateX / 10.0);
	if (transform.hasRotateY) addTrackKeyframe(tracks, Property::TransformRotateY, offset, transform.rotateY / 10.0);
	if (transform.hasRotateZ) addTrackKeyframe(tracks, Property::TransformRotate, offset, transform.rotateZ / 10.0);
	if (transform.hasTranslateX) addTrackKeyframe(tracks, Property::TransformTranslateX, offset, transform.translateX);
	if (transform.hasTranslateY) addTrackKeyframe(tracks, Property::TransformTranslateY, offset, transform.translateY);
	if (transform.hasTranslateZ) addTrackKeyframe(tracks, Property::TransformTranslateZ, offset, transform.translateZ);
	if (transform.hasTranslateX) addTrackKeyframe(tracks, Property::TransformTranslateXPercent, offset, transform.translateXPercent);
	if (transform.hasTranslateY) addTrackKeyframe(tracks, Property::TransformTranslateYPercent, offset, transform.translateYPercent);
	if (transform.hasScaleX) addTrackKeyframe(tracks, Property::TransformScaleX, offset, transform.scaleX);
	if (transform.hasScaleY) addTrackKeyframe(tracks, Property::TransformScaleY, offset, transform.scaleY);
	if (transform.hasScaleZ) addTrackKeyframe(tracks, Property::TransformScaleZ, offset, transform.scaleZ);
}

void addTransformKeyframes(CssAnimationTrackList &tracks, int nodeId, int offsetPermille, const std::string &value)
{
	const TransformComponents transform = parseTransformComponents(value, nodeId);
	const double offset = std::max(0, std::min(1000, offsetPermille)) / 1000.0;
	addTransformComponentKeyframes(tracks, offset, transform);
}

bool addCompiledPropertyKeyframe(CssAnimationTrackList &tracks, int nodeId, int offsetPermille, const CssCompiledValue &compiled)
{
	const double offset = std::max(0, std::min(1000, offsetPermille)) / 1000.0;
	switch (compiled.kind) {
	case CssCompiledKind::Noop:
		return true;
	case CssCompiledKind::DirectProperty: {
		const Property property = static_cast<Property>(compiled.values[0]);
		switch (property) {
		case Property::RotateAngle:
			addTrackKeyframe(tracks, property, offset, compiled.values[1] / 10.0); return true;
		case Property::RotatePresent:
		case Property::RotateAxisX:
		case Property::RotateAxisY:
		case Property::RotateAxisZ:
		case Property::ScalePresent:
		case Property::ScaleX:
		case Property::ScaleY:
		case Property::ScaleZ:
		case Property::Opacity:
		case Property::Width:
		case Property::Height:
		case Property::Left:
		case Property::Top:
		case Property::BackgroundColor:
		case Property::Color:
		case Property::FilterBlur:
			addTrackKeyframe(tracks, property, offset, compiled.values[1]);
			return true;
		default:
			return false;
		}
	}
	case CssCompiledKind::Opacity:
		addTrackKeyframe(tracks, Property::Opacity, offset, compiled.values[0]);
		return true;
	case CssCompiledKind::Color:
		if (compiled.declaration == CssDeclarationId::Background || compiled.declaration == CssDeclarationId::BackgroundColor) {
			addTrackKeyframe(tracks, Property::BackgroundColor, offset, compiled.values[0]);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Color) {
			addTrackKeyframe(tracks, Property::Color, offset, compiled.values[0]);
			return true;
		}
		return false;
	case CssCompiledKind::ColorVar: {
		ResolvedCompiledCssColor color;
		const bool hasFallback = compiled.aux != 0;
		if (!resolveCompiledColorRef(nodeId,
		                             static_cast<CssAtomId>(compiled.values[0]),
		                             hasFallback ? 1 : 0,
		                             compiled.values[1],
		                             static_cast<style_color_t>(compiled.values[2]),
		                             static_cast<std::uint8_t>(hasFallback ? compiled.values[3] : 0),
		                             color))
			return false;
		if (compiled.declaration == CssDeclarationId::Background || compiled.declaration == CssDeclarationId::BackgroundColor) {
			addTrackKeyframe(tracks, Property::BackgroundColor, offset, color.styleColor);
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Color) {
			addTrackKeyframe(tracks, Property::Color, offset, color.styleColor);
			return true;
		}
		return false;
	}
	case CssCompiledKind::Length:
	case CssCompiledKind::Size:
	case CssCompiledKind::PositionOffset: {
		const CssLengthSpec &length = compiled.lengths[0];
		if (compiled.declaration == CssDeclarationId::Width) {
			addTrackKeyframe(tracks, Property::Width, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Height) {
			addTrackKeyframe(tracks, Property::Height, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Left) {
			addTrackKeyframe(tracks, Property::Left, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Horizontal));
			return true;
		}
		if (compiled.declaration == CssDeclarationId::Top) {
			addTrackKeyframe(tracks, Property::Top, offset, resolveCompiledLengthForNode(length, nodeId, LengthAxis::Vertical));
			return true;
		}
		return false;
	}
	case CssCompiledKind::Transform:
		if (compiled.declaration == CssDeclarationId::Translate) {
			const auto t = transformFromCompiled(compiled, nodeId);
			addTrackKeyframe(tracks, Property::TranslatePresent, offset, t.hasTranslateX || t.hasTranslateY || t.hasTranslateZ);
			addTrackKeyframe(tracks, Property::TranslateX, offset, t.translateX);
			addTrackKeyframe(tracks, Property::TranslateY, offset, t.translateY);
			addTrackKeyframe(tracks, Property::TranslateZ, offset, t.translateZ);
			addTrackKeyframe(tracks, Property::TranslateXPercent, offset, t.translateXPercent);
			addTrackKeyframe(tracks, Property::TranslateYPercent, offset, t.translateYPercent);
			return true;
		}
		addTransformComponentKeyframes(tracks, offset, transformFromCompiled(compiled, nodeId));
		return true;
	case CssCompiledKind::Rotate:
		if (compiled.declaration != CssDeclarationId::Rotate) return false;
		addTrackKeyframe(tracks, Property::RotatePresent, offset, compiled.aux);
		addTrackKeyframe(tracks, Property::RotateAngle, offset, compiled.values[0] / 10.0);
		addTrackKeyframe(tracks, Property::RotateAxisX, offset, compiled.values[1]);
		addTrackKeyframe(tracks, Property::RotateAxisY, offset, compiled.values[2]);
		addTrackKeyframe(tracks, Property::RotateAxisZ, offset, compiled.values[3]);
		return true;
	case CssCompiledKind::Scale:
		if (compiled.declaration != CssDeclarationId::Scale) return false;
		addTrackKeyframe(tracks, Property::ScalePresent, offset, compiled.aux);
		addTrackKeyframe(tracks, Property::ScaleX, offset, compiled.values[0]);
		addTrackKeyframe(tracks, Property::ScaleY, offset, compiled.values[1]);
		addTrackKeyframe(tracks, Property::ScaleZ, offset, compiled.values[2]);
		return true;
	case CssCompiledKind::FilterBlur: {
		int radius = compiled.aux == 0 ? 0 : resolveCompiledLengthForNode(compiled.lengths[0], nodeId, LengthAxis::None);
		if (radius < 0) radius = 0;
		if (radius > 64) radius = 64;
		addTrackKeyframe(tracks, Property::FilterBlur, offset, radius);
		return true;
	}
	default:
		return false;
	}
}

void addPropertyKeyframe(CssAnimationTrackList &tracks, int nodeId, int offsetPermille, CssDeclarationId declaration, const std::string &value)
{
	const double offset = std::max(0, std::min(1000, offsetPermille)) / 1000.0;
	if (declaration == CssDeclarationId::Translate) {
		std::string function;
		if (!individualTranslateFunction(value, function)) return;
		const auto t = parseTransformComponents(function, nodeId);
		addTrackKeyframe(tracks, Property::TranslatePresent, offset, t.hasTranslateX || t.hasTranslateY || t.hasTranslateZ);
		addTrackKeyframe(tracks, Property::TranslateX, offset, t.translateX);
		addTrackKeyframe(tracks, Property::TranslateY, offset, t.translateY);
		addTrackKeyframe(tracks, Property::TranslateZ, offset, t.translateZ);
		addTrackKeyframe(tracks, Property::TranslateXPercent, offset, t.translateXPercent);
		addTrackKeyframe(tracks, Property::TranslateYPercent, offset, t.translateYPercent);
		return;
	}
	if (declaration == CssDeclarationId::Transform) {
		addTransformKeyframes(tracks, nodeId, offsetPermille, value);
		return;
	}
	if (declaration == CssDeclarationId::Opacity) addTrackKeyframe(tracks, Property::Opacity, offset, parseOpacity(value));
	else if (declaration == CssDeclarationId::Rotate) {
		IndividualRotation r;
		if (!parseIndividualRotation(value, r)) return;
		addTrackKeyframe(tracks, Property::RotatePresent, offset, toLowerAscii(trimCssValue(value)) != "none");
		addTrackKeyframe(tracks, Property::RotateAngle, offset, r.angle / 10.0);
		addTrackKeyframe(tracks, Property::RotateAxisX, offset, r.x);
		addTrackKeyframe(tracks, Property::RotateAxisY, offset, r.y);
		addTrackKeyframe(tracks, Property::RotateAxisZ, offset, r.z);
	}
	else if (declaration == CssDeclarationId::Scale) {
		int scale[3]; if (!parseIndividualScale(value, scale)) return;
		addTrackKeyframe(tracks, Property::ScalePresent, offset, toLowerAscii(trimCssValue(value)) != "none");
		addTrackKeyframe(tracks, Property::ScaleX, offset, scale[0]);
		addTrackKeyframe(tracks, Property::ScaleY, offset, scale[1]);
		addTrackKeyframe(tracks, Property::ScaleZ, offset, scale[2]);
	}
	else if (declaration == CssDeclarationId::Width) addTrackKeyframe(tracks, Property::Width, offset, parseLengthForNode(value, nodeId, LengthAxis::Horizontal));
	else if (declaration == CssDeclarationId::Height) addTrackKeyframe(tracks, Property::Height, offset, parseLengthForNode(value, nodeId, LengthAxis::Vertical));
	else if (declaration == CssDeclarationId::Left) addTrackKeyframe(tracks, Property::Left, offset, parseLengthForNode(value, nodeId, LengthAxis::Horizontal));
	else if (declaration == CssDeclarationId::Top) addTrackKeyframe(tracks, Property::Top, offset, parseLengthForNode(value, nodeId, LengthAxis::Vertical));
	else if (declaration == CssDeclarationId::Background || declaration == CssDeclarationId::BackgroundColor)
		addTrackKeyframe(tracks, Property::BackgroundColor, offset, parseColorStyleValue(value));
	else if (declaration == CssDeclarationId::Color)
		addTrackKeyframe(tracks, Property::Color, offset, parseColorStyleValue(value));
	else if (declaration == CssDeclarationId::Filter)
		addTrackKeyframe(tracks, Property::FilterBlur, offset, parseFilterBlurRadius(value, nodeId));
}

void sortTrackKeyframesByOffset(gea::css::KeyframeList &keyframes)
{
	for (std::size_t i = 1; i < keyframes.size(); ++i) {
		const gea::css::Keyframe keyframe = keyframes[i];
		std::size_t j = i;
		while (j > 0 && keyframe.offset < keyframes[j - 1].offset) {
			keyframes[j] = keyframes[j - 1];
			--j;
		}
		keyframes[j] = keyframe;
	}
}

void normalizeTrackEndpoints(CssAnimationTrack &track, const Node &node)
{
	sortTrackKeyframesByOffset(track.keyframes);
	if (track.keyframes.empty()) return;
	const double current = currentStyleValue(node, track.property);
	const bool needsStart = track.keyframes.front().offset > 0.0;
	const bool needsEnd = track.keyframes.back().offset < 1.0;
	if (needsStart) track.keyframes.push_back({0.0, current});
	if (needsEnd) track.keyframes.push_back({1.0, current});
	if (needsStart) sortTrackKeyframesByOffset(track.keyframes);
}

void buildAnimationTracksForNode(int nodeId, const CssAnimationSpec &spec, CssAnimationTrackList &tracks)
{
	if (!spec.valid) return;
	auto &state = treeState();
	if (nodeId < 0 || nodeId >= state.nodeCount) return;

	const DenseRuleBucketSpan indices = keyframeRuleIndicesForName(spec.nameAtom);
	if (indices.empty()) return;
	const auto &rules = keyframeRules();
	for (std::size_t i = 0; i < indices.count; ++i) {
		const int ri = indices.data[i];
		if (ri < 0 || ri >= static_cast<int>(rules.size())) continue;
		const auto &rule = rules[ri];
		if (const CssCompiledValue *compiled = compiledCssValueForHandle(rule.compiledValue)) {
			if (addCompiledPropertyKeyframe(tracks, nodeId, rule.offsetPermille, *compiled)) continue;
		}
		addPropertyKeyframe(tracks,
		                    nodeId,
		                    rule.offsetPermille,
		                    rule.declaration,
		                    cssRuleTextForHandle(rule.valueText).str());
	}
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		auto &track = tracks.at(i);
		normalizeTrackEndpoints(track, state.nodes[nodeId]);
		if (track.property == Property::TranslatePresent || track.property == Property::RotatePresent || track.property == Property::ScalePresent) {
			// `none` interpolates as identity whenever another endpoint has a
			// translation. The entire active interval then establishes a context.
			bool present = false;
			for (std::size_t k = 0; k < track.keyframes.size(); ++k) present |= track.keyframes[k].value != 0;
			if (present) for (std::size_t k = 0; k < track.keyframes.size(); ++k) track.keyframes[k].value = 1;
		}
	}
	tracks.removeShortTracks();
}

bool isRotationAxisTrack(Property property)
{
	return property == Property::RotateAxisX || property == Property::RotateAxisY || property == Property::RotateAxisZ;
}

gea::css::Animation animationFromTrack(int nodeId, const CssAnimationSpec &spec, const CssAnimationTrack &track, const CssAnimationTrackList &tracks)
{
	gea::css::Animation animation;
	animation.nodeId = nodeId;
	animation.property = track.property;
	animation.kind = gea::css::kindOf(track.property);
	animation.keyframes = track.keyframes;
	if (track.property == Property::RotateAngle) {
		for (std::size_t k = 0; k < track.keyframes.size(); ++k) {
			gea::css::RotationAxis axis;
			for (std::size_t i = 0; i < tracks.size(); ++i) {
				const auto &component = tracks.at(i);
				if (!isRotationAxisTrack(component.property)) continue;
				for (std::size_t j = 0; j < component.keyframes.size(); ++j) {
					if (component.keyframes[j].offset != track.keyframes[k].offset) continue;
					const double value = component.keyframes[j].value / 1000000.0;
					if (component.property == Property::RotateAxisX) axis.x = value;
					else if (component.property == Property::RotateAxisY) axis.y = value;
					else axis.z = value;
				}
			}
			animation.rotationAxes.push_back(axis);
		}
	}
	animation.easing = spec.easing;
	animation.durationMs = spec.durationMs;
	animation.delayMs = spec.delayMs;
	animation.iterations = spec.iterations;
	animation.direction = spec.direction;
	animation.fill = spec.fill;
	return animation;
}

void applyPrimedAnimationValue(const gea::css::Animation &animation, double value)
{
	NodeHandle node(animation.nodeId);
	if (!node) return;
	switch (animation.kind) {
	case gea::css::ValueKind::Angle:
		setStyleValue(node, animation.property, static_cast<int>(std::llround(value * 10.0)), StyleApplicationSource::ClassRule);
		return;
	case gea::css::ValueKind::Color: {
		const int rgb565 = static_cast<int>(std::llround(value));
		setStyleValue(node, animation.property, rgb565, StyleApplicationSource::ClassRule);
		if (animation.property == Property::BackgroundColor)
			setStyleValue(node, Property::HasBackground, 1, StyleApplicationSource::ClassRule);
		else if (animation.property == Property::ActiveBackgroundColor)
			setStyleValue(node, Property::HasActiveBackground, 1, StyleApplicationSource::ClassRule);
		return;
	}
	case gea::css::ValueKind::Scalar:
		setStyleValue(node, animation.property, static_cast<int>(std::llround(value)), StyleApplicationSource::ClassRule);
		return;
	}
}

void primeCssAnimationsForNode(int node, const ActiveRulePlan *activePlan)
{
	if (activePlan) {
		if (!activePlan->has(kActiveAnimation)) return;
	} else {
		rebuildRuleIndexIfNeeded();
		if (g_ruleIndex.animationRules.empty()) return;
	}
	const CssAnimationSpec spec = activePlan ? animationSpecForNodeFromActivePlan(*activePlan) : animationSpecForNode(node);
	CssAnimationTrackList tracks;
	buildAnimationTracksForNode(node, spec, tracks);
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		const CssAnimationTrack &track = tracks.at(i);
		if (isRotationAxisTrack(track.property)) continue;
		const gea::css::Animation animation = animationFromTrack(node, spec, track, tracks);
		const gea::css::Progress progress = gea::css::computeProgress(animation, 0.0);
		if (!progress.active) continue;
		if (!animation.rotationAxes.empty())
			gea::css::applyRotationSample(animation, progress.p, [&](Property property, int value) {
				setStyleValue(NodeHandle(node), property, value, StyleApplicationSource::ClassRule);
			});
		else applyPrimedAnimationValue(animation, gea::css::sampleTrack(animation, progress.p));
	}
}

void startAnimationForNode(int nodeId, const CssAnimationSpec &spec, std::uint32_t nowMs)
{
	CssAnimationTrackList tracks;
	buildAnimationTracksForNode(nodeId, spec, tracks);
	for (std::size_t i = 0, n = tracks.size(); i < n; ++i) {
		const CssAnimationTrack &track = tracks.at(i);
		if (isRotationAxisTrack(track.property)) continue;
		gea::css::Animation animation = animationFromTrack(nodeId, spec, track, tracks);
		gea::css::AnimationEngine::instance().start(std::move(animation), nowMs);
	}
}

}  // namespace

bool StyleValues::hasTextBackgroundClip(const ComputedStyle &style)
{
	const auto handle = rstyle(style).bg_clip;
	const auto &lists = backgroundClipLists();
	if (!handle || handle > lists.size()) return false;
	const auto &list = lists[handle - 1];
	return std::find(list.begin(), list.end(), 3) != list.end();
}

int StyleValues::backgroundClip(const ComputedStyle &style, int layer)
{
	const auto handle = rstyle(style).bg_clip;
	const auto &lists = backgroundClipLists();
	if (!handle || handle > lists.size()) return 0;
	const auto &list = lists[handle - 1];
	return list[std::max(0, layer) % list.size()];
}

BackgroundPlacement StyleValues::backgroundPlacement(const ComputedStyle &style, int nodeId, int layer,
                                                     int x, int y, int width, int height)
{
	const auto &r = rstyle(style);
	auto entry = [&](int handle) -> const CssBackgroundPair * {
		const auto &lists = backgroundPlacementLists();
		if (handle < 0 || handle >= static_cast<int>(lists.size()) || lists[handle].empty()) return nullptr;
		return &lists[handle][std::max(0, layer) % lists[handle].size()];
	};
	BackgroundPlacement out{x, y, width, height};
	if (const auto *v = entry(r.bg_attachment_list)) out.attachment = v->a;
	if (const auto *v = entry(r.bg_origin_list)) out.origin = v->a;
	if (const auto *v = entry(r.bg_repeat_list)) { out.repeatX = v->a; out.repeatY = v->b; }
	const Node &node = treeState().nodes[nodeId];
	if (out.attachment == 1) {
		x = y = 0; width = treeState().mountedWidth; height = treeState().mountedHeight;
	} else {
		if (out.attachment == 2) {
			width = std::max<int>(width, node.layout.scroll_content_width);
			height = std::max<int>(height, node.layout.scroll_content_height);
			x -= node.layout.scroll_x; y -= node.layout.scroll_y;
		}
		if (out.origin != 0) {
			int inset[4];
			for (int i = 0; i < 4; ++i) inset[i] = std::max<int>(node.style.border_width, rstyle(node.style).border_side_width[i]) +
			    (out.origin == 2 ? std::max<int>(0, node.style.padding[i]) : 0);
			x += inset[3]; y += inset[0]; width -= inset[1]+inset[3]; height -= inset[0]+inset[2];
		}
	}
	width = std::max(0, width); height = std::max(0, height);
	out.areaX = x; out.areaY = y; out.areaWidth = width; out.areaHeight = height;
	auto length = [&](const CssLengthSpec &spec, int basis, int automatic) {
		const auto value = resolveCompiledLengthForNodeDetailed(spec, nodeId, LengthAxis::None);
		return value.isAuto ? automatic : roundToInt(value.isPercent ? value.value*basis/1000.0 : value.value);
	};
	out.width = width; out.height = height;
	if (const auto *v = entry(r.bg_size_list)) {
		out.width = std::max(0, length(v->x, width, width));
		out.height = std::max(0, length(v->y, height, height));
	}
	if (out.repeatX == 2 && out.width > 0) out.width = std::max(1, width / std::max(1, roundToInt(static_cast<double>(width)/out.width)));
	if (out.repeatY == 2 && out.height > 0) out.height = std::max(1, height / std::max(1, roundToInt(static_cast<double>(height)/out.height)));
	out.x = x; out.y = y;
	if (const auto *v = entry(r.bg_position_list)) {
		out.x += length(v->x, width-out.width, 0);
		out.y += length(v->y, height-out.height, 0);
	}
	return out;
}

bool StyleValues::applyBackgroundImage(ComputedStyle &style, int handle, int nodeId)
{
	return applyBackgroundImageToStyle(style, handle, nodeId);
}


void beginStyleMountBatch()
{
	g_styleMountBatchActive = true;
}

bool styleMountBatchActive()
{
	return g_styleMountBatchActive;
}

void endStyleMountBatch()
{
	if (!g_styleMountBatchActive) return;
	g_styleMountBatchActive = false;
	if (g_pendingRecomputeRoots.empty()) {
		g_forceFullSubtreeMark.clear();
		return;
	}

	g_pendingRecomputeMark.clear();
	std::vector<int> roots;
	auto &state = treeState();
	roots.reserve(g_pendingRecomputeRoots.size());
	for (const int node : g_pendingRecomputeRoots) {
		if (node < 0 || node >= state.nodeCount) continue;
		if (g_pendingRecomputeMark.contains(node)) continue;
		g_pendingRecomputeMark.insert(node);
		roots.push_back(node);
	}
	g_pendingRecomputeRoots.clear();
	if (roots.empty()) {
		g_forceFullSubtreeMark.clear();
		return;
	}

#if GEA_RECPROF
	g_profResetUs = g_profCandUs = g_profApplyUs = g_profVarUs = g_profMiscUs = g_profPseudoUs = g_profBodyUs = 0;
	g_profBgUs = g_profXformUs = g_profLenUs = g_profColorUs = 0;
	g_profNodes = g_profApplyCalls = g_profBgCalls = g_profXformCalls = g_profLenCalls = g_profColorCalls = 0;
	const int64_t _batchStart = recNow();
#endif
	// The top-most pending roots: a root whose ancestor is also pending is covered by
	// that ancestor's subtree recompute (and for a mount where every node is pending,
	// recomputing each separately would reintroduce the O(nodes^2) blow-up).
	std::vector<int> topRoots;
	topRoots.reserve(roots.size());
	for (const int node : roots) {
		if (node < 0 || node >= state.nodeCount) continue;
		bool covered = false;
		for (int ancestor = state.nodes[node].parent; ancestor >= 0; ancestor = state.nodes[ancestor].parent) {
			if (g_pendingRecomputeMark.contains(ancestor)) {
				covered = true;
				break;
			}
		}
		if (!covered) topRoots.push_back(node);
	}

	// Incremental recompute: a near-root class change (e.g. a theme switch) only
	// re-styles nodes that actually depend on what changed; the rest keep their
	// already-correct computed style. Equivalent to recomputeSubtreeClassStyles but
	// skips the per-node parse/dispatch cost for unaffected nodes.
#if GEA_INCREMENTAL_VERIFY
	g_incrRecomputedNodes.clear();
#endif
	for (const int node : topRoots) recomputeSubtreeIncremental(node, g_pendingRecomputeMark);

#if GEA_INCREMENTAL_VERIFY
	// Prove equivalence on-device: snapshot the incremental result, then run the full
	// recompute and assert every node's computed style is byte-identical.
	std::vector<int> vIds;
	std::vector<ComputedStyle> vStyles;
	std::vector<int> vImg;
	for (const int node : topRoots) collectSubtreeStyles(node, vIds, vStyles, vImg);
	for (const int node : topRoots) recomputeSubtreeClassStyles(node);
	int mismatches = 0;
	int logged = 0;
	for (std::size_t i = 0; i < vIds.size(); ++i) {
		const int id = vIds[i];
		// Field-wise compare (ComputedStyle has padding, so memcmp gives false diffs).
		// styleEqualExceptTextPaint covers every field but text_color, which we add.
		const bool equal = styleEqualExceptTextPaint(vStyles[i], state.nodes[id].style) &&
		                   vStyles[i].text_color == state.nodes[id].style.text_color &&
		                   vImg[i] == state.nodes[id].image_id;
		if (!equal) {
			mismatches++;
			if (logged < 8) {
				const bool recomputed = g_incrRecomputedNodes.count(id) > 0;
				const std::string refs = (id >= 0 && id < kMaxNodes) ? g_nodeRefs[id].debugString() : std::string();
				GEA_STYLE_LOGW("gea.recincr", "  mismatch node=%d tag=%s class=[%s] incrRecomputed=%d refs=[%s]", id,
				               tagFromId(state.nodes[id].tag_id), state.classLists[id].value().c_str(), recomputed ? 1 : 0,
				               refs.c_str());
				logged++;
			}
		}
	}
	if (mismatches > 0)
		GEA_STYLE_LOGW("gea.recincr", "VERIFY MISMATCH: %d/%d nodes differ", mismatches, static_cast<int>(vIds.size()));
	else
		GEA_STYLE_LOGW("gea.recincr", "VERIFY OK: %d nodes identical", static_cast<int>(vIds.size()));
#endif
#if GEA_RECPROF
	const int64_t _batchUs = recNow() - _batchStart;
	if (_batchUs > 50000) {
		std::snprintf(g_recprofLast, sizeof g_recprofLast,
			"recompute=%lldus nodes=%d applyCalls=%d | reset=%lldus cand=%lldus apply=%lldus(var=%lldus filter=%lldus body=%lldus[bg=%lldus/%d xform=%lldus/%d len=%lldus/%d color=%lldus/%d simple=%lldus]) misc=%lldus pseudo=%lldus",
			static_cast<long long>(_batchUs), g_profNodes, g_profApplyCalls,
			static_cast<long long>(g_profResetUs), static_cast<long long>(g_profCandUs),
			static_cast<long long>(g_profApplyUs), static_cast<long long>(g_profVarUs),
			static_cast<long long>(g_profApplyUs - g_profVarUs - g_profBodyUs),
			static_cast<long long>(g_profBodyUs),
			static_cast<long long>(g_profBgUs), g_profBgCalls,
			static_cast<long long>(g_profXformUs), g_profXformCalls,
			static_cast<long long>(g_profLenUs), g_profLenCalls,
			static_cast<long long>(g_profColorUs), g_profColorCalls,
			static_cast<long long>(g_profBodyUs - g_profBgUs - g_profXformUs - g_profLenUs - g_profColorUs),
			static_cast<long long>(g_profMiscUs), static_cast<long long>(g_profPseudoUs));
		GEA_STYLE_LOGW("gea.recprof", "%s", g_recprofLast);
	}
#endif
	g_forceFullSubtreeMark.clear();
}

// Query hook for the last recompute-batch profile line (GEA_RECPROF builds).
extern "C" const char *gea_recprof_last()
{
#if GEA_RECPROF
	return g_recprofLast[0] ? g_recprofLast : nullptr;
#else
	return nullptr;
#endif
}

StyleSheet &StyleSheet::instance()
{
	static StyleSheet sheet;
	return sheet;
}

void Style::rotateDegrees(double value) const
{
	set(Property::TransformRotate, numericRotateTenths(value));
}

void Style::scale(double value) const
{
	const int scale = numericScalePermille(value);
	set(Property::TransformScaleX, scale);
	set(Property::TransformScaleY, scale);
}

void Style::cssRotateDegrees(double value) const
{
	if (!std::isfinite(value)) return;
	set(Property::RotatePresent, 1);
	set(Property::RotateAngle, numericRotateTenths(value));
	set(Property::RotateAxisX, 0); set(Property::RotateAxisY, 0); set(Property::RotateAxisZ, 1000000);
}

void Style::cssScale(double value) const
{
	if (!std::isfinite(value)) return;
	const int scale = roundToInt(std::clamp(value * 1000, -32768.0, 32767.0));
	set(Property::ScalePresent, 1);
	set(Property::ScaleX, scale); set(Property::ScaleY, scale); set(Property::ScaleZ, 1000);
}

void StyleSheet::clear()
{
	rules().clear();
	keyframeRules().clear();
	g_ruleRegistrationBatchDepth = 0;
	g_ruleRegistrationRulesChanged = false;
	g_ruleRegistrationKeyframesChanged = false;
	clearKeyframeRuleIndex();
	clearCssRuleTexts();
	compiledCssValues().clear();
	clearInlineCompiledStyleCache();
	compiledCssAnimationSpecs().clear();
	clearCompiledCssBackgrounds();
	clearCompiledCssGridTemplates();
	clearCompiledCssLengthExpressions();
	clearCompiledCssLengthCache();
	clearCompiledCssColorCache();
	clearMediaConditionPlans();
	gea::css::AnimationEngine::instance().clear();
	invalidateRuleIndex();
	clearSelectorPartsCache();
	g_nodeRefOverflow.clear();
	clearCustomPropertyLookupCache();
	for (auto &refs : g_nodeRefs) refs.reset();  // stale per-node custom-prop dependencies from the previous app
	g_pendingRecomputeMark.clear();
	g_forceFullSubtreeMark.clear();
	recomputeAllClassStyles();
}

void StyleSheet::beginRuleRegistrationBatch()
{
	if (g_ruleRegistrationBatchDepth < 0x7FFF) ++g_ruleRegistrationBatchDepth;
}

void StyleSheet::endRuleRegistrationBatch()
{
	if (g_ruleRegistrationBatchDepth <= 0) return;
	--g_ruleRegistrationBatchDepth;
	if (g_ruleRegistrationBatchDepth == 0) flushRuleRegistrationBatch();
}

void StyleSheet::registerRule(const std::string &className, const std::string &property, const std::string &value, const std::string &media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Class,
	                              CssRule::PseudoElement::None,
	                              CssText::view(className),
	                              CssText::copy(property),
	                              CssText::copy(value),
	                              CssText::copy(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerElementRule(const std::string &elementName, const std::string &property, const std::string &value, const std::string &media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Element,
	                              CssRule::PseudoElement::None,
	                              CssText::view(elementName),
	                              CssText::copy(property),
	                              CssText::copy(value),
	                              CssText::copy(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerSelectorRule(const std::string &selector, const std::string &property, const std::string &value, const std::string &media)
{
	const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(selector.c_str(), selector.size());
	rules().push_back(makeCssRule(CssRule::SelectorType::Selector,
	                              selectorSlice.pseudo,
	                              CssText::view(selectorSlice.data, selectorSlice.length),
	                              CssText::copy(property),
	                              CssText::copy(value),
	                              CssText::copy(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerUserAgentElementRule(const std::string &elementName, const std::string &property, const std::string &value)
{
	CssRule rule = makeCssRule(CssRule::SelectorType::Element, CssRule::PseudoElement::None,
	    CssText::view(elementName), CssText::copy(property), CssText::copy(value), CssText::copy(std::string()));
	rule.userAgent = true;
	rules().push_back(rule);
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerKeyframeRule(const std::string &name, int offsetPermille, const std::string &property, const std::string &value)
{
	keyframeRules().push_back(makeCssKeyframeRule(CssText::view(name), offsetPermille, CssText::view(property), CssText::copy(value)));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticRule(const char *className, const char *property, const char *value, const char *media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Class,
	                              CssRule::PseudoElement::None,
	                              CssText::literal(className),
	                              CssText::literal(property),
	                              CssText::literal(value),
	                              CssText::literal(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticElementRule(const char *elementName, const char *property, const char *value, const char *media)
{
	rules().push_back(makeCssRule(CssRule::SelectorType::Element,
	                              CssRule::PseudoElement::None,
	                              CssText::literal(elementName),
	                              CssText::literal(property),
	                              CssText::literal(value),
	                              CssText::literal(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticSelectorRule(const char *selector, const char *property, const char *value, const char *media)
{
	const char *selectorText = selector ? selector : "";
	const SelectorTextSlice selectorSlice = selectorTextWithoutPseudo(selectorText, std::strlen(selectorText));
	rules().push_back(makeCssRule(CssRule::SelectorType::Selector,
	                              selectorSlice.pseudo,
	                              CssText::view(selectorSlice.data, selectorSlice.length),
	                              CssText::literal(property),
	                              CssText::literal(value),
	                              CssText::literal(media)));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticKeyframeRule(const char *name, int offsetPermille, const char *property, const char *value)
{
	keyframeRules().push_back(makeCssKeyframeRule(CssText::literal(name), offsetPermille, CssText::literal(property), CssText::literal(value)));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticPropertyKeyframeRule(const char *name, int offsetPermille, Property property, int value)
{
	keyframeRules().push_back(makeStaticPropertyKeyframeRule(name, offsetPermille, property, value));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticColorKeyframeRule(const char *name,
                                                 int offsetPermille,
                                                 StaticStyleColorProperty property,
                                                 int r,
                                                 int g,
                                                 int b,
                                                 int a)
{
	keyframeRules().push_back(makeStaticColorKeyframeRule(name, offsetPermille, property, r, g, b, a));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticColorVarKeyframeRule(const char *name,
                                                    int offsetPermille,
                                                    StaticStyleColorProperty property,
                                                    const char *varName,
                                                    bool hasFallback,
                                                    int r,
                                                    int g,
                                                    int b,
                                                    int a)
{
	keyframeRules().push_back(makeStaticColorVarKeyframeRule(name,
	                                                        offsetPermille,
	                                                        property,
	                                                        varName,
	                                                        hasFallback,
	                                                        r,
	                                                        g,
	                                                        b,
	                                                        a));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticLengthKeyframeRule(const char *name,
                                                  int offsetPermille,
                                                  StaticStyleLengthProperty property,
                                                  StaticStyleLengthUnit unit,
                                                  float value)
{
	keyframeRules().push_back(makeStaticLengthKeyframeRule(name, offsetPermille, property, StaticStyleLengthSpec{unit, value}));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticLengthSpecKeyframeRule(const char *name,
                                                      int offsetPermille,
                                                      StaticStyleLengthProperty property,
                                                      StaticStyleLengthSpec length)
{
	keyframeRules().push_back(makeStaticLengthKeyframeRule(name, offsetPermille, property, length));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticFilterBlurKeyframeRule(const char *name,
                                                      int offsetPermille,
                                                      StaticStyleLengthSpec radius)
{
	keyframeRules().push_back(makeStaticFilterBlurKeyframeRule(name, offsetPermille, radius));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticTransformKeyframeRule(const char *name,
                                                     int offsetPermille,
                                                     std::uint16_t flags,
                                                     int rotateX,
                                                     int rotateY,
                                                     int rotateZ,
                                                     StaticStyleLengthSpec translateX,
                                                     StaticStyleLengthSpec translateY,
                                                     StaticStyleLengthSpec translateZ,
                                                     int scaleX,
                                                     int scaleY,
                                                     int scaleZ)
{
	keyframeRules().push_back(makeStaticTransformKeyframeRule(name,
	                                                        offsetPermille,
	                                                        flags,
	                                                        rotateX,
	                                                        rotateY,
	                                                        rotateZ,
	                                                        translateX,
	                                                        translateY,
	                                                        translateZ,
	                                                        scaleX,
	                                                        scaleY,
	                                                        scaleZ));
	noteKeyframeRuleRegistrationChanged();
}

void StyleSheet::registerStaticPropertyRule(StaticStyleSelectorKind selectorKind,
                                            const char *selector,
                                            Property property,
                                            int value,
                                            const char *media)
{
	rules().push_back(makeDirectPropertyCssRule(selectorKind, selector, property, value, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticPropertyGroupRule(StaticStyleSelectorKind selectorKind,
                                                 const char *selector,
                                                 std::initializer_list<StaticStylePropertyValue> properties,
                                                 const char *media)
{
	rules().push_back(makeDirectPropertyGroupCssRule(selectorKind, selector, properties, media));
	noteStyleRuleRegistrationChanged();
}

std::uint16_t StyleSheet::registerStaticSelectorPlan(const char *selector,
                                                     std::initializer_list<StaticStyleSelectorPartSpec> parts)
{
	return storeStaticSelectorPlan(selector, parts);
}

std::uint16_t StyleSheet::registerStaticMediaConditionPlan(const char *condition,
                                                           std::initializer_list<StaticStyleMediaQuerySpec> queries)
{
	return storeStaticMediaConditionPlan(condition, queries);
}

void StyleSheet::registerStaticColorRule(StaticStyleSelectorKind selectorKind,
                                         const char *selector,
                                         StaticStyleColorProperty property,
                                         int r,
                                         int g,
                                         int b,
                                         int a,
                                         const char *media)
{
	rules().push_back(makeStaticColorCssRule(selectorKind, selector, property, r, g, b, a, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticColorVarRule(StaticStyleSelectorKind selectorKind,
                                            const char *selector,
                                            StaticStyleColorProperty property,
                                            const char *name,
                                            bool hasFallback,
                                            int r,
                                            int g,
                                            int b,
                                            int a,
                                            const char *media)
{
	rules().push_back(makeStaticColorVarCssRule(selectorKind,
	                                           selector,
	                                           property,
	                                           name,
	                                           hasFallback,
	                                           r,
	                                           g,
	                                           b,
	                                           a,
	                                           media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticLengthRule(StaticStyleSelectorKind selectorKind,
                                          const char *selector,
                                          StaticStyleLengthProperty property,
                                          StaticStyleLengthUnit unit,
                                          float value,
                                          const char *media)
{
	rules().push_back(makeStaticLengthCssRule(selectorKind, selector, property, unit, value, media));
	noteStyleRuleRegistrationChanged();
}

std::uint16_t StyleSheet::registerStaticLengthExpression(StaticStyleLengthExpressionKind kind,
                                                         StaticStyleLengthSpec a,
                                                         StaticStyleLengthSpec b,
                                                         StaticStyleLengthSpec c,
                                                         float scalar,
                                                         const char *name,
                                                         bool hasFallback)
{
	CssLengthExpression expression;
	expression.kind = cssLengthExpressionKindForStatic(kind);
	expression.a = cssLengthSpecForStatic(a);
	expression.b = cssLengthSpecForStatic(b);
	expression.c = cssLengthSpecForStatic(c);
	expression.scalar = scalar;
	if (kind == StaticStyleLengthExpressionKind::Var) {
		expression.nameAtom = internCssAtom(name ? name : "");
		expression.hasFallback = hasFallback ? 1 : 0;
	}
	return storeCompiledCssLengthExpression(expression);
}

void StyleSheet::registerStaticLengthSpecRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLengthProperty property,
                                              StaticStyleLengthSpec length,
                                              const char *media)
{
	rules().push_back(makeStaticLengthSpecCssRule(selectorKind, selector, property, length, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticFontFamilyRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              const char *family,
                                              const char *media)
{
	rules().push_back(makeStaticFontFamilyCssRule(selectorKind, selector, family, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticLineHeightRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLineHeightKind kind,
                                              StaticStyleLengthSpec value,
                                              const char *media)
{
	rules().push_back(makeStaticLineHeightCssRule(selectorKind, selector, kind, value, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticFlexRule(StaticStyleSelectorKind selectorKind,
                                        const char *selector,
                                        int grow,
                                        StaticStyleLengthSpec basis,
                                        bool hasBasis,
                                        const char *media)
{
	rules().push_back(makeStaticFlexCssRule(selectorKind, selector, grow, basis, hasBasis, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBorderRule(StaticStyleSelectorKind selectorKind,
                                          const char *selector,
                                          StaticStyleLengthSpec width,
                                          int r,
                                          int g,
                                          int b,
                                          int a,
                                          const char *media)
{
	rules().push_back(makeStaticBorderCssRule(selectorKind, selector, width, r, g, b, a, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBorderRadiusRule(StaticStyleSelectorKind selectorKind,
                                                const char *selector,
                                                StaticStyleLengthSpec topLeft,
                                                StaticStyleLengthSpec topRight,
                                                StaticStyleLengthSpec bottomRight,
                                                StaticStyleLengthSpec bottomLeft,
                                                const char *media)
{
	rules().push_back(makeStaticBorderRadiusCssRule(selectorKind, selector, topLeft, topRight, bottomRight, bottomLeft, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBorderRadiusCornerRule(StaticStyleSelectorKind selectorKind,
                                                      const char *selector,
                                                      StaticStyleBorderRadiusCorner corner,
                                                      StaticStyleLengthSpec radius,
                                                      const char *media)
{
	rules().push_back(makeStaticBorderRadiusCornerCssRule(selectorKind, selector, corner, radius, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticFilterBlurRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLengthSpec radius,
                                              const char *media)
{
	rules().push_back(makeStaticFilterBlurCssRule(selectorKind, selector, radius, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBoxShadowNoneRule(StaticStyleSelectorKind selectorKind,
                                                 const char *selector,
                                                 const char *media)
{
	rules().push_back(makeStaticBoxShadowNoneCssRule(selectorKind, selector, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBackgroundRule(StaticStyleSelectorKind selectorKind,
                                              const char *selector,
                                              StaticStyleLinearGradient gradient,
                                              StaticStyleLinearGradient overlayGradient,
                                              bool hasOverlayGradient,
                                              StaticStyleBackgroundGridLine gridX,
                                              StaticStyleBackgroundGridLine gridY,
                                              const char *media, bool imageOnly)
{
	rules().push_back(makeStaticBackgroundCssRule(selectorKind,
	                                             selector,
	                                             gradient,
	                                             overlayGradient,
	                                             hasOverlayGradient,
	                                             gridX,
	                                             gridY,
	                                             media, imageOnly));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBackgroundFullRule(StaticStyleSelectorKind selectorKind,
                                                  const char *selector,
                                                  StaticStyleLinearGradientRef gradient,
                                                  StaticStyleLinearGradientRef overlayGradient,
                                                  bool hasOverlayGradient,
                                                  StaticStyleRadialGradientRef radialGradient,
                                                  StaticStyleBackgroundGridLine gridX,
                                                  StaticStyleBackgroundGridLine gridY,
                                                  const char *media, bool imageOnly)
{
	rules().push_back(makeStaticBackgroundFullCssRule(selectorKind,
	                                                 selector,
	                                                 gradient,
	                                                 overlayGradient,
	                                                 hasOverlayGradient,
	                                                 radialGradient,
	                                                 gridX,
	                                                 gridY,
	                                                 media, imageOnly));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticBackgroundSizeRule(StaticStyleSelectorKind selectorKind,
                                                  const char *selector,
                                                  StaticStyleLengthSpec stepX,
                                                  StaticStyleLengthSpec stepY,
                                                  const char *media)
{
	rules().push_back(makeStaticBackgroundSizeCssRule(selectorKind, selector, stepX, stepY, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticAnimationRule(StaticStyleSelectorKind selectorKind,
                                             const char *selector,
                                             const char *name,
                                             std::uint32_t durationMs,
                                             std::uint32_t delayMs,
                                             int iterations,
                                             StaticStyleAnimationDirection direction,
                                             StaticStyleAnimationFill fill,
                                             StaticStyleAnimationEasing easing,
                                             const char *media)
{
	rules().push_back(makeStaticAnimationCssRule(selectorKind,
	                                           selector,
	                                           name,
	                                           durationMs,
	                                           delayMs,
	                                           iterations,
	                                           direction,
	                                           fill,
	                                           easing,
	                                           media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticCustomLengthRule(StaticStyleSelectorKind selectorKind,
                                                const char *selector,
                                                const char *name,
                                                StaticStyleLengthSpec length,
                                                const char *media)
{
	rules().push_back(makeStaticCustomLengthCssRule(selectorKind, selector, name, length, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticCustomColorRule(StaticStyleSelectorKind selectorKind,
                                               const char *selector,
                                               const char *name,
                                               int r,
                                               int g,
                                               int b,
                                               int a,
                                               const char *media)
{
	rules().push_back(makeStaticCustomColorCssRule(selectorKind, selector, name, r, g, b, a, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticTransformRule(StaticStyleSelectorKind selectorKind,
                                             const char *selector,
                                             std::uint16_t flags,
                                             int rotateX,
                                             int rotateY,
                                             int rotateZ,
                                             StaticStyleLengthSpec translateX,
                                             StaticStyleLengthSpec translateY,
                                             StaticStyleLengthSpec translateZ,
                                             int scaleX,
                                             int scaleY,
                                             const char *media,
                                             int scaleZ)
{
	rules().push_back(makeStaticTransformCssRule(selectorKind,
	                                            selector,
	                                            flags,
	                                            rotateX,
	                                            rotateY,
	                                            rotateZ,
	                                            translateX,
	                                            translateY,
	                                            translateZ,
	                                            scaleX,
	                                            scaleY,
	                                            scaleZ,
	                                            media));
	{
		static const bool gTraceCube = std::getenv("GEA_DEBUG_CUBE") != nullptr;
		if (gTraceCube)
			std::printf("[cube] registerStaticTransformRule sel=%s flags=%u rx=%d ry=%d rz=%d tzUnit=%d tzVal=%.2f\n",
			            selector ? selector : "(null)", static_cast<unsigned>(flags), rotateX, rotateY, rotateZ,
			            static_cast<int>(translateZ.unit), static_cast<double>(translateZ.value));
	}
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticOriginRule(StaticStyleSelectorKind selectorKind,
                                          const char *selector,
                                          StaticStyleOriginProperty property,
                                          int xPermille,
                                          int yPermille,
                                          const char *media)
{
	rules().push_back(makeStaticOriginCssRule(selectorKind, selector, property, xPermille, yPermille, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::registerStaticGridTemplateRule(StaticStyleSelectorKind selectorKind,
                                                const char *selector,
                                                StaticStyleGridTemplateProperty property,
                                                std::initializer_list<StaticStyleGridTemplateTrack> tracks,
                                                const char *media)
{
	rules().push_back(makeStaticGridTemplateCssRule(selectorKind, selector, property, tracks, media));
	noteStyleRuleRegistrationChanged();
}

void StyleSheet::applyClass(NodeHandle node, const std::string &className) const
{
	if (!node) return;
	setClassName(node, className);
}

void StyleSheet::setClassName(NodeHandle node, const std::string &className) const
{
	if (!node) return;
	Tree::instance().setClassName(node.id(), className);
}

bool StyleSheet::applyPixelLengthProperty(NodeHandle node, StyleDeclaration declaration, double value) const
{
	return applyNumberDeclarationWithSource(node, declaration, value * g_device_pixel_ratio, StyleApplicationSource::Inline);
}

bool StyleSheet::applyNumberProperty(NodeHandle node, const char *property, double value) const
{
	return applyNumberPropertyWithSource(node, property, value, StyleApplicationSource::Inline);
}

bool StyleSheet::applyNumberProperty(NodeHandle node, StyleDeclaration declaration, double value) const
{
	return applyNumberDeclarationWithSource(node, declaration, value, StyleApplicationSource::Inline);
}

bool StyleSheet::removeProperty(NodeHandle node, const std::string &property) const
{
	return removeInlineStyleProperty(node, property);
}

void StyleSheet::applyProperty(NodeHandle node, StyleDeclaration declaration, const std::string &value) const
{
	applyPropertyWithSource(node, declaration, value, StyleApplicationSource::Inline);
}

void StyleSheet::applyProperty(NodeHandle node, const char *property, const std::string &value) const
{
	applyPropertyWithSource(node, property, value, StyleApplicationSource::Inline);
}

void StyleSheet::applyProperty(NodeHandle node, const std::string &property, const std::string &value) const
{
	applyPropertyWithSource(node, property, value, StyleApplicationSource::Inline);
}

void StyleSheet::hoverChanged() const
{
	rebuildRuleIndexIfNeeded();
	if (!g_ruleIndex.hasHoverRules) return;
	// Hover can change ancestor selectors and inherited/custom properties.
	// Matching depends on pointer state, not just the class/tag signature.
	clearActiveRulePlanCache();
	recomputeAllClassStyles();
}

void StyleSheet::recomputeSubtree(int nodeId) const
{
	recomputeSubtreeClassStyles(nodeId);
}

void StyleSheet::startCssAnimations(std::uint32_t nowMs) const
{
	rebuildRuleIndexIfNeeded();
	if (g_ruleIndex.animationRules.empty()) return;
	auto &state = treeState();
	for (int node = 0; node < state.nodeCount; ++node) {
		if (isDisplayNone(state.nodes[node].style)) continue;
		const CssAnimationSpec spec = animationSpecForNode(node);
		startAnimationForNode(node, spec, nowMs);
	}
}

void Tree::setClassName(int node, const std::string &className)
{
	setClassName(node, className.c_str());
}

void Tree::setClassName(int node, const char *className)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	// Capture the old classes before mutating, so the incremental recompute can tell
	// whether this change could flip a descendant-combinator match (added OR removed).
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	const bool changed = state.classLists[node].set(className);
	if (!changed) return;
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
}

// Global tag-string table. A std::deque keeps element addresses stable across
// growth, so tagFromId can hand out c_str() pointers that stay valid. Index 0
// is the empty tag, matching Node::tag_id's zero default.
namespace {
std::deque<std::string> &tagTable()
{
	static std::deque<std::string> table = {std::string()};
	return table;
}
}  // namespace

int16_t internTag(const char *tag)
{
	const char *s = tag ? tag : "";
	auto &table = tagTable();
	for (std::size_t i = 0; i < table.size(); i++) {
		if (table[i] == s) return static_cast<int16_t>(i);
	}
	table.emplace_back(s);
	return static_cast<int16_t>(table.size() - 1);
}

const char *tagFromId(int16_t id)
{
	auto &table = tagTable();
	if (id < 0 || static_cast<std::size_t>(id) >= table.size()) return "";
	return table[static_cast<std::size_t>(id)].c_str();
}

// ---- RareStyle pool ---------------------------------------------------------
// Cold style fields, pooled and keyed by ComputedStyle::rare_style. The pool can
// keep a target-sized first tier in SRAM and spill to a deque; a free list keeps
// handles reused.
// rareStylePool() + rstyle() are now inline in node_model.h (hot read path). The
// free list stays here — only rstyleMut/releaseRareStyle/resetRareStylePool use it.
// When GEA_EMBEDDED_RARE_STYLE_INLINE is enabled, the rare fields are embedded in
// ComputedStyle, so these become trivial (rstyleMut returns the embedded struct;
// release/reset are no-ops) and the pool + free list are compiled out.
#if !GEA_EMBEDDED_RARE_STYLE_INLINE
namespace {
std::vector<int16_t> &rareStyleFreeList()
{
	static std::vector<int16_t> freeList;
	return freeList;
}
}  // namespace
#endif

RareStyle &rstyleMut(ComputedStyle &style)
{
	// rstyleMut is also used by native setup/tests that write transform fields
	// directly, bypassing Tree::setStyle's transform-cache invalidation.
	auto &state = treeState();
	state.transformScanSerial = ~0ull;
	state.transformScanValid = false;
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	return style.rare;
#else
	auto &pool = rareStylePool();
	if (style.rare_style >= 0) return pool[static_cast<std::size_t>(style.rare_style)];
	int16_t handle;
	auto &freeList = rareStyleFreeList();
	if (!freeList.empty()) {
		handle = freeList.back();
		freeList.pop_back();
	} else {
		handle = static_cast<int16_t>(pool.size());
		pool.emplace_back();
	}
	style.rare_style = handle;
	return pool[static_cast<std::size_t>(handle)];
#endif
}

void releaseRareStyle(int16_t &handle)
{
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	(void)handle;  // embedded rare is freed with the node; nothing to release
#else
	if (handle < 0) return;
	rareStylePool()[static_cast<std::size_t>(handle)] = RareStyle{};  // clear for reuse
	rareStyleFreeList().push_back(handle);
	handle = -1;
#endif
}

void resetRareStylePool()
{
#if GEA_EMBEDDED_RARE_STYLE_INLINE
	// rare fields are embedded per-node; there is no shared pool to reset.
#else
	rareStylePool().clear();
	rareStyleFreeList().clear();
#endif
}

void Tree::setTagName(int node, const char *tagName)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	const int16_t id = internTag(tagName ? tagName : "");
	if (state.nodes[node].tag_id == id) return;
	state.nodes[node].tag_id = id;
	recomputeSubtreeClassStyles(node);
}

const char *Tree::tagName(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return "";
	return tagFromId(state.nodes[node].tag_id);
}

bool Tree::addClass(int node, const std::string &token)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	if (!state.classLists[node].add(token)) return false;
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
	return true;
}

bool Tree::removeClass(int node, const std::string &token)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	if (!state.classLists[node].remove(token)) return false;
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
	return true;
}

bool Tree::toggleClass(int node, const std::string &token)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	const bool willContain = !state.classLists[node].contains(token);
	if (willContain) {
		if (!state.classLists[node].add(token)) return false;
	} else {
		if (!state.classLists[node].remove(token)) return false;
	}
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
	return willContain;
}

bool Tree::toggleClass(int node, const std::string &token, bool force)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	const bool changed = force ? state.classLists[node].add(token) : state.classLists[node].remove(token);
	if (changed) {
		noteClassMutationForIncremental(node, oldTokens);
		syncNodeClassAttribute(node);
		recomputeSubtreeClassStyles(node);
	}
	return force ? state.classLists[node].contains(token) : false;
}

void Tree::clearClasses(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (state.classLists[node].empty()) {
		syncNodeClassAttribute(node);
		return;
	}
	const NodeClassSnapshot oldTokens(state.classLists[node]);
	state.classLists[node].clear();
	noteClassMutationForIncremental(node, oldTokens);
	syncNodeClassAttribute(node);
	recomputeSubtreeClassStyles(node);
}

bool Tree::hasClass(int node, const std::string &token) const
{
	auto &state = treeState();
	return node >= 0 && node < state.nodeCount && state.classLists[node].contains(token);
}

std::string Tree::className(int node) const
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return std::string();
	return state.classLists[node].value();
}

void setViewportMetrics(int width, int height, double devicePixelRatio)
{
	g_viewport_width = width;
	g_viewport_height = height;
	g_device_pixel_ratio = sanitizedDevicePixelRatio(devicePixelRatio);
	clearStaticLengthExpressionResolutionCache();
	invalidateRuleIndex();
	recomputeAllClassStyles();
}

double devicePixelRatio()
{
	return g_device_pixel_ratio;
}

void setDevicePixelRatio(double devicePixelRatio)
{
	const double sanitized = sanitizedDevicePixelRatio(devicePixelRatio);
	if (sanitized == g_device_pixel_ratio) return;
	g_device_pixel_ratio = sanitized;
	clearStaticLengthExpressionResolutionCache();
	invalidateRuleIndex();
	recomputeAllClassStyles();
}

void setSafeAreaInsetBottom(int inset)
{
	g_safe_area_inset_bottom = inset > 0 ? inset : 0;
}

int safeAreaInsetBottom()
{
	return g_safe_area_inset_bottom;
}

// C ABI shims are kept for older generated code, but C++ code should call the
// namespaced functions above.
extern "C" void gea_style_set_viewport_metrics(int width, int height, double devicePixelRatio)
{
	gea::embedded::ui::setViewportMetrics(width, height, devicePixelRatio);
}

extern "C" double gea_style_get_device_pixel_ratio()
{
	return gea::embedded::ui::devicePixelRatio();
}

extern "C" void gea_style_set_device_pixel_ratio(double devicePixelRatio)
{
	gea::embedded::ui::setDevicePixelRatio(devicePixelRatio);
}

// Physical-pixel height reserved at the bottom of the panel for a
// platform-drawn overlay (geaos home button). Set once by the target;
// read by the virtual keyboard so it floats above the overlay instead
// of being painted over by it.
extern "C" void gea_style_set_safe_area_inset_bottom(int inset)
{
	gea::embedded::ui::setSafeAreaInsetBottom(inset);
}

extern "C" int gea_style_get_safe_area_inset_bottom()
{
	return gea::embedded::ui::safeAreaInsetBottom();
}

// Backward-compatible entry point for targets that have no DPR concept yet.
extern "C" void gea_style_set_viewport_size(int width, int height)
{
	gea::embedded::ui::setViewportMetrics(width, height, 1.0);
}

}  // namespace gea::embedded::ui
