// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/mjpeg_parser.h"
#include <deque>
#include <optional>

namespace gea::host::video {
// Keep references in order. On overload, abandon the entire reference chain
// and restart at the next IDR; never retain an unbounded latency backlog.
class H264PacketQueue {
 public:
  unsigned push(JpegPacket packet) {
    unsigned dropped = 0;
    if (packets_.size() == 3) {
      dropped = unsigned(packets_.size());
      packets_.clear();
      needKeyframe_ = true;
    }
    if (needKeyframe_) {
      if (!packet.h264Keyframe) return dropped + 1;
      packet.resetDecoder = true;
      needKeyframe_ = false;
    }
    packets_.push_back(std::move(packet));
    return dropped;
  }
  std::optional<JpegPacket> pop() {
    if (packets_.empty()) return {};
    auto packet = std::move(packets_.front());
    packets_.pop_front();
    return packet;
  }
  bool empty() const { return packets_.empty(); }
 private:
  std::deque<JpegPacket> packets_;
  bool needKeyframe_ = true;
};
} // namespace gea::host::video
