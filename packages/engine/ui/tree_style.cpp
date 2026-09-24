// SPDX-License-Identifier: Apache-2.0
#include "display_invalidation.h"
#include "internal.h"
#include "node_lifecycle.h"
#include "refresh_perf.h"
#include "style_values.h"
#include "tree_state.h"

// Skip the inline-style record for Left/Top (see setStyleValue). Default off: keep the
// record so static-authored absolute elements survive class-recompute. Opt in per-app
// for reactive-keyed-list-position apps (bouncing-balls-jsx) that re-apply every frame.
#ifndef GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD
#define GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD 0
#endif

namespace gea::embedded::ui {

namespace {

bool nodeParticipatesInMountedTree(const TreeState &state, int node)
{
	const int root = Tree::instance().mountedRoot();
	if (root < 0) return false;
	for (int current = node; current >= 0 && current < state.nodeCount; current = state.nodes[current].parent) {
		if (current == root) return true;
	}
	return false;
}

bool isTransformProperty(Property prop)
{
	switch (prop) {
	case Property::RotateAngle:
	case Property::RotateAxisX:
	case Property::RotateAxisY:
	case Property::RotateAxisZ:
	case Property::ScaleX:
	case Property::ScaleY:
	case Property::ScaleZ:
	case Property::TransformTranslateOuterAxes:
	case Property::TranslateX:
	case Property::TranslateY:
	case Property::TranslateZ:
	case Property::TranslateXPercent:
	case Property::TranslateYPercent:
	case Property::TransformRotate:
	case Property::TransformRotateX:
	case Property::TransformRotateY:
	case Property::TransformTranslateX:
	case Property::TransformTranslateY:
	case Property::TransformTranslateZ:
	case Property::TransformTranslateXPercent:
	case Property::TransformTranslateYPercent:
	case Property::TransformScaleX:
	case Property::TransformScaleY:
	case Property::TransformScaleZ:
	case Property::TransformOriginX:
	case Property::TransformOriginY:
	case Property::Perspective:
	case Property::PerspectiveOriginX:
	case Property::PerspectiveOriginY:
		return true;
	default:
		return false;
	}
}

bool isLayoutProperty(Property prop)
{
	switch (prop) {
	case Property::FlexLineCount:
	case Property::AspectRatio:
	case Property::TranslatePresent:
	case Property::TransformPresent:
	case Property::RotatePresent:
	case Property::ScalePresent:
	case Property::FilterPresent:
	case Property::Display:
	case Property::FlexDirection:
	case Property::BoxSizing:
	case Property::Float:
	case Property::MarginTrim:
	case Property::Clear:
	case Property::WritingMode:
	case Property::Direction:
	case Property::RowGap:
	case Property::ColumnGap:
	case Property::RowGapPercent:
	case Property::ColumnGapPercent:
	case Property::MarginTopAuto:
	case Property::MarginRightAuto:
	case Property::MarginBottomAuto:
	case Property::MarginLeftAuto:
	case Property::MarginTopExpression:
	case Property::MarginRightExpression:
	case Property::MarginBottomExpression:
	case Property::MarginLeftExpression:
	case Property::PaddingTopExpression:
	case Property::PaddingRightExpression:
	case Property::PaddingBottomExpression:
	case Property::PaddingLeftExpression:
	case Property::WidthExpression:
	case Property::HeightExpression:
	case Property::Order:
	case Property::FlexWrap:
	case Property::JustifyContent:
	case Property::AlignItems:
	case Property::JustifyItems:
	case Property::JustifySelf:
	case Property::GridRowStart:
	case Property::GridColumnStart:
	case Property::GridRowEnd:
	case Property::GridColumnEnd:
	case Property::AlignContent:
	case Property::AlignSelf:
	case Property::Gap:
	case Property::Width:
	case Property::Height:
	case Property::WidthPercent:
	case Property::HeightPercent:
	case Property::MinWidth:
	case Property::MinHeight:
	case Property::MaxWidth:
	case Property::MaxHeight:
	case Property::Flex:
	case Property::FlexShrink:
	case Property::FlexBasis:
	case Property::FlexBasisExpression:
	case Property::BorderWidth:
	case Property::BorderTopWidth:
	case Property::BorderRightWidth:
	case Property::BorderBottomWidth:
	case Property::BorderLeftWidth:
	case Property::PaddingTop:
	case Property::PaddingRight:
	case Property::PaddingBottom:
	case Property::PaddingLeft:
	case Property::MarginTop:
	case Property::MarginRight:
	case Property::MarginBottom:
	case Property::MarginLeft:
	case Property::Position:
	case Property::Top:
	case Property::Right:
	case Property::Bottom:
	case Property::Left:
	case Property::TopPercent:
	case Property::RightPercent:
	case Property::BottomPercent:
	case Property::LeftPercent:
	case Property::Overflow:
	case Property::OverflowX:
	case Property::OverflowY:
	case Property::FontId:
	case Property::FontSize:
	case Property::FontWeight:
	case Property::LineHeight:
	case Property::LineHeightExpression:
	case Property::LineHeightMultiplier:
	case Property::Visibility:
	case Property::WhiteSpace:
	case Property::TextOverflow:
	case Property::ImageId:
	case Property::ImageFit:
		return true;
	default:
		return false;
	}
}

bool isPaintProperty(Property prop)
{
	return DisplayInvalidation::rebuildsNodeDisplayCommands(prop) && !isTransformProperty(prop);
}

struct ScopedRefreshStat
{
	std::int64_t start;
	std::int64_t &slot;

	explicit ScopedRefreshStat(std::int64_t &slot)
		: start(refreshPerfNowUs()), slot(slot)
	{
	}

	~ScopedRefreshStat()
	{
		slot += refreshPerfNowUs() - start;
	}
};

void setStyleValue(Tree &tree, int node, Property prop, int value, bool recordInline)
{
	auto &perf = refreshPerfStatsMutable();
	ScopedRefreshStat timer(perf.treeSetStyleUs);
	perf.treeSetStyleCalls++;
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	// Record every authored inline property — including Left/Top — so it survives a
	// class-change / style recompute. resetStyleForClassRecompute() rebuilds a node's
	// style from its class rules + inlineStyles, so anything NOT recorded here is lost
	// on the next recompute. A static template that authors inline left/top exactly
	// once (e.g. an absolutely-positioned board: `style={{ left: BOARD_X, top: BOARD_Y }}`)
	// depends on this: previously Left/Top were skipped as a perf shortcut for the
	// reactive keyed-list position path (~128 writes/frame in bouncing-balls), which
	// pinned every statically-authored absolute element to (0,0) after the first
	// recompute while width/height (which WERE recorded) survived. The keyed-list path
	// re-applies its live left/top every frame, so a recompute that momentarily reverts
	// to the last-recorded value self-corrects on the next frame; the only cost there is
	// the per-write record.
	// Per-app opt (GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD): the inline-style record
	// exists only so authored styles survive a class-recompute. Apps whose Left/Top come
	// exclusively from a reactive keyed-list (bouncing-balls: ~128 writes/frame) re-apply
	// live position every frame, so a recompute self-corrects next frame — the record is
	// pure waste (~384µs/frame here). Skip Left/Top recording for those apps ONLY; keep it
	// for any app that authors static absolute left/top AND toggles classes.
#if GEA_EMBEDDED_SKIP_POSITION_INLINE_RECORD
	if (recordInline && prop != Property::Left && prop != Property::Top)
#else
	if (recordInline)
#endif
	{
		auto &overrides = ensureRareData(node).inlineStyles;
		// Width/height and their percentage/expression companions are one
		// authored declaration. Replaying a stale companion would override a
		// later numeric value (or an intrinsic sizing keyword).
		if (prop == Property::Height || prop == Property::HeightPercent || prop == Property::HeightExpression) {
			for (Property companion : {Property::Height, Property::HeightPercent, Property::HeightExpression})
				if (companion != prop) overrides.remove(companion);
		}
		if (prop == Property::Width || prop == Property::WidthPercent || prop == Property::WidthExpression) {
			for (Property companion : {Property::Width, Property::WidthPercent, Property::WidthExpression})
				if (companion != prop) overrides.remove(companion);
		}
		if (prop == Property::LineHeight || prop == Property::LineHeightExpression || prop == Property::LineHeightMultiplier)
			for (Property companion : {Property::LineHeight, Property::LineHeightExpression, Property::LineHeightMultiplier})
				if (companion != prop) overrides.remove(companion);
		if (prop == Property::FlexBasis || prop == Property::FlexBasisExpression)
			overrides.remove(prop == Property::FlexBasis ? Property::FlexBasisExpression : Property::FlexBasis);
		if (prop >= Property::MarginTop && prop <= Property::MarginLeft)
			overrides.remove(static_cast<Property>(static_cast<int>(Property::MarginTopExpression) + static_cast<int>(prop) - static_cast<int>(Property::MarginTop)));
		if (prop >= Property::PaddingTop && prop <= Property::PaddingLeft)
			overrides.remove(static_cast<Property>(static_cast<int>(Property::PaddingTopExpression) + static_cast<int>(prop) - static_cast<int>(Property::PaddingTop)));
		if (prop == Property::BorderWidth)
			for (Property side : {Property::BorderTopWidth, Property::BorderRightWidth, Property::BorderBottomWidth, Property::BorderLeftWidth}) overrides.remove(side);
		if (prop == Property::BorderRelief)
			for (int side = 0; side < 4; ++side) overrides.remove(static_cast<Property>(static_cast<int>(Property::BorderTopRelief) + side));

		// Literal/currentColor companions form one authored color. A common
		// border-color resets all side colors; later side writes retain their order.
		if (prop == Property::BorderColor || prop == Property::BorderColorCurrent) {
			for (Property color : {Property::BorderColor, Property::BorderColorCurrent,
			     Property::BorderTopColor, Property::BorderRightColor, Property::BorderBottomColor, Property::BorderLeftColor,
			     Property::BorderTopColorCurrent, Property::BorderRightColorCurrent, Property::BorderBottomColorCurrent, Property::BorderLeftColorCurrent, Property::BorderAlpha, Property::BorderTopAlpha, Property::BorderRightAlpha, Property::BorderBottomAlpha, Property::BorderLeftAlpha})
				overrides.remove(color);
		} else {
			const Property colors[] = {Property::BorderTopColor, Property::BorderRightColor, Property::BorderBottomColor, Property::BorderLeftColor};
			for (int side = 0; side < 4; ++side) {
				const Property current = static_cast<Property>(static_cast<int>(Property::BorderTopColorCurrent) + side);
				if (prop == colors[side] || prop == current) { overrides.remove(colors[side]); overrides.remove(current); overrides.remove(static_cast<Property>(static_cast<int>(Property::BorderTopAlpha) + side)); break; }
			}
		}
		if (prop == Property::BackgroundColor) { overrides.remove(Property::BackgroundColor); overrides.remove(Property::BackgroundAlpha); }
		if (prop == Property::Color) { overrides.remove(Property::Color); overrides.remove(Property::ColorAlpha); }
		overrides.set(prop, value);
	}
	Node *n = &state.nodes[node];
	int changed = 0;
	int prevOpacity = -1;
	// These previous-bg values feed ONLY the BackgroundColor solid-recolor fast path
	// below (itself gated on prop == BackgroundColor). Reading them on every setStyle —
	// including the 4 pooled rstyle() bg-gradient lookups — is pure waste for the common
	// layout/paint writes (e.g. 128 left/top writes per frame in bouncing-balls = ~512
	// wasted pooled reads). Gate them on the property.
	const bool isBgColorChange = (prop == Property::BackgroundColor);
	const style_color_t previousBgColor = isBgColorChange ? n->style.bg_color : style_color_t{};
	const uint8_t previousHasBg = isBgColorChange ? n->style.has_bg : 0;
	const uint8_t previousBgAlpha = isBgColorChange ? n->style.bg_alpha : 0;
	const int8_t previousBgFill = isBgColorChange ? n->style.bg_fill : 0;
	const uint8_t previousBgGradientHasMid = isBgColorChange ? rstyle(n->style).bg_gradient_has_mid : 0;
	const uint8_t previousBgOverlayGradient = isBgColorChange ? rstyle(n->style).bg_overlay_gradient : 0;
	const uint8_t previousBgRadialGradient = isBgColorChange ? rstyle(n->style).bg_radial_gradient : 0;
	const uint8_t previousBgGridAxes = isBgColorChange ? rstyle(n->style).bg_grid_axes : 0;
	switch (prop) {
	case Property::Display:
		// Any application of the `display` property is explicit authoring — record
		// it so the inline-formatting heuristic treats e.g. `display:block` on a
		// <span> as block-level (stacks) rather than its default inline behaviour.
		if (!n->style.display_explicit) { n->style.display_explicit = 1; changed = 1; }
		if (n->style.display != value) { n->style.display = value; changed = 1; }
		break;
	case Property::FlexDirection:
		if (!n->style.flex_direction_explicit) {
			n->style.flex_direction_explicit = 1;
			changed = 1;
		}
		if (n->style.flex_direction != value) {
			n->style.flex_direction = value;
			changed = 1;
		}
		break;
	case Property::BoxSizing: if (n->style.box_sizing != value) { n->style.box_sizing = value; changed = 1; } break;
	case Property::Float: if (n->style.float_side != value) { n->style.float_side = value; changed = 1; } break;
	case Property::AspectRatio: if (rstyle(n->style).aspect_ratio != value) { rstyleMut(n->style).aspect_ratio = value; changed = 1; } break;
	case Property::Containment: if (rstyle(n->style).containment != value) { rstyleMut(n->style).containment = value; changed = 1; } break;
	case Property::FlexLineCount: if (rstyle(n->style).flex_line_count != value) { rstyleMut(n->style).flex_line_count = value; changed = 1; } break;
	case Property::MarginTrim: if (rstyle(n->style).margin_trim != value) { rstyleMut(n->style).margin_trim = value; changed = 1; } break;
	case Property::Clear: if (n->style.clear_side != value) { n->style.clear_side = value; changed = 1; } break;
	case Property::Direction: if (n->style.direction != value) { n->style.direction = value; changed = 1; } break;
	case Property::WritingMode: if (n->style.writing_mode != value) { n->style.writing_mode = value; changed = 1; } break;
	case Property::RowGap: if (n->style.row_gap != value) { n->style.row_gap = value; changed = 1; } break;
	case Property::ColumnGap: if (n->style.column_gap != value) { n->style.column_gap = value; changed = 1; } break;
	case Property::RowGapPercent: if (n->style.row_gap_percent != value) { n->style.row_gap_percent = value; changed = 1; } break;
	case Property::ColumnGapPercent: if (n->style.column_gap_percent != value) { n->style.column_gap_percent = value; changed = 1; } break;
	case Property::MarginTopAuto: { const auto mask = (n->style.margin_auto & ~1) | (value ? 1 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginRightAuto: { const auto mask = (n->style.margin_auto & ~2) | (value ? 2 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginBottomAuto: { const auto mask = (n->style.margin_auto & ~4) | (value ? 4 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginLeftAuto: { const auto mask = (n->style.margin_auto & ~8) | (value ? 8 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginTopExpression: if (rstyle(n->style).margin_expression[0] != value) { rstyleMut(n->style).margin_expression[0] = value; changed = 1; } break;
	case Property::MarginRightExpression: if (rstyle(n->style).margin_expression[1] != value) { rstyleMut(n->style).margin_expression[1] = value; changed = 1; } break;
	case Property::MarginBottomExpression: if (rstyle(n->style).margin_expression[2] != value) { rstyleMut(n->style).margin_expression[2] = value; changed = 1; } break;
	case Property::MarginLeftExpression: if (rstyle(n->style).margin_expression[3] != value) { rstyleMut(n->style).margin_expression[3] = value; changed = 1; } break;
	case Property::PaddingTopExpression: if (rstyle(n->style).padding_expression[0] != value) { rstyleMut(n->style).padding_expression[0] = value; changed = 1; } break;
	case Property::PaddingRightExpression: if (rstyle(n->style).padding_expression[1] != value) { rstyleMut(n->style).padding_expression[1] = value; changed = 1; } break;
	case Property::PaddingBottomExpression: if (rstyle(n->style).padding_expression[2] != value) { rstyleMut(n->style).padding_expression[2] = value; changed = 1; } break;
	case Property::PaddingLeftExpression: if (rstyle(n->style).padding_expression[3] != value) { rstyleMut(n->style).padding_expression[3] = value; changed = 1; } break;
	case Property::WidthExpression: if (n->style.width_expression != value) { n->style.width_expression = value; n->style.width = n->style.width_percent = kUnset; changed = 1; } break;
	case Property::HeightExpression: if (n->style.height_expression != value) { n->style.height_expression = value; n->style.height = n->style.height_percent = kUnset; changed = 1; } break;
	case Property::Order:           if (n->style.order != value) { n->style.order = value; changed = 1; } break;
	case Property::FlexWrap:        if (n->style.flex_wrap != value) { n->style.flex_wrap = value; changed = 1; } break;
	case Property::JustifyContent:  if (n->style.justify_content != value) { n->style.justify_content = value; changed = 1; } break;
	case Property::AlignItems:      if (n->style.align_items != value) { n->style.align_items = value; changed = 1; } break;
	case Property::JustifyItems:    if (n->style.justify_items != value) { n->style.justify_items = value; changed = 1; } break;
	case Property::AlignContent:    if (n->style.align_content != value) { n->style.align_content = value; changed = 1; } break;
	case Property::AlignSelf:       if (n->style.align_self != value) { n->style.align_self = value; changed = 1; } break;
	case Property::JustifySelf:     if (rstyle(n->style).justify_self != value) { rstyleMut(n->style).justify_self = value; changed = 1; } break;
	case Property::GridRowStart: if (rstyle(n->style).grid_line[0] != value) { rstyleMut(n->style).grid_line[0] = value; changed = 1; } break;
	case Property::GridColumnStart: if (rstyle(n->style).grid_line[1] != value) { rstyleMut(n->style).grid_line[1] = value; changed = 1; } break;
	case Property::GridRowEnd: if (rstyle(n->style).grid_line[2] != value) { rstyleMut(n->style).grid_line[2] = value; changed = 1; } break;
	case Property::GridColumnEnd: if (rstyle(n->style).grid_line[3] != value) { rstyleMut(n->style).grid_line[3] = value; changed = 1; } break;
	case Property::Gap:
		if (n->style.gap != value || n->style.row_gap != kUnset || n->style.column_gap != kUnset || n->style.row_gap_percent != kUnset || n->style.column_gap_percent != kUnset) {
			n->style.gap = value;
			n->style.row_gap = n->style.column_gap = n->style.row_gap_percent = n->style.column_gap_percent = kUnset;
			changed = 1;
		}
		break;
	case Property::Width:
		if (n->style.width_expression >= 0) { n->style.width_expression = -1; changed = 1; }
		if (n->style.width != value || n->style.width_percent != kUnset) {
			n->style.width = value;
			n->style.width_percent = kUnset;
			changed = 1;
		}
		break;
	case Property::Height:
		if (n->style.height_expression != -1) { n->style.height_expression = -1; changed = 1; }
		if (n->style.height != value || n->style.height_percent != kUnset) {
			n->style.height = value;
			n->style.height_percent = kUnset;
			changed = 1;
		}
		break;
	case Property::WidthPercent:
		if (n->style.width_expression >= 0) { n->style.width_expression = -1; changed = 1; }
		if (n->style.width_percent != value || n->style.width != kUnset) {
			n->style.width_percent = value;
			n->style.width = kUnset;
			changed = 1;
		}
		break;
	case Property::HeightPercent:
		if (n->style.height_expression != -1) { n->style.height_expression = -1; changed = 1; }
		if (n->style.height_percent != value || n->style.height != kUnset) {
			n->style.height_percent = value;
			n->style.height = kUnset;
			changed = 1;
		}
		break;
	case Property::MinWidth:        if (n->style.min_width != value) { n->style.min_width = value; changed = 1; } break;
	case Property::MinHeight:       if (n->style.min_height != value) { n->style.min_height = value; changed = 1; } break;
	case Property::MaxWidth:        if (n->style.max_width != value) { n->style.max_width = value; changed = 1; } break;
	case Property::MaxHeight:       if (n->style.max_height != value) { n->style.max_height = value; changed = 1; } break;
	case Property::Flex:             if (n->style.flex != value) { n->style.flex = value; changed = 1; } break;
	case Property::FlexShrink:       if (n->style.flex_shrink != value) { n->style.flex_shrink = value; changed = 1; } break;
	case Property::FlexBasis:
		if (rstyle(n->style).flex_basis_expression >= 0) { rstyleMut(n->style).flex_basis_expression = -1; changed = 1; }
		if (n->style.flex_basis != value) { n->style.flex_basis = value; changed = 1; } break;
	case Property::FlexBasisExpression:
		if (rstyle(n->style).flex_basis_expression != value || n->style.flex_basis != kUnset) {
			rstyleMut(n->style).flex_basis_expression = value; n->style.flex_basis = kUnset; changed = 1;
		} break;
	case Property::PaddingTop: if (rstyle(n->style).padding_expression[0] >= 0) { rstyleMut(n->style).padding_expression[0] = -1; changed = 1; }      if (n->style.padding[0] != value) { n->style.padding[0] = value; changed = 1; } break;
	case Property::PaddingRight: if (rstyle(n->style).padding_expression[1] >= 0) { rstyleMut(n->style).padding_expression[1] = -1; changed = 1; }    if (n->style.padding[1] != value) { n->style.padding[1] = value; changed = 1; } break;
	case Property::PaddingBottom: if (rstyle(n->style).padding_expression[2] >= 0) { rstyleMut(n->style).padding_expression[2] = -1; changed = 1; }   if (n->style.padding[2] != value) { n->style.padding[2] = value; changed = 1; } break;
	case Property::PaddingLeft: if (rstyle(n->style).padding_expression[3] >= 0) { rstyleMut(n->style).padding_expression[3] = -1; changed = 1; }     if (n->style.padding[3] != value) { n->style.padding[3] = value; changed = 1; } break;
	case Property::MarginTop: if (rstyle(n->style).margin_expression[0] >= 0) { rstyleMut(n->style).margin_expression[0] = -1; changed = 1; } if (n->style.margin[0] != value || (n->style.margin_auto & 1)) { n->style.margin[0] = value; n->style.margin_auto &= ~1; changed = 1; } break;
	case Property::MarginRight: if (rstyle(n->style).margin_expression[1] >= 0) { rstyleMut(n->style).margin_expression[1] = -1; changed = 1; } if (n->style.margin[1] != value || (n->style.margin_auto & 2)) { n->style.margin[1] = value; n->style.margin_auto &= ~2; changed = 1; } break;
	case Property::MarginBottom: if (rstyle(n->style).margin_expression[2] >= 0) { rstyleMut(n->style).margin_expression[2] = -1; changed = 1; } if (n->style.margin[2] != value || (n->style.margin_auto & 4)) { n->style.margin[2] = value; n->style.margin_auto &= ~4; changed = 1; } break;
	case Property::MarginLeft: if (rstyle(n->style).margin_expression[3] >= 0) { rstyleMut(n->style).margin_expression[3] = -1; changed = 1; } if (n->style.margin[3] != value || (n->style.margin_auto & 8)) { n->style.margin[3] = value; n->style.margin_auto &= ~8; changed = 1; } break;
	case Property::Position:         if (value == kPositionFixed) state.fixedPositionUsed = true; if (n->style.position != value) { n->style.position = value; changed = 1; } break;
	case Property::Top:
		if (n->style.pos_offsets[0] != value || n->style.pos_offset_percent[0] != kUnset) {
			n->style.pos_offsets[0] = value;
			n->style.pos_offset_percent[0] = kUnset;
			changed = 1;
		}
		break;
	case Property::Right:
		if (n->style.pos_offsets[1] != value || n->style.pos_offset_percent[1] != kUnset) {
			n->style.pos_offsets[1] = value;
			n->style.pos_offset_percent[1] = kUnset;
			changed = 1;
		}
		break;
	case Property::Bottom:
		if (n->style.pos_offsets[2] != value || n->style.pos_offset_percent[2] != kUnset) {
			n->style.pos_offsets[2] = value;
			n->style.pos_offset_percent[2] = kUnset;
			changed = 1;
		}
		break;
	case Property::Left:
		if (n->style.pos_offsets[3] != value || n->style.pos_offset_percent[3] != kUnset) {
			n->style.pos_offsets[3] = value;
			n->style.pos_offset_percent[3] = kUnset;
			changed = 1;
		}
		break;
	case Property::TopPercent:
		if (n->style.pos_offset_percent[0] != value || n->style.pos_offsets[0] != kUnset) {
			n->style.pos_offset_percent[0] = value;
			n->style.pos_offsets[0] = kUnset;
			changed = 1;
		}
		break;
	case Property::RightPercent:
		if (n->style.pos_offset_percent[1] != value || n->style.pos_offsets[1] != kUnset) {
			n->style.pos_offset_percent[1] = value;
			n->style.pos_offsets[1] = kUnset;
			changed = 1;
		}
		break;
	case Property::BottomPercent:
		if (n->style.pos_offset_percent[2] != value || n->style.pos_offsets[2] != kUnset) {
			n->style.pos_offset_percent[2] = value;
			n->style.pos_offsets[2] = kUnset;
			changed = 1;
		}
		break;
	case Property::LeftPercent:
		if (n->style.pos_offset_percent[3] != value || n->style.pos_offsets[3] != kUnset) {
			n->style.pos_offset_percent[3] = value;
			n->style.pos_offsets[3] = kUnset;
			changed = 1;
		}
		break;
	case Property::ZIndex: {
		const bool automatic = value == kZIndexAuto;
		const int level = automatic ? 0 : std::clamp(value, -32768, 32767);
		if (n->style.z_index != level || n->style.z_index_auto != automatic) {
			n->style.z_index = level; n->style.z_index_auto = automatic; changed = 1;
		}
		break;
	}
	case Property::BackgroundColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.bg_color != next) { n->style.bg_color = next; changed = 1; }
		if (n->style.bg_alpha != 255) { n->style.bg_alpha = 255; changed = 1; }
		break;
	}
	case Property::BackgroundAlpha: {
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (n->style.bg_alpha != alpha) { n->style.bg_alpha = alpha; changed = 1; }
		break;
	}
	case Property::BackgroundClip: if (rstyle(n->style).bg_clip != value) { rstyleMut(n->style).bg_clip = value; changed = 1; } break;
	case Property::BackgroundSizeList: if (rstyle(n->style).bg_size_list != value) { rstyleMut(n->style).bg_size_list = value; changed = 1; } break;
	case Property::BackgroundPositionList: if (rstyle(n->style).bg_position_list != value) { rstyleMut(n->style).bg_position_list = value; changed = 1; } break;
	case Property::BackgroundRepeatList: if (rstyle(n->style).bg_repeat_list != value) { rstyleMut(n->style).bg_repeat_list = value; changed = 1; } break;
	case Property::BackgroundAttachmentList: if (rstyle(n->style).bg_attachment_list != value) { rstyleMut(n->style).bg_attachment_list = value; changed = 1; } break;
	case Property::BackgroundOriginList: if (rstyle(n->style).bg_origin_list != value) { rstyleMut(n->style).bg_origin_list = value; changed = 1; } break;

	case Property::BackgroundImage:
		changed = StyleValues::applyBackgroundImage(n->style, value, node);
		break;
	case Property::HasBackground:           if (n->style.has_bg != value) { n->style.has_bg = value; changed = 1; } break;
	case Property::ActiveBackgroundColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.active_bg_color != next) { n->style.active_bg_color = next; changed = 1; }
		break;
	}
	case Property::HasActiveBackground:    if (n->style.has_active_bg != value) { n->style.has_active_bg = value; changed = 1; } break;
	case Property::Color: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.text_color != next) { n->style.text_color = next; changed = 1; }
		if (n->style.text_alpha != 255) { n->style.text_alpha = 255; changed = 1; }
		break;
	}
	case Property::Opacity: {
		uint8_t next = (uint8_t)value;
		if (n->style.opacity != next) { prevOpacity = n->style.opacity; n->style.opacity = next; changed = 1; }
		break;
	}
	case Property::BlinkInterval:
		value = value > 0 ? value : 0;
		if (n->style.blink_interval_ms != value) {
			n->style.blink_interval_ms = value;
			n->style.blink_started_ms = state.lastFrameMs;
			n->style.blink_visible = 1;
			changed = 1;
		}
		break;
	case Property::ColorAlpha: {
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (n->style.text_alpha != alpha) { n->style.text_alpha = alpha; changed = 1; }
		break;
	}
	case Property::BorderAlpha: {
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (n->style.border_alpha != alpha) { n->style.border_alpha = alpha; changed = 1; }
		break;
	}
	case Property::BorderTopAlpha:
	case Property::BorderRightAlpha:
	case Property::BorderBottomAlpha:
	case Property::BorderLeftAlpha:
	{
		const int side = static_cast<int>(prop) - static_cast<int>(Property::BorderTopAlpha);
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (rstyle(n->style).border_side_alpha[side] != alpha) { rstyleMut(n->style).border_side_alpha[side] = alpha; changed = 1; }
		break;
	}
	case Property::BorderColorCurrent: changed |= setBorderColorBinding(n->style, -1, value != 0); break;
	case Property::BorderTopColorCurrent:
	case Property::BorderRightColorCurrent:
	case Property::BorderBottomColorCurrent:
	case Property::BorderLeftColorCurrent:
		changed |= setBorderColorBinding(n->style, static_cast<int>(prop) - static_cast<int>(Property::BorderTopColorCurrent), value != 0);
		break;
	case Property::BorderWidth: changed |= setComputedBorderWidth(n->style, -1, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
	case Property::BorderColor: {
		changed |= setBorderColorBinding(n->style, -1, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.border_color != next) { n->style.border_color = next; changed = 1; }
		if (n->style.border_alpha != 255) { n->style.border_alpha = 255; changed = 1; }
		break;
	}
	case Property::BorderTopWidth: changed |= setComputedBorderWidth(n->style, 0, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
	case Property::BorderRelief:
	case Property::BorderTopRelief:
	case Property::BorderRightRelief:
	case Property::BorderBottomRelief:
	case Property::BorderLeftRelief:
		for (int side = 0; side < 4; ++side) {
			if (prop != Property::BorderRelief && side != static_cast<int>(prop) - static_cast<int>(Property::BorderTopRelief)) continue;
			if (rstyle(n->style).border_relief[side] != value) {
				rstyleMut(n->style).border_relief[side] = static_cast<uint8_t>(value);
				changed = 1;
			}
		}
		break;
	case Property::BorderRightWidth: changed |= setComputedBorderWidth(n->style, 1, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
	case Property::BorderBottomWidth: changed |= setComputedBorderWidth(n->style, 2, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
	case Property::BorderLeftWidth: changed |= setComputedBorderWidth(n->style, 3, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
	case Property::BorderTopColor: {
		changed |= setBorderColorBinding(n->style, 0, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[0] != next) { rstyleMut(n->style).border_side_color[0] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[0] != 255) { rstyleMut(n->style).border_side_alpha[0] = 255; changed = 1; }
		break;
	}
	case Property::BorderRightColor: {
		changed |= setBorderColorBinding(n->style, 1, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[1] != next) { rstyleMut(n->style).border_side_color[1] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[1] != 255) { rstyleMut(n->style).border_side_alpha[1] = 255; changed = 1; }
		break;
	}
	case Property::BorderBottomColor: {
		changed |= setBorderColorBinding(n->style, 2, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[2] != next) { rstyleMut(n->style).border_side_color[2] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[2] != 255) { rstyleMut(n->style).border_side_alpha[2] = 255; changed = 1; }
		break;
	}
	case Property::BorderLeftColor: {
		changed |= setBorderColorBinding(n->style, 3, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).border_side_color[3] != next) { rstyleMut(n->style).border_side_color[3] = next; changed = 1; }
		if (rstyle(n->style).border_side_alpha[3] != 255) { rstyleMut(n->style).border_side_alpha[3] = 255; changed = 1; }
		break;
	}
	case Property::BorderRadiusTopLeft:
		if (n->style.border_radius[0] != value || n->style.border_radius_percent[0] != kUnset) {
			n->style.border_radius[0] = value;
			n->style.border_radius_percent[0] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRight:
		if (n->style.border_radius[1] != value || n->style.border_radius_percent[1] != kUnset) {
			n->style.border_radius[1] = value;
			n->style.border_radius_percent[1] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRight:
		if (n->style.border_radius[2] != value || n->style.border_radius_percent[2] != kUnset) {
			n->style.border_radius[2] = value;
			n->style.border_radius_percent[2] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeft:
		if (n->style.border_radius[3] != value || n->style.border_radius_percent[3] != kUnset) {
			n->style.border_radius[3] = value;
			n->style.border_radius_percent[3] = kUnset;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopLeftPercent:
		if (n->style.border_radius_percent[0] != value || n->style.border_radius[0] != 0) {
			n->style.border_radius_percent[0] = value;
			n->style.border_radius[0] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRightPercent:
		if (n->style.border_radius_percent[1] != value || n->style.border_radius[1] != 0) {
			n->style.border_radius_percent[1] = value;
			n->style.border_radius[1] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRightPercent:
		if (n->style.border_radius_percent[2] != value || n->style.border_radius[2] != 0) {
			n->style.border_radius_percent[2] = value;
			n->style.border_radius[2] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeftPercent:
		if (n->style.border_radius_percent[3] != value || n->style.border_radius[3] != 0) {
			n->style.border_radius_percent[3] = value;
			n->style.border_radius[3] = 0;
			changed = 1;
		}
		break;
	case Property::FontId:          if (n->style.font_id != value) { n->style.font_id = value; changed = 1; } break;
	case Property::FontSize:        if (n->style.font_size != value) { n->style.font_size = value; changed = 1; } break;
	case Property::FontWeight:      if (n->style.font_weight != value) { n->style.font_weight = value; changed = 1; } break;
	case Property::LineHeight:
		if (n->style.line_height_multiplier >= 0) { n->style.line_height_multiplier = -1; changed = 1; }
		if (rstyle(n->style).line_height_expression >= 0) { rstyleMut(n->style).line_height_expression = -1; changed = 1; }
		if (n->style.line_height != value) { n->style.line_height = value; changed = 1; } break;
	case Property::LineHeightExpression: {
		if (n->style.line_height_multiplier >= 0) { n->style.line_height_multiplier = -1; changed = 1; }
		const int height = resolveLineHeightExpression(node, value);
		if (rstyle(n->style).line_height_expression != value || n->style.line_height != height) {
			rstyleMut(n->style).line_height_expression = value; n->style.line_height = height; changed = 1;
		} break;
	}
	case Property::LineHeightMultiplier: {
		const int height = resolveLineHeightMultiplier(node, value);
		if (rstyle(n->style).line_height_expression >= 0) { rstyleMut(n->style).line_height_expression = -1; changed = 1; }
		if (n->style.line_height_multiplier != value || n->style.line_height != height) {
			n->style.line_height_multiplier = value; n->style.line_height = height; changed = 1;
		} break;
	}
	case Property::TextAlign:       if (n->style.text_align != value) { n->style.text_align = value; changed = 1; } break;
	case Property::TextDecoration:  if (n->style.text_decoration != value) { n->style.text_decoration = value; changed = 1; } break;
	case Property::TextTransform:   if (n->style.text_transform != value) { n->style.text_transform = value; changed = 1; } break;
	case Property::WhiteSpace:      if (n->style.white_space != static_cast<int8_t>(value)) { n->style.white_space = static_cast<int8_t>(value); changed = 1; } break;
	case Property::TextOverflow:    if (n->style.text_overflow != static_cast<int8_t>(value)) { n->style.text_overflow = static_cast<int8_t>(value); changed = 1; } break;
	case Property::TransformStyle: if (rstyle(n->style).transform_preserve_3d != (value != 0)) { rstyleMut(n->style).transform_preserve_3d = value != 0; changed = 1; } break;
	case Property::Visibility: if (n->style.visibility != static_cast<int8_t>(value)) { n->style.visibility = static_cast<int8_t>(value); changed = 1; } break;
	case Property::Backface:        if (n->style.backface_hidden != static_cast<int8_t>(value)) { n->style.backface_hidden = static_cast<int8_t>(value); changed = 1; } break;
	case Property::PointerEvents:   if (n->style.pointer_events != static_cast<int8_t>(value)) { n->style.pointer_events = static_cast<int8_t>(value); changed = 1; } break;
	case Property::Overflow: {
		const int8_t next = static_cast<int8_t>(value);
		if (n->style.overflow != next || n->style.overflow_x != next || n->style.overflow_y != next) {
			n->style.overflow = next;
			n->style.overflow_x = next;
			n->style.overflow_y = next;
			changed = 1;
		}
		break;
	}
	case Property::OverflowX: {
		const int8_t next = static_cast<int8_t>(value);
		const int8_t aggregate = aggregateOverflow(next, n->style.overflow_y);
		if (n->style.overflow_x != next || n->style.overflow != aggregate) {
			n->style.overflow_x = next;
			n->style.overflow = aggregate;
			changed = 1;
		}
		break;
	}
	case Property::OverflowY: {
		const int8_t next = static_cast<int8_t>(value);
		const int8_t aggregate = aggregateOverflow(n->style.overflow_x, next);
		if (n->style.overflow_y != next || n->style.overflow != aggregate) {
			n->style.overflow_y = next;
			n->style.overflow = aggregate;
			changed = 1;
		}
		break;
	}
	case Property::MaskRightFadeWidth: {
		const int16_t next = static_cast<int16_t>(value < 0 ? 0 : value > 32767 ? 32767 : value);
		if (n->style.mask_right_fade_width != next) {
			n->style.mask_right_fade_width = next;
			changed = 1;
		}
		break;
	}
	case Property::ImageId:         if (n->image_id != value) { n->image_id = value; changed = 1; } break;
	case Property::ImageFit:        if (n->style.image_fit != value) { n->style.image_fit = value; changed = 1; } break;
	case Property::TransformTranslateOuterAxes: if (rstyle(n->style).transform_translate_outer_axes != value) { rstyleMut(n->style).transform_translate_outer_axes = value; changed = 1; } break;
	case Property::RotateAngle: if (rstyle(n->style).rotate_angle != value) { rstyleMut(n->style).rotate_angle = value; changed = 1; } break;
	case Property::RotateAxisX: if (rstyle(n->style).rotate_axis_x != value) { rstyleMut(n->style).rotate_axis_x = value; changed = 1; } break;
	case Property::RotateAxisY: if (rstyle(n->style).rotate_axis_y != value) { rstyleMut(n->style).rotate_axis_y = value; changed = 1; } break;
	case Property::RotateAxisZ: if (rstyle(n->style).rotate_axis_z != value) { rstyleMut(n->style).rotate_axis_z = value; changed = 1; } break;
	case Property::ScaleX: if (rstyle(n->style).scale_x != value) { rstyleMut(n->style).scale_x = value; changed = 1; } break;
	case Property::ScaleY: if (rstyle(n->style).scale_y != value) { rstyleMut(n->style).scale_y = value; changed = 1; } break;
	case Property::ScaleZ: if (rstyle(n->style).scale_z != value) { rstyleMut(n->style).scale_z = value; changed = 1; } break;
	case Property::TranslatePresent: if (rstyle(n->style).translate_present != (value != 0)) { rstyleMut(n->style).translate_present = value != 0; changed = 1; } break;
	case Property::TranslateX: if (rstyle(n->style).translate_x != value) { rstyleMut(n->style).translate_x = value; changed = 1; } break;
	case Property::TranslateY: if (rstyle(n->style).translate_y != value) { rstyleMut(n->style).translate_y = value; changed = 1; } break;
	case Property::TranslateZ: if (rstyle(n->style).translate_z != value) { rstyleMut(n->style).translate_z = value; changed = 1; } break;
	case Property::TranslateXPercent: if (rstyle(n->style).translate_x_percent != value) { rstyleMut(n->style).translate_x_percent = value; changed = 1; } break;
	case Property::TranslateYPercent: if (rstyle(n->style).translate_y_percent != value) { rstyleMut(n->style).translate_y_percent = value; changed = 1; } break;
	case Property::TransformPresent: if (rstyle(n->style).transform_present != (value != 0)) { rstyleMut(n->style).transform_present = value != 0; changed = 1; } break;
	case Property::RotatePresent: if (rstyle(n->style).rotate_present != (value != 0)) { rstyleMut(n->style).rotate_present = value != 0; changed = 1; } break;
	case Property::ScalePresent: if (rstyle(n->style).scale_present != (value != 0)) { rstyleMut(n->style).scale_present = value != 0; changed = 1; } break;
	case Property::FilterPresent: if (rstyle(n->style).filter_present != (value != 0)) { rstyleMut(n->style).filter_present = value != 0; changed = 1; } break;
	case Property::TransformRotate:
		if (rstyle(n->style).transform_rotate != value) {
			rstyleMut(n->style).transform_rotate = value;
			changed = 1;
		}
		break;
	case Property::TransformRotateX: if (rstyle(n->style).transform_rotate_x != value) { rstyleMut(n->style).transform_rotate_x = value; changed = 1; } break;
	case Property::TransformRotateY: if (rstyle(n->style).transform_rotate_y != value) { rstyleMut(n->style).transform_rotate_y = value; changed = 1; } break;
	case Property::TransformTranslateX: if (rstyle(n->style).transform_translate_x != value) { rstyleMut(n->style).transform_translate_x = value; changed = 1; } break;
	case Property::TransformTranslateY: if (rstyle(n->style).transform_translate_y != value) { rstyleMut(n->style).transform_translate_y = value; changed = 1; } break;
	case Property::TransformTranslateZ: if (rstyle(n->style).transform_translate_z != value) { rstyleMut(n->style).transform_translate_z = value; changed = 1; } break;
	case Property::TransformTranslateXPercent: if (rstyle(n->style).transform_translate_x_percent != value) { rstyleMut(n->style).transform_translate_x_percent = value; changed = 1; } break;
	case Property::TransformTranslateYPercent: if (rstyle(n->style).transform_translate_y_percent != value) { rstyleMut(n->style).transform_translate_y_percent = value; changed = 1; } break;
	case Property::TransformScaleX: if (rstyle(n->style).transform_scale_x != value) { rstyleMut(n->style).transform_scale_x = value; changed = 1; } break;
	case Property::TransformScaleY: if (rstyle(n->style).transform_scale_y != value) { rstyleMut(n->style).transform_scale_y = value; changed = 1; } break;
	case Property::TransformScaleZ: if (rstyle(n->style).transform_scale_z != value) { rstyleMut(n->style).transform_scale_z = value; changed = 1; } break;
	case Property::TransformOriginX: if (rstyle(n->style).transform_origin_x != value) { rstyleMut(n->style).transform_origin_x = value; changed = 1; } break;
	case Property::TransformOriginY: if (rstyle(n->style).transform_origin_y != value) { rstyleMut(n->style).transform_origin_y = value; changed = 1; } break;
	case Property::Perspective: if (rstyle(n->style).perspective != value) { rstyleMut(n->style).perspective = value; changed = 1; } break;
	case Property::PerspectiveOriginX: if (rstyle(n->style).perspective_origin_x != value) { rstyleMut(n->style).perspective_origin_x = value; changed = 1; } break;
	case Property::PerspectiveOriginY: if (rstyle(n->style).perspective_origin_y != value) { rstyleMut(n->style).perspective_origin_y = value; changed = 1; } break;
	case Property::FilterBlur: if (rstyle(n->style).filter_blur_radius != value) { rstyleMut(n->style).filter_blur_radius = value; changed = 1; } break;
	case Property::BoxShadowInset: {
		const uint8_t next = value != 0 ? 1 : 0;
		if (rstyle(n->style).box_shadow_inset != next) { rstyleMut(n->style).box_shadow_inset = next; changed = 1; }
		break;
	}
	case Property::BoxShadowOffsetX: if (rstyle(n->style).box_shadow_offset_x != value) { rstyleMut(n->style).box_shadow_offset_x = value; changed = 1; } break;
	case Property::BoxShadowOffsetY: if (rstyle(n->style).box_shadow_offset_y != value) { rstyleMut(n->style).box_shadow_offset_y = value; changed = 1; } break;
	case Property::BoxShadowBlur: if (rstyle(n->style).box_shadow_blur_radius != value) { rstyleMut(n->style).box_shadow_blur_radius = value; changed = 1; } break;
	case Property::BoxShadowSpread: if (rstyle(n->style).box_shadow_spread != value) { rstyleMut(n->style).box_shadow_spread = value; changed = 1; } break;
	case Property::BoxShadowColor: {
		const style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).box_shadow_color != next) { rstyleMut(n->style).box_shadow_color = next; changed = 1; }
		break;
	}
	case Property::BoxShadowAlpha: {
		const uint8_t next = static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		if (rstyle(n->style).box_shadow_alpha != next) { rstyleMut(n->style).box_shadow_alpha = next; changed = 1; }
		break;
	}
	default:
		perf.treeSetStyleNoop++;
		return;
	}
	if (!changed) {
		perf.treeSetStyleNoop++;
		return;
	}
	perf.treeSetStyleChanged++;
	if (isLayoutProperty(prop) || (isTransformProperty(prop) && state.fixedPositionUsed))
		perf.treeSetStyleLayoutChanged++;
	if (isTransformProperty(prop))
		perf.treeSetStyleTransformChanged++;
	else if (isPaintProperty(prop))
		perf.treeSetStylePaintChanged++;
	if (state.styleInvalidationSuppressionDepth > 0) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;
	if (prop == Property::Visibility || prop == Property::Backface || prop == Property::TransformStyle)
		tree.markDisplayListDirty();
	const int documentRoot = state.mountedRoot;
	if (documentRoot >= 0 && documentRoot < state.nodeCount && isDocumentCanvasRoot(state.nodes[documentRoot]) &&
	    (node == documentRoot || (n->parent == documentRoot && std::string_view(tagFromId(n->tag_id)) == "body")))
		tree.markDisplayListDirty();
	// An ImageId (src) swap on an element with a FIXED CSS display box (explicit
	// width+height — e.g. a forecast row's 24x20 icon re-pointed at a different icon
	// every data refresh) is PAINT-ONLY: the layout box is the styled box regardless
	// of the new image's intrinsic size, so no reflow and no sibling movement. Patch
	// just this node's BlitImage command in place and repaint its box; DON'T set
	// layout_dirty or displayListDirty (ImageId is otherwise treated as a layout
	// property, which would force a full relayout + full display-list rebuild + a
	// full-screen-coalesced re-raster — measured as the bulk of a ~350ms weather city
	// switch's 18 icon swaps). An auto-sized <img> still falls through to the full
	// path below, since its intrinsic size can move layout.
	if (prop == Property::ImageId) {
		const bool fixedBox =
			(n->style.width != kUnset || n->style.width_percent != kUnset) &&
			(n->style.height != kUnset || n->style.height_percent != kUnset);
		if (fixedBox) {
			n->render.dirty = 1;
			n->render.non_scroll_dirty = 1;
			tree.markNodeDisplayCommandsDirty(node);
			return;
		}
	}
	if (prop == Property::BackgroundColor &&
	    previousHasBg &&
	    previousBgAlpha == 255 &&
	    previousBgFill == 0 &&
	    previousBgGradientHasMid == 0 &&
	    previousBgOverlayGradient == 0 &&
	    previousBgRadialGradient == 0 &&
	    previousBgGridAxes == 0 &&
	    n->style.has_bg &&
	    n->style.bg_alpha == 255 &&
	    n->style.bg_fill == 0 &&
	    rstyle(n->style).bg_gradient_has_mid == 0 &&
	    rstyle(n->style).bg_overlay_gradient == 0 &&
	    rstyle(n->style).bg_radial_gradient == 0 &&
	    rstyle(n->style).bg_grid_axes == 0 &&
	    previousBgColor != n->style.bg_color) {
		n->render.bg_recolor_pending = 1;
		n->render.bg_recolor_from = previousBgColor;
		n->render.bg_recolor_to = n->style.bg_color;
	}
	n->render.dirty = 1;
	// Paint-only properties (background, colors, shadows, ...) repaint in
	// place: geometry is untouched, so they don't demand a relayout pass.
	// Keep geometry dirt separate from transform dirt: coalescing both must
	// not let a retained transform update suppress the required relayout.
	if (isLayoutProperty(prop) || (isTransformProperty(prop) && state.fixedPositionUsed))
		n->render.layout_dirty = 1;
	n->render.non_scroll_dirty = 1;
	if (isTransformProperty(prop)) {
		n->render.transform_dirty = 1;
		state.transformScanSerial = ~0ull;
		state.transformScanValid = false;  // a transform was added/changed → drop durable no-transform cache
	}
	if (DisplayInvalidation::rebuildsNodeDisplayCommands(prop)) {
		const bool stayLocal = DisplayInvalidation::nodeDisplayChangeCanStayLocal(node);
		bool forceFull = false;
		if (prop == Property::Display) {
			forceFull = !stayLocal;
		} else if (prop == Property::Opacity) {
			// Partial→partial fade: the SetAlpha scope already exists on both
			// sides (opacity 0 still records a scope), so patch its value in place
			// and keep the display list — no rebuild, no displayListDirty. The
			// full per-rect replay path (tree_render) replays the scope in order,
			// so the patched alpha applies correctly. (The incremental
			// direct-replay path can't use this: it replays only a node's
			// [drawStart,drawEnd], which excludes the scope — which is why
			// canReplayDirectDirtyRegions still bails and we fall to the safe
			// full-per-rect replay. The record stays incremental either way.)
			if (stayLocal && prevOpacity < 255 && n->style.opacity < 255 &&
			    DisplayList::instance().patchNodeAlpha(node, static_cast<uint8_t>(n->style.opacity)))
				return;
			// Crossing the 255 boundary (scope appears/disappears) or a non-leaf:
			// a leaf toggling strictly 0↔255 re-records locally; otherwise rebuild.
			const bool toggleOnly = (prevOpacity == 0 || prevOpacity == 255) &&
			                        (n->style.opacity == 0 || n->style.opacity == 255);
			forceFull = !stayLocal || !toggleOnly;
		} else if (prop == Property::MaskRightFadeWidth ||
		           prop == Property::FilterBlur ||
		           prop == Property::BoxShadowInset ||
		           prop == Property::BoxShadowOffsetX ||
		           prop == Property::BoxShadowOffsetY ||
		           prop == Property::BoxShadowBlur ||
		           prop == Property::BoxShadowSpread ||
		           prop == Property::BoxShadowColor ||
		           prop == Property::BoxShadowAlpha) {
			forceFull = true;
		}
		if (forceFull) {
			// A partial-opacity change only alters this node's own pixels — no
			// reflow, no draw-order change — so the rebuilt list can be replayed
			// over just the dirty-node region (content-dirty skips the
			// full-viewport repaint). Display toggles (visibility/order) and
			// filter/box-shadow (pixel spillover past the node box) keep the
			// conservative full repaint.
			if (prop == Property::Opacity)
				tree.markDisplayListContentDirty();
			else
				tree.markDisplayListDirty();
		} else {
			tree.markNodeDisplayCommandsDirty(node);
		}
	} else if (!DisplayInvalidation::retainsDisplayCommands(prop)) {
		// An absolute, childless leaf changing size is out of flow: no sibling
		// reflow, no draw-order change. Mark only this node's commands dirty (so
		// it re-records at the new size) and let AbsoluteLeafRefresh reconcile
		// layout.{width,height} from style — no displayListDirty, so the display
		// list stays keepable and only this node re-records (the static text
		// labels are NOT re-recorded). Every other size/layout/z-index change
		// reflows or reorders → full rebuild.
		const bool absLeafSize = (prop == Property::Width || prop == Property::Height) &&
		                         n->style.position == 1 && n->first_child < 0 && n->type != NodeType::Text;
		if (absLeafSize)
			tree.markNodeDisplayCommandsDirty(node);
		else
			tree.markDisplayListDirty();
	}
}

}  // namespace

void Tree::setStyle(int node, Property prop, int value)
{
	setStyleValue(*this, node, prop, value, true);
}

void Tree::setDefaultStyle(int node, Property prop, int value)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	ensureRareData(node).defaultStyles.set(prop, value);
	StyleSheet::instance().recomputeSubtree(node);
}

void Tree::setStyleFromClass(int node, Property prop, int value)
{
	setStyleValue(*this, node, prop, value, false);
}

void Tree::resetStyleForClassRecompute(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;

	Node defaults;
	NodeLifecycle::init(&defaults, state.nodes[node].type);
	state.nodes[node].style = defaults.style;
	state.nodes[node].image_id = defaults.image_id;
	if (state.styleInvalidationSuppressionDepth > 0) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;
	state.nodes[node].render.dirty = 1;
	state.nodes[node].render.layout_dirty = 1;
	state.nodes[node].render.non_scroll_dirty = 1;
	markDisplayListDirty();
}

}  // namespace gea::embedded::ui
