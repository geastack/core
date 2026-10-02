// SPDX-License-Identifier: Apache-2.0
#include "host/video.h"
#include "host/mjpeg_parser.h"
#include "ui/node.h"
#include "image.h"
#include "memory.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <map>

namespace {
struct Slot {
  gea::framework::graphics::pixel::native_t* pixels;
  bool owned;
};
std::map<int, Slot> slots;
int nextImage = 0, nodeImage = -1;
}

// Replace only the display store/style boundary. Exercise the real video
// element and media publication/lifetime, including concurrent-producer holds.
namespace gea::framework::graphics {
ImageStore& ImageStore::instance() { static ImageStore store; return store; }
int ImageStore::registerBuffer(pixel::native_t* pixels, int, int, int, bool owned) {
  const int id = nextImage++;
  slots.emplace(id, Slot{pixels, owned});
  return id;
}
void ImageStore::dispose(int id) {
  const auto found = slots.find(id);
  assert(found != slots.end());
  if (found->second.owned) std::free(found->second.pixels);
  slots.erase(found);
}
}
namespace gea::framework::memory {
void* Allocator::allocatePreferSpiram(std::size_t size, std::size_t) { return std::malloc(size); }
void Allocator::free(void* pointer) noexcept { std::free(pointer); }
}
namespace gea::embedded::ui {
void Style::set(Property property, int value) const {
  assert(property == Property::ImageId);
  nodeImage = value;
}
}
namespace gea::platform::storage { bool ensureMounted() { return false; } }

int main() {
  using namespace gea::host;
  using namespace gea::framework::graphics;
  HTMLVideoElement video(gea::embedded::ui::NodeHandle(42));
  const MediaStream stream(media::create_remote_video_stream());
  const auto track = stream.getVideoTracks().front();
  video.setAutoplay(true);
  video.setSrcObject(stream);
  unsigned playing = 0;
  video.setOnPlaying([&] { ++playing; });

  auto publish = [&](std::uint16_t color, bool panelEndian = false) {
    auto frame = std::make_unique<media::VideoFrame>();
    frame->width = frame->height = 2;
    frame->panelEndian = panelEndian;
    frame->rgb565.assign(4, color);
    assert(media::publish_video_frame(track.nativeHandle, std::move(frame)));
    return media::latest_video_frame(track.nativeHandle);
  };
  auto first = publish(0xf800);
  std::weak_ptr<const media::VideoFrame> firstLifetime = first;
  HTMLVideoElement::presentFrames();
  assert(slots.size() == 1 && playing == 1 && video.videoWidth() == 2);
  assert(slots.at(nodeImage).pixels[0] == pixel::nativeColor(255, 0, 0));
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565 && !GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
  assert(slots.at(nodeImage).pixels == first->rgb565.data());
  assert(!slots.at(nodeImage).owned);
#else
  assert(slots.at(nodeImage).owned);
#endif
  const int firstSlot = nodeImage;
  HTMLVideoElement::presentFrames();
  assert(nodeImage == firstSlot); // Same frame does not allocate another slot.
  first.reset();
  auto second = publish(0x07e0);
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565 && !GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
  assert(!firstLifetime.expired()); // Display still borrows the previous frame.
#endif
  HTMLVideoElement::presentFrames();
  assert(firstLifetime.expired() && slots.size() == 1 && nodeImage != firstSlot);
  assert(slots.at(nodeImage).pixels[0] == pixel::nativeColor(0, 255, 0));
  assert(playing == 1);
  std::weak_ptr<const media::VideoFrame> secondLifetime = second;
  second.reset();
  track.stop();
  video.setSrcObject(nullptr);
  assert(slots.empty() && nodeImage == -1 && secondLifetime.expired());
  media::destroy_stream(stream.nativeHandle);

  // Decoder-native panel bytes can be borrowed directly on an AMOLED target.
  const MediaStream nativeStream(media::create_remote_video_stream());
  const auto nativeTrack = nativeStream.getVideoTracks().front();
  auto nativeFrame = std::make_unique<media::VideoFrame>();
  nativeFrame->width = nativeFrame->height = 2;
  nativeFrame->panelEndian = true;
  nativeFrame->rgb565.assign(4, pixel::byteSwap16(0xf800));
  assert(media::publish_video_frame(nativeTrack.nativeHandle, std::move(nativeFrame)));
  auto native = media::latest_video_frame(nativeTrack.nativeHandle);
  video.setSrcObject(nativeStream);
  HTMLVideoElement::presentFrames();
  assert(slots.at(nodeImage).pixels[0] == pixel::nativeColor(255, 0, 0));
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565 && GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
  assert(slots.at(nodeImage).pixels == native->rgb565.data() && !slots.at(nodeImage).owned);
#endif
  native.reset();
  nativeTrack.stop();
  video.setSrcObject(nullptr);
  assert(slots.empty());
  media::destroy_stream(nativeStream.nativeHandle);

  // Exercise the new wire format through the actual media/element boundary.
  // Partial raw pictures stay invisible; completed panel bytes are borrowed
  // without a second allocation on an AMOLED RGB565 target.
  const MediaStream rawStream(media::create_remote_video_stream());
  const auto rawTrack = rawStream.getVideoTracks().front();
  video.setSrcObject(rawStream);
  video.play();
  video::MjpegParser parser;
  std::string rawPixels(368 * 448 * 2, '\0');
  for (size_t i = 0; i < rawPixels.size(); i += 2) rawPixels[i] = '\xf8';
  const std::string rawWire = "--frame\r\nContent-Type: application/x-rgb565-be\r\n"
      "Content-Length: 329728\r\nX-Width: 368\r\nX-Height: 448\r\n\r\n" + rawPixels + "\r\n";
  const auto halfway = rawWire.size() / 2;
  unsigned rawPublished = 0;
  const auto publishRaw = [&](video::JpegPacket packet) {
    assert(packet.bytes.empty() && packet.rawFrame);
    assert(media::publish_video_frame(rawTrack.nativeHandle, std::move(packet.rawFrame)));
    ++rawPublished;
  };
  assert(parser.push(std::span(reinterpret_cast<const uint8_t*>(rawWire.data()), halfway), publishRaw));
  HTMLVideoElement::presentFrames();
  assert(rawPublished == 0 && slots.empty());
  assert(parser.push(std::span(reinterpret_cast<const uint8_t*>(rawWire.data() + halfway),
      rawWire.size() - halfway), publishRaw));
  assert(rawPublished == 1);
  auto raw = media::latest_video_frame(rawTrack.nativeHandle);
  std::weak_ptr<const media::VideoFrame> rawLifetime = raw;
  HTMLVideoElement::presentFrames();
  assert(slots.size() == 1 && video.videoWidth() == 368 && video.videoHeight() == 448);
  assert(slots.at(nodeImage).pixels[0] == pixel::nativeColor(255, 0, 0));
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565 && GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
  assert(slots.at(nodeImage).pixels == raw->rgb565.data() && !slots.at(nodeImage).owned);
#endif
  raw.reset();
  video.pause();
  video.setSrcObject(nullptr);
  media::destroy_stream(rawStream.nativeHandle);
  assert(slots.empty() && rawLifetime.expired());

  // Oversized allocation is a recoverable error, not a wrapping allocation.
  bool rejected = false;
  try { media::VideoAllocator<std::uint16_t>{}.allocate(SIZE_MAX); }
  catch (const std::bad_alloc&) { rejected = true; }
  assert(rejected);
  std::puts("Video presentation: pixel format, raw wire frames, lifetime, detach and allocation bounds passed");
}
