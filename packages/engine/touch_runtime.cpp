// SPDX-License-Identifier: Apache-2.0
#include "app.h"
#include "ui/state_init.h"
#include "events.h"
#include "event.h"
#include "services/frame_scheduler.h"
#include "host/display_orientation.h"
#include "touch.h"
#include "ui/tree_internal.h"
#include "ui/virtual_keyboard.h"
#include "ui/debugger_picker.h"
#include "ui/input_retirement.h"
#include <stdexcept>
#include <exception>

#include <cstring>

#if defined(GEA_EMBEDDED_COMPARISON_BENCHMARK) && GEA_EMBEDDED_COMPARISON_BENCHMARK
// Optional target diagnostic: actual consumption, after move-cache coalescing.
extern "C" void gea_touch_trace_consumed(int phase, bool touching, int x, int y, int pointerId, int handlerX,
										 int handlerY) __attribute__((weak));
#endif

// Weak default for the optional multi-finger observer. Single-touch controllers
// (every target except the GT911 ones) don't override it, so registering a
// pointer observer is a harmless no-op there and they keep using the single
// Observer. Controllers that support simultaneous touches provide a strong
// override (see the P4-7 touch.cpp).
__attribute__((weak)) void gea::platform::touch::Touchscreen::setPointerObserver(
	gea::platform::touch::Touchscreen::PointerObserver) {}
__attribute__((weak)) void gea::platform::touch::Touchscreen::poll(int) {}

namespace gea::framework::events {

namespace {

class TouchEventDispatcher {
public:
	static TouchEventDispatcher &instance()
	{
		static TouchEventDispatcher dispatcher;
		return dispatcher;
	}
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
	// Not inlined: with the constant member initializers folded in, the
	// dispatcher is a .data object (see ui/state_init.h).
	__attribute__((noinline)) TouchEventDispatcher() {}
#endif

	void queue(TouchPhase phase, bool touching, int x, int y, int pointerId)
	{
		// The latest-move cache (consumeLatestMove) is the PRIMARY finger's only;
		// secondary fingers (pointerId >= 1) carry their coords in the event.
		if (!dispatchEnabled_ || !gea::framework::services::FrameScheduler::eventQueue()) {
			if (phase == TouchPhase::Move && pointerId == 0) {
				gea::platform::touch::Touchscreen::consumeLatestMove(nullptr, nullptr);
			}
			return;
		}

		Event event{};
		event.type = EventType::Touch;
		event.touchPhase = phase;
		event.touching = touching;
		event.x = x;
		event.y = y;
		event.pointerId = pointerId;

		// Move events are deliberately lossy: the controller keeps the freshest
		// coordinate in consumeLatestMove(), so a full queue may drop the queued
		// marker and let the next sample re-arm it. Down and Up are gesture edges;
		// dropping either leaves wasActive_ in the wrong state and makes a later
		// city/button tap appear to select the previous target. Give those edges
		// enough time to enter the queue even while a long render is completing.
		const int waitMs = phase == TouchPhase::Move ? 0 : 250;
		const bool sent = gea::framework::services::FrameScheduler::sendEvent(event, waitMs);
		if (!sent &&
		    phase == TouchPhase::Move && pointerId == 0) {
			gea::platform::touch::Touchscreen::consumeLatestMove(nullptr, nullptr);
		}
	}

	void setDispatchEnabled(bool enabled)
	{
		dispatchEnabled_ = enabled;
	}

	bool dispatchEnabled() const
	{
		return dispatchEnabled_;
	}

	void resetGesture()
	{
		++primaryTicket_;primaryOwner_={};primaryQuarantined_=false;primaryEnding_=false;
		wasActive_ = false;
		lastX_ = 0;
		lastY_ = 0;
		startX_ = 0;
		startY_ = 0;
		dragged_ = false;
		activeDefaultPrevented_ = false;
		activeTargetId_ = -1;
		for (Secondary &s : secondary_) {const auto next=s.ticket+1;s=Secondary{};s.ticket=next;}
	}

	// Returns true if this event may have changed on-screen state (a real
	// down/move/up that ran handlers), so the runtime can render immediately
	// instead of waiting for the next timer frame. A redundant move (finger held
	// at the same coordinates) returns false so a static hold can't busy-render.
	bool dispatch(const Event &event)
	{
#if defined(GEA_NATIVE_DEBUGGER) && GEA_NATIVE_DEBUGGER
		auto &picker = gea::embedded::ui::debuggerPicker;
		if (picker.capturesInput()) {
			int x = event.x, y = event.y;
			if (event.touchPhase == TouchPhase::Move && event.pointerId == 0) {
				gea::platform::touch::Touchscreen::consumeLatestMove(&x, &y);
				TouchRuntime::transformTouchToLogical(&x, &y);
			}
			if (picker.consume(event.touchPhase, x, y, event.pointerId)) return true;
		}
#endif
		// A primary move marker holds the controller's queued-move flag: every
		// path below must drain it, or the controller stops reporting moves and
		// every later drag is dead while taps still work.
		int tx = event.x;
		int ty = event.y;
		if (event.touchPhase == TouchPhase::Move && event.pointerId == 0) {
			gea::platform::touch::Touchscreen::consumeLatestMove(&tx, &ty);
			TouchRuntime::transformTouchToLogical(&tx, &ty);
		}
		// Immediate-mode apps (Display.ctx games) mount no UI tree: hitTestNode
		// returns -1 and there is no root to bind document listeners onto, so the
		// tree dispatch below can never reach the app. Deliver pointer events
		// straight to document-level listeners, exactly like the DOM document, so
		// every finger (pointerId 0..N) reaches document.addEventListener().
		if (gea::embedded::ui::Tree::instance().mountedRoot() < 0) {
			recordConsumed(event, event.touching, event.x, event.y, event.x, event.y);
			return dispatchToDocument(event);
		}
		// Additional simultaneous fingers are routed to independent per-pointer
		// slots so they can hold/tap their own buttons (e.g. move + jump) without
		// disturbing the primary pointer's scroll / focus / keyboard gesture.
		if (event.pointerId >= 1) {
			recordConsumed(event, event.touching, event.x, event.y, event.x, event.y);
			return dispatchSecondary(event);
		}

		if (retireInvalidPrimary(!event.touching && event.touchPhase == TouchPhase::Up)) return false;
		if(primaryQuarantined_){
			if(!event.touching && event.touchPhase==TouchPhase::Up)primaryQuarantined_=false;
			return false;
		}
		auto dispatchTicket=primaryTicket_;bool primaryBegan=false;
		bool changed = false;

		bool touching = event.touching;
		if (event.touchPhase == TouchPhase::Move) touching = true;
		// Up callbacks inherit the last consumed move, while tx/ty retain the
		// consumed edge coordinates. Expose both without changing routing.
		recordConsumed(event, touching, tx, ty, !touching && wasActive_ ? lastX_ : tx,
					   !touching && wasActive_ ? lastY_ : ty);

		if (touching && !wasActive_) {
			changed = true;
			startX_ = lastX_ = tx;
			startY_ = lastY_ = ty;
			dragged_ = false;
			activeDefaultPrevented_ = false;
			gea::embedded::ui::Tree::instance().pointerDown(tx, ty);
			const int targetId = gea::embedded::ui::Tree::instance().hitTestNode(tx, ty);
			if (targetId >= 0) {
				activeTargetId_ = targetId;primaryOwner_=gea::embedded::ui::NodeHandle(targetId);
				wasActive_=true;primaryBegan=true;primaryEnding_=false;dispatchTicket=++primaryTicket_;
				const bool prevented = dispatchPointer(PointerEventType::TouchStart, targetId, tx, ty, true);
				if(dispatchTicket!=primaryTicket_ || retireInvalidPrimary(primaryEnding_))return true;
				activeDefaultPrevented_ = prevented;
			}
		}

		if (touching && wasActive_ && !primaryBegan && (tx != lastX_ || ty != lastY_)) {
			changed = true;
			if (!dragged_) {
				int dx = tx - startX_;
				int dy = ty - startY_;
				// 10 px was too tight on a 410-px capacitive panel — finger
				// jitter during a stationary tap routinely reported 8-14 px of
				// drift, tripping the drag flag and suppressing the press
				// (visible as "tap on launcher tile sometimes does nothing").
				// 16 px is roughly Android's ViewConfiguration touch-slop, and
				// only delays intentional scroll initiation by 6 px.
				if (dx > 16 || dx < -16 || dy > 16 || dy < -16) {
					dragged_ = true;
				}
			}
			bool defaultPrevented = activeDefaultPrevented_;
			if (activeTargetId_ >= 0) {
				defaultPrevented = dispatchPointer(PointerEventType::TouchMove, activeTargetId_, tx, ty, true) || defaultPrevented;
				if(dispatchTicket!=primaryTicket_ || retireInvalidPrimary(primaryEnding_))return true;
			}
			if (!defaultPrevented) gea::embedded::ui::Tree::instance().pointerMove(tx, ty);
		}

		if (!touching && wasActive_) {
			changed = true;primaryEnding_=true;
			// Captured BEFORE any click handler runs: if a handler
			// programmatically focuses an input (element.focus()), the
			// focus-management default below must not clear it.
			const int focusBeforeUp = gea::embedded::ui::Tree::instance().activeInputId();
			gea::embedded::ui::Tree::instance().pointerUp();
			const int focusTarget = activeTargetId_ >= 0 ? inputAncestor(activeTargetId_) : -1;
			const bool targetInsideKeyboard =
			    activeTargetId_ >= 0 && gea::embedded::ui::VirtualKeyboard::instance().containsNode(activeTargetId_);
			const bool keyboardDismissTap =
			    !dragged_ &&
			    !activeDefaultPrevented_ &&
			    activeTargetId_ >= 0 &&
			    gea::embedded::ui::VirtualKeyboard::instance().active() &&
			    !targetInsideKeyboard &&
			    focusTarget < 0;
			if (keyboardDismissTap) {
				auto &tree = gea::embedded::ui::Tree::instance();
				tree.setActiveInput(-1);
				gea::embedded::ui::VirtualKeyboard::instance().sync();
			} else if (activeTargetId_ >= 0) {
				dispatchPointer(PointerEventType::TouchEnd, activeTargetId_, lastX_, lastY_, false);
				if(dispatchTicket!=primaryTicket_ || retireInvalidPrimary(primaryEnding_))return true;
			}
			if (!dragged_ && !activeDefaultPrevented_ && activeTargetId_ >= 0) {
				if (!keyboardDismissTap) dispatchPointer(PointerEventType::Click, activeTargetId_, lastX_, lastY_, false);
				if(dispatchTicket!=primaryTicket_ || retireInvalidPrimary(primaryEnding_))return true;
				// Focus management — set the active input if the tap
				// landed on one (or the closest input ancestor); clear
				// focus on taps outside any input.
				//
				// Skip the focus update entirely when the tap was on
				// a keyboard key: the keyboard's click handler called
				// preventDefault, AND we don't want the act of typing
				// to immediately close the keyboard. The keyboard's
				// keys carry pressIds in the reserved
				// VirtualKeyboard::keyboardPressIdRange — if the tap
				// went there, the keyboard already routed it and we
				// must leave activeInputId untouched.
				auto &tree = gea::embedded::ui::Tree::instance();
				// Don't run focus-clearing logic when the tap landed
				// anywhere inside the keyboard subtree — hitTestNode
				// often returns a key's child label, not the key
				// itself, so a pressId range check misses those.
				// Walking the ancestor chain is reliable.
				// A click handler that PROGRAMMATICALLY focused an input
				// (element.focus()) wins over the tap-position default — clearing
				// it here would close the keyboard the handler just opened.
				const bool handlerTookFocus = tree.activeInputId() != focusBeforeUp;
				if (!targetInsideKeyboard && !keyboardDismissTap && !handlerTookFocus) {
					tree.setActiveInput(focusTarget);
					gea::embedded::ui::VirtualKeyboard::instance().sync();
				}
			}
			if(dispatchTicket!=primaryTicket_ || retireInvalidPrimary(primaryEnding_))return true;
			activeTargetId_ = -1;primaryOwner_={};primaryEnding_=false;
			activeDefaultPrevented_ = false;
		}

		wasActive_ = touching;
		if (touching) {
			lastX_ = tx;
			lastY_ = ty;
		}
		if (changed) lastActivityMs_ = gea::framework::services::FrameScheduler::nowMs();
		return changed;
	}

	// A raw allocation index is never a gesture lease. Destructive removal
	// invalidates the captured birth owner, even if that index is reused.
	bool retireInvalidPrimary(bool physicalUp)
	{
		if (!wasActive_ || activeTargetId_ < 0 || primaryOwner_.valid()) return false;
		primaryQuarantined_ = !physicalUp;primaryEnding_=false;++primaryTicket_;
		primaryOwner_={};activeTargetId_=-1;wasActive_=false;activeDefaultPrevented_=false;
		gea::embedded::ui::cancelPrimaryPointerInput();
		return true;
	}

	void retireSubtrees(const gea::embedded::ui::InputRetirementRoots &removed,const gea::embedded::ui::InputRetirementRoots &retained)
	{
		using namespace gea::embedded::ui;
		struct Cancel {NodeHandle owner;int x,y,pointer;bool primary,emit;};
		std::vector<Cancel> cancelled;cancelled.reserve(1+kMaxSecondaryPointers);
		if(inputOwnerLeaving(primaryOwner_,removed,retained)){
			cancelled.push_back({primaryOwner_,lastX_,lastY_,1,true,!primaryEnding_});
			primaryQuarantined_=!primaryEnding_;primaryEnding_=false;++primaryTicket_;
			primaryOwner_={};activeTargetId_=-1;wasActive_=false;activeDefaultPrevented_=false;
			cancelPrimaryPointerInput();
		}
		for(int i=0;i<kMaxSecondaryPointers;++i){auto &slot=secondary_[i];
			if(!inputOwnerLeaving(slot.owner,removed,retained))continue;
			cancelled.push_back({slot.owner,slot.lastX,slot.lastY,i+2,false,!slot.ending});
			slot.quarantined=!slot.ending;slot.ending=false;++slot.ticket;slot.active=false;slot.targetId=-1;slot.owner={};
		}
		// All admitted leases are closed before the first user cancellation callback.
		std::exception_ptr failure;
		for(const auto &entry:cancelled)if(entry.emit&&entry.owner.valid()) {
			try {dispatchPointer(PointerEventType::PointerCancel,entry.owner.id(),entry.x,entry.y,false,entry.pointer,entry.primary);}
			catch(...) {if(!failure)failure=std::current_exception();}
		}
		if(failure)std::rethrow_exception(failure);
		if(inputOwnerLeaving(primaryOwner_,removed,retained))throw std::runtime_error("primary gesture revived during retirement");
		for(const auto &slot:secondary_)if(inputOwnerLeaving(slot.owner,removed,retained))throw std::runtime_error("secondary gesture revived during retirement");
	}

	// Finger down now AND a down/move/up fired within the last windowMs. Used by
	// the frame loop to render a drag back-to-back; a static hold posts no moves,
	// so this lapses to false after the window and rendering quiesces.
	bool activeWithin(int nowMs, int windowMs) const
	{
		if (!wasActive_) return false;
		const int dt = nowMs - lastActivityMs_;
		return dt >= 0 && dt <= windowMs;
	}

private:
  static void recordConsumed(const Event &event, bool touching, int x, int y, int handlerX, int handlerY)
  {
#if defined(GEA_EMBEDDED_COMPARISON_BENCHMARK) && GEA_EMBEDDED_COMPARISON_BENCHMARK
	  if (gea_touch_trace_consumed) {
		  gea_touch_trace_consumed(static_cast<int>(event.touchPhase), touching, x, y, event.pointerId, handlerX,
								   handlerY);
	  }
#else
	  (void)event;
	  (void)touching;
	  (void)x;
	  (void)y;
	  (void)handlerX;
	  (void)handlerY;
#endif
  }

  // Lightweight per-pointer handler for non-primary fingers. Independently
  // hit-tests and fires touch*/click on its own target, but does NOT touch
  // scroll / focus / keyboard / the hardware Touchscreen state — those stay
  // owned by the primary pointer (pointerId 0).
  bool dispatchSecondary(const Event &event)
  {
    const int index=event.pointerId-1;
    if(index<0||index>=kMaxSecondaryPointers)return false;
    auto &slot=secondary_[index];const int x=event.x,y=event.y;
    const bool touching=event.touching||event.touchPhase==TouchPhase::Move;
    const int pointer=event.pointerId+1;
    auto retireInvalid=[&](){
      if(!slot.active || slot.targetId<0 || slot.owner.valid())return false;
      slot.quarantined=touching;slot.ending=false;++slot.ticket;slot.active=false;
      slot.targetId=-1;slot.owner={};slot.dragged=false;slot.defaultPrevented=false;return true;
    };
    if(retireInvalid())return false;
    if(slot.quarantined){if(!touching&&event.touchPhase==TouchPhase::Up)slot.quarantined=false;return false;}
    if(touching&&!slot.active){
      slot.active=true;slot.ending=false;const auto ticket=++slot.ticket;
      slot.startX=slot.lastX=x;slot.startY=slot.lastY=y;slot.dragged=false;slot.defaultPrevented=false;
      slot.targetId=gea::embedded::ui::Tree::instance().hitTestNode(x,y);
      slot.owner=gea::embedded::ui::NodeHandle(slot.targetId);
      if(slot.targetId>=0){const bool prevented=dispatchPointer(PointerEventType::TouchStart,slot.targetId,x,y,true,pointer,false);if(ticket!=slot.ticket || retireInvalid())return true;slot.defaultPrevented=prevented;}
      return true;
    }
    const auto ticket=slot.ticket;
    if(touching&&slot.active){
      const bool moved=x!=slot.lastX||y!=slot.lastY;
      if(moved){if(!slot.dragged){const int dx=x-slot.startX,dy=y-slot.startY;slot.dragged=dx>16||dx< -16||dy>16||dy< -16;}
        if(slot.targetId>=0)dispatchPointer(PointerEventType::TouchMove,slot.targetId,x,y,true,pointer,false);
        if(ticket!=slot.ticket || retireInvalid())return true;slot.lastX=x;slot.lastY=y;
      }return moved;
    }
    if(!touching&&slot.active){
      slot.ending=true;
      if(slot.targetId>=0){dispatchPointer(PointerEventType::TouchEnd,slot.targetId,slot.lastX,slot.lastY,false,pointer,false);
        if(ticket!=slot.ticket || retireInvalid())return true;
        if(!slot.dragged&&!slot.defaultPrevented)dispatchPointer(PointerEventType::Click,slot.targetId,slot.lastX,slot.lastY,false,pointer,false);
        if(ticket!=slot.ticket || retireInvalid())return true;
      }
      slot.active=false;slot.ending=false;slot.targetId=-1;slot.owner={};slot.dragged=false;slot.defaultPrevented=false;return true;
    }return false;
  }

  // Immediate-mode (no UI tree) path: synthesize the pointer event from the raw
  // touch and hand it straight to document-level listeners. No node target, no
  // tree walk — `document` sees every pointer event, exactly like the DOM.
  static bool dispatchToDocument(const Event &event)
  {
	  PointerEventType type;
	  switch (event.touchPhase) {
	  case TouchPhase::Down:
		  type = PointerEventType::TouchStart;
		  break;
	  case TouchPhase::Move:
		  type = PointerEventType::TouchMove;
		  break;
	  case TouchPhase::Up:
		  type = PointerEventType::TouchEnd;
		  break;
	  default:
		  return false;
	  }
	  PointerEvent pe{};
	  pe.type = type;
	  pe.button = type == PointerEventType::TouchMove ? -1 : 0;
	  pe.buttons = event.touching ? 1u : 0u;
	  pe.targetId = -1;
	  // Match the mounted-tree path and the public TypeScript surface:
	  // web-facing pointer ids start at 1, while internal touch slots start at 0.
	  const int webPointerId = event.pointerId + 1;
	  pe.pointerId = webPointerId;
	  pe.x = event.x;
	  pe.y = event.y;
	  pe.clientX = event.x;
	  pe.clientY = event.y;
	  pe.pageX = event.x;
	  pe.pageY = event.y;
	  pe.screenX = event.x;
	  pe.screenY = event.y;
	  pe.primary = event.pointerId == 0;
	  pe.touchesLength = event.touching ? 1 : 0;
	  pe.targetTouchesLength = event.touching ? 1 : 0;
	  pe.changedTouchesLength = 1;
	  pe.touches[0] = makeTouchPoint(-1, event.x, event.y, webPointerId);
	  pe.targetTouches[0] = pe.touches[0];
	  pe.changedTouches[0] = pe.touches[0];
	  return gea::embedded::ui::dispatchDocumentPointer(pe);
  }

  static bool dispatchPointer(PointerEventType type, int targetId, int x, int y, bool activeTouch, int pointerId = 1,
							  bool primary = true)
  {
	  PointerEvent event{};
	  event.type = type;
	  event.button = (type == PointerEventType::TouchStart || type == PointerEventType::TouchEnd || type == PointerEventType::Click) ? 0 : -1;
	  event.buttons = activeTouch ? 1u : 0u;
	  // Skip entirely when no node listens for this type — don't build the
	  // event payload or walk the tree. Apps that bind only touch* never pay
	  // for the paired pointer* dispatch (and vice versa).
	  if (!gea::embedded::ui::Tree::instance().hasListenersForType(event.typeName()))
		  return false;
	  event.targetId = targetId;
	  event.pointerId = pointerId;
	  event.x = x;
	  event.y = y;
	  event.clientX = x;
	  event.clientY = y;
	  event.pageX = x;
	  event.pageY = y;
	  event.screenX = x;
	  event.screenY = y;
	  event.primary = primary;
	  event.touchesLength = activeTouch ? 1 : 0;
	  event.targetTouchesLength = activeTouch ? 1 : 0;
	  event.changedTouchesLength = 1;
	  event.touches[0] = makeTouchPoint(targetId, x, y, pointerId);
	  event.targetTouches[0] = event.touches[0];
	  event.changedTouches[0] = event.touches[0];
	  gea::embedded::ui::Tree::instance().dispatchEvent(event);
	  return event.defaultPrevented;
  }

  static TouchPoint makeTouchPoint(int targetId, int x, int y, int identifier = 1)
  {
	  TouchPoint point{};
	  point.identifier = identifier;
	  point.target = EventTarget(targetId);
	  point.screenX = x;
	  point.screenY = y;
	  point.clientX = x;
	  point.clientY = y;
	  point.pageX = x;
	  point.pageY = y;
	  return point;
  }

  static int inputAncestor(int nodeId)
  {
	  auto &tree = gea::embedded::ui::Tree::instance();
	  for (int n = nodeId; n >= 0 && n < tree.nodeCount(); n = tree.node(n).parent) {
		  if (std::strcmp(tree.tagName(n), "input") == 0)
			  return n;
	  }
	  return -1;
  }

  bool dispatchEnabled_ = true;
  bool wasActive_ = false;
  int lastActivityMs_ = 0;
  bool activeDefaultPrevented_ = false;
  int lastX_ = 0;
  int lastY_ = 0;
  int startX_ = 0;
  int startY_ = 0;
  bool dragged_ = false;
  int activeTargetId_ = -1;
  gea::embedded::ui::NodeHandle primaryOwner_;
  unsigned int primaryTicket_=0;
  bool primaryQuarantined_=false,primaryEnding_=false;

  // Per-pointer state for non-primary (pointerId >= 1) simultaneous fingers.
  struct Secondary {
    gea::embedded::ui::NodeHandle owner;
    unsigned int ticket=0;
    bool quarantined=false,ending=false;
	  bool active = false;
	  bool dragged = false;
	  bool defaultPrevented = false;
	  int targetId = -1;
	  int startX = 0;
	  int startY = 0;
	  int lastX = 0;
	  int lastY = 0;
  };
  static constexpr int kMaxSecondaryPointers = 9;
  Secondary secondary_[kMaxSecondaryPointers]{};
};

}  // namespace

} // namespace gea::framework::events
namespace gea::embedded::ui {
void retireTouchInputForSubtrees(const InputRetirementRoots &removed,const InputRetirementRoots &retained)
{gea::framework::events::TouchEventDispatcher::instance().retireSubtrees(removed,retained);}
}

namespace gea::framework::events {
// Touch controllers report PANEL-NATIVE coordinates; the UI hit-tests in
// LOGICAL (orientation-rotated) space. Inverse of the display's
// panelForSourcePixel mapping. Applied to QUEUED events AND to the raw
// latest-move cache at consumption (the dispatcher overrides Move coords
// from that cache, and Up inherits the last Move — leaving those raw made
// rail taps land in dead zones on rotated apps).
// (When INJECTING test events, pass panel-frame coordinates: px = ly,
// py = nativeH-1-lx — injecting logical coords double-transforms.)
void TouchRuntime::transformTouchToLogical(int *x, int *y)
{
	using gea::framework::display::DisplayOrientation;
	namespace od = gea::framework::display::detail;
	const int nativeW = od::DisplayOrientationState::nativeWidth();
	const int nativeH = od::DisplayOrientationState::nativeHeight();
	const int px = *x;
	const int py = *y;
	switch (od::DisplayOrientationState::orientation()) {
	case DisplayOrientation::LandscapePrimary:
		// panel = {srcY, nativeH - 1 - srcX}  =>  src = {nativeH - 1 - py, px}
		*x = nativeH - 1 - py;
		*y = px;
		break;
	case DisplayOrientation::LandscapeSecondary:
		// panel = {nativeW - 1 - srcY, srcX}  =>  src = {py, nativeW - 1 - px}
		*x = py;
		*y = nativeW - 1 - px;
		break;
	case DisplayOrientation::PortraitSecondary:
		*x = nativeW - 1 - px;
		*y = nativeH - 1 - py;
		break;
	case DisplayOrientation::PortraitPrimary:
	default:
		break;
	}
}

void TouchRuntime::queueTouchEvent(TouchPhase phase, bool touching, int x, int y, int pointerId)
{
	TouchRuntime::transformTouchToLogical(&x, &y);
	TouchEventDispatcher::instance().queue(phase, touching, x, y, pointerId);
}

bool TouchRuntime::start()
{
	// Single-finger path (all controllers): primary pointer, id 0.
	gea::platform::touch::Touchscreen::setObserver(
		[](gea::platform::touch::Phase phase, bool touching, int x, int y) {
			TouchRuntime::queueTouchEvent(static_cast<TouchPhase>(phase), touching, x, y, 0);
		});
	// Multi-finger path (controllers that implement it, e.g. GT911): each finger
	// arrives with its index. A controller fires EITHER this or the single
	// observer, never both, so there's no double dispatch.
	gea::platform::touch::Touchscreen::setPointerObserver(
		[](gea::platform::touch::Phase phase, bool touching, int x, int y, int pointerId) {
			TouchRuntime::queueTouchEvent(static_cast<TouchPhase>(phase), touching, x, y, pointerId);
		});
	return gea::platform::touch::Touchscreen::init();
}

void TouchRuntime::poll(int nowMs)
{
	gea::platform::touch::Touchscreen::poll(nowMs);
}

void TouchRuntime::setDispatchEnabled(bool enabled)
{
	TouchEventDispatcher::instance().setDispatchEnabled(enabled);
}

bool TouchRuntime::dispatchEnabled()
{
	return TouchEventDispatcher::instance().dispatchEnabled();
}

void TouchRuntime::resetGestureState()
{
	TouchEventDispatcher::instance().resetGesture();
}

bool TouchRuntime::dispatchEvent(const Event &event)
{
	return TouchEventDispatcher::instance().dispatch(event);
}

bool TouchRuntime::gestureActiveWithin(int nowMs, int windowMs)
{
	return TouchEventDispatcher::instance().activeWithin(nowMs, windowMs);
}

}  // namespace gea::framework::events
