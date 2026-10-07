#include "audio_oscillator.h"

#include <cassert>
#include <cmath>

using namespace gea::platform::audio;

int main() {
  // Activating the worklet changes the context clock origin while physical
  // audio remains at ESP uptime. One offset preserves the 20 ms voice duration.
  const double physicalNow = 1000;
  const double contextNow = 2;
  const double start =
      physicalAudioDeadline(contextNow, contextNow, physicalNow);
  const double end =
      physicalAudioDeadline(contextNow + 0.020, contextNow, physicalNow);
  assert(start == physicalNow && std::abs(end - start - 0.020) < 1e-10);
  FiniteOscillatorSchedule schedule;
  schedule.start(start, physicalNow);
  const auto tone = schedule.stop(end, physicalNow);
  assert(tone && tone->durationMs == 20 && tone->delayMs == 0);

  // Default stop after the tone has ended previously replayed uptime-sized
  // audio, capped to 2.5 seconds. It must not submit any new voice.
  assert(!schedule.stop(0, 1001));
  assert(!schedule.stop(0, 2000));
  assert(!schedule.stop(2001, 2000));

  FiniteOscillatorSchedule delayed;
  delayed.start(5, 3);
  const auto future = delayed.stop(5.070, 3);
  assert(future && future->durationMs == 70 && future->delayMs == 2000);
  assert(!delayed.stop(0, 3));

  FiniteOscillatorSchedule immediate;
  immediate.start(0, 4);
  assert(!immediate.stop(0, 4));
  assert(!immediate.stop(0, 5));

  // Preserve the engine's bounded finite-tone limit even for remote deadlines.
  FiniteOscillatorSchedule bounded;
  bounded.start(10, 10);
  const auto longTone = bounded.stop(1e30, 10);
  assert(longTone && longTone->durationMs == 2500);
}
