#pragma once

#include "event.h"

#include <cstdint>
#include <functional>

namespace gea::framework::events {

enum class PointerEventType {
	TouchStart,
	TouchMove,
	TouchEnd,
	Click,
	Input,
	KeyDown,
	Rotary,
	Scroll
};

enum class EventPhase {
	None = 0,
	Capturing = 1,
	AtTarget = 2,
	Bubbling = 3
};

class EventTarget {
public:
	explicit EventTarget(int nodeId = -1) : nodeId_(nodeId) {}

	int id() const { return nodeId_; }
	bool valid() const { return nodeId_ >= 0; }
	const char *getAttribute(const char *name) const;
	const char *dataset(const char *name) const;
	int pressId() const;
	int pressValue() const;

private:
	int nodeId_;
};

struct TouchPoint {
	int identifier = 1;
	EventTarget target{};
	int screenX = 0;
	int screenY = 0;
	int clientX = 0;
	int clientY = 0;
	int pageX = 0;
	int pageY = 0;
};

struct PointerEvent {
	PointerEventType type;
	EventTarget target{};
	EventTarget currentTarget{};
	int targetId = -1;
	int currentTargetId = -1;
	int pointerId = 0;
	int x = 0;
	int y = 0;
	int clientX = 0;
	int clientY = 0;
	int pageX = 0;
	int pageY = 0;
	int screenX = 0;
	int screenY = 0;
	int pressId = -1;
	int pressValue = -1;
	int keyCode = 0;
	int delta = 0;
	bool primary = true;
	bool bubbles = true;
	bool cancelable = true;
	bool defaultPrevented = false;
	bool propagationStopped = false;
	EventPhase eventPhase = EventPhase::None;
	TouchPoint touches[1]{};
	TouchPoint targetTouches[1]{};
	TouchPoint changedTouches[1]{};
	int touchesLength = 0;
	int targetTouchesLength = 0;
	int changedTouchesLength = 0;

	const char *typeName() const;
	void preventDefault();
	void stopPropagation();
};

using EventListener = std::function<void(PointerEvent &)>;
using EventListenerId = std::uint64_t;
inline constexpr EventListenerId kInvalidEventListenerId = 0;

class TouchRuntime {
public:
	static bool start();
	static void poll(int nowMs);
	static void setDispatchEnabled(bool enabled);
	static bool dispatchEnabled();
	static void queueTouchEvent(TouchPhase phase, bool touching, int x, int y, int pointerId = 0);
	// Panel-physical -> logical (orientation-rotated) coordinate mapping.
	static void transformTouchToLogical(int *x, int *y);
	// Returns true if the event may have changed what's on screen (a real
	// down/move/up that fired handlers), so the caller can render immediately
	// instead of waiting for the next timer frame. Redundant moves (finger held
	// still, same coords) return false.
	static bool dispatchEvent(const Event &event);
	// True if a finger is currently down AND fired a down/move/up within the last
	// `windowMs` ms. Lets the frame loop keep rendering back-to-back through a drag
	// (like momentum's overrun catch-up) instead of stalling for the next touch
	// event after each frame. Goes false on a static hold so there's no busy-render.
	static bool gestureActiveWithin(int nowMs, int windowMs);
	static void resetGestureState();
};

}  // namespace gea::framework::events
