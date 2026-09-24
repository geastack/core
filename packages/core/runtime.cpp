// SPDX-License-Identifier: Apache-2.0
#include "runtime.h"

#include "app.h"
#include "apps.h"
#include "display.h"
#include "input.h"
#include "memory.h"
#include "events.h"
#include "power.h"
#include "services/app_runner.h"
#include "services/app_state.h"
#include "services/battery_service.h"
#include "services/bluetooth_service.h"
#include "services/diagnostics.h"
#include "services/frame_scheduler.h"
#include "services/network_services.h"
#include "services/runtime_log.h"
#include "services/settings_surface.h"
#include "services/storage_service.h"

#include "host/storage.h"
#include "wifi.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#if __has_include("gea_embedded_app_config.h")
#include "gea_embedded_app_config.h"
#endif
#ifndef GEA_EMBEDDED_WIFI_SSID
#define GEA_EMBEDDED_WIFI_SSID ""
#endif
#ifndef GEA_EMBEDDED_WIFI_EARLY_CONNECT
#define GEA_EMBEDDED_WIFI_EARLY_CONNECT 0
#endif

// UI/audio applications can keep touch event-driven rather than spending CPU
// on back-to-back no-op frames between samples. Touch events still request an
// immediate frame; animations continue on the configured frame timer.
#ifndef GEA_EMBEDDED_CONTINUOUS_TOUCH_FRAMES
#define GEA_EMBEDDED_CONTINUOUS_TOUCH_FRAMES 1
#endif

#include "css/declarative.h"
#include "css/engine.h"
#include "ui/style.h"
#include "ui/document.h"
#include "ui/tree_internal.h"

namespace gea::framework {

namespace {

#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT

void run_app_frame(int timestampMs, void *context)
{
	(void)context;
	events::TouchRuntime::poll(timestampMs);
	services::BatteryService::poll(timestampMs);
	app::Application::frame(timestampMs);
}

void dispatch_event(const events::Event &event, const RuntimeOptions &)
{
	if (event.type != events::EventType::Frame) return;
	services::FrameScheduler::FrameCallbacks frameCallbacks{};
	frameCallbacks.frame = run_app_frame;
	services::FrameScheduler::runFrame(frameCallbacks);
}

#else

void dispatch_rotary_input()
{
	const int delta = input::consumeRotaryDelta();
	if (delta == 0) return;
	// Rotary is a document-level event — no element target, no tree walk.
	gea::embedded::ui::dispatchDocumentRotary(delta);
}

// Hardware-button key presses (boards without touch — e.g. the e-paper's
// BOOT/PWR buttons) queued via input::queueKeyDown. A focused <input> gets the
// key first through its per-node keydown listener; then the document-level
// keydown handler runs (hardware buttons / app-level shortcuts bound on a
// non-input element), unless the input consumed it.
void dispatch_key_input()
{
	auto &tree = gea::embedded::ui::Tree::instance();
	for (int keyCode = input::consumeKeyDown(); keyCode != 0; keyCode = input::consumeKeyDown()) {
		events::PointerEvent event{};
		event.type = events::PointerEventType::KeyDown;
		event.keyCode = keyCode;
		event.bubbles = true;
		event.cancelable = true;

		const int inputId = tree.activeInputId();
		bool stopped = false;
		if (inputId >= 0) {
			event.targetId = inputId;
			event.currentTargetId = inputId;
			tree.dispatchEvent(event);
			stopped = event.propagationStopped;
		}
		if (!stopped) {
			event.propagationStopped = false;
			gea::embedded::ui::dispatchDocumentKeyDown(event);
		}
	}
}

void run_app_frame(int timestampMs, void *context)
{
	(void)context;
	const uint32_t nowMs = static_cast<uint32_t>(timestampMs);
	// Low-rate platform work shares the runtime event loop. This keeps input and
	// battery observation deterministic without dedicating several mostly-idle
	// FreeRTOS tasks (and their stacks) to polling.
	events::TouchRuntime::poll(static_cast<int>(nowMs));
	services::BatteryService::poll(static_cast<int>(nowMs));
	// A firmware image contains one application, mounted once before the frame
	// loop. Start its declarative animations on the first frame.
	static bool animationsScanned = false;
	if (!animationsScanned) {
		animationsScanned = true;
		gea::css::DeclarativeAnimations::scanAndStart(nowMs);
		gea::embedded::ui::StyleSheet::instance().startCssAnimations(nowMs);
	}

	gea::css::AnimationEngine::instance().tick(nowMs);
	// Render-only while settings overlays the frozen application.
	const bool refreshOnly = SettingsSurface::instance().active();
	if (refreshOnly)
		gea::embedded::ui::Document::instance().refreshMountedIfDirty();
	else {
		dispatch_rotary_input();
		dispatch_key_input();
		app::Application::frame(timestampMs);
	}
}

void dispatch_event(const events::Event &event, const RuntimeOptions &options)
{
	switch (event.type) {
	case events::EventType::Touch: {
		if (SettingsSurface::instance().active()) {   // settings is modal: taps go to it
			if (event.touchPhase == events::TouchPhase::Up)
				SettingsSurface::instance().handleTap(event.x, event.y);
			break;
		}
		services::AppState::lock();
		const bool touchChangedScene = events::TouchRuntime::dispatchEvent(event);
		services::AppState::unlock();
		// Input-triggered render: a touch that actually ran handlers (down / real
		// move / up) likely changed the scene, so post a Frame now instead of
		// letting the render wait for the timer's next tick. Without this, a
		// finger-down drag updates state here but only paints on the timer cadence
		// — a multi-ms idle gap per frame that drops a full-screen pan from ~60 to
		// ~42fps (finger-up momentum has no touch events, so it already renders
		// back-to-back via the overrun catch-up). Redundant moves (finger held
		// still) return false and post nothing — no busy-render on a static hold —
		// and the frame loop's drainFramesAndCheckInput discards duplicate posts.
		// When the panel TE is the single frame clock, do NOT post a touch-driven frame:
		// it would render off-TE (tearing, since the present no longer waits for VBlank)
		// and add a second producer. The touch handler already updated pan state; the next
		// TE edge (≤16.6ms) renders it. Only post the extra frame in the non-TE path.
		if (touchChangedScene && !services::FrameScheduler::vsyncDriven()) {
			events::Event frameEvent{};
			frameEvent.type = events::EventType::Frame;
			services::FrameScheduler::sendEvent(frameEvent, 0);
		}
		break;
	}

	case events::EventType::Frame: {
		services::FrameScheduler::FrameCallbacks frameCallbacks{};
		frameCallbacks.frame = run_app_frame;
		services::FrameScheduler::runFrame(frameCallbacks);
		// Graceful overrun: if the frame blew the 16ms budget, run its successor
		// back-to-back instead of waiting for the next timer tick (which would snap
		// the overrun to a 30fps cliff). drainFramesAndCheckInput() discards the
		// timer's redundant frame posts and reports the instant real input (touch)
		// is queued.
		// Keep rendering back-to-back while EITHER the last frame overran (momentum
		// / heavy animation, as before) OR a finger is down and moved recently. The
		// second case makes a finger-down drag self-drive at the render rate the way
		// momentum does: without it the loop renders one frame then blocks for the
		// next touch/timer event (~6ms idle per frame), which pinned a full-screen
		// finger-down pan to ~42fps while finger-up momentum ran ~60. The touch
		// driver only emits a move event when the finger position changes and only
		// re-arms after the previous one is consumed, so after each frame the next
		// move can be up to a poll-interval away — rendering on that cadence stalls.
		// Driving the loop ourselves reads the latest pan every frame instead.
		// A finger held still posts no moves, so gestureActiveWithin() lapses to
		// false after the window and the loop quiesces — no busy-render on a hold.
		for (;;) {
			const bool catchUp = services::FrameScheduler::takeCatchUpRequest();
			const bool activeDrag =
				GEA_EMBEDDED_CONTINUOUS_TOUCH_FRAMES &&
				events::TouchRuntime::gestureActiveWithin(services::FrameScheduler::nowMs(), 50);
			// TE single-clock: never run a frame back-to-back here — the panel TE posts the
			// next frame exactly one VBlank later, which is the whole point. Back-to-back
			// would render off-TE (tearing) and re-introduce a second cadence.
			if (services::FrameScheduler::vsyncDriven() || (!catchUp && !activeDrag)) break;
			// Service any queued input INLINE (touch dispatch is microseconds) and
			// keep going, instead of returning to the timer-paced outer loop.
			// drainFramesAndCheckInput() leaves a non-Frame event at the queue
			// front, so receiveEvent() returns it immediately without blocking.
			while (services::FrameScheduler::drainFramesAndCheckInput()) {
				events::Event pending{};
				if (!services::FrameScheduler::receiveEvent(&pending)) break;
				dispatch_event(pending, options);
			}
			services::FrameScheduler::runFrame(frameCallbacks);
		}
		// Persist any localStorage writes made this frame (or from a store's
		// init() on the PSRAM-stack task since the last frame). This is the only
		// place flash is touched for localStorage — always the main frame task.
		gea::host::Storage.flushPending();
		break;
	}

	case events::EventType::Timeout:
		break;

	case events::EventType::SettingsToggle:
		services::AppState::lock();
		SettingsSurface::instance().toggle();
		services::AppState::unlock();
		break;
	}
}

#endif

}  // namespace

void Runtime::runNativeBoot()
{
	// An app with native hardware of its own (audio, USB host, sensors) brings
	// it up here, before the display framebuffer and the radios take the
	// contiguous internal RAM they need. Idempotent, so a target may run it on
	// its bring-up task and run() below then finds nothing left to do.
	static bool done = false;
	if (done) return;
	done = true;
	if (gea_app_native_boot) gea_app_native_boot();
}

// Everything that has to happen before the first frame, split out of run() so a
// target can run it on a DIFFERENT task than the event loop. It reads and writes
// flash -- the SPIFFS mount, the localStorage restore, NVS from the radios --
// and a flash operation disables the cache, which trips
// esp_task_stack_is_sane_cache_disabled() for any task whose stack lives in
// external RAM. So a board that puts the loop on a PSRAM stack (the only way to
// keep 20 KB of internal DRAM free on a board whose app already owns most of it)
// calls boot() from its bring-up task first, and the loop task's run() then
// finds the work done. Idempotent, and single-shot in both outcomes: a target
// that only ever calls run() behaves exactly as before.
namespace {
// The app's own reserve, taken from inside gea_app_native_boot at a moment only
// the app knows. File-scope because boot() takes it before the hook runs and
// releases it after, and the hook has no runtime handle to pass it back through.
void *g_appDisplayReserve = nullptr;
}  // namespace

void Runtime::holdDisplayReserve(void *reserve)
{
	if (g_appDisplayReserve == reserve) return;
	gea::platform::memory::Memory::releaseInternalDma(g_appDisplayReserve);
	g_appDisplayReserve = reserve;
}

namespace {
void releaseDisplayReserves(void *runtimeReserve)
{
	gea::platform::memory::Memory::releaseInternalDma(runtimeReserve);
	gea::platform::memory::Memory::releaseInternalDma(g_appDisplayReserve);
	g_appDisplayReserve = nullptr;
}
}  // namespace

bool Runtime::boot(const RuntimeOptions &options)
{
	static bool done = false;
	static bool ok = false;
	if (done) return ok;
	done = true;
	// Held across the app's hook, released just before Display::init() below.
	// See GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES in runtime.h.
	void *displayReserve =
	    gea::platform::memory::Memory::reserveInternalDma(GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES);
	runNativeBoot();
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) || !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
	services::HeapProbe::log("runtime:start");
	services::StackProbe::logCurrentTask("runtime:start");
#endif
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
	if (!services::AppState::init()) return false;
	auto eventQueue = services::FrameScheduler::createEventQueue();
	if (!eventQueue) return false;
	releaseDisplayReserves(displayReserve);
	if (!gea::platform::display::Display::init()) return false;
	// The layout ratio, not the board's font ratio -- see the comment on
	// GEA_EMBEDDED_CSS_LAYOUT_DEVICE_PIXEL_RATIO in app.h. It defaults to 1.0, so
	// this is the same call the hardcoded literal made; an app that declares
	// `gea.cssDevicePixelRatio` gets its own value here and in the font atlas.
	app::Application::init(options.width, options.height, GEA_EMBEDDED_CSS_LAYOUT_DEVICE_PIXEL_RATIO);
	if (!gea::platform::display::Display::start()) return false;
	services::FrameScheduler::start(eventQueue);
#else
	if (!services::StorageService::init()) return false;
	// Restore persisted localStorage into its RAM mirror once, here on the
	// bring-up task, whose stack can touch flash — so app code can read
	// localStorage from anywhere, including a store's init().
	gea::host::Storage.load();
	services::HeapProbe::log("runtime:after_storage");
	if (!services::AppState::init()) return false;
	services::HeapProbe::log("runtime:after_app_state");

#if !GEA_EMBEDDED_NO_DISPLAY
	auto eventQueue = services::FrameScheduler::createEventQueue();
	if (!eventQueue) return false;
	services::HeapProbe::log("runtime:after_frame_queue");
#endif

	// BLE is opt-in but must be preinited EARLY for apps that use it. The BT
	// controller needs a large (~24-41 KB) contiguous block of internal DRAM, and
	// on a PSRAM display board the only moment that block is reliably available is
	// here at boot — before the app's JSX mount + display framebuffers fragment
	// internal RAM. If we wait for the lazy first-use path (HidServer::init's
	// preinit()), the controller's allocation fails (OOM) and BLE is silently
	// skipped → the device never advertises. preinitForApp() is gated on
	// GEA_EMBEDDED_APP_USES_BLE is derived from the application. Non-BLE images omit the controller source and component
	// entirely, so they pay neither its static pools nor its boot allocation.
	services::BluetoothService::preinitForApp();
	services::HeapProbe::log("runtime:after_preinit");

	gea::platform::power::Power::init();
	services::HeapProbe::log("runtime:after_power");
	releaseDisplayReserves(displayReserve);
	// A board that declares no panel and no canvas has no surface to render to,
	// so nothing here is constructed: no framebuffers, no app tree, no frame
	// loop. This is not a display that is switched off -- it is a board whose
	// hardware the firmware takes at its word, the same way an absent PMIC means
	// no PMIC driver. Everything the app starts from its native boot hook, which
	// ran above, is untouched.
#if !GEA_EMBEDDED_NO_DISPLAY
	if (!gea::platform::display::Display::init()) return false;
	services::HeapProbe::log("runtime:after_display_init");
#endif

#if GEA_EMBEDDED_WIFI_EARLY_CONNECT
	if (GEA_EMBEDDED_WIFI_SSID[0] != '\0') {
		// Both radios need large contiguous internal allocations, and BLE has the
		// stricter one-shot controller allocation, so it is claimed first above.
		// WiFi comes up AFTER Display::init(), not before it. The elastic-RAM
		// arbiter is one-directional: the display yields staging to the radios
		// (reserveInternal) and the radios never give any back. Starting WiFi first
		// therefore left the panel with a few hundred bytes of DMA-capable RAM, no
		// flush staging, and a dark screen — while the framebuffer rendered
		// correctly in PSRAM the whole time. Display::init() now claims its floor
		// before this line, and this bring-up shrinks it the designed way.
		services::DiagnosticsServer::print("Starting WiFi after display init\n");
		network::wifi().init();
	}
#endif

	if (!events::TouchRuntime::start()) return false;
	services::BatteryService::update();
	services::BatteryService::start();
	services::HeapProbe::log("runtime:after_input_battery");

	services::NetworkServicesOptions networkOptions{};
#ifdef GEA_EMBEDDED_DIAGNOSTICS_ENABLED
	// Opt in to the TCP diagnostics server (port 8081, channel 1 = log stream).
	// It defaults off because it costs a ~6 KiB task, and the default assumed
	// logs and screenshots come over USB. A board that has the DRAM to spare
	// gets device logs with no cable attached -- which is the only way to see a
	// boot log at all once the USB console is gone.
	networkOptions.diagnosticsEnabled = true;
#endif
#ifdef GEA_EMBEDDED_DIAGNOSTICS_DISABLED
	// Production lockdown: don't start the diagnostics server (diag_srv TCP
	// task + the esp_log_set_vprintf log-ring sink). OTA below stays enabled.
	networkOptions.diagnosticsEnabled = false;
#endif
	bool networkReady = services::NetworkServices::start(networkOptions);
	services::HeapProbe::log("runtime:after_network");

	services::RuntimeLog::printRuntimeBanner();
#if GEA_EMBEDDED_NO_DISPLAY
	(void)options;
	services::RuntimeLog::appStarted(networkReady);
#else
	if (!services::AppRunner::initApplication(options.width, options.height)) return false;
	services::HeapProbe::log("runtime:after_app_init");
	services::StackProbe::logCurrentTask("runtime:after_app_init");

	if (!gea::platform::display::Display::start()) return false;
	services::HeapProbe::log("runtime:after_display_start");
	services::StackProbe::logCurrentTask("runtime:after_display_start");

	services::FrameScheduler::start(eventQueue);
	services::HeapProbe::log("runtime:after_frame_start");
	services::StackProbe::logCurrentTask("runtime:after_frame_start");
	services::HeapTaskSummary::log("runtime:after_frame_start");
	services::RuntimeLog::appStarted(networkReady);
#endif

#endif
	ok = true;
	return true;
}

void Runtime::run(const RuntimeOptions &options)
{
	// A target that runs the loop on its own task calls boot() first, from a task
	// whose stack can touch flash; this call then returns that recorded outcome
	// without repeating any of the work.
	if (!boot(options)) return;
#if GEA_EMBEDDED_NO_DISPLAY
	// No surface means no frame queue, so there is no event to wait on and
	// nothing to dispatch. The loop below would find receiveEvent() failing
	// immediately against a queue that was never created and spin the core,
	// which starves whatever else shares it. boot() was the work; return.
	(void)options;
#else
	while (true) {
		events::Event event{};
		if (services::FrameScheduler::receiveEvent(&event)) {
			dispatch_event(event, options);
		}
	}
#endif
}

}  // namespace gea::framework
