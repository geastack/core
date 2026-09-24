// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pixel.h"

#include <cstdint>

namespace gea::embedded::ui {

struct ComputedStyle;
struct BackgroundPlacement {
	int x, y, width, height;
	int repeatX = 0, repeatY = 0; // repeat, no-repeat, round, space
	int attachment = 0; // scroll, fixed, local
	int origin = 1; // border, padding, content
	int areaX = 0, areaY = 0, areaWidth = 0, areaHeight = 0;
};

class StyleValues {
public:
	// Image handles belong to the stylesheet pool; -1 is CSS none.
	static bool applyBackgroundImage(ComputedStyle &style, int handle, int nodeId);
	// 0: border-box, 1: padding-box, 2: content-box. Lists repeat by image layer.
	static int backgroundClip(const ComputedStyle &style, int layer);
	static bool hasTextBackgroundClip(const ComputedStyle &style);
	static BackgroundPlacement backgroundPlacement(const ComputedStyle &style, int nodeId, int layer,
	                                              int x, int y, int width, int height);
	// Convert a raw style-value colour int into this board's native pixel, applied
	// once when the colour is written into node.style. The style-value int holds
	// the authoring colour in the board's pre-panel form: raw (unswapped) RGB565 on
	// 16-bit panels — swapped to panel byte order here — or the target's 8888 layout on full-colour
	// boards (no panel concept, identity). After this, node.style colours are native
	// and nothing downstream converts.
	static gea::framework::graphics::pixel::native_t pixelFromStyleValue(int value)
	{
#if GEA_PIXEL_FORMAT_IS_8888 || GEA_PIXEL_FORMAT_IS_GRAY
		// No panel byte-swap: the style value already IS the final native pixel
		// (RGBA8888, or a gray level — 0..15 GRAY4 / 0..3 GRAY2 — from
		// nativeStyleValue). Identity.
		return static_cast<gea::framework::graphics::pixel::native_t>(static_cast<std::uint32_t>(value));
#else
		return gea::framework::graphics::pixel::fromRgb565((uint16_t)value);
#endif
	}
};

}  // namespace gea::embedded::ui
