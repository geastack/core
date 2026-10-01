#include "ui/tree_state.h"
#ifndef GEA_CSS_POSITION_PX
#define GEA_CSS_POSITION_PX(style, side) ((style).pos_offsets[side])
#define GEA_CSS_POSITION_PERCENT(style, side) ((style).pos_offset_percent[side])
#endif
using namespace gea::embedded::ui;
extern "C" {
char css_size_node[sizeof(Node)];
__attribute__((noinline)) int css_probe_indexed_width(const TreeState *state, unsigned id) { return state->nodes[id].style.width; }
__attribute__((noinline)) int css_probe_indexed_background(const TreeState *state, unsigned id) { return state->nodes[id].style.bg_color; }
__attribute__((noinline)) int css_probe_indexed_geometry(const TreeState *state, unsigned id) { const auto &n = state->nodes[id]; return n.layout.x + n.layout.y + n.layout.width + n.layout.height; }
char css_size_tree[sizeof(TreeState)];
char css_size_rare[sizeof(RareStyle)];
char css_size_style[sizeof(ComputedStyle)];
char css_size_layout[sizeof(LayoutBox)];
char css_size_render[sizeof(RenderState)];
char css_size_overrides[sizeof(NodeStyleOverrideStore)];
char css_size_rare_data[sizeof(NodeRareData)];
char css_size_custom_property[sizeof(NodeCustomProperty)];
__attribute__((noinline)) int css_probe_position_px_0(const Node *node) { return GEA_CSS_POSITION_PX(node->style, 0); }
__attribute__((noinline)) int css_probe_position_px_1(const Node *node) { return GEA_CSS_POSITION_PX(node->style, 1); }
__attribute__((noinline)) int css_probe_position_px_2(const Node *node) { return GEA_CSS_POSITION_PX(node->style, 2); }
__attribute__((noinline)) int css_probe_position_px_3(const Node *node) { return GEA_CSS_POSITION_PX(node->style, 3); }
__attribute__((noinline)) int css_probe_position_percent_0(const Node *node) { return GEA_CSS_POSITION_PERCENT(node->style, 0); }
__attribute__((noinline)) int css_probe_position_percent_1(const Node *node) { return GEA_CSS_POSITION_PERCENT(node->style, 1); }
__attribute__((noinline)) int css_probe_position_percent_2(const Node *node) { return GEA_CSS_POSITION_PERCENT(node->style, 2); }
__attribute__((noinline)) int css_probe_position_percent_3(const Node *node) { return GEA_CSS_POSITION_PERCENT(node->style, 3); }
char css_size_text[sizeof(NodeText)];
char css_size_string[sizeof(std::string)];
__attribute__((noinline)) int css_probe_corner3(const Node *node) { return node->style.border_radius[GEA_CSS_RADIUS_INDEX(3)]; }
__attribute__((noinline)) int css_probe_corner_dynamic(const Node *node, int corner) { return node->style.border_radius[GEA_CSS_RADIUS_INDEX(corner)]; }
__attribute__((noinline)) unsigned css_probe_memo_pass(const Node *node) { return node->layout.memo_pass; }
__attribute__((noinline)) int css_probe_overflow_x(const Node *node) { return overflowX(node->style); }
__attribute__((noinline)) int css_probe_overflow_y(const Node *node) { return overflowY(node->style); }
__attribute__((noinline)) int css_probe_gap(const Node *node) { return node->style.gap; }
__attribute__((noinline)) int css_probe_padding(const Node *node) { return node->style.padding[0]; }
__attribute__((noinline)) int css_probe_border_width(const Node *node) { return node->style.border_width; }
__attribute__((noinline)) int css_probe_font_size(const Node *node) { return node->style.font_size; }
__attribute__((noinline)) int css_probe_line_height(const Node *node) { return node->style.line_height; }
__attribute__((noinline)) int css_probe_flex(const Node *node) { return node->style.flex; }
__attribute__((noinline)) int css_probe_flex_shrink(const Node *node) { return node->style.flex_shrink; }
__attribute__((noinline)) int css_probe_border_flags(const Node *node) { return node->style.border_color_flags; }
__attribute__((noinline)) int css_probe_cached_style_color(const NodeCustomProperty *entry) { return entry->colorStyle; }
__attribute__((noinline)) int css_probe_cached_native_color(const NodeCustomProperty *entry) { return entry->colorNative; }
// Inspect these with the target objdump: field elimination must leave direct
// loads for used properties and constants for absent ones, without lookups.
__attribute__((noinline)) int css_probe_recolor_from(const Node *node) { return node->render.bg_recolor_from; }
__attribute__((noinline)) int css_probe_text_dirty_x0(const Node *node) { return node->render.text_dirty.x0; }
__attribute__((noinline)) int css_probe_width(const Node *node) { return node->style.width; }
__attribute__((noinline)) int css_probe_background(const Node *node) { return node->style.bg_color; }
__attribute__((noinline)) int css_probe_text_color(const Node *node) { return node->style.text_color; }
__attribute__((noinline)) int css_probe_opacity(const Node *node) { return node->style.opacity; }
__attribute__((noinline)) int css_probe_order(const Node *node) { return node->style.order; }
__attribute__((noinline)) int css_probe_blink(const Node *node) { return node->style.blink_interval_ms > 0 && !node->style.blink_visible; }
}
