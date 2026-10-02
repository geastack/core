// SPDX-License-Identifier: Apache-2.0
#include "host/video.h"
#include "host/mjpeg.h"
#include "ui/node.h"
#include "image.h"
#include "memory.h"
#include "pixel.h"
#include <stdexcept>
#include <unordered_map>
#include <chrono>

namespace gea::host {
struct HTMLVideoElement::State {
  int node = -1, image = -1;
  uint32_t width = 0, height = 0;
  uint64_t sequence = 0;
  uint32_t selected = 0, maxGapMs = 0;
  std::chrono::steady_clock::time_point selectedAt{};
  MediaStream source;
  std::string url;
  std::unique_ptr<video::MjpegStream> mjpeg;
  bool autoplay = false, paused = true, playing = false;
  std::function<void()> onPlaying;
  std::shared_ptr<const media::VideoFrame> presentedFrame;
  ~State() { if (image >= 0) gea::framework::graphics::ImageStore::instance().dispose(image); }
  static std::unordered_map<int, std::weak_ptr<State>>& registry() {
    static std::unordered_map<int, std::weak_ptr<State>> states;
    return states;
  }
};

HTMLVideoElement::HTMLVideoElement(const gea::embedded::ui::NodeHandle& node) {
  if (!node.valid()) return;
  auto& entry = State::registry()[node.id()];
  state_ = entry.lock();
  if (!state_) { state_ = std::make_shared<State>(); state_->node = node.id(); entry = state_; }
}
bool HTMLVideoElement::play() const {
  if (!state_) return false;
  if (!state_->url.empty() && !state_->mjpeg) {
    state_->mjpeg = std::make_unique<video::MjpegStream>(state_->url);
    state_->source = state_->mjpeg->stream();
    state_->sequence = 0;
  }
  if (state_->source.getVideoTracks().empty()) return false;
  state_->paused = false;
  return true;
}
void HTMLVideoElement::pause() const {
  if (state_) {
    state_->paused = true;
    state_->playing = false;
    if (state_->mjpeg) { state_->mjpeg.reset(); state_->source = {}; }
  }
}
bool HTMLVideoElement::paused() const { return !state_ || state_->paused; }
bool HTMLVideoElement::autoplay() const { return state_ && state_->autoplay; }
void HTMLVideoElement::setAutoplay(bool enabled) const {
  if (!state_) return;
  state_->autoplay = enabled;
  if (enabled) play();
}
std::string HTMLVideoElement::src() const { return state_ ? state_->url : std::string{}; }
void HTMLVideoElement::setSrc(const std::string& url) const {
  setSrcObject(MediaStream{});
  state_->url = url;
  if (state_->autoplay && !url.empty()) play();
}
MediaStream HTMLVideoElement::srcObject() const {
  return state_ && state_->url.empty() ? state_->source : MediaStream{};
}
void HTMLVideoElement::setSrcObject(MediaStream stream) const {
  if (!state_) throw std::logic_error("null HTMLVideoElement");
  state_->mjpeg.reset();
  state_->url.clear();
  state_->source = stream;
  state_->sequence = 0;
  state_->selected = state_->maxGapMs = 0;
  state_->selectedAt = {};
  state_->width = state_->height = 0;
  state_->playing = false;
  // Detaching/replacing a stream releases its presentation buffer immediately.
  // ImageStore borrows that buffer on RGB565 targets, so remove the slot first.
  if (state_->image >= 0) {
    gea::embedded::ui::NodeHandle(state_->node).style().imageId(-1);
    gea::framework::graphics::ImageStore::instance().dispose(state_->image);
    state_->image = -1;
  }
  state_->presentedFrame.reset();
  if (state_->autoplay) play();
}
double HTMLVideoElement::videoWidth() const { return state_ ? state_->width : 0; }
double HTMLVideoElement::videoHeight() const { return state_ ? state_->height : 0; }
void HTMLVideoElement::setOnPlayingHandler(std::function<void()> callback) const {
  if (state_) state_->onPlaying = std::move(callback);
}

void HTMLVideoElement::presentFrames() {
  using namespace gea::framework::graphics;
  using gea::framework::memory::Allocator;
  // Only the UI thread touches this registry and ImageStore. Decode workers
  // publish immutable frames through media; they never paint the display.
  std::vector<std::shared_ptr<State>> active;
  for (auto it = State::registry().begin(); it != State::registry().end();) {
    auto state = it->second.lock();
    if (!state) it = State::registry().erase(it);
    else { active.push_back(std::move(state)); ++it; }
  }
  for (const auto& state : active) {
    if (state->paused) continue;
    const auto tracks = state->source.getVideoTracks();
    if (tracks.empty()) continue;
    auto frame = media::latest_video_frame(tracks.front().nativeHandle);
    if (!frame || frame->sequence == state->sequence) continue;
    auto& store = ImageStore::instance();
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
    // Decoder and renderer already agree on the pixel format. Retain the
    // immutable frame for the whole slot lifetime instead of allocating and
    // converting a second full-sized buffer for every presentation.
    const bool borrowed = frame->panelEndian == bool(GEA_EMBEDDED_PIXEL_PANEL_ENDIAN);
#else
    const bool borrowed = false;
#endif
    int image = -1;
    if (borrowed) {
      image = store.registerBuffer(reinterpret_cast<pixel::native_t*>(
          const_cast<uint16_t*>(frame->rgb565.data())), frame->width, frame->height);
    } else {
      const size_t size = frame->rgb565.size() * sizeof(pixel::native_t);
      auto* pixels = static_cast<pixel::native_t*>(Allocator::allocatePreferSpiram(size, 128));
      if (!pixels) continue;
      for (size_t i = 0; i < frame->rgb565.size(); ++i) {
        const uint16_t p = frame->panelEndian ? pixel::byteSwap16(frame->rgb565[i]) : frame->rgb565[i];
        pixels[i] = pixel::nativeColor(((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31);
      }
      image = store.registerBuffer(pixels, frame->width, frame->height, -1, true);
      if (image < 0) { Allocator::free(pixels); continue; }
    }
    if (image < 0) continue;
    // A new slot makes normal image invalidation redraw the video while
    // preserving its CSS clipping, stacking and the controls above it.
    gea::embedded::ui::NodeHandle(state->node).style().imageId(image);
    if (state->image >= 0) store.dispose(state->image);
    state->image = image;
    state->presentedFrame = borrowed ? frame : nullptr;
    state->sequence = frame->sequence;
    const auto now = std::chrono::steady_clock::now();
    if (state->selected) state->maxGapMs = std::max(state->maxGapMs,
        uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - state->selectedAt).count()));
    state->selectedAt = now;
    ++state->selected;
    state->width = frame->width;
    state->height = frame->height;
    if (!state->playing) {
      state->playing = true;
      const auto callback = state->onPlaying;
      if (callback) callback();
    }
  }
}
VideoPresentationStats HTMLVideoElement::presentationStats() {
  VideoPresentationStats result;
  for (const auto& [id, weak] : State::registry()) {
    const auto state = weak.lock();
    if (!state || state->paused) continue;
    const auto tracks = state->source.getVideoTracks();
    if (tracks.empty()) continue;
    const auto latest = media::latest_video_frame(tracks.front().nativeHandle);
    if (latest) result.decoded += uint32_t(latest->sequence);
    result.selected += state->selected;
    result.maxGapMs = std::max(result.maxGapMs, state->maxGapMs);
  }
  return result;
}
namespace video { void presentFrames() { HTMLVideoElement::presentFrames(); } }
} // namespace gea::host
