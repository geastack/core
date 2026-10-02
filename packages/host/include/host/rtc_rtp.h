// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <cstring>
#include <vector>
#include "host/video_pixels.h"

namespace gea::host::rtc::rtp {

inline uint16_t read16(const uint8_t* bytes) {
  return (uint16_t(bytes[0]) << 8) | bytes[1];
}

inline uint32_t read32(const uint8_t* bytes) {
  return (uint32_t(read16(bytes)) << 16) | read16(bytes + 2);
}

inline void write32(uint8_t* bytes, uint32_t value) {
  bytes[0] = uint8_t(value >> 24);
  bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8);
  bytes[3] = uint8_t(value);
}

struct Packet {
  std::span<const uint8_t> payload;
  uint32_t ssrc = 0;
  uint32_t timestamp = 0;
  uint16_t sequence = 0;
  uint8_t payloadType = 0;
  bool marker = false;
};

// The caller MUST authenticate/decrypt SRTP first. Bounds cover RFC 3550's
// CSRC list, optional extension block and padding before exposing a payload.
inline std::optional<Packet> parse(std::span<const uint8_t> bytes) {
  if (bytes.size() < 12 || bytes[0] >> 6 != 2) return std::nullopt;
  // RFC 5761: this range is reserved for RTCP on a multiplexed transport.
  if (bytes[1] >= 192 && bytes[1] <= 223) return std::nullopt;
  size_t offset = 12 + 4 * (bytes[0] & 15);
  if (offset > bytes.size()) return std::nullopt;
  if (bytes[0] & 0x10) {
    if (bytes.size() - offset < 4) return std::nullopt;
    const size_t length = 4 + size_t(read16(bytes.data() + offset + 2)) * 4;
    if (length > bytes.size() - offset) return std::nullopt;
    offset += length;
  }
  size_t end = bytes.size();
  if (bytes[0] & 0x20) {
    const auto padding = bytes.back();
    if (!padding || padding > end - offset) return std::nullopt;
    end -= padding;
  }
  if (offset == end) return std::nullopt;
  return Packet{bytes.subspan(offset, end - offset), read32(bytes.data() + 8),
                read32(bytes.data() + 4), read16(bytes.data() + 2),
                uint8_t(bytes[1] & 127), bool(bytes[1] & 128)};
}

// No delay for in-order traffic. Only a sequence gap starts a bounded wait.
// The callback runs synchronously; its packet spans must not escape it.
class Reorder {
 public:
  static constexpr size_t capacity = 64;
  static constexpr size_t maxPacketBytes = 4096;
  static constexpr uint64_t gapWaitMs = 30;

  void reset() {
    for (auto& slot : slots_) slot.clear();
    queued_ = 0;
    initialized_ = false;
  }

  template<class Deliver>
  void push(std::span<const uint8_t> bytes, uint64_t nowMs, Deliver&& deliver) {
    if (bytes.size() > maxPacketBytes) return;
    const auto packet = parse(bytes);
    if (!packet) return;
    if (!initialized_) {
      next_ = packet->sequence;
      initialized_ = true;
    }
    const auto distance = uint16_t(packet->sequence - next_);
    if (distance >= 0x8000) return; // Duplicate or late, including sequence wrap.
    if (distance >= capacity) {
      // Decoder overload or a large network loss: discard stale queued packets.
      // The downstream assembler sees the sequence gap and requires a keyframe.
      for (auto& slot : slots_) slot.clear();
      queued_ = 0;
      next_ = packet->sequence;
    }
    if (packet->sequence == next_) {
      // The normal path is already ordered: deliver the caller's authenticated
      // span directly, without copying or reserving a full packet window.
      ++next_;
      deliver(*packet);
      poll(nowMs, deliver);
      return;
    }
    auto& slot = slots_[packet->sequence % capacity];
    if (!slot.bytes.empty()) return;
    try { slot.bytes.assign(bytes.begin(), bytes.end()); }
    catch (const std::bad_alloc&) { return; } // Packet loss, never a device abort.
    slot.arrivedMs = nowMs;
    ++queued_;
    poll(nowMs, deliver);
  }

  template<class Deliver>
  void poll(uint64_t nowMs, Deliver&& deliver) {
    if (!initialized_) return;
    // Almost all packets are already ordered. Do not scan 63 PSRAM-backed
    // slots after every audio/video packet (or every transport timer tick)
    // when there is no outstanding sequence gap.
    while (queued_) {
      auto& slot = slots_[next_ % capacity];
      if (!slot.bytes.empty()) {
        // Release the temporary PSRAM allocation after synchronous delivery;
        // retaining capacity per sequence slot grows to half a megabyte across
        // the two tracks even when no packets are currently out of order.
        Slot::Bytes bytes;
        bytes.swap(slot.bytes);
        --queued_;
        const auto packet = parse(std::span(bytes.data(), bytes.size()));
        // The bounded window guarantees that this slot belongs to next_.
        ++next_;
        if (packet) deliver(*packet);
        continue;
      }
      size_t nearest = capacity;
      uint64_t earliest = nowMs;
      for (size_t distance = 1; distance < capacity; ++distance) {
        const auto& queued = slots_[uint16_t(next_ + distance) % capacity];
        if (queued.bytes.empty()) continue;
        if (nearest == capacity) nearest = distance;
        if (queued.arrivedMs < earliest) earliest = queued.arrivedMs;
      }
      if (nearest == capacity || nowMs - earliest < gapWaitMs) return;
      next_ = uint16_t(next_ + nearest);
    }
  }

 private:
  struct Slot {
    // Only genuinely reordered packets own a buffer. The allocator explicitly
    // uses PSRAM on ESP32, including for small packets below IDF's threshold.
    using Bytes = std::vector<uint8_t, media::VideoAllocator<uint8_t>>;
    Bytes bytes;
    uint64_t arrivedMs = 0;
    void clear() { Bytes{}.swap(bytes); arrivedMs = 0; }
  };
  std::array<Slot, capacity> slots_;
  size_t queued_ = 0;
  uint16_t next_ = 0;
  bool initialized_ = false;
};

// RFC 4585 picture loss indication. Caller protects this with SRTCP before
// transmission. Reduced-size RTCP must have been negotiated for standalone PLI.
inline std::array<uint8_t, 12> pictureLossIndication(uint32_t sender, uint32_t media) {
  std::array<uint8_t, 12> bytes{0x81, 206, 0, 2};
  write32(bytes.data() + 4, sender);
  write32(bytes.data() + 8, media);
  return bytes;
}

} // namespace gea::host::rtc::rtp
