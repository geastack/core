// SPDX-License-Identifier: Apache-2.0
#include "node_lifecycle.h"

#include "pixel.h"

#include <cstring>

namespace gea::embedded::ui {

void NodeLifecycle::init(Node *n, NodeType type)
{
	// Node contains a `std::string text` member — it is NOT a trivially-
	// copyable struct, so `std::memset(n, 0, sizeof(*n))` is undefined
	// behavior (it would zero the string's internal _M_dataplus._M_p,
	// and the subsequent .clear() would null-deref writing the NUL
	// terminator to address 0). Every Node field is explicitly assigned
	// below, so the memset was a redundant belt-and-suspenders zero —
	// dropping it both fixes the crash and is no-loss because the next
	// ~80 lines reassign every member.
	n->type = type;
	n->style.display = kDisplayBlock;
	n->style.display_explicit = 0;
	n->style.flex_direction = 0;
	n->style.flex_direction_explicit = 0;
	n->style.flex_wrap = 0;
	n->style.justify_content = 0;
	n->style.align_items = 0;
	n->style.justify_items = 0;
	n->style.align_content = 0;
	n->style.align_self = -1;
	n->style.gap = 0;
	n->style.order = 0;
	n->style.box_sizing = 0;
	n->style.float_side = 0;
	n->style.clear_side = 0;
	n->style.writing_mode = -1;
	n->style.direction = -1;
	n->style.margin_auto = 0;
	n->style.row_gap = n->style.column_gap = kUnset;
	n->style.row_gap_percent = n->style.column_gap_percent = kUnset;
	// Grid tracks moved to RareStyle (default 0 for a node with no rare_style),
	// so a fresh node needs no explicit reset here.
	n->style.width_expression = n->style.height_expression = -1;
	n->style.width = kUnset;
	n->style.height = kUnset;
	n->style.width_percent = kUnset;
	n->style.height_percent = kUnset;
	n->style.min_width = kUnset;
	n->style.min_height = kUnset;
	n->style.max_width = kUnset;
	n->style.max_height = kUnset;
	n->style.flex = 0;
	n->style.flex_shrink = 1;
	n->style.flex_basis = kUnset;
	for (int i = 0; i < 4; i++) {
		n->style.padding[i] = 0;
		n->style.margin[i] = 0;
		n->style.pos_offsets[i] = kUnset;
		n->style.pos_offset_percent[i] = kUnset;
		// per-side border width/color/alpha moved to RareStyle (defaults there).
		n->style.border_radius[i] = 0;
		n->style.border_radius_percent[i] = kUnset;
	}
	n->style.position = 0;
	n->style.z_index = 0;
	n->style.z_index_auto = 1;
	n->style.bg_color = 0;
	n->style.has_bg = 0;
	n->style.bg_alpha = 0;
	n->style.bg_fill = 0;
	// gradients + background-grid moved to RareStyle (defaults there).
	n->style.active_bg_color = 0;
	n->style.has_active_bg = 0;
	n->style.text_color = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
	n->style.text_alpha = 255;
	n->style.opacity = 255;
	n->style.blink_interval_ms = 0;
	n->style.blink_started_ms = 0;
	n->style.blink_visible = 1;
	n->style.border_width = 0;
	n->style.border_color = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
	n->style.border_alpha = 255;
	// transform / perspective / filter / box-shadow moved to RareStyle; a fresh
	// node (rare_style = -1) reads their correct defaults from the zero entry.
	n->style.font_id = -1;
	n->style.font_size = 0;
	n->style.font_weight = 400;
	n->style.line_height = 0;
	n->style.line_height_multiplier = -1;
	n->style.text_align = 0;
	n->style.text_decoration = 0;
	n->style.text_transform = 0;
	n->style.white_space = 0;
	n->style.text_overflow = 0;
	n->style.overflow = 0;
	n->style.overflow_x = 0;
	n->style.overflow_y = 0;
	n->style.mask_right_fade_width = 0;
	n->text.clear();
	n->image_id = -1;
	n->tag_id = 0;  // 0 = empty tag (interned tag table index 0)
	n->style.image_fit = 0;
	n->style.backface_hidden = 0;
	n->style.visibility = 0;
	n->style.pointer_events = 0;
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
	n->layout.scroll_x = 0;
	n->layout.scroll_y = 0;
	n->layout.scroll_content_width = 0;
	n->layout.scroll_content_height = 0;
	n->layout.previous_x = 0;
	n->layout.previous_y = 0;
	n->layout.previous_width = 0;
	n->layout.previous_height = 0;
	n->layout.previous_scroll_x = 0;
	n->layout.previous_scroll_y = 0;
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
	n->render.previous_filter_blur_radius = 0;
	n->render.inline_baseline = 0;
	n->render.dirty = 0;
	n->render.scroll_dirty = 0;
	n->render.non_scroll_dirty = 0;
	n->render.transform_dirty = 0;
	n->render.text_layout_stable = 0;
}

}  // namespace gea::embedded::ui
