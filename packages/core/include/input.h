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

// Reads and clears the oldest pending key press. Returns 0 when none pending.
int consumeKeyDown();

} // namespace gea::framework::input
