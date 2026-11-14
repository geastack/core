// SPDX-License-Identifier: Apache-2.0
#include "viewport_region.h"

#include "display.h"
#include "internal.h"

#include <algorithm>

// SRAM strip staging (render.cpp defines weak no-op fallbacks; a display that
// stages overrides them).
extern "C" int gea_bulk_replay_stage_rows_for_width(int width);
extern "C" bool gea_replay_stage_begin(int x0, int y0, int x1, int y1);
extern "C" void gea_replay_stage_end();
extern "C" bool gea_replay_stage_active();
extern "C" void gea_replay_stage_skip_seed(bool skip);

namespace gea::embedded::ui {

int ViewportRegion::clipRect(int width, int height, int *x, int *y, int *w, int *h)
{
	if (*w <= 0 || *h <= 0 || width <= 0 || height <= 0) return 0;
	int x0 = *x;
	int y0 = *y;
	int x1 = *x + *w - 1;
	int y1 = *y + *h - 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width) x1 = width - 1;
	if (y1 >= height) y1 = height - 1;
	if (x0 > x1 || y0 > y1) return 0;
	*x = x0;
	*y = y0;
	*w = x1 - x0 + 1;
	*h = y1 - y0 + 1;
	return 1;
}

void ViewportRegion::replayDisplayRegionStaged(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	// A whole-list replay clipped to the region rasters straight into the back
	// buffer. On a panel that scans out of PSRAM, those read-modify-writes
	// compete with the scanout refill for the bus and starve it (a visible tear).
	// Raster the region in SRAM strips instead and copy each back once.
	const int stageRows = gea_bulk_replay_stage_rows_for_width(w);
	if (stageRows > 0 && !gea_replay_stage_active()) {
		gea::platform::display::Display::pushClip(x, y, w, h);
		const int cullStart = DisplayList::instance().replayCullStart();
		gea::platform::display::Display::popClip();
		DisplayList::instance().setReplayCullStart(cullStart);
		// An opaque fill covers the region: each strip is painted whole, so it
		// need not be seeded from the back buffer.
		gea_replay_stage_skip_seed(DisplayList::instance().replayCullCovered());
		for (int sy = y; sy < y + h; sy += stageRows) {
			const int sh = std::min(stageRows, y + h - sy);
			const bool staged = gea_replay_stage_begin(x, sy, x + w - 1, sy + sh - 1);
			gea::platform::display::Display::pushClip(x, sy, w, sh);
			DisplayList::instance().replay();
			gea::platform::display::Display::popClip();
			if (staged) gea_replay_stage_end();
		}
		DisplayList::instance().setReplayCullStart(-1);
		gea_replay_stage_skip_seed(false);
		return;
	}
	replayDisplayRegion(x, y, w, h);
}

void ViewportRegion::replayDisplayRegion(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	gea::platform::display::Display::pushClip(x, y, w, h);
	DisplayList::instance().replay();
	gea::platform::display::Display::popClip();
}

}  // namespace gea::embedded::ui
