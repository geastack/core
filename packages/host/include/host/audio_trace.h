#pragma once

#ifndef GEA_AUDIO_DEBUG_PCM_TRACE
#define GEA_AUDIO_DEBUG_PCM_TRACE 0
#endif

#if GEA_AUDIO_DEBUG_PCM_TRACE
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "esp_log.h"
#include "esp_timer.h"

namespace gea::host {
// Diagnostic only: buffer levels in RAM, then print after the opening. No
// per-packet logging on the real-time path and no microphone audio retained.
class OpeningPcmTrace {
public:
  explicit OpeningPcmTrace(const char *name) : name_(name) {}

  void reset() { count_ = 0; started_ = finished_ = false; }

  void record(const int16_t *samples, size_t count) {
    if (finished_ || !count) return;
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i)
      energy += int64_t(samples[i]) * samples[i];
    const auto rms = uint16_t(std::sqrt(double(energy) / count));
    if (!started_ && rms < 20) return;
    started_ = true;
    entries_[count_++] = {uint32_t(esp_timer_get_time() / 1000), rms};
    if (count_ != entries_.size()) return;
    finished_ = true;
    for (size_t first = 0; first < count_; first += 16) {
      char line[384]{};
      size_t used = 0;
      for (size_t i = first; i < first + 16; ++i)
        used += std::snprintf(line + used, sizeof(line) - used, "%lu:%u,",
                              static_cast<unsigned long>(entries_[i].ms), entries_[i].rms);
      ESP_LOGI("gea_pcm_trace", "%s time_ms:rms=%s", name_, line);
    }
  }

private:
  struct Entry { uint32_t ms; uint16_t rms; };
  std::array<Entry, 256> entries_{};
  const char *name_;
  size_t count_ = 0;
  bool started_ = false, finished_ = false;
};
} // namespace gea::host
#endif
