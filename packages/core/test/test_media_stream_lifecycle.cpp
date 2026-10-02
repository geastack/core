#include "host/media.h"

#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <thread>
#include <vector>

namespace { int attached = 0, detached = 0; }
namespace gea::host::media {
void platform_attach_track(NativeMediaTrackHandle) { ++attached; }
void platform_detach_track(NativeMediaTrackHandle) { ++detached; }
}
#ifdef GEA_RTC_STANDALONE_MEDIA_TEST
namespace gea::platform::storage { bool ensureMounted() { return false; } }
#endif

int main() {
  auto empty = gea::host::media::create_media_stream();
  assert(!empty.id().empty() && empty.nativeHandle != 0);
  assert(empty.getTracks().empty() && empty.getVideoTracks().empty() && !empty.active());
  assert(attached == 0);
  auto sh = gea::host::media::create_stream();
  gea::host::MediaStream stream(sh);

  auto tracks = stream.getAudioTracks();
  assert(tracks.size() == 1);
  auto track = tracks[0];
  assert(track.kind() == "audio");
  assert(track.enabled());
  assert(attached == 1);
  empty.addTrack(track);
  empty.addTrack(track);
  assert(empty.getTracks().size() == 1 && empty.active());
  assert(empty.getTrackById(track.id()).nativeHandle == track.nativeHandle);
  assert(empty.getTrackById("missing").nativeHandle == 0);
  auto copy = gea::host::media::create_media_stream(empty);
  auto sequence = gea::host::media::create_media_stream(std::vector{track, track});
  assert(copy.id() != empty.id() && sequence.id() != copy.id());
  assert(sequence.getTracks().size() == 1 && copy.getTracks()[0].nativeHandle == track.nativeHandle);
  empty.removeTrack(track);
  empty.removeTrack(track);
  assert(!empty.active() && empty.getTracks().empty());
  assert(copy.active() && track.readyState() == "live");
  gea::host::media::destroy_stream(empty.nativeHandle);
  gea::host::media::destroy_stream(sequence.nativeHandle);
  assert(detached == 0 && track.readyState() == "live");

  std::vector<std::int16_t> input(320);
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::int16_t>(i);
  gea::host::media::TrackPcmReader firstReader(track.nativeHandle);
  gea::host::media::TrackPcmReader secondReader(track.nativeHandle);
  gea::host::media::track_inject_pcm(track.nativeHandle, input.data(), input.size());

  assert(firstReader.pendingSamples() == input.size());
  assert(secondReader.pendingSamples() == input.size());

  std::vector<std::int16_t> output(320);
  auto read = gea::host::media::track_read_pcm(track.nativeHandle, output.data(), output.size());
  assert(read == 320);
  for (std::size_t i = 0; i < 320; ++i) assert(output[i] == static_cast<std::int16_t>(i));

  auto read_again = gea::host::media::track_read_pcm(track.nativeHandle, output.data(), output.size());
  assert(read_again == 0);
  // Transport consumption cannot steal audio from either attached element.
  assert(firstReader.read(output.data(), 120) == 120);
  assert(firstReader.pendingSamples() == 200);
  for (int i = 0; i < 120; ++i) assert(output[i] == i);
  assert(secondReader.read(output.data(), output.size()) == 320);
  assert(output == input);
  assert(firstReader.read(output.data(), output.size()) == 200);
  for (int i = 0; i < 200; ++i) assert(output[i] == i + 120);
  assert(firstReader.read(output.data(), output.size()) == 0);
  gea::host::media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  gea::host::media::TrackPcmReader lateReader(track.nativeHandle);
  assert(lateReader.read(output.data(), output.size()) == 0);

  gea::host::media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  track.setEnabled(false);
  track.setEnabled(false);
  assert(detached == 1);
  assert(gea::host::media::track_read_pcm(track.nativeHandle, output.data(), output.size()) == 0);
  assert(firstReader.read(output.data(), output.size()) == 0);
  assert(secondReader.read(output.data(), output.size()) == 0);
  track.setEnabled(true);
  track.setEnabled(true);
  assert(attached == 2);
  gea::host::media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  assert(firstReader.read(output.data(), output.size()) == 320 && output == input);
  assert(lateReader.read(output.data(), output.size()) == 320 && output == input);
  assert(firstReader.droppedSamples() == 0);

  // A stalled sink is bounded, reports its loss, and resumes at retained data.
  std::vector<std::int16_t> overflow(160000);
  for (std::size_t i = 0; i < overflow.size(); ++i) overflow[i] = static_cast<std::int16_t>(i % 30000);
  gea::host::media::track_inject_pcm(track.nativeHandle, overflow.data(), overflow.size());
  assert(firstReader.droppedSamples() == overflow.size() - 128000);
  assert(firstReader.read(output.data(), output.size()) == output.size());
  for (std::size_t i = 0; i < output.size(); ++i) assert(output[i] == overflow[overflow.size() - 128000 + i]);
  auto movedReader = std::move(firstReader);
  assert(firstReader.read(output.data(), output.size()) == 0);
  assert(movedReader.read(output.data(), output.size()) == output.size());
  for (std::size_t i = 0; i < output.size(); ++i) assert(output[i] == overflow[overflow.size() - 128000 + 320 + i]);

  const auto remote = gea::host::media::create_remote_stream();
  assert(attached == 2);
  const auto remoteTrack = gea::host::MediaStream(remote).getAudioTracks()[0];
  gea::host::media::TrackPcmReader remoteReader(remoteTrack.nativeHandle);
  auto remoteContainer = gea::host::media::create_media_stream(std::vector{remoteTrack});
  const auto remoteId = remoteTrack.id();
  remoteTrack.setEnabled(false); remoteTrack.setEnabled(true);
  assert(attached == 2 && detached == 1);
  gea::host::media::track_inject_pcm(remoteTrack.nativeHandle, input.data(), input.size());
  assert(gea::host::media::track_read_pcm(remoteTrack.nativeHandle, output.data(), output.size()) == 320);
  gea::host::media::destroy_stream(remote);
  assert(detached == 1);
  assert(!remoteContainer.active());
  assert(remoteContainer.getTrackById(remoteId).nativeHandle == remoteTrack.nativeHandle);
  assert(remoteTrack.readyState() == "ended");
  assert(remoteReader.read(output.data(), output.size()) == 0);
  gea::host::media::track_inject_pcm(remoteTrack.nativeHandle, input.data(), input.size());
  assert(gea::host::media::track_read_pcm(remoteTrack.nativeHandle, output.data(), output.size()) == 0);
  gea::host::media::destroy_stream(remoteContainer.nativeHandle);

  track.stop();
  assert(track.readyState() == "ended");
  assert(movedReader.read(output.data(), output.size()) == 0);
  assert(detached == 2);
  track.setEnabled(true);
  assert(attached == 2);

  gea::host::media::destroy_stream(sh);
  assert(detached == 2);
  assert(!copy.active());
  gea::host::media::destroy_stream(copy.nativeHandle);

  const auto concurrentStream = gea::host::media::create_remote_stream();
  const auto concurrentTrack = gea::host::media::stream_audio_track(concurrentStream);
  gea::host::media::TrackPcmReader left(concurrentTrack), right(concurrentTrack);
  std::vector<std::int16_t> expected(20000);
  for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::int16_t>(i);
  std::atomic<bool> complete{false};
  auto receive = [&](gea::host::media::TrackPcmReader &reader) {
    std::vector<std::int16_t> actual;
    std::int16_t chunk[137];
    for (;;) {
      const auto count = reader.read(chunk, 137);
      actual.insert(actual.end(), chunk, chunk + count);
      if (actual.size() == expected.size()) break;
      if (!count && complete.load()) {
        // Completion can race a read that found no data: read once more
        // after acquiring the producer's completion store.
        const auto tail = reader.read(chunk, 137);
        actual.insert(actual.end(), chunk, chunk + tail);
        if (!tail) break;
      }
      std::this_thread::yield();
    }
    assert(actual == expected && reader.droppedSamples() == 0);
  };
  std::thread first(receive, std::ref(left)), second(receive, std::ref(right));
  for (std::size_t i = 0; i < expected.size(); i += 200)
    gea::host::media::track_inject_pcm(concurrentTrack, expected.data() + i, 200);
  complete.store(true);
  first.join(); second.join();
  gea::host::media::destroy_stream(concurrentStream);
  assert(left.read(output.data(), output.size()) == 0);

  std::puts("media lifecycle OK");
  return 0;
}
