// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/media.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <span>

namespace gea::host::rtc::vp8 {

// Explicit diagnostic only. Retain a bounded prefix of accepted encoded
// references by moving their existing PSRAM vectors, never copying a frame
// or writing flash from the live worker. Saving requires every worker stopped.
class Capture {
 public:
  using Bytes = std::vector<uint8_t, media::VideoAllocator<uint8_t>>;
  static constexpr unsigned maxFrames = 12;
  static constexpr size_t maxBytes = 128 * 1024;
  struct Frame {
    Bytes bytes;
    uint32_t pts = 0;
    uint64_t wallUs = 0, cpuUs = 0, codecUs = 0, convertUs = 0;
  };

  void workerStarted() {
    std::lock_guard lock(mutex_);
    workers_.fetch_add(1, std::memory_order_relaxed);
  }
  void workerStopped() { workers_.fetch_sub(1, std::memory_order_relaxed); }

  bool start() {
    std::lock_guard lock(mutex_);
    if (workers_.load(std::memory_order_relaxed)) return false;
    for (auto& frame : frames_) frame = Frame{};
    count_ = 0; size_ = 0; width_ = height_ = 0;
    requested_.store(true, std::memory_order_release);
    return true;
  }

  bool requested() const { return requested_.load(std::memory_order_acquire); }

  void cancel() {
    std::lock_guard lock(mutex_);
    requested_.store(false, std::memory_order_release);
    for (auto& frame : frames_) frame = Frame{};
    count_ = 0; size_ = 0; width_ = height_ = 0;
  }

  bool take(Bytes& bytes, uint32_t pts, uint64_t wallUs, uint64_t cpuUs,
            uint64_t codecUs, uint64_t convertUs) {
    if (!requested()) return false;
    std::lock_guard lock(mutex_);
    if (!requested()) return false;
    if (!count_) {
      if (bytes.size() < 10 || bytes[0] & 1 || bytes[3] != 0x9d ||
          bytes[4] != 0x01 || bytes[5] != 0x2a) return false;
      width_ = (unsigned(bytes[6]) | unsigned(bytes[7]) << 8) & 0x3fff;
      height_ = (unsigned(bytes[8]) | unsigned(bytes[9]) << 8) & 0x3fff;
      if (!width_ || !height_) return false;
    }
    if (count_ == maxFrames || bytes.empty() || bytes.size() > maxBytes - size_) {
      requested_.store(false, std::memory_order_release);
      return false;
    }
    size_ += bytes.size();
    auto& frame = frames_[count_++];
    frame = Frame{std::move(bytes), pts, wallUs, cpuUs, codecUs, convertUs};
    if (count_ == maxFrames) requested_.store(false, std::memory_order_release);
    return true;
  }

  bool save(const char* path) {
    std::lock_guard lock(mutex_);
    // In particular, don't write SPIFFS while a live decoder uses a PSRAM stack.
    if (!path || !*path || workers_.load(std::memory_order_relaxed) || !count_) return false;
    requested_.store(false, std::memory_order_release);
    auto* file = std::fopen(path, "wb");
    if (!file) return false;
    bool ok = write(file);
    if (std::fclose(file)) ok = false;
    return ok;
  }

  void report() {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < count_; ++i) {
      const auto& f = frames_[i];
      std::printf("GEADEV:VP8CAPTURE FRAME n=%u bytes=%u pts=%u wall_us=%llu cpu_us=%llu codec_us=%llu convert_us=%llu\n",
          i, unsigned(f.bytes.size()), unsigned(f.pts), (unsigned long long)f.wallUs,
          (unsigned long long)f.cpuUs, (unsigned long long)f.codecUs, (unsigned long long)f.convertUs);
    }
    std::printf("GEADEV:VP8CAPTURE END frames=%u bytes=%u width=%u height=%u workers=%u\n",
        count_, unsigned(size_), width_, height_, workers_.load(std::memory_order_relaxed));
  }

  // Also used by the portable format/ownership regression without a temp file.
  bool write(std::FILE* file) const {
    if (!file || !count_) return false;
    std::array<uint8_t, 32> header{'D', 'K', 'I', 'F', 0, 0, 32, 0, 'V', 'P', '8', '0'};
    put(header.data() + 12, width_, 2); put(header.data() + 14, height_, 2);
    put(header.data() + 16, 1000, 4); put(header.data() + 20, 1, 4);
    put(header.data() + 24, count_, 4);
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) return false;
    for (unsigned i = 0; i < count_; ++i) {
      const auto& f = frames_[i];
      std::array<uint8_t, 12> frameHeader{};
      put(frameHeader.data(), f.bytes.size(), 4);
      put(frameHeader.data() + 4, uint32_t(f.pts - frames_[0].pts), 8);
      if (std::fwrite(frameHeader.data(), 1, frameHeader.size(), file) != frameHeader.size() ||
          std::fwrite(f.bytes.data(), 1, f.bytes.size(), file) != f.bytes.size()) return false;
    }
    return true;
  }

 private:
  static void put(uint8_t* dst, uint64_t value, unsigned length) {
    for (unsigned i = 0; i < length; ++i) { dst[i] = uint8_t(value); value >>= 8; }
  }
  mutable std::mutex mutex_;
  std::atomic<bool> requested_{false};
  std::atomic<unsigned> workers_{0};
  std::array<Frame, maxFrames> frames_;
  unsigned count_ = 0, width_ = 0, height_ = 0;
  size_t size_ = 0;
};

inline Capture& capture() { static Capture instance; return instance; }
} // namespace gea::host::rtc::vp8
