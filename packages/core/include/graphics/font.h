#pragma once

#include <cstdint>

namespace gea::framework::graphics {

struct Glyph {
	int codepoint = 0;
	int sourceX = 0;
	int sourceY = 0;
	int width = 0;
	int height = 0;
	int advance = 0;
	int bearingX = 0;
	int bearingY = 0;
	int advance16 = -1;
};

class Font {
public:
	virtual ~Font() = default;

	virtual bool valid() const = 0;
	virtual int sizePx() const = 0;
	virtual int lineHeight() const = 0;
	virtual int ascender() const = 0;
	virtual bool glyph(int codepoint, Glyph *out) const = 0;
	virtual std::uint8_t coverage(const Glyph &glyph, int row, int col) const = 0;
	virtual int advance(int codepoint, int nextCodepoint = 0) const
	{
		(void)nextCodepoint;
		Glyph value{};
		return glyph(codepoint, &value) ? value.advance : sizePx() / 2;
	}
};

class BitmapFont : public Font {
public:
	static constexpr int kFirstCodepoint = 0x20;
	static constexpr int kLastCodepoint = 0x7e;
	static constexpr int kGlyphCount = kLastCodepoint - kFirstCodepoint + 1;
	static constexpr int kWidth = 8;
	static constexpr int kHeight = 16;

	bool valid() const final;
	int sizePx() const final;
	int lineHeight() const final;
	int ascender() const final;
	bool glyph(int codepoint, Glyph *out) const final;
	std::uint8_t coverage(const Glyph &glyph, int row, int col) const final;

	// Takes a CODEPOINT, not a byte. Typed `char` this silently truncated every
	// non-ASCII glyph: a caller decoding UTF-8 properly still narrowed U+2014 to
	// one byte, and a caller not decoding at all handed over three bytes in turn
	// and got three substitution glyphs. `rowsForCodepoint` already substitutes
	// out-of-range codepoints, which is the whole of the fallback policy.
	const std::uint8_t *glyphRows(int codepoint) const;

protected:
	virtual const std::uint8_t *rowsForCodepoint(int codepoint) const = 0;
};

class BitmapFont8x16 final : public BitmapFont {
public:
	static const BitmapFont8x16 &instance();

private:
	const std::uint8_t *rowsForCodepoint(int codepoint) const final;

	static const std::uint8_t glyphs[kGlyphCount][kHeight];
};

struct FontKerningPair
{
	int left = 0;
	int right = 0;
	int adjustment16 = 0;
};

struct RasterizedFontData {
	int id = 0;
	int sizePx = 0;
	int lineHeight = 0;
	int ascender = 0;
	int descender = 0;
	int glyphCount = 0;
	const Glyph *glyphs = nullptr;
	int atlasWidth = 0;
	int atlasHeight = 0;
	const std::uint8_t *atlas = nullptr;
	// Bits per atlas pixel. 8 = one coverage byte per pixel (the default, and
	// what runtime-rasterized fonts always produce). 2 = four pixels per byte,
	// baked offline for panels that can only show four gray levels; rows are
	// byte-aligned (stride = (atlasWidth + 3) / 4) and the leftmost pixel of a
	// byte sits in the most significant bit pair.
	int atlasBits = 8;
	int kerningCount = 0;
	const FontKerningPair *kerning = nullptr;
	bool strictGlyphLookup = false;
	int fallbackCodepoint = -1;
};

class RasterizedFont final : public Font {
public:
	RasterizedFont() = default;
	explicit RasterizedFont(const RasterizedFontData *data);

	bool valid() const final;
	int sizePx() const final;
	int lineHeight() const final;
	int ascender() const final;
	bool glyph(int codepoint, Glyph *out) const final;
	std::uint8_t coverage(const Glyph &glyph, int row, int col) const final;
	int advance(int codepoint, int nextCodepoint = 0) const final;
	const RasterizedFontData *data() const { return data_; }

private:
	const RasterizedFontData *data_ = nullptr;
};

class FontRegistry {
public:
	static RasterizedFont rasterized(int fontId);
	static RasterizedFont rasterizedFamily(int familyId, int sizePx);
	static const BitmapFont8x16 &bitmap8x16();
	static int familyId(const char *family);
	static const char *familyName(int familyId);
};

namespace generated {

const RasterizedFontData *lookupFont(int fontId);
const RasterizedFontData *lookupFontForFamily(int familyId, int sizePx);
int lookupFontFamily(const char *family);
const char *lookupFontFamilyName(int familyId);
const std::uint8_t *lookupRuntimeTtfFontForFamily(int familyId, unsigned long *length);
void ensureLinked();

}  // namespace generated

}  // namespace gea::framework::graphics
