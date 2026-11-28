#pragma once

#include "event.h"
#include "native_identity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace gea::detail { struct RefVisitor; template<class T> struct TraceEdges; }

namespace gea::framework::events {

enum class PointerEventType {
	TouchStart,
	TouchMove,
	TouchEnd,
	Click,
	Input,
	KeyDown,
	Rotary,
	Scroll,
	KeyUp,
	PointerCancel,
	WindowBlur,
	WindowResize,
	Change,
	FocusIn,
	FocusOut
};

enum class EventPhase {
	None = 0,
	Capturing = 1,
	AtTarget = 2,
	Bubbling = 3
};

class EventTarget {
public:
	explicit EventTarget(int nodeId = -1) : nodeId_(nodeId), identity_(identity::node(nodeId)) {}
	const void *identity() const { return identity_.get(); }
	std::shared_ptr<const void> weakIdentityOwner() const { return identity_; }
	const identity::Owner &identityOwner() const { return identity_; }

	int id() const { return valid() ? nodeId_ : -1; }
	bool valid() const { return identity::current(nodeId_, identity_); }
	const char *getAttribute(const char *name) const;
	const char *dataset(const char *name) const;
	int pressId() const;
	int pressValue() const;

private:
	int nodeId_;
	identity::Owner identity_;
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

// Native callback lowering may copy the event carrier. Its cancellation flags
// still belong to the same dispatched event, including copies retained later.
// Allocate shared storage only when a carrier is actually copied.
class EventControlFlag {
public:
  EventControlFlag(bool value = false) : initial_(value) {}
  EventControlFlag(const EventControlFlag &other) : shared_(other.share()) {}
  EventControlFlag &operator=(const EventControlFlag &other) {
    if (this != &other) shared_ = other.share();
    return *this;
  }
  EventControlFlag &operator=(bool value) {
    if (shared_) *shared_ = value;
    else initial_ = value;
    return *this;
  }
  operator bool() const { return shared_ ? *shared_ : initial_; }
private:
  std::shared_ptr<bool> share() const {
    if (!shared_) shared_ = std::make_shared<bool>(initial_);
    return shared_;
  }
  bool initial_ = false;
  mutable std::shared_ptr<bool> shared_;
};

struct PointerEvent {
	PointerEventType type;
	EventTarget target{};
	EventTarget currentTarget{};
	int targetId = -1;
	int currentTargetId = -1;
	int pointerId = 0;
	// Unknown/non-pointer producers never impersonate a primary button edge.
	int button = -1;
	std::uint32_t buttons = 0;
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
	// Native keyboard metadata. Non-keyboard producers retain explicit empty
	// strings/false flags; no browser record or boxed event is involved.
	std::string key;
	std::string code;
	bool altKey = false;
	bool ctrlKey = false;
	bool metaKey = false;
	bool shiftKey = false;
	bool repeat = false;
	bool isComposing = false;
	int delta = 0;
	bool primary = true;
	bool bubbles = true;
	bool cancelable = true;
	EventControlFlag defaultPrevented;
	EventControlFlag propagationStopped;
	identity::Occurrence occurrenceIdentity;
	explicit constexpr operator bool() const noexcept { return true; }
	const void *identity() const { return occurrenceIdentity.identity(); }
	std::shared_ptr<const void> weakIdentityOwner() const { return occurrenceIdentity.owner(); }
	EventPhase eventPhase = EventPhase::None;
	TouchPoint touches[1]{};
	TouchPoint targetTouches[1]{};
	TouchPoint changedTouches[1]{};
	int touchesLength = 0;
	int targetTouchesLength = 0;
	int changedTouchesLength = 0;

	// Scoped subscriber spelling; physical phase/type and occurrence stay unchanged.
	std::string subscriberTypeName;
	const char *typeName() const;
	void preventDefault();
	void stopPropagation();
};

// Ordinary native callbacks keep the platform std::function's own small-object
// storage. Explicit ADL-traceable callbacks instead own a concrete copy: a
// shared erased model would report one physical capture once for every copy.
class EventListener {
 inline static thread_local const PointerEvent *currentEvent_ = nullptr;
 struct Model {
  virtual ~Model() = default;
  virtual std::unique_ptr<Model> clone() const = 0;
  virtual void invoke(PointerEvent&) const = 0;
  virtual void trace(gea::detail::RefVisitor&) const = 0;
 };
 template<class F> struct Concrete final : Model {
  mutable F callable;
  explicit Concrete(F value) : callable(std::move(value)) {}
  std::unique_ptr<Model> clone() const override { return std::make_unique<Concrete>(callable); }
  void invoke(PointerEvent& event) const override { callable(event); }
  void trace(gea::detail::RefVisitor& visitor) const override { geaTraceRefs(callable, visitor); }
 };
 std::function<void(PointerEvent&)> ordinary_;
 std::unique_ptr<Model> traced_;
public:
 EventListener() = default;
 EventListener(std::nullptr_t) noexcept {}
 template<class F> requires (!std::is_same_v<std::decay_t<F>,EventListener> &&
                            std::is_copy_constructible_v<std::decay_t<F>> &&
                            std::is_invocable_r_v<void,std::decay_t<F>&,PointerEvent&>)
 EventListener(F&& callable) {
  using Held = std::decay_t<F>;
  if constexpr (requires(const Held& held, gea::detail::RefVisitor& visitor) { geaTraceRefs(held,visitor); })
   traced_ = std::make_unique<Concrete<Held>>(std::forward<F>(callable));
  else ordinary_ = std::forward<F>(callable);
 }
 EventListener(const EventListener& other)
  : ordinary_(other.ordinary_), traced_(other.traced_ ? other.traced_->clone() : nullptr) {}
 EventListener(EventListener&&) noexcept = default;
 EventListener& operator=(const EventListener& other) { EventListener copy(other); swap(copy); return *this; }
 EventListener& operator=(EventListener&&) noexcept = default;
 EventListener& operator=(std::nullptr_t) noexcept { ordinary_=nullptr; traced_.reset(); return *this; }
 void swap(EventListener& other) noexcept { ordinary_.swap(other.ordinary_); traced_.swap(other.traced_); }
 explicit operator bool() const noexcept { return bool(traced_) || bool(ordinary_); }
 static const PointerEvent *currentEvent() noexcept { return currentEvent_; }
 void operator()(PointerEvent& event) const {
  struct RestoreEvent {
   const PointerEvent *previous;
   ~RestoreEvent() { currentEvent_ = previous; }
  } restore{std::exchange(currentEvent_, &event)};
  if (traced_) traced_->invoke(event);
  else ordinary_(event);
 }
 friend void geaTraceRefs(const EventListener& value, gea::detail::RefVisitor& visitor) {
  if(value.traced_) value.traced_->trace(visitor);
 }
};

// Invocation has no hidden captures. Each owned capture occurs exactly once,
// in this tuple, and nested adapters preserve that same ADL trace authority.
template<class Invoke,class... Captures>
struct ExplicitEventCallback {
 static_assert(std::is_empty_v<Invoke>, "event invocation must be capture-free");
 Invoke invoke;
 std::tuple<Captures...> captures;
 void operator()(PointerEvent& event) {
  std::apply([&](auto&... values){invoke(values...,event);},captures);
 }
 friend void geaTraceRefs(const ExplicitEventCallback& value,gea::detail::RefVisitor& visitor) {
  std::apply([&](const auto&... values){
   auto trace=[&]<class T>(const T& capture){
    if constexpr(requires { gea::detail::TraceEdges<T>::supported; })
     gea::detail::TraceEdges<T>::visit(capture,visitor);
    else if constexpr(requires { geaTraceRefs(capture,visitor); }) geaTraceRefs(capture,visitor);
   };
   (trace(values),...);
  },value.captures);
 }
};
template<class Invoke,class... Captures>
auto explicitEventCallback(Invoke invoke,Captures&&... captures) {
 return ExplicitEventCallback<Invoke,std::decay_t<Captures>...>{std::move(invoke),{std::forward<Captures>(captures)...}};
}
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
