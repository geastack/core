// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(GEA_NATIVE_DEBUGGER) && GEA_NATIVE_DEBUGGER
#include "debugger_overlay.h"
#include "events.h"
#include "event.h"
#include "services/frame_scheduler.h"

namespace gea::embedded::ui {
// Shared by every retained-tree target. Calls run under the app/render lock;
// transports own a token and renew its lease while their inspector is connected.
struct DebuggerPicker {
  unsigned token = 0, revision = 0;
  bool active = false;
  std::int64_t deadline = 0;
  std::uint32_t held = 0;
  int hovered = -1, selected = -1;
  unsigned hoverIdentity = 0, selectedIdentity = 0;

  static int now() { return gea::framework::services::FrameScheduler::nowMs(); }
  static int identity(int slot, unsigned lifetime) {
    auto &state = treeState();
    return slot >= 0 && slot < state.nodeCount && state.nodeActive[slot] &&
           state.nodes[slot].debugger_identity == lifetime &&
           Tree::instance().containsNode(state.mountedRoot, slot)
             ? int(lifetime * 2 + 2) : 0;
  }
  int hoverId() const { return identity(hovered, hoverIdentity); }
  int selectedId() const { return identity(selected, selectedIdentity); }
  void expire() {
    if (active && std::int64_t(now()) >= deadline) cancel(token);
  }
  void outline(int slot) {
    debuggerOverlay.set(slot, gea::framework::graphics::pixel::nativeColor(0, 200, 255));
    debuggerOverlay.deadline = std::int64_t(now()) + 2500;
  }
  void begin(unsigned owner) {
    expire();
    if (token == owner) return; // A delayed renewal must never rearm a picked node.
    token = owner;
    active = true;
    selected = hovered = -1;
    selectedIdentity = hoverIdentity = 0;
    deadline = std::int64_t(now()) + 2500;
    Tree::instance().resetInput();
    gea::framework::events::TouchRuntime::resetGestureState();
    outline(-1);
  }
  void renew(unsigned owner) {
    expire();
    if (token == owner && active) {
      deadline = std::int64_t(now()) + 2500;
      if (hoverId()) outline(hovered);
    }
  }
  void cancel(unsigned owner) {
    if (token != owner) return;
    active = false;
    selected = hovered = -1;
    selectedIdentity = hoverIdentity = 0;
    outline(-1);
    // Keep captured fingers until release, including cancellation mid-gesture.
  }
  bool point(unsigned owner, int x, int y, bool select) {
    expire();
    if (!active || owner != token) return false;
    hovered = Tree::instance().hitTestNode(x, y);
    hoverIdentity = hovered >= 0 ? treeState().nodes[hovered].debugger_identity : 0;
    outline(hovered);
    if (select && hoverId()) {
      selected = hovered;
      selectedIdentity = hoverIdentity;
      ++revision;
      active = false;
    }
    return true;
  }
  bool capturesInput() { expire(); return active || held; }
  bool consume(gea::framework::events::TouchPhase phase, int x, int y, int pointer) {
    using gea::framework::events::TouchPhase;
    // capturesInput() has already expired the lease before coordinates are
    // consumed. Do not switch routing after resolving the primary move cache.
    // Hardware controllers use IDs 0..9. Keep a bounded mask, even for a
    // malformed target event; never let inspection leak an application gesture.
    const unsigned bit = 1u << unsigned(pointer >= 0 && pointer < 32 ? pointer : 31);
    if (phase == TouchPhase::Down) {
      held &= ~bit; // Recover a missing release on the next fresh press.
      if (!active && !held) return false;
      held |= bit;
      if (active) point(token, x, y, true);
      return true;
    }
    if (!active && !held) return false;
    if (phase == TouchPhase::Up) held &= ~bit;
    else {
      held |= bit;
      if (active) point(token, x, y, false);
    }
    return true;
  }
};
inline DebuggerPicker debuggerPicker;
} // namespace gea::embedded::ui
#endif
