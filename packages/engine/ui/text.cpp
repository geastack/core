// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "display.h"
#include "graphics/font.h"
#include "refresh_perf.h"
#include "tree_state.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ESP-IDF system headers may define quad as a compatibility type macro.
#ifdef quad
#undef quad
#endif

#ifndef GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_EMBEDDED_RENDER_HOT_SRAM 0
#endif

#if GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_TEXT_HOT_SRAM __attribute__((noinline, noclone, section(".time_critical.gea_text.record")))
#else
#define GEA_TEXT_HOT_SRAM
#endif

namespace gea::embedded::ui {

namespace {

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

bool hasTransformState(const Node &node)
{
	return hasIndividualLinearTransform(rstyle(node.style)) || hadIndividualLinearTransform(node.render) ||
	       rstyle(node.style).transform_rotate != 0 ||
	       node.render.previous_transform_rotate != 0 ||
	       rstyle(node.style).transform_rotate_x != 0 ||
	       node.render.previous_transform_rotate_x != 0 ||
	       rstyle(node.style).transform_rotate_y != 0 ||
	       node.render.previous_transform_rotate_y != 0 ||
	       composedTranslateX(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_x != 0 ||
	       composedTranslateY(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_y != 0 ||
	       composedTranslateZ(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_z != 0 ||
	       composedTranslateXPercent(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_x_percent != 0 ||
	       composedTranslateYPercent(rstyle(node.style)) != 0 ||
	       node.render.previous_transform_translate_y_percent != 0 ||
	       rstyle(node.style).transform_scale_x != 1000 ||
	       node.render.previous_transform_scale_x != 1000 ||
	       rstyle(node.style).transform_scale_y != 1000 ||
	       rstyle(node.style).transform_scale_z != 1000 ||
	       node.render.previous_transform_scale_y != 1000 ||
	       node.render.previous_transform_scale_z != 1000 ||
	       rstyle(node.style).perspective > 0 ||
	       node.render.previous_perspective > 0;
}

constexpr int kBitmapFontWidth = gea::framework::graphics::BitmapFont8x16::kWidth;
constexpr int kBitmapFontHeight = gea::framework::graphics::BitmapFont8x16::kHeight;
constexpr int kWrappedLineBufferBytes = 512;

// Line-break cache for TextDrawer. drawRasterizedWrapped / drawBitmapWrapped
// previously iterated every line of a text command in their inner per-char
// glyph-advance loop just to find where each line ends — even for lines that
// would be skipped by the Y-clip check. With many text commands re-rendered
// each scroll frame (typography uses ~40 paragraphs of varied font specimens)
// and per-chunk streaming replaying the same command across multiple chunks,
// this per-line layout work piles up.
//
// The cache stores chars-per-line + lineWidth-per-line for the most recently
// laid-out text commands, keyed by (text_ptr, content fingerprint, maxWidth,
// fontId, fontSize, lineHeight, scale_times_256). On hit, the inner glyph-
// advance loop is skipped entirely — the renderer iterates the cached chars[]
// array and only emits draw calls for in-clip lines.
//
// Direct-mapped by a hash of text_ptr; collisions overwrite. Capped at 64
// lines per entry — text commands with more lines fall back to the original
// uncached path (a paragraph that long would already be unusual for a
// 502-px-tall display).
#ifndef GEA_EMBEDDED_LINE_BREAK_CACHE_SLOTS
#define GEA_EMBEDDED_LINE_BREAK_CACHE_SLOTS 16
#endif
#ifndef GEA_EMBEDDED_LINE_BREAK_CACHE_LINES
#define GEA_EMBEDDED_LINE_BREAK_CACHE_LINES 64
#endif

constexpr int kLineBreakCacheSlots = GEA_EMBEDDED_LINE_BREAK_CACHE_SLOTS;  // must be power of two
constexpr int kMaxCachedLines = GEA_EMBEDDED_LINE_BREAK_CACHE_LINES;
static_assert(kLineBreakCacheSlots > 0);
static_assert((kLineBreakCacheSlots & (kLineBreakCacheSlots - 1)) == 0);
static_assert(kMaxCachedLines > 0);

struct LineBreakCacheEntry {
	const char *textPtr = nullptr;
	std::uint32_t textHashPrefix = 0;
	int maxWidth = -1;
	// The first line of an inline continuation gets a different budget and origin
	// than the rest, so two runs of the same string at the same maxWidth can break
	// differently — the indent is part of the key.
	int firstLineIndent = 0;
	int fontId = -2;
	int fontSize = -1;
	int lineHeight = 0;
	int scaleQ8 = 0;  // scale * 256, integer key for bitmap-font variant
	int lineCount = 0;
	std::uint16_t renderBytes[kMaxCachedLines];
	std::uint16_t consumedBytes[kMaxCachedLines];
	std::uint16_t lineWidths[kMaxCachedLines];
};

LineBreakCacheEntry gLineBreakCache[kLineBreakCacheSlots];

inline std::uint32_t hashTextPrefix(const char *text)
{
	// 32-bit FNV-1a over first 16 bytes (or up to NUL). Cheap and good
	// enough to disambiguate text-mutations on the same buffer.
	std::uint32_t h = 2166136261u;
	for (int i = 0; i < 16; i++) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (!c) break;
		h ^= c;
		h *= 16777619u;
	}
	return h;
}

inline int lineBreakCacheSlot(const char *text, int maxWidth)
{
	auto ptr = reinterpret_cast<std::uintptr_t>(text);
	std::uint32_t mix = static_cast<std::uint32_t>(ptr) ^ static_cast<std::uint32_t>(ptr >> 16);
	mix ^= static_cast<std::uint32_t>(maxWidth);
	mix *= 0x9e3779b1u;
	return static_cast<int>(mix & (kLineBreakCacheSlots - 1));
}

inline int textDecorationThickness(int fontSize)
{
	if (fontSize <= 0) fontSize = kBitmapFontHeight;
	int thickness = (fontSize + 11) / 12;
	return thickness < 1 ? 1 : thickness;
}

inline int textDecorationY(int textTop, int fontSize, int thickness, int decoration)
{
	if (fontSize <= 0) fontSize = kBitmapFontHeight;
	int center = textTop;
	if (decoration == 2)
		center = textTop + (fontSize * 62 + 50) / 100;
	else if (decoration == 1)
		center = textTop + (fontSize * 88 + 50) / 100;

	int y = center - thickness / 2;
	if (y < textTop) y = textTop;
	const int maxY = textTop + fontSize - thickness;
	if (y > maxY) y = maxY;
	return y;
}

inline int minInt(int a, int b)
{
	return a < b ? a : b;
}

inline int maxInt(int a, int b)
{
	return a > b ? a : b;
}

inline int roundedHalf(int value)
{
	return value >= 0 ? (value + 1) / 2 : (value - 1) / 2;
}

inline float clampFloat(float value, float minValue, float maxValue)
{
	if (value < minValue) return minValue;
	if (value > maxValue) return maxValue;
	return value;
}

bool currentTransformChainIsPureTranslate(const Node &node, float *outX, float *outY)
{
	const Tree &tree = Tree::instance();
	const Node *nodes = tree.nodes();
	const int count = tree.nodeCount();
	int id = -1;
	if (nodes && &node >= nodes && &node < nodes + count)
		id = static_cast<int>(&node - nodes);

	float dx = 0.0f;
	float dy = 0.0f;
	bool anyTranslate = false;
	for (;;) {
		const Node &n = id >= 0 ? tree.node(id) : node;
		if (hasIndividualLinearTransform(rstyle(n.style)) || (rstyle(n.style).transform_rotate % 3600) != 0 ||
		    (rstyle(n.style).transform_rotate_x % 3600) != 0 ||
		    (rstyle(n.style).transform_rotate_y % 3600) != 0 ||
		    composedTranslateZ(rstyle(n.style)) != 0 ||
		    rstyle(n.style).transform_scale_x != 1000 ||
		    rstyle(n.style).transform_scale_y != 1000 ||
		    rstyle(n.style).transform_scale_z != 1000 ||
		    rstyle(n.style).perspective > 0)
			return false;

		const float localDx = static_cast<float>(composedTranslateX(rstyle(n.style))) +
		                      static_cast<float>(n.layout.width) * static_cast<float>(composedTranslateXPercent(rstyle(n.style))) * 0.001f;
		const float localDy = static_cast<float>(composedTranslateY(rstyle(n.style))) +
		                      static_cast<float>(n.layout.height) * static_cast<float>(composedTranslateYPercent(rstyle(n.style))) * 0.001f;
		if (localDx != 0.0f || localDy != 0.0f) {
			anyTranslate = true;
			dx += localDx;
			dy += localDy;
		}

		if (id < 0) break;
		id = n.parent;
		if (id < 0 || id >= count) break;
	}

	if (!anyTranslate) return false;
	if (outX) *outX = dx;
	if (outY) *outY = dy;
	return true;
}

inline int translateCoord(int value, float delta)
{
	return static_cast<int>(std::lroundf(static_cast<float>(value) + delta));
}

uint8_t combineAlpha(uint8_t parentAlpha, uint8_t localAlpha)
{
	return static_cast<uint8_t>((static_cast<int>(parentAlpha) * static_cast<int>(localAlpha) + 127) / 255);
}

void appendAlphaCommand(uint8_t alpha, int bx, int by, int bw, int bh)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::SetAlpha;
	cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
	cmd->alpha.alpha = alpha;
}

void boundsFromCorners(const int16_t *xs, const int16_t *ys, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = *x1 = xs[0];
	*y0 = *y1 = ys[0];
	for (int i = 1; i < 4; i++) {
		if (xs[i] < *x0) *x0 = xs[i];
		if (xs[i] > *x1) *x1 = xs[i];
		if (ys[i] < *y0) *y0 = ys[i];
		if (ys[i] > *y1) *y1 = ys[i];
	}
}

int alignedOffset(int textAlign, int containerWidth, int lineWidth)
{
	int offset = 0;
	if (textAlign == 1) offset = (containerWidth - lineWidth) / 2;
	else if (textAlign == 2) offset = containerWidth - lineWidth;
	return offset < 0 ? 0 : offset;
}

inline bool isAsciiWordChar(unsigned char c)
{
	return std::isalnum(c) != 0;
}

int nextUtf8Codepoint(const char *&p)
{
	const unsigned char c0 = static_cast<unsigned char>(*p++);
	if (c0 < 0x80) return c0;

	const auto isContinuation = [](unsigned char c) {
		return (c & 0xc0) == 0x80;
	};
	if ((c0 & 0xe0) == 0xc0) {
		const unsigned char c1 = static_cast<unsigned char>(*p);
		if (c1 && isContinuation(c1)) {
			++p;
			return ((c0 & 0x1f) << 6) | (c1 & 0x3f);
		}
		return c0;
	}
	if ((c0 & 0xf0) == 0xe0) {
		const unsigned char c1 = static_cast<unsigned char>(p[0]);
		const unsigned char c2 = static_cast<unsigned char>(p[1]);
		if (c1 && c2 && isContinuation(c1) && isContinuation(c2)) {
			p += 2;
			return ((c0 & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (c2 & 0x3f);
		}
		return c0;
	}
	if ((c0 & 0xf8) == 0xf0) {
		const unsigned char c1 = static_cast<unsigned char>(p[0]);
		const unsigned char c2 = static_cast<unsigned char>(p[1]);
		const unsigned char c3 = static_cast<unsigned char>(p[2]);
		if (c1 && c2 && c3 && isContinuation(c1) && isContinuation(c2) && isContinuation(c3)) {
			p += 3;
			return ((c0 & 0x07) << 18) | ((c1 & 0x3f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
		}
	}
	return c0;
}

struct WrappedLine {
	int renderBytes = 0;
	int consumedBytes = 0;
	int width = 0;

};

inline bool isWrappingSpace(int cp)
{
	// NBSP deliberately is not included: it is a word-joining space.
	return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f';
}

inline bool canBreakAfter(int cp)
{
	return cp == '-' || cp == '/' || cp == 0x2010 || cp == 0x2013 || cp == 0x2014;
}

// CSS white-space: normal/pre-line line breaking for the embedded renderer.
// Break at legal word boundaries. An unbreakable word can overflow the line;
// normal wrapping must never manufacture a break inside that word. `consumedBytes` includes discarded wrapping spaces or
// an explicit newline, while `renderBytes` contains only the visible run.
template <typename AdvanceForCodepoint>
WrappedLine nextWrappedLine(const char *text, int maxWidth, AdvanceForCodepoint advanceForCodepoint)
{
	WrappedLine line{};
	if (!text || !*text) return line;
	if (maxWidth <= 0) maxWidth = 32767;

	const char *p = text;
	int width = 0;
	int bytes = 0;
	int breakRenderBytes = -1;
	int breakConsumedBytes = -1;
	int breakWidth = 0;
	bool extendingSpaceBreak = false;

	while (*p) {
		const char *glyphStart = p;
		const int cp = nextUtf8Codepoint(p);
		const int glyphBytes = static_cast<int>(p - glyphStart);
		if (cp == '\n') {
			line.renderBytes = bytes;
			line.consumedBytes = bytes + glyphBytes;
			line.width = width;
			return line;
		}

		const int advance = advanceForCodepoint(cp);
		// A collapsible space at the end of a line HANGS: CSS trims it when the
		// line is positioned, so it never overflows and never forces the break
		// itself. Letting it break the line makes this wrapper non-idempotent —
		// re-wrapping a run at the max line width it just produced pushes the
		// last word off that line (its width is exactly maxWidth, so the space
		// behind it "overflows"). Layout depends on idempotence: TextRenderer::
		// layout stores the measured max line width as layout.width and the
		// renderer re-wraps the run at that width, so a disagreement drew one
		// more line than the box was tall and the run overlapped its next
		// sibling (typography's specimen paragraphs).
		// Leading collapsible spaces cannot be an empty soft-wrapped line.
		if (!isWrappingSpace(cp) && width + advance > maxWidth && bytes > 0 && breakRenderBytes > 0) {
			line.renderBytes = breakRenderBytes;
			line.consumedBytes = breakConsumedBytes;
			line.width = breakWidth;
			return line;
		}

		const int widthBefore = width;
		const int bytesBefore = bytes;
		width += advance;
		bytes += glyphBytes;

		if (isWrappingSpace(cp)) {
			if (!extendingSpaceBreak) {
				breakRenderBytes = bytesBefore;
				breakWidth = widthBefore;
			}
			breakConsumedBytes = bytes;
			extendingSpaceBreak = true;
		} else {
			extendingSpaceBreak = false;
			if (canBreakAfter(cp)) {
				breakRenderBytes = bytes;
				breakConsumedBytes = bytes;
				breakWidth = width;
			}
		}
	}

	line.renderBytes = bytes;
	line.consumedBytes = bytes;
	line.width = width;
	return line;
}

void appendUtf8(std::string &out, int cp)
{
	if (cp < 0) return;
	if (cp <= 0x7f) {
		out.push_back(static_cast<char>(cp));
	} else if (cp <= 0x7ff) {
		out.push_back(static_cast<char>(0xc0 | ((cp >> 6) & 0x1f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	} else if (cp <= 0xffff) {
		out.push_back(static_cast<char>(0xe0 | ((cp >> 12) & 0x0f)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	} else if (cp <= 0x10ffff) {
		out.push_back(static_cast<char>(0xf0 | ((cp >> 18) & 0x07)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
	}
}

int transformedTextCodepoint(int cp, int textTransform, bool &wordStart)
{
	if (cp < 0 || cp > 0x7f) {
		if (textTransform == 3) wordStart = true;
		return cp;
	}
	const unsigned char uc = static_cast<unsigned char>(cp);
	if (textTransform == 1) return static_cast<char>(std::toupper(uc));
	if (textTransform == 2) return static_cast<char>(std::tolower(uc));
	if (textTransform == 3) {
		if (!isAsciiWordChar(uc)) {
			wordStart = true;
			return cp;
		}
		const int out = wordStart
		    ? std::toupper(uc)
		    : std::tolower(uc);
		wordStart = false;
		return out;
	}
	return cp;
}

std::string transformedTextCopy(const char *text, int textTransform)
{
	std::string out;
	if (!text || !text[0] || textTransform == 0) return out;
	out.reserve(std::strlen(text));
	bool wordStart = true;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		appendUtf8(out, transformedTextCodepoint(cp, textTransform, wordStart));
	}
	return out;
}

const char *textWithTransform(const char *text, int textTransform, std::string &storage, int whiteSpace = -1)
{
	// Perform CSS segment-break/space collapsing identically in measurement
	// and painting, while retaining the authored Node::text for later mutation.
	// Edge trimming belongs to line layout, not to individual inline runs.
	if (text && (whiteSpace == 0 || whiteSpace == 1 || whiteSpace == 4) &&
	    (std::strpbrk(text, "\t\r\n\f") || std::strstr(text, "  "))) {
		std::string normalized;
		normalized.reserve(std::strlen(text));
		bool space = false;
		for (const char *p = text; *p; ++p) {
			char c = *p;
			if (c == '\r') { if (p[1] == '\n') ++p; c = '\n'; }
			if (whiteSpace == 4 && (c == '\n' || c == '\f')) {
				if (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
				normalized += '\n';
				space = true;
			} else if (c == ' ' || c == '\t' || c == '\n' || c == '\f') {
				if (!space) normalized += ' ';
				space = true;
			} else {
				normalized += c;
				space = false;
			}
		}
		storage = textTransform ? transformedTextCopy(normalized.c_str(), textTransform) : std::move(normalized);
		return storage.c_str();
	}
	if (!text || textTransform == 0) return text;
	storage = transformedTextCopy(text, textTransform);
	return storage.c_str();
}

struct TextMeasure {
	int width = 0;
	int height = 0;
};

struct TextLayoutMeasure {
	int width = 0;
	int height = 0;
};

#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
bool rasterizedTextInkBounds(const char *text,
                             int maxWidth,
                             int textAlign,
                             int containerWidth,
                             int fontId,
                             int fontSize,
                             int lineHeight,
                             int *outX0,
                             int *outY0,
                             int *outX1,
                             int *outY1)
{
	if (!text || !text[0] || !outX0 || !outY0 || !outX1 || !outY1) return false;
	gea::framework::graphics::RasterizedFont font =
	    gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize);
	if (!font.valid()) return false;
	if (maxWidth <= 0) maxWidth = 32767;

	const int fontLineHeight = font.lineHeight();
	const int lineAdvance = lineHeight > 0 ? lineHeight : fontLineHeight;
	const int lineBoxOffset = lineHeight > 0 ? (lineAdvance - fontLineHeight) / 2 : 0;
	bool any = false;
	int bx0 = 0;
	int by0 = 0;
	int bx1 = -1;
	int by1 = -1;
	const char *lineStart = text;
	int penY = 0;
	while (*lineStart) {
		const WrappedLine line = nextWrappedLine(lineStart, maxWidth, [&](int cp) {
			gea::framework::graphics::Glyph glyph{};
			return font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
		});

		int penX = alignedOffset(textAlign, containerWidth, line.width);
		const int lineY = penY + lineBoxOffset;
		const char *glyphPtr = lineStart;
		int consumed = 0;
		while (consumed < line.renderBytes) {
			const char *glyphStart = glyphPtr;
			const int cp = nextUtf8Codepoint(glyphPtr);
			consumed += static_cast<int>(glyphPtr - glyphStart);
			gea::framework::graphics::Glyph glyph{};
			if (!font.glyph(cp, &glyph)) {
				penX += font.sizePx() / 2;
				continue;
			}
			const int gx0 = penX + glyph.bearingX;
			const int gy0 = lineY + font.ascender() - glyph.bearingY;
			const int gx1 = gx0 + glyph.width - 1;
			const int gy1 = gy0 + glyph.height - 1;
			if (!any) {
				bx0 = gx0;
				by0 = gy0;
				bx1 = gx1;
				by1 = gy1;
				any = true;
			} else {
				if (gx0 < bx0) bx0 = gx0;
				if (gy0 < by0) by0 = gy0;
				if (gx1 > bx1) bx1 = gx1;
				if (gy1 > by1) by1 = gy1;
			}
			penX += glyph.advance;
		}

		lineStart += line.consumedBytes;
		penY += lineAdvance;
	}
	if (!any) return false;
	*outX0 = bx0;
	*outY0 = by0;
	*outX1 = bx1;
	*outY1 = by1;
	return true;
}

// The vertical ink extent of a FONT: how far above the baseline its tallest
// glyph reaches and how far below its deepest one does. Every run drawn in this
// font shares these two numbers, which is the whole point — see the comment on
// rasterizedSingleLineInkCenterOffsetY.
//
// Probed through Font::glyph rather than read out of RasterizedFontData::glyphs
// so a runtime-rasterized font fills its metrics on demand exactly as a paint
// would. ASCII is the alphabet that decides it: a cap and a descender are all
// the extremes are, and a font whose Latin letters fit its own ascender and
// descender is every font we ship.
struct FontInkExtent {
	int above = 0;  // rows of ink above the baseline
	int below = 0;  // rows of ink below it
	bool valid = false;
};

FontInkExtent rasterizedFontInkExtent(const gea::framework::graphics::RasterizedFont &font)
{
	// Paint calls this per text node per frame; the answer is a constant of the
	// font, so a handful of entries keyed on the font data pointer keeps the
	// 95-glyph probe off the frame.
	struct Entry {
		const gea::framework::graphics::RasterizedFontData *key = nullptr;
		FontInkExtent extent{};
	};
	static Entry cache[8];
	static int next = 0;

	const gea::framework::graphics::RasterizedFontData *key = font.data();
	if (!key) return {};
	for (const Entry &entry : cache)
		if (entry.key == key) return entry.extent;

	FontInkExtent extent{};
	for (int cp = 0x21; cp <= 0x7e; ++cp) {
		gea::framework::graphics::Glyph glyph{};
		if (!font.glyph(cp, &glyph) || glyph.width <= 0 || glyph.height <= 0) continue;
		const int above = glyph.bearingY;
		const int below = glyph.height - glyph.bearingY;
		if (!extent.valid || above > extent.above) extent.above = above;
		if (!extent.valid || below > extent.below) extent.below = below;
		extent.valid = true;
	}
	if (!extent.valid) return extent;

	cache[next].key = key;
	cache[next].extent = extent;
	next = (next + 1) % static_cast<int>(sizeof(cache) / sizeof(cache[0]));
	return extent;
}

// How far to move a single-line run so its ink sits centred in the line box.
//
// Centred on the FONT's ink, never on the run's own. Centring each run on the
// glyphs it happens to contain gives every string its own baseline: "Comp" and
// "Delay" carry a descender, so their ink box is taller and its centre sits
// lower relative to the baseline, and centring it lifts them; "Gate", "Drive"
// and "Mod" have none and stay put. Side by side in a row — a pedal chain, a
// tab bar, a table header — that reads as a two-pixel stagger with no cause the
// author can see, because the text is identically styled and only the letters
// differ. Text sits on a baseline; a baseline is a property of the font and the
// line box, not of the word.
//
// What the font-wide box keeps is the reason ink centring exists at all: a
// font's ascender and descender metrics carry padding that its glyphs never
// reach, so centring by metrics leaves the text visibly high in a tight box.
// The tallest ink the font can draw is still ink, so the run is still optically
// centred — it is just centred on the same thing every other run in that font
// is.
//
// The box ends AT the baseline: a descender hangs out of it. Most of what a UI
// writes — "SD card", "Gate", "1 / 3" — never reaches into the descender space,
// so reserving it inside the centred box lifts those lines by half a descender,
// and against a row's own edges that reads as text sitting too high. What the
// eye centres is the block of caps and x-heights standing on the baseline; the
// tail of a "p" is overhang, not mass. Trimming the box at the baseline is what
// a designer means by optically centred, and it costs nothing in consistency —
// the box is still the font's, so every run in it still shares one baseline.
//
// Except where there is no room: in a box no taller than the font's whole ink,
// pushing down by half a descender would only push the descenders out of the
// box and clip them, so the offset is clamped to keep all of the ink inside.
int rasterizedSingleLineInkCenterOffsetY(const char *text,
                                         int /*textAlign*/,
                                         int /*containerWidth*/,
                                         int fontId,
                                         int fontSize,
                                         int lineHeight)
{
	if (!text || !text[0] || std::strchr(text, '\n')) return 0;
	gea::framework::graphics::RasterizedFont font =
	    gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize);
	if (!font.valid()) return 0;

	const FontInkExtent ink = rasterizedFontInkExtent(font);
	if (!ink.valid) return 0;

	const int fontLineHeight = font.lineHeight();
	const int lineAdvance = lineHeight > 0 ? lineHeight : fontLineHeight;
	const int lineBoxOffset = lineHeight > 0 ? (lineAdvance - fontLineHeight) / 2 : 0;
	const int baseline = lineBoxOffset + font.ascender();
	const int iy0 = baseline - ink.above;
	const int offset = roundedHalf((lineAdvance - 1) - (iy0 + baseline - 1));

	const int lowest = baseline + ink.below - 1;
	if (lowest + offset > lineAdvance - 1) return std::max((lineAdvance - 1) - lowest, -iy0);
	if (iy0 + offset < 0) return -iy0;
	return offset;
}

bool recordProjectedRasterText(const Node &node, const char *text, uint8_t /*parentAlpha*/, int tx, int ty, int tw)
{
	if (node.style.font_id < 0 || std::strchr(text, '\n')) return false;
	const int fontSize = node.style.font_size > 0 ? node.style.font_size : kBitmapFontHeight;
	gea::framework::graphics::RasterizedFont font =
	    gea::framework::graphics::FontRegistry::rasterizedFamily(node.style.font_id, fontSize);
	if (!font.valid()) return false;

	int lineWidth = 0;
	for (const char *p = text; *p;) {
		gea::framework::graphics::Glyph glyph{};
		const int cp = nextUtf8Codepoint(p);
		lineWidth += font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
	}

	int bx0, by0, bx1, by1;
	ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);

 if (lineWidth <= 0) return true;

	const int fontLineHeight = font.lineHeight();
	const int lineAdvance = node.style.line_height > 0 ? node.style.line_height : fontLineHeight;
	const int lineBoxOffset = node.style.line_height > 0 ? (lineAdvance - fontLineHeight) / 2 : 0;
	const int srcX = tx + alignedOffset(node.style.text_align, tw, lineWidth);
	const int srcY = ty + lineBoxOffset;
	const int srcW = lineWidth;
	const int srcH = fontLineHeight;
	if (srcW <= 0 || srcH <= 0) return false;

	int16_t xs[4], ys[4];
	ViewRenderer::transformedRectCorners(node, false, srcX, srcY, srcW, srcH, xs, ys);
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return false;
	cmd->type = DisplayCommandType::DrawProjectedText;
	cmd->bx = bx0;
	cmd->by = by0;
	cmd->bw = bx1 - bx0 + 1;
	cmd->bh = by1 - by0 + 1;
	cmd->projectedText.text = node.text.c_str();
	cmd->projectedText.srcX = srcX;
	cmd->projectedText.srcY = srcY;
	cmd->projectedText.srcW = srcW;
	cmd->projectedText.srcH = srcH;
	cmd->projectedText.x0 = xs[0];
	cmd->projectedText.y0 = ys[0];
	cmd->projectedText.x1 = xs[1];
	cmd->projectedText.y1 = ys[1];
	cmd->projectedText.x2 = xs[2];
	cmd->projectedText.y2 = ys[2];
	cmd->projectedText.x3 = xs[3];
	cmd->projectedText.y3 = ys[3];
	cmd->projectedText.fontId = node.style.font_id;
	cmd->projectedText.fontSize = fontSize;
	cmd->projectedText.color = node.style.text_color;
	cmd->projectedText.alpha = node.style.text_alpha;
	cmd->projectedText.textTransform = node.style.text_transform;
	cmd->projectedText.whiteSpace = node.style.white_space;
	// Flattened ancestor groups are culled by the recorder for every paint type.
	// This command's flag describes only its own face in a preserve-3d group.
	cmd->projectedText.backfaceHidden = node.style.backface_hidden ? 1 : 0;
	return true;
}
#endif

// Host-overridable text measurement hook. Platforms that render text through
// their own typesetter (e.g. macOS via NSTextField + CoreText) need the
// framework's layout sizes to match what the host will actually draw, not the
// framework's bitmap-font fallback. A platform that wants to take over
// measurement defines this symbol to return true and fill *outW/*outH; the
// default weak no-op returns false and lets the bitmap/rasterized paths run.
extern "C" __attribute__((weak)) bool gea_host_measure_text(const char *text,
                                                            int maxWidth,
                                                            int fontId,
                                                            int fontSize,
                                                            int *outWidth,
                                                            int *outHeight);
extern "C" __attribute__((weak)) bool gea_host_measure_text_with_line_height(const char *text,
                                                                             int maxWidth,
                                                                             int fontId,
                                                                             int fontSize,
                                                                             int lineHeight,
                                                                             int *outWidth,
                                                                             int *outHeight);
extern "C" __attribute__((weak)) bool gea_host_measure_text_with_style(const char *text,
                                                                       int maxWidth,
                                                                       int fontId,
                                                                       int fontSize,
                                                                       int fontWeight,
                                                                       int lineHeight,
                                                                       int *outWidth,
                                                                       int *outHeight);
extern "C" __attribute__((weak)) bool gea_host_measure_text(const char *,
                                                            int,
                                                            int,
                                                            int,
                                                            int *,
                                                            int *)
{
	return false;
}
extern "C" __attribute__((weak)) bool gea_host_measure_text_with_line_height(const char *,
                                                                             int,
                                                                             int,
                                                                             int,
                                                                             int,
                                                                             int *,
                                                                             int *)
{
	return false;
}
extern "C" __attribute__((weak)) bool gea_host_measure_text_with_style(const char *,
                                                                       int,
                                                                       int,
                                                                       int,
                                                                       int,
                                                                       int,
                                                                       int *,
                                                                       int *)
{
	return false;
}

class TextMetrics {
public:
	static TextMeasure measure(const char *text, int maxWidth, int fontId, int fontSize, int lineHeight = 0, int fontWeight = 400)
	{
		// Give the host first crack — when it returns true we trust its
		// measurement absolutely, even if the values look surprising next to
		// the bitmap fallback (e.g. macOS reports actual CoreText-laid-out
		// widths which include kerning while the bitmap path would be a
		// crude advance sum).
		{
			int hw = 0;
			int hh = 0;
			if (gea_host_measure_text_with_style(text, maxWidth, fontId, fontSize, fontWeight, lineHeight, &hw, &hh) ||
			    gea_host_measure_text_with_line_height(text, maxWidth, fontId, fontSize, lineHeight, &hw, &hh) ||
			    gea_host_measure_text(text, maxWidth, fontId, fontSize, &hw, &hh)) {
				TextMeasure m;
				m.width = hw;
				m.height = hh;
				return m;
			}
		}
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
		if (fontId >= 0) {
			const TextMeasure measured = measureRasterized(text, maxWidth, fontId, fontSize, lineHeight);
			if (!text || !text[0] || measured.height > 0) return measured;
		}
#else
		(void)fontId;
#endif
		const float scale = fontSize > 0 ? static_cast<float>(fontSize) / kBitmapFontHeight : 1.0f;
		return measureBitmap(text, maxWidth, scale, lineHeight);
	}

	static TextMeasure measureBitmap(const char *text, int maxWidth, float scale, int lineHeight = 0)
	{
		TextMeasure result{};
		if (!text || !text[0]) return result;

		int glyphWidth = static_cast<int>(kBitmapFontWidth * scale + 0.5f);
		int glyphHeight = static_cast<int>(kBitmapFontHeight * scale + 0.5f);
		if (glyphWidth < 1) glyphWidth = 1;
		if (glyphHeight < 1) glyphHeight = 1;
		const int lineAdvance = lineHeight > 0 ? lineHeight : glyphHeight;

		int maxLineWidth = 0;
		int lines = 1;
		if (maxWidth <= 0) maxWidth = 32767;
		const char *lineStart = text;
		while (*lineStart) {
			const WrappedLine line = nextWrappedLine(lineStart, maxWidth, [&](int) { return glyphWidth; });
			if (line.width > maxLineWidth) maxLineWidth = line.width;
			lineStart += line.consumedBytes;
			if (*lineStart) lines++;
		}
		result.width = maxLineWidth;
		result.height = lines * lineAdvance;
		return result;
	}

#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	static TextMeasure measureRasterized(const char *text, int maxWidth, int fontId, int fontSize, int lineHeight = 0)
	{
		TextMeasure result{};
		if (!text || !text[0]) return result;

		gea::framework::graphics::RasterizedFont font = gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize);
		if (!font.valid()) return result;
		const int lineAdvance = lineHeight > 0 ? lineHeight : font.lineHeight();

		int maxLineWidth = 0;
		int lines = 1;
		if (maxWidth <= 0) maxWidth = 32767;
		const char *lineStart = text;
		while (*lineStart) {
			const WrappedLine line = nextWrappedLine(lineStart, maxWidth, [&](int cp) {
			gea::framework::graphics::Glyph glyph{};
				return font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
			});
			if (line.width > maxLineWidth) maxLineWidth = line.width;
			lineStart += line.consumedBytes;
			if (*lineStart) lines++;
		}
		result.width = maxLineWidth;
		result.height = lines * lineAdvance;
		return result;
	}
#endif
};

// Per-codepoint advances for one node, taken from exactly the source the DRAW
// path wraps with: the rasterized atlas when the family resolves, the fixed-cell
// bitmap font otherwise. Inline fragmentation must agree with the drawer glyph
// for glyph — a wrapper that disagrees with the one that paints puts a line
// where the box has no room for it.
struct GlyphAdvanceSource {
	bool valid = false;
	int lineAdvance = 0;
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	bool rasterized = false;
	gea::framework::graphics::RasterizedFont font{};
#endif
	int glyphWidth = 0;

	int operator()(int cp) const
	{
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
		if (rasterized) {
			gea::framework::graphics::Glyph glyph{};
			return font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
		}
#else
		(void)cp;
#endif
		return glyphWidth;
	}
};

GlyphAdvanceSource advanceSourceForNode(const Node &node)
{
	GlyphAdvanceSource src;
	const int fontSize = node.style.font_size > 0 ? node.style.font_size : kBitmapFontHeight;
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	if (node.style.font_id >= 0) {
		src.font = gea::framework::graphics::FontRegistry::rasterizedFamily(node.style.font_id, fontSize);
		if (src.font.valid()) {
			src.rasterized = true;
			src.valid = true;
			src.lineAdvance = node.style.line_height > 0 ? node.style.line_height : src.font.lineHeight();
			return src;
		}
	}
#endif
	const float scale = static_cast<float>(fontSize) / kBitmapFontHeight;
	src.glyphWidth = static_cast<int>(kBitmapFontWidth * scale + 0.5f);
	if (src.glyphWidth < 1) src.glyphWidth = 1;
	int glyphHeight = static_cast<int>(kBitmapFontHeight * scale + 0.5f);
	if (glyphHeight < 1) glyphHeight = 1;
	src.lineAdvance = node.style.line_height > 0 ? node.style.line_height : glyphHeight;
	src.valid = true;
	return src;
}

// Bytes of collapsible whitespace at the head of `text`. CSS removes them when
// they fall at the start of a line box.
int leadingCollapsibleBytes(const char *text)
{
	const char *p = text;
	while (*p) {
		const char *glyphStart = p;
		const char *probe = p;
		const int cp = nextUtf8Codepoint(probe);
		if (!isWrappingSpace(cp)) return static_cast<int>(glyphStart - text);
		p = probe;
	}
	return static_cast<int>(p - text);
}

TextLayoutMeasure measureTextLayoutForNode(const Node &node, const char *text, int contentWidth)
{
	if (contentWidth < 0) contentWidth = 0;
	std::string transformed;
	const char *measureText = textWithTransform(text, node.style.text_transform, transformed, node.style.white_space);
	// white-space: nowrap lays the run out as a single line — measure against an
	// unbounded width so the content box sizes to the full text (then max-width /
	// the parent box clamps it). Drawing truncates/ellipsizes the overflow.
	const int measureWidth = (node.style.white_space == 1 || node.style.white_space == 2) ? 32767 : contentWidth;
	const TextMeasure measured = TextMetrics::measure(measureText,
	                                                  measureWidth,
	                                                  node.style.font_id,
	                                                  node.style.font_size,
	                                                  node.style.line_height,
	                                                  node.style.font_weight);
	TextLayoutMeasure out;
	out.width = measured.width + boxInset(node.style, 1) + boxInset(node.style, 3);
	out.height = measured.height + boxInset(node.style, 0) + boxInset(node.style, 2);
	if (node.style.width != kUnset) out.width = contentSizeToBorderSize(node.style, node.style.width, true);
	else if ((node.style.width_percent != kUnset || node.style.width_expression >= 0) && node.layout.width > 0) out.width = node.layout.width;
	if (node.style.height != kUnset) out.height = contentSizeToBorderSize(node.style, node.style.height, false);
	else if ((node.style.height_percent != kUnset || node.style.height_expression >= 0) && node.layout.height > 0) out.height = node.layout.height;
	out.width = clampBorderBoxSize(node.style, out.width, true);
	out.height = clampBorderBoxSize(node.style, out.height, false);
	return out;
}

int measureSingleLineTextWidthForNode(const Node &node, const char *text)
{
	if (!text || !text[0]) return 0;
	std::string transformed;
	const char *measureText = textWithTransform(text, node.style.text_transform, transformed, node.style.white_space);
	return TextMetrics::measure(measureText,
	                            32767,
	                            node.style.font_id,
	                            node.style.font_size,
	                            node.style.line_height,
	                            node.style.font_weight).width;
}

struct TextCoverageSink {
	int screenY = 0;
	int screenX = 0;
	int width = 0;
	int clipX0 = 0;
	int clipY0 = 0;
	int clipX1 = -1;
	int clipY1 = -1;
	std::uint8_t *coverage = nullptr;

	void add(int x, int y, std::uint8_t value) const
	{
		if (!coverage || value == 0 || y != screenY || y < clipY0 || y > clipY1 ||
		    x < screenX || x >= screenX + width || x < clipX0 || x > clipX1) return;
		auto &dst = coverage[x - screenX];
		if (value > dst) dst = value;
	}
};

class TextDrawer {
public:
	static void unionCoverageRow(const DisplayCommand &command, int screenY, int screenX,
	                             int width, std::uint8_t *outCoverage)
	{
		if (!outCoverage || width <= 0) return;
		if (command.textDecorationInk) {
			if (command.type != DisplayCommandType::FillRect) return;
			if (screenY < command.fill.y || screenY >= command.fill.y + command.fill.h) return;
			int clipX0, clipY0, clipX1, clipY1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			if (screenY < clipY0 || screenY > clipY1) return;
			const int left = std::max({screenX, static_cast<int>(command.fill.x), clipX0});
			const int right = std::min({screenX + width, static_cast<int>(command.fill.x) + command.fill.w, clipX1 + 1});
			for (int x = left; x < right; ++x) outCoverage[x - screenX] = 255;
			return;
		}
		if (command.type != DisplayCommandType::DrawText) return;
		TextCoverageSink sink;
		sink.screenY = screenY;
		sink.screenX = screenX;
		sink.width = width;
		sink.coverage = outCoverage;
		drawWrapped(command.text.text, command.text.x, command.text.y, command.text.maxWidth,
		            command.text.color, command.text.scale, command.text.align, command.text.containerWidth,
			    command.text.fontId, command.text.textTransform, command.text.lineHeight,
			    command.text.whiteSpace, command.text.textOverflow, command.text.maxHeight,
			    command.text.firstLineIndent, &sink);
	}

#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	static void rasterizedGlyphRun(const char *text, int x, int y, int fontId, int fontSize,
	                              const TextCoverageSink &sink)
	{
		gea::framework::graphics::RasterizedFont font =
		    gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize);
		if (!font.valid()) return;
		int penX = x;
		for (const char *p = text; p && *p;) {
			const int cp = nextUtf8Codepoint(p);
			gea::framework::graphics::Glyph glyph{};
			if (!font.glyph(cp, &glyph)) { penX += font.sizePx() / 2; continue; }
			const int gx = penX + glyph.bearingX;
			const int gy = y + font.ascender() - glyph.bearingY;
			const int row = sink.screenY - gy;
			if (row >= 0 && row < glyph.height)
				for (int col = 0; col < glyph.width; ++col)
					sink.add(gx + col, sink.screenY, font.coverage(glyph, row, col));
			penX += glyph.advance;
		}
	}
#endif

	static void bitmapGlyphRun(const char *text, int x, int y, float scale,
	                           const TextCoverageSink &sink)
	{
		if (scale < 0.1f) scale = 1.0f;
		const int glyphWidth = std::max(1, static_cast<int>(kBitmapFontWidth * scale + 0.5f));
		const int glyphHeight = std::max(1, static_cast<int>(kBitmapFontHeight * scale + 0.5f));
		const int row = sink.screenY - y;
		if (row < 0 || row >= glyphHeight) return;
		const int srcRow = row * kBitmapFontHeight / glyphHeight;
		const auto &font = gea::framework::graphics::FontRegistry::bitmap8x16();
		int penX = x;
		for (const char *p = text; p && *p;) {
			const int cp = nextUtf8Codepoint(p);
			if (cp == '\n') { penX = x; continue; }
			gea::framework::graphics::Glyph glyph{};
			font.glyph(cp, &glyph);
			for (int col = 0; col < glyphWidth; ++col) {
				const int srcCol = col * kBitmapFontWidth / glyphWidth;
				sink.add(penX + col, sink.screenY, font.coverage(glyph, srcRow, srcCol));
			}
			penX += glyphWidth;
		}
	}

	static void drawWrapped(const char *text, int x, int y, int maxWidth, gea::framework::graphics::pixel::native_t color, float scale, int textAlign, int containerWidth, int fontId, int textTransform, int lineHeight, int whiteSpace, int textOverflow, int maxHeight = 0, int firstLineIndent = 0, TextCoverageSink *coverageSink = nullptr)
	{
		if (!text || !text[0]) return;
		std::string transformed;
		text = textWithTransform(text, textTransform, transformed, whiteSpace);
		// Pre preserves forced breaks and paints every line without soft wrap.
		if (whiteSpace == 2) maxWidth = 32767;

		int clipX0;
		int clipY0;
		int clipX1;
		int clipY1;
		gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
		TextCoverageSink clippedSink;
		if (coverageSink) {
			clippedSink = *coverageSink;
			clippedSink.clipX0 = clipX0;
			clippedSink.clipY0 = clipY0;
			clippedSink.clipX1 = clipX1;
			clippedSink.clipY1 = clipY1;
			coverageSink = &clippedSink;
		}

		const bool noWrap = whiteSpace == 1;
		const bool ellipsis = textOverflow == 1;

#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
		if (fontId >= 0) {
			const int fontSize = static_cast<int>(scale * kBitmapFontHeight + 0.5f);
			if (gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize).valid()) {
				if (noWrap)
					drawRasterizedSingleLine(text, x, y, maxWidth, color, textAlign, containerWidth, fontId, fontSize, lineHeight, ellipsis, clipX0, clipY0, clipX1, clipY1, coverageSink);
				else
					drawRasterizedWrapped(text, x, y, maxWidth, color, textAlign, containerWidth, fontId, fontSize, lineHeight, clipX0, clipY0, clipX1, clipY1,
					                      ellipsis ? maxHeight : 0, firstLineIndent, coverageSink);
				return;
			}
		}
#else
		(void)fontId;
#endif
		if (noWrap)
			drawBitmapSingleLine(text, x, y, maxWidth, color, scale, textAlign, containerWidth, lineHeight, ellipsis, clipX0, clipY0, clipX1, clipY1, coverageSink);
		else
			drawBitmapWrapped(text, x, y, maxWidth, color, scale, textAlign, containerWidth, lineHeight, clipX0, clipY0, clipX1, clipY1, firstLineIndent, coverageSink);
	}

private:
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	static void drawRasterizedWrapped(const char *text, int x, int y, int maxWidth, std::uint16_t color, int textAlign, int containerWidth, int fontId, int fontSize, int lineHeight, int clipX0, int clipY0, int clipX1, int clipY1, int ellipsisMaxHeight = 0, int firstLineIndent = 0, const TextCoverageSink *coverageSink = nullptr)
	{
		// Inline continuation: the run's first line starts `firstLineIndent` px in
		// from the box's left edge (the pen position it inherited from the box
		// before it on that line box) and therefore has that much less room. A
		// negative indent is the leading-whitespace trim — see
		// InlineFlowMeasure::firstLineIndentAdjust.
		const auto lineBudget_ = [&](int li) {
			const int budget = li == 0 ? maxWidth - firstLineIndent : maxWidth;
			return budget < 1 ? 1 : budget;
		};
		const auto lineOriginX_ = [&](int li) { return li == 0 ? x + firstLineIndent : x; };
		gea::framework::graphics::RasterizedFont font = gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize);
		if (!font.valid()) return;

		const int glyphHeight = font.lineHeight();
		const int lineAdvance = lineHeight > 0 ? lineHeight : glyphHeight;
		const int lineBoxOffset = lineHeight > 0 ? (lineAdvance - glyphHeight) / 2 : 0;
		// Multi-line clamp (text-overflow: ellipsis on WRAPPED text): the content
		// box caps how many lines draw; the last budgeted line renders through
		// the single-line ellipsis routine when more text would follow it.
		const int lineBudget = ellipsisMaxHeight > 0 ? (ellipsisMaxHeight / lineAdvance > 0 ? ellipsisMaxHeight / lineAdvance : 1) : 0x7fffffff;

		// Cache lookup: if the same (text content, layout params) was laid out
		// previously, skip the per-glyph advance loop and iterate the cached
		// chars[] array directly. See LineBreakCacheEntry comment.
		const std::uint32_t hash = hashTextPrefix(text);
		const int slot = lineBreakCacheSlot(text, maxWidth);
		LineBreakCacheEntry &entry = gLineBreakCache[slot];
		const bool cacheHit =
			entry.textPtr == text &&
			entry.textHashPrefix == hash &&
			entry.maxWidth == maxWidth &&
			entry.firstLineIndent == firstLineIndent &&
			entry.fontId == fontId &&
			entry.fontSize == fontSize &&
			entry.lineHeight == lineAdvance;

		if (!cacheHit) {
			entry.textPtr = text;
			entry.textHashPrefix = hash;
			entry.maxWidth = maxWidth;
			entry.firstLineIndent = firstLineIndent;
			entry.fontId = fontId;
			entry.fontSize = fontSize;
			entry.lineHeight = lineAdvance;
			entry.scaleQ8 = 0;
			entry.lineCount = 0;

			const char *p = text;
			while (*p && entry.lineCount < kMaxCachedLines) {
				const WrappedLine line = nextWrappedLine(p, lineBudget_(entry.lineCount), [&](int cp) {
					gea::framework::graphics::Glyph glyph{};
					return font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
				});
				entry.renderBytes[entry.lineCount] = static_cast<std::uint16_t>(line.renderBytes > 65535 ? 65535 : line.renderBytes);
				entry.consumedBytes[entry.lineCount] = static_cast<std::uint16_t>(line.consumedBytes > 65535 ? 65535 : line.consumedBytes);
				entry.lineWidths[entry.lineCount] = static_cast<std::uint16_t>(line.width > 65535 ? 65535 : line.width);
				entry.lineCount++;
				p += line.consumedBytes;
			}
			// If we ran out of cache slots, mark invalid so we don't read junk.
			if (*p) {
				entry.textPtr = nullptr;  // invalidate; fall through to original loop
			}
		}

		// Iterate cached (or freshly populated) lines with the Y-clip filter.
		// Above-clip lines skip the entire render call. The lineStart pointer
		// advances via cached `chars` — no per-glyph advance work.
		if (entry.textPtr == text) {
			int penY = y;
			const char *lineStart = text;
			for (int li = 0; li < entry.lineCount; li++) {
				if (penY + lineBoxOffset > clipY1) break;
				if (li == lineBudget - 1 && li < entry.lineCount - 1) {
					// Last budgeted line with more text behind it: ellipsize the rest.
						drawRasterizedSingleLine(lineStart, lineOriginX_(li), penY, lineBudget_(li), color, textAlign, containerWidth,
						                         fontId, fontSize, lineHeight, /*ellipsis=*/true,
						                         clipX0, clipY0, clipX1, clipY1, coverageSink);
					return;
				}
				const int chars = entry.renderBytes[li];
				const int lineBoxY = penY + lineBoxOffset;
				if (lineBoxY + glyphHeight - 1 >= clipY0) {
					const int lineWidth = entry.lineWidths[li];
					const int lineX = lineOriginX_(li) + alignedOffset(textAlign, containerWidth, lineWidth);
					if (lineX <= clipX1 && lineX + lineWidth - 1 >= clipX0) {
						char lineBuffer[kWrappedLineBufferBytes + 1];
						const int copyLen = chars < kWrappedLineBufferBytes ? chars : kWrappedLineBufferBytes;
						std::memcpy(lineBuffer, lineStart, copyLen);
						lineBuffer[copyLen] = '\0';
						if (coverageSink) rasterizedGlyphRun(lineBuffer, lineX, lineBoxY, fontId, fontSize, *coverageSink);
						else gea::platform::display::Display::drawTextFontFamily(lineBuffer, lineX, lineBoxY, color, fontId, fontSize);
					}
				}
				lineStart += entry.consumedBytes[li];
				penY += lineAdvance;
			}
			return;
		}

		// Fallback for the cache-overflow case (text with > 64 lines).
		const char *lineStart = text;
		int penY = y;
		int li = 0;
		while (*lineStart) {
			if (penY + lineBoxOffset > clipY1) break;
			if (li == lineBudget - 1) {
				// Budget reached: ellipsize whatever remains onto this final line.
				drawRasterizedSingleLine(lineStart, lineOriginX_(li), penY, lineBudget_(li), color, textAlign, containerWidth,
				                         fontId, fontSize, lineHeight, /*ellipsis=*/true,
				                         clipX0, clipY0, clipX1, clipY1, coverageSink);
				return;
			}
			const int lineIndex = li;
			li++;

			const WrappedLine line = nextWrappedLine(lineStart, lineBudget_(lineIndex), [&](int cp) {
				gea::framework::graphics::Glyph glyph{};
				return font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
			});

			const int lineBoxY = penY + lineBoxOffset;
			if (lineBoxY + glyphHeight - 1 >= clipY0) {
				const int lineX = lineOriginX_(lineIndex) + alignedOffset(textAlign, containerWidth, line.width);
				if (lineX <= clipX1 && lineX + line.width - 1 >= clipX0) {
					char lineBuffer[kWrappedLineBufferBytes + 1];
					const int copyLen = line.renderBytes < kWrappedLineBufferBytes ? line.renderBytes : kWrappedLineBufferBytes;
					std::memcpy(lineBuffer, lineStart, copyLen);
					lineBuffer[copyLen] = '\0';
				if (coverageSink) rasterizedGlyphRun(lineBuffer, lineX, lineBoxY, fontId, fontSize, *coverageSink);
				else gea::platform::display::Display::drawTextFontFamily(lineBuffer, lineX, lineBoxY, color, fontId, fontSize);
				}
			}

			lineStart += line.consumedBytes;
			penY += lineAdvance;
		}
	}

	// white-space: nowrap path. Lays the run out as a single line (newlines end
	// the line) and, when it overflows maxWidth, truncates at a glyph boundary —
	// appending an ASCII "..." when text-overflow: ellipsis is set, or hard-
	// clipping at the box edge otherwise. The truncated run always fits within
	// maxWidth, so it never wraps and never spills past the content box.
	static void drawRasterizedSingleLine(const char *text, int x, int y, int maxWidth, std::uint16_t color, int textAlign, int containerWidth, int fontId, int fontSize, int lineHeight, bool ellipsis, int clipX0, int clipY0, int clipX1, int clipY1, const TextCoverageSink *coverageSink = nullptr)
	{
		gea::framework::graphics::RasterizedFont font = gea::framework::graphics::FontRegistry::rasterizedFamily(fontId, fontSize);
		if (!font.valid()) return;

		const int glyphHeight = font.lineHeight();
		const int lineAdvance = lineHeight > 0 ? lineHeight : glyphHeight;
		const int lineBoxOffset = lineHeight > 0 ? (lineAdvance - glyphHeight) / 2 : 0;
		const int lineBoxY = y + lineBoxOffset;
		if (lineBoxY > clipY1 || lineBoxY + glyphHeight - 1 < clipY0) return;
		if (maxWidth <= 0) maxWidth = 32767;

		const auto advanceOf = [&font](int cp) {
			gea::framework::graphics::Glyph glyph{};
			return font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
		};

		// Full single-line advance width (stop at the first newline).
		int fullWidth = 0;
		for (const char *p = text; *p && *p != '\n';) fullWidth += advanceOf(nextUtf8Codepoint(p));

		char buf[256];
		int len = 0;
		int drawnWidth = 0;
		constexpr int kBufCap = static_cast<int>(sizeof(buf));

		if (fullWidth <= maxWidth) {
			for (const char *p = text; *p && *p != '\n' && len < kBufCap - 1; ++p) buf[len++] = *p;
			drawnWidth = fullWidth;
		} else if (ellipsis) {
			const int dotAdvance = advanceOf('.');
			const int ellipsisWidth = dotAdvance * 3;
			const int budget = maxWidth > ellipsisWidth ? maxWidth - ellipsisWidth : 0;
			int w = 0;
			const char *p = text;
			while (*p && *p != '\n') {
				const char *glyphStart = p;
				const int advance = advanceOf(nextUtf8Codepoint(p));
				if (w + advance > budget) break;
				const int bytes = static_cast<int>(p - glyphStart);
				if (len + bytes > kBufCap - 4) break;  // leave room for "..." + NUL
				for (int i = 0; i < bytes; ++i) buf[len++] = glyphStart[i];
				w += advance;
			}
			buf[len++] = '.';
			buf[len++] = '.';
			buf[len++] = '.';
			drawnWidth = w + ellipsisWidth;
		} else {
			int w = 0;
			const char *p = text;
			while (*p && *p != '\n') {
				const char *glyphStart = p;
				const int advance = advanceOf(nextUtf8Codepoint(p));
				if (w + advance > maxWidth && len > 0) break;
				const int bytes = static_cast<int>(p - glyphStart);
				if (len + bytes > kBufCap - 1) break;
				for (int i = 0; i < bytes; ++i) buf[len++] = glyphStart[i];
				w += advance;
			}
			drawnWidth = w;
		}

		if (len <= 0) return;
		buf[len] = '\0';
		const int xOffset = alignedOffset(textAlign, containerWidth, drawnWidth);
		if (coverageSink) rasterizedGlyphRun(buf, x + xOffset, lineBoxY, fontId, fontSize, *coverageSink);
		else gea::platform::display::Display::drawTextFontFamily(buf, x + xOffset, lineBoxY, color, fontId, fontSize);
	}
#endif

	static void drawBitmapWrapped(const char *text, int x, int y, int maxWidth, std::uint16_t color, float scale, int textAlign, int containerWidth, int lineHeight, int clipX0, int clipY0, int clipX1, int clipY1, int firstLineIndent = 0, const TextCoverageSink *coverageSink = nullptr)
	{
		if (scale < 0.1f) scale = 1.0f;
		int glyphWidth = static_cast<int>(kBitmapFontWidth * scale + 0.5f);
		int glyphHeight = static_cast<int>(kBitmapFontHeight * scale + 0.5f);
		if (glyphWidth < 1) glyphWidth = 1;
		if (glyphHeight < 1) glyphHeight = 1;
		const int lineAdvance = lineHeight > 0 ? lineHeight : glyphHeight;

		const char *lineStart = text;
		int penY = y;
		int lineIndex = 0;

		while (*lineStart) {
			if (penY > clipY1) break;
			// See drawRasterizedWrapped: line 0 of an inline continuation starts at
			// the inherited pen and has that much less room.
			const int budget = lineIndex == 0 ? maxWidth - firstLineIndent : maxWidth;
			const int lineOriginX = lineIndex == 0 ? x + firstLineIndent : x;
			const WrappedLine line = nextWrappedLine(lineStart, budget < 1 ? 1 : budget, [&](int) { return glyphWidth; });
			lineIndex++;

			if (penY + glyphHeight - 1 >= clipY0) {
				int penX = lineOriginX + alignedOffset(textAlign, containerWidth, line.width);
				if (penX <= clipX1 && penX + line.width - 1 >= clipX0) {
					const char *glyph = lineStart;
					const char *renderEnd = lineStart + line.renderBytes;
					while (glyph < renderEnd) {
						const char *glyphStart = glyph;
						nextUtf8Codepoint(glyph);
						char buf[5] = {};
						const int bytes = static_cast<int>(glyph - glyphStart);
						std::memcpy(buf, glyphStart, bytes);
						if (coverageSink) bitmapGlyphRun(buf, penX, penY, scale, *coverageSink);
						else gea::platform::display::Display::drawText(buf, penX, penY, color, scale);
						penX += glyphWidth;
					}
				}
			}

			lineStart += line.consumedBytes;
			penY += lineAdvance;
		}
	}

	// Bitmap-font nowrap path — fixed-cell analogue of drawRasterizedSingleLine.
	// Like drawBitmapWrapped, it treats each CODEPOINT as one fixed-width cell:
	// that is what `measureBitmap` charges for, so counting bytes here put the
	// truncation, the ellipsis budget and the alignment offset in different units
	// from the layout that placed the text.
	static void drawBitmapSingleLine(const char *text, int x, int y, int maxWidth, std::uint16_t color, float scale, int textAlign, int containerWidth, int lineHeight, bool ellipsis, int clipX0, int clipY0, int clipX1, int clipY1, const TextCoverageSink *coverageSink = nullptr)
	{
		(void)lineHeight;
		if (scale < 0.1f) scale = 1.0f;
		int glyphWidth = static_cast<int>(kBitmapFontWidth * scale + 0.5f);
		int glyphHeight = static_cast<int>(kBitmapFontHeight * scale + 0.5f);
		if (glyphWidth < 1) glyphWidth = 1;
		if (glyphHeight < 1) glyphHeight = 1;
		const int penY = y;
		if (penY > clipY1 || penY + glyphHeight - 1 < clipY0) return;
		if (maxWidth <= 0) maxWidth = 32767;

		int fullChars = 0;
		for (const char *p = text; *p && *p != '\n';) {
			nextUtf8Codepoint(p);
			fullChars++;
		}
		const int fullWidth = fullChars * glyphWidth;

		int drawChars = fullChars;
		bool addEllipsis = false;
		if (fullWidth > maxWidth) {
			if (ellipsis) {
				const int ellipsisWidth = 3 * glyphWidth;
				const int budget = maxWidth > ellipsisWidth ? maxWidth - ellipsisWidth : 0;
				drawChars = budget / glyphWidth;
				addEllipsis = true;
			} else {
				drawChars = maxWidth / glyphWidth;
				if (drawChars < 1) drawChars = 1;
			}
			if (drawChars > fullChars) drawChars = fullChars;
		}

		const int drawnWidth = drawChars * glyphWidth + (addEllipsis ? 3 * glyphWidth : 0);
		if (drawnWidth <= 0) return;
		int penX = x + alignedOffset(textAlign, containerWidth, drawnWidth);
		if (penX > clipX1 || penX + drawnWidth - 1 < clipX0) return;
		const char *glyph = text;
		for (int i = 0; i < drawChars && *glyph && *glyph != '\n'; ++i) {
			const char *glyphStart = glyph;
			nextUtf8Codepoint(glyph);
			char b[5] = {};
			std::memcpy(b, glyphStart, static_cast<std::size_t>(glyph - glyphStart));
			if (coverageSink) bitmapGlyphRun(b, penX, penY, scale, *coverageSink);
			else gea::platform::display::Display::drawText(b, penX, penY, color, scale);
			penX += glyphWidth;
		}
		if (addEllipsis) {
			for (int i = 0; i < 3; ++i) {
				char b[2] = { '.', '\0' };
				if (coverageSink) bitmapGlyphRun(b, penX, penY, scale, *coverageSink);
				else gea::platform::display::Display::drawText(b, penX, penY, color, scale);
				penX += glyphWidth;
			}
		}
	}

	static int alignedOffset(int textAlign, int containerWidth, int lineWidth)
	{
		int offset = 0;
		if (textAlign == 1) offset = (containerWidth - lineWidth) / 2;
		else if (textAlign == 2) offset = containerWidth - lineWidth;
		return offset < 0 ? 0 : offset;
	}
};

}  // namespace

void Tree::setText(int node, const char *text)
{
	auto &perf = refreshPerfStatsMutable();
	ScopedRefreshStat timer(perf.treeSetTextUs);
	perf.treeSetTextCalls++;
	if (node < 0 || node >= nodeCount() || !text) return;
	Node &target = this->node(node);
	if (target.text == text) return;
	perf.treeSetTextChanged++;
	// Hidden subtree: just store the text. A display:none node paints nothing, so
	// dirtying it for a text change would churn layout (its zero-size box defeats
	// the bbox prediction below -> full relayout) and drop the static-backdrop
	// cache every tick — a hidden ticking label re-baked the rotary's 230k-px
	// floor gradient (~185ms) on every value change. The display-toggle path
	// re-records the subtree with the fresh text when it becomes visible.
	{
		int guard = 0;
		for (int a = node; a >= 0 && a < nodeCount() && guard <= nodeCount(); a = this->node(a).parent, ++guard)
		{
			if (this->node(a).style.display == 1)
			{
				// Same realloc-dangle guard as the visible path below: a node hidden
				// NOW may still have retained commands from when it was visible.
				const char *gid_oldBuffer = target.text.c_str();
				target.text.assign(text);
				if (target.text.c_str() != gid_oldBuffer) DisplayList::instance().scrubNodeText(gid_oldBuffer);
				perf.treeSetTextHidden++;
				return;
			}
		}
	}
	// Dynamic-leaf text (a ticking fps badge, a clock): mark it STICKY-excluded so
	// every future backdrop bake skips it, and only drop the existing cache when
	// this node's pixels are actually baked into it. An excluded ticker updates
	// through the normal dirty-region replay without ever touching the backdrop.
	// Sticky on purpose: a decaying cooldown re-baked the ticker in whenever its
	// text went stable for a few seconds, and the next change invalidated again —
	// each cycle a full-screen rebake+sync that flashed the static stage at
	// fps-jitter-random times. The INITIAL set (empty -> value, the mount) is not
	// a tick — marking it would keep every static label out of the first bake.
	{
		auto &st = treeState();
		if (!target.text.empty())
			st.nodeBackdropCooldown[node] = 1;
#ifndef ESP_PLATFORM
		{
			static const bool debugBackdrop = std::getenv("GEA_DEBUG_BACKDROP") != nullptr;
			if (debugBackdrop)
				std::printf("[settext] node=%d '%s'->'%s' backdropActive=%d inBackdrop=%d\n",
										node, target.text.c_str(), text,
										DisplayList::instance().staticBackdropActive() ? 1 : 0,
										st.nodeInBackdrop[node] ? 1 : 0);
		}
#endif
		if (!DisplayList::instance().staticBackdropActive() || st.nodeInBackdrop[node])
			DisplayList::instance().invalidateStaticBackdrop();
	}
	// Glyph-incremental dirty: diff the old text (target.text, not yet overwritten)
	// against the new text to find the unchanged common prefix/suffix, and remember
	// the old box height. If the box stays the same height below, only the middle run
	// differs, so the collector can flush just that run (a ticking counter re-copies
	// only its changed digits instead of the whole — possibly huge — glyph box).
	const std::string gid_new(text);
	const int gid_oldLen = static_cast<int>(target.text.size());
	const int gid_newLen = static_cast<int>(gid_new.size());
	int gid_prefix = 0;
	while (gid_prefix < gid_oldLen && gid_prefix < gid_newLen && target.text[gid_prefix] == gid_new[gid_prefix])
		gid_prefix++;
	int gid_suffix = 0;
	while (gid_suffix < gid_oldLen - gid_prefix && gid_suffix < gid_newLen - gid_prefix &&
	       target.text[gid_oldLen - 1 - gid_suffix] == gid_new[gid_newLen - 1 - gid_suffix])
		gid_suffix++;
	const bool gid_singleLine =
		target.text.find('\n') == std::string::npos && gid_new.find('\n') == std::string::npos;
	const int gid_oldH = target.layout.height;
	// Glyph-run widths must be measured independently of the CSS layout box.
	// measureTextLayoutForNode() intentionally replaces its natural result with an
	// explicit style width. Using it for substring bounds made every substring of a
	// fixed-width label appear equally wide. A shrinking update such as
	// "San Francisco" -> "Lisbon" then dirtied only the new, shorter run (or even a
	// sliver at the box edge), leaving the old trailing glyphs on the physical panel.
	const bool gid_canPartial = gid_singleLine && target.style.white_space == 1 &&
			target.style.text_align == 0 && gid_prefix < gid_newLen;
	int gid_preW = 0;
	int gid_rightW = 0;
	if (gid_canPartial) {
		const std::string gid_pre = gid_new.substr(0, static_cast<size_t>(gid_prefix));
		const std::string gid_oldHead = target.text.substr(0, static_cast<size_t>(gid_oldLen - gid_suffix));
		const std::string gid_newHead = gid_new.substr(0, static_cast<size_t>(gid_newLen - gid_suffix));
		gid_preW = measureSingleLineTextWidthForNode(target, gid_pre.c_str());
		const int gid_oldHeadW = measureSingleLineTextWidthForNode(target, gid_oldHead.c_str());
		const int gid_newHeadW = measureSingleLineTextWidthForNode(target, gid_newHead.c_str());
		if (gid_oldHeadW == gid_newHeadW) {
			// The unchanged suffix stays at the same x position, so it does not need
			// repainting; stop at the end of the changed head.
			gid_rightW = gid_newHeadW;
		} else {
			// The suffix moved, or the new run is shorter. Cover both complete run
			// extents so all shifted/removed glyph pixels are restored from backdrop.
			const int gid_oldTextW = measureSingleLineTextWidthForNode(target, target.text.c_str());
			const int gid_newTextW = measureSingleLineTextWidthForNode(target, gid_new.c_str());
			gid_rightW = gid_oldTextW > gid_newTextW ? gid_oldTextW : gid_newTextW;
		}
	}

	const bool gid_hadLayoutDirty = target.render.layout_dirty != 0;
	target.render.dirty = 1;
	target.render.layout_dirty = 1;
	target.render.non_scroll_dirty = 1;
	target.render.text_layout_stable = 0;
	markNodeDisplayCommandsDirty(node);
	// assign() past capacity reallocates and frees the old buffer, leaving any
	// retained display command's raw text pointer dangling until the re-record.
	// Scrub only when the buffer actually moved — in-place reuse (the common
	// same-or-smaller-length tick) keeps retained commands valid and visible.
	const char *oldBuffer = target.text.c_str();
	target.text.assign(text);
	if (target.text.c_str() != oldBuffer) DisplayList::instance().scrubNodeText(oldBuffer);

	// Predict the new text bbox so AbsoluteLeafRefresh::mode() can skip the
	// full LayoutEngine::layoutNode pass for content-only text frames.
	// Absolute-positioned text can safely accept a bbox change because it is
	// out of flow. In-flow text stays on the fast path only when its measured
	// layout box is unchanged, so flex/grid siblings and parents cannot move.
	//
	// This mirrors TextRenderer::layout's formula exactly, except for the
	// avail_w sourcing: layout receives avail_w from the parent. We don't have
	// it here. For absolute natural-width text we use a deliberately large
	// bound, matching the previous FPS-counter behavior. For in-flow text we
	// require an explicit style width, including percentages already resolved
	// by the stylesheet, so wrapping is measured against the same fixed box.
	if (target.layout.width > 0 && target.layout.height > 0 &&
	    target.parent >= 0 && target.first_child < 0 &&
	    !hasTransformState(target)) {
		const bool absolute = target.style.position == 1;
		const bool fixedWidth = target.style.width != kUnset || target.style.width_percent != kUnset;
		if (absolute || fixedWidth) {
			const int fixedBoxWidth = target.style.width != kUnset ? target.style.width : target.layout.width;
			const int content_w = fixedWidth
				? fixedBoxWidth - target.style.padding[1] - target.style.padding[3]
				: 0x4000;  // 16384 px — well beyond any panel dimension
			const TextLayoutMeasure measured = measureTextLayoutForNode(target, target.text.c_str(), content_w);
			if (absolute) {
				target.layout.width = measured.width;
				target.layout.height = measured.height;
				target.render.text_layout_stable = 1;
			} else if (measured.width == target.layout.width && measured.height == target.layout.height) {
				target.render.text_layout_stable = 1;
			}
			// Glyph-incremental: single line + left-aligned + a real change after the
			// common prefix → flush only the changed glyph run. The left edge is the
			// unchanged prefix's width (pixel-identical, never repainted). The right edge:
			// if the total width is unchanged the trailing run is pixel-stable too, so stop
			// at fullWidth-suffixWidth (tightest); if the width changed (proportional digits
			// shift the tail), cover both the old and new extents so no stale pixels remain.
			const bool gid_sameHeight = (measured.height == gid_oldH);
			if (gid_sameHeight && gid_canPartial) {
				const int gid_originX = target.layout.x + target.style.padding[3];
				constexpr int kTextDirtyPadPx = 4;  // AA / glyph-overhang guard
				target.render.text_dirty_x0 = static_cast<int16_t>(gid_originX + gid_preW - kTextDirtyPadPx);
				target.render.text_dirty_x1 = static_cast<int16_t>(gid_originX + gid_rightW + kTextDirtyPadPx);
				target.render.text_partial_dirty = 1;
			}
		}
	}
	if (target.render.text_layout_stable) {
		perf.treeSetTextStable++;
		// The pre-measure confirmed the box is unchanged: geometry can't move,
		// so this is paint-only dirt (unless something else already made the
		// node layout-dirty this frame).
		if (!gid_hadLayoutDirty)
			target.render.layout_dirty = 0;
	}
}

bool TextRenderer::canFragmentInlineRuns(const Node &node)
{
	// A host that measures whole strings (CoreText on the Apple targets) does its
	// own line breaking; this engine has no way to ask it where line two starts,
	// so those targets keep flowing each run as one box. The probe is cached: a
	// target either links the hooks or links the weak no-ops.
	static int cached = -1;
	if (cached >= 0) return cached != 0;
	int w = 0;
	int h = 0;
	const int fontId = node.style.font_id;
	const int fontSize = node.style.font_size > 0 ? node.style.font_size : kBitmapFontHeight;
	const bool hostMeasures =
	    gea_host_measure_text_with_style("Mg", 32767, fontId, fontSize, node.style.font_weight, node.style.line_height, &w, &h) ||
	    gea_host_measure_text_with_line_height("Mg", 32767, fontId, fontSize, node.style.line_height, &w, &h) ||
	    gea_host_measure_text("Mg", 32767, fontId, fontSize, &w, &h);
	cached = hostMeasures ? 0 : 1;
	return cached != 0;
}

int TextRenderer::baselineOffset(const Node &node, bool last)
{
	const int fontSize = node.style.font_size > 0 ? node.style.font_size : 16;
	const auto font = gea::framework::graphics::FontRegistry::rasterizedFamily(node.style.font_id, fontSize);
	if (!font.valid()) return node.layout.height + node.style.margin[2];
	const int lineAdvance = node.style.line_height > 0 ? node.style.line_height : font.lineHeight();
	const int halfLeading = node.style.line_height > 0 ? (lineAdvance - font.lineHeight()) / 2 : 0;
	int offset = boxInset(node.style, 0) + halfLeading + font.ascender();
	if (last) {
		std::string storage;
		const char *text = prepareText(node.text.c_str(), node.style.text_transform, node.style.white_space, storage);
		const int width = node.style.white_space == 1 || node.style.white_space == 2
		    ? 32767 : std::max(1, node.layout.width - boxInsets(node.style, true));
		const auto measured = TextMetrics::measure(text, width, node.style.font_id, fontSize, node.style.line_height, node.style.font_weight);
		offset += std::max(0, measured.height - lineAdvance);
	}
	return offset;
}

InlineFlowMeasure TextRenderer::measureInlineFlow(const Node &node, int firstAvail, int contentWidth, bool atLineStart)
{
	InlineFlowMeasure out;
	const char *raw = node.text.c_str();
	if (!raw || !raw[0]) return out;
	// nowrap is a single line by definition, and an author-sized box wraps within
	// its own width — neither participates in the block's line boxes.
	if (node.style.white_space == 1 || node.style.white_space == 2) return out;
	if (!canFragmentInlineRuns(node)) return out;

	const GlyphAdvanceSource advance = advanceSourceForNode(node);
	if (!advance.valid || advance.lineAdvance <= 0) return out;

	std::string transformed;
	const char *text = textWithTransform(raw, node.style.text_transform, transformed, node.style.white_space);
	if (!text || !text[0]) return out;

	if (contentWidth < 1) contentWidth = 1;
	if (firstAvail < 0) firstAvail = 0;

	if (atLineStart && (node.style.white_space == 0 || node.style.white_space == 4)) {
		const int lead = leadingCollapsibleBytes(text);
		if (lead > 0) {
			const char *p = text;
			const char *end = text + lead;
			int width = 0;
			while (p < end) width += advance(nextUtf8Codepoint(p));
			out.firstLineIndentAdjust = -width;
		}
	}

	const char *p = text;
	int lineIndex = 0;
	while (*p) {
		const int budget = lineIndex == 0 ? (firstAvail - out.firstLineIndentAdjust) : contentWidth;
		const WrappedLine line = nextWrappedLine(p, budget < 1 ? 1 : budget, advance);
		if (line.consumedBytes <= 0) break;
		const int visible = line.width + (lineIndex == 0 ? out.firstLineIndentAdjust : 0);
		if (lineIndex == 0) {
			out.firstLineWidth = visible;
			if (node.style.white_space == 0 || node.style.white_space == 4) {
				const char *end = p + line.renderBytes;
				for (const char *glyph = p; glyph < end;) {
					const int cp = nextUtf8Codepoint(glyph);
					out.firstLineTrailingSpace = isWrappingSpace(cp)
					    ? out.firstLineTrailingSpace + advance(cp) : 0;
				}
			}
		}
		out.lastLineWidth = visible;
		if (visible > out.maxLineWidth) out.maxLineWidth = visible;
		p += line.consumedBytes;
		lineIndex++;
	}
	out.lineCount = lineIndex;
	out.lineAdvance = advance.lineAdvance;
	return out;
}

int TextRenderer::firstUnbreakableWidth(const Node &node)
{
	std::string storage;
	const char *text = prepareText(node.text.c_str(), node.style.text_transform, node.style.white_space, storage);
	if (!text) return 0;
	if (node.style.white_space == 0 || node.style.white_space == 1 || node.style.white_space == 4)
		text += leadingCollapsibleBytes(text);
	const bool wrapping = node.style.white_space != 1 && node.style.white_space != 2;
	const char *end = text;
	while (*end) {
		const char *start = end;
		const int cp = nextUtf8Codepoint(end);
		if (cp == '\n' || (wrapping && isWrappingSpace(cp))) { end = start; break; }
		if (wrapping && canBreakAfter(cp)) break;
	}
	return measureSingleLineTextWidthForNode(node, std::string(text, end - text).c_str());
}

int TextRenderer::minContentWidth(const Node &node, int *pendingWord)
{
	std::string storage;
	const char *text = prepareText(node.text.c_str(), node.style.text_transform, node.style.white_space, storage);
	const bool wrapping = node.style.white_space != 1 && node.style.white_space != 2;
	const char *start = text;
	int width = 0, current = pendingWord ? *pendingWord : 0;
	for (const char *p = text;; ++p) {
		if (!*p || *p == '\n' || (wrapping && (*p == ' ' || *p == '\t'))) {
			const std::string word(start, p - start);
			current += measureSingleLineTextWidthForNode(node, word.c_str());
			width = std::max(width, current);
			if (!*p) { if (pendingWord) *pendingWord = current; break; }
			current = 0;
			start = p + 1;
		}
	}
	return width;
}

const char *TextRenderer::prepareText(const char *text, int textTransform, int whiteSpace, std::string &storage)
{
	return textWithTransform(text, textTransform, storage, whiteSpace);
}

void TextRenderer::layout(int id, int avail_w)
{
	Node *n = &Tree::instance().nodes()[id];

	int content_w = avail_w - boxInset(n->style, 1) - boxInset(n->style, 3)
	              - n->style.margin[1] - n->style.margin[3];
	if (n->style.width != kUnset) content_w = contentSizeToBorderSize(n->style, n->style.width, true) - boxInset(n->style, 1) - boxInset(n->style, 3);
	else if ((n->style.width_percent != kUnset || n->style.width_expression >= 0) && n->layout.width > 0) content_w = n->layout.width - boxInset(n->style, 1) - boxInset(n->style, 3);
	if (content_w < 0) content_w = 0;

	const TextLayoutMeasure measured = measureTextLayoutForNode(*n, n->text.c_str(), content_w);
	n->layout.width = measured.width;
	n->layout.height = measured.height;
}

bool TextRenderer::remeasureContentBox(int id, bool keepBoxWidth)
{
	Node &n = Tree::instance().nodes()[id];
	if (n.type != NodeType::Text) return false;
	// A run that continues a line box (inline_indent != 0) has a box its SIBLINGS
	// determine — its first line is short by the pen it inherited, and where it
	// ends decides where the next box starts. There is no re-measuring that in
	// isolation, so the scroll fast path leaves it alone; text_layout_stable stays
	// 0, so the settle frame's full layout reflows the line properly.
	if (n.layout.inline_indent != 0) return false;
	// keepBoxWidth: the box was cross-stretched by the layout engine (a block
	// span in a column flow fills the parent's content width), so its geometry
	// is independent of the string — re-wrap the new text within it and update
	// only the height. Otherwise same content-width sourcing as layout(): an
	// explicit width re-wraps within the fixed box; natural-width text hugs the
	// new content (the 0x4000 bound matches setText's bbox predictor).
	int content_w = 0x4000;
	if (keepBoxWidth) content_w = n.layout.width - boxInsets(n.style, true);
	else if (n.style.width != kUnset) content_w = contentSizeToBorderSize(n.style, n.style.width, true) - boxInset(n.style, 1) - boxInset(n.style, 3);
	else if ((n.style.width_percent != kUnset || n.style.width_expression >= 0) && n.layout.width > 0) content_w = n.layout.width - boxInset(n.style, 1) - boxInset(n.style, 3);
	if (content_w < 0) content_w = 0;
	const TextLayoutMeasure measured = measureTextLayoutForNode(n, n.text.c_str(), content_w);
	if (keepBoxWidth) {
		if (measured.height == n.layout.height) return false;
		n.layout.height = measured.height;
		return true;
	}
	if (measured.width == n.layout.width && measured.height == n.layout.height) return false;
	n.layout.width = measured.width;
	n.layout.height = measured.height;
	return true;
}

void TextRenderer::drawWrapped(const char *text, int x, int y, int maxWidth, gea::framework::graphics::pixel::native_t color, float scale, int text_align, int containerWidth, int fontId, int textTransform, int lineHeight, int whiteSpace, int textOverflow, int maxHeight, int firstLineIndent)
{
	TextDrawer::drawWrapped(text, x, y, maxWidth, color, scale, text_align, containerWidth, fontId, textTransform, lineHeight, whiteSpace, textOverflow, maxHeight, firstLineIndent);
}

void TextRenderer::unionCoverageRow(const DisplayCommand &command, int screenY, int screenX,
                                    int width, std::uint8_t *outCoverage)
{
	TextDrawer::unionCoverageRow(command, screenY, screenX, width, outCoverage);
}

int TextRenderer::measureWidth(const char *text, int fontId, int fontSize, int textTransform)
{
	if (!text || !text[0]) return 0;
	// 32767 = no horizontal wrapping; we want the full single-line width
	// that the chosen font would render. Returning that as int16-safe.
	std::string transformed;
	const char *measureText = textWithTransform(text, textTransform, transformed);
	const TextMeasure measured = TextMetrics::measure(measureText, 32767, fontId, fontSize);
	return measured.width;
}

int TextRenderer::measureHeight(const char *text, int fontId, int fontSize, int textTransform, int lineHeight)
{
	if (!text || !text[0]) return 0;
	std::string transformed;
	const char *measureText = textWithTransform(text, textTransform, transformed);
	const TextMeasure measured = TextMetrics::measure(measureText, 32767, fontId, fontSize, lineHeight);
	return measured.height;
}

void GEA_TEXT_HOT_SRAM TextRenderer::record(const Node &node, uint8_t parentAlpha)
{
	const Node *n = &node;
	if (n->type != NodeType::Text || n->text.empty()) return;

	// A first-line background belongs to the inline fragment, not the block's
	// entire width. Layout records this run's first line box/advance only when an
	// ancestor has a resolved ::first-line background. Emit it before the text
	// command so the glyphs remain painted above the inline background.
	const int textNodeId = static_cast<int>(n - Tree::instance().nodes());
	const NodeRareData *textRare = rareDataFor(textNodeId);
	const NodeRareData *lineStyleRare = nullptr;
	for (int ancestor = n->parent; ancestor >= 0 && ancestor < Tree::instance().nodeCount();
	     ancestor = Tree::instance().node(ancestor).parent) {
		const NodeRareData *candidate = rareDataFor(ancestor);
		if (candidate && candidate->firstLineBackground.hasColor) {
			lineStyleRare = candidate;
			break;
		}
	}
	if (textRare && textRare->firstLineFragment.valid && lineStyleRare &&
	    lineStyleRare->firstLineBackground.lineValid &&
	    textRare->firstLineFragment.width > 0 && textRare->firstLineFragment.height > 0 &&
	    lineStyleRare->firstLineBackground.alpha > 0) {
		const auto &fragment = textRare->firstLineFragment;
		const auto &background = lineStyleRare->firstLineBackground;
		const uint8_t backgroundAlpha = combineAlpha(parentAlpha, background.alpha);
		int fragmentX = fragment.x;
		int fragmentY = fragment.y;
		const Tree &tree = Tree::instance();
		if (fragment.contextNode >= 0 && fragment.contextNode < tree.nodeCount()) {
			fragmentX += tree.node(fragment.contextNode).layout.x;
			fragmentY += tree.node(fragment.contextNode).layout.y;
		}
		int ownerId = -1;
		for (int ancestor = n->parent; ancestor >= 0 && ancestor < tree.nodeCount();
		     ancestor = tree.node(ancestor).parent) {
			const NodeRareData *candidate = rareDataFor(ancestor);
			if (candidate && &candidate->firstLineBackground == &lineStyleRare->firstLineBackground) {
				ownerId = ancestor;
				break;
			}
		}
		const bool onOwnerFirstLine = ownerId >= 0 && background.lineContextNode >= 0 &&
		    background.lineContextNode < tree.nodeCount() &&
		    fragmentY == tree.node(background.lineContextNode).layout.y + background.lineY;
		if (onOwnerFirstLine) {
		int16_t xs[4], ys[4];
		ViewRenderer::transformedRectCorners(*n, false, fragmentX, fragmentY,
		                                     fragment.width, fragment.height, xs, ys);
		int bx0, by0, bx1, by1;
		boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
		if (backgroundAlpha != parentAlpha)
			appendAlphaCommand(backgroundAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
		DisplayCommand *fill = DisplayList::instance().append();
		if (fill) {
			fill->type = DisplayCommandType::FillQuad;
			fill->bx = bx0; fill->by = by0;
			fill->bw = bx1 - bx0 + 1; fill->bh = by1 - by0 + 1;
			fill->quad.x0 = xs[0]; fill->quad.y0 = ys[0];
			fill->quad.x1 = xs[1]; fill->quad.y1 = ys[1];
			fill->quad.x2 = xs[2]; fill->quad.y2 = ys[2];
			fill->quad.x3 = xs[3]; fill->quad.y3 = ys[3];
			fill->quad.lx = static_cast<int16_t>(fragmentX); fill->quad.ly = static_cast<int16_t>(fragmentY);
			fill->quad.lw = fragment.width; fill->quad.lh = fragment.height;
			fill->quad.aux = 0;
			fill->quad.color = background.color;
			fill->quad.reprojectMode = 1;
		}
		if (backgroundAlpha != parentAlpha)
			appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
		}
	}


	int x = n->layout.x;
	int y = n->layout.y;
	int w = n->layout.width;
	int h = n->layout.height;
	int tx = x + boxInset(n->style, 3);
	int ty = y + boxInset(n->style, 0);
	int tw = w - boxInset(n->style, 1) - boxInset(n->style, 3);
	if (tw < 0) tw = 0;
	float textScale = n->style.font_size > 0 ? static_cast<float>(n->style.font_size) / kBitmapFontHeight : 1.0f;

	int bx = x;
	int by = y;
	int bw = w;
	int bh = h;
	int drawX = tx;
	int drawY = ty;
	int drawW = tw;
	int containerW = tw;
	int commandLineHeight = n->style.line_height;
	int transformedX0 = x;
	int transformedY0 = y;
	int transformedX1 = x + w - 1;
	int transformedY1 = y + h - 1;
	ViewRenderer::transformedBounds(*n, false, &transformedX0, &transformedY0, &transformedX1, &transformedY1);
	bool transformed =
	    transformedX0 != x ||
	    transformedY0 != y ||
	    transformedX1 != x + w - 1 ||
	    transformedY1 != y + h - 1;
	if (transformed) {
		float translateX = 0.0f;
		float translateY = 0.0f;
		if (currentTransformChainIsPureTranslate(*n, &translateX, &translateY)) {
			x = translateCoord(x, translateX);
			y = translateCoord(y, translateY);
			tx = translateCoord(tx, translateX);
			ty = translateCoord(ty, translateY);
			bx = x;
			by = y;
			drawX = tx;
			drawY = ty;
			transformed = false;
		}
	}
	std::string transformedText;
	const char *measureText = textWithTransform(n->text.c_str(), n->style.text_transform, transformedText, n->style.white_space);
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	if (transformed && recordProjectedRasterText(*n, measureText, parentAlpha, tx, ty, tw)) return;
#endif
	if (transformed && transformedX1 >= transformedX0 && transformedY1 >= transformedY0) {
		const int projectedW = transformedX1 - transformedX0 + 1;
		const int projectedH = transformedY1 - transformedY0 + 1;
		const int contentH = h - boxInset(n->style, 0) - boxInset(n->style, 2);
		const int centerX = transformedX0 + projectedW / 2;
		const int centerY = transformedY0 + projectedH / 2;
		float projectedScaleX = tw > 0 ? static_cast<float>(projectedW) / static_cast<float>(tw) : 1.0f;
		float projectedScaleY = 1.0f;
		if (contentH > 0) {
			projectedScaleY = static_cast<float>(projectedH) / static_cast<float>(contentH);
		}
		float projectedScale = projectedScaleX < projectedScaleY ? projectedScaleX : projectedScaleY;
		projectedScale = clampFloat(projectedScale, 0.35f, 1.35f);
		textScale *= projectedScale;
		const int projectedFontSize = static_cast<int>(textScale * kBitmapFontHeight + 0.5f);
		const int projectedLineHeight = n->style.line_height > 0
		                                  ? static_cast<int>(static_cast<float>(n->style.line_height) * projectedScale + 0.5f)
		                                  : 0;
		commandLineHeight = projectedLineHeight;
		const TextMeasure projectedMeasure =
		    TextMetrics::measure(measureText, 32767, n->style.font_id, projectedFontSize, projectedLineHeight, n->style.font_weight);
		drawW = projectedMeasure.width > 0 ? projectedMeasure.width : projectedW;
		const int drawH = projectedMeasure.height > 0 ? projectedMeasure.height : (contentH > 0 ? contentH : projectedH);
		containerW = drawW;
		drawX = centerX - drawW / 2;
		drawY = centerY - drawH / 2;
		if (drawY < transformedY0) drawY = transformedY0;
		const int drawX1 = drawX + drawW - 1;
		const int drawY1 = drawY + drawH - 1;
		const int pad = 2;
		bx = drawX - pad;
		by = drawY - pad;
		bw = drawX1 - drawX + 1 + pad * 2;
		bh = drawY1 - drawY + 1 + pad * 2;
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
		if (n->style.font_id >= 0) {
			const int commandFontSize = static_cast<int>(textScale * kBitmapFontHeight + 0.5f);
			int ix0 = 0;
			int iy0 = 0;
			int ix1 = -1;
			int iy1 = -1;
			if (rasterizedTextInkBounds(measureText,
			                            drawW,
			                            n->style.text_align,
			                            containerW,
			                            n->style.font_id,
			                            commandFontSize,
			                            commandLineHeight,
			                            &ix0,
			                            &iy0,
			                            &ix1,
			                            &iy1)) {
				const int inkPad = 2;
				bx = drawX + ix0 - inkPad;
				by = drawY + iy0 - inkPad;
				bw = ix1 - ix0 + 1 + inkPad * 2;
				bh = iy1 - iy0 + 1 + inkPad * 2;
			}
		}
#endif
	}

	const bool noWrap = n->style.white_space == 1;
	const bool noSoftWrap = noWrap || n->style.white_space == 2;
	// Inline continuation (see LayoutBox::inline_indent): line 0 starts at
	// `drawX + inlineIndent`, every later line at `drawX`. The tight ink/paint
	// bounds below all assume one origin for the whole run, so an indented run
	// keeps the full box as its command bound — conservative, and the only cost
	// is a slightly larger dirty rect for the handful of runs that continue a
	// line box. A negative indent (the leading-whitespace trim) reaches left of
	// the box, so widen for it.
	const int inlineIndent = transformed ? 0 : n->layout.inline_indent;
	if (inlineIndent < 0) {
		bx += inlineIndent;
		bw -= inlineIndent;
	}
	if (!transformed && inlineIndent == 0) {
		const int commandFontSize = static_cast<int>(textScale * kBitmapFontHeight + 0.5f);
		const TextMeasure paintMeasure =
		    TextMetrics::measure(measureText, noSoftWrap ? 32767 : drawW, n->style.font_id, commandFontSize, commandLineHeight, n->style.font_weight);
		if (paintMeasure.width > 0 && paintMeasure.height > 0) {
			// CSS: a flex container's bare text becomes an anonymous flex item, so
			// align-items centers (or end-aligns) it on the cross axis. This node
			// draws its own run top-anchored otherwise, which made
			// `display:flex; align-items:center` on a childless span a no-op — apps
			// had to fake it with line-height. Row-direction flex only (the cross
			// axis is vertical there); a child-bearing flex container aligns its
			// child BOXES through layout as before.
			if (n->style.display == kDisplayFlex && n->first_child < 0 &&
			    usesRowLayout(n->style)) {
				const int contentH = h - boxInset(n->style, 0) - boxInset(n->style, 2);
				const int slack = contentH - paintMeasure.height;
				const int alignment = usedAlignment(n->style.align_items, slack, (n->style.flex_wrap & 3) == 2);
				if (alignment == 1) drawY += slack / 2;
				else if (alignment == 2) drawY += slack;
			}
			// nowrap truncates/clips the run to the content box, so the painted
			// width never exceeds drawW even though the unbounded measure may.
			int paintWidth = paintMeasure.width;
			if (noWrap && paintWidth > drawW) paintWidth = drawW;
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
			if (n->style.font_id >= 0) {
				gea::framework::graphics::RasterizedFont font =
				    gea::framework::graphics::FontRegistry::rasterizedFamily(n->style.font_id, commandFontSize);
				const int lineAdvance = commandLineHeight > 0 ? commandLineHeight : (font.valid() ? font.lineHeight() : 0);
				const bool singleLine = font.valid() &&
				                        std::strchr(measureText, '\n') == nullptr &&
				                        paintMeasure.height <= lineAdvance;
				// Ink centring moves the whole line box so the font's ink sits centred
				// in the line advance. That is right for a run that owns its line and
				// wrong for one that shares it, where the inline flow has already
				// placed a baseline every box on the line must draw at. See
				// RenderState::inline_baseline.
				if (singleLine && !n->render.inline_baseline) {
					drawY += rasterizedSingleLineInkCenterOffsetY(measureText,
					                                              n->style.text_align,
					                                              containerW,
					                                              n->style.font_id,
					                                              commandFontSize,
					                                              commandLineHeight);
				}
			}
#endif
			const int xOffset = alignedOffset(n->style.text_align, containerW, paintWidth);
			int paintX0 = drawX + xOffset;
			int paintY0 = drawY;
			int paintX1 = paintX0 + paintWidth - 1;
			int paintY1 = paintY0 + paintMeasure.height - 1;
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
			if (n->style.font_id >= 0) {
				int ix0 = 0;
				int iy0 = 0;
				int ix1 = -1;
				int iy1 = -1;
				if (rasterizedTextInkBounds(measureText,
				                            noSoftWrap ? 32767 : drawW,
				                            n->style.text_align,
				                            containerW,
				                            n->style.font_id,
				                            commandFontSize,
				                            commandLineHeight,
				                            &ix0,
				                            &iy0,
				                            &ix1,
				                            &iy1)) {
					// A nowrap run may be clipped/ellipsized horizontally, so its
					// unbounded ink X extent is not a valid command bound. Its Y ink
					// extent is still exact, however, and is essential when CSS
					// line-height is shorter than the raster font's line height. The
					// previous line-box bound missed the bottom 4-5 rows of large
					// compressed text (weather's 20 -> 17 temperature left residue).
					if (!noWrap) paintX0 = drawX + ix0;
					paintY0 = drawY + iy0;
					if (!noWrap) paintX1 = drawX + ix1;
					paintY1 = drawY + iy1;
				}
			}
#endif
			const int pad = 2;
			bx = paintX0 - pad;
			by = paintY0 - pad;
			bw = paintX1 - paintX0 + 1 + pad * 2;
			bh = paintY1 - paintY0 + 1 + pad * 2;
		}
	}

	// TEMPORARY BASELINE INSTRUMENTATION (env-gated) — remove before landing.
	if (std::getenv("GEA_TEXT_BASELINE_DEBUG") && n->style.font_id >= 0) {
		const int dbgFontSize = static_cast<int>(textScale * kBitmapFontHeight + 0.5f);
		gea::framework::graphics::RasterizedFont dbgFont =
		    gea::framework::graphics::FontRegistry::rasterizedFamily(n->style.font_id, dbgFontSize);
		const int fontLH = dbgFont.valid() ? dbgFont.lineHeight() : -1;
		const int asc = dbgFont.valid() ? dbgFont.ascender() : -1;
		const int lineAdv = commandLineHeight > 0 ? commandLineHeight : fontLH;
		const int lineBoxOff = commandLineHeight > 0 ? (lineAdv - fontLH) / 2 : 0;
		int inkOff = 0;
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
		const int genFonts = 1;
		inkOff = rasterizedSingleLineInkCenterOffsetY(measureText, n->style.text_align, containerW,
		                                             n->style.font_id, dbgFontSize, commandLineHeight);
#else
		const int genFonts = 0;
#endif
		char preview[29];
		int pi = 0;
		for (const char *p = n->text.c_str(); *p && pi < 28; ++p)
			preview[pi++] = (*p == '\n') ? ' ' : *p;
		preview[pi] = '\0';
		std::printf("[baseline] gen=%d '%s' font=%d size=%d asc=%d fontLH=%d cssLH=%d "
		            "box=(%d,%d,%dx%d) ty=%d drawY=%d inkOff=%d lineBoxOff=%d baselineY=%d\n",
		            genFonts, preview, n->style.font_id, dbgFontSize, asc, fontLH,
		            n->style.line_height, n->layout.x, n->layout.y, n->layout.width,
		            n->layout.height, ty, drawY, inkOff, lineBoxOff, drawY + lineBoxOff + asc);
		std::fflush(stdout);
	}


	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, n->style.text_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx, by, bw, bh);

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) {
		if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx, by, bw, bh);
		return;
	}
	cmd->type = DisplayCommandType::DrawText;
	cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
	cmd->text.text = n->text.c_str();
	cmd->text.x = drawX; cmd->text.y = drawY;
	cmd->text.maxWidth = drawW;
	cmd->text.color = n->style.text_color;
	cmd->text.scale = textScale;
	cmd->text.align = n->style.text_align;
	cmd->text.textTransform = n->style.text_transform;
	cmd->text.lineHeight = commandLineHeight;
	cmd->text.containerWidth = containerW;
	cmd->text.fontId = n->style.font_id;
	cmd->text.whiteSpace = n->style.white_space;
	cmd->text.textOverflow = n->style.text_overflow;
	cmd->text.firstLineIndent = static_cast<int16_t>(inlineIndent);
	{
		const int contentH = h - boxInset(n->style, 0) - boxInset(n->style, 2);
		cmd->text.maxHeight = static_cast<int16_t>(contentH > 0 ? (contentH > 32767 ? 32767 : contentH) : 0);
	}

	// Text decoration — paint a horizontal line on top of the glyph row.
	// We approximate via font size (no font-metric introspection available
	// cross-platform): line-through sits a little below the em-box midpoint,
	// underline sits near the baseline, and thickness scales with the text.
	// The width matches the measured text width so the line doesn't extend
	// past the actual glyphs.
	if (n->style.text_decoration != 0 && !n->text.empty()) {
		const int fontSize = n->style.font_size > 0 ? n->style.font_size : kBitmapFontHeight;
		const int lineWidth = TextMetrics::measure(measureText, tw, n->style.font_id, n->style.font_size, 0, n->style.font_weight).width;
		const int lineHeight = textDecorationThickness(fontSize);
		const int lineY = textDecorationY(ty, fontSize, lineHeight, n->style.text_decoration);
		DisplayCommand *deco = DisplayList::instance().append();
		if (deco) {
			deco->type = DisplayCommandType::FillRect;
			deco->textDecorationInk = true;
			deco->bx = x; deco->by = y; deco->bw = w; deco->bh = h;
			deco->fill.x = tx;
			deco->fill.y = lineY;
			deco->fill.w = lineWidth;
			deco->fill.h = lineHeight;
			deco->fill.color = n->style.text_color;
		}
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx, by, bw, bh);
}

}  // namespace gea::embedded::ui
