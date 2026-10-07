#include "graphics/font.h"
#include <cassert>
#include <cstdio>

using namespace gea::framework::graphics;

int main()
{
	const std::uint8_t atlas[] = {0, 85, 170, 255};
	const Glyph glyphs[] = {
		{65, 0, 0, 2, 2, 2, 0, 2, 24},
		{86, 0, 0, 2, 2, 2, 0, 2, 24},
		{65533, 0, 0, 2, 2, 3, 0, 2, 48},
	};
	const FontKerningPair pairs[] = {{65, 86, -4}};
	const RasterizedFontData data{0, 10, 10, 8, 2, 3, glyphs, 2,
								  2, atlas, 8, 1, pairs, true, 65533};
	const RasterizedFont font(&data);
	assert(font.advance('A') == 2);
	assert(font.advance('A', 'V') == 1);
	assert(font.advance('V', 'A') == 2);
	assert(font.advance(0x1f600) == 3);
	Glyph value{};
	assert(font.glyph('A', &value));
	assert(font.coverage(value, 0, 0) == 0);
	assert(font.coverage(value, 0, 1) == 85);
	assert(font.coverage(value, 1, 0) == 170);
	assert(font.coverage(value, 1, 1) == 255);
	assert(font.glyph(0x1f600, &value) && value.codepoint == 65533);
	const Glyph legacyGlyph[] = {{65, 0, 0, 2, 2, 9, 0, 2}};
	const RasterizedFontData legacyData{0, 10, 10, 8, 2,
										1, legacyGlyph, 2, 2, atlas};
	const RasterizedFont legacy(&legacyData);
	assert(legacy.advance('A', 'V') == 9);
	assert(legacy.advance('!') == 9); // Existing first-glyph fallback remains.
	std::puts("bitmap font fixed16 kerning, exact coverage, placeholder and "
			  "legacy tests passed");
}
