// SPDX-License-Identifier: Apache-2.0
// Include the queue implementation to pause a producer deterministically
// between ticket reservation and publication, without production test hooks.
#include "../../host/input.cpp"

#include <cassert>
#include <climits>
#include <latch>
#include <thread>

int main() {
  using namespace gea::framework::input;

  std::latch reserved(1);
  std::latch publish(1);
  std::thread producer([&] {
    const unsigned slot =
        g_key_write.fetch_add(1, std::memory_order_acq_rel) % kKeyQueueSize;
    reserved.count_down();
    publish.wait();
    g_key_queue[slot].store((37u << 1) | 1u, std::memory_order_release);
  });
  reserved.wait();
  // A second producer can publish before the oldest reservation is ready.
  queueKeyDown(39);
  assert(consumeKeyDown() == 0);
  assert(consumeKeyDown() == 0);
  publish.count_down();
  producer.join();
  assert(consumeKeyDown() == 37);
  assert(consumeKeyDown() == 39);
  assert(consumeKeyDown() == 0);

  HardwareKeyEvent event{};
  queueKeyDown(37);
  queueKeyUp(37);
  queueKeyDown(39);
  queueKeyUp(39);
  assert(consumeKeyEvent(event) && event.keyCode == 37 && event.pressed);
  assert(consumeKeyEvent(event) && event.keyCode == 37 && !event.pressed);
  assert(consumeKeyEvent(event) && event.keyCode == 39 && event.pressed);
  assert(consumeKeyEvent(event) && event.keyCode == 39 && !event.pressed);
  assert(!consumeKeyEvent(event));
  queueKeyUp(INT_MAX);
  assert(consumeKeyEvent(event) && event.keyCode == INT_MAX && !event.pressed);
  for (int index = 0; index < 12; ++index) {
    if (index % 2 == 0)
      queueKeyDown(index + 1);
    else
      queueKeyUp(index + 1);
  }
  for (int index = 4; index < 12; ++index) {
    assert(consumeKeyEvent(event) && event.keyCode == index + 1 &&
           event.pressed == (index % 2 == 0));
  }
  assert(!consumeKeyEvent(event));

  queueKeyUp(37);
  queueKeyDown(39);
  assert(consumeKeyDown() == 39);

  // Overflow discards the oldest keys and preserves the newest eight in order.
  for (int key = 1; key <= 12; ++key)
    queueKeyDown(key);
  for (int key = 5; key <= 12; ++key)
    assert(consumeKeyDown() == key);
  assert(consumeKeyDown() == 0);

  // An ignored zero must not create an unpublished reservation that stalls
  // the queue; the next real press remains immediately consumable.
  queueKeyDown(0);
  queueKeyDown(27);
  assert(consumeKeyDown() == 27);
  assert(consumeKeyDown() == 0);
}
