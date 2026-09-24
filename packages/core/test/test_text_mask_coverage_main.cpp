#include "native_test_harness.h"

#include "ui/internal.h"
#include "display.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }

using namespace gea::embedded::ui;

namespace {

int fail(const char *message)
{
	std::fprintf(stderr, "[text_mask_coverage] FAIL: %s\n", message);
	return 1;
}

bool matchesDrawnPixels(const DisplayCommand &command, const char *label)
{
	using namespace gea::embedded::test;
	constexpr std::uint16_t ink = 0xffff;
	gea::platform::display::Display::resetClip();
	gea::platform::display::Display::clearNoFlush();
	TextRenderer::drawWrapped(command.text.text, command.text.x, command.text.y,
	                          command.text.maxWidth, ink, command.text.scale,
	                          command.text.align, command.text.containerWidth,
	                          command.text.fontId, command.text.textTransform,
	                          command.text.lineHeight, command.text.whiteSpace,
	                          command.text.textOverflow, command.text.maxHeight,
	                          command.text.firstLineIndent);
	std::array<std::uint8_t, 64> row{};
	for (int y = 0; y < 64; ++y) {
		row.fill(0);
		TextRenderer::unionCoverageRow(command, y, 0, 64, row.data());
		for (int x = 0; x < 64; ++x) {
			const bool maskInk = row[static_cast<std::size_t>(x)] != 0;
			const bool drawnInk = displayPixelAt(x, y) == ink;
			if (maskInk != drawnInk) {
				std::fprintf(stderr, "[text_mask_coverage] %s parity mismatch at (%d,%d): mask=%u pixel=%04x\n",
				             label, x, y, row[static_cast<std::size_t>(x)], displayPixelAt(x, y));
				return false;
			}
		}
	}
	return true;
}

bool rowHasCoverage(const DisplayCommand &command, int y, int x, int width,
                    std::array<std::uint8_t, 64> &row)
{
	row.fill(0);
	TextRenderer::unionCoverageRow(command, y, x, width, row.data());
	for (int i = 0; i < width; ++i) if (row[static_cast<std::size_t>(i)] != 0) return true;
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	setNativeDisplaySize(64, 64);

	DisplayCommand text{};
	text.type = DisplayCommandType::DrawText;
	text.text.text = "AB CD";
	text.text.x = 0;
	text.text.y = 0;
	text.text.maxWidth = 24;
	text.text.color = 0; // Coverage must not depend on foreground color.
	text.text.scale = 1.0f;
	text.text.align = 0;
	text.text.containerWidth = 24;
	text.text.fontId = -1; // Exercise the bitmap fallback deterministically.
	text.text.lineHeight = 16;
	text.text.whiteSpace = 0;
	text.text.textOverflow = 0;
	text.text.maxHeight = 48;
	text.text.firstLineIndent = 8;

	std::array<std::uint8_t, 64> row{};
	bool firstLine = false;
	for (int y = 0; y < 16; ++y) firstLine |= rowHasCoverage(text, y, 0, 48, row);
	if (!firstLine) return fail("wrapped first line has no glyph coverage");
	bool continuation = false;
	for (int y = 16; y < 32; ++y) continuation |= rowHasCoverage(text, y, 0, 48, row);
	if (!continuation) return fail("wrapped continuation line has no glyph coverage");
	if (rowHasCoverage(text, 40, 0, 48, row)) return fail("coverage escaped the rendered line range");

	// The first line inherits an 8px inline cursor; subsequent lines restart at
	// the text box origin. Find the first ink pixel on each line rather than
	// depending on a font-specific glyph outline.
	int firstInk = 64;
	for (int y = 0; y < 16; ++y) {
		rowHasCoverage(text, y, 0, 48, row);
		for (int x = 0; x < 48; ++x) if (row[static_cast<std::size_t>(x)]) { firstInk = x; break; }
		if (firstInk < 64) break;
	}
	if (firstInk < 8) return fail("first-line indent was not applied");
	if (!matchesDrawnPixels(text, "wrapped inline continuation")) return 1;

	DisplayCommand clipped{};
	clipped.type = DisplayCommandType::DrawText;
	clipped.text.text = "AAAAA";
	clipped.text.x = 0; clipped.text.y = 0; clipped.text.maxWidth = 24;
	clipped.text.scale = 1.0f; clipped.text.containerWidth = 24; clipped.text.fontId = -1;
	clipped.text.whiteSpace = 1; clipped.text.textOverflow = 1; clipped.text.maxHeight = 16;
	bool ellipsisInk = false;
	for (int y = 0; y < 16; ++y) {
		row.fill(0);
		TextRenderer::unionCoverageRow(clipped, y, 0, 64, row.data());
		for (int i = 0; i < 24; ++i) ellipsisInk |= row[static_cast<std::size_t>(i)] != 0;
		for (int i = 24; i < 64; ++i) if (row[static_cast<std::size_t>(i)]) return fail("nowrap coverage escaped maxWidth");
	}
	if (!ellipsisInk) return fail("nowrap ellipsis produced no glyph coverage");
	if (!matchesDrawnPixels(clipped, "nowrap ellipsis")) return 1;

	DisplayCommand hardBreak{};
	hardBreak.type = DisplayCommandType::DrawText;
	hardBreak.text.text = "A\nB";
	hardBreak.text.x = 3; hardBreak.text.y = 4; hardBreak.text.maxWidth = 48;
	hardBreak.text.scale = 1.0f; hardBreak.text.containerWidth = 48; hardBreak.text.fontId = -1;
	hardBreak.text.lineHeight = 18; hardBreak.text.whiteSpace = 2;
	if (!matchesDrawnPixels(hardBreak, "preformatted hard break")) return 1;

	// The coverage sink is subject to the same active display clip as the draw
	// path, including a clip that cuts through a glyph.
	gea::platform::display::Display::clearNoFlush();
	gea::platform::display::Display::pushClip(7, 5, 11, 13);
	TextRenderer::drawWrapped(hardBreak.text.text, hardBreak.text.x, hardBreak.text.y,
	                          hardBreak.text.maxWidth, 0xffff, hardBreak.text.scale,
	                          hardBreak.text.align, hardBreak.text.containerWidth,
	                          hardBreak.text.fontId, hardBreak.text.textTransform,
	                          hardBreak.text.lineHeight, hardBreak.text.whiteSpace,
	                          hardBreak.text.textOverflow, hardBreak.text.maxHeight,
	                          hardBreak.text.firstLineIndent);
	for (int y = 0; y < 64; ++y) {
		row.fill(0);
		TextRenderer::unionCoverageRow(hardBreak, y, 0, 64, row.data());
		for (int x = 0; x < 64; ++x) {
			if ((row[static_cast<std::size_t>(x)] != 0) != (displayPixelAt(x, y) == 0xffff)) {
				gea::platform::display::Display::popClip();
				return fail("active clip changed mask/draw parity");
			}
		}
	}
	gea::platform::display::Display::popClip();

	DisplayCommand decoration{};
	decoration.type = DisplayCommandType::FillRect;
	decoration.textDecorationInk = true;
	decoration.fill.x = 4; decoration.fill.y = 9; decoration.fill.w = 5; decoration.fill.h = 2;
	if (!rowHasCoverage(decoration, 9, 0, 16, row)) return fail("marked text decoration was not included in coverage");
	for (int i = 0; i < 16; ++i) {
		const bool expected = i >= 4 && i < 9;
		if ((row[static_cast<std::size_t>(i)] != 0) != expected) return fail("decoration coverage did not match its fill rectangle");
	}
	decoration.textDecorationInk = false;
	if (rowHasCoverage(decoration, 9, 0, 16, row)) return fail("unmarked fill was incorrectly treated as text ink");

	std::puts("[text_mask_coverage] ALL PASS");
	return 0;
}
