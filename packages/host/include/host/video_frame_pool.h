// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/media.h"
#include <array>
#include <limits>

namespace gea::host::media {

// One decoder, newest published picture and an older renderer-held picture
// can coexist. Reserve their large pixel buffers together, then reuse only
// storage with no reader. Each published frame gets a fresh control block so
// weak frame references expire normally. Slow/additional sinks skip presentation,
// never receive a buffer being overwritten and never grow this bounded pool.
class VideoFramePool {
 public:
  std::shared_ptr<VideoFrame> acquire(unsigned width, unsigned height) {
    if (!width || !height || size_t(width) > std::numeric_limits<size_t>::max() / height)
      return nullptr;
    const size_t count = size_t(width) * height;
    std::shared_ptr<VideoPixels::Storage> selected;
    for (auto& slot : slots_) {
      if (slot && slot.use_count() != 1) continue;
      if (!slot || slot->capacity < count) {
        auto replacement = std::make_shared<VideoPixels::Storage>();
        replacement->pixels = VideoPixels::allocate(count);
        if (!replacement->pixels) continue;
        replacement->capacity = count;
        slot = std::move(replacement);
      }
      if (!selected) selected = slot;
    }
    if (!selected) return nullptr;
    auto result = std::make_shared<VideoFrame>();
    result->width = width;
    result->height = height;
    result->rgb565.pixels_ = selected->pixels;
    result->rgb565.size_ = count;
    result->rgb565.capacity_ = selected->capacity;
    result->rgb565.pooled_ = std::move(selected);
    return result;
  }

  void reset() { slots_.fill(nullptr); }

 private:
  std::array<std::shared_ptr<VideoPixels::Storage>, 3> slots_{};
};

} // namespace gea::host::media
