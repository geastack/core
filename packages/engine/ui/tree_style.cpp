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
#if GEA_CSS_TRANSFORMS
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
#endif
	default:
		return false;
	}
}

bool isLayoutProperty(Property prop)
{
	switch (prop) {
	case Property::FlexLineCount:
	case Property::AspectRatio:
#if GEA_CSS_TRANSFORMS
	case Property::TranslatePresent:
	case Property::TransformPresent:
	case Property::RotatePresent:
	case Property::ScalePresent:
#endif
	case Property::FilterPresent:
	case Property::Display:
	case Property::FlexDirection:
	case Property::BoxSizing:
#if GEA_CSS_FLOATS
	case Property::Float:
#endif
	case Property::MarginTrim:
	case Property::MaxLines:
	case Property::LineClampContinue:
	case Property::BlockEllipsis:
	case Property::ColumnCountSet:
	case Property::ColumnWidthSet:
	case Property::BlockEllipsisString:
	case Property::ColumnCount:
	case Property::ColumnWidth:
	case Property::ColumnFillAuto:
	case Property::LineClampDiscard:
	case Property::ColumnSpanAll:
	case Property::VerticalAlign:
#if GEA_CSS_FLOATS
	case Property::Clear:
#endif
#if GEA_CSS_WRITING_MODE
	case Property::WritingMode:
	case Property::Direction:
#endif
#if GEA_CSS_AXIS_GAP
	case Property::RowGap:
#endif
#if GEA_CSS_AXIS_GAP
	case Property::ColumnGap:
#endif
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
#if GEA_CSS_GRID
	case Property::GridRowStart:
	case Property::GridColumnStart:
	case Property::GridRowEnd:
	case Property::GridColumnEnd:
#endif
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
#if GEA_UI_IMAGE_NODES
	case Property::ImageId:
#endif
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
		if (prop == Property::Gap)
			for (Property companion : {Property::RowGap, Property::ColumnGap, Property::RowGapPercent, Property::ColumnGapPercent}) overrides.remove(companion);
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
#if GEA_CSS_FLOATS
	case Property::Float: if (n->style.float_side != value) { n->style.float_side = value; changed = 1; } break;
#endif
#if GEA_CSS_ASPECT_RATIO
	case Property::AspectRatio: if ((GEA_CSS_ASPECT_RATIO ? rstyle(n->style).aspect_ratio : 0) != value) { rstyleMut(n->style).aspect_ratio = value; changed = 1; } break;
#endif
#if GEA_CSS_CONTAINMENT
	case Property::Containment: if ((GEA_CSS_CONTAINMENT ? rstyle(n->style).containment : 0) != value) { rstyleMut(n->style).containment = value; changed = 1; } break;
#endif
#if GEA_CSS_FLEX_LINE_COUNT
	case Property::FlexLineCount: if ((GEA_CSS_FLEX_LINE_COUNT ? rstyle(n->style).flex_line_count : 1) != value) { rstyleMut(n->style).flex_line_count = value; changed = 1; } break;
#endif
#if GEA_CSS_MARGIN_TRIM
	case Property::MarginTrim: if ((GEA_CSS_MARGIN_TRIM ? rstyle(n->style).margin_trim : 0) != value) { rstyleMut(n->style).margin_trim = value; changed = 1; } break;
#endif
	case Property::MaxLines: if (rstyle(n->style).max_lines != value) { rstyleMut(n->style).max_lines = value; changed = 1; } break;
	case Property::BlockEllipsisString: if (rstyle(n->style).block_ellipsis != value) { rstyleMut(n->style).block_ellipsis = value; changed = 1; } break;
	case Property::ColumnCount: if (rstyle(n->style).column_count != value) { rstyleMut(n->style).column_count = value; changed = 1; } break;
	case Property::ColumnWidth: if (rstyle(n->style).column_width != value) { rstyleMut(n->style).column_width = value; changed = 1; } break;
	case Property::LineClampContinue:
	case Property::BlockEllipsis:
	case Property::ColumnCountSet:
	case Property::ColumnWidthSet:
	case Property::ColumnFillAuto:
	case Property::LineClampDiscard:
	case Property::ColumnSpanAll: {
		const int bit = lineClampFlagBit(prop);
		const int next = value ? rstyle(n->style).line_clamp_flags | bit : rstyle(n->style).line_clamp_flags & ~bit;
		if (rstyle(n->style).line_clamp_flags != next) { rstyleMut(n->style).line_clamp_flags = next; changed = 1; }
		break;
	}
#if GEA_CSS_FLOATS
	case Property::Clear: if (n->style.clear_side != value) { n->style.clear_side = value; changed = 1; } break;
#endif
#if GEA_CSS_WRITING_MODE
	case Property::Direction: if (n->style.direction != value) { n->style.direction = value; changed = 1; } break;
	case Property::WritingMode: if (n->style.writing_mode != value) { n->style.writing_mode = value; changed = 1; } break;
#endif
#if GEA_CSS_AXIS_GAP
	case Property::RowGap: if (n->style.row_gap != value) { n->style.row_gap = value; changed = 1; } break;
#endif
#if GEA_CSS_AXIS_GAP
	case Property::ColumnGap: if (n->style.column_gap != value) { n->style.column_gap = value; changed = 1; } break;
#endif
#if GEA_CSS_PERCENT_GAP
	case Property::RowGapPercent: if (n->style.row_gap_percent != value) { n->style.row_gap_percent = value; changed = 1; } break;
	case Property::ColumnGapPercent: if (n->style.column_gap_percent != value) { n->style.column_gap_percent = value; changed = 1; } break;
#endif
	case Property::MarginTopAuto: { const auto mask = (n->style.margin_auto & ~1) | (value ? 1 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginRightAuto: { const auto mask = (n->style.margin_auto & ~2) | (value ? 2 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginBottomAuto: { const auto mask = (n->style.margin_auto & ~4) | (value ? 4 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
	case Property::MarginLeftAuto: { const auto mask = (n->style.margin_auto & ~8) | (value ? 8 : 0); if (mask != n->style.margin_auto) { n->style.margin_auto = mask; changed = 1; } break; }
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginTopExpression: if (rstyle(n->style).margin_expression[0] != value) { rstyleMut(n->style).margin_expression[0] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginRightExpression: if (rstyle(n->style).margin_expression[1] != value) { rstyleMut(n->style).margin_expression[1] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginBottomExpression: if (rstyle(n->style).margin_expression[2] != value) { rstyleMut(n->style).margin_expression[2] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginLeftExpression: if (rstyle(n->style).margin_expression[3] != value) { rstyleMut(n->style).margin_expression[3] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingTopExpression: if (rstyle(n->style).padding_expression[0] != value) { rstyleMut(n->style).padding_expression[0] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingRightExpression: if (rstyle(n->style).padding_expression[1] != value) { rstyleMut(n->style).padding_expression[1] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingBottomExpression: if (rstyle(n->style).padding_expression[2] != value) { rstyleMut(n->style).padding_expression[2] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingLeftExpression: if (rstyle(n->style).padding_expression[3] != value) { rstyleMut(n->style).padding_expression[3] = value; changed = 1; } break;
#endif
	case Property::WidthExpression: if (n->style.width_expression != value) { n->style.width_expression = value; n->style.width = n->style.width_percent = kUnset; changed = 1; } break;
#if GEA_CSS_HEIGHT_EXPRESSIONS
	case Property::HeightExpression: if (n->style.height_expression != value) { n->style.height_expression = value; n->style.height = n->style.height_percent = kUnset; changed = 1; } break;
#endif
#if GEA_CSS_ORDER
	case Property::Order:           if (n->style.order != value) { n->style.order = value; changed = 1; } break;
#endif
#if GEA_CSS_FLEX_WRAP
	case Property::FlexWrap:        if (n->style.flex_wrap != value) { n->style.flex_wrap = value; changed = 1; } break;
#endif
	case Property::JustifyContent:  if (n->style.justify_content != value) { n->style.justify_content = value; changed = 1; } break;
	case Property::AlignItems:      if (n->style.align_items != value) { n->style.align_items = value; changed = 1; } break;
#if GEA_CSS_JUSTIFY_ITEMS
	case Property::JustifyItems:    if (n->style.justify_items != value) { n->style.justify_items = value; changed = 1; } break;
#endif
#if GEA_CSS_ALIGN_CONTENT
	case Property::AlignContent:    if (n->style.align_content != value) { n->style.align_content = value; changed = 1; } break;
#endif
#if GEA_CSS_ALIGN_SELF
	case Property::AlignSelf:       if (n->style.align_self != value) { n->style.align_self = value; changed = 1; } break;
#endif
#if GEA_CSS_JUSTIFY_SELF
	case Property::JustifySelf:     if ((GEA_CSS_JUSTIFY_SELF ? rstyle(n->style).justify_self : -1) != value) { rstyleMut(n->style).justify_self = value; changed = 1; } break;
#endif
#if GEA_CSS_GRID
	case Property::GridRowStart: if (rstyle(n->style).grid_line[0] != value) { rstyleMut(n->style).grid_line[0] = value; changed = 1; } break;
	case Property::GridColumnStart: if (rstyle(n->style).grid_line[1] != value) { rstyleMut(n->style).grid_line[1] = value; changed = 1; } break;
	case Property::GridRowEnd: if (rstyle(n->style).grid_line[2] != value) { rstyleMut(n->style).grid_line[2] = value; changed = 1; } break;
	case Property::GridColumnEnd: if (rstyle(n->style).grid_line[3] != value) { rstyleMut(n->style).grid_line[3] = value; changed = 1; } break;
#endif
	case Property::Gap:
		if (n->style.gap != value || n->style.row_gap != kUnset || n->style.column_gap != kUnset || n->style.row_gap_percent != kUnset || n->style.column_gap_percent != kUnset) {
			n->style.gap = value;
#if GEA_CSS_AXIS_GAP
	n->style.row_gap = n->style.column_gap = kUnset;
#endif
#if GEA_CSS_PERCENT_GAP
			n->style.row_gap_percent = n->style.column_gap_percent = kUnset;
#endif
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
#if GEA_CSS_HEIGHT_EXPRESSIONS
		if (n->style.height_expression != -1) { n->style.height_expression = -1; changed = 1; }
#endif
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
#if GEA_CSS_HEIGHT_EXPRESSIONS
		if (n->style.height_expression != -1) { n->style.height_expression = -1; changed = 1; }
#endif
		if (n->style.height_percent != value || n->style.height != kUnset) {
			n->style.height_percent = value;
			n->style.height = kUnset;
			changed = 1;
		}
		break;
#if GEA_CSS_MIN_WIDTH
	case Property::MinWidth:        if (n->style.min_width != value) { n->style.min_width = value; changed = 1; } break;
#endif
	case Property::MinHeight:       if (n->style.min_height != value) { n->style.min_height = value; changed = 1; } break;
	case Property::MaxWidth:        if (n->style.max_width != value) { n->style.max_width = value; changed = 1; } break;
#if GEA_CSS_MAX_HEIGHT
	case Property::MaxHeight:       if (n->style.max_height != value) { n->style.max_height = value; changed = 1; } break;
#endif
	case Property::Flex:             if (n->style.flex != value) { n->style.flex = value; changed = 1; } break;
	case Property::FlexShrink:       if (n->style.flex_shrink != value) { n->style.flex_shrink = value; changed = 1; } break;
	case Property::FlexBasis:
#if GEA_CSS_FLEX_BASIS_EXPRESSIONS
		if (rstyle(n->style).flex_basis_expression >= 0) { rstyleMut(n->style).flex_basis_expression = -1; changed = 1; }
#endif
#if GEA_CSS_FLEX_BASIS
		if (n->style.flex_basis != value) { n->style.flex_basis = value; changed = 1; }
#endif
		break;
#if GEA_CSS_FLEX_BASIS_EXPRESSIONS
	case Property::FlexBasisExpression:
		if (rstyle(n->style).flex_basis_expression != value || n->style.flex_basis != kUnset) {
			rstyleMut(n->style).flex_basis_expression = value;
#if GEA_CSS_FLEX_BASIS
			n->style.flex_basis = kUnset;
#endif
			changed = 1;
		}
		break;
#endif
	case Property::PaddingTop:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).padding_expression[0] >= 0) { rstyleMut(n->style).padding_expression[0] = -1; changed = 1; }
#endif
		if (n->style.padding[0] != value) { n->style.padding[0] = value; changed = 1; } break;
	case Property::PaddingRight:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).padding_expression[1] >= 0) { rstyleMut(n->style).padding_expression[1] = -1; changed = 1; }
#endif
		if (n->style.padding[1] != value) { n->style.padding[1] = value; changed = 1; } break;
	case Property::PaddingBottom:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).padding_expression[2] >= 0) { rstyleMut(n->style).padding_expression[2] = -1; changed = 1; }
#endif
		if (n->style.padding[2] != value) { n->style.padding[2] = value; changed = 1; } break;
	case Property::PaddingLeft:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).padding_expression[3] >= 0) { rstyleMut(n->style).padding_expression[3] = -1; changed = 1; }
#endif
		if (n->style.padding[3] != value) { n->style.padding[3] = value; changed = 1; } break;
	case Property::MarginTop:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).margin_expression[0] >= 0) { rstyleMut(n->style).margin_expression[0] = -1; changed = 1; }
#endif
		if (n->style.margin[0] != value || (n->style.margin_auto & 1)) { n->style.margin[0] = value; n->style.margin_auto &= ~1; changed = 1; } break;
	case Property::MarginRight:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).margin_expression[1] >= 0) { rstyleMut(n->style).margin_expression[1] = -1; changed = 1; }
#endif
		if (n->style.margin[1] != value || (n->style.margin_auto & 2)) { n->style.margin[1] = value; n->style.margin_auto &= ~2; changed = 1; } break;
	case Property::MarginBottom:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).margin_expression[2] >= 0) { rstyleMut(n->style).margin_expression[2] = -1; changed = 1; }
#endif
		if (n->style.margin[2] != value || (n->style.margin_auto & 4)) { n->style.margin[2] = value; n->style.margin_auto &= ~4; changed = 1; } break;
	case Property::MarginLeft:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(n->style).margin_expression[3] >= 0) { rstyleMut(n->style).margin_expression[3] = -1; changed = 1; }
#endif
		if (n->style.margin[3] != value || (n->style.margin_auto & 8)) { n->style.margin[3] = value; n->style.margin_auto &= ~8; changed = 1; } break;
	case Property::Position:         if (value == kPositionFixed) state.fixedPositionUsed = true; if (n->style.position != value) { n->style.position = value; changed = 1; } break;
	case Property::Top:
#if GEA_CSS_POSITION_TOP
		if (GEA_CSS_POSITION_PX_0(n->style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_0(n->style) != kUnset) {
			GEA_CSS_POSITION_PX_0(n->style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_TOP_PERCENT
			GEA_CSS_POSITION_PERCENT_0(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::Right:
#if GEA_CSS_POSITION_RIGHT
		if (GEA_CSS_POSITION_PX_1(n->style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_1(n->style) != kUnset) {
			GEA_CSS_POSITION_PX_1(n->style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_RIGHT_PERCENT
			GEA_CSS_POSITION_PERCENT_1(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::Bottom:
#if GEA_CSS_POSITION_BOTTOM
		if (GEA_CSS_POSITION_PX_2(n->style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_2(n->style) != kUnset) {
			GEA_CSS_POSITION_PX_2(n->style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_BOTTOM_PERCENT
			GEA_CSS_POSITION_PERCENT_2(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::Left:
#if GEA_CSS_POSITION_LEFT
		if (GEA_CSS_POSITION_PX_3(n->style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_3(n->style) != kUnset) {
			GEA_CSS_POSITION_PX_3(n->style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_LEFT_PERCENT
			GEA_CSS_POSITION_PERCENT_3(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::TopPercent:
#if GEA_CSS_POSITION_TOP_PERCENT
		if (GEA_CSS_POSITION_PERCENT_0(n->style) != value || GEA_CSS_POSITION_PX_0(n->style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_0(n->style) = value;
#if GEA_CSS_POSITION_TOP
			GEA_CSS_POSITION_PX_0(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::RightPercent:
#if GEA_CSS_POSITION_RIGHT_PERCENT
		if (GEA_CSS_POSITION_PERCENT_1(n->style) != value || GEA_CSS_POSITION_PX_1(n->style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_1(n->style) = value;
#if GEA_CSS_POSITION_RIGHT
			GEA_CSS_POSITION_PX_1(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::BottomPercent:
#if GEA_CSS_POSITION_BOTTOM_PERCENT
		if (GEA_CSS_POSITION_PERCENT_2(n->style) != value || GEA_CSS_POSITION_PX_2(n->style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_2(n->style) = value;
#if GEA_CSS_POSITION_BOTTOM
			GEA_CSS_POSITION_PX_2(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::LeftPercent:
#if GEA_CSS_POSITION_LEFT_PERCENT
		if (GEA_CSS_POSITION_PERCENT_3(n->style) != value || GEA_CSS_POSITION_PX_3(n->style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_3(n->style) = value;
#if GEA_CSS_POSITION_LEFT
			GEA_CSS_POSITION_PX_3(n->style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
#if GEA_CSS_Z_INDEX
	case Property::ZIndex: {
		const bool automatic = value == kZIndexAuto;
		const int level = automatic ? 0 : std::clamp(value, -32768, 32767);
		if (n->style.z_index != level || n->style.z_index_auto != automatic) {
			n->style.z_index = level; n->style.z_index_auto = automatic; changed = 1;
		}
		break;
	}
#endif
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
#if GEA_CSS_BACKGROUND_LAYERS
	case Property::BackgroundClip: if (rstyle(n->style).bg_clip != value) { rstyleMut(n->style).bg_clip = value; changed = 1; } break;
	case Property::BackgroundSizeList: if (rstyle(n->style).bg_size_list != value) { rstyleMut(n->style).bg_size_list = value; changed = 1; } break;
	case Property::BackgroundPositionList: if (rstyle(n->style).bg_position_list != value) { rstyleMut(n->style).bg_position_list = value; changed = 1; } break;
	case Property::BackgroundRepeatList: if (rstyle(n->style).bg_repeat_list != value) { rstyleMut(n->style).bg_repeat_list = value; changed = 1; } break;
	case Property::BackgroundAttachmentList: if (rstyle(n->style).bg_attachment_list != value) { rstyleMut(n->style).bg_attachment_list = value; changed = 1; } break;
	case Property::BackgroundOriginList: if (rstyle(n->style).bg_origin_list != value) { rstyleMut(n->style).bg_origin_list = value; changed = 1; } break;

#endif
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
#if GEA_CSS_TEXT_ALPHA
		if (n->style.text_alpha != 255) { n->style.text_alpha = 255; changed = 1; }
#endif
		break;
	}
#if GEA_CSS_OPACITY
	case Property::Opacity: {
		uint8_t next = (uint8_t)value;
		if (n->style.opacity != next) { prevOpacity = n->style.opacity; n->style.opacity = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_BLINK
	case Property::BlinkInterval:
		value = value > 0 ? value : 0;
		if (n->style.blink_interval_ms != value) {
			n->style.blink_interval_ms = value;
			n->style.blink_started_ms = state.lastFrameMs;
			n->style.blink_visible = 1;
			changed = 1;
		}
		break;
#endif
	case Property::ColorAlpha: {
#if GEA_CSS_TEXT_ALPHA
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (n->style.text_alpha != alpha) { n->style.text_alpha = alpha; changed = 1; }
#endif
		break;
	}
	case Property::BorderAlpha: {
#if GEA_CSS_BORDER_ALPHA
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (n->style.border_alpha != alpha) { n->style.border_alpha = alpha; changed = 1; }
#endif
		break;
	}
#if GEA_CSS_SIDE_BORDERS
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
#endif
	case Property::BorderColorCurrent: changed |= setBorderColorBinding(n->style, -1, value != 0); break;
#if GEA_CSS_SIDE_BORDERS
	case Property::BorderTopColorCurrent:
	case Property::BorderRightColorCurrent:
	case Property::BorderBottomColorCurrent:
	case Property::BorderLeftColorCurrent:
		changed |= setBorderColorBinding(n->style, static_cast<int>(prop) - static_cast<int>(Property::BorderTopColorCurrent), value != 0);
		break;
#endif
	case Property::BorderWidth: changed |= setComputedBorderWidth(n->style, -1, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
	case Property::BorderColor: {
		changed |= setBorderColorBinding(n->style, -1, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (n->style.border_color != next) { n->style.border_color = next; changed = 1; }
#if GEA_CSS_BORDER_ALPHA
		if (n->style.border_alpha != 255) { n->style.border_alpha = 255; changed = 1; }
#endif
		break;
	}
#if GEA_CSS_SIDE_BORDERS
	case Property::BorderTopWidth: changed |= setComputedBorderWidth(n->style, 0, value, n->parent >= 0 ? &state.nodes[n->parent].style : nullptr); break;
#endif
#if GEA_CSS_BORDER_RELIEF
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
			// border-style: none leaves the side without a border.
			if (value & kBorderStyleNone) changed |= setComputedBorderWidth(n->style, side, 0, nullptr);
		}
		break;
#endif
#if GEA_CSS_SIDE_BORDERS
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
#endif
	case Property::BorderRadiusTopLeft:
		if (n->style.border_radius[GEA_CSS_RADIUS_INDEX(0)] != value || n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] != kUnset) {
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(0)] = value;
#if GEA_CSS_PERCENT_RADIUS
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRight:
		if (n->style.border_radius[GEA_CSS_RADIUS_INDEX(1)] != value || n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] != kUnset) {
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(1)] = value;
#if GEA_CSS_PERCENT_RADIUS
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRight:
		if (n->style.border_radius[GEA_CSS_RADIUS_INDEX(2)] != value || n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] != kUnset) {
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(2)] = value;
#if GEA_CSS_PERCENT_RADIUS
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeft:
		if (n->style.border_radius[GEA_CSS_RADIUS_INDEX(3)] != value || n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] != kUnset) {
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(3)] = value;
#if GEA_CSS_PERCENT_RADIUS
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] = kUnset;
#endif
			changed = 1;
		}
		break;
#if GEA_CSS_PERCENT_RADIUS
	case Property::BorderRadiusTopLeftPercent:
		if (n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] != value || n->style.border_radius[GEA_CSS_RADIUS_INDEX(0)] != 0) {
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] = value;
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(0)] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRightPercent:
		if (n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] != value || n->style.border_radius[GEA_CSS_RADIUS_INDEX(1)] != 0) {
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] = value;
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(1)] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRightPercent:
		if (n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] != value || n->style.border_radius[GEA_CSS_RADIUS_INDEX(2)] != 0) {
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] = value;
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(2)] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeftPercent:
		if (n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] != value || n->style.border_radius[GEA_CSS_RADIUS_INDEX(3)] != 0) {
			n->style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] = value;
			n->style.border_radius[GEA_CSS_RADIUS_INDEX(3)] = 0;
			changed = 1;
		}
		break;
#endif
	case Property::FontId:          if (n->style.font_id != value) { n->style.font_id = value; changed = 1; } break;
	case Property::FontSize:        if (n->style.font_size != value) { n->style.font_size = value; changed = 1; } break;
	case Property::FontWeight:      if (n->style.font_weight != value) { n->style.font_weight = value; changed = 1; } break;
	case Property::LineHeight:
		if (n->style.line_height_multiplier >= 0) { n->style.line_height_multiplier = -1; changed = 1; }
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
		if (rstyle(n->style).line_height_expression >= 0) { rstyleMut(n->style).line_height_expression = -1; changed = 1; }
#endif
		if (n->style.line_height != value) { n->style.line_height = value; changed = 1; } break;
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
	case Property::LineHeightExpression: {
		if (n->style.line_height_multiplier >= 0) { n->style.line_height_multiplier = -1; changed = 1; }
		const int height = resolveLineHeightExpression(node, value);
		if (rstyle(n->style).line_height_expression != value || n->style.line_height != height) {
			rstyleMut(n->style).line_height_expression = value; n->style.line_height = height; changed = 1;
		} break;
	}
#endif
	case Property::LineHeightMultiplier: {
		const int height = resolveLineHeightMultiplier(node, value);
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
		if (rstyle(n->style).line_height_expression >= 0) { rstyleMut(n->style).line_height_expression = -1; changed = 1; }
#endif
		if (n->style.line_height_multiplier != value || n->style.line_height != height) {
			n->style.line_height_multiplier = value; n->style.line_height = height; changed = 1;
		} break;
	}
	case Property::TextAlign:       if (n->style.text_align != value) { n->style.text_align = value; changed = 1; } break;
	case Property::TextAlignLast:   if (n->style.text_align_last != value) { n->style.text_align_last = value; changed = 1; } break;
	case Property::TextEmphasisStyle:
	case Property::TextEmphasisPosition:
	case Property::TextEmphasisColorMode: {
		const int mask = prop == Property::TextEmphasisStyle ? 0x0f : prop == Property::TextEmphasisPosition ? 0x10 : 0x60;
		const int next = (n->style.text_emphasis & ~mask) | (value & mask);
		if (n->style.text_emphasis != next) { n->style.text_emphasis = static_cast<uint8_t>(next); changed = 1; }
		break;
	}
	case Property::TextEmphasisColor: {
		const auto color = StyleValues::pixelFromStyleValue(value);
		if (n->style.text_emphasis_color != color) { n->style.text_emphasis_color = color; changed = 1; }
		break;
	}
	case Property::VerticalAlign:   if (n->style.vertical_align != value) { n->style.vertical_align = value; changed = 1; } break;
#if GEA_CSS_TEXT_DECORATION
	case Property::TextDecoration:  if (n->style.text_decoration != value) { n->style.text_decoration = value; changed = 1; } break;
#endif
#if GEA_CSS_TEXT_TRANSFORM
	case Property::TextTransform:   if (n->style.text_transform != value) { n->style.text_transform = value; changed = 1; } break;
#endif
	case Property::WhiteSpace:      if (n->style.white_space != static_cast<int8_t>(value)) { n->style.white_space = static_cast<int8_t>(value); changed = 1; } break;
	case Property::TextOverflow:    if (n->style.text_overflow != static_cast<int8_t>(value)) { n->style.text_overflow = static_cast<int8_t>(value); changed = 1; } break;
#if GEA_CSS_TRANSFORMS
	case Property::TransformStyle: if (rstyle(n->style).transform_preserve_3d != (value != 0)) { rstyleMut(n->style).transform_preserve_3d = value != 0; changed = 1; } break;
#endif
#if GEA_CSS_VISIBILITY
	case Property::Visibility: if (n->style.visibility != static_cast<int8_t>(value)) { n->style.visibility = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
	case Property::Backface:        if (n->style.backface_hidden != static_cast<int8_t>(value)) { n->style.backface_hidden = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_POINTER_EVENTS
	case Property::PointerEvents:   if (n->style.pointer_events != static_cast<int8_t>(value)) { n->style.pointer_events = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_OVERFLOW_AXES
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
#else
	case Property::Overflow:
	case Property::OverflowX:
	case Property::OverflowY: {
		const int8_t next = static_cast<int8_t>(value);
		if (n->style.overflow != next) { n->style.overflow = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_MASK
	case Property::MaskRightFadeWidth: {
		const int16_t next = static_cast<int16_t>(value < 0 ? 0 : value > 32767 ? 32767 : value);
		if (n->style.mask_right_fade_width != next) {
			n->style.mask_right_fade_width = next;
			changed = 1;
		}
		break;
	}
#endif
#if GEA_UI_IMAGE_NODES
	case Property::ImageId:         if (n->image_id != value) { n->image_id = value; changed = 1; } break;
#endif
#if GEA_CSS_IMAGE_FIT
	case Property::ImageFit:        if (n->style.image_fit != value) { n->style.image_fit = value; changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
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
#endif
#if GEA_CSS_FILTERS
	case Property::FilterPresent: if (rstyle(n->style).filter_present != (value != 0)) { rstyleMut(n->style).filter_present = value != 0; changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
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
#endif
#if GEA_CSS_FILTERS
	case Property::FilterBlur: if (rstyle(n->style).filter_blur_radius != value) { rstyleMut(n->style).filter_blur_radius = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowInset: {
		const uint8_t next = value != 0 ? 1 : 0;
		if (rstyle(n->style).box_shadow_inset != next) { rstyleMut(n->style).box_shadow_inset = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowOffsetX: if (rstyle(n->style).box_shadow_offset_x != value) { rstyleMut(n->style).box_shadow_offset_x = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowOffsetY: if (rstyle(n->style).box_shadow_offset_y != value) { rstyleMut(n->style).box_shadow_offset_y = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowBlur: if (rstyle(n->style).box_shadow_blur_radius != value) { rstyleMut(n->style).box_shadow_blur_radius = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowSpread: if (rstyle(n->style).box_shadow_spread != value) { rstyleMut(n->style).box_shadow_spread = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowColor: {
		const style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(n->style).box_shadow_color != next) { rstyleMut(n->style).box_shadow_color = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowAlpha: {
		const uint8_t next = static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		if (rstyle(n->style).box_shadow_alpha != next) { rstyleMut(n->style).box_shadow_alpha = next; changed = 1; }
		break;
	}
#endif
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
	const bool hadRecolor = n->render.bg_recolor_pending;
	const bool canRecolor = !n->render.dirty || hadRecolor;
	n->render.text_partial_dirty = 0;
	n->render.bg_recolor_pending = 0;
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
#if GEA_CSS_SCROLLING
			n->render.non_scroll_dirty = 1;
#endif
			tree.markNodeDisplayCommandsDirty(node);
			return;
		}
	}
	if (canRecolor && prop == Property::BackgroundColor &&
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
		if (!hadRecolor) n->render.bg_recolor_from = previousBgColor;
		n->render.bg_recolor_pending = 1;
	}
	n->render.dirty = 1;
	// Paint-only properties (background, colors, shadows, ...) repaint in
	// place: geometry is untouched, so they don't demand a relayout pass.
	// Keep geometry dirt separate from transform dirt: coalescing both must
	// not let a retained transform update suppress the required relayout.
	if (isLayoutProperty(prop) || (isTransformProperty(prop) && state.fixedPositionUsed))
		n->render.layout_dirty = 1;
#if GEA_CSS_SCROLLING
	n->render.non_scroll_dirty = 1;
#endif
	if (isTransformProperty(prop)) {
#if GEA_CSS_TRANSFORMS
		n->render.transform_dirty = 1;
#endif

#if GEA_CSS_TRANSFORMS
		state.transformScanSerial = ~0ull;
		state.transformScanValid = false;  // a transform was added/changed → drop durable no-transform cache
#endif
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

	NodeLifecycle::resetStyle(state.nodes[node].style);
#if GEA_UI_IMAGE_NODES
	state.nodes[node].image_id = -1;
#endif
	if (state.styleInvalidationSuppressionDepth > 0) return;
	if (!nodeParticipatesInMountedTree(state, node)) return;
	state.nodes[node].render.dirty = 1;
	state.nodes[node].render.layout_dirty = 1;
#if GEA_CSS_SCROLLING
	state.nodes[node].render.non_scroll_dirty = 1;
#endif
	markDisplayListDirty();
}

}  // namespace gea::embedded::ui
