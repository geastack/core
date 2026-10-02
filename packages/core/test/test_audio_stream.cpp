#include "audio_stream.h"
#include "host/audio_level.h"
#include "host/pcm_stream.h"
#include "host/audio.h"
#include "host/websocket.h"
#include "ui/node.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
gea::platform::audio::PcmStreamMixer mixer;
gea::host::websocket::TextProducer nativeProducer;
gea::host::websocket::TextConsumer nativeConsumer;
unsigned playbackFlushes = 0;
}

namespace gea::host::websocket {
void setTextProducer(NativeWebSocketHandle, TextProducer producer) {
  nativeProducer = std::move(producer);
}
void setTextConsumer(NativeWebSocketHandle, TextConsumer consumer) {
  nativeConsumer = std::move(consumer);
}
}

// Only hardware output is replaced. The test exercises the actual audio
// element, media-track buffer, independent reader, and PCM mixer together.
namespace gea::platform::audio {
std::uint64_t AudioSystem::playPcmStream(std::function<std::size_t(std::int16_t*, std::size_t)> pull) {
  return mixer.add(std::move(pull));
}
void AudioSystem::stopPcmStream(std::uint64_t id) { mixer.remove(id); }
bool AudioSystem::pcmStreamSettled(std::uint64_t id) { return mixer.settled(id); }
bool AudioSystem::playFile(const std::string&) { return false; }
void AudioSystem::stopPlayback() { mixer.clear(); }
void AudioSystem::flushPlayback() { ++playbackFlushes; }
}
namespace gea::host::media {
void platform_attach_track(NativeMediaTrackHandle) {}
void platform_detach_track(NativeMediaTrackHandle) {}
}
namespace gea::platform::storage { bool ensureMounted() { return false; } }
namespace gea::embedded::ui {
const char* NodeHandle::getAttribute(const char*) const { return ""; }
void NodeHandle::setAttribute(const char*, const char*) const {}
}

int main() {
  using namespace gea::host;
  HTMLAudioElement absent;
  assert(!absent && absent.paused() && !absent.play());
  HTMLAudioElement missing(gea::embedded::ui::NodeHandle(-1));
  assert(!missing);
  HTMLAudioElement nodeAudio(gea::embedded::ui::NodeHandle(42));
  HTMLAudioElement nodeAgain(gea::embedded::ui::NodeHandle(42));
  HTMLAudioElement differentNode(gea::embedded::ui::NodeHandle(43));
  assert(nodeAudio == nodeAgain && !(nodeAudio == differentNode));
  nodeAudio.setAutoplay(true);
  assert(nodeAgain.autoplay() && !differentNode.autoplay());
  const MediaStream stream(media::create_remote_stream());
  const auto track = stream.getAudioTracks().front();
  const std::array<std::int16_t, 4> first{100, -200, 300, -400};
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  auto audio = HTMLAudioElement::create();
  auto shared = audio;
  assert(audio && audio == shared && audio.paused());
  audio.setAutoplay(true);
  audio.setSrcObject(stream);
  assert(!shared.paused() && shared.srcObject() == stream && mixer.active());
  assert(audio.play());  // Idempotent, never double the amplitude.
  std::array<std::int16_t, 1024> pcm{};
  assert(mixer.mix(pcm.data(), 512) == first.size());
  for (std::size_t i = 0; i < first.size(); ++i)
    assert(pcm[2 * i] == first[i] && pcm[2 * i + 1] == first[i]);
  assert(audio.audioLevel() > 0.008 && audio.audioLevel() < 0.009);
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == 0);
  assert(audio.audioLevel() > 0.008); // A missing packet is not silent PCM.
  assert(mixer.active() && !audio.paused()); // Packet gap is not end of speech.
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  assert(mixer.mix(pcm.data(), 512) == first.size());
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size()); // Old queued reply.
  audio.clearBufferedAudio();
  assert(!audio.paused() && mixer.active());
  assert(mixer.mix(pcm.data(), 512) == 0); // Cancel old reply without stopping output.
  for (int i = 0; i < 100; ++i) {
    const std::array<std::int16_t, 320> silence{};
    media::track_inject_pcm(track.nativeHandle, silence.data(), silence.size());
    assert(mixer.mix(pcm.data(), 512) == silence.size());
    mixer.didWrite(true);
  }
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == first.size());
  assert(pcm[0] == first[0]); // No silence backlog before the next reply.
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  shared.pause();
  assert(audio.paused() && !mixer.active() && audio.audioLevel() == 0);
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  assert(audio.play());
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == first.size()); // Preserve new reply received while paused.
  assert(mixer.mix(pcm.data(), 512) == 0); // Old queued reply was discarded at pause.
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  assert(mixer.mix(pcm.data(), 512) == first.size());
  audio.setSrcObject(nullptr);
  assert(shared.paused() && !mixer.active());
  assert(!shared.srcObject());
  {
    auto temporary = HTMLAudioElement::create();
    temporary.setSrcObject(stream);
    assert(temporary.play() && mixer.active());
  }
  assert(!mixer.active()); // Last element reference releases its own reader.
  audio.setAutoplay(false);
  audio.setSrcObject(stream);
  assert(audio.play());
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  audio.beginDrain();
  assert(!audio.drained());
  // Late RTP packets (including silence) must not move the drain endpoint.
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  audio.beginDrain(); // Idempotent: repeated stop events cannot extend it.
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == first.size());
  assert(!audio.drained()); // Last block pulled, but not written yet.
  mixer.didWrite(true);
  assert(audio.drained());
  assert(mixer.mix(pcm.data(), 512) == 0);
  audio.pause();
  assert(audio.play()); // Reopens at the live edge after draining.
  media::track_inject_pcm(track.nativeHandle, first.data(), first.size());
  audio.beginDrain();
  assert(mixer.mix(pcm.data(), 512) == first.size());
  mixer.didWrite(false);
  assert(!audio.drained()); // A failed write must never be called drained.
  mixer.didWrite(true);
  assert(!audio.drained());
  audio.pause(); // Stop remains immediate even after output failure.
  assert(audio.drained() && !mixer.active());
  track.stop();
  audio.setSrcObject(stream);
  assert(!audio.play());
  media::destroy_stream(stream.nativeHandle);

  AudioLevelMeter meter;
  meter.update(first.data(), first.size(), 100);
  const auto amplitude = meter.read(100);
  assert(amplitude > 0);
  assert(meter.read(99) == amplitude); // Producer can publish after the UI samples its clock.
  meter.update(nullptr, 0, 110);
  assert(meter.read(120) == amplitude);
  assert(meter.read(351) == 0); // No frozen lamps after a stalled transport.
  meter.update(first.data(), first.size(), UINT32_MAX - 50);
  assert(meter.read(20) == amplitude); // Millisecond clock rollover.
  const std::array<std::int16_t, 4> silence{};
  meter.update(silence.data(), silence.size(), 30);
  assert(meter.read(30) == 0); // Actual PCM silence takes effect immediately.

  auto loud = [](std::int16_t* out, std::size_t) { out[0] = 30000; out[1] = -30000; return std::size_t{2}; };
  const auto one = mixer.add(loud), two = mixer.add(loud);
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == 2);
  assert(pcm[0] == 32767 && pcm[1] == 32767 && pcm[2] == -32768 && pcm[3] == -32768);
  mixer.remove(one);
  mixer.remove(one);
  mixer.remove(two);
  assert(!mixer.active());
  const MediaStream capture(media::create_remote_stream());
  const auto mic = capture.getAudioTracks().front();
  auto transport = PcmAudioStream::create(24000);
  transport.setInput(capture);
  std::array<std::int16_t, 320> twentyMs;
  twentyMs.fill(1200);
  assert(transport.readBase64().empty());
  media::track_inject_pcm(mic.nativeHandle, twentyMs.data(), twentyMs.size());
  const auto block = transport.readBase64();
  assert(pcm::decode(block).size() == 480);
  assert(transport.capturePendingMs() == 0 && transport.captureDroppedSamples() == 0);
  for (int i = 0; i < 150; ++i) transport.writeBase64(block);
  assert(transport.queuedMs() == 3000); // Generated output can exceed a live track ring.
  assert(transport.playedMs() == 0 && !transport.drained());
  std::size_t played = 0;
  while (transport.queuedMs()) {
    pcm.fill(0);
    const auto count = mixer.mix(pcm.data(), 512);
    assert(count > 0);
    played += count;
    for (std::size_t i = 0; i < count * 2; ++i) assert(pcm[i] == 1200);
    mixer.didWrite(true);
  }
  assert(played == 48000);
  transport.writeBase64(block);
  transport.resetPlayback(true);
  assert(playbackFlushes == 1);
  assert(transport.queuedMs() == 0 && transport.playedMs() == 0);
  assert(mixer.mix(pcm.data(), 512) == 0);
  media::track_inject_pcm(mic.nativeHandle, twentyMs.data(), twentyMs.size());
  assert(!transport.readBase64().empty()); // Barge-in never stops capture.
  transport.close();
  transport.close();
  assert(!mixer.active());
  mic.stop();
  media::destroy_stream(capture.nativeHandle);
  // KITT's 480-token audio budget is at most 24 seconds (50ms/token).
  // Generate the entire turn before the mixer runs: it must fit unchanged,
  // and replacing that turn must free its full tail before the next arrives.
  auto bounded = PcmAudioStream::create(24000);
  const auto tenMs = pcm::encode(std::vector<std::int16_t>(240, 900));
  for (int turn = 0; turn < 3; ++turn) {
    for (int packet = 0; packet < 2400; ++packet) bounded.writeBase64(tenMs);
    assert(bounded.queuedMs() == 24000);
    bounded.resetPlayback(true);
    assert(bounded.queuedMs() == 0);
  }
  bounded.close();
  // Simulate the socket worker with the UI/event loop completely stopped.
  const MediaStream backgroundCapture(media::create_remote_stream());
  const auto backgroundMic = backgroundCapture.getAudioTracks().front();
  auto native = PcmAudioStream::create(16000);
  native.setInput(backgroundCapture);
  native.pipeTo(WebSocket(123u), "prefix:", ":suffix");
  std::vector<std::int16_t> speech(32000, 1234);
  media::track_inject_pcm(backgroundMic.nativeHandle, speech.data(), speech.size());
  std::thread sender([&] {
    for (int i = 0; i < 100; ++i) {
      const auto message = nativeProducer();
      assert(message.starts_with("prefix:") && message.ends_with(":suffix"));
      const auto samples = pcm::decode(message.substr(7, message.size() - 14));
      assert(samples.size() == 320);
      for (auto sample : samples) assert(sample == 1234);
    }
    assert(nativeProducer().empty());
  });
  sender.join();
  assert(native.capturePackets() == 100 && native.capturePendingMs() == 0);
  assert(native.captureDroppedSamples() == 0);
  native.receiveFrom(WebSocket(123u), "{\"pcm\":\"", "\"}");
  assert(!nativeConsumer("{\"event\":\"bot_ready\"}"));
  const std::vector<std::int16_t> incoming(320, 2222);
  std::thread receiver([&] {
    for (int i = 0; i < 3; ++i)
      assert(nativeConsumer("{\"pcm\":\"" + pcm::encode(incoming) + "\"}"));
  });
  receiver.join();
  assert(native.queuedMs() == 60);
  assert(mixer.mix(pcm.data(), 512) == 0); // Native network startup waits for headroom.
  for (int i = 0; i < 2; ++i)
    assert(nativeConsumer("{\"pcm\":\"" + pcm::encode(incoming) + "\"}"));
  assert(native.queuedMs() == 100);
  assert(mixer.mix(pcm.data(), 512) == 0); // A burst cannot bypass the startup clock.
  std::this_thread::sleep_for(std::chrono::milliseconds(210));
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == 512);
  for (auto sample : pcm) assert(sample == 2222);
  native.close();
  assert(nativeConsumer("{\"pcm\":\"" + pcm::encode(incoming) + "\"}"));
  assert(native.queuedMs() == 0); // A late native packet cannot revive Stop.
  assert(nativeProducer().empty()); // A retained producer cannot revive Stop.
  native = {};
  assert(nativeProducer().empty()); // Nor keep a destroyed PCM stream alive.
  backgroundMic.stop();
  media::destroy_stream(backgroundCapture.nativeHandle);
  // A coordinated upstream A/V clock must not stack another native delay.
  auto coordinated = PcmAudioStream::create(16000);
  coordinated.receiveFrom(WebSocket(123u), "{\"pcm\":\"", "\"}", 0);
  assert(nativeConsumer("{\"pcm\":\"" + pcm::encode(incoming) + "\"}"));
  assert(coordinated.queuedMs() == 20);
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == 320);
  for (int i = 0; i < 320; ++i) assert(pcm[i] == 2222);
  bool invalidDelay = false;
  try { coordinated.receiveFrom(WebSocket(123u), "", "", -1); }
  catch (const std::invalid_argument&) { invalidDelay = true; }
  assert(invalidDelay);
  coordinated.close();
  assert(nativeConsumer("{\"pcm\":\"" + pcm::encode(incoming) + "\"}"));
  assert(coordinated.queuedMs() == 0);
  // Deterministic clock checks: no sleeping or hardware needed to verify the
  // startup deadline, wraparound, short tails and interruption rearming.
  pcm::PlaybackStartGate startup;
  assert(startup.ready(1, 0)); // Direct/generated PCM retains immediate playback.
  startup.enable(100);
  assert(!startup.ready(0, 1000));
  startup.pushed(1000);
  assert(!startup.ready(320, 1099));
  assert(startup.ready(320, 1100)); // Even a 20 ms final reply is released.
  assert(startup.ready(1, 1101)); // Never hold a tail after playback begins.
  startup.reset(); startup.pushed(2000);
  assert(!startup.ready(1599, 2000));
  assert(!startup.ready(32000, 2000)); // A burst must not consume the startup headroom.
  assert(startup.ready(32000, 2100));
  startup.reset(); startup.pushed(0xfffffff0u);
  assert(!startup.ready(320, 0x53u));
  assert(startup.ready(320, 0x54u)); // Millisecond counter wrap preserves deadline.
  std::puts("Streaming audio: first packet, gaps, drain/write ordering, late packets, failure, cancellation and clipping passed");
}
