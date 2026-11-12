#include "gea_runtime.h"
#include "runtime.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#if __has_include(<sanitizer/lsan_interface.h>)
#include <sanitizer/lsan_interface.h>
#endif
#include <vector>

namespace {
bool locked = false;
bool callbackActive = false;
bool collectedInsideCallback = false;
std::atomic<unsigned> callbackCount = 0;
unsigned collections = 0;
bool changed = true;
bool focused = false;
bool stopKey = false;
int rotary = 0;
int keyCount = 0;
std::atomic<int> postedFrames = 0;
std::atomic<unsigned> frames = 0;
std::atomic<unsigned> completedFrames = 0;
std::atomic<unsigned> moveDiscards = 0;
std::atomic<unsigned> gestureResets = 0;
std::atomic<unsigned> storageFlushes = 0;
std::atomic<bool> forceDrag = false;
std::atomic<bool> framePending = false;
std::atomic<bool> queueReady = false;
std::atomic<unsigned> received = 0;
std::mutex testMutex;
std::condition_variable testChanged;
bool blockCallback = false;
bool callbackBlocked = false;
bool blockFrameTail = false;
bool frameTailBlocked = false;
bool blockReceive = false;
bool receiveBlocked = false;
bool stopRuntime = false;
struct RuntimeStopped {};

void blockAt(bool &enabled, bool &entered) {
  std::unique_lock lock(testMutex);
  if (!enabled)
    return;
  entered = true;
  testChanged.notify_all();
  testChanged.wait(lock, [&] { return !enabled; });
}

template <class Predicate> void waitFor(Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline &&
           "runtime boundary timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void waitBlocked(bool &entered) {
  std::unique_lock lock(testMutex);
  assert(testChanged.wait_for(lock, std::chrono::seconds(2),
                              [&] { return entered; }));
}

void release(bool &enabled) {
  std::lock_guard lock(testMutex);
  enabled = false;
  testChanged.notify_all();
}

struct Cycle {
  static inline int live = 0;
  gea::Ref<Cycle> next;
  Cycle() { ++live; }
  ~Cycle() {
    --live;
    ++collections;
    collectedInsideCallback |= callbackActive;
  }
  friend void geaTraceRefs(const Cycle &value,
                           gea::detail::RefVisitor &visitor) {
    gea::detail::traceRefs(value.next, visitor);
  }
};

void generatedCallback() {
  assert(locked);
  callbackActive = true;
  ++callbackCount;
  blockAt(blockCallback, callbackBlocked);
  {
    // A two-object cycle avoids the runtime's immediate self-cycle release.
    auto left = gea::makeRef<Cycle>();
    auto right = gea::makeRef<Cycle>();
    left->next = right;
    right->next = left;
  }
  // This is the safepoint makeRef reaches under allocation pressure. A host
  // callback is not a quiescent boundary: generated transient environments and
  // the dispatcher's borrowed handler references are still on the stack.
  gea::collectCyclesIfNeeded();
  callbackActive = false;
}
} // namespace

extern "C" void __attribute__((weak)) gea_cycle_collection_defer_begin() {
  ++gea::detail::cycleState().deferDepth;
}
extern "C" void __attribute__((weak)) gea_cycle_collection_defer_end() {
  assert(gea::detail::cycleState().deferDepth > 0);
  --gea::detail::cycleState().deferDepth;
  gea::collectCyclesIfNeeded();
}

namespace gea::framework {
bool Runtime::boot(const RuntimeOptions &) { return true; }
namespace app {
enum class ApplicationFramePhase { Idle, CycleCollection };
ApplicationFramePhase phase = ApplicationFramePhase::Idle;
ApplicationFramePhase applicationFramePhaseRead() { return phase; }
void applicationFramePhaseSet(ApplicationFramePhase value) { phase = value; }
struct Application {
  static void frame(int) {
    // Application::frame owns this scope even for the direct-canvas runtime,
    // which intentionally has no input/poll prelude around it.
    gea_cycle_collection_defer_begin();
    generatedCallback();
    gea_cycle_collection_defer_end();
  }
};
} // namespace app
namespace events {
enum class EventType { Touch, Frame, Timeout, SettingsToggle };
enum class TouchPhase { Down, Move, Up };
enum class PointerEventType { KeyDown, KeyUp };
struct Event {
  EventType type;
  TouchPhase touchPhase;
  int x = 0;
  int y = 0;
};
struct PointerEvent {
  PointerEventType type;
  int keyCode = 0;
  std::string key;
  std::string code;
  int targetId = -1;
  int currentTargetId = -1;
  bool bubbles = false;
  bool cancelable = false;
  bool propagationStopped = false;
};
struct TouchRuntime {
  static bool dispatchEvent(const Event &) {
    generatedCallback();
    return changed;
  }
  static void poll(int) { generatedCallback(); }
  static bool gestureActiveWithin(int, int) { return forceDrag.load(); }
  static void resetGestureState() { ++gestureResets; }
};
} // namespace events
namespace input {
struct HardwareKeyEvent {
  bool pressed;
  int keyCode;
  int button = -1;
};
const char *buttonCode(int) { return ""; }
const char *buttonKey(int) { return ""; }
int consumeRotaryDelta() {
  const int value = rotary;
  rotary = 0;
  return value;
}
bool consumeKeyEvent(HardwareKeyEvent &event) {
  if (!keyCount)
    return false;
  event = {keyCount == 2, 32};
  --keyCount;
  return true;
}
} // namespace input
struct SettingsSurface {
  static SettingsSurface &instance() {
    static SettingsSurface value;
    return value;
  }
  bool active() { return false; }
  void handleTap(int, int) {}
  void toggle() {}
};
namespace services {
std::deque<events::Event> queuedEvents;
struct AppState {
  static void lock() {
    assert(!locked);
    locked = true;
  }
  static void unlock() {
    assert(locked);
    locked = false;
  }
};
struct FrameScheduler {
  struct FrameCallbacks {
    void (*frame)(int, void *);
  };
  static bool vsyncDriven() { return false; }
  static bool sendEvent(const events::Event &event, int) {
    if (event.type == events::EventType::Frame) {
      ++postedFrames;
      framePending = true;
    }
    if (!queueReady.load())
      return false;
    std::lock_guard lock(testMutex);
    if (queuedEvents.size() == 8)
      return false;
    queuedEvents.push_back(event);
    testChanged.notify_all();
    return true;
  }
  static bool takeCatchUpRequest() { return false; }
  static int nowMs() { return 0; }
  static bool drainFramesAndCheckInput() {
    std::lock_guard lock(testMutex);
    while (!queuedEvents.empty() &&
           queuedEvents.front().type == events::EventType::Frame) {
      queuedEvents.pop_front();
      framePending = false;
    }
    return !queuedEvents.empty();
  }
  static bool receiveEvent(events::Event *event) {
    if (!queueReady.load())
      return false;
    {
      std::unique_lock lock(testMutex);
      testChanged.wait(lock,
                       [] { return stopRuntime || !queuedEvents.empty(); });
      if (stopRuntime)
        throw RuntimeStopped{};
      *event = queuedEvents.front();
      queuedEvents.pop_front();
    }
    blockAt(blockReceive, receiveBlocked);
    ++received;
    return true;
  }
  static void runFrame(const FrameCallbacks &callbacks) {
    ++frames;
    framePending = false;
    AppState::lock();
    callbacks.frame(0, nullptr);
    AppState::unlock();
    blockAt(blockFrameTail, frameTailBlocked);
    ++completedFrames;
  }
};
struct BatteryService {
  static void poll(int) {}
};
} // namespace services
} // namespace gea::framework
namespace gea::platform::touch {
struct Touchscreen {
  static void consumeLatestMove(int *, int *) { ++moveDiscards; }
};
} // namespace gea::platform::touch
namespace gea::embedded::ui {
void dispatchDocumentRotary(int) { generatedCallback(); }
void dispatchDocumentKeyDown(gea::framework::events::PointerEvent &) {
  generatedCallback();
}
void dispatchDocumentKeyUp(gea::framework::events::PointerEvent &) {
  generatedCallback();
}
struct Tree {
  static Tree &instance() {
    static Tree value;
    return value;
  }
  int activeInputId() { return focused ? 7 : -1; }
  void dispatchEvent(gea::framework::events::PointerEvent &event) {
    generatedCallback();
    event.propagationStopped = stopKey;
  }
};
// The engine's root key dispatch: the focused target first, then the document
// unless the target stopped propagation.
bool dispatchRootKeyEvent(gea::framework::events::PointerEvent &event,
                          bool = false) {
  if (event.targetId >= 0)
    Tree::instance().dispatchEvent(event);
  if (event.propagationStopped)
    return true;
  if (event.type == gea::framework::events::PointerEventType::KeyDown)
    dispatchDocumentKeyDown(event);
  else
    dispatchDocumentKeyUp(event);
  return true;
}
struct StyleSheet {
  static StyleSheet &instance() {
    static StyleSheet value;
    return value;
  }
  void startCssAnimations(unsigned) {}
};
struct TestDocument {
  static TestDocument &instance() {
    static TestDocument value;
    return value;
  }
  void refreshMountedIfDirty() {}
};
} // namespace gea::embedded::ui
namespace gea::css {
struct DeclarativeAnimations {
  static void scanAndStart(unsigned) {}
};
struct AnimationEngine {
  static AnimationEngine &instance() {
    static AnimationEngine value;
    return value;
  }
  void tick(unsigned) {}
};
} // namespace gea::css
namespace gea::host {
struct TestStorage {
  void flushPending() { ++storageFlushes; }
};
TestStorage Storage;
} // namespace gea::host

#define GEA_EMBEDDED_CONTINUOUS_TOUCH_FRAMES 1
#define Document TestDocument
namespace gea::framework {
#include "runtime_input_dispatch.inc"
}
#undef Document

void testInputCollection() {
  using namespace gea::framework;
  gea::configureAutomaticCycleCollection(std::chrono::milliseconds(0), 0, 1);
  const events::Event touch{events::EventType::Touch, events::TouchPhase::Up};
  const events::Event frame{events::EventType::Frame, events::TouchPhase::Up};
  const auto verify = [] {
    assert(!locked && !callbackActive);
    assert(gea::detail::cycleState().deferDepth == 0);
    assert(!collectedInsideCallback &&
           "input callback reached an unsafe collection boundary");
    assert(Cycle::live == 0 &&
           "input garbage must collect after dispatch returns");
    assert(app::phase == app::ApplicationFramePhase::Idle);
  };

  // Two successive preset-like callbacks both allocate; the second is not
  // protected by a later frame's scope merely because it requests that frame.
  dispatch_event(touch, {});
  verify();
  dispatch_event(touch, {});
  verify();
  assert(callbackCount == 2 && postedFrames == 2 && collections == 4);

  changed = false;
  dispatch_event(touch, {});
  verify();
  assert(postedFrames == 2);

  // A surrounding callback scope still owns its lifetime after nested input.
  gea_cycle_collection_defer_begin();
  const auto before = collections;
  dispatch_event(touch, {});
  assert(gea::detail::cycleState().deferDepth == 1 && collections == before);
  assert(Cycle::live == 2);
  gea_cycle_collection_defer_end();
  verify();

  // Poll, rotary, and key handlers execute before Application::frame and need
  // an outer scope too. Cover focused input, document handlers, and
  // stopPropagation.
  for (const bool inputFocused : {false, true}) {
    for (const bool stop : {false, true}) {
      focused = inputFocused;
      stopKey = stop;
      rotary = 1;
      keyCount = 2;
      const auto count = callbackCount.load();
      dispatch_event(frame, {});
      verify();
      assert(callbackCount - count == (focused && !stopKey ? 7U : 5U));
    }
  }
  std::puts("runtime input collection boundaries: passed");
}

void testUiPause() {
  using namespace gea::framework;
  assert(!Runtime::uiPauseRequested() && !Runtime::uiPaused());
  Runtime::requestUiPause(true);
  assert(Runtime::uiPauseRequested());
#if GEA_EMBEDDED_NO_DISPLAY
  assert(Runtime::uiPaused());
  Runtime::run({});
  Runtime::requestUiPause(false);
  assert(!Runtime::uiPaused());
  std::puts("headless runtime UI pause: passed");
#else
  // A pre-boot request cannot acknowledge itself. The queue does not exist
  // yet, so correctness cannot depend on the wake event reaching it.
  assert(!Runtime::uiPaused());
  const auto firstFrame = frames.load();
  const auto firstCallback = callbackCount.load();
  queueReady = true;
  std::thread runtime([] {
#if __has_include(<sanitizer/lsan_interface.h>)
    // A runtime thread keeps its cycle state and pool chunks for its whole
    // life by design; only this test lets one exit. Live cycles are asserted
    // directly (Cycle::live) after the join instead.
    __lsan::ScopedDisabler runtimeThreadState;
#endif
    gea::configureAutomaticCycleCollection(std::chrono::milliseconds(0), 0, 1);
    try {
      Runtime::run({});
      assert(false && "runtime unexpectedly returned");
    } catch (const RuntimeStopped &) {
    }
  });
  waitFor([] { return Runtime::uiPaused(); });
  assert(!locked && frames == firstFrame && callbackCount == firstCallback);

  const auto discarded = received.load();
  const auto post = [](events::EventType type,
                       events::TouchPhase phase = events::TouchPhase::Up) {
    assert(services::FrameScheduler::sendEvent({type, phase}, 0));
  };
  post(events::EventType::Touch, events::TouchPhase::Move);
  post(events::EventType::Frame);
  post(events::EventType::SettingsToggle);
  waitFor([&] { return received >= discarded + 3; });
  assert(callbackCount == firstCallback && frames == firstFrame);
  assert(framePending &&
         "discarding a Frame leaves the scheduler's coalescing flag set");
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) ||                            \
    !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
  assert(moveDiscards >= 2 && gestureResets > 0);
#endif

  // Resumption must reset coalescing without relying on another timer post.
  Runtime::requestUiPause(false);
  assert(!Runtime::uiPaused());
  waitFor([&] { return completedFrames > firstFrame && !framePending.load(); });

  // Request a pause while a callback holds AppState, then retain the fake
  // renderer in its tail after unlock. Neither point is a safe acknowledgement.
  // An active drag would otherwise keep this event in its catch-up loop.
  {
    std::lock_guard lock(testMutex);
    blockCallback = true;
    blockFrameTail = true;
    callbackBlocked = false;
    frameTailBlocked = false;
  }
  forceDrag = true;
  const auto frame0 = frames.load();
  const auto flush0 = storageFlushes.load();
  post(events::EventType::Frame);
  waitBlocked(callbackBlocked);
  Runtime::requestUiPause(true);
  assert(locked && !Runtime::uiPaused());
  release(blockCallback);
  waitBlocked(frameTailBlocked);
  assert(!locked && !Runtime::uiPaused());
  release(blockFrameTail);
  waitFor([] { return Runtime::uiPaused(); });
  assert(!locked && frames == frame0 + 1);
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) ||                            \
    !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
  assert(storageFlushes == flush0 + 1);
#endif
  forceDrag = false;

  // Hold the owner after a queue read and issue resume -> pause. The previous
  // acknowledgement must not satisfy the new generation. Fill the queue to
  // also prove correctness when the second wake event cannot be enqueued.
  {
    std::lock_guard lock(testMutex);
    blockReceive = true;
    receiveBlocked = false;
  }
  Runtime::requestUiPause(false);
  waitBlocked(receiveBlocked);
  const auto callback0 = callbackCount.load();
  for (unsigned i = 0; i != 8; ++i)
    post(events::EventType::Timeout);
  Runtime::requestUiPause(true);
  assert(!Runtime::uiPaused());
  release(blockReceive);
  waitFor([] { return Runtime::uiPaused(); });
  assert(callbackCount == callback0 && !locked);

#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) ||                            \
    !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
  // The same boundary applies to touch callbacks, not only frame callbacks.
  // Drain a synchronous frame after resume before arranging the touch gate.
  const auto resumedFrame = completedFrames.load();
  Runtime::requestUiPause(false);
  waitFor([&] { return completedFrames > resumedFrame; });
  // A pause acknowledgement is also a convenient, actual-owner completion
  // fence for the preceding refresh callback and its storage flush.
  Runtime::requestUiPause(true);
  waitFor([] { return Runtime::uiPaused(); });
  const auto refreshed = completedFrames.load();
  Runtime::requestUiPause(false);
  waitFor([&] { return completedFrames > refreshed; });
  {
    std::lock_guard lock(testMutex);
    blockCallback = true;
    callbackBlocked = false;
  }
  changed = false;
  post(events::EventType::Touch);
  waitBlocked(callbackBlocked);
  Runtime::requestUiPause(true);
  assert(locked && !Runtime::uiPaused());
  release(blockCallback);
  waitFor([] { return Runtime::uiPaused(); });
  assert(!locked);
#endif

  {
    std::lock_guard lock(testMutex);
    stopRuntime = true;
    testChanged.notify_all();
  }
  runtime.join();
  assert(!locked && !collectedInsideCallback && Cycle::live == 0);
  queueReady = false;
  Runtime::requestUiPause(false);
  assert(!Runtime::uiPaused());
  std::puts("runtime UI pause boundaries, wake-up and resume: passed");
#endif
}

int main() {
#if !GEA_EMBEDDED_NO_DISPLAY &&                                                \
    (!defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) ||                           \
     !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT)
  testInputCollection();
#else
  gea::configureAutomaticCycleCollection(std::chrono::milliseconds(0), 0, 1);
#endif
  testUiPause();
}
