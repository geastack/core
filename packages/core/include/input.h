#pragma once

namespace gea::framework::input {

// Sets a one-shot "back button pressed" flag. Called from the platform
// launcher-button task (BOOT short press on ESP32) and from the per-app
// application's `_settings_toggle()` entry stub.
void pressBackButton();

// Reads and clears the flag. Returns true if there was a pending press.
// Polled from the launcher's rAF loop; when true and Settings is visible,
// the launcher closes Settings.
bool consumeBackButton();

// Adds rotary encoder detents to the pending input delta. Called by platform
// input tasks; drained by the runtime frame loop and dispatched through the UI
// tree as a normal bubbling `rotary` event.
void queueRotaryDelta(int delta);

// Reads and clears the pending rotary delta. Returns 0 if there is no pending
// movement.
int consumeRotaryDelta();

// Queues a key press (web keyCode, e.g. 38/40 for ArrowUp/ArrowDown) from a
// platform input task — hardware buttons on boards without touch. Drained by
// the runtime frame loop and dispatched through the UI tree as a bubbling
// `keydown` event. keyCode 0 is ignored.
void queueKeyDown(int keyCode);

// Queue a release in the same ordered stream as key presses.
void queueKeyUp(int keyCode);

// Named physical buttons. A board declares each of its buttons once, by the
// name its silkscreen gives it; a press then reaches the app as a keydown/keyup
// whose `code` is that name ("Boot", "Power") and whose `key`/`keyCode` are the
// button's default key alias ("Escape"/27), so an app that only knows keys
// keeps working and one that cares can tell the buttons apart.
constexpr int kMaxButtons = 8;

// Returns the button's id, or -1 when the table is full or `code` is empty.
// Registering a code that is already registered returns its existing id. The
// strings must outlive the program (board tables are string literals). A
// button with no key alias passes key "" and keyCode 0: its events carry
// key "Unidentified". `pin` is its GPIO, -1 for one that is not a GPIO (a
// PMU's power key).
int registerButton(const char *code, const char *key, int keyCode, int pin = -1);

// Queues a press or release of a registered button, in the same ordered stream
// as queueKeyDown/queueKeyUp. Safe from any task (not from an ISR).
void queueButton(int id, bool pressed);

int buttonCount();
// "" for an id that is not registered.
const char *buttonCode(int id);
const char *buttonKey(int id);
int buttonKeyCode(int id);
// -1 for an id that is not registered or a button that is not a GPIO.
int buttonPin(int id);
// The id of a registered code, or -1.
int findButton(const char *code);
// The level the platform last reported, not what the app has consumed yet.
bool buttonPressed(int id);

struct HardwareKeyEvent {
  int keyCode;
  bool pressed;
  // The registered button that raised it, or -1 for a bare key code.
  int button = -1;
};

// Consume the oldest press or release without changing their order.
bool consumeKeyEvent(HardwareKeyEvent &event);

// Reads and clears the oldest pending key press. Returns 0 when none pending.
int consumeKeyDown();

} // namespace gea::framework::input
