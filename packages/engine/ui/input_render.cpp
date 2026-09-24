// SPDX-License-Identifier: Apache-2.0
// `<input>` rendering for the pixel-backed display targets (ESP32 etc.).
//
// JSX `<input>` falls through createElement to createView() (host_document_gea
// only special-cases button/canvas/img), so input elements are View nodes with
// tag_name == "input". The framework's View pipeline draws the box / border /
// background; this file handles the per-input details that diverge from a
// plain View:
//
// 1. The user-visible text comes from the node's `value` attribute (mirrors
//    DOM `input.value`), falling back to `placeholder` when value is empty.
//    The macOS target paints this with an editable NSTextField; on the pixel
//    target we draw the text as a DrawText display command using the same
//    glyph pipeline TextRenderer uses, but with the framework reading the
//    string from the attribute store rather than the Node::text buffer.
//
// 2. When the input is the active focus (Tree::activeInputId() returns its
//    id) we additionally record a thin vertical caret at the end of the
//    rendered text for pixel targets that keep input rendering in the
//    framework.
//
// Note: this file does NOT manage focus or keyboard services themselves —
// those live in tree_events.cpp / input.cpp / virtual_keyboard.cpp. We only
// paint based on the state they expose via Tree.

#include "internal.h"
#include "graphics/font.h"
#include "pixel.h"
#include "tree_internal.h"

#include <cstring>

namespace gea::embedded::ui {

namespace {

constexpr int kInputCaretWidth = 2;
constexpr int kInputCaretInsetY = 6;
// Match text.cpp's local kBitmapFontHeight — used as the implicit font size
// when an input doesn't have an explicit font_size set, so the vertical
// centering formula here matches the height the framework's bitmap text path
// would draw at by default.
constexpr int kBitmapFontHeight = gea::framework::graphics::BitmapFont8x16::kHeight;
// #475569 (slate-600). Computed via packRgb565Components because the
// display layer byte-swaps the wire format — a raw 0x4A69 literal would
// land as BGR garbage (the "purple placeholder" symptom).
inline std::uint16_t kPlaceholderColor()
{
	return gea::framework::graphics::pixel::packRgb565Components(71 / 8, 85 / 4, 105 / 8);
}

const char *inputContentText(int id, std::uint16_t *outColor, std::uint16_t textColor)
{
	auto &tree = Tree::instance();
	const char *value = tree.getAttribute(id, "value");
	if (value && value[0]) {
		if (outColor) *outColor = textColor;
		return value;
	}
	const char *placeholder = tree.getAttribute(id, "placeholder");
	if (placeholder && placeholder[0]) {
		if (outColor) {
			const gea::framework::graphics::pixel::native_t defaultTextColor = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
			*outColor = textColor != defaultTextColor ? textColor : kPlaceholderColor();
		}
		return placeholder;
	}
	return nullptr;
}

const char *inputMeasureText(int id)
{
	const char *content = inputContentText(id, nullptr, 0);
	return content && content[0] ? content : "M";
}

}  // namespace

void InputRenderer::layout(int id, int)
{
	auto &tree = Tree::instance();
	if (id < 0 || id >= tree.nodeCount()) return;
	Node &n = tree.node(id);
	if (n.type != NodeType::View) return;
	if (std::strcmp(tagFromId(n.tag_id), "input") != 0) return;

	const char *content = inputContentText(id, nullptr, n.style.text_color);
	const char *measureText = inputMeasureText(id);
	int width = TextRenderer::measureWidth(content, n.style.font_id, n.style.font_size, n.style.text_transform) + boxInset(n.style, 1) + boxInset(n.style, 3);
	int height = TextRenderer::measureHeight(measureText, n.style.font_id, n.style.font_size, n.style.text_transform, n.style.line_height) + boxInset(n.style, 0) + boxInset(n.style, 2);

	if (n.style.width != kUnset) width = contentSizeToBorderSize(n.style, n.style.width, true);
	else if ((n.style.width_percent != kUnset || n.style.width_expression >= 0) && n.layout.width > 0) width = n.layout.width;
	if (n.style.height != kUnset) height = contentSizeToBorderSize(n.style, n.style.height, false);
	else if ((n.style.height_percent != kUnset || n.style.height_expression >= 0) && n.layout.height > 0) height = n.layout.height;
	n.layout.width = clampBorderBoxSize(n.style, width, true);
	n.layout.height = clampBorderBoxSize(n.style, height, false);
	n.layout.scroll_content_height = n.layout.height;
	n.layout.scroll_y = 0;
}

void InputRenderer::record(int id)
{
	auto &tree = Tree::instance();
	if (id < 0 || id >= tree.nodeCount()) return;
	const Node &n = tree.node(id);
	if (n.type != NodeType::View) return;
	if (std::strcmp(tagFromId(n.tag_id), "input") != 0) return;

	std::uint16_t color = n.style.text_color;
	const char *content = inputContentText(id, &color, n.style.text_color);

	const int x = n.layout.x;
	const int y = n.layout.y;
	const int w = n.layout.width;
	const int h = n.layout.height;
	const int padLeft = boxInset(n.style, 3);
	const int padRight = boxInset(n.style, 1);
	const int padTop = boxInset(n.style, 0);
	const int padBottom = boxInset(n.style, 2);
	const int contentX = x + padLeft;
	const int contentY = y + padTop;
	int contentW = w - padLeft - padRight;
	int contentH = h - padTop - padBottom;
	if (contentW < 0) contentW = 0;
	if (contentH < 0) contentH = 0;

	if (content) {
		// Matches TextRenderer::record's scale formula. The denominator is
		// the bitmap font's height (BitmapFont8x16::kHeight = 16). This
		// scale is ignored when font_id selects a rasterized TTF — the font
		// system rasterizes at font_size directly — but it still needs to
		// be non-zero to keep the bitmap path's math consistent for callers
		// without `font-family` set.
		const float textScale = n.style.font_size > 0
		                            ? static_cast<float>(n.style.font_size) / 16.0f
		                            : 1.0f;

		DisplayCommand *cmd = DisplayList::instance().append();
		if (cmd) {
			cmd->type = DisplayCommandType::DrawText;
			cmd->bx = x; cmd->by = y; cmd->bw = w; cmd->bh = h;
			cmd->text.text = content;
			// `getAttribute` returns a pointer to the in-tree attribute slot, so
			// the string is stable until the attribute is overwritten. The
			// DisplayList commands are replayed within the same frame, so this
			// stays valid for the duration of the present.
			cmd->text.x = contentX;
			// Vertically center the single-line text within the content area —
			// inputs are conventionally one line and the framework's text
			// renderer would otherwise top-align.
			// 16 = BitmapFont8x16::kHeight (default when no font_size set).
			const int fontSize = n.style.font_size > 0 ? n.style.font_size : 16;
			const int yCenter = contentY + (contentH - fontSize) / 2;
			cmd->text.y = yCenter > contentY ? yCenter : contentY;
			cmd->text.maxWidth = contentW;
			cmd->text.color = color;
			cmd->text.scale = textScale;
			cmd->text.align = n.style.text_align;
			cmd->text.textTransform = n.style.text_transform;
			cmd->text.lineHeight = n.style.line_height;
			cmd->text.containerWidth = contentW;
			cmd->text.fontId = n.style.font_id;
			// append() hands back uninitialized scratch — set these explicitly so
			// the deferred draw doesn't read stale white-space/text-overflow.
			cmd->text.whiteSpace = n.style.white_space;
			cmd->text.textOverflow = n.style.text_overflow;
			cmd->text.maxHeight = 0;
			cmd->text.firstLineIndent = 0;
		}
	}

	// Caret — drawn only when this input is the active focus. The frame loop
	// in tree_render.cpp flips the caret-blink phase via inputTickRequired,
	// which Tree::activeInputCaretVisible() returns. Showing the caret while
	// unfocused would be misleading on targets where the framework handles
	// the input's visual editing state.
	if (tree.activeInputId() == id && tree.activeInputCaretVisible()) {
		// Caret sits at the END of the user-typed value (consistent with
		// the insertion point of a real text field). When `value` is
		// empty and we're falling back to the placeholder, the caret
		// goes at the START of the content box — typing the first
		// character would land there, and the placeholder gets visually
		// pushed aside by the appearing glyph. Measuring the placeholder
		// here would be wrong: it'd put the caret at the end of a
		// string the user isn't actually editing.
		const char *valueAttr = tree.getAttribute(id, "value");
		const bool hasValue = valueAttr && valueAttr[0];
		int caretX = contentX;
		if (hasValue) {
			const int textWidth = TextRenderer::measureWidth(valueAttr, n.style.font_id, n.style.font_size, n.style.text_transform);
			caretX += textWidth < contentW ? textWidth : contentW - kInputCaretWidth;
			if (caretX < contentX) caretX = contentX;
		}
		const int caretY = contentY + kInputCaretInsetY;
		int caretH = contentH - kInputCaretInsetY * 2;
		if (caretH < 4) caretH = contentH > 4 ? contentH - 2 : contentH;
		const std::uint16_t caretColor = n.style.text_color ? n.style.text_color : 0xFFFF;

		DisplayCommand *cmd = DisplayList::instance().append();
		if (cmd) {
			cmd->type = DisplayCommandType::FillRect;
			cmd->bx = x; cmd->by = y; cmd->bw = w; cmd->bh = h;
			cmd->fill.x = caretX;
			cmd->fill.y = caretY;
			cmd->fill.w = kInputCaretWidth;
			cmd->fill.h = caretH;
			cmd->fill.color = caretColor;
		}
	}
}

}  // namespace gea::embedded::ui
