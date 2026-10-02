// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/media.h"
#include <memory>
#include <string>

namespace gea::host::video {
// Network and optional JPEG decoding stay in native workers. Multipart RGB565
// writes directly into pooled pixels. The UI receives only the
// newest immutable RGB565 frame through the same media sink as WebRTC.
class MjpegStream {
 public:
  explicit MjpegStream(const std::string& url);
  ~MjpegStream();
  MjpegStream(const MjpegStream&) = delete;
  MjpegStream& operator=(const MjpegStream&) = delete;
  MediaStream stream() const;
  void stop();
 private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace gea::host::video
