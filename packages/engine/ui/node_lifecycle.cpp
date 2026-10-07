// SPDX-License-Identifier: Apache-2.0
#include "node_lifecycle.h"

#include "pixel.h"

#include <cstring>
#include <cstdlib>
#include <new>
#include <utility>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace gea::embedded::ui {

namespace {
// One immutable default image. Resetting CSS must not construct/destroy a whole
// Node, touch its text ownership, or initialize layout/render state on the stack.
constexpr ComputedStyle makeDefaultStyle()
{
	ComputedStyle style{};
	style.display = kDisplayBlock;
#if GEA_CSS_DISPLAY_EXPLICIT
	style.display_explicit = 0;
#endif
#if GEA_CSS_FLEX_DIRECTION
	style.flex_direction = 0;
#endif
#if GEA_CSS_FLEX_DIRECTION
	style.flex_direction_explicit = 0;
#endif
#if GEA_CSS_FLEX_WRAP
	style.flex_wrap = 0;
#endif
#if GEA_CSS_JUSTIFY_CONTENT
	style.justify_content = 0;
#endif
#if GEA_CSS_ALIGN_ITEMS
	style.align_items = 0;
#endif
#if GEA_CSS_JUSTIFY_ITEMS
	style.justify_items = 0;
#endif
#if GEA_CSS_ALIGN_CONTENT
	style.align_content = 0;
#endif
#if GEA_CSS_ALIGN_SELF
	style.align_self = -1;
#endif
#if GEA_CSS_GAP
	style.gap = 0;
#endif
#if GEA_CSS_ORDER
	style.order = 0;
#endif
#if GEA_CSS_BOX_SIZING
	style.box_sizing = 0;
#endif
#if GEA_CSS_FLOATS
	style.float_side = 0;
	style.clear_side = 0;
#endif
#if GEA_CSS_WRITING_MODE
	style.writing_mode = -1;
	style.direction = -1;
#endif
#if GEA_CSS_MARGIN_AUTO
	style.margin_auto = 0;
#endif
#if GEA_CSS_AXIS_GAP
	style.row_gap = style.column_gap = kUnset;
#endif
#if GEA_CSS_PERCENT_GAP
	style.row_gap_percent = style.column_gap_percent = kUnset;
#endif
	// Grid tracks moved to RareStyle (default 0 for a node with no rare_style),
	// so a fresh node needs no explicit reset here.
#if GEA_CSS_WIDTH_EXPRESSIONS
	style.width_expression = -1;
#endif
#if GEA_CSS_HEIGHT_EXPRESSIONS
	style.height_expression = -1;
#endif
	style.width = kUnset;
	style.height = kUnset;
#if GEA_CSS_WIDTH_PERCENT
	style.width_percent = kUnset;
#endif
#if GEA_CSS_HEIGHT_PERCENT
	style.height_percent = kUnset;
#endif
#if GEA_CSS_MIN_WIDTH
	style.min_width = kUnset;
#endif
#if GEA_CSS_MIN_HEIGHT
	style.min_height = kUnset;
#endif
#if GEA_CSS_MAX_WIDTH
	style.max_width = kUnset;
#endif
#if GEA_CSS_MAX_HEIGHT
	style.max_height = kUnset;
#endif
#if GEA_CSS_FLEX_FACTORS
	style.flex = 0;
#endif
#if GEA_CSS_FLEX_FACTORS
	style.flex_shrink = 1;
#endif
#if GEA_CSS_FLEX_BASIS
	style.flex_basis = kUnset;
#endif
	for (int i = 0; i < 4; i++) {
#if GEA_CSS_PADDING
		style.padding[i] = 0;
#endif
#if GEA_CSS_MARGINS
		style.margin[i] = 0;
#endif
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
#if GEA_CSS_ACTIVE_BACKGROUND
	style.active_bg_color = 0;
#endif
#if GEA_CSS_ACTIVE_BACKGROUND
	style.has_active_bg = 0;
#endif
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
#if GEA_CSS_BORDER_COLORS
	style.border_color_flags = 0;
#endif
#if GEA_CSS_BORDER_WIDTHS
	style.border_width = 0;
#endif
#if GEA_CSS_BORDER_COLORS
	style.border_color = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
#endif
#if GEA_CSS_BORDER_ALPHA
	style.border_alpha = 255;
#endif
	// transform / perspective / filter / box-shadow moved to RareStyle; a fresh
	// node (rare_style = -1) reads their correct defaults from the zero entry.
	style.font_id = -1;
	style.font_size = 0;
#if GEA_CSS_FONT_WEIGHT
	style.font_weight = 400;
#endif
#if GEA_CSS_LINE_HEIGHT
	style.line_height = 0;
#endif
#if GEA_CSS_LINE_HEIGHT_MULTIPLIER
	style.line_height_multiplier = -1;
#endif
#if GEA_CSS_TEXT_ALIGN
	style.text_align = 0;
#endif
#if GEA_CSS_TEXT_DECORATION
	style.text_decoration = 0;
#endif
#if GEA_CSS_TEXT_TRANSFORM
	style.text_transform = 0;
#endif
#if GEA_CSS_WHITE_SPACE
	style.white_space = 0;
#endif
#if GEA_CSS_TEXT_OVERFLOW
	style.text_overflow = 0;
#endif
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

#if GEA_EMBEDDED_SHARED_STYLES
namespace {
SharedStyleRecord *g_sharedStyleHead = nullptr;
std::size_t g_sharedStyleRecords = 0;
SharedStyleRecord &defaultStyleRecord()
{
	static SharedStyleRecord record{kDefaultStyle, 0, nullptr};
	return record;
}
void retainStyle(SharedStyleRecord *record)
{
	if (record != &defaultStyleRecord()) ++record->references;
}
void releaseStyle(SharedStyleRecord *record)
{
	if (record == &defaultStyleRecord() || --record->references != 0) return;
	// Reclamation is cold; keep no backward link in every style record.
	// The per-frame read/write path still uses the same direct record pointer.
	auto **link = &g_sharedStyleHead;
	while (*link && *link != record) link = &(*link)->next;
	if (!*link) std::abort();
	*link = record->next;
	record->~SharedStyleRecord();
#if defined(ESP_PLATFORM)
	heap_caps_free(record);
#else
	std::free(record);
#endif
	--g_sharedStyleRecords;
}
SharedStyleRecord *copyStyle(const ComputedStyle &value)
{
#if defined(ESP_PLATFORM)
	void *storage = heap_caps_malloc(sizeof(SharedStyleRecord), MALLOC_CAP_SPIRAM);
#else
	void *storage = std::malloc(sizeof(SharedStyleRecord));
#endif
	if (!storage) std::abort();
	auto *record = new (storage) SharedStyleRecord{value, 1, g_sharedStyleHead};
	g_sharedStyleHead = record;
	++g_sharedStyleRecords;
	return record;
}
}
NodeStyleStorage::NodeStyleStorage() : record_(&defaultStyleRecord()) {}
NodeStyleStorage::~NodeStyleStorage() { releaseStyle(record_); }
NodeStyleStorage::NodeStyleStorage(const NodeStyleStorage &other) : record_(other.record_) { retainStyle(record_); }
NodeStyleStorage &NodeStyleStorage::operator=(const NodeStyleStorage &other)
{
	if (this != &other) { retainStyle(other.record_); releaseStyle(record_); record_ = other.record_; }
	return *this;
}
NodeStyleStorage::NodeStyleStorage(NodeStyleStorage &&other) noexcept
	: record_(std::exchange(other.record_, &defaultStyleRecord())) {}
NodeStyleStorage &NodeStyleStorage::operator=(NodeStyleStorage &&other) noexcept
{
	if (this != &other) { releaseStyle(record_); record_ = std::exchange(other.record_, &defaultStyleRecord()); }
	return *this;
}
ComputedStyle &NodeStyleStorage::detach()
{
	auto *copy = copyStyle(record_->value);
	releaseStyle(record_);
	record_ = copy;
	return record_->value;
}
void NodeStyleStorage::reset() { releaseStyle(record_); record_ = &defaultStyleRecord(); }
void NodeStyleStorage::intern()
{
	if (record_ == &defaultStyleRecord() || record_->value.rare_style >= 0) return;
	// Called at the end of CSS recomputation, never in the per-frame read path.
	for (auto *other = g_sharedStyleHead; other; other = other->next) {
		if (other == record_ || !(other->value == record_->value)) continue;
		retainStyle(other);
		releaseStyle(record_);
		record_ = other;
		return;
	}
}
std::size_t NodeStyleStorage::allocatedBytes() { return g_sharedStyleRecords * sizeof(SharedStyleRecord); }
std::size_t NodeStyleStorage::allocatedRecords() { return g_sharedStyleRecords; }
std::size_t NodeStyleStorage::allocatedHeapBytes()
{
#if defined(ESP_PLATFORM)
    std::size_t bytes = 0;
    for (auto *record = g_sharedStyleHead; record; record = record->next)
        bytes += heap_caps_get_allocated_size(record);
    return bytes;
#else
    return allocatedBytes();
#endif
}

#else
void Node::resetComputedStyle() { NodeLifecycle::resetStyle(style); }
#endif

void NodeLifecycle::init(Node *n, NodeType type)
{
#if defined(GEA_NATIVE_DEBUGGER) && GEA_NATIVE_DEBUGGER
	static uint32_t nextDebuggerIdentity = 0;
	// Two protocol ids per native node (element and optional text child).
	if (nextDebuggerIdentity >= 0x3fffffff) std::abort();
	n->debugger_identity = ++nextDebuggerIdentity;
#endif
	n->type = type;
#if GEA_EMBEDDED_SHARED_STYLES && !GEA_CSS_CUSTOM_PROPERTIES
	n->class_style_tracked = false;
#endif
	n->resetComputedStyle();
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
#if !GEA_EMBEDDED_SHARED_STYLES
	n->layout.memo_avail_w = n->layout.memo_avail_h = 0;
	n->layout.memo_pass = 0;
#endif
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
	n->render.canvas_pixels_only_dirty = 0;
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
