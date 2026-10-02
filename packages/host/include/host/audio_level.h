// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gea::host {

// Display telemetry only: an empty pull is a scheduling/network gap, not a
// silent PCM block. Never use this meter to end playback or gate the mic.
class AudioLevelMeter {
 public:
  void update(const std::int16_t* samples, std::size_t count, std::uint32_t nowMs) {
    if (!count) return;
    std::uint64_t energy = 0;
    for (std::size_t i = 0; i < count; ++i)
      energy += std::int64_t(samples[i]) * samples[i];
    level_.store(static_cast<unsigned>(std::sqrt(double(energy) / count)), std::memory_order_relaxed);
    updatedMs_.store(nowMs, std::memory_order_release);
  }

  double read(std::uint32_t nowMs) const {
    // Expire stale telemetry if transport stops delivering even silent PCM.
    // This changes no audio buffering, playback, or turn detection.
    if (std::int32_t(nowMs - updatedMs_.load(std::memory_order_acquire)) > 250) return 0;
    return level_.load(std::memory_order_relaxed) / 32768.0;
  }

  void reset() { level_.store(0, std::memory_order_relaxed); }

 private:
  std::atomic<unsigned> level_{0};
  std::atomic<std::uint32_t> updatedMs_{0};
};
}
