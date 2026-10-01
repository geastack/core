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
	GEA_REFRESH_PERF(perf.treeSetStyleCalls++);
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
	ComputedStyle &style = n->mutableStyle();
	int changed = 0;
	int prevOpacity = -1;
	// These previous-bg values feed ONLY the BackgroundColor solid-recolor fast path
	// below (itself gated on prop == BackgroundColor). Reading them on every setStyle —
	// including the 4 pooled rstyle() bg-gradient lookups — is pure waste for the common
	// layout/paint writes (e.g. 128 left/top writes per frame in bouncing-balls = ~512
	// wasted pooled reads). Gate them on the property.
	const bool isBgColorChange = (prop == Property::BackgroundColor);
	const style_color_t previousBgColor = isBgColorChange ? style.bg_color : style_color_t{};
	const uint8_t previousHasBg = isBgColorChange ? style.has_bg : 0;
	const uint8_t previousBgAlpha = isBgColorChange ? style.bg_alpha : 0;
	const int8_t previousBgFill = isBgColorChange ? style.bg_fill : 0;
	const uint8_t previousBgGradientHasMid = isBgColorChange ? rstyle(style).bg_gradient_has_mid : 0;
	const uint8_t previousBgOverlayGradient = isBgColorChange ? rstyle(style).bg_overlay_gradient : 0;
	const uint8_t previousBgRadialGradient = isBgColorChange ? rstyle(style).bg_radial_gradient : 0;
	const uint8_t previousBgGridAxes = isBgColorChange ? rstyle(style).bg_grid_axes : 0;
	switch (prop) {
	case Property::Display:
		// Any application of the `display` property is explicit authoring — record
		// it so the inline-formatting heuristic treats e.g. `display:block` on a
		// <span> as block-level (stacks) rather than its default inline behaviour.
#if GEA_CSS_DISPLAY_EXPLICIT
		// Keyword bits above the box kind (flow-root) are recorded with it.
		if (style.display_explicit != (kDisplayExplicit | (value >> kDisplayFlagShift))) {
			style.display_explicit = kDisplayExplicit | (value >> kDisplayFlagShift);
			changed = 1;
		}
#endif
		if (style.display != (value & kDisplayKindMask)) { style.display = value & kDisplayKindMask; changed = 1; }
		break;
#if GEA_CSS_FLEX_DIRECTION
	case Property::FlexDirection:
		if (!style.flex_direction_explicit) {
			style.flex_direction_explicit = 1;
			changed = 1;
		}
		if (style.flex_direction != value) {
			style.flex_direction = value;
			changed = 1;
		}
		break;
#endif
#if GEA_CSS_BOX_SIZING
	case Property::BoxSizing: if (style.box_sizing != value) { style.box_sizing = value; changed = 1; } break;
#endif
#if GEA_CSS_FLOATS
	case Property::Float: if (style.float_side != value) { style.float_side = value; changed = 1; } break;
#endif
#if GEA_CSS_ASPECT_RATIO
	case Property::AspectRatio: if ((GEA_CSS_ASPECT_RATIO ? rstyle(style).aspect_ratio : 0) != value) { rstyleMut(style).aspect_ratio = value; changed = 1; } break;
#endif
#if GEA_CSS_CONTAINMENT
	case Property::Containment: if ((GEA_CSS_CONTAINMENT ? rstyle(style).containment : 0) != value) { rstyleMut(style).containment = value; changed = 1; } break;
#endif
#if GEA_CSS_FLEX_LINE_COUNT
	case Property::FlexLineCount: if ((GEA_CSS_FLEX_LINE_COUNT ? rstyle(style).flex_line_count : 1) != value) { rstyleMut(style).flex_line_count = value; changed = 1; } break;
#endif
#if GEA_CSS_MARGIN_TRIM
	case Property::MarginTrim: if ((GEA_CSS_MARGIN_TRIM ? rstyle(style).margin_trim : 0) != value) { rstyleMut(style).margin_trim = value; changed = 1; } break;
#endif
	case Property::MaxLines: if (rstyle(style).max_lines != value) { rstyleMut(style).max_lines = value; changed = 1; } break;
	case Property::BlockEllipsisString: if (rstyle(style).block_ellipsis != value) { rstyleMut(style).block_ellipsis = value; changed = 1; } break;
	case Property::ColumnCount: if (rstyle(style).column_count != value) { rstyleMut(style).column_count = value; changed = 1; } break;
	case Property::ColumnWidth: if (rstyle(style).column_width != value) { rstyleMut(style).column_width = value; changed = 1; } break;
	case Property::LineClampContinue:
	case Property::BlockEllipsis:
	case Property::ColumnCountSet:
	case Property::ColumnWidthSet:
	case Property::ColumnFillAuto:
	case Property::LineClampDiscard:
	case Property::ColumnSpanAll: {
		const int bit = lineClampFlagBit(prop);
		const int next = value ? rstyle(style).line_clamp_flags | bit : rstyle(style).line_clamp_flags & ~bit;
		if (rstyle(style).line_clamp_flags != next) { rstyleMut(style).line_clamp_flags = next; changed = 1; }
		break;
	}
#if GEA_CSS_FLOATS
	case Property::Clear: if (style.clear_side != value) { style.clear_side = value; changed = 1; } break;
#endif
#if GEA_CSS_WRITING_MODE
	case Property::Direction: if (style.direction != value) { style.direction = value; changed = 1; } break;
	case Property::WritingMode: if (style.writing_mode != value) { style.writing_mode = value; changed = 1; } break;
#endif
#if GEA_CSS_AXIS_GAP
	case Property::RowGap: if (style.row_gap != value) { style.row_gap = value; changed = 1; } break;
#endif
#if GEA_CSS_AXIS_GAP
	case Property::ColumnGap: if (style.column_gap != value) { style.column_gap = value; changed = 1; } break;
#endif
#if GEA_CSS_PERCENT_GAP
	case Property::RowGapPercent: if (style.row_gap_percent != value) { style.row_gap_percent = value; changed = 1; } break;
	case Property::ColumnGapPercent: if (style.column_gap_percent != value) { style.column_gap_percent = value; changed = 1; } break;
#endif
#if GEA_CSS_MARGIN_AUTO
	case Property::MarginTopAuto: { const auto mask = (style.margin_auto & ~1) | (value ? 1 : 0); if (mask != style.margin_auto) { style.margin_auto = mask; changed = 1; } break; }
#endif
#if GEA_CSS_MARGIN_AUTO
	case Property::MarginRightAuto: { const auto mask = (style.margin_auto & ~2) | (value ? 2 : 0); if (mask != style.margin_auto) { style.margin_auto = mask; changed = 1; } break; }
#endif
#if GEA_CSS_MARGIN_AUTO
	case Property::MarginBottomAuto: { const auto mask = (style.margin_auto & ~4) | (value ? 4 : 0); if (mask != style.margin_auto) { style.margin_auto = mask; changed = 1; } break; }
#endif
#if GEA_CSS_MARGIN_AUTO
	case Property::MarginLeftAuto: { const auto mask = (style.margin_auto & ~8) | (value ? 8 : 0); if (mask != style.margin_auto) { style.margin_auto = mask; changed = 1; } break; }
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginTopExpression: if (rstyle(style).margin_expression[0] != value) { rstyleMut(style).margin_expression[0] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginRightExpression: if (rstyle(style).margin_expression[1] != value) { rstyleMut(style).margin_expression[1] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginBottomExpression: if (rstyle(style).margin_expression[2] != value) { rstyleMut(style).margin_expression[2] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::MarginLeftExpression: if (rstyle(style).margin_expression[3] != value) { rstyleMut(style).margin_expression[3] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingTopExpression: if (rstyle(style).padding_expression[0] != value) { rstyleMut(style).padding_expression[0] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingRightExpression: if (rstyle(style).padding_expression[1] != value) { rstyleMut(style).padding_expression[1] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingBottomExpression: if (rstyle(style).padding_expression[2] != value) { rstyleMut(style).padding_expression[2] = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_EXPRESSIONS
	case Property::PaddingLeftExpression: if (rstyle(style).padding_expression[3] != value) { rstyleMut(style).padding_expression[3] = value; changed = 1; } break;
#endif
#if GEA_CSS_WIDTH_EXPRESSIONS
	case Property::WidthExpression:
		if (style.width_expression != value) {
			style.width_expression = value;
			style.width = kUnset;
#if GEA_CSS_WIDTH_PERCENT
			style.width_percent = kUnset;
#endif
			changed = 1;
		}
		break;
#endif
#if GEA_CSS_HEIGHT_EXPRESSIONS
	case Property::HeightExpression:
		if (style.height_expression != value) {
			style.height_expression = value;
			style.height = kUnset;
#if GEA_CSS_HEIGHT_PERCENT
			style.height_percent = kUnset;
#endif
			changed = 1;
		}
		break;
#endif
#if GEA_CSS_ORDER
	case Property::Order:           if (style.order != value) { style.order = value; changed = 1; } break;
#endif
#if GEA_CSS_FLEX_WRAP
	case Property::FlexWrap:        if (style.flex_wrap != value) { style.flex_wrap = value; changed = 1; } break;
#endif
#if GEA_CSS_JUSTIFY_CONTENT
	case Property::JustifyContent:  if (style.justify_content != value) { style.justify_content = value; changed = 1; } break;
#endif
#if GEA_CSS_ALIGN_ITEMS
	case Property::AlignItems:      if (style.align_items != value) { style.align_items = value; changed = 1; } break;
#endif
#if GEA_CSS_JUSTIFY_ITEMS
	case Property::JustifyItems:    if (style.justify_items != value) { style.justify_items = value; changed = 1; } break;
#endif
#if GEA_CSS_ALIGN_CONTENT
	case Property::AlignContent:    if (style.align_content != value) { style.align_content = value; changed = 1; } break;
#endif
#if GEA_CSS_ALIGN_SELF
	case Property::AlignSelf:       if (style.align_self != value) { style.align_self = value; changed = 1; } break;
#endif
#if GEA_CSS_JUSTIFY_SELF
	case Property::JustifySelf:     if ((GEA_CSS_JUSTIFY_SELF ? rstyle(style).justify_self : -1) != value) { rstyleMut(style).justify_self = value; changed = 1; } break;
#endif
#if GEA_CSS_GRID
	case Property::GridRowStart: if (rstyle(style).grid_line[0] != value) { rstyleMut(style).grid_line[0] = value; changed = 1; } break;
	case Property::GridColumnStart: if (rstyle(style).grid_line[1] != value) { rstyleMut(style).grid_line[1] = value; changed = 1; } break;
	case Property::GridRowEnd: if (rstyle(style).grid_line[2] != value) { rstyleMut(style).grid_line[2] = value; changed = 1; } break;
	case Property::GridColumnEnd: if (rstyle(style).grid_line[3] != value) { rstyleMut(style).grid_line[3] = value; changed = 1; } break;
#endif
#if GEA_CSS_GAP
	case Property::Gap:
		if (style.gap != value || style.row_gap != kUnset || style.column_gap != kUnset || style.row_gap_percent != kUnset || style.column_gap_percent != kUnset) {
			style.gap = value;
#if GEA_CSS_AXIS_GAP
	style.row_gap = style.column_gap = kUnset;
#endif
#if GEA_CSS_PERCENT_GAP
			style.row_gap_percent = style.column_gap_percent = kUnset;
#endif
			changed = 1;
		}
		break;
#endif
	case Property::Width:
#if GEA_CSS_WIDTH_EXPRESSIONS
		if (style.width_expression >= 0) { style.width_expression = -1; changed = 1; }
#endif
		if (style.width != value || style.width_percent != kUnset) {
			style.width = value;
#if GEA_CSS_WIDTH_PERCENT
			style.width_percent = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::Height:
#if GEA_CSS_HEIGHT_EXPRESSIONS
		if (style.height_expression != -1) { style.height_expression = -1; changed = 1; }
#endif
		if (style.height != value || style.height_percent != kUnset) {
			style.height = value;
#if GEA_CSS_HEIGHT_PERCENT
			style.height_percent = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::WidthPercent:
#if GEA_CSS_WIDTH_EXPRESSIONS
		if (style.width_expression >= 0) { style.width_expression = -1; changed = 1; }
#endif
		if (style.width_percent != value || style.width != kUnset) {
#if GEA_CSS_WIDTH_PERCENT
			style.width_percent = value;
#endif
			style.width = kUnset;
			changed = 1;
		}
		break;
	case Property::HeightPercent:
#if GEA_CSS_HEIGHT_EXPRESSIONS
		if (style.height_expression != -1) { style.height_expression = -1; changed = 1; }
#endif
		if (style.height_percent != value || style.height != kUnset) {
#if GEA_CSS_HEIGHT_PERCENT
			style.height_percent = value;
#endif
			style.height = kUnset;
			changed = 1;
		}
		break;
#if GEA_CSS_MIN_WIDTH
	case Property::MinWidth:        if (style.min_width != value) { style.min_width = value; changed = 1; } break;
#endif
#if GEA_CSS_MIN_HEIGHT
	case Property::MinHeight:       if (style.min_height != value) { style.min_height = value; changed = 1; } break;
#endif
#if GEA_CSS_MAX_WIDTH
	case Property::MaxWidth:        if (style.max_width != value) { style.max_width = value; changed = 1; } break;
#endif
#if GEA_CSS_MAX_HEIGHT
	case Property::MaxHeight:       if (style.max_height != value) { style.max_height = value; changed = 1; } break;
#endif
#if GEA_CSS_FLEX_FACTORS
	case Property::Flex:             if (style.flex != value) { style.flex = value; changed = 1; } break;
	case Property::FlexShrink:       if (style.flex_shrink != value) { style.flex_shrink = value; changed = 1; } break;
#endif
	case Property::FlexBasis:
#if GEA_CSS_FLEX_BASIS_EXPRESSIONS
		if (rstyle(style).flex_basis_expression >= 0) { rstyleMut(style).flex_basis_expression = -1; changed = 1; }
#endif
#if GEA_CSS_FLEX_BASIS
		if (style.flex_basis != value) { style.flex_basis = value; changed = 1; }
#endif
		break;
#if GEA_CSS_FLEX_BASIS_EXPRESSIONS
	case Property::FlexBasisExpression:
		if (rstyle(style).flex_basis_expression != value || style.flex_basis != kUnset) {
			rstyleMut(style).flex_basis_expression = value;
#if GEA_CSS_FLEX_BASIS
			style.flex_basis = kUnset;
#endif
			changed = 1;
		}
		break;
#endif
#if GEA_CSS_PADDING
	case Property::PaddingTop:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).padding_expression[0] >= 0) { rstyleMut(style).padding_expression[0] = -1; changed = 1; }
#endif
		if (style.padding[0] != value) { style.padding[0] = value; changed = 1; } break;
	case Property::PaddingRight:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).padding_expression[1] >= 0) { rstyleMut(style).padding_expression[1] = -1; changed = 1; }
#endif
		if (style.padding[1] != value) { style.padding[1] = value; changed = 1; } break;
	case Property::PaddingBottom:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).padding_expression[2] >= 0) { rstyleMut(style).padding_expression[2] = -1; changed = 1; }
#endif
		if (style.padding[2] != value) { style.padding[2] = value; changed = 1; } break;
	case Property::PaddingLeft:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).padding_expression[3] >= 0) { rstyleMut(style).padding_expression[3] = -1; changed = 1; }
#endif
		if (style.padding[3] != value) { style.padding[3] = value; changed = 1; } break;
#endif
#if GEA_CSS_MARGINS
	case Property::MarginTop:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).margin_expression[0] >= 0) { rstyleMut(style).margin_expression[0] = -1; changed = 1; }
#endif
		if (style.margin[0] != value || (style.margin_auto & 1)) {
			style.margin[0] = value;
#if GEA_CSS_MARGIN_AUTO
			style.margin_auto &= ~1;
#endif
			changed = 1;
		} break;
	case Property::MarginRight:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).margin_expression[1] >= 0) { rstyleMut(style).margin_expression[1] = -1; changed = 1; }
#endif
		if (style.margin[1] != value || (style.margin_auto & 2)) {
			style.margin[1] = value;
#if GEA_CSS_MARGIN_AUTO
			style.margin_auto &= ~2;
#endif
			changed = 1;
		} break;
	case Property::MarginBottom:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).margin_expression[2] >= 0) { rstyleMut(style).margin_expression[2] = -1; changed = 1; }
#endif
		if (style.margin[2] != value || (style.margin_auto & 4)) {
			style.margin[2] = value;
#if GEA_CSS_MARGIN_AUTO
			style.margin_auto &= ~4;
#endif
			changed = 1;
		} break;
	case Property::MarginLeft:
#if GEA_CSS_BOX_EXPRESSIONS
		if (rstyle(style).margin_expression[3] >= 0) { rstyleMut(style).margin_expression[3] = -1; changed = 1; }
#endif
		if (style.margin[3] != value || (style.margin_auto & 8)) {
			style.margin[3] = value;
#if GEA_CSS_MARGIN_AUTO
			style.margin_auto &= ~8;
#endif
			changed = 1;
		} break;
#endif
	case Property::Position:         if (value == kPositionFixed) state.fixedPositionUsed = true; if (style.position != value) { style.position = value; changed = 1; } break;
	case Property::Top:
#if GEA_CSS_POSITION_TOP
		if (GEA_CSS_POSITION_PX_0(style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_0(style) != kUnset) {
			GEA_CSS_POSITION_PX_0(style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_TOP_PERCENT
			GEA_CSS_POSITION_PERCENT_0(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::Right:
#if GEA_CSS_POSITION_RIGHT
		if (GEA_CSS_POSITION_PX_1(style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_1(style) != kUnset) {
			GEA_CSS_POSITION_PX_1(style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_RIGHT_PERCENT
			GEA_CSS_POSITION_PERCENT_1(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::Bottom:
#if GEA_CSS_POSITION_BOTTOM
		if (GEA_CSS_POSITION_PX_2(style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_2(style) != kUnset) {
			GEA_CSS_POSITION_PX_2(style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_BOTTOM_PERCENT
			GEA_CSS_POSITION_PERCENT_2(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::Left:
#if GEA_CSS_POSITION_LEFT
		if (GEA_CSS_POSITION_PX_3(style) != storedPositionOffset(value) || GEA_CSS_POSITION_PERCENT_3(style) != kUnset) {
			GEA_CSS_POSITION_PX_3(style) = storedPositionOffset(value);
#if GEA_CSS_POSITION_LEFT_PERCENT
			GEA_CSS_POSITION_PERCENT_3(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::TopPercent:
#if GEA_CSS_POSITION_TOP_PERCENT
		if (GEA_CSS_POSITION_PERCENT_0(style) != value || GEA_CSS_POSITION_PX_0(style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_0(style) = value;
#if GEA_CSS_POSITION_TOP
			GEA_CSS_POSITION_PX_0(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::RightPercent:
#if GEA_CSS_POSITION_RIGHT_PERCENT
		if (GEA_CSS_POSITION_PERCENT_1(style) != value || GEA_CSS_POSITION_PX_1(style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_1(style) = value;
#if GEA_CSS_POSITION_RIGHT
			GEA_CSS_POSITION_PX_1(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::BottomPercent:
#if GEA_CSS_POSITION_BOTTOM_PERCENT
		if (GEA_CSS_POSITION_PERCENT_2(style) != value || GEA_CSS_POSITION_PX_2(style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_2(style) = value;
#if GEA_CSS_POSITION_BOTTOM
			GEA_CSS_POSITION_PX_2(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
	case Property::LeftPercent:
#if GEA_CSS_POSITION_LEFT_PERCENT
		if (GEA_CSS_POSITION_PERCENT_3(style) != value || GEA_CSS_POSITION_PX_3(style) != kUnset) {
			GEA_CSS_POSITION_PERCENT_3(style) = value;
#if GEA_CSS_POSITION_LEFT
			GEA_CSS_POSITION_PX_3(style) = kUnset;
#endif
			changed = 1;
		}
#endif
		break;
#if GEA_CSS_Z_INDEX
	case Property::ZIndex: {
		const bool automatic = value == kZIndexAuto;
		const int level = automatic ? 0 : std::clamp(value, -32768, 32767);
		if (style.z_index != level || style.z_index_auto != automatic) {
			style.z_index = level; style.z_index_auto = automatic; changed = 1;
		}
		break;
	}
#endif
	case Property::BackgroundColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (style.bg_color != next) { style.bg_color = next; changed = 1; }
		if (style.bg_alpha != 255) { style.bg_alpha = 255; changed = 1; }
		break;
	}
	case Property::BackgroundAlpha: {
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (style.bg_alpha != alpha) { style.bg_alpha = alpha; changed = 1; }
		break;
	}
#if GEA_CSS_BACKGROUND_LAYERS
	case Property::BackgroundClip: if (rstyle(style).bg_clip != value) { rstyleMut(style).bg_clip = value; changed = 1; } break;
	case Property::BackgroundBlendMode: if (rstyle(style).bg_blend != value) { rstyleMut(style).bg_blend = value; changed = 1; } break;
	case Property::BorderImageSource: if (rstyle(style).border_image_source != value) { rstyleMut(style).border_image_source = value; changed = 1; } break;
	case Property::BorderImageSlice: if (rstyle(style).border_image_slice != value) { rstyleMut(style).border_image_slice = value; changed = 1; } break;
	case Property::BorderImageWidth: if (rstyle(style).border_image_width != value) { rstyleMut(style).border_image_width = value; changed = 1; } break;
	case Property::BorderImageOutset: if (rstyle(style).border_image_outset != value) { rstyleMut(style).border_image_outset = value; changed = 1; } break;
	case Property::BorderImageRepeat: if (rstyle(style).border_image_repeat != value) { rstyleMut(style).border_image_repeat = value; changed = 1; } break;
	case Property::BackgroundSizeList: if (rstyle(style).bg_size_list != value) { rstyleMut(style).bg_size_list = value; changed = 1; } break;
	case Property::BackgroundPositionList: if (rstyle(style).bg_position_list != value) { rstyleMut(style).bg_position_list = value; changed = 1; } break;
	case Property::BackgroundRepeatList: if (rstyle(style).bg_repeat_list != value) { rstyleMut(style).bg_repeat_list = value; changed = 1; } break;
	case Property::BackgroundAttachmentList: if (rstyle(style).bg_attachment_list != value) { rstyleMut(style).bg_attachment_list = value; changed = 1; } break;
	case Property::BackgroundOriginList: if (rstyle(style).bg_origin_list != value) { rstyleMut(style).bg_origin_list = value; changed = 1; } break;

#endif
	case Property::BackgroundImage:
		changed = StyleValues::applyBackgroundImage(style, value, node);
		break;
	case Property::HasBackground:           if (style.has_bg != value) { style.has_bg = value; changed = 1; } break;
#if GEA_CSS_ACTIVE_BACKGROUND
	case Property::ActiveBackgroundColor: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (style.active_bg_color != next) { style.active_bg_color = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_ACTIVE_BACKGROUND
	case Property::HasActiveBackground:    if (style.has_active_bg != value) { style.has_active_bg = value; changed = 1; } break;
#endif
	case Property::Color: {
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (style.text_color != next) { style.text_color = next; changed = 1; }
#if GEA_CSS_TEXT_ALPHA
		if (style.text_alpha != 255) { style.text_alpha = 255; changed = 1; }
#endif
		break;
	}
#if GEA_CSS_OPACITY
	case Property::Opacity: {
		uint8_t next = (uint8_t)value;
		if (style.opacity != next) { prevOpacity = style.opacity; style.opacity = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_BLINK
	case Property::BlinkInterval:
		value = value > 0 ? value : 0;
		if (style.blink_interval_ms != value) {
			style.blink_interval_ms = value;
			style.blink_started_ms = state.lastFrameMs;
			style.blink_visible = 1;
			changed = 1;
		}
		break;
#endif
	case Property::ColorAlpha: {
#if GEA_CSS_TEXT_ALPHA
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (style.text_alpha != alpha) { style.text_alpha = alpha; changed = 1; }
#endif
		break;
	}
	case Property::BorderAlpha: {
#if GEA_CSS_BORDER_ALPHA
		const uint8_t alpha = static_cast<uint8_t>(std::clamp(value, 0, 255));
		if (style.border_alpha != alpha) { style.border_alpha = alpha; changed = 1; }
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
		if (rstyle(style).border_side_alpha[side] != alpha) { rstyleMut(style).border_side_alpha[side] = alpha; changed = 1; }
		break;
	}
#endif
	case Property::BorderColorCurrent: changed |= setBorderColorBinding(style, -1, value != 0); break;
#if GEA_CSS_SIDE_BORDERS
	case Property::BorderTopColorCurrent:
	case Property::BorderRightColorCurrent:
	case Property::BorderBottomColorCurrent:
	case Property::BorderLeftColorCurrent:
		changed |= setBorderColorBinding(style, static_cast<int>(prop) - static_cast<int>(Property::BorderTopColorCurrent), value != 0);
		break;
#endif
	case Property::BorderWidth: changed |= setComputedBorderWidth(style, -1, value, n->parent >= 0 ? &state.nodes[n->parent].computedStyle() : nullptr); break;
#if GEA_CSS_BORDER_COLORS
	case Property::BorderColor: {
		changed |= setBorderColorBinding(style, -1, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (style.border_color != next) { style.border_color = next; changed = 1; }
#if GEA_CSS_BORDER_ALPHA
		if (style.border_alpha != 255) { style.border_alpha = 255; changed = 1; }
#endif
		break;
	}
#endif
#if GEA_CSS_SIDE_BORDERS
	case Property::BorderTopWidth: changed |= setComputedBorderWidth(style, 0, value, n->parent >= 0 ? &state.nodes[n->parent].computedStyle() : nullptr); break;
#endif
#if GEA_CSS_BORDER_RELIEF
	case Property::BorderRelief:
	case Property::BorderTopRelief:
	case Property::BorderRightRelief:
	case Property::BorderBottomRelief:
	case Property::BorderLeftRelief:
		for (int side = 0; side < 4; ++side) {
			if (prop != Property::BorderRelief && side != static_cast<int>(prop) - static_cast<int>(Property::BorderTopRelief)) continue;
			if (rstyle(style).border_relief[side] != value) {
				rstyleMut(style).border_relief[side] = static_cast<uint8_t>(value);
				changed = 1;
			}
			// border-style: none leaves the side without a border.
			if (value & kBorderStyleNone) changed |= setComputedBorderWidth(style, side, 0, nullptr);
		}
		break;
#endif
#if GEA_CSS_SIDE_BORDERS
	case Property::BorderRightWidth: changed |= setComputedBorderWidth(style, 1, value, n->parent >= 0 ? &state.nodes[n->parent].computedStyle() : nullptr); break;
	case Property::BorderBottomWidth: changed |= setComputedBorderWidth(style, 2, value, n->parent >= 0 ? &state.nodes[n->parent].computedStyle() : nullptr); break;
	case Property::BorderLeftWidth: changed |= setComputedBorderWidth(style, 3, value, n->parent >= 0 ? &state.nodes[n->parent].computedStyle() : nullptr); break;
	case Property::BorderTopColor: {
		changed |= setBorderColorBinding(style, 0, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(style).border_side_color[0] != next) { rstyleMut(style).border_side_color[0] = next; changed = 1; }
		if (rstyle(style).border_side_alpha[0] != 255) { rstyleMut(style).border_side_alpha[0] = 255; changed = 1; }
		break;
	}
	case Property::BorderRightColor: {
		changed |= setBorderColorBinding(style, 1, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(style).border_side_color[1] != next) { rstyleMut(style).border_side_color[1] = next; changed = 1; }
		if (rstyle(style).border_side_alpha[1] != 255) { rstyleMut(style).border_side_alpha[1] = 255; changed = 1; }
		break;
	}
	case Property::BorderBottomColor: {
		changed |= setBorderColorBinding(style, 2, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(style).border_side_color[2] != next) { rstyleMut(style).border_side_color[2] = next; changed = 1; }
		if (rstyle(style).border_side_alpha[2] != 255) { rstyleMut(style).border_side_alpha[2] = 255; changed = 1; }
		break;
	}
	case Property::BorderLeftColor: {
		changed |= setBorderColorBinding(style, 3, false);
		style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(style).border_side_color[3] != next) { rstyleMut(style).border_side_color[3] = next; changed = 1; }
		if (rstyle(style).border_side_alpha[3] != 255) { rstyleMut(style).border_side_alpha[3] = 255; changed = 1; }
		break;
	}
#endif
	case Property::BorderRadiusTopLeft:
		if (style.border_radius[GEA_CSS_RADIUS_INDEX(0)] != value || style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] != kUnset) {
			style.border_radius[GEA_CSS_RADIUS_INDEX(0)] = value;
#if GEA_CSS_PERCENT_RADIUS
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRight:
		if (style.border_radius[GEA_CSS_RADIUS_INDEX(1)] != value || style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] != kUnset) {
			style.border_radius[GEA_CSS_RADIUS_INDEX(1)] = value;
#if GEA_CSS_PERCENT_RADIUS
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRight:
		if (style.border_radius[GEA_CSS_RADIUS_INDEX(2)] != value || style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] != kUnset) {
			style.border_radius[GEA_CSS_RADIUS_INDEX(2)] = value;
#if GEA_CSS_PERCENT_RADIUS
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] = kUnset;
#endif
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeft:
		if (style.border_radius[GEA_CSS_RADIUS_INDEX(3)] != value || style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] != kUnset) {
			style.border_radius[GEA_CSS_RADIUS_INDEX(3)] = value;
#if GEA_CSS_PERCENT_RADIUS
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] = kUnset;
#endif
			changed = 1;
		}
		break;
#if GEA_CSS_PERCENT_RADIUS
	case Property::BorderRadiusTopLeftPercent:
		if (style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] != value || style.border_radius[GEA_CSS_RADIUS_INDEX(0)] != 0) {
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(0)] = value;
			style.border_radius[GEA_CSS_RADIUS_INDEX(0)] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusTopRightPercent:
		if (style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] != value || style.border_radius[GEA_CSS_RADIUS_INDEX(1)] != 0) {
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(1)] = value;
			style.border_radius[GEA_CSS_RADIUS_INDEX(1)] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomRightPercent:
		if (style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] != value || style.border_radius[GEA_CSS_RADIUS_INDEX(2)] != 0) {
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(2)] = value;
			style.border_radius[GEA_CSS_RADIUS_INDEX(2)] = 0;
			changed = 1;
		}
		break;
	case Property::BorderRadiusBottomLeftPercent:
		if (style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] != value || style.border_radius[GEA_CSS_RADIUS_INDEX(3)] != 0) {
			style.border_radius_percent[GEA_CSS_RADIUS_INDEX(3)] = value;
			style.border_radius[GEA_CSS_RADIUS_INDEX(3)] = 0;
			changed = 1;
		}
		break;
#endif
	case Property::FontId:          if (style.font_id != value) { style.font_id = value; changed = 1; } break;
	case Property::FontSize:        if (style.font_size != value) { style.font_size = value; changed = 1; } break;
#if GEA_CSS_FONT_WEIGHT
	case Property::FontWeight:      if (style.font_weight != value) { style.font_weight = value; changed = 1; } break;
#endif
#if GEA_CSS_LINE_HEIGHT
	case Property::LineHeight:
#if GEA_CSS_LINE_HEIGHT_MULTIPLIER
		if (style.line_height_multiplier >= 0) { style.line_height_multiplier = -1; changed = 1; }
#endif
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
		if (rstyle(style).line_height_expression >= 0) { rstyleMut(style).line_height_expression = -1; changed = 1; }
#endif
		if (style.line_height != value) { style.line_height = value; changed = 1; } break;
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
	case Property::LineHeightExpression: {
#if GEA_CSS_LINE_HEIGHT_MULTIPLIER
		if (style.line_height_multiplier >= 0) { style.line_height_multiplier = -1; changed = 1; }
#endif
		const int height = resolveLineHeightExpression(node, value);
		if (rstyle(style).line_height_expression != value || style.line_height != height) {
			rstyleMut(style).line_height_expression = value; style.line_height = height; changed = 1;
		} break;
	}
#endif
#if GEA_CSS_LINE_HEIGHT_MULTIPLIER
	case Property::LineHeightMultiplier: {
		const int height = resolveLineHeightMultiplier(node, value);
#if GEA_CSS_LINE_HEIGHT_EXPRESSIONS
		if (rstyle(style).line_height_expression >= 0) { rstyleMut(style).line_height_expression = -1; changed = 1; }
#endif
		if (style.line_height_multiplier != value || style.line_height != height) {
			style.line_height_multiplier = value; style.line_height = height; changed = 1;
		} break;
	}
#endif
#endif
#if GEA_CSS_TEXT_ALIGN
	case Property::TextAlign:       if (style.text_align != value) { style.text_align = value; changed = 1; } break;
#endif
	case Property::TextAlignLast:   if (style.text_align_last != value) { style.text_align_last = value; changed = 1; } break;
	case Property::TextEmphasisStyle:
	case Property::TextEmphasisPosition:
	case Property::TextEmphasisColorMode: {
		const int mask = prop == Property::TextEmphasisStyle ? 0x0f : prop == Property::TextEmphasisPosition ? 0x10 : 0x60;
		const int next = (style.text_emphasis & ~mask) | (value & mask);
		if (style.text_emphasis != next) { style.text_emphasis = static_cast<uint8_t>(next); changed = 1; }
		break;
	}
	case Property::TextEmphasisColor: {
		const auto color = StyleValues::pixelFromStyleValue(value);
		if (style.text_emphasis_color != color) { style.text_emphasis_color = color; changed = 1; }
		break;
	}
	case Property::VerticalAlign:   if (style.vertical_align != value) { style.vertical_align = value; changed = 1; } break;
#if GEA_CSS_TEXT_DECORATION
	case Property::TextDecoration:  if (style.text_decoration != value) { style.text_decoration = value; changed = 1; } break;
#endif
#if GEA_CSS_TEXT_TRANSFORM
	case Property::TextTransform:   if (style.text_transform != value) { style.text_transform = value; changed = 1; } break;
#endif
#if GEA_CSS_WHITE_SPACE
	case Property::WhiteSpace:      if (style.white_space != static_cast<int8_t>(value)) { style.white_space = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_TEXT_OVERFLOW
	case Property::TextOverflow:    if (style.text_overflow != static_cast<int8_t>(value)) { style.text_overflow = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
	case Property::TransformStyle: if (rstyle(style).transform_preserve_3d != (value != 0)) { rstyleMut(style).transform_preserve_3d = value != 0; changed = 1; } break;
#endif
#if GEA_CSS_VISIBILITY
	case Property::Visibility: if (style.visibility != static_cast<int8_t>(value)) { style.visibility = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
	case Property::Backface:        if (style.backface_hidden != static_cast<int8_t>(value)) { style.backface_hidden = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_POINTER_EVENTS
	case Property::PointerEvents:   if (style.pointer_events != static_cast<int8_t>(value)) { style.pointer_events = static_cast<int8_t>(value); changed = 1; } break;
#endif
#if GEA_CSS_OVERFLOW_AXES
	case Property::Overflow: {
		const int8_t next = static_cast<int8_t>(value);
		if (style.overflow != next || style.overflow_x != next || style.overflow_y != next) {
			style.overflow = next;
			style.overflow_x = next;
			style.overflow_y = next;
			changed = 1;
		}
		break;
	}
	case Property::OverflowX: {
		const int8_t next = static_cast<int8_t>(value);
		const int8_t aggregate = aggregateOverflow(next, style.overflow_y);
		if (style.overflow_x != next || style.overflow != aggregate) {
			style.overflow_x = next;
			style.overflow = aggregate;
			changed = 1;
		}
		break;
	}
	case Property::OverflowY: {
		const int8_t next = static_cast<int8_t>(value);
		const int8_t aggregate = aggregateOverflow(style.overflow_x, next);
		if (style.overflow_y != next || style.overflow != aggregate) {
			style.overflow_y = next;
			style.overflow = aggregate;
			changed = 1;
		}
		break;
	}
#else
	case Property::Overflow:
	case Property::OverflowX:
	case Property::OverflowY: {
		const int8_t next = static_cast<int8_t>(value);
		if (style.overflow != next) { style.overflow = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_MASK
	case Property::MaskRightFadeWidth: {
		const int16_t next = static_cast<int16_t>(value < 0 ? 0 : value > 32767 ? 32767 : value);
		if (style.mask_right_fade_width != next) {
			style.mask_right_fade_width = next;
			changed = 1;
		}
		break;
	}
#endif
#if GEA_UI_IMAGE_NODES
	case Property::ImageId:         if (n->image_id != value) { n->image_id = value; changed = 1; } break;
#endif
#if GEA_CSS_IMAGE_FIT
	case Property::ImageFit:        if (style.image_fit != value) { style.image_fit = value; changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
	case Property::TransformTranslateOuterAxes: if (rstyle(style).transform_translate_outer_axes != value) { rstyleMut(style).transform_translate_outer_axes = value; changed = 1; } break;
	case Property::RotateAngle: if (rstyle(style).rotate_angle != value) { rstyleMut(style).rotate_angle = value; changed = 1; } break;
	case Property::RotateAxisX: if (rstyle(style).rotate_axis_x != value) { rstyleMut(style).rotate_axis_x = value; changed = 1; } break;
	case Property::RotateAxisY: if (rstyle(style).rotate_axis_y != value) { rstyleMut(style).rotate_axis_y = value; changed = 1; } break;
	case Property::RotateAxisZ: if (rstyle(style).rotate_axis_z != value) { rstyleMut(style).rotate_axis_z = value; changed = 1; } break;
	case Property::ScaleX: if (rstyle(style).scale_x != value) { rstyleMut(style).scale_x = value; changed = 1; } break;
	case Property::ScaleY: if (rstyle(style).scale_y != value) { rstyleMut(style).scale_y = value; changed = 1; } break;
	case Property::ScaleZ: if (rstyle(style).scale_z != value) { rstyleMut(style).scale_z = value; changed = 1; } break;
	case Property::TranslatePresent: if (rstyle(style).translate_present != (value != 0)) { rstyleMut(style).translate_present = value != 0; changed = 1; } break;
	case Property::TranslateX: if (rstyle(style).translate_x != value) { rstyleMut(style).translate_x = value; changed = 1; } break;
	case Property::TranslateY: if (rstyle(style).translate_y != value) { rstyleMut(style).translate_y = value; changed = 1; } break;
	case Property::TranslateZ: if (rstyle(style).translate_z != value) { rstyleMut(style).translate_z = value; changed = 1; } break;
	case Property::TranslateXPercent: if (rstyle(style).translate_x_percent != value) { rstyleMut(style).translate_x_percent = value; changed = 1; } break;
	case Property::TranslateYPercent: if (rstyle(style).translate_y_percent != value) { rstyleMut(style).translate_y_percent = value; changed = 1; } break;
	case Property::TransformPresent: if (rstyle(style).transform_present != (value != 0)) { rstyleMut(style).transform_present = value != 0; changed = 1; } break;
	case Property::RotatePresent: if (rstyle(style).rotate_present != (value != 0)) { rstyleMut(style).rotate_present = value != 0; changed = 1; } break;
	case Property::ScalePresent: if (rstyle(style).scale_present != (value != 0)) { rstyleMut(style).scale_present = value != 0; changed = 1; } break;
#endif
#if GEA_CSS_FILTERS
	case Property::FilterPresent: if (rstyle(style).filter_present != (value != 0)) { rstyleMut(style).filter_present = value != 0; changed = 1; } break;
#endif
#if GEA_CSS_TRANSFORMS
	case Property::TransformRotate:
		if (rstyle(style).transform_rotate != value) {
			rstyleMut(style).transform_rotate = value;
			changed = 1;
		}
		break;
	case Property::TransformRotateX: if (rstyle(style).transform_rotate_x != value) { rstyleMut(style).transform_rotate_x = value; changed = 1; } break;
	case Property::TransformRotateY: if (rstyle(style).transform_rotate_y != value) { rstyleMut(style).transform_rotate_y = value; changed = 1; } break;
	case Property::TransformTranslateX: if (rstyle(style).transform_translate_x != value) { rstyleMut(style).transform_translate_x = value; changed = 1; } break;
	case Property::TransformTranslateY: if (rstyle(style).transform_translate_y != value) { rstyleMut(style).transform_translate_y = value; changed = 1; } break;
	case Property::TransformTranslateZ: if (rstyle(style).transform_translate_z != value) { rstyleMut(style).transform_translate_z = value; changed = 1; } break;
	case Property::TransformTranslateXPercent: if (rstyle(style).transform_translate_x_percent != value) { rstyleMut(style).transform_translate_x_percent = value; changed = 1; } break;
	case Property::TransformTranslateYPercent: if (rstyle(style).transform_translate_y_percent != value) { rstyleMut(style).transform_translate_y_percent = value; changed = 1; } break;
	case Property::TransformScaleX: if (rstyle(style).transform_scale_x != value) { rstyleMut(style).transform_scale_x = value; changed = 1; } break;
	case Property::TransformScaleY: if (rstyle(style).transform_scale_y != value) { rstyleMut(style).transform_scale_y = value; changed = 1; } break;
	case Property::TransformScaleZ: if (rstyle(style).transform_scale_z != value) { rstyleMut(style).transform_scale_z = value; changed = 1; } break;
	case Property::TransformOriginX: if (rstyle(style).transform_origin_x != value) { rstyleMut(style).transform_origin_x = value; changed = 1; } break;
	case Property::TransformOriginY: if (rstyle(style).transform_origin_y != value) { rstyleMut(style).transform_origin_y = value; changed = 1; } break;
	case Property::Perspective: if (rstyle(style).perspective != value) { rstyleMut(style).perspective = value; changed = 1; } break;
	case Property::PerspectiveOriginX: if (rstyle(style).perspective_origin_x != value) { rstyleMut(style).perspective_origin_x = value; changed = 1; } break;
	case Property::PerspectiveOriginY: if (rstyle(style).perspective_origin_y != value) { rstyleMut(style).perspective_origin_y = value; changed = 1; } break;
#endif
#if GEA_CSS_FILTERS
	case Property::FilterBlur: if (rstyle(style).filter_blur_radius != value) { rstyleMut(style).filter_blur_radius = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowInset: {
		const uint8_t next = value != 0 ? 1 : 0;
		if (rstyle(style).box_shadow_inset != next) { rstyleMut(style).box_shadow_inset = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowOffsetX: if (rstyle(style).box_shadow_offset_x != value) { rstyleMut(style).box_shadow_offset_x = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowOffsetY: if (rstyle(style).box_shadow_offset_y != value) { rstyleMut(style).box_shadow_offset_y = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowBlur: if (rstyle(style).box_shadow_blur_radius != value) { rstyleMut(style).box_shadow_blur_radius = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowSpread: if (rstyle(style).box_shadow_spread != value) { rstyleMut(style).box_shadow_spread = value; changed = 1; } break;
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowColor: {
		const style_color_t next = StyleValues::pixelFromStyleValue(value);
		if (rstyle(style).box_shadow_color != next) { rstyleMut(style).box_shadow_color = next; changed = 1; }
		break;
	}
#endif
#if GEA_CSS_BOX_SHADOW
	case Property::BoxShadowAlpha: {
		const uint8_t next = static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
		if (rstyle(style).box_shadow_alpha != next) { rstyleMut(style).box_shadow_alpha = next; changed = 1; }
		break;
	}
#endif
	default:
		GEA_REFRESH_PERF(perf.treeSetStyleNoop++);
		return;
	}
	if (!changed) {
		GEA_REFRESH_PERF(perf.treeSetStyleNoop++);
		return;
	}
	GEA_REFRESH_PERF(perf.treeSetStyleChanged++);
	if (isLayoutProperty(prop) || (isTransformProperty(prop) && state.fixedPositionUsed))
		GEA_REFRESH_PERF(perf.treeSetStyleLayoutChanged++);
	if (isTransformProperty(prop))
		GEA_REFRESH_PERF(perf.treeSetStyleTransformChanged++);
	else if (isPaintProperty(prop))
		GEA_REFRESH_PERF(perf.treeSetStylePaintChanged++);
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
			(style.width != kUnset || style.width_percent != kUnset) &&
			(style.height != kUnset || style.height_percent != kUnset);
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
	    style.has_bg &&
	    style.bg_alpha == 255 &&
	    style.bg_fill == 0 &&
	    rstyle(style).bg_gradient_has_mid == 0 &&
	    rstyle(style).bg_overlay_gradient == 0 &&
	    rstyle(style).bg_radial_gradient == 0 &&
	    rstyle(style).bg_grid_axes == 0 &&
	    previousBgColor != style.bg_color) {
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
			if (stayLocal && prevOpacity < 255 && style.opacity < 255 &&
			    DisplayList::instance().patchNodeAlpha(node, static_cast<uint8_t>(style.opacity)))
				return;
			// Crossing the 255 boundary (scope appears/disappears) or a non-leaf:
			// a leaf toggling strictly 0↔255 re-records locally; otherwise rebuild.
			const bool toggleOnly = (prevOpacity == 0 || prevOpacity == 255) &&
			                        (style.opacity == 0 || style.opacity == 255);
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
		                         style.position == 1 && n->first_child < 0 && n->type != NodeType::Text;
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

	state.nodes[node].resetComputedStyle();
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
