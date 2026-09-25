// SPDX-License-Identifier: Apache-2.0
#include "node_lifecycle.h"

#include "pixel.h"

#include <cstring>

namespace gea::embedded::ui {

namespace {
// One immutable default image. Resetting CSS must not construct/destroy a whole
// Node, touch its text ownership, or initialize layout/render state on the stack.
constexpr ComputedStyle makeDefaultStyle()
{
	ComputedStyle style{};
	style.display = kDisplayBlock;
	style.display_explicit = 0;
	style.flex_direction = 0;
	style.flex_direction_explicit = 0;
#if GEA_CSS_FLEX_WRAP
	style.flex_wrap = 0;
#endif
	style.justify_content = 0;
	style.align_items = 0;
#if GEA_CSS_JUSTIFY_ITEMS
	style.justify_items = 0;
#endif
#if GEA_CSS_ALIGN_CONTENT
	style.align_content = 0;
#endif
#if GEA_CSS_ALIGN_SELF
	style.align_self = -1;
#endif
	style.gap = 0;
#if GEA_CSS_ORDER
	style.order = 0;
#endif
	style.box_sizing = 0;
#if GEA_CSS_FLOATS
	style.float_side = 0;
	style.clear_side = 0;
#endif
#if GEA_CSS_WRITING_MODE
	style.writing_mode = -1;
	style.direction = -1;
#endif
	style.margin_auto = 0;
#if GEA_CSS_AXIS_GAP
	style.row_gap = style.column_gap = kUnset;
#endif
#if GEA_CSS_PERCENT_GAP
	style.row_gap_percent = style.column_gap_percent = kUnset;
#endif
	// Grid tracks moved to RareStyle (default 0 for a node with no rare_style),
	// so a fresh node needs no explicit reset here.
	style.width_expression = -1;
#if GEA_CSS_HEIGHT_EXPRESSIONS
	style.height_expression = -1;
#endif
	style.width = kUnset;
	style.height = kUnset;
	style.width_percent = kUnset;
	style.height_percent = kUnset;
#if GEA_CSS_MIN_WIDTH
	style.min_width = kUnset;
#endif
	style.min_height = kUnset;
	style.max_width = kUnset;
#if GEA_CSS_MAX_HEIGHT
	style.max_height = kUnset;
#endif
	style.flex = 0;
	style.flex_shrink = 1;
#if GEA_CSS_FLEX_BASIS
	style.flex_basis = kUnset;
#endif
	for (int i = 0; i < 4; i++) {
		style.padding[i] = 0;
		style.margin[i] = 0;
		// per-side border width/color/alpha moved to RareStyle (defaults there).
		style.border_radius[GEA_CSS_RADIUS_INDEX(i)] = 0;
#if GEA_CSS_PERCENT_RADIUS
		style.border_radius_percent[GEA_CSS_RADIUS_INDEX(i)] = kUnset;
#endif
	}
#if GEA_CSS_POSITION_TOP
	GEA_CSS_POSITION_PX_0(style) = kUnset;
#endif
#if GEA_CSS_POSITION_TOP_PERCENT
	GEA_CSS_POSITION_PERCENT_0(style) = kUnset;
#endif
#if GEA_CSS_POSITION_RIGHT
	GEA_CSS_POSITION_PX_1(style) = kUnset;
#endif
#if GEA_CSS_POSITION_RIGHT_PERCENT
	GEA_CSS_POSITION_PERCENT_1(style) = kUnset;
#endif
#if GEA_CSS_POSITION_BOTTOM
	GEA_CSS_POSITION_PX_2(style) = kUnset;
#endif
#if GEA_CSS_POSITION_BOTTOM_PERCENT
	GEA_CSS_POSITION_PERCENT_2(style) = kUnset;
#endif
#if GEA_CSS_POSITION_LEFT
	GEA_CSS_POSITION_PX_3(style) = kUnset;
#endif
#if GEA_CSS_POSITION_LEFT_PERCENT
	GEA_CSS_POSITION_PERCENT_3(style) = kUnset;
#endif
	style.position = 0;
#if GEA_CSS_Z_INDEX
	style.z_index = 0;
	style.z_index_auto = 1;
#endif
	style.bg_color = 0;
	style.has_bg = 0;
	style.bg_alpha = 0;
#if GEA_EMBEDDED_RENDERER_LINEAR_GRADIENTS
	style.bg_fill = 0;
#endif
	// gradients + background-grid moved to RareStyle (defaults there).
	style.active_bg_color = 0;
	style.has_active_bg = 0;
	style.text_color = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
#if GEA_CSS_TEXT_ALPHA
	style.text_alpha = 255;
#endif
#if GEA_CSS_OPACITY
	style.opacity = 255;
#endif
#if GEA_CSS_BLINK
	style.blink_interval_ms = 0;
	style.blink_started_ms = 0;
	style.blink_visible = 1;
#endif
	style.border_color_flags = 0;
	style.border_width = 0;
	style.border_color = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
#if GEA_CSS_BORDER_ALPHA
	style.border_alpha = 255;
#endif
	// transform / perspective / filter / box-shadow moved to RareStyle; a fresh
	// node (rare_style = -1) reads their correct defaults from the zero entry.
	style.font_id = -1;
	style.font_size = 0;
	style.font_weight = 400;
	style.line_height = 0;
	style.line_height_multiplier = -1;
	style.text_align = 0;
	style.text_align_last = 0;
#if GEA_CSS_TEXT_DECORATION
	style.text_decoration = 0;
#endif
#if GEA_CSS_TEXT_TRANSFORM
	style.text_transform = 0;
#endif
	style.white_space = 0;
	style.text_overflow = 0;
	style.overflow = 0;
#if GEA_CSS_OVERFLOW_AXES
	style.overflow_x = 0;
#endif
#if GEA_CSS_OVERFLOW_AXES
	style.overflow_y = 0;
#endif
#if GEA_CSS_MASK
	style.mask_right_fade_width = 0;
#endif
#if GEA_CSS_IMAGE_FIT
	style.image_fit = 0;
#endif
#if GEA_CSS_TRANSFORMS
	style.backface_hidden = 0;
#endif
#if GEA_CSS_VISIBILITY
	style.visibility = 0;
#endif
#if GEA_CSS_POINTER_EVENTS
	style.pointer_events = 0;
#endif
	return style;
}
constexpr ComputedStyle kDefaultStyle = makeDefaultStyle();
} // namespace

void NodeLifecycle::resetStyle(ComputedStyle &style)
{
	// Member assignment preserves adjacent state when Node reuses tail padding.
	style = kDefaultStyle;
}

void NodeLifecycle::init(Node *n, NodeType type)
{
	n->type = type;
	resetStyle(n->style);
	n->text.clear();
#if GEA_UI_IMAGE_NODES
	n->image_id = -1;
#endif
	n->tag_id = 0;  // 0 = empty tag (interned tag table index 0)
	n->parent = -1;
	n->first_child = -1;
	n->last_child = -1;
	n->next_sibling = -1;
	n->prev_sibling = -1;
	n->layout.x = 0;
	n->layout.y = 0;
	n->layout.width = 0;
	n->layout.height = 0;
	n->layout.inline_indent = 0;
#if GEA_CSS_SCROLLING
	n->layout.scroll_x = 0;
	n->layout.scroll_y = 0;
	n->layout.scroll_content_width = 0;
	n->layout.scroll_content_height = 0;
#endif
	n->layout.previous_x = 0;
	n->layout.previous_y = 0;
	n->layout.previous_width = 0;
	n->layout.previous_height = 0;
#if GEA_CSS_SCROLLING
	n->layout.previous_scroll_x = 0;
	n->layout.previous_scroll_y = 0;
#endif
#if GEA_CSS_TRANSFORMS
	n->render.previous_rotate_angle = 0;
	n->render.previous_rotate_axis_x = 0;
	n->render.previous_rotate_axis_y = 0;
	n->render.previous_rotate_axis_z = 1000000;
	n->render.previous_scale_x = 1000;
	n->render.previous_scale_y = 1000;
	n->render.previous_scale_z = 1000;
	n->render.previous_transform_rotate = 0;
	n->render.previous_transform_rotate_x = 0;
	n->render.previous_transform_rotate_y = 0;
	n->render.previous_transform_translate_x = 0;
	n->render.previous_translate_x = n->render.previous_translate_y = n->render.previous_translate_z = 0;
	n->render.previous_translate_x_percent = n->render.previous_translate_y_percent = 0;
	n->render.previous_transform_translate_outer_axes = 0;
	n->render.previous_transform_translate_y = 0;
	n->render.previous_transform_translate_z = 0;
	n->render.previous_transform_translate_x_percent = 0;
	n->render.previous_transform_translate_y_percent = 0;
	n->render.previous_transform_scale_x = 1000;
	n->render.previous_transform_scale_y = 1000;
	n->render.previous_transform_scale_z = 1000;
	n->render.previous_transform_origin_x = 500;
	n->render.previous_transform_origin_y = 500;
	n->render.previous_perspective = 0;
	n->render.previous_perspective_origin_x = 500;
	n->render.previous_perspective_origin_y = 500;
	n->render.previous_transformable_box = 0;
#endif
#if GEA_CSS_FILTERS
	n->render.previous_filter_blur_radius = 0;
#endif
	n->render.inline_baseline = 0;
	n->render.dirty = 0;
#if GEA_CSS_SCROLLING
	n->render.scroll_dirty = 0;
#endif
#if GEA_CSS_SCROLLING
	n->render.non_scroll_dirty = 0;
#endif
#if GEA_CSS_TRANSFORMS
	n->render.transform_dirty = 0;
#endif
	n->render.text_layout_stable = 0;
}

}  // namespace gea::embedded::ui
