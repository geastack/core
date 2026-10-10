#include "audio_oscillator.h"
#include "audio_stream.h"
#include "host/audio.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <thread>

namespace {
gea::platform::audio::PcmStreamMixer mixer;
std::atomic<unsigned> capturedFrames{0};
std::atomic<unsigned> outputsAttached{0};
std::atomic<unsigned> alternatingCalls{0}, longestBurst{0};
double physicalNow = 1000;
gea::platform::audio::FiniteOscillatorSchedule oscillatorSchedule;
unsigned submittedTones = 0;
int submittedDuration = 0;
} // namespace

namespace gea::platform::audio {
std::uint64_t AudioSystem::playPcmStream(
    std::function<std::size_t(std::int16_t *, std::size_t)> pull) {
  ++outputsAttached;
  return mixer.add(std::move(pull));
}
void AudioSystem::stopPcmStream(std::uint64_t id) { mixer.remove(id); }
bool AudioSystem::pcmStreamSettled(std::uint64_t id) {
  return mixer.settled(id);
}
void AudioSystem::flushPlayback() {}
double AudioSystem::processingLatency() { return 512.0 / deviceSampleRate; }
double AudioSystem::outputLatency() { return 1440.0 / deviceSampleRate; }
AudioContext AudioSystem::sharedContext() { return {}; }
AudioNode::AudioNode(NativeAudioHandle id) : native_(id) {}
NativeAudioHandle AudioNode::nativeId() const { return native_; }
AudioDestinationNode::AudioDestinationNode(NativeAudioHandle id)
    : AudioNode(id) {}
AudioDestinationNode AudioContext::destination() const {
  return AudioDestinationNode(1);
}
double AudioContext::currentTime() const { return physicalNow; }
AudioParam::AudioParam(NativeAudioHandle id) : oscillator_(id) {}
OscillatorNode::OscillatorNode(NativeAudioHandle id)
    : AudioNode(id), frequency(id) {}
OscillatorNode AudioContext::createOscillator() const {
  oscillatorSchedule = FiniteOscillatorSchedule{};
  return OscillatorNode(1);
}
void OscillatorNode::start(double when) {
  oscillatorSchedule.start(when, physicalNow);
}
void OscillatorNode::stop(double when) {
  if (const auto timing = oscillatorSchedule.stop(when, physicalNow)) {
    ++submittedTones;
    submittedDuration = timing->durationMs;
  }
}
} // namespace gea::platform::audio

namespace gea::host::media {
void platform_attach_track(NativeMediaTrackHandle) {}
void platform_detach_track(NativeMediaTrackHandle) {}
} // namespace gea::host::media

namespace gea::platform::storage {
bool ensureMounted() { return false; }
} // namespace gea::platform::storage

using namespace gea::host;

struct CaptureProcessor final : AudioWorkletProcessor {
  bool process(const AudioWorkletBus &inputs, AudioWorkletBus &outputs,
               const AudioWorkletParameters &) override {
    assert(audio_worklet::sampleRate() ==
           gea::platform::audio::deviceSampleRate);
    for (const auto sample : inputs[0][0])
      assert(sample == 0.5f);
    capturedFrames += inputs[0][0].size();
    std::fill(outputs[0][0].begin(), outputs[0][0].end(), 0.25f);
    return true;
  }
};

struct InputOnlyProcessor final : AudioWorkletProcessor {
  bool process(const AudioWorkletBus &inputs, AudioWorkletBus &outputs,
               const AudioWorkletParameters &) override {
    assert(outputs.empty());
    assert(inputs.size() == 1 && inputs[0].size() == 1);
    for (const auto sample : inputs[0][0])
      assert(sample == 0.5f);
    capturedFrames += inputs[0][0].size();
    return true;
  }
};

struct AlternatingProcessor final : AudioWorkletProcessor {
  unsigned burst = 0;
  std::chrono::steady_clock::time_point lastEnd;
  bool process(const AudioWorkletBus &inputs, AudioWorkletBus &outputs,
               const AudioWorkletParameters &) override {
    assert(outputs.empty() && inputs[0][0].size() == 128);
    const auto now = std::chrono::steady_clock::now();
    burst = lastEnd.time_since_epoch().count() &&
                    now - lastEnd < std::chrono::microseconds(800)
                ? burst + 1
                : 1;
    longestBurst = std::max(longestBurst.load(), burst);
    const auto call = ++alternatingCalls;
    // One FFT-sized call exceeds a quantum; the following call is cheap. A
    // one-quantum batch budget inserts a forced tick after every heavy call,
    // preventing a run of three calls even with capture already available.
    if (call & 1)
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    lastEnd = std::chrono::steady_clock::now();
    return true;
  }
};

void waitForFrames(unsigned expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (capturedFrames < expected &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(capturedFrames == expected);
}

int main() {
  constexpr auto rate = gea::platform::audio::deviceSampleRate;
  const MediaStream stream(media::create_remote_stream());
  const auto track = stream.getAudioTracks()[0];
  std::array<std::int16_t, rate / 50> samples;
  samples.fill(16384);

  // Twenty milliseconds stays twenty milliseconds at both native and wire
  // rates.
  auto pcm = PcmAudioStream::create(rate);
  pcm.setInput(stream);
  media::track_inject_pcm(track.nativeHandle, samples.data(), samples.size());
  assert(pcm.capturePendingMs() == 20);
  const auto packet = pcm.readBase64();
  pcm.writeBase64(packet);
  assert(pcm.queuedMs() == 20);
  assert(pcm.capturePendingMs() == 0);
  pcm.close();

  auto wire = PcmAudioStream::create(16000);
  wire.setInput(stream);
  media::track_inject_pcm(track.nativeHandle, samples.data(), samples.size());
  wire.writeBase64(wire.readBase64());
  assert(std::abs(wire.queuedMs() - 20) < 0.1);
  wire.close();

  audio_worklet::registerModule("/capture-44100.js", [] {
    audio_worklet::registerProcessor(
        "capture", [] { return std::make_shared<CaptureProcessor>(); });
  });
  AudioContext context{double(rate)};
  assert(std::abs(context.baseLatency() - 2560.0 / rate) < 1e-12);
  context.suspend();
  context.audioWorklet.addModule("/capture-44100.js");
  auto source = context.createMediaStreamSource(stream);
  AudioWorkletNode node(context, "capture");
  source.connect(node).connect(context.destination);
  std::array<std::int16_t, 1024> input;
  input.fill(16384);
  media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  context.resume();
  std::array<std::int16_t, 1024> output{};
  bool heard = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((capturedFrames < input.size() || !heard) &&
         std::chrono::steady_clock::now() < deadline) {
    output.fill(0);
    if (mixer.mix(output.data(), 512)) {
      assert(output[0] == 8192 && output[1] == 8192);
      heard = true;
      mixer.didWrite(true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(capturedFrames == input.size() && heard);
  context.close();

  // A microphone sink advances on input without ever attaching a DAC stream.
  const auto outputsBefore = outputsAttached.load();
  capturedFrames = 0;
  audio_worklet::registerModule("/input-only.js", [] {
    audio_worklet::registerProcessor(
        "sink", [] { return std::make_shared<InputOnlyProcessor>(); });
  });
  AudioContext sinkContext{double(rate)};
  sinkContext.suspend();
  sinkContext.audioWorklet.addModule("/input-only.js");
  auto sinkSource = sinkContext.createMediaStreamSource(stream);
  AudioWorkletNode sink(sinkContext, "sink", AudioWorkletNodeOptions{1, 0, 1});
  bool rejected = false;
  try {
    sink.connect(sinkContext.destination);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
  sinkSource.connect(sink);
  media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  sinkContext.resume();
  waitForFrames(input.size());
  assert(outputsAttached == outputsBefore);

  // The same context's worklet clock starts near zero, while physical output
  // is at uptime 1000. Host oscillator deadlines preserve a 20 ms voice.
  auto beep = sinkContext.createOscillator();
  const double now = sinkContext.currentTime;
  beep.start(now);
  beep.stop(now + 0.020);
  assert(submittedTones == 1 && submittedDuration == 20);
  physicalNow += 1;
  beep.stop();
  assert(submittedTones == 1);

  sinkSource.disconnect();
  const auto paused = capturedFrames.load();
  media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(capturedFrames == paused);
  sinkSource.connect(sink);
  sink.disconnect(); // No outgoing edge; the live incoming edge stays active.
  media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  waitForFrames(paused + input.size());
  sinkContext.close();
  media::track_inject_pcm(track.nativeHandle, input.data(), input.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(capturedFrames == paused + input.size());
  assert(outputsAttached == outputsBefore);
  if (rate == 44100) {
    audio_worklet::registerModule("/alternating.js", [] {
      audio_worklet::registerProcessor("alternating", [] {
        return std::make_shared<AlternatingProcessor>();
      });
    });
    AudioContext alternatingContext{double(rate)};
    alternatingContext.suspend();
    alternatingContext.audioWorklet.addModule("/alternating.js");
    auto alternatingSource = alternatingContext.createMediaStreamSource(stream);
    AudioWorkletNode alternating(alternatingContext, "alternating",
                                 AudioWorkletNodeOptions{1, 0, 1});
    alternatingSource.connect(alternating);
    std::array<std::int16_t, 8192> backlog{};
    media::track_inject_pcm(track.nativeHandle, backlog.data(), backlog.size());
    alternatingContext.resume();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (alternatingCalls < 64 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(alternatingCalls == 64);
    assert(longestBurst >= 3 && longestBurst <= 4);
    alternatingContext.close();
  }
  track.stop();
}
