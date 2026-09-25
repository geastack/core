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

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace gea::embedded::ui {

namespace {

// lv_theme_default's textarea cursor: a 2 px left border, pad_left -1, so it
// sits one pixel left of the letter and spans the font's line height plus the
// border above and below.
constexpr int kInputCaretWidth = 2;
constexpr int kInputCaretOutset = 2;
// Match text.cpp's local kBitmapFontHeight — used as the implicit font size
// when an input doesn't have an explicit font_size set, so the vertical
// centering formula here matches the height the framework's bitmap text path
// would draw at by default.
constexpr int kBitmapFontHeight = gea::framework::graphics::BitmapFont8x16::kHeight;
// #475569 (slate-600), packed in the target native color format.
inline gea::framework::graphics::pixel::native_t kPlaceholderColor()
{
	return gea::framework::graphics::pixel::nativeColor(71, 85, 105);
}

bool isPasswordInput(int id)
{
	const char *type = Tree::instance().getAttribute(id, "type");
	return type && std::strcmp(type, "password") == 0;
}

// lv_textarea's password bullet (LV_TEXTAREA_PWD_BULLET_UNICODE).
constexpr const char *kPasswordBullet = "\xE2\x80\xA2";

// What a password input shows for value[0, end): a bullet per character,
// except the ones typed in the last burst, still shown in clear.
std::string passwordDisplay(int id, const char *value, int end)
{
	int from = 0, to = 0;
	const bool reveal = Tree::instance().inputPasswordReveal(id, &from, &to);
	std::string out;
	for (int i = 0; i < end && value[i]; ++i) {
		if (reveal && i >= from && i < to) out.push_back(value[i]);
		else out += kPasswordBullet;
	}
	return out;
}

// The display string of each password input, kept alive for its DrawText.
std::unordered_map<int, std::string> &passwordTexts()
{
	static std::unordered_map<int, std::string> texts;
	return texts;
}

const char *inputContentText(int id, gea::framework::graphics::pixel::native_t *outColor, gea::framework::graphics::pixel::native_t textColor)
{
	auto &tree = Tree::instance();
	const char *value = tree.getAttribute(id, "value");
	if (value && value[0]) {
		if (outColor) *outColor = textColor;
		if (isPasswordInput(id)) {
			std::string &text = passwordTexts()[id];
			text = passwordDisplay(id, value, static_cast<int>(std::strlen(value)));
			return text.c_str();
		}
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

// How far each input's one line is scrolled left so its caret stays in view
// (lv_textarea one-line mode scrolls the label the same way).
std::unordered_map<int, int> &inputScrollX()
{
	static std::unordered_map<int, int> scroll;
	return scroll;
}

const char *inputMeasureText(int id)
{
	const char *content = inputContentText(id, nullptr, 0);
	return content && content[0] ? content : "M";
}

}  // namespace

void InputRenderer::layout(int id, int)
{
	if (!GEA_UI_INPUT_NODES) return;
	auto &tree = Tree::instance();
	if (id < 0 || id >= tree.nodeCount()) return;
	Node &n = tree.node(id);
	if (n.type != NodeType::View) return;
	if (std::strcmp(tagFromId(n.tag_id), "input") != 0) return;

	const char *content = inputContentText(id, nullptr, n.computedStyle().text_color);
	const char *measureText = inputMeasureText(id);
	int width = TextRenderer::measureWidth(content, n.computedStyle().font_id, n.computedStyle().font_size, n.computedStyle().text_transform) + boxInset(n.computedStyle(), 1) + boxInset(n.computedStyle(), 3);
	int height = TextRenderer::measureHeight(measureText, n.computedStyle().font_id, n.computedStyle().font_size, n.computedStyle().text_transform, n.computedStyle().line_height) + boxInset(n.computedStyle(), 0) + boxInset(n.computedStyle(), 2);

	if (n.computedStyle().width != kUnset) width = contentSizeToBorderSize(n.computedStyle(), n.computedStyle().width, true);
	else if ((n.computedStyle().width_percent != kUnset || n.computedStyle().width_expression >= 0) && n.layout.width > 0) width = n.layout.width;
	if (n.computedStyle().height != kUnset) height = contentSizeToBorderSize(n.computedStyle(), n.computedStyle().height, false);
	else if ((n.computedStyle().height_percent != kUnset || n.computedStyle().height_expression >= 0) && n.layout.height > 0) height = n.layout.height;
	n.layout.width = clampBorderBoxSize(n.computedStyle(), width, true);
	n.layout.height = clampBorderBoxSize(n.computedStyle(), height, false);
#if GEA_CSS_SCROLLING
	n.layout.scroll_content_height = n.layout.height;
	n.layout.scroll_y = 0;
#endif
}

void InputRenderer::record(int id)
{
	if (!GEA_UI_INPUT_NODES) return;
	auto &tree = Tree::instance();
	if (id < 0 || id >= tree.nodeCount()) return;
	const Node &n = tree.node(id);
	if (n.type != NodeType::View) return;
	if (std::strcmp(tagFromId(n.tag_id), "input") != 0) return;

	gea::framework::graphics::pixel::native_t color = n.computedStyle().text_color;
	const char *content = inputContentText(id, &color, n.computedStyle().text_color);

	const int x = n.layout.x;
	const int y = n.layout.y;
	const int w = n.layout.width;
	const int h = n.layout.height;
	const int padLeft = boxInset(n.computedStyle(), 3);
	const int padRight = boxInset(n.computedStyle(), 1);
	const int padTop = boxInset(n.computedStyle(), 0);
	const int padBottom = boxInset(n.computedStyle(), 2);
	const int contentX = x + padLeft;
	const int contentY = y + padTop;
	int contentW = w - padLeft - padRight;
	int contentH = h - padTop - padBottom;
	if (contentW < 0) contentW = 0;
	if (contentH < 0) contentH = 0;

	const auto &style = n.computedStyle();
	const bool focused = tree.activeInputId() == id;
	const char *valueAttr = tree.getAttribute(id, "value");
	const bool hasValue = valueAttr && valueAttr[0];
	// The width of what is shown before the caret (bullets on a password).
	int caretPrefixW = -1;
	if (focused && hasValue) {
		const int caret = tree.activeInputCaret();
		const std::string prefix = isPasswordInput(id) ? passwordDisplay(id, valueAttr, caret) : std::string(valueAttr, caret);
		caretPrefixW = TextRenderer::measureWidth(prefix.c_str(), style.font_id, style.font_size, style.text_transform);
	}

	// An input is one line: its text never wraps. When it is wider than the
	// box, the focused input scrolls it so the caret stays inside; an
	// unfocused one shows its start. Either way it is clipped to the box.
	int textW = content ? TextRenderer::measureWidth(content, style.font_id, style.font_size, style.text_transform) : 0;
	int &scroll = inputScrollX()[id];
	const int room = contentW - kInputCaretWidth;
	if (!focused || !hasValue || textW <= room) {
		scroll = 0;
	} else {
		if (caretPrefixW - scroll > room) scroll = caretPrefixW - room;
		if (caretPrefixW < scroll) scroll = caretPrefixW;
		const int maxScroll = textW - room;
		if (scroll > maxScroll) scroll = maxScroll;
		if (scroll < 0) scroll = 0;
	}
	const bool scrolled = focused && hasValue && textW > room;

	// The clip: the content box widened by the caret's 1 px left offset, the
	// node's full height (the caret overhangs the text by its outset).
	const int clipX = contentX - 1;
	const int clipW = contentW + 1;
	const bool clip = clipW > 0 && h > 0;
	if (clip) {
		if (DisplayCommand *begin = DisplayList::instance().append()) {
			begin->type = DisplayCommandType::PushClip;
			begin->bx = begin->clip.x = static_cast<int16_t>(clipX);
			begin->by = begin->clip.y = static_cast<int16_t>(y);
			begin->bw = begin->clip.w = static_cast<int16_t>(clipW);
			begin->bh = begin->clip.h = static_cast<int16_t>(h);
			begin->clip.nodeId = -1;
		}
	}

	if (content) {
		// Matches TextRenderer::record's scale formula. The denominator is
		// the bitmap font's height (BitmapFont8x16::kHeight = 16). This
		// scale is ignored when font_id selects a rasterized TTF — the font
		// system rasterizes at font_size directly — but it still needs to
		// be non-zero to keep the bitmap path's math consistent for callers
		// without `font-family` set.
		const float textScale = style.font_size > 0
		                            ? static_cast<float>(style.font_size) / 16.0f
		                            : 1.0f;

		DisplayCommand *cmd = DisplayList::instance().append();
		if (cmd) {
			cmd->type = DisplayCommandType::DrawText;
			cmd->bx = x; cmd->by = y; cmd->bw = w; cmd->bh = h;
			// `getAttribute` returns a pointer to the in-tree attribute slot, so
			// the string is stable until the attribute is overwritten. The
			// DisplayList commands are replayed within the same frame, so this
			// stays valid for the duration of the present.
			cmd->text.text = content;
			// A scrolled line starts left of the box and is as wide as its text.
			cmd->text.x = scrolled ? contentX - scroll : contentX;
			// Vertically center the single-line text within the content area.
			// 16 = BitmapFont8x16::kHeight (default when no font_size set).
			const int fontSize = style.font_size > 0 ? style.font_size : 16;
			const int yCenter = contentY + (contentH - fontSize) / 2;
			cmd->text.y = yCenter > contentY ? yCenter : contentY;
			cmd->text.maxWidth = scrolled ? textW + 1 : contentW;
			cmd->text.color = color;
			cmd->text.scale = textScale;
			cmd->text.align = scrolled ? 0 : static_cast<int8_t>(LayoutEngine::physicalTextAlign(n));
			cmd->text.textTransform = style.text_transform;
			cmd->text.lineHeight = style.line_height;
			cmd->text.containerWidth = scrolled ? textW + 1 : contentW;
			cmd->text.fontId = style.font_id;
			// append() hands back uninitialized scratch — set these explicitly.
			// white-space is always nowrap: wrapping put long text on a second
			// line that spilled out of the box.
			cmd->text.whiteSpace = 1;
			cmd->text.textOverflow = scrolled ? 0 : style.text_overflow;
			cmd->text.maxHeight = 0;
			cmd->text.firstLineIndent = 0;
			cmd->text.alignLast = -1;
			cmd->text.lineLimit = 0;
			cmd->text.blockEllipsis = 0;
			cmd->text.ellipsisWidth = 0;
		}
	}

	// Caret — drawn only when this input is the active focus. The frame loop
	// in tree_render.cpp flips the caret-blink phase via inputTickRequired,
	// which Tree::activeInputCaretVisible() returns. Showing the caret while
	// unfocused would be misleading on targets where the framework handles
	// the input's visual editing state.
	if (focused && tree.activeInputCaretVisible()) {
		// At the caret, from the content's left edge, less the scroll; at the
		// start when the placeholder shows.
		int caretX = contentX - 1;
		if (caretPrefixW >= 0) {
			const int shown = caretPrefixW - scroll;
			caretX += shown < room ? shown : room;
		}
		const int fontSize = style.font_size > 0 ? style.font_size : 16;
		const int lineHeight = TextRenderer::measureHeight("M", style.font_id, style.font_size, style.text_transform, style.line_height);
		int textY = contentY + (contentH - fontSize) / 2;
		if (textY < contentY) textY = contentY;
		const int caretY = textY - kInputCaretOutset;
		const int caretH = lineHeight + kInputCaretOutset * 2;
		// caret-color, else the text colour (a placeholder's grey is not it).
		gea::framework::graphics::pixel::native_t caretColor = style.text_color ? style.text_color : gea::framework::graphics::pixel::nativeColor(255, 255, 255);
		if (const char *cc = tree.getAttribute(id, "caret-color"); cc && cc[0] == '#' && std::strlen(cc) >= 7) {
			const unsigned long rgb = std::strtoul(cc + 1, nullptr, 16);
			caretColor = gea::framework::graphics::pixel::nativeColor((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
		}

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

	if (clip) {
		if (DisplayCommand *end = DisplayList::instance().append()) {
			end->type = DisplayCommandType::PopClip;
			end->bx = static_cast<int16_t>(clipX); end->by = static_cast<int16_t>(y);
			end->bw = static_cast<int16_t>(clipW); end->bh = static_cast<int16_t>(h);
			end->clip.nodeId = -1;
		}
	}
}

}  // namespace gea::embedded::ui
