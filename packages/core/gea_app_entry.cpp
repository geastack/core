// SPDX-License-Identifier: Apache-2.0
// Adapter that runs a gea app compiled as:
// TSX -> vite-plugin-gea JS -> geatsc C++.

#include "app.h"
#include "gea_perf_config.h"  // GEA_EMBEDDED_PERF master + per-subsystem perf flags
#include "gea/embedded.h"
#include "host/backends.h"
#include "host/worker.h"
#include "ui/style.h"
#include "ui/tree_internal.h"  // Tree::mount for the post-mount clean repaint in init()

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#include "services/frame_scheduler.h"
#else
#include <chrono>
#include <cstdio>
#endif

extern void __gea_top_level();
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) || !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
// Forward-declared inline runtime helper from packages/geatsc/src/targets/cpp/runtime/value.cpp.
// Drops pending microtasks WITHOUT firing them — see comment in value.cpp.
extern void gea_cpp_clear_microtasks();
#endif
namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

extern "C" void gea_cycle_collection_defer_begin() __attribute__((weak));
extern "C" void gea_cycle_collection_defer_end() __attribute__((weak));

namespace gea::framework::app {

namespace {

// May be unused when all per-frame perf consumers are compiled out
// (GEA_FRAME_PHASE_TIMING==0 and GEA_FRAME_PERF_LOG==0): phaseClockUs() returns
// a constant 0 and the window-log blocks vanish, leaving no caller.
[[maybe_unused]] std::int64_t nowUs()
{
#ifdef ESP_PLATFORM
	return esp_timer_get_time();
#else
	using Clock = std::chrono::steady_clock;
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
#endif
}

bool applicationFrameOwnsMountedRefresh()
{
#ifdef ESP_PLATFORM
	return !gea::framework::services::FrameScheduler::vsyncDriven();
#else
	return true;
#endif
}

class FrameCycleCollectionDeferral {
public:
	FrameCycleCollectionDeferral()
		: active_(gea_cycle_collection_defer_begin && gea_cycle_collection_defer_end)
	{
		if (active_) gea_cycle_collection_defer_begin();
	}
	~FrameCycleCollectionDeferral() {
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
		const auto started = nowUs();
#endif
		if (active_) {
			// Scope-exit collection runs after frame phases finish. Keep it
			// visible to the watchdog instead of reporting a stalled frame as idle.
			const auto previous = applicationFramePhaseRead();
			applicationFramePhaseSet(ApplicationFramePhase::CycleCollection);
			gea_cycle_collection_defer_end();
			applicationFramePhaseSet(previous);
		}
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
		const auto elapsed = nowUs() - started;
		if (elapsed > 30000) ESP_LOGW("frame_phase", "cycle_collection duration_ms=%lld", (long long)(elapsed / 1000));
#endif
	}

private:
	bool active_;
};

}  // namespace

void Application::init(int width, int height, double devicePixelRatio)
{
	// Top-level construction allocates the entire native object graph. Keep the
	// generated collector at a host safepoint until initialization has unwound,
	// just as Application::frame does for animation callbacks.
	FrameCycleCollectionDeferral deferCycleCollection;
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
	(void)width;
	(void)height;
	(void)devicePixelRatio;
	gea::host::resetAnimationFrameCallbacks();
	gea::host::resetScheduledTimers();
	__gea_top_level();
#else
	if (width > 0 && height > 0) {
		gea::embedded::ui::Document::setPreferredMountSize(width, height);
		// `vw`/`vh` resolve against the physical viewport. Explicit CSS `px`
		// lengths resolve through the target's device pixel ratio.
		gea::embedded::ui::setViewportMetrics(width, height, devicePixelRatio);
	}
	gea::host::resetAnimationFrameCallbacks();
	gea::host::resetScheduledTimers();
	// DROP (don't fire) any pending microtasks left over from the previous
	// app. The runtime's gea_cpp_microtask_queue is a process-wide static-inline;
	// reactive-binding callbacks scheduled by the previous app's last tick
	// hold node-id references and would otherwise fire on the new app's
	// first frame against reused indexes — leaking styles/transforms (analog-
	// clock hand rotations into breakout, balls-jsx positions into launcher
	// cards). DRAINING (firing them) is unsafe at this boundary because some
	// callbacks reach back into the runtime and synchronously touch display
	// state, which interleaves badly with the panel/SPI flush pipeline and
	// has been observed to wedge the SPI driver in mid-tx. CLEARING is safe:
	// the previous tree is being destroyed by Document::clear() on the next
	// line, so the dropped callbacks couldn't have done anything useful.
	gea_cpp_clear_microtasks();
	gea::embedded::ui::Document::instance().clear();
	// Defer per-node class-style recomputes for the duration of the initial
	// mount: __gea_top_level registers the stylesheet and builds the whole tree,
	// and recomputing each node's class styles per setTagName/setClassName/
	// appendChild is O(nodes^2 x rules). endStyleMountBatch() does one full pass.
	gea::embedded::ui::beginStyleMountBatch();
	__gea_top_level();
	gea::embedded::ui::endStyleMountBatch();
	gea::embedded::ui::Document::instance().refreshMountedIfDirty();

	// The initial mount inside __gea_top_level() ran while the style batch was
	// still open, so Tree::mount SKIPPED its flush (that frame is unstyled). Now
	// that endStyleMountBatch() has applied styles, do one clean full re-mount to
	// present the first STYLED frame. This is the guaranteed first present (it
	// can't be skipped — the batch is closed) and repaints the whole framebuffer
	// from settled layout, so there's no flash of unstyled content and no stale
	// pre-layout pixels. One cheap full paint at startup, before the frame loop.
	{
		auto &tree = gea::embedded::ui::Tree::instance();
		const int root = tree.mountedRoot();
		if (root >= 0) tree.mount(root, tree.mountedWidth(), tree.mountedHeight());
	}
#endif
}

namespace {

// Rolling-window frame stats: every kFrameLogInterval frames, log the per-phase
// µs aggregates and reset. Gives us a "where did the time go over the last ~1
// sec" view without a UI overlay or external profiler. GEA_FRAME_PERF_LOG (this
// file's "gea.perf:" line) and GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG (the esp32
// scheduler harvest) — and the GEA_EMBEDDED_PERF master that governs both — are
// defined in gea_perf_config.h (included via app.h-side headers / directly).
//
// The per-frame phase TIMING (the nowUs() reads bracketing each phase + the stat
// accumulation in recordFramePhase) only has a consumer when one of those flags
// is on, so gate it on their OR. With both off (master off) the timing is pure
// dead work (~10 esp_timer_get_time() reads/frame feeding an accumulator nobody
// reads). NOTE: applicationFramePhaseSet() stays UNGATED below — it is a single
// relaxed atomic store feeding the frame watchdog's "app_phase=" stall report
// (see frame_scheduler.cpp), not perf machinery.
#define GEA_FRAME_PHASE_TIMING (GEA_FRAME_PERF_LOG || GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG)

#if GEA_FRAME_PERF_LOG
// 300-frame window (~5s). Each emission is an ESP_LOGI on the frame task; at 60 frames
// it cost ~1 dropped VBlank per ~60 under TE-sync (the frame overran its ~16.7ms TE
// window during the console write). A 5s cadence keeps the rolling fps/phase readout
// while making that perturbation negligible, so vsync stays locked at the panel rate.
constexpr int kFrameLogInterval = 300;
int gFrameLogCounter = 0;
int64_t gFrameLogWindowStartUs = 0;
// The window keeps its OWN sums. The shared applicationFramePerfStats
// accumulator is owned per-frame by the esp32 frame scheduler: runFrame()
// resets it at frame start and harvests it after the app callback for the
// perf-lite line. Reading/resetting it here used to (a) show ~one frame's
// stats divided by kFrameLogInterval — near-zero busy/idle regardless of real
// load — and (b) zero the 60th frame before the scheduler's harvest.
ApplicationFramePerfStats gWindowPerfStats;
// Sum of Application::frame() entry→exit wall over the window. Splits the
// idle (= wall − busy) into in-callback non-phase overhead (appWall − busy) vs
// out-of-callback scheduler dispatch (wall − appWall) — the 0.9ms-idle hunt.
int64_t gAppFrameWallUs = 0;

void logFramePerfWindow(int64_t windowEndUs)
{
	const ApplicationFramePerfStats stats = gWindowPerfStats;
	const int64_t totalPhaseUs = stats.drainMicrotasksUs + stats.animationFrameUs +
	                             stats.styleRecomputeUs + stats.documentFrameUs +
	                             stats.refreshMountedUs;
	const int64_t wallUs = windowEndUs - gFrameLogWindowStartUs;
	const double frames = kFrameLogInterval;
	const double fps = wallUs > 0 ? (frames * 1e6) / static_cast<double>(wallUs) : 0.0;
	const double drainMs = stats.drainMicrotasksUs / 1000.0 / frames;
	const double animMs = stats.animationFrameUs / 1000.0 / frames;
	const double styleMs = stats.styleRecomputeUs / 1000.0 / frames;
	const double docMs = stats.documentFrameUs / 1000.0 / frames;
	const double refMs = stats.refreshMountedUs / 1000.0 / frames;
	const double busyMs = totalPhaseUs / 1000.0 / frames;
	const double wallMsPerFrame = wallUs / 1000.0 / frames;
	const double idleMs = wallMsPerFrame - busyMs;
	const double appWallMs = gAppFrameWallUs / 1000.0 / frames;
	const double appOverheadMs = appWallMs - busyMs;       // non-phase work inside the callback
	const double dispatchMs = wallMsPerFrame - appWallMs;  // scheduler dispatch outside the callback
#ifdef ESP_PLATFORM
	ESP_LOGI("gea.perf",
	         "fps=%.1f wall=%.2fms busy=%.2fms idle=%.2fms(appovh=%.2f disp=%.2f) | drain=%.2f anim=%.2f style=%.2f doc=%.2f refresh=%.2f",
	         fps, wallMsPerFrame, busyMs, idleMs, appOverheadMs, dispatchMs, drainMs, animMs, styleMs, docMs, refMs);
#else
	std::printf("[gea.perf] fps=%.1f wall=%.2fms busy=%.2fms idle=%.2fms(appovh=%.2f disp=%.2f) | drain=%.2f anim=%.2f style=%.2f doc=%.2f refresh=%.2f\n",
	            fps, wallMsPerFrame, busyMs, idleMs, appOverheadMs, dispatchMs, drainMs, animMs, styleMs, docMs, refMs);
#endif
	gWindowPerfStats = {};
	gAppFrameWallUs = 0;
}
#endif  // GEA_FRAME_PERF_LOG

// Reads the frame timer ONLY when a per-frame perf consumer is compiled in (see
// GEA_FRAME_PHASE_TIMING). Otherwise returns 0, so the bracketing reads + the
// recordFramePhase() adds below constant-fold to nothing — a production build
// pays zero for phase instrumentation.
std::int64_t phaseClockUs()
{
#if GEA_FRAME_PHASE_TIMING || CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
	return nowUs();
#else
	return 0;
#endif
}

// Feeds whichever per-frame perf consumers are compiled in: the shared
// accumulator harvested by the esp32 scheduler (GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG)
// and/or this file's rolling app-perf window (GEA_FRAME_PERF_LOG). A no-op when
// neither is set.
void recordFramePhase(ApplicationFramePhase phase, int64_t durationUs)
{
#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
	if (durationUs > 30000)
		ESP_LOGW("frame_phase", "%s duration_ms=%lld", applicationFramePhaseName(phase),
			static_cast<long long>(durationUs / 1000));
#endif
#if GEA_EMBEDDED_FRAME_SCHEDULER_PERF_LOG
	applicationFramePerfStatsAdd(phase, durationUs);
#endif
#if GEA_FRAME_PERF_LOG
	applicationFramePerfStatsAdd(gWindowPerfStats, phase, durationUs);
#endif
#if !GEA_FRAME_PHASE_TIMING
	(void)phase;
	(void)durationUs;
#endif
}

}  // namespace

void Application::frame(int timestampMs)
{
	// Generated callbacks keep native references in ordinary C++ stack locals.
	// An allocation-pressure collection inside one of those callbacks cannot see
	// those roots, so defer it until every phase in this frame has unwound.
	FrameCycleCollectionDeferral deferCycleCollection;
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
	applicationFramePhaseSet(ApplicationFramePhase::DrainMicrotasks);
	generated::drainMicrotasks();
	applicationFramePhaseSet(ApplicationFramePhase::AnimationFrameCallbacks);
	gea::host::runAnimationFrameCallbacks(timestampMs);
	generated::drainMicrotasks();
	applicationFramePhaseSet(ApplicationFramePhase::Idle);
#else
#if GEA_FRAME_PERF_LOG
	if (gFrameLogWindowStartUs == 0) gFrameLogWindowStartUs = nowUs();
	const int64_t kAppFrameStartUs = nowUs();
#endif

	gea::framework::display::DisplayBackend::updateAutoRotationFromAccelerometer();

	// Coalesce class-style recomputes triggered by this frame's reactive updates
	// (microtask drains + rAF callbacks). Without this, one interaction that
	// changes a near-root class — e.g. a weather city switch flipping the shell's
	// theme class — triggers a full-tree style recompute per change on the frame
	// task, freezing the UI for seconds. endStyleMountBatch() does one coalesced
	// pass before layout/render; it is cheap when no class change was deferred.
	gea::embedded::ui::beginStyleMountBatch();

	int64_t startUs = phaseClockUs();
	applicationFramePhaseSet(ApplicationFramePhase::DrainMicrotasks);
	generated::drainMicrotasks();
	recordFramePhase(ApplicationFramePhase::DrainMicrotasks, phaseClockUs() - startUs);

	startUs = phaseClockUs();
	applicationFramePhaseSet(ApplicationFramePhase::AnimationFrameCallbacks);
	gea::host::runAnimationFrameCallbacks(timestampMs);
	applicationFramePhaseSet(ApplicationFramePhase::CallbackMicrotasks);
	generated::drainMicrotasks();
	recordFramePhase(ApplicationFramePhase::AnimationFrameCallbacks, phaseClockUs() - startUs);
	// The build already proves whether network services are reachable. Keep their
	// callback pumps out of offline frame loops, while still servicing local workers.
#if !defined(GEA_EMBEDDED_NETWORK_SERVICES_DISABLED) || !GEA_EMBEDDED_NETWORK_SERVICES_DISABLED
	applicationFramePhaseSet(ApplicationFramePhase::WebSocketCallbacks);
	startUs = phaseClockUs();
	gea::host::websocket::runCallbacks();
	recordFramePhase(ApplicationFramePhase::WebSocketCallbacks, phaseClockUs() - startUs);
	applicationFramePhaseSet(ApplicationFramePhase::RtcCallbacks);
	startUs = phaseClockUs();
	gea::host::rtc::runCallbacks();
	recordFramePhase(ApplicationFramePhase::RtcCallbacks, phaseClockUs() - startUs);
#else
	gea::host::workers::Context::runMainPending();
#endif
	applicationFramePhaseSet(ApplicationFramePhase::VideoPresentation);
	startUs = phaseClockUs();
	gea::host::video::presentFrames();
	recordFramePhase(ApplicationFramePhase::VideoPresentation, phaseClockUs() - startUs);
#if !defined(GEA_EMBEDDED_NETWORK_SERVICES_DISABLED) || !GEA_EMBEDDED_NETWORK_SERVICES_DISABLED
	applicationFramePhaseSet(ApplicationFramePhase::HttpRequests);
	startUs = phaseClockUs();
	gea::host::http::runRequests();
	recordFramePhase(ApplicationFramePhase::HttpRequests, phaseClockUs() - startUs);
#endif
	// Apply the coalesced style recompute for everything changed above, so layout
	// and render below see final styles. Measured as its own phase: a city/theme
	// class change near the root cascades a full-subtree recompute here, and it was
	// previously invisible (folded into `app=` but in no phase counter).
	startUs = phaseClockUs();
	applicationFramePhaseSet(ApplicationFramePhase::StyleRecompute);
	gea::embedded::ui::endStyleMountBatch();
	recordFramePhase(ApplicationFramePhase::StyleRecompute, phaseClockUs() - startUs);

	startUs = phaseClockUs();
	applicationFramePhaseSet(ApplicationFramePhase::DocumentFrame);
	gea::embedded::ui::Document::instance().frame(timestampMs);
	recordFramePhase(ApplicationFramePhase::DocumentFrame, phaseClockUs() - startUs);

	startUs = phaseClockUs();
	applicationFramePhaseSet(ApplicationFramePhase::RefreshMounted);
	if (applicationFrameOwnsMountedRefresh())
		gea::embedded::ui::Document::instance().refreshMountedIfDirty();
	recordFramePhase(ApplicationFramePhase::RefreshMounted, phaseClockUs() - startUs);
	applicationFramePhaseSet(ApplicationFramePhase::Idle);

#if GEA_FRAME_PERF_LOG
	gAppFrameWallUs += nowUs() - kAppFrameStartUs;
	if (++gFrameLogCounter >= kFrameLogInterval) {
		const int64_t windowEndUs = nowUs();
		logFramePerfWindow(windowEndUs);
		gFrameLogCounter = 0;
		gFrameLogWindowStartUs = windowEndUs;
	}
#endif
#endif
}

void Application::toggleSettings() {}

}  // namespace gea::framework::app
