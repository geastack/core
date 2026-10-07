// SPDX-License-Identifier: Apache-2.0
#include "native_test_harness.h"
#include "graphics/font.h"
#include "event.h"
#include "events.h"
#include "ui/document.h"
#include "ui/debugger_picker.h"
#include "ui/node.h"
#include "ui/style.h"
#include <cassert>
#include <cstdio>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}

int main() {
  using namespace gea::embedded::test;
  using namespace gea::embedded::ui;
  using namespace gea::framework::events;
  resetNativeHost();
  setNativeDisplaySize(180, 120);
  setViewportMetrics(180, 120, 1.0);
  auto root = Document::instance().createView();
  root.style().width(180); root.style().height(120);
  auto button = Document::instance().createButton();
  button.style().setProperty("position", "absolute");
  button.style().left(20); button.style().top(20);
  button.style().width(60); button.style().height(60);
  auto child = Document::instance().createView();
  child.style().setProperty("position", "absolute");
  child.style().left(10); child.style().top(10);
  child.style().width(20); child.style().height(20);
  button.appendChild(child);
  root.appendChild(button);
  Document::instance().mount(root, 180, 120);
  auto &tree = Tree::instance();
  int appEvents = 0, clicks = 0;
  for (const auto *name : {"touchstart", "touchmove", "touchend"})
    tree.setEventListener(button.id(), name, [&](PointerEvent &) { ++appEvents; });
  tree.setEventListener(button.id(), "click", [&](PointerEvent &) { ++clicks; });
  auto touch = [&](TouchPhase phase, int pointer = 0, int x = 35, int y = 35) {
    Event event{};
    event.type = EventType::Touch; event.touchPhase = phase;
    event.touching = phase != TouchPhase::Up;
    event.x = x; event.y = y; event.pointerId = pointer;
    TouchRuntime::dispatchEvent(event);
  };
  auto &picker = debuggerPicker;
  const int expected = int(tree.node(child.id()).debugger_identity * 2 + 2);
  const auto nodes = tree.nodeCount();
  setNativeNowMs(0);
  picker.begin(1);
  touch(TouchPhase::Down);
  assert(!picker.active && picker.selectedId() == expected);
  // Chrome may cancel on selection before the finger comes off the screen.
  picker.cancel(1);
  touch(TouchPhase::Move);
  touch(TouchPhase::Down, 1);
  touch(TouchPhase::Up);
  touch(TouchPhase::Up, 1);
  assert(appEvents == 0 && clicks == 0 && picker.held == 0);
  touch(TouchPhase::Down); touch(TouchPhase::Up);
  assert(appEvents == 2 && clicks == 1);
  assert(tree.nodeCount() == nodes);

  // Arming while an app gesture is down cancels its future activation.
  touch(TouchPhase::Down);
  const int beforeArm = appEvents;
  picker.begin(2);
  touch(TouchPhase::Up);
  assert(appEvents == beforeArm && clicks == 1);
  assert(picker.active);
  assert(picker.point(2, 35, 35, false));
  assert(picker.hoverId() == expected && debuggerOverlay.slot == child.id());
  assert(picker.point(2, 35, 35, true));
  assert(picker.selectedId() == expected && !picker.active);
  picker.renew(2); picker.begin(2);
  assert(!picker.active); // Delayed keep-alives cannot rearm a completed pick.

  picker.begin(3); picker.begin(4);
  picker.cancel(3); picker.point(3, 35, 35, true);
  assert(picker.active && picker.token == 4 && !picker.selectedId());
  picker.cancel(4);
  touch(TouchPhase::Down); touch(TouchPhase::Up);
  assert(clicks == 2);

  picker.begin(5);
  setNativeNowMs(2000); picker.renew(5);
  setNativeNowMs(3000); assert(picker.capturesInput());
  setNativeNowMs(4500); assert(!picker.capturesInput());
  touch(TouchPhase::Down); touch(TouchPhase::Up);
  assert(clicks == 3 && debuggerOverlay.slot == -1);

  // A lost release cannot leave controls permanently blocked.
  picker.begin(6); touch(TouchPhase::Down);
  touch(TouchPhase::Down); touch(TouchPhase::Up);
  assert(clicks == 4 && picker.held == 0);

  // Use the actual engine hit test for preview and hardware, including transforms.
  child.style().setProperty("transform", "translateX(50px)");
  Document::instance().refresh(root, 180, 120);
  picker.begin(7); picker.point(7, 85, 35, true);
  assert(picker.selectedId() == expected);
  child.style().setProperty("display", "none");
  Document::instance().refresh(root, 180, 120);
  picker.begin(8); picker.point(8, 35, 35, true);
  assert(picker.selectedId() != expected);
  child.style().setProperty("display", "block");
  child.style().removeProperty("transform");
  Document::instance().refresh(root, 180, 120);
  picker.begin(9); picker.point(9, 35, 35, true);
  assert(picker.selectedId() == expected);
  tree.removeNode(child.id());
  auto replacement = Document::instance().createView();
  button.appendChild(replacement);
  assert(!picker.selectedId()); // Recycled slots never select a different lifetime.
  std::puts("[debugger_picker] ALL PASS");
}
