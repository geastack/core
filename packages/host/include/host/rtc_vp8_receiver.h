// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/rtc_rtp.h"
#include "host/rtc_vp8.h"

namespace gea::host::rtc::vp8 {

// One negotiated VP8 SSRC, after SRTP authentication. Audio, RTCP, RTX and
// unrelated SSRCs never enter the video sequence space. No RTX is advertised.
class Receiver {
 public:
  Receiver(uint8_t payloadType, uint32_t ssrc) : payloadType_(payloadType), ssrc_(ssrc) {}

  template<class Deliver>
  void push(std::span<const uint8_t> bytes, uint64_t nowMs, Deliver&& deliver) {
    const auto packet = rtp::parse(bytes);
    if (!packet || packet->payloadType != payloadType_ || packet->ssrc != ssrc_) return;
    reorder_.push(bytes, nowMs, [&](const rtp::Packet& ordered) { assemble(ordered, deliver); });
  }

  template<class Deliver>
  void poll(uint64_t nowMs, Deliver&& deliver) {
    reorder_.poll(nowMs, [&](const rtp::Packet& ordered) { assemble(ordered, deliver); });
  }

  // Decoder failure/overflow invalidates references, even if RTP had no loss.
  void requireKeyframe() { assembler_.reset(); }

  bool needsKeyframe() const { return assembler_.needsKeyframe(); }

  // Call on a timer too, so a lost initial PLI cannot leave the picture frozen.
  bool takeKeyframeRequest(uint64_t nowMs) {
    if (!needsKeyframe() || (requested_ && nowMs - lastRequestMs_ < 500)) return false;
    requested_ = true;
    lastRequestMs_ = nowMs;
    return true;
  }

 private:
  template<class Deliver>
  void assemble(const rtp::Packet& packet, Deliver&& deliver) {
    auto frame = assembler_.push(packet.sequence, packet.timestamp, packet.marker, packet.payload);
    if (frame) deliver(std::move(*frame));
  }

  rtp::Reorder reorder_;
  Assembler assembler_;
  uint8_t payloadType_;
  uint32_t ssrc_;
  uint64_t lastRequestMs_ = 0;
  bool requested_ = false;
};

} // namespace gea::host::rtc::vp8
