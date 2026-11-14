#include "node.h"
// SPDX-License-Identifier: Apache-2.0
// On-screen keyboard implemented as document-tree nodes.
//
// Architecture (ported from the pre-geatsc C-codegen plugin's
// `emitSystemKeyboardVisualRuntime`, deleted in commit 071c591a):
//
//   - One "keyboard root" View, positioned absolute at the bottom of
//     the display, z-index above the app content.
//   - 4 row Views inside it (flex-direction:row). Each row holds up to
//     kColCount key Views; unused slots are display:none.
//   - Each key View holds a single Text label as its child.
//   - Each key View carries a unique pressId in
//     [kPressIdBase, kPressIdBase + kSlotCount). On Press, our listener
//     on body() checks event.pressId and routes the corresponding
//     keyCode into the focused input.
//
// Three layouts (modes): `alpha` (QWERTY), `symbols` (numeric +
// punctuation), `moreSymbols` (brackets / math). Mode-switch keys
// carry sentinel codes 1001 ("123" → symbols), 1002 ("ABC" → alpha),
// 1003 ("#+=" → moreSymbols). Shift (code 16) capitalizes the next
// character key in alpha mode; double-tap could enable caps lock but
// that's not wired through this port — single-shot only for now.
//
// Tap routing: when the keyboard mounts it calls
// `Tree::setEventListener(body, "click", …)`. That listener:
//   1. Reads event.pressId. If out of range, returns (lets app handler
//      that registered via JSX onClick fire normally — they're chained
//      by Tree::setEventListener).
//   2. Calls preventDefault() so the app's body-level click handlers
//      don't see the keyboard tap as a real app tap (they'd see the
//      keyboard's underlying View and fire spurious app actions).
//   3. Mutates the focused input's `value` attribute, marks the node
//      command-dirty so the input redraws, then synthesizes `input`
//      and `keydown` PointerEvents and dispatches them via
//      Tree::dispatchEvent so the app's `onInput` / `onKeyDown`
//      handlers fire.

#include "virtual_keyboard.h"
#include "node_features.h"

#ifndef GEA_EMBEDDED_ENABLE_VIRTUAL_KEYBOARD
#define GEA_EMBEDDED_ENABLE_VIRTUAL_KEYBOARD 1
#endif

#if GEA_EMBEDDED_ENABLE_VIRTUAL_KEYBOARD && GEA_UI_INPUT_NODES

#include "display.h"
#include "internal.h"
#include "tree_internal.h"
#include "tree_state.h"
#include "events.h"
#include "pixel.h"
#include "style.h"
#if GEA_UI_IMAGE_NODES
#include <image.h>
#include "virtual_keyboard_icons.h"
#endif

#include <cstdint>
#include <cstring>
#include <string>

namespace gea::embedded::ui {

namespace {

constexpr int kRowCount = 4;
// Slots per row: the compact layout uses ten, the wide (LVGL) one up to 13.
constexpr int kColCount = 13;
constexpr int kCompactColCount = 10;
constexpr int kSlotCount = kRowCount * kColCount;
constexpr int kPressIdBase = 30000;
constexpr int kKeyboardHeight = 192;

// Mode-switch / control sentinel codes (sit above the ASCII range so
// they never collide with a printable key).
constexpr int kCodeShift = 16;
constexpr int kCodeBackspace = 8;
constexpr int kCodeReturn = 13;
constexpr int kCodeSpace = 32;
constexpr int kCodeModeSymbols = 1001;
constexpr int kCodeModeAlpha = 1002;
constexpr int kCodeModeMoreSymbols = 1003;
constexpr int kCodeModeUpper = 1004;
constexpr int kCodeHide = 1005;
constexpr int kCodeOk = 1006;
constexpr int kCodeLeft = 1007;
constexpr int kCodeRight = 1008;

enum class KeyKind : std::uint8_t { Light = 0, Utility = 1, Primary = 2 };
enum class LabelKind : std::uint8_t { Normal = 0, Wide = 1, Small = 2 };
// Symbol keys of the wide layout; indexes keyboard_icons::kIcons (minus one).
enum class Icon : std::int8_t { None = 0, Backspace, NewLine, Keyboard, Left, Right, Ok };
constexpr int kIconCount = 6;

struct KeyDef {
	const char *label;
	int code;
	KeyKind kind;
	LabelKind labelKind;
	std::int16_t width;
	std::int16_t flex;
	Icon icon;
};

struct RowDef {
	int count;
	int gap;
	int paddingLeft;
	int paddingRight;
	KeyDef keys[kColCount];
};

// Macros to keep the layout tables readable. width=0+flex=1 means "share
// the row's leftover space"; width>0+flex=0 means "fixed width".
#define KEY_L(LABEL, CODE) {LABEL, CODE, KeyKind::Light, LabelKind::Normal, 0, 1}
#define KEY_U_SMALL(LABEL, CODE) {LABEL, CODE, KeyKind::Utility, LabelKind::Small, 58, 0}
#define KEY_P_SMALL(LABEL, CODE) {LABEL, CODE, KeyKind::Primary, LabelKind::Small, 86, 0}
#define KEY_L_WIDE(LABEL, CODE) {LABEL, CODE, KeyKind::Light, LabelKind::Wide, 0, 1}

// Ten letter keys in row 1 of QWERTY — easier to spell out than to
// build at runtime from a string, especially since the symbols rows
// use literal char arrays anyway.
constexpr RowDef kAlphaRows[kRowCount] = {
	{10, 5, 0, 0, {
		KEY_L("q", 'q'), KEY_L("w", 'w'), KEY_L("e", 'e'), KEY_L("r", 'r'), KEY_L("t", 't'),
		KEY_L("y", 'y'), KEY_L("u", 'u'), KEY_L("i", 'i'), KEY_L("o", 'o'), KEY_L("p", 'p'),
	}},
	{9, 5, 18, 18, {
		KEY_L("a", 'a'), KEY_L("s", 's'), KEY_L("d", 'd'), KEY_L("f", 'f'), KEY_L("g", 'g'),
		KEY_L("h", 'h'), KEY_L("j", 'j'), KEY_L("k", 'k'), KEY_L("l", 'l'), {nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
	{9, 5, 0, 0, {
		KEY_U_SMALL("shift", kCodeShift),
		KEY_L("z", 'z'), KEY_L("x", 'x'), KEY_L("c", 'c'), KEY_L("v", 'v'),
		KEY_L("b", 'b'), KEY_L("n", 'n'), KEY_L("m", 'm'),
		KEY_U_SMALL("delete", kCodeBackspace),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
	{3, 6, 30, 30, {
		KEY_U_SMALL("123", kCodeModeSymbols),
		KEY_L_WIDE("space", kCodeSpace),
		KEY_P_SMALL("return", kCodeReturn),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
};

constexpr RowDef kSymbolsRows[kRowCount] = {
	{10, 5, 0, 0, {
		KEY_L("1", '1'), KEY_L("2", '2'), KEY_L("3", '3'), KEY_L("4", '4'), KEY_L("5", '5'),
		KEY_L("6", '6'), KEY_L("7", '7'), KEY_L("8", '8'), KEY_L("9", '9'), KEY_L("0", '0'),
	}},
	{10, 5, 0, 0, {
		KEY_L("-", '-'), KEY_L("/", '/'), KEY_L(":", ':'), KEY_L(";", ';'), KEY_L("(", '('),
		KEY_L(")", ')'), KEY_L("$", '$'), KEY_L("&", '&'), KEY_L("@", '@'), KEY_L("\"", '"'),
	}},
	{7, 5, 0, 0, {
		KEY_U_SMALL("#+=", kCodeModeMoreSymbols),
		KEY_L(".", '.'), KEY_L(",", ','), KEY_L("?", '?'), KEY_L("!", '!'), KEY_L("'", '\''),
		KEY_U_SMALL("delete", kCodeBackspace),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
	{3, 6, 30, 30, {
		KEY_U_SMALL("ABC", kCodeModeAlpha),
		KEY_L_WIDE("space", kCodeSpace),
		KEY_P_SMALL("return", kCodeReturn),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
};

constexpr RowDef kMoreSymbolsRows[kRowCount] = {
	{10, 5, 0, 0, {
		KEY_L("[", '['), KEY_L("]", ']'), KEY_L("{", '{'), KEY_L("}", '}'), KEY_L("#", '#'),
		KEY_L("%", '%'), KEY_L("^", '^'), KEY_L("*", '*'), KEY_L("+", '+'), KEY_L("=", '='),
	}},
	{7, 5, 0, 0, {
		KEY_L("_", '_'), KEY_L("\\", '\\'), KEY_L("|", '|'), KEY_L("~", '~'),
		KEY_L("<", '<'), KEY_L(">", '>'), KEY_L("`", '`'),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
	{7, 5, 0, 0, {
		KEY_U_SMALL("123", kCodeModeSymbols),
		KEY_L(".", '.'), KEY_L(",", ','), KEY_L("?", '?'), KEY_L("!", '!'), KEY_L("'", '\''),
		KEY_U_SMALL("delete", kCodeBackspace),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
	{3, 6, 30, 30, {
		KEY_U_SMALL("ABC", kCodeModeAlpha),
		KEY_L_WIDE("space", kCodeSpace),
		KEY_P_SMALL("return", kCodeReturn),
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
		{nullptr,0,KeyKind::Light,LabelKind::Normal,0,0},
	}},
};

#undef KEY_L
#undef KEY_U_SMALL
#undef KEY_P_SMALL
#undef KEY_L_WIDE

// Wide layout: LVGL 8.3's lv_keyboard, key for key. Landscape panels get
// it -- the CrowPanel 7.0 runs LVGL's widgets demo, whose keyboard this
// is. Flex factors are lv_btnmatrix's width units; LVGL's "checked"
// (grey) keys are the Utility kind.
#define W_L(LABEL, CODE, UNITS) {LABEL, CODE, KeyKind::Light, LabelKind::Normal, 0, UNITS, Icon::None}
#define W_U(LABEL, CODE, UNITS) {LABEL, CODE, KeyKind::Utility, LabelKind::Normal, 0, UNITS, Icon::None}
#define W_I(ICON, LABEL, CODE, UNITS) {LABEL, CODE, KeyKind::Utility, LabelKind::Normal, 0, UNITS, Icon::ICON}
#define W_BOTTOM_ROW {5, 11, 0, 0, { \
		W_I(Keyboard, "kb", kCodeHide, 2), W_I(Left, "<", kCodeLeft, 2), W_L(" ", kCodeSpace, 6), \
		W_I(Right, ">", kCodeRight, 2), W_I(Ok, "ok", kCodeOk, 2), \
	}}

constexpr RowDef kWideLowerRows[kRowCount] = {
	{12, 11, 0, 0, {
		W_U("1#", kCodeModeSymbols, 5),
		W_L("q", 'q', 4), W_L("w", 'w', 4), W_L("e", 'e', 4), W_L("r", 'r', 4), W_L("t", 't', 4),
		W_L("y", 'y', 4), W_L("u", 'u', 4), W_L("i", 'i', 4), W_L("o", 'o', 4), W_L("p", 'p', 4),
		W_I(Backspace, "del", kCodeBackspace, 7),
	}},
	{11, 11, 0, 0, {
		W_U("ABC", kCodeModeUpper, 6),
		W_L("a", 'a', 3), W_L("s", 's', 3), W_L("d", 'd', 3), W_L("f", 'f', 3), W_L("g", 'g', 3),
		W_L("h", 'h', 3), W_L("j", 'j', 3), W_L("k", 'k', 3), W_L("l", 'l', 3),
		W_I(NewLine, "ret", kCodeReturn, 7),
	}},
	{12, 11, 0, 0, {
		W_U("_", '_', 1), W_U("-", '-', 1),
		W_L("z", 'z', 1), W_L("x", 'x', 1), W_L("c", 'c', 1), W_L("v", 'v', 1),
		W_L("b", 'b', 1), W_L("n", 'n', 1), W_L("m", 'm', 1),
		W_U(".", '.', 1), W_U(",", ',', 1), W_U(":", ':', 1),
	}},
	W_BOTTOM_ROW,
};

constexpr RowDef kWideUpperRows[kRowCount] = {
	{12, 11, 0, 0, {
		W_U("1#", kCodeModeSymbols, 5),
		W_L("Q", 'Q', 4), W_L("W", 'W', 4), W_L("E", 'E', 4), W_L("R", 'R', 4), W_L("T", 'T', 4),
		W_L("Y", 'Y', 4), W_L("U", 'U', 4), W_L("I", 'I', 4), W_L("O", 'O', 4), W_L("P", 'P', 4),
		W_I(Backspace, "del", kCodeBackspace, 7),
	}},
	{11, 11, 0, 0, {
		W_U("abc", kCodeModeAlpha, 6),
		W_L("A", 'A', 3), W_L("S", 'S', 3), W_L("D", 'D', 3), W_L("F", 'F', 3), W_L("G", 'G', 3),
		W_L("H", 'H', 3), W_L("J", 'J', 3), W_L("K", 'K', 3), W_L("L", 'L', 3),
		W_I(NewLine, "ret", kCodeReturn, 7),
	}},
	{12, 11, 0, 0, {
		W_U("_", '_', 1), W_U("-", '-', 1),
		W_L("Z", 'Z', 1), W_L("X", 'X', 1), W_L("C", 'C', 1), W_L("V", 'V', 1),
		W_L("B", 'B', 1), W_L("N", 'N', 1), W_L("M", 'M', 1),
		W_U(".", '.', 1), W_U(",", ',', 1), W_U(":", ':', 1),
	}},
	W_BOTTOM_ROW,
};

constexpr RowDef kWideSpecialRows[kRowCount] = {
	{11, 11, 0, 0, {
		W_L("1", '1', 1), W_L("2", '2', 1), W_L("3", '3', 1), W_L("4", '4', 1), W_L("5", '5', 1),
		W_L("6", '6', 1), W_L("7", '7', 1), W_L("8", '8', 1), W_L("9", '9', 1), W_L("0", '0', 1),
		W_I(Backspace, "del", kCodeBackspace, 2),
	}},
	{13, 11, 0, 0, {
		W_U("abc", kCodeModeAlpha, 2),
		W_L("+", '+', 1), W_L("&", '&', 1), W_L("/", '/', 1), W_L("*", '*', 1), W_L("=", '=', 1),
		W_L("%", '%', 1), W_L("!", '!', 1), W_L("?", '?', 1), W_L("#", '#', 1), W_L("<", '<', 1),
		W_L(">", '>', 1),
	}},
	{12, 11, 0, 0, {
		W_L("\\", '\\', 1), W_L("@", '@', 1), W_L("$", '$', 1), W_L("(", '(', 1), W_L(")", ')', 1),
		W_L("{", '{', 1), W_L("}", '}', 1), W_L("[", '[', 1), W_L("]", ']', 1), W_L(";", ';', 1),
		W_L("\"", '"', 1), W_L("'", '\'', 1),
	}},
	W_BOTTOM_ROW,
};

#undef W_L
#undef W_U
#undef W_I
#undef W_BOTTOM_ROW

enum class Mode { Alpha, Upper, Symbols, MoreSymbols };

struct KeyboardState {
	int rootNode = -1;
	int rowNodes[kRowCount] = {-1, -1, -1, -1};
	int keyNodes[kSlotCount];
	int labelNodes[kSlotCount];
	int iconNodes[kSlotCount];
	int keyCodes[kSlotCount];
	// Decoded symbol images of the wide layout (ImageStore ids), -1 until
	// the keyboard first mounts wide.
	int iconImages[kIconCount] = {-1, -1, -1, -1, -1, -1};
	// The layout chosen at mount: LVGL's for landscape panels.
	bool wide = false;
	bool visible = false;
	bool shiftHeld = false;
	Mode mode = Mode::Alpha;
	int boundDocumentRoot = -1;
	bool listenerInstalled = false;
	// When the keyboard shows we shrink the app's content node so it
	// can scroll behind the keys rather than getting overlapped. These
	// fields remember what we changed so we can restore on hide.
	//
	// Each touched property needs BOTH halves remembered, because setStyle
	// writes two places: the computed `style.*` field, and the node's
	// inline-override record. `Tree::resetStyleForClassRecompute` rebuilds
	// computed style out of class rules plus that record, so a property the
	// keyboard recorded and never un-recorded comes back at the next class
	// recompute -- long after the keyboard is gone. Hide therefore restores
	// the computed value AND puts the record back as it was: re-recording
	// the app's own inline value, or removing the entry entirely when the
	// app never authored one.
	struct SavedStyle {
		Property property = Property::Height;
		int computed = 0;
		int inlineValue = 0;
		bool hadInline = false;
	};
	static constexpr int kMaxSavedStyles = 6;
	int resizedNode = -1;
	SavedStyle savedStyles[kMaxSavedStyles];
	int savedStyleCount = 0;
	bool resizeApplied = false;

	KeyboardState()
	{
		for (int i = 0; i < kSlotCount; i++) {
			keyNodes[i] = -1;
			labelNodes[i] = -1;
			iconNodes[i] = -1;
			keyCodes[i] = 0;
		}
	}
};

KeyboardState gState;

const RowDef *rowsForMode(Mode mode)
{
	if (gState.wide) {
		switch (mode) {
		case Mode::Upper: return kWideUpperRows;
		case Mode::Symbols:
		case Mode::MoreSymbols: return kWideSpecialRows;
		case Mode::Alpha: default: return kWideLowerRows;
		}
	}
	switch (mode) {
	case Mode::Symbols: return kSymbolsRows;
	case Mode::MoreSymbols: return kMoreSymbolsRows;
	case Mode::Alpha: default: return kAlphaRows;
	}
}

// Physical-pixel zone reserved at the bottom of the panel for a
// platform overlay (geaos home button). 0 on targets without one.
int bottomInset() { return gea::embedded::ui::safeAreaInsetBottom(); }

// Tall portrait panels (height >= 1.5x width — phones like the geaos
// Redmi 6A at 720x1440) have room to spare, so the keyboard switches to
// a "full size" layout with 1.5x taller keys and proportionally larger
// labels. Squarer panels (ESP32 ~410x502) keep the compact default
// untouched.
bool fullSizeKeyboard()
{
	Tree &tree = Tree::instance();
	const int vw = tree.mountedWidth() > 0 ? tree.mountedWidth() : 410;
	const int vh = tree.mountedHeight() > 0 ? tree.mountedHeight() : 502;
	return vh * 2 >= vw * 3;  // vh / vw >= 1.5
}

// Landscape panels (the CrowPanel 7.0 at 800x480) get LVGL's keyboard:
// the bottom half of the screen, 13-unit-wide rows with symbol keys.
bool wideKeyboard()
{
	Tree &tree = Tree::instance();
	const int vw = tree.mountedWidth() > 0 ? tree.mountedWidth() : 410;
	const int vh = tree.mountedHeight() > 0 ? tree.mountedHeight() : 502;
	return vw > vh;
}

int colCount() { return gState.wide ? kColCount : kCompactColCount; }

struct KbMetrics {
	int keyHeight;
	int rootGap;
	int padTop;
	int padBottom;
	int padLeft;
	int padRight;
	int fontNormal;
	int fontSmall;
	int fontWide;
	int borderRadius;
	int height;  // total keyboard root height
	int lastRowExtra;  // pixels the bottom row is taller than keyHeight
};

KbMetrics kbMetrics()
{
	if (gState.wide) {
		// LVGL 8.3's default theme at DISP_LARGE and 130 DPI: pad_small and
		// row/column gaps of 11, key radius 10, Montserrat 16, and the
		// keyboard half the screen tall (the widgets demo's size). LVGL
		// hands the leftover pixel of the four rows to the bottom one.
		Tree &tree = Tree::instance();
		const int vh = tree.mountedHeight() > 0 ? tree.mountedHeight() : 480;
		const int height = vh / 2;
		const int pad = 11;
		const int rows = height - 2 * pad - (kRowCount - 1) * pad;
		const int keyH = rows / kRowCount;
		return {keyH, pad, pad, pad, pad, pad, 16, 16, 16, 10, height, rows - keyH * kRowCount};
	}
	if (!fullSizeKeyboard()) {
		// Unchanged compact default — keeps the ESP32 keyboard identical.
		return {38, 5, 8, 8, 12, 12, 19, 15, 17, 9, kKeyboardHeight, 0};
	}
	// 1.5x taller keys. Height is computed from the parts so the
	// background exactly wraps the four rows + gaps + padding.
	const int keyH = 57;       // 38 * 1.5
	const int rootGap = 8;     // 5 * 1.5
	const int padTop = 12;
	const int padBottom = 12;
	const int height = padTop + kRowCount * keyH + (kRowCount - 1) * rootGap + padBottom;
	return {keyH, rootGap, padTop, padBottom, 16, 16, 28, 22, 25, 13, height, 0};
}

// Total height the keyboard occupies plus the reserved bottom overlay
// zone — what both the mount position and the app-resize math key off.
int keyboardOccupiedHeight() { return kbMetrics().height + bottomInset(); }

// `setStyle(Property::BackgroundColor, X)` expects X to be a RAW RGB565
// value — the framework's renderer byte-swaps once on the way to the
// display. Using `pixel::fromRgb888` here would double-swap and land
// the bytes in the wrong order on the wire (the earlier "purple
// keyboard on purple background" symptom). Use `rgb565FromRgb888`
// which returns the plain RGB565 value the framework expects.
// The wide layout wears LVGL's default light theme: a #f5f5f5 screen
// background, white keys, #e0e0e0 "checked" keys, #212121 text, and a
// pressed state darkened by LV_OPA_35 of black.
std::uint16_t rgb(int r, int g, int b) { return gea::framework::graphics::pixel::rgb565FromRgb888(r, g, b); }
std::uint16_t kbBg()              { return gState.wide ? rgb(0xf5, 0xf5, 0xf5) : rgb(0xd1, 0xd3, 0xda); }
std::uint16_t keyLightBg()        { return gState.wide ? rgb(0xff, 0xff, 0xff) : rgb(0xf2, 0xf2, 0xf7); }
std::uint16_t keyUtilityBg()      { return gState.wide ? rgb(0xe0, 0xe0, 0xe0) : rgb(0x6f, 0x73, 0x7d); }
std::uint16_t keyPrimaryBg()      { return gea::framework::graphics::pixel::rgb565FromRgb888(0x0a, 0x84, 0xff); }
std::uint16_t keyShiftActiveBg()  { return gea::framework::graphics::pixel::rgb565FromRgb888(0xff, 0xff, 0xff); }
std::uint16_t labelDark()         { return gState.wide ? rgb(0x21, 0x21, 0x21) : rgb(0x11, 0x11, 0x16); }
std::uint16_t labelLight()        { return gea::framework::graphics::pixel::rgb565FromRgb888(0xff, 0xff, 0xff); }

void applyKeyStyle(Tree &tree, int node, KeyKind kind, bool shiftActiveKey, const KbMetrics &m)
{
	tree.setStyle(node, Property::Height, m.keyHeight);
	tree.setStyle(node, Property::AlignItems, 1);
	tree.setStyle(node, Property::JustifyContent, 1);
	tree.setStyle(node, Property::HasBackground, 1);
	tree.setStyle(node, Property::BorderRadiusTopLeft, m.borderRadius);
	tree.setStyle(node, Property::BorderRadiusTopRight, m.borderRadius);
	tree.setStyle(node, Property::BorderRadiusBottomRight, m.borderRadius);
	tree.setStyle(node, Property::BorderRadiusBottomLeft, m.borderRadius);
	std::uint16_t bg;
	std::uint16_t activeBg;
	switch (kind) {
	case KeyKind::Primary:
		bg = keyPrimaryBg();
		// Slightly darker iOS blue for the press feedback. The framework's
		// InputController::pointerDown lightens bg-having nodes by default
		// (lightenRgb565), which would turn the already-bright blue near
		// white — explicitly setting active_bg makes the press obviously
		// "darker, not lighter".
		activeBg = gea::framework::graphics::pixel::rgb565FromRgb888(0x06, 0x6a, 0xd6);
		break;
	case KeyKind::Utility:
		bg = shiftActiveKey ? keyShiftActiveBg() : keyUtilityBg();
		activeBg = gState.wide ? rgb(0xc1, 0xc1, 0xc1) : rgb(0x55, 0x59, 0x63);
		break;
	case KeyKind::Light:
	default:
		bg = keyLightBg();
		if (gState.wide) {
			activeBg = rgb(0xdc, 0xdc, 0xdc);
			break;
		}
		// Light keys go slightly darker to a visible iOS-press grey rather
		// than the framework's default "lighten" which would push #f2f2f7
		// past pure white and look like nothing happened.
		activeBg = gea::framework::graphics::pixel::rgb565FromRgb888(0xc7, 0xc7, 0xcc);
		break;
	}
	tree.setStyle(node, Property::BackgroundColor, static_cast<int>(bg));
	tree.setStyle(node, Property::ActiveBackgroundColor, static_cast<int>(activeBg));
	// HasActiveBackground = 1 tells the framework's findActiveTouchNodeId
	// to surface THIS node (not an ancestor) as the press-feedback target.
	// Without it, pointerDown's auto-lighten doesn't trigger and the
	// keys look static during the tap.
	tree.setStyle(node, Property::HasActiveBackground, 1);
}

// The font the keys are labelled in: the focused input's, else the first
// font found under the mounted root, else -1 (the bitmap fallback).
int keyboardFontId()
{
	Tree &tree = Tree::instance();
	const int active = tree.activeInputId();
	if (active >= 0 && tree.node(active).computedStyle().font_id >= 0) return tree.node(active).computedStyle().font_id;
	for (int id = 0; id < tree.nodeCount(); id++) {
		if (tree.node(id).computedStyle().font_id >= 0) return tree.node(id).computedStyle().font_id;
	}
	return -1;
}

void applyLabelStyle(Tree &tree, int node, LabelKind kind, KeyKind keyKind, bool shiftActiveKey, const KbMetrics &m)
{
	int fontSize = m.fontNormal;
	if (kind == LabelKind::Small) fontSize = m.fontSmall;
	else if (kind == LabelKind::Wide) fontSize = m.fontWide;
	tree.setStyle(node, Property::FontSize, fontSize);
	// Runtime-created text has no font from the stylesheet; take the
	// focused input's, so the keys read in the app's own face instead of
	// the 8x16 bitmap fallback.
	tree.setStyle(node, Property::FontId, keyboardFontId());
	std::uint16_t color;
	if (gState.wide || keyKind == KeyKind::Light || (keyKind == KeyKind::Utility && shiftActiveKey)) color = labelDark();
	else color = labelLight();
	tree.setStyle(node, Property::Color, static_cast<int>(color));
}

// Forward decl for the press listener so applyMode can wire it.
void onKeyboardPress(gea::framework::events::PointerEvent &event);

void mountIfNeeded()
{
	Tree &tree = Tree::instance();
	if (gState.rootNode >= 0) return;
	if (tree.mountedRoot() < 0) return;

	// Canvas-rooted apps (full-screen map/game surfaces): a canvas is a
	// layout LEAF, so the keyboard can't lay out as its child. Wrap the
	// canvas in a plain view (which doesn't disturb the display-backed
	// fast path while the keyboard is hidden) and remount.
	if (tree.node(tree.mountedRoot()).type == NodeType::Canvas) {
		const int wrapper = tree.createView();
		if (wrapper < 0) return;
		const int canvasId = tree.mountedRoot();
		const int w = tree.mountedWidth();
		const int h = tree.mountedHeight();
		tree.setStyle(wrapper, Property::Width, w);
		tree.setStyle(wrapper, Property::Height, h);
		tree.setParent(canvasId, wrapper);
		tree.mount(wrapper, w, h);
	}

	gState.wide = wideKeyboard();
#if GEA_UI_IMAGE_NODES
	if (gState.wide) {
		auto &images = gea::framework::graphics::ImageStore::instance();
		for (int i = 0; i < kIconCount; i++) {
			if (gState.iconImages[i] >= 0) continue;
			const auto &icon = keyboard_icons::kIcons[i];
			gState.iconImages[i] = images.decode(icon.png, icon.length);
		}
	}
#endif
	const int kbRoot = tree.createView();
	if (kbRoot < 0) return;
	gState.rootNode = kbRoot;
	tree.setParent(kbRoot, tree.mountedRoot());

	const int viewportW = tree.mountedWidth() > 0 ? tree.mountedWidth() : 410;
	const int viewportH = tree.mountedHeight() > 0 ? tree.mountedHeight() : 502;
	// Absolute pos_offsets are measured FROM the parent's padding box
	// (positionAbsoluteChild adds parent.padding[side] before the
	// offset). To land the keyboard at exactly (0, viewportH - kbH) in
	// display coords regardless of mountedRoot's CSS padding, subtract
	// the parent's padding from our offsets — negative Left/Top
	// cancels the inset.
	const Node &parent = tree.node(tree.mountedRoot());
	const int padTop = parent.computedStyle().padding[0];
	const int padLeft = parent.computedStyle().padding[3];

	const KbMetrics m = kbMetrics();
	// Float the keyboard above any reserved bottom overlay (home button)
	// instead of letting its lower rows hide behind it.
	tree.setStyle(kbRoot, Property::Position, 1);
	tree.setStyle(kbRoot, Property::Left, -padLeft);
	tree.setStyle(kbRoot, Property::Top, viewportH - m.height - bottomInset() - padTop);
	tree.setStyle(kbRoot, Property::Width, viewportW);
	tree.setStyle(kbRoot, Property::Height, m.height);
	tree.setStyle(kbRoot, Property::ZIndex, 32000);
	tree.setStyle(kbRoot, Property::HasBackground, 1);
	tree.setStyle(kbRoot, Property::BackgroundColor, static_cast<int>(kbBg()));
	tree.setStyle(kbRoot, Property::PaddingTop, m.padTop);
	tree.setStyle(kbRoot, Property::PaddingBottom, m.padBottom);
	tree.setStyle(kbRoot, Property::PaddingLeft, m.padLeft);
	tree.setStyle(kbRoot, Property::PaddingRight, m.padRight);
	tree.setStyle(kbRoot, Property::Gap, m.rootGap);
	tree.setStyle(kbRoot, Property::FlexDirection, 0);  // column
	tree.setStyle(kbRoot, Property::Display, kDisplayNone);  // hidden until shown

	for (int row = 0; row < kRowCount; row++) {
		const int rowNode = tree.createView();
		if (rowNode < 0) return;
		gState.rowNodes[row] = rowNode;
		tree.setParent(rowNode, kbRoot);
		tree.setStyle(rowNode, Property::Display, kDisplayFlex);
		tree.setStyle(rowNode, Property::FlexDirection, 1);  // row
		tree.setStyle(rowNode, Property::AlignItems, 1);     // center (cross-axis)
		tree.setStyle(rowNode, Property::JustifyContent, 1); // center (main-axis)
		tree.setStyle(rowNode, Property::Height, m.keyHeight + (row == kRowCount - 1 ? m.lastRowExtra : 0));
		// Rows stretch the full keyboard width so they share the leftover
		// horizontal space between fixed-width keys (Shift, delete) and
		// flex:1 letters. Without this they shrink to content width and
		// the row sits left-justified.
		tree.setStyle(rowNode, Property::Width, viewportW - m.padLeft - m.padRight);
		for (int col = 0; col < colCount(); col++) {
			const int slot = row * kColCount + col;
			const int keyNode = tree.createView();
			if (keyNode < 0) return;
			gState.keyNodes[slot] = keyNode;
			tree.setParent(keyNode, rowNode);
			tree.setPressId(keyNode, kPressIdBase + slot);

			const int labelNode = tree.createText();
			if (labelNode < 0) return;
			gState.labelNodes[slot] = labelNode;
			tree.setParent(labelNode, keyNode);
#if GEA_UI_IMAGE_NODES
			if (gState.wide) {
				const int iconNode = tree.createImage();
				if (iconNode < 0) return;
				gState.iconNodes[slot] = iconNode;
				tree.setParent(iconNode, keyNode);
				tree.setStyle(iconNode, Property::Display, kDisplayNone);
			}
#endif
		}
	}

	// Install the tap listener on body once, the first time the
	// keyboard mounts. Subsequent app switches (Tree::clear) will reset
	// listenerInstalled and re-install via the same path.
	if (!gState.listenerInstalled) {
		tree.setEventListener(tree.mountedRoot(), "click", onKeyboardPress);
		gState.listenerInstalled = true;
		gState.boundDocumentRoot = tree.mountedRoot();
	}
}

// Shows the key's symbol image (wide layout) and hides it on every other
// key. True when the key shows a symbol instead of its text label; false
// keeps the label -- also where the build has no image nodes, so the
// symbol keys fall back to their text.
bool applyIcon(Tree &tree, int slot, Icon icon)
{
#if GEA_UI_IMAGE_NODES
	const int iconNode = gState.iconNodes[slot];
	if (iconNode < 0) return false;
	const int image = icon == Icon::None ? -1 : gState.iconImages[static_cast<int>(icon) - 1];
	if (image < 0) {
		tree.setStyle(iconNode, Property::Display, kDisplayNone);
		return false;
	}
	auto &images = gea::framework::graphics::ImageStore::instance();
	tree.setStyle(iconNode, Property::ImageId, image);
	tree.setStyle(iconNode, Property::Width, images.width(image));
	tree.setStyle(iconNode, Property::Height, images.height(image));
	tree.setStyle(iconNode, Property::Display, kDisplayBlock);
	return true;
#else
	(void)tree;
	(void)slot;
	(void)icon;
	return false;
#endif
}

void applyMode()
{
	Tree &tree = Tree::instance();
	if (gState.rootNode < 0) return;
	const KbMetrics m = kbMetrics();
	const RowDef *rows = rowsForMode(gState.mode);
	for (int row = 0; row < kRowCount; row++) {
		const RowDef &rdef = rows[row];
		const int rowNode = gState.rowNodes[row];
		if (rowNode < 0) continue;
		tree.setStyle(rowNode, Property::Gap, rdef.gap);
		tree.setStyle(rowNode, Property::PaddingLeft, rdef.paddingLeft);
		tree.setStyle(rowNode, Property::PaddingRight, rdef.paddingRight);

		for (int col = 0; col < colCount(); col++) {
			const int slot = row * kColCount + col;
			const int keyNode = gState.keyNodes[slot];
			const int labelNode = gState.labelNodes[slot];
			if (keyNode < 0 || labelNode < 0) continue;

			if (col >= rdef.count || rdef.keys[col].label == nullptr) {
				// Hide unused slot.
				gState.keyCodes[slot] = 0;
				tree.setStyle(keyNode, Property::Display, kDisplayNone);
				continue;
			}

			const KeyDef &kdef = rdef.keys[col];
			gState.keyCodes[slot] = kdef.code;

			tree.setStyle(keyNode, Property::Display, kDisplayFlex);
			tree.setStyle(keyNode, Property::Width, kdef.width);
			tree.setStyle(keyNode, Property::Flex, kdef.flex);
			const bool shiftActive = kdef.code == kCodeShift && gState.shiftHeld;
			applyKeyStyle(tree, keyNode, kdef.kind, shiftActive, m);
			if (row == kRowCount - 1 && m.lastRowExtra) tree.setStyle(keyNode, Property::Height, m.keyHeight + m.lastRowExtra);
			applyLabelStyle(tree, labelNode, kdef.labelKind, kdef.kind, shiftActive, m);
			if (applyIcon(tree, slot, kdef.icon)) {
				tree.setText(labelNode, "");
				continue;
			}

			// Capitalize alpha keys when shift is held — matches what
			// the user expects to type and what the press handler
			// will actually emit.
			if (gState.mode == Mode::Alpha && gState.shiftHeld && kdef.code >= 'a' && kdef.code <= 'z') {
				char upper[2] = {static_cast<char>(kdef.code - 'a' + 'A'), '\0'};
				tree.setText(labelNode, upper);
			} else {
				tree.setText(labelNode, kdef.label);
			}
		}
	}
}

void applyKeyToActiveInput(int code)
{
	Tree &tree = Tree::instance();
	const int activeId = tree.activeInputId();
	if (activeId < 0 || !tree.activeInputAdmitted()) return;

	if (code == kCodeShift) {
		gState.shiftHeld = !gState.shiftHeld;
		applyMode();
		return;
	}
	if (code == kCodeModeAlpha) {
		gState.mode = Mode::Alpha;
		gState.shiftHeld = false;
		applyMode();
		return;
	}
	if (code == kCodeModeSymbols) {
		gState.mode = Mode::Symbols;
		applyMode();
		return;
	}
	if (code == kCodeModeMoreSymbols) {
		gState.mode = Mode::MoreSymbols;
		applyMode();
		return;
	}
	if (code == kCodeModeUpper) {
		gState.mode = Mode::Upper;
		applyMode();
		return;
	}
	// LVGL's keyboard / OK keys: CANCEL and READY. The widgets demo answers
	// both by closing the keyboard and dropping focus (ta_event_cb); the text
	// typed stays either way -- a text area has no undo.
	if (code == kCodeHide || code == kCodeOk) {
		tree.commitActiveInput();
		tree.setActiveInput(-1);
		return;
	}

	const char *currentValue = tree.getAttribute(activeId, "value");
	std::string next = currentValue ? std::string(currentValue) : std::string();
	const int caret = tree.activeInputCaret();

	// lv_textarea_cursor_left / _right: the caret moves one character; the
	// app also sees the keydown (ArrowLeft / ArrowRight).
	if (code == kCodeLeft || code == kCodeRight) {
		const int moved = code == kCodeLeft ? caret - 1 : caret + 1;
		if (moved >= 0 && moved <= static_cast<int>(next.size())) tree.setActiveInputCaret(moved);
		gea::framework::events::PointerEvent kd{};
		kd.type = gea::framework::events::PointerEventType::KeyDown;
		kd.targetId = activeId;
		kd.keyCode = code == kCodeLeft ? 37 : 39;
		tree.dispatchEvent(kd);
		return;
	}

	// Helper: marking commands dirty + setting displayListDirty isn't
	// enough — the framework's DirtyRegions tracker computes the per-
	// frame flush rectangles from each node's render.dirty bits, and
	// without them the new DrawText command is recorded but never sent
	// to the panel. Setting both render.dirty AND non_scroll_dirty
	// matches what Tree::setText does for plain Text nodes.
	auto markInputDirty = [&](int nodeId) {
		if (nodeId < 0 || nodeId >= tree.nodeCount()) return;
		tree.markNodeDisplayCommandsDirty(nodeId);
		tree.setDisplayListRebuildRequired(true);
		Node &nn = tree.node(nodeId);
		nn.render.dirty = 1;
		nn.render.layout_dirty = 1;
#if GEA_CSS_SCROLLING
		nn.render.non_scroll_dirty = 1;
#endif
	};

	// lv_textarea_del_char: the character before the caret.
	if (code == kCodeBackspace) {
		if (caret > 0) {
			next.erase(static_cast<std::size_t>(caret - 1), 1);
			tree.setAttribute(activeId, "value", next.c_str());
			tree.setActiveInputCaret(caret - 1);
		tree.markActiveInputEdited();
			markInputDirty(activeId);
			gea::framework::events::PointerEvent ev{};
			ev.type = gea::framework::events::PointerEventType::Input;
			ev.targetId = activeId;
			tree.dispatchEvent(ev);
		}
		gea::framework::events::PointerEvent kd{};
		kd.type = gea::framework::events::PointerEventType::KeyDown;
		kd.targetId = activeId;
		kd.keyCode = kCodeBackspace;
		tree.dispatchEvent(kd);
		return;
	}
	if (code == kCodeReturn) {
		const NodeHandle originalInput(activeId);
		const char*tag=tree.tagName(activeId);
		const bool commitsReturn=!tag || std::strcmp(tag,"textarea")!=0;
		gea::framework::events::PointerEvent kd{};
		kd.type = gea::framework::events::PointerEventType::KeyDown;
		kd.targetId = activeId;
		kd.keyCode = kCodeReturn;
		tree.dispatchEvent(kd);
		if(commitsReturn && originalInput.valid() && tree.activeInputId()==activeId){
			tree.commitActiveInput();
			// LVGL's new-line key on a one-line field is READY: the
			// keyboard closes.
			if(gState.wide)tree.setActiveInput(-1);
		}
		return;
	}

	// Printable character (space + ASCII).
	char c = static_cast<char>(code);
	if (gState.mode == Mode::Alpha && gState.shiftHeld && c >= 'a' && c <= 'z') {
		c = static_cast<char>(c - 'a' + 'A');
		gState.shiftHeld = false;  // single-shot shift
		applyMode();
	}
	// lv_textarea_add_char: at the caret, which then moves past it.
	next.insert(static_cast<std::size_t>(caret), 1, c);
	tree.setAttribute(activeId, "value", next.c_str());
	tree.setActiveInputCaret(caret + 1);
	tree.noteActiveInputTyped(caret, caret + 1);
		tree.markActiveInputEdited();
	markInputDirty(activeId);

	gea::framework::events::PointerEvent ev{};
	ev.type = gea::framework::events::PointerEventType::Input;
	ev.targetId = activeId;
	tree.dispatchEvent(ev);
}

// Find the last non-keyboard child of mountedRoot — that's the
// scrollable content area in conventional app layouts (header / status
// / compose row above, the long list below). Resizing this single
// child to fit above the keyboard, with overflow=2 (scroll), lets the
// user scroll within the reduced area while keeping the keyboard
// itself positioned absolutely outside the scroll container's clip.
int findScrollableChild()
{
	Tree &tree = Tree::instance();
	const int root = tree.mountedRoot();
	if (root < 0) return -1;
	int last = -1;
	for (int child = tree.node(root).first_child;
	     child >= 0 && child < tree.nodeCount();
	     child = tree.node(child).next_sibling) {
		if (child != gState.rootNode) last = child;
	}
	return last;
}

// The node's own inline-override entry for `property`, when the app authored
// one. NodeStyleOverrideStore has no keyed lookup; the store holds a handful of
// entries, so a scan is the shipped access shape.
bool inlineStyleEntry(int node, Property property, int &valueOut)
{
	const NodeRareData *rd = rareDataFor(node);
	if (!rd) return false;
	const NodeStyleOverrideStore &store = rd->inlineStyles;
	for (std::size_t i = 0; i < store.size(); i++) {
		const NodeStyleOverride &entry = store.at(i);
		if (entry.property == property) {
			valueOut = entry.value;
			return true;
		}
	}
	return false;
}

// Drop the node's inline entry for `property`, leaving the computed field
// alone -- callers set that first, through setStyle. This is the half a raw
// `style.*` write cannot undo, and the reason a dismissed keyboard used to
// leave the app node pinned `position: absolute` for good: the record still
// said `absolute`, so the next class recompute put it straight back.
void clearInlineStyleEntry(int node, Property property)
{
	if (NodeRareData *rd = rareDataFor(node)) rd->inlineStyles.remove(property);
}

// Set a style through setStyle -- so the computed field, the inline record and
// the dirty flags all move together -- remembering what each half held first
// so restoreAppResize can be an exact inverse. `previousComputed` is read by
// the caller because the computed style has no generic per-property reader.
void pinStyle(Tree &tree, int node, Property property, int previousComputed, int value)
{
	if (gState.savedStyleCount < KeyboardState::kMaxSavedStyles) {
		KeyboardState::SavedStyle &saved = gState.savedStyles[gState.savedStyleCount++];
		saved.property = property;
		saved.computed = previousComputed;
		saved.hadInline = inlineStyleEntry(node, property, saved.inlineValue);
	}
	tree.setStyle(node, property, value);
}

void applyAppResize()
{
	if (gState.resizeApplied) return;
	Tree &tree = Tree::instance();
	const int target = findScrollableChild();
	if (target < 0) return;
	const Node &n = tree.node(target);
	gState.resizedNode = target;
	gState.savedStyleCount = 0;
	// Snapshot every computed value we are about to overwrite before the
	// first setStyle lands, so none of them is read back post-mutation.
	const int prevHeight = n.computedStyle().height;
	const int prevFlex = n.computedStyle().flex;
#if GEA_CSS_OVERFLOW_AXES
	const int prevOverflowY = n.computedStyle().overflow_y;
#else
	const int prevOverflowY = n.computedStyle().overflow;
#endif
	const int prevPosition = n.computedStyle().position;
	const int prevTop = GEA_CSS_POSITION_PX_0(n.computedStyle());
	const int prevLeft = GEA_CSS_POSITION_PX_3(n.computedStyle());
	const int viewportH = tree.mountedHeight();
	// Available vertical space between the top of this node (set by
	// previous siblings' heights during the most recent layout pass)
	// and the keyboard's top edge — which now also clears the reserved
	// bottom overlay zone. Negative falls back to 0 so we don't pass a
	// junk height that explodes layout.
	int newHeight = viewportH - keyboardOccupiedHeight() - n.layout.y;
	if (newHeight < 0) newHeight = 0;
	// Pin the node absolutely at its CURRENT box before shrinking it: the
	// height math above assumes layout.y stays put, but a centering parent
	// (weather: the root vertically centers a fixed-height shell) re-centers
	// the shrunken box — the whole app slid down 72px, stayed overlapped by
	// the keys, and the vacated rows (which nothing paints) kept a stale
	// band of the old screen. Absolute takes it out of the parent's flow so
	// it cannot move; restore puts the original position styles back.
	const Node &parentNode = tree.node(n.parent >= 0 ? n.parent : target);
	const int pinTop = n.layout.y - parentNode.layout.y - parentNode.computedStyle().padding[0];
	const int pinLeft = n.layout.x - parentNode.layout.x - parentNode.computedStyle().padding[3];
	pinStyle(tree, target, Property::Position, prevPosition, 1);
	pinStyle(tree, target, Property::Top, prevTop, pinTop);
	pinStyle(tree, target, Property::Left, prevLeft, pinLeft);
	pinStyle(tree, target, Property::Height, prevHeight, newHeight);
	pinStyle(tree, target, Property::Flex, prevFlex, 0);
	pinStyle(tree, target, Property::OverflowY, prevOverflowY, 2);
	gState.resizeApplied = true;
}

void restoreAppResize()
{
	if (!gState.resizeApplied) return;
	const int target = gState.resizedNode;
	if (target < 0) {
		gState.resizeApplied = false;
		return;
	}
	Tree &tree = Tree::instance();
	if (target < tree.nodeCount()) {
		// Reverse order, so the pin comes off last exactly as it went on first.
		for (int i = gState.savedStyleCount - 1; i >= 0; i--) {
			const KeyboardState::SavedStyle &saved = gState.savedStyles[i];
			// setStyle puts the computed field back and marks the node dirty
			// for this frame; it also re-records the value inline, which is
			// what we want only when the app had authored one there. When it
			// had not, drop the entry -- otherwise the record keeps saying
			// what the keyboard said and a later class recompute restores the
			// keyboard's geometry to a node it no longer covers.
			tree.setStyle(target, saved.property,
			              saved.hadInline ? saved.inlineValue : saved.computed);
			if (!saved.hadInline) clearInlineStyleEntry(target, saved.property);
		}
	}
	gState.resizedNode = -1;
	gState.savedStyleCount = 0;
	gState.resizeApplied = false;
}

void onKeyboardPress(gea::framework::events::PointerEvent &event)
{
	// `event.pressId` is set during dispatch from the FIRST ancestor
	// in the bubble chain that has a non-negative pressId, OR (as
	// fallback) the original target's pressId. Hit-tests on a key
	// often land on the key's text-label child — that label has no
	// pressId, so the fallback misses our reserved range. Walk up
	// from event.targetId ourselves looking for the key node.
	Tree &tree = Tree::instance();
	int keyPressId = -1;
	for (int n = event.targetId;
	     n >= 0 && n < tree.nodeCount();
	     n = tree.node(n).parent) {
		const int pid = tree.pressId(n);
		if (pid >= kPressIdBase && pid < kPressIdBase + kSlotCount) {
			keyPressId = pid;
			break;
		}
	}
	if (keyPressId < 0) return;  // not a keyboard tap

	event.preventDefault();
	event.stopPropagation();
	const int slot = keyPressId - kPressIdBase;
	const int code = gState.keyCodes[slot];
	if (code == 0) return;
	applyKeyToActiveInput(code);
}

}  // namespace

VirtualKeyboard &VirtualKeyboard::instance()
{
	static VirtualKeyboard kb;
	return kb;
}

bool VirtualKeyboard::active() const { return gState.visible; }

bool VirtualKeyboard::containsNode(int nodeId) const
{
	if (gState.rootNode < 0) return false;
	Tree &tree = Tree::instance();
	for (int n = nodeId; n >= 0 && n < tree.nodeCount(); n = tree.node(n).parent) {
		if (n == gState.rootNode) return true;
	}
	return false;
}

void VirtualKeyboard::reset()
{
	// Called from Tree::clear() to drop any stale node ids before the
	// next app's tree gets mounted. The nodes themselves are gone (the
	// whole tree was wiped); we just have to clear our local handles so
	// the next mount creates fresh ones.
	gState.rootNode = -1;
	for (int i = 0; i < kRowCount; i++) gState.rowNodes[i] = -1;
	for (int i = 0; i < kSlotCount; i++) {
		gState.keyNodes[i] = -1;
		gState.labelNodes[i] = -1;
		gState.keyCodes[i] = 0;
	}
	gState.visible = false;
	gState.shiftHeld = false;
	gState.mode = Mode::Alpha;
	gState.listenerInstalled = false;
	gState.boundDocumentRoot = -1;
	gState.resizedNode = -1;
	gState.savedStyleCount = 0;
	gState.resizeApplied = false;
}

int VirtualKeyboard::debugRootNode() const
{
	return gState.rootNode;
}

void VirtualKeyboard::sync()
{
	Tree &tree = Tree::instance();
	const int activeId = tree.activeInputId();
	const bool shouldBeVisible = activeId >= 0;

	if (shouldBeVisible) {
		mountIfNeeded();
		if (gState.rootNode < 0) return;
		if (!gState.visible) {
			gState.visible = true;
			tree.setStyle(gState.rootNode, Property::Display, kDisplayFlex);
			applyMode();
			// Shrink the app's content area so the rows it holds can
			// scroll above the keyboard rather than being permanently
			// hidden behind it. mountIfNeeded() reparents the keyboard
			// to body, so the app content is the OTHER child — found
			// dynamically because we don't want to assume a fixed
			// tree shape across apps.
			applyAppResize();
			// Bring the focused field above the keys, the way LVGL's
			// widgets demo scrolls it into the shrunken tabview.
			tree.scrollIntoViewKeepingPadding(activeId);
			tree.setDisplayListRebuildRequired(true);
			// The resize relayout moves the whole app (a centered fixed-height
			// shell re-centers in the shrunken box); per-node dirty rects have
			// been observed to miss the vacated rows, leaving a stale band of
			// the pre-keyboard screen (weather: duplicated header). Show/hide
			// is a screen-scale change — repaint everything.
			if (auto *canvas = gea::platform::display::Display::canvas())
				canvas->markDirty(0, 0, canvas->width() - 1, canvas->height() - 1);
		}
	} else {
		if (gState.visible && gState.rootNode >= 0) {
			gState.visible = false;
			tree.setStyle(gState.rootNode, Property::Display, kDisplayNone);
			restoreAppResize();
			tree.setDisplayListRebuildRequired(true);
			// The keyboard's rows may extend below everything the app paints
			// (weather: a centered fixed-height shell leaves the bottom rows
			// ownerless), so hiding must clear the framebuffer back to the
			// mount-time base or the keys linger as stale pixels. Then repaint
			// everything.
			gea::platform::display::Display::clearNoFlush();
			if (auto *canvas = gea::platform::display::Display::canvas())
				canvas->markDirty(0, 0, canvas->width() - 1, canvas->height() - 1);
		}
	}
}

}  // namespace gea::embedded::ui

#else

namespace gea::embedded::ui {

VirtualKeyboard &VirtualKeyboard::instance()
{
	static VirtualKeyboard kb;
	return kb;
}

bool VirtualKeyboard::active() const { return false; }
bool VirtualKeyboard::containsNode(int) const { return false; }
int VirtualKeyboard::debugRootNode() const { return -1; }
void VirtualKeyboard::reset() {}
void VirtualKeyboard::sync() {}

}  // namespace gea::embedded::ui

#endif
