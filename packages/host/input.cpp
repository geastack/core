// SPDX-License-Identifier: Apache-2.0
#include "input.h"

#include <atomic>

namespace gea::framework::input {

namespace {

// One-shot "back button pressed" flag. Atomic so the platform button task
// (priority 10 on a FreeRTOS core) can race-set without locks against the
// app's rAF tick (priority 23 on a different core).
std::atomic_bool g_back_pending{false};
std::atomic_int g_rotary_delta{0};

// Pending hardware key presses. A tiny ring keeps fast double-presses from
// coalescing (the single consumer is the frame loop, once per frame); a
// burst deeper than the ring just overwrites the oldest press.
constexpr unsigned kKeyQueueSize = 8;
std::atomic_uint g_key_queue[kKeyQueueSize]{};
std::atomic<unsigned> g_key_write{0};
std::atomic<unsigned> g_key_read{0};

} // namespace

void pressBackButton() {
  g_back_pending.store(true, std::memory_order_release);
}

bool consumeBackButton() {
  return g_back_pending.exchange(false, std::memory_order_acq_rel);
}

void queueRotaryDelta(int delta) {
  if (delta == 0)
    return;
  g_rotary_delta.fetch_add(delta, std::memory_order_acq_rel);
}

int consumeRotaryDelta() {
  return g_rotary_delta.exchange(0, std::memory_order_acq_rel);
}

void queueKey(int keyCode, bool pressed) {
  if (keyCode <= 0)
    return;
  const unsigned slot =
      g_key_write.fetch_add(1, std::memory_order_acq_rel) % kKeyQueueSize;
  g_key_queue[slot].store((static_cast<unsigned>(keyCode) << 1) |
                              unsigned(pressed),
                          std::memory_order_release);
}

void queueKeyDown(int keyCode) { queueKey(keyCode, true); }

void queueKeyUp(int keyCode) { queueKey(keyCode, false); }

bool consumeKeyEvent(HardwareKeyEvent &event) {
  const unsigned write = g_key_write.load(std::memory_order_acquire);
  unsigned read = g_key_read.load(std::memory_order_acquire);
  // Keep only the newest ring's worth of reservations after overflow. Without
  // this, overwritten slots leave holes that can never be published again.
  if (write - read > kKeyQueueSize) {
    read = write - kKeyQueueSize;
    g_key_read.store(read, std::memory_order_release);
  }
  if (read != write) {
    const unsigned slot = read % kKeyQueueSize;
    const unsigned code =
        g_key_queue[slot].exchange(0, std::memory_order_acq_rel);
    // A producer reserves its ticket before publishing the key. Leave that
    // reservation pending so the next frame can consume it after publication.
    // Zero key codes never reserve a ticket in queueKeyDown().
    if (code == 0)
      return false;
    g_key_read.store(read + 1, std::memory_order_release);
    event = {static_cast<int>(code >> 1), (code & 1) != 0};
    return true;
  }
  return false;
}

int consumeKeyDown() {
  HardwareKeyEvent event{};
  while (consumeKeyEvent(event)) {
    if (event.pressed)
      return event.keyCode;
  }
  return 0;
}

} // namespace gea::framework::input
