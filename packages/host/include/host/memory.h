// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "backends.h"

#include <cstddef>

// A platform with fast on-chip RAM next to a large slow one (ESP32-S3: SRAM
// and PSRAM) defines these; elsewhere they are absent and the calls do nothing.
extern "C" void gea_memory_begin_internal(std::size_t bytes) __attribute__((weak));
extern "C" void gea_memory_end_internal() __attribute__((weak));

namespace gea::host {

struct MemoryFacade {
  double internalFree() const { return gea::framework::memory::MemoryBackend::internalFree(); }
  double internalLargestFreeBlock() const { return gea::framework::memory::MemoryBackend::internalLargestFreeBlock(); }
  double internalMinimumFree() const { return gea::framework::memory::MemoryBackend::internalMinimumFree(); }
  double psramFree() const { return gea::framework::memory::MemoryBackend::psramFree(); }
  double currentTaskStackHighWaterMark() const {
    return gea::framework::memory::MemoryBackend::currentTaskStackHighWaterMark();
  }

  double geaMainStackBytes() const { return gea::framework::memory::MemoryBackend::geaMainStackBytes(); }
  double geaInitStackBytes() const { return gea::framework::memory::MemoryBackend::geaInitStackBytes(); }
  double appFrameStackWords() const { return gea::framework::memory::MemoryBackend::appFrameStackWords(); }
  double appFrameStackBytes() const { return gea::framework::memory::MemoryBackend::appFrameStackBytes(); }

  double displayFlushConfiguredRows() const {
    return gea::framework::memory::MemoryBackend::displayFlushConfiguredRows();
  }
  double displayFlushConfiguredDepth() const {
    return gea::framework::memory::MemoryBackend::displayFlushConfiguredDepth();
  }
  double displayFlushBufferMaxBytes() const {
    return gea::framework::memory::MemoryBackend::displayFlushBufferMaxBytes();
  }
  double displayFlushRows() const { return gea::framework::memory::MemoryBackend::displayFlushRows(); }
  double displayFlushDepth() const { return gea::framework::memory::MemoryBackend::displayFlushDepth(); }
  double displayFlushBufferBytes() const { return gea::framework::memory::MemoryBackend::displayFlushBufferBytes(); }
  double allocationSramCount() const { return gea::framework::memory::MemoryBackend::allocationSramCount(); }
  double allocationPsramCount() const { return gea::framework::memory::MemoryBackend::allocationPsramCount(); }
  double allocationSramBytes() const { return gea::framework::memory::MemoryBackend::allocationSramBytes(); }
  double allocationPsramBytes() const { return gea::framework::memory::MemoryBackend::allocationPsramBytes(); }
  double allocationSramPeakBytes() const { return gea::framework::memory::MemoryBackend::allocationSramPeakBytes(); }
  double allocationPsramPeakBytes() const { return gea::framework::memory::MemoryBackend::allocationPsramPeakBytes(); }
  // Allocations this task makes until endInternal() prefer on-chip RAM; `bytes`
  // says how much to make room for (the display may yield staging buffers).
  void beginInternal(double bytes) const {
    if (gea_memory_begin_internal) gea_memory_begin_internal(bytes > 0 ? static_cast<std::size_t>(bytes) : 0);
  }
  void endInternal() const {
    if (gea_memory_end_internal) gea_memory_end_internal();
  }
};

inline constexpr MemoryFacade Memory{};

}  // namespace gea::host
