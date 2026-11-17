// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

class ViewportRegion {
public:
	static int clipRect(int width, int height, int *x, int *y, int *w, int *h);
	static void replayDisplayRegion(int x, int y, int w, int h);
	// Same, rastered through the display's SRAM strips where it has them: for
	// the rare large repaints whose raw PSRAM writes would starve the scanout.
	static void replayDisplayRegionStaged(int x, int y, int w, int h);
};

}  // namespace gea::embedded::ui
