#pragma once

#include <cstdint>

#ifndef GEA_EMBEDDED_CSS_DEVICE_PIXEL_RATIO
#define GEA_EMBEDDED_CSS_DEVICE_PIXEL_RATIO 1.0
#endif

// The ratio the FRAME LOOP hands to Application::init, i.e. the one that makes
// `width: 100px` mean 100 or 200 physical pixels. It is deliberately separate
// from GEA_EMBEDDED_CSS_DEVICE_PIXEL_RATIO above: that one is what a BOARD
// declares, and boards declare it for the font rasteriser (the CMakeLists hands
// the same variable to geatsc as --font-device-pixel-ratio, which bakes glyph
// atlases at cssPx * ratio). Runtime::run passed a hardcoded 1.0 for layout, so
// on every board declaring 1.5 the two disagreed. Promoting the board's value to
// the layout ratio would relayout every app already authored against the 1.0
// behaviour, so the layout ratio stays 1.0 unless an APP asks for another one --
// only the app's own CSS knows which pixels its lengths are written in. Declare
// it with `gea.cssDevicePixelRatio` in the app manifest: the CLI sets this and
// the font ratio together, so the atlas and the layout agree.
#ifndef GEA_EMBEDDED_CSS_LAYOUT_DEVICE_PIXEL_RATIO
#define GEA_EMBEDDED_CSS_LAYOUT_DEVICE_PIXEL_RATIO 1.0
#endif

namespace gea::framework::app {

enum class ApplicationFramePhase : std::uint8_t {
	Idle,
	DrainMicrotasks,
	AnimationFrameCallbacks,
	StyleRecompute,
	DocumentFrame,
	RefreshMounted,
	CycleCollection,
	CallbackMicrotasks,
	WebSocketCallbacks,
	RtcCallbacks,
	VideoPresentation,
	HttpRequests,
};

struct ApplicationFramePerfStats {
	std::int64_t drainMicrotasksUs = 0;
	std::int64_t animationFrameUs = 0;
	std::int64_t styleRecomputeUs = 0;
	std::int64_t documentFrameUs = 0;
	std::int64_t refreshMountedUs = 0;
};

class Application {
public:
	static void init(int width, int height, double devicePixelRatio = GEA_EMBEDDED_CSS_DEVICE_PIXEL_RATIO);
	static void prepareMount(int width, int height, double devicePixelRatio);
	static void finishMount(bool mounted);
	// Tears down everything the running program left in the runtime -- its tree,
	// stylesheet, timers, frame callbacks, microtasks, animations, bundled image
	// decodes and text rasters -- so another program can mount in its place.
	static void unmount();
	static void frame(int timestampMs);
	static void toggleSettings();
};

void applicationFramePerfStatsReset();
void applicationFramePerfStatsAdd(ApplicationFramePhase phase, std::int64_t durationUs);
void applicationFramePerfStatsAdd(ApplicationFramePerfStats &stats, ApplicationFramePhase phase,
                                  std::int64_t durationUs);
ApplicationFramePerfStats applicationFramePerfStatsRead();
void applicationFramePhaseSet(ApplicationFramePhase phase);
ApplicationFramePhase applicationFramePhaseRead();
const char *applicationFramePhaseName(ApplicationFramePhase phase);

}  // namespace gea::framework::app
