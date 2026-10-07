#pragma once

#include "event.h"

namespace gea::framework::services {

class EventQueue {
public:
	constexpr EventQueue() = default;
	explicit constexpr EventQueue(void *native) : native_(native) {}

	explicit constexpr operator bool() const { return native_ != nullptr; }
	constexpr void *nativeHandle() const { return native_; }

private:
	void *native_ = nullptr;
};

class FrameScheduler {
public:
	using FrameCallback = void (*)(int timestamp_ms, void *context);

	struct FrameCallbacks {
		FrameCallback frame = nullptr;
		void *context = nullptr;
	};

#ifndef GEA_EMBEDDED_DEFAULT_FRAME_INTERVAL_US
#define GEA_EMBEDDED_DEFAULT_FRAME_INTERVAL_US 16667
#endif

	static constexpr int kDefaultFrameIntervalUs = GEA_EMBEDDED_DEFAULT_FRAME_INTERVAL_US;
	static constexpr int kDefaultFrameIntervalMs = 16;
	static constexpr int kMinFrameIntervalMs = 1;
	static constexpr int kMaxFrameIntervalMs = 1000;
	static constexpr int kFrameIntervalUs = kDefaultFrameIntervalUs;
	static constexpr int kFrameIntervalMs = kDefaultFrameIntervalMs;
	static constexpr int kWatchdogMs = 2000;
	static constexpr int kEventQueueDepth = 32;

	static EventQueue createEventQueue();
	static EventQueue eventQueue();
	static bool sendEvent(const gea::framework::events::Event &event, int wait_ms = 0);
	static bool receiveEvent(gea::framework::events::Event *event);
	static void start(EventQueue queue);
	static void runFrame(const FrameCallbacks &callbacks);
	// Graceful-overrun catch-up (see runFrame): takeCatchUpRequest() returns+clears
	// whether the last frame overran the budget; drainFramesAndCheckInput() discards
	// the timer's redundant queued frames and returns true if real input is waiting,
	// so the catch-up loop yields to input but not to its own frame posts.
	static bool takeCatchUpRequest();
	static bool drainFramesAndCheckInput();
	// TE-vsync single-clock pacing: when the active display has TE-sync enabled, the
	// panel's TE edge is the ONE frame clock. The TE ISR (teEdgeIsr → notifyVsyncFromISR)
	// becomes the sole frame producer; the scheduler's own timer stops posting frames
	// (it stays only as a dead-TE watchdog), and the runtime loop suppresses its
	// timer/touch frame posts + back-to-back catch-up. This eliminates the second clock
	// (scheduler timer) that raced the TE → unstable 30-44fps pan. No effect on non-vsync
	// apps. setVsyncDriven() is called from the display backend's setVSync().
	static void setVsyncDriven(bool driven);
	static bool vsyncDriven();
	// Post exactly one (coalesced) frame from the panel TE edge ISR. ISR-safe.
	static void notifyVsyncFromISR();
	static void setFrameIntervalMs(int interval_ms);
	static int frameIntervalMs();
	static void setFrameRate(double fps);
	static double frameRate();
#if defined(ESP_PLATFORM) && defined(GEA_NATIVE_DEBUGGER) && GEA_NATIVE_DEBUGGER
	// Debug cap applies to timer, TE and input-driven frames. Application FPS
	// changes cannot exceed it; zero restores normal scheduling.
	static void setDebuggerFrameRate(int fps);
	static int debuggerFrameRate();
#endif
	static int nowMs();
};

}  // namespace gea::framework::services
