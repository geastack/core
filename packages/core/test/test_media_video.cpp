// SPDX-License-Identifier: Apache-2.0
#include "host/media.h"

#include <cassert>
#include <cstdio>
#include <thread>

namespace gea::platform::storage { bool ensureMounted() { return false; } }

using namespace gea::host;

std::unique_ptr<media::VideoFrame> frame(std::uint32_t timestamp) {
  auto result = std::make_unique<media::VideoFrame>();
  result->width = 4;
  result->height = 2;
  result->timestampMs = timestamp;
  result->rgb565.assign(8, static_cast<std::uint16_t>(timestamp));
  return result;
}

int main() {
  const MediaStream stream(media::create_remote_video_stream());
  assert(stream.getAudioTracks().empty());
  assert(stream.getVideoTracks().size() == 1);
  const auto track = stream.getVideoTracks().front();
  assert(track.kind() == "video" && track.enabled());
  assert(media::stream_audio_track(stream.nativeHandle) == 0);
  assert(!media::latest_video_frame(track.nativeHandle));
  assert(media::publish_video_frame(track.nativeHandle, frame(10)));
  auto first = media::latest_video_frame(track.nativeHandle);
  assert(first->timestampMs == 10 && first->sequence == 1);
  assert(first == media::latest_video_frame(track.nativeHandle));
  std::weak_ptr<const media::VideoFrame> firstLifetime = first;

  // A producer can replace the live frame without invalidating a renderer
  // using the previous one. Frames aren't copied into per-consumer queues.
  std::thread producer([&] {
    for (unsigned i = 11; i <= 1000; ++i)
      assert(media::publish_video_frame(track.nativeHandle, frame(i)));
  });
  producer.join();
  assert(first->rgb565.front() == 10 && first->timestampMs == 10);
  const auto latest = media::latest_video_frame(track.nativeHandle);
  assert(latest->timestampMs == 1000 && latest->sequence == 991);
  first.reset();
  assert(firstLifetime.expired());

  auto malformed = frame(1001);
  malformed->rgb565.pop_back();
  assert(!media::publish_video_frame(track.nativeHandle, std::move(malformed)));
  malformed = frame(1001);
  malformed->width = UINT32_MAX;
  assert(!media::publish_video_frame(track.nativeHandle, std::move(malformed)));
  assert(!media::publish_video_frame(track.nativeHandle, nullptr));
  assert(!media::publish_video_frame(0, frame(1001)));
  assert(media::latest_video_frame(track.nativeHandle) == latest);

  const MediaStream audio(media::create_remote_stream());
  assert(!media::publish_video_frame(audio.getAudioTracks()[0].nativeHandle, frame(1001)));
  std::int16_t pcm[2] = {1, 2};
  media::track_inject_pcm(track.nativeHandle, pcm, 2);
  assert(media::track_read_pcm(track.nativeHandle, pcm, 2) == 0);

  track.setEnabled(false);
  assert(!media::latest_video_frame(track.nativeHandle));
  assert(!media::publish_video_frame(track.nativeHandle, frame(1002)));
  track.setEnabled(true);
  assert(media::publish_video_frame(track.nativeHandle, frame(1003)));
  assert(media::latest_video_frame(track.nativeHandle)->sequence == 992);

  const auto shared = media::create_media_stream(stream);
  assert(shared.getVideoTracks().front().nativeHandle == track.nativeHandle);
  media::destroy_stream(stream.nativeHandle);
  assert(track.readyState() == "ended" && !shared.active());
  assert(!media::latest_video_frame(track.nativeHandle));
  assert(!media::publish_video_frame(track.nativeHandle, frame(1004)));
  media::destroy_stream(shared.nativeHandle);
  media::destroy_stream(audio.nativeHandle);
  assert(latest->rgb565.front() == 1000);

  const MediaStream stopped(media::create_remote_video_stream());
  const auto stoppedTrack = stopped.getVideoTracks().front();
  assert(media::publish_video_frame(stoppedTrack.nativeHandle, frame(5)));
  stoppedTrack.stop();
  assert(!media::latest_video_frame(stoppedTrack.nativeHandle));
  assert(!media::publish_video_frame(stoppedTrack.nativeHandle, frame(6)));
  media::destroy_stream(stopped.nativeHandle);
  std::puts("video frame ownership, latest-frame delivery and lifecycle OK");
}
