#pragma once

// NS4168: a mono I2S class-D amplifier with no control interface. It takes
// standard (Philips) I2S frames, 16-32 bit, 8-96 kHz, and plays the channel its
// CTRL pin's voltage selects (left, right or their average); with CTRL low it
// shuts down. Everything a platform binding needs is the I2S stream itself, so
// this header only documents the part.

namespace gea::chips::ns4168 {

inline constexpr int kMinSampleRate = 8000;
inline constexpr int kMaxSampleRate = 96000;

}  // namespace gea::chips::ns4168
