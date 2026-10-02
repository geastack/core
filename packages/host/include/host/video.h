// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/media.h"
#include <functional>
#include <memory>

namespace gea::embedded::ui { class NodeHandle; }
namespace gea::host {
struct VideoPresentationStats {
  uint32_t decoded = 0, selected = 0, maxGapMs = 0;
};
class HTMLVideoElement {
 public:
  HTMLVideoElement() = default;
  explicit HTMLVideoElement(const gea::embedded::ui::NodeHandle& node);
  bool play() const;
  void pause() const;
  bool paused() const;
  bool autoplay() const;
  void setAutoplay(bool value) const;
  std::string src() const;
  void setSrc(const std::string& url) const;
  MediaStream srcObject() const;
  void setSrcObject(MediaStream source) const;
  void setSrcObject(std::nullptr_t) const { setSrcObject(MediaStream{}); }
  double videoWidth() const;
  double videoHeight() const;
  void setOnPlaying(std::nullptr_t) const { setOnPlayingHandler({}); }
  template<class Callback> void setOnPlaying(Callback callback) const {
    setOnPlayingHandler([callback] { callback(); });
  }
  bool operator==(std::nullptr_t) const { return !state_; }
  bool operator==(const HTMLVideoElement& other) const { return state_ == other.state_; }
  explicit operator bool() const { return static_cast<bool>(state_); }
  static void presentFrames();
  static VideoPresentationStats presentationStats();
 private:
  void setOnPlayingHandler(std::function<void()> callback) const;
  struct State;
  std::shared_ptr<State> state_;
};
namespace video { void presentFrames(); }
} // namespace gea::host
