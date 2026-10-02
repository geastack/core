#include "host/worker.h"
#include "host/websocket.h"
#include "host/timers.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace gea::host;
using namespace gea::host::workers;
using namespace std::chrono_literals;

static std::atomic<int> realmEntries{0};
static const bool registeredBeforeMain = [] {
  registerModule("static-worker.js", [] {});
  setRealmRunner([](std::function<void()> body) {
    ++realmEntries;
    body();
  });
  return true;
}();

static bool waitFor(const std::function<bool()> &predicate) {
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
  return predicate();
}
struct CloseOnRelease {
  MessagePort port;
  ~CloseOnRelease() { port.close(); }
};
int main() {
  assert(registeredBeforeMain);
  auto staticWorker = Worker::create("static-worker.js");
  staticWorker.terminate();
  for (bool replace : {false, true}) {
    MessageChannel channel;
    auto capture = std::make_shared<CloseOnRelease>();
    capture->port = channel.port1;
    std::weak_ptr<CloseOnRelease> released = capture;
    channel.port1.setOnMessage([capture](MessageEvent &) {});
    capture.reset();
    // Releasing the last handler capture can reenter close(); native locks must
    // not surround that destruction, whether replacing or closing the handler.
    if (replace) channel.port1.setOnMessage([](MessageEvent &) {});
    else channel.port1.close();
    assert(released.expired());
  }
  {
    MessageChannel transport;
    MessageChannel transferred;
    auto capture = std::make_shared<CloseOnRelease>();
    capture->port = transport.port2;
    std::weak_ptr<CloseOnRelease> released = capture;
    transferred.port1.setOnMessage([capture](MessageEvent &) {});
    capture.reset();
    // Discarding an old listener during transfer must not destroy its captures
    // while holding the destination queue lock either.
    transport.port1.postMessage("port", {transferred.port1});
    assert(transferred.port1.detached() && released.expired());
  }
  std::atomic<int> delivered{0};
  std::atomic<int> ticks{0};
  std::atomic<unsigned> socketHandle{0};
  std::atomic<bool> ready{false};
  std::weak_ptr<int> callbackLifetime;
  registerModule("worker.js", [&] {
    assert(!Context::current()->isMain());
    const auto handle = websocket::create_handle("ws://worker");
    websocket::test_set_message_callback(handle, [&](const std::string &data) {
      assert(!Context::current()->isMain());
      if (data == "audio") delivered++;
    });
    socketHandle = handle;
    setInterval([&] { ticks++; }, 2);
    self().setOnMessage([&](MessageEvent &event) {
      if (!event.ports.empty()) {
        auto port = event.ports[0];
        auto lifetime = std::make_shared<int>(1);
        callbackLifetime = lifetime;
        // A native callback can capture its own port through a compiled object.
        // Context teardown must break that opaque ownership cycle on its owner.
        port.setOnMessage([&, port, lifetime](MessageEvent &audio) {
          assert(!port.detached() && *lifetime == 1);
          assert(std::get<MessageData::Bytes>(audio.data.value).size() == 1920);
          delivered++;
        });
      }
      self().postMessage(std::move(event.data));
    });
    ready = true;
  });
  auto worker = Worker::create("worker.js");
  assert(waitFor([&] { return ready.load(); }));
  assert(realmEntries >= 1);
  int responses = 0;
  worker.setOnMessage([&](MessageEvent &event) {
    assert(Context::current()->isMain());
    assert(std::get<std::string>(event.data.value) == "connect");
    responses++;
  });
  MessageChannel audio;
  audio.port2.setOnMessage([](MessageEvent &) { assert(false && "stale sender callback executed"); });
  audio.port1.postMessage(MessageData::Bytes(1920, 7));
  auto alias = audio.port2;
  worker.postMessage("connect", {audio.port2});
  assert(alias.detached());
  for (int i = 0; i < 50; ++i) {
    audio.port1.postMessage(MessageData::Bytes(1920, 42));
    websocket::test_inject_message(socketHandle, "audio");
  }
  // Main/UI is entirely asleep, both native message and WebSocket paths proceed.
  std::this_thread::sleep_for(300ms);
  assert(delivered == 101);
  assert(ticks >= 2);
  assert(responses == 0);
  Context::main()->runPending();
  assert(responses == 1);
  worker.terminate();
  std::this_thread::sleep_for(20ms);
  const int stoppedTicks = ticks;
  std::this_thread::sleep_for(20ms);
  assert(ticks == stoppedTicks);
  assert(callbackLifetime.expired());

  MessageChannel invalid;
  bool rejected = false;
  try { invalid.port1.postMessage("bad", {invalid.port1}); }
  catch (const std::exception &) { rejected = true; }
  assert(rejected && !invalid.port1.detached());
  MessageChannel transfer;
  rejected = false;
  try { invalid.port1.postMessage("bad", {transfer.port1, transfer.port1}); }
  catch (const std::exception &) { rejected = true; }
  assert(rejected && !transfer.port1.detached());
  MessageChannel bounded;
  rejected = false;
  try { bounded.port1.postMessage(MessageData::Bytes(2 * 1024 * 1024 + 1)); }
  catch (const std::exception &) { rejected = true; }
  assert(rejected);
  std::puts("worker runtime: independent message/WebSocket/timers, transfer, bounds, teardown OK");
}
