// SPDX-License-Identifier: Apache-2.0
#include "input.h"

#include <atomic>
#include <cstring>

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
// The registered button behind each slot's key, as id + 1 (0 = a bare key
// code). Written before the slot's code is published, read after it is taken.
std::atomic_int g_key_button[kKeyQueueSize]{};

// Board button table. Entries are only ever appended, each one fully written
// before the count that exposes it is published, so readers need no lock.
struct ButtonEntry {
  const char *code = "";
  const char *key = "";
  int keyCode = 0;
  int pin = -1;
};
ButtonEntry g_buttons[kMaxButtons];
std::atomic_bool g_button_pressed[kMaxButtons]{};
std::atomic_int g_button_count{0};
std::atomic_flag g_button_register_lock = ATOMIC_FLAG_INIT;

// A button with no key alias still needs a nonzero ticket in the queue (zero
// marks an unpublished slot). The dispatcher reports the button's own
// keyCode, 0, not this.
constexpr int kUnaliasedButtonTicket = 0xFFFF;

bool validButton(int id) {
  return id >= 0 && id < g_button_count.load(std::memory_order_acquire);
}

void queueKeyFor(int keyCode, bool pressed, int button) {
  if (keyCode <= 0)
    return;
  const unsigned slot =
      g_key_write.fetch_add(1, std::memory_order_acq_rel) % kKeyQueueSize;
  g_key_button[slot].store(button + 1, std::memory_order_relaxed);
  g_key_queue[slot].store((static_cast<unsigned>(keyCode) << 1) |
                              unsigned(pressed),
                          std::memory_order_release);
}

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

void queueKeyDown(int keyCode) { queueKeyFor(keyCode, true, -1); }

void queueKeyUp(int keyCode) { queueKeyFor(keyCode, false, -1); }

int registerButton(const char *code, const char *key, int keyCode, int pin) {
  if (!code || !*code)
    return -1;
  while (g_button_register_lock.test_and_set(std::memory_order_acquire)) {
  }
  int id = findButton(code);
  if (id < 0) {
    const int count = g_button_count.load(std::memory_order_relaxed);
    if (count < kMaxButtons) {
      g_buttons[count] = {code, key ? key : "", keyCode > 0 ? keyCode : 0, pin >= 0 ? pin : -1};
      g_button_pressed[count].store(false, std::memory_order_relaxed);
      g_button_count.store(count + 1, std::memory_order_release);
      id = count;
    }
  }
  g_button_register_lock.clear(std::memory_order_release);
  return id;
}

void queueButton(int id, bool pressed) {
  if (!validButton(id))
    return;
  g_button_pressed[id].store(pressed, std::memory_order_release);
  const int keyCode = g_buttons[id].keyCode;
  queueKeyFor(keyCode > 0 ? keyCode : kUnaliasedButtonTicket, pressed, id);
}

int buttonCount() { return g_button_count.load(std::memory_order_acquire); }

const char *buttonCode(int id) { return validButton(id) ? g_buttons[id].code : ""; }

const char *buttonKey(int id) { return validButton(id) ? g_buttons[id].key : ""; }

int buttonKeyCode(int id) { return validButton(id) ? g_buttons[id].keyCode : 0; }

int buttonPin(int id) { return validButton(id) ? g_buttons[id].pin : -1; }

int findButton(const char *code) {
  if (!code)
    return -1;
  const int count = buttonCount();
  for (int id = 0; id < count; ++id)
    if (std::strcmp(g_buttons[id].code, code) == 0)
      return id;
  return -1;
}

bool buttonPressed(int id) {
  return validButton(id) && g_button_pressed[id].load(std::memory_order_acquire);
}

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
    const int button =
        g_key_button[slot].load(std::memory_order_relaxed) - 1;
    g_key_read.store(read + 1, std::memory_order_release);
    event = {static_cast<int>(code >> 1), (code & 1) != 0, button};
    if (button >= 0)
      event.keyCode = buttonKeyCode(button);
    return true;
  }
  return false;
}

int consumeKeyDown() {
  HardwareKeyEvent event{};
  while (consumeKeyEvent(event)) {
    if (event.pressed && event.keyCode > 0)
      return event.keyCode;
  }
  return 0;
}

} // namespace gea::framework::input
