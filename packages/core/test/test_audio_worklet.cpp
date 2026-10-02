#include "host/audio.h"
#include "audio_stream.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
gea::platform::audio::PcmStreamMixer mixer;
std::atomic<unsigned> moduleRuns{0}, processCalls{0}, captures{0}, destroyed{0};
std::atomic<unsigned> captureSamples{0};
std::atomic<unsigned> flushes{0};
std::atomic<bool> isolated{true}, connectedInput{false};
const auto mainThread = std::this_thread::get_id();
}
namespace gea::platform::audio {
std::uint64_t AudioSystem::playPcmStream(std::function<std::size_t(std::int16_t*, std::size_t)> pull) { return mixer.add(std::move(pull)); }
void AudioSystem::stopPcmStream(std::uint64_t id) { mixer.remove(id); }
bool AudioSystem::pcmStreamSettled(std::uint64_t id) { return mixer.settled(id); }
void AudioSystem::flushPlayback() { ++flushes; }
double AudioSystem::processingLatency() { return 512.0 / 16000; }
double AudioSystem::outputLatency() { return 6.0 * 240 / 16000; }
AudioContext AudioSystem::sharedContext() { return {}; }
AudioNode::AudioNode(NativeAudioHandle id) : native_(id) {}
NativeAudioHandle AudioNode::nativeId() const { return native_; }
AudioDestinationNode::AudioDestinationNode(NativeAudioHandle id) : AudioNode(id) {}
AudioDestinationNode AudioContext::destination() const { return AudioDestinationNode(1); }
double AudioContext::currentTime() const { return 0; }
}
namespace gea::host::media {
void platform_attach_track(NativeMediaTrackHandle) {}
void platform_detach_track(NativeMediaTrackHandle) {}
}
namespace gea::platform::storage { bool ensureMounted() { return false; } }

using namespace gea::host;
struct Processor final : AudioWorkletProcessor {
  std::thread::id thread = std::this_thread::get_id();
  bool fail = false;
  workers::MessagePort network;
  Processor() {
    isolated = isolated && thread != mainThread && !workers::Context::current()->isMain();
    assert(audio_worklet::sampleRate() == 24000);
    port.setOnMessage([this](workers::MessageEvent& event) {
      assert(std::this_thread::get_id() == thread);
      if (!event.ports.empty()) {
        network = event.ports[0];
        network.setOnMessage([this](workers::MessageEvent& e) {
          assert(std::this_thread::get_id() == thread);
          network.postMessage(std::get<std::string>(e.data.value) + "-audio");
        });
      } else if (std::get<std::string>(event.data.value) == "flush") audio_worklet::flushOutput();
      else if (std::get<std::string>(event.data.value) == "fail") fail = true;
    });
  }
  ~Processor() override { isolated = isolated && std::this_thread::get_id() == thread; ++destroyed; }
  bool process(const AudioWorkletBus& inputs, AudioWorkletBus& outputs, const AudioWorkletParameters& parameters) override {
    assert(std::this_thread::get_id() == thread);
    assert(outputs.size() == 1 && outputs[0].size() == 1 && outputs[0][0].size() == 128);
    assert(parameters.empty());
    assert(inputs.size() == 1);
    connectedInput = !inputs[0].empty();
    if (!inputs[0].empty()) {
      assert(inputs[0][0].size() == 128);
      bool captured = false;
      for (float sample : inputs[0][0]) if (sample > 0.4f) { ++captureSamples; captured = true; }
      if (captured) ++captures;
    }
    for (auto& sample : outputs[0][0]) sample = 0.25f;
    ++processCalls;
    if (fail) throw std::runtime_error("planned processor failure");
    return true;
  }
};

template <typename Predicate> void until(const char* label, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return;
    workers::Context::main()->runPending();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::fprintf(stderr, "timeout: %s; calls=%u captures=%u\n", label, processCalls.load(), captures.load()); std::abort();
}

void testAudioWorkletTypedBridge();

int main() {
  bool invalid = false;
  try { AudioContext bad(0.0); } catch (const std::invalid_argument&) { invalid = true; }
  assert(invalid);
  invalid = false;
  try { audio_worklet::registerProcessor("outside", [] { return std::make_shared<Processor>(); }); }
  catch (const std::logic_error&) { invalid = true; }
  assert(invalid);
  audio_worklet::registerModule("/capture.js", [] {
    thread_local bool evaluated = false;
    assert(!evaluated); evaluated = true;
    ++moduleRuns;
    audio_worklet::registerProcessor("capture", [] { return std::make_shared<Processor>(); });
  });
  AudioContext context(24000.0);
  assert(context.baseLatency() == 0.16 && context.outputLatency() == 0.09);
  context.suspend();
  assert(context.state() == "suspended" && context.sampleRate == 24000);
  context.audioWorklet.addModule("/capture.js");
  context.audioWorklet.addModule("/capture.js");
  assert(moduleRuns == 1);
  const MediaStream stream(media::create_remote_stream());
  const auto track = stream.getAudioTracks()[0];
  auto source = context.createMediaStreamSource(stream);
  AudioWorkletNode node(context, "capture");
  source.connect(node).connect(context.destination);
  std::array<std::int16_t, 320> mic;
  mic.fill(16384);
  media::track_inject_pcm(track.nativeHandle, mic.data(), mic.size());
  context.resume();
  until("capture", [] { return captures > 0; });
  std::array<std::int16_t, 1024> pcm{};
  until("output", [&] { return mixer.mix(pcm.data(), 512) > 0; });
  assert(pcm[0] == 8192 && pcm[1] == 8192); // 24 kHz float -> actual 16 kHz stereo mixer.
  mixer.didWrite(true);
  assert(double(context.currentTime) > 0);

  // Recover a backlog larger than the former 100 ms discard threshold. Keep
  // the hardware mixer draining while every real resampled sample is counted.
  context.suspend();
  source.disconnect(); source.connect(node);
  captureSamples = 0;
  std::array<std::int16_t, 2048> batch; batch.fill(16384);
  media::track_inject_pcm(track.nativeHandle, batch.data(), batch.size());
  context.resume();
  until("capture backlog retained", [&] {
    mixer.mix(pcm.data(), 512); mixer.didWrite(true);
    return captureSamples >= 3072; // Exactly 2048 hardware -> 3072 context samples.
  });
  assert(captureSamples == 3072);
  // A sub-quantum AEC tail survives several render calls without partial zero
  // padding. The following batch completes it, preserving all original samples.
  media::track_inject_pcm(track.nativeHandle, batch.data(), 20);
  for (int i = 0; i < 15; ++i) {
    mixer.mix(pcm.data(), 512); mixer.didWrite(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(captureSamples == 3072);
  media::track_inject_pcm(track.nativeHandle, batch.data(), 236);
  until("partial capture retained", [&] {
    mixer.mix(pcm.data(), 512); mixer.didWrite(true);
    return captureSamples >= 3456;
  });
  assert(captureSamples == 3456);

  // Cancel converted speaker samples at a message boundary, preserving the
  // microphone reader, render clock, processor and continuous mixer source.
  node.port.postMessage("flush");
  until("native playback cancellation", [] { return flushes == 1; });
  pcm.fill(0);
  assert(mixer.mix(pcm.data(), 512) == 0);
  assert(context.state() == "running" && track.readyState() == "live");
  const auto beforeCancellationCapture = captures.load();
  media::track_inject_pcm(track.nativeHandle, mic.data(), mic.size());
  until("capture after playback cancellation", [&] { return captures > beforeCancellationCapture; });
  assert(mixer.mix(pcm.data(), 512) > 0);
  mixer.didWrite(true);

  // A port transferred to the worklet talks directly to a network realm. Main
  // does not run any event pump while this roundtrip happens.
  auto network = workers::Context::create("test_network");
  workers::MessageChannel direct;
  direct.port1.adopt(network);
  std::atomic<bool> delivered{false};
  network->start([&] {
    direct.port1.setOnMessage([&](workers::MessageEvent& event) {
      assert(std::get<std::string>(event.data.value) == "ping-audio"); delivered = true;
    });
  });
  node.port.postMessage("attach", {direct.port2});
  assert(direct.port2.detached());
  direct.port1.postMessage("ping");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!delivered && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(delivered);
  direct.port1.close(); network->stop();

  source.disconnect();
  until("disconnect", [] { return !connectedInput; });
  context.suspend();
  const auto time = double(context.currentTime);
  std::this_thread::sleep_for(std::chrono::milliseconds(12));
  assert(double(context.currentTime) == time);
  pcm.fill(0); assert(mixer.mix(pcm.data(), 512) == 0);
  context.resume();
  bool errored = false;
  bool handlerDestroyed = false;
  struct HandlerLifetime {
    bool& destroyed;
    ~HandlerLifetime() { assert(std::this_thread::get_id() == mainThread); destroyed = true; }
  };
  auto lifetime = std::make_shared<HandlerLifetime>(HandlerLifetime{handlerDestroyed});
  handlerDestroyed = false;
  node.setOnProcessorError([&, lifetime](const std::string& error) { assert(error == "planned processor failure"); errored = true; });
  lifetime.reset();
  node.port.postMessage("fail");
  until("processorerror", [&] { return errored; });
  const auto calls = processCalls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(12));
  assert(processCalls == calls && !handlerDestroyed);
  node.setOnProcessorError({});
  assert(handlerDestroyed);
  context.close(); context.close();
  assert(context.state() == "closed" && !mixer.active());
  assert(destroyed == 1 && isolated);
  invalid = false;
  try { context.resume(); } catch (const std::runtime_error&) { invalid = true; }
  assert(invalid);

  // The same URL executes once in each independent context, with distinct TLS.
  AudioContext other(24000.0);
  other.audioWorklet.addModule("/capture.js");
  AudioWorkletNode second(other, "capture");
  assert(moduleRuns == 2);
  other.close(); assert(destroyed == 2);
  track.stop(); media::destroy_stream(stream.nativeHandle);
  assert(isolated);
  testAudioWorkletTypedBridge();
  std::puts("audio worklet native task, typed bridge, PCM, direct ports and lifecycle: PASS");
}
