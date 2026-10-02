// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "host/video_pixels.h"

namespace gea::host::rtc::vp8 {

struct Payload {
  std::span<const uint8_t> bytes;
  bool startsFrame = false;
};

// RFC 7741 section 4.2. RTP framing, authentication, retransmission and packet
// reordering belong to the transport; this consumes its ordered VP8 payloads.
inline std::optional<Payload> parsePayload(std::span<const uint8_t> packet) {
  if (packet.empty()) return std::nullopt;
  const auto first = packet[0];
  size_t offset = 1;
  if (first & 0x80) {
    if (offset == packet.size()) return std::nullopt;
    const auto extension = packet[offset++];
    if (extension & 0x80) {
      if (offset == packet.size()) return std::nullopt;
      const bool longId = packet[offset++] & 0x80;
      if (longId) ++offset;
    }
    if (extension & 0x40) ++offset;
    if (extension & 0x30) ++offset; // T and K share one byte.
  }
  if (offset >= packet.size()) return std::nullopt;
  // Reserved bits must be ignored, including bit 3 adjacent to PID.
  return Payload{packet.subspan(offset), bool(first & 0x10) && !(first & 0x07)};
}

struct EncodedFrame {
  using Bytes = std::vector<uint8_t, media::VideoAllocator<uint8_t>>;
  // The assembler and decoder queue share this owned PSRAM buffer. Moving
  // a complete frame must not allocate or copy it at the worker boundary.
  Bytes bytes;
  uint32_t timestamp = 0; // Original 90 kHz RTP clock; do not lose A/V timing.
  bool keyframe = false;
};

class Assembler {
 public:
  static constexpr size_t maxFrameBytes = 256 * 1024;

  void reset() {
    current_ = {};
    active_ = haveSequence_ = false;
    needsKeyframe_ = true;
  }

  bool needsKeyframe() const { return needsKeyframe_; }

  std::optional<EncodedFrame> push(uint16_t sequence, uint32_t timestamp, bool marker,
                                   std::span<const uint8_t> packet) {
    const auto payload = parsePayload(packet);
    if (!payload) { reset(); return std::nullopt; }

    if (haveSequence_) {
      const auto distance = uint16_t(sequence - nextSequence_);
      // Late/duplicate packets cannot append a second copy or restart a frame.
      if (distance >= 0x8000) return std::nullopt;
      if (distance) { active_ = false; current_ = {}; needsKeyframe_ = true; }
    }
    haveSequence_ = true;
    nextSequence_ = uint16_t(sequence + 1);

    if (payload->startsFrame) {
      if (active_) needsKeyframe_ = true; // Previous frame lost its marker/tail.
      current_ = {};
      current_.timestamp = timestamp;
      current_.keyframe = !(payload->bytes[0] & 1);
      active_ = !needsKeyframe_ || current_.keyframe;
    } else if (active_ && timestamp != current_.timestamp) {
      active_ = false;
      current_ = {};
      needsKeyframe_ = true;
    }
    if (!active_) {
      needsKeyframe_ = true;
      return std::nullopt;
    }
    if (payload->bytes.size() > maxFrameBytes - current_.bytes.size()) {
      reset();
      return std::nullopt;
    }
    try {
      current_.bytes.insert(current_.bytes.end(), payload->bytes.begin(), payload->bytes.end());
    } catch (const std::bad_alloc&) {
      reset();
      return std::nullopt; // A lost reference requests a keyframe, not a disconnect.
    }
    if (!marker) return std::nullopt;
    active_ = false;
    needsKeyframe_ = false;
    return std::exchange(current_, {});
  }

 private:
  EncodedFrame current_;
  uint16_t nextSequence_ = 0;
  bool haveSequence_ = false;
  bool active_ = false;
  bool needsKeyframe_ = true;
};

} // namespace gea::host::rtc::vp8
