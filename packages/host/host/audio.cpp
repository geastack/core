// SPDX-License-Identifier: Apache-2.0
#include "audio.h"
#include "audio_oscillator.h"
#include "host/audio_level.h"
#include "host/pcm_stream.h"
#include "host/websocket.h"
#include <deque>
#include <mutex>
#include <chrono>
#include "gea/embedded-host.h"
#include "ui/document.h"  // host/audio.cpp HTMLAudioElement bridge (entangled — moves to elements)

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <limits>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

// Platforms without the streaming output engine reject attachment. The ESP
// audio runtime supplies the strong definitions through its existing task.
namespace gea::platform::audio {
__attribute__((weak)) std::uint64_t AudioSystem::playPcmStream(std::function<std::size_t(std::int16_t*, std::size_t)>) { return 0; }
__attribute__((weak)) void AudioSystem::stopPcmStream(std::uint64_t) {}
__attribute__((weak)) bool AudioSystem::pcmStreamSettled(std::uint64_t) { return false; }
__attribute__((weak)) double AudioSystem::processingLatency() { return 0; }
__attribute__((weak)) double AudioSystem::outputLatency() { return 0; }
}

namespace gea::framework::host {

class AudioHost {
 public:
  static gea::platform::audio::OscillatorType oscillatorType(double value) {
    switch (static_cast<int>(value)) {
      case 1: return gea::platform::audio::OscillatorType::Square;
      case 2: return gea::platform::audio::OscillatorType::Sawtooth;
      case 3: return gea::platform::audio::OscillatorType::Triangle;
      default: return gea::platform::audio::OscillatorType::Sine;
    }
  }

  static gea::platform::audio::AudioContext context() {
    return gea::platform::audio::AudioSystem::sharedContext();
  }

  static gea::platform::audio::OscillatorNode oscillator(gea::host::NativeAudioHandle oscillator) {
    return gea::platform::audio::OscillatorNode(oscillator);
  }

  static gea::platform::audio::AudioDestinationNode destination(gea::host::NativeAudioHandle destination) {
    return gea::platform::audio::AudioDestinationNode(destination);
  }
};

}  // namespace gea::framework::host

namespace gea::framework::audio {

double AudioBackend::volume() {
  return static_cast<double>(gea::platform::audio::AudioSystem::volume());
}

void AudioBackend::setVolume(double volume) {
  gea::platform::audio::AudioSystem::setVolume(static_cast<int>(volume));
}

}  // namespace gea::framework::audio

namespace gea::host {

namespace {

std::uint16_t le16(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  if (offset + 2 > bytes.size())
    return 0;
  return static_cast<std::uint16_t>(
      bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t le32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  if (offset + 4 > bytes.size()) return 0;
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool tagAt(const std::vector<std::uint8_t> &bytes, std::size_t offset, const char *tag) {
  return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, tag, 4) == 0;
}

AudioBuffer decodePcm16Wav(const std::vector<std::uint8_t> &bytes) {
  if (bytes.size() < 44 || !tagAt(bytes, 0, "RIFF") || !tagAt(bytes, 8, "WAVE")) return {};

  int channels = 1;
  int sampleRate = 16000;
  std::size_t dataOffset = 0;
  std::size_t dataBytes = 0;
  std::uint16_t format = 0;
  std::uint16_t bitsPerSample = 0;

  std::size_t cursor = 12;
  while (cursor + 8 <= bytes.size()) {
    const std::uint32_t chunkSize = le32(bytes, cursor + 4);
    const std::size_t payload = cursor + 8;
    if (payload + chunkSize > bytes.size()) break;

    if (tagAt(bytes, cursor, "fmt ") && chunkSize >= 16) {
      format = le16(bytes, payload);
      channels = static_cast<int>(le16(bytes, payload + 2));
      sampleRate = static_cast<int>(le32(bytes, payload + 4));
      bitsPerSample = le16(bytes, payload + 14);
    } else if (tagAt(bytes, cursor, "data")) {
      dataOffset = payload;
      dataBytes = chunkSize;
    }

    cursor = payload + chunkSize + (chunkSize & 1u);
  }

  if (format != 1 || bitsPerSample != 16 || dataOffset == 0 || dataBytes < 2) return {};
  if (channels <= 0) channels = 1;
  if (sampleRate <= 0) sampleRate = 16000;

  const std::size_t sampleCount = dataBytes / sizeof(std::int16_t);
  std::vector<std::int16_t> samples(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    const std::size_t off = dataOffset + i * 2;
    samples[i] = static_cast<std::int16_t>(le16(bytes, off));
  }
  return AudioBuffer(std::move(samples), sampleRate, channels);
}

} // namespace

AudioDestinationProperty::operator AudioDestinationNode() const {
  AudioDestinationNode result(gea::framework::host::AudioHost::context().destination().nativeId());
  result.context = context;
  return result;
}

AudioDestinationProperty::operator double() const {
  return static_cast<double>(gea::framework::host::AudioHost::context().destination().nativeId());
}

AudioContextCurrentTimeProperty::operator double() const {
  if (context) return audio_worklet::contextTime(context);
  return gea::framework::host::AudioHost::context().currentTime();
}

AudioContext::AudioContext(double rate)
    : state_(audio_worklet::createContext(rate)), sampleRate(rate),
      destination{state_}, currentTime{state_}, audioWorklet(state_) {}
double AudioContext::baseLatency() const { return audio_worklet::contextBaseLatency(state_); }
double AudioContext::outputLatency() const { return gea::platform::audio::AudioSystem::outputLatency(); }
std::string AudioContext::state() const { return audio_worklet::contextState(state_); }
void AudioContext::resume() const { audio_worklet::resume(state_); }
void AudioContext::suspend() const { audio_worklet::suspend(state_); }
void AudioContext::close() const { audio_worklet::close(state_); }
MediaStreamAudioSourceNode AudioContext::createMediaStreamSource(MediaStream stream) const {
  return MediaStreamAudioSourceNode(state_, stream);
}

const AudioParamValueProperty &AudioParamValueProperty::operator=(double frequency_hz) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.setValue(frequency_hz);
  return *this;
}

AudioParamValueProperty::operator double() const {
  return gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.value();
}

const AudioParam &AudioParam::operator=(double frequency_hz) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.setValue(frequency_hz);
  return *this;
}

void AudioParam::setValueAtTime(double frequency_hz, double start_time) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).frequency.setValueAtTime(frequency_hz, start_time);
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(double type) const {
  gea::framework::host::AudioHost::oscillator(oscillatorHandle).setType(gea::framework::host::AudioHost::oscillatorType(type));
  return *this;
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const char *type) const {
  return (*this = detail::oscillator_type_from_name(type));
}

const OscillatorTypeProperty &OscillatorTypeProperty::operator=(const std::string &type) const {
  return (*this = type.c_str());
}

void OscillatorNode::connect(AudioDestinationNode destination) const {
  gea::framework::host::AudioHost::oscillator(nativeHandle).connect(gea::framework::host::AudioHost::destination(destination.nativeHandle));
}

void OscillatorNode::connect(AudioDestinationProperty destination) const {
  connect(static_cast<AudioDestinationNode>(destination));
}

void OscillatorNode::connect(double destinationHandle) const {
  connect(AudioDestinationNode(destinationHandle));
}

void OscillatorNode::start(double when) const {
  if (context) {
    clockOffset = gea::framework::host::AudioHost::context().currentTime() - audio_worklet::contextTime(context);
    clockAligned = true;
    if (when > 0) when += clockOffset;
  }
  gea::framework::host::AudioHost::oscillator(nativeHandle).start(when);
}

void OscillatorNode::stop(double when) const {
  if (context && when > 0) {
    if (clockAligned) when += clockOffset;
    else when = gea::platform::audio::physicalAudioDeadline(when, audio_worklet::contextTime(context),
        gea::framework::host::AudioHost::context().currentTime());
  }
  gea::framework::host::AudioHost::oscillator(nativeHandle).stop(when);
}

AudioDestinationNode AudioBufferSourceNode::connect(AudioDestinationNode destination) const {
  connected = true;
  return destination;
}

AudioDestinationNode AudioBufferSourceNode::connect(AudioDestinationProperty destination) const {
  return connect(static_cast<AudioDestinationNode>(destination));
}

AudioDestinationNode AudioBufferSourceNode::connect(double destinationHandle) const {
  return connect(AudioDestinationNode(destinationHandle));
}

void AudioBufferSourceNode::start(double when) const {
  (void)when;
  if (!connected)
    return;
  const auto &samples = buffer.pcmSamples();
  if (samples.empty())
    return;
  gea::platform::audio::AudioSystem::playPcm(
      samples.data(), samples.size(), buffer.sampleRate, buffer.channels);
}

void AudioBufferSourceNode::stop(double when) const {
  (void)when;
  gea::platform::audio::AudioSystem::stopPlayback();
}

OscillatorNode AudioContext::createOscillator() const {
  OscillatorNode oscillator(gea::framework::host::AudioHost::context().createOscillator().nativeId());
  oscillator.context = state_;
  return oscillator;
}

AudioBufferSourceNode AudioContext::createBufferSource() const {
  return AudioBufferSourceNode(true);
}

AudioBuffer
AudioContext::decodeAudioData(const std::vector<std::uint8_t> &bytes) const {
  return decodePcm16Wav(bytes);
}

struct HTMLAudioElement::State {
  static std::unordered_map<int, std::weak_ptr<State>>& registry() {
    static std::unordered_map<int, std::weak_ptr<State>> states;
    return states;
  }
  int nodeId = -1;
  std::string src;
  MediaStream source;
  std::uint64_t output = 0;
  std::shared_ptr<media::TrackPcmReader> reader;
  std::shared_ptr<AudioLevelMeter> level = std::make_shared<AudioLevelMeter>();
  bool autoplay = false, paused = true, freshSource = true, draining = false;
  ~State() { if (output) gea::platform::audio::AudioSystem::stopPcmStream(output); }
};

HTMLAudioElement HTMLAudioElement::create() {
  HTMLAudioElement result;
  result.state_ = std::make_shared<State>();
  return result;
}
HTMLAudioElement HTMLAudioElement::create(const std::string& src) { return HTMLAudioElement(src); }

HTMLAudioElement::HTMLAudioElement(const char *src) : HTMLAudioElement(std::string(src ? src : "")) {}

HTMLAudioElement::HTMLAudioElement(const std::string &src) : state_(std::make_shared<State>()) { state_->src = src; }

HTMLAudioElement::HTMLAudioElement(const gea::embedded::ui::NodeHandle &node) {
  if (!node.valid()) return;
  auto& entry = State::registry()[node.id()];
  state_ = entry.lock();
  if (!state_) {
    state_ = std::make_shared<State>();
    state_->nodeId = node.id();
    entry = state_;
  }
}

std::string HTMLAudioElement::src() const {
  if (!state_) return {};
  if (state_->nodeId >= 0) return std::string(gea::embedded::ui::NodeHandle(state_->nodeId).getAttribute("src"));
  return state_->src;
}

void HTMLAudioElement::setSrc(const std::string &src) {
  if (!state_) throw std::logic_error("null HTMLAudioElement");
  if (state_->nodeId >= 0) gea::embedded::ui::NodeHandle(state_->nodeId).setAttribute("src", src.c_str());
  state_->src = src;
}

bool HTMLAudioElement::play() const {
  if (!state_) return false;
  if (state_->draining) pause();
  if (state_->output) return true;
  if (state_->source.nativeHandle) {
    const auto tracks = state_->source.getAudioTracks();
    if (tracks.empty() || tracks.front().readyState() == "ended") return false;
    auto reader = state_->reader;
    if (!reader) reader = std::make_shared<media::TrackPcmReader>(tracks.front().nativeHandle, state_->freshSource);
    state_->reader = reader;
    auto level = state_->level;
    state_->output = gea::platform::audio::AudioSystem::playPcmStream(
      [reader, level](std::int16_t* samples, std::size_t count) {
        const auto read = reader->read(samples, count);
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        level->update(samples, read, static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
        return read;
      });
    if (!state_->output) { state_->reader.reset(); return false; }
    state_->freshSource = false;
    state_->paused = false;
    return true;
  }
  const bool ok = gea::platform::audio::AudioSystem::playFile(src());
  state_->paused = !ok;
  return ok;
}

void HTMLAudioElement::pause() const {
  if (!state_) return;
  state_->draining = false;
  if (state_->output) {
#ifdef ESP_PLATFORM
    ESP_LOGI("gea::host::audio", "pause: queued samples=%u dropped=%u",
      static_cast<unsigned>(state_->reader ? state_->reader->pendingSamples() : 0),
      static_cast<unsigned>(state_->reader ? state_->reader->droppedSamples() : 0));
#endif
    gea::platform::audio::AudioSystem::stopPcmStream(state_->output);
    state_->output = 0;
  } else if (!state_->paused && !state_->source.nativeHandle) gea::platform::audio::AudioSystem::stopPlayback();
  // Discard the cancelled reply at the interruption boundary, not when
  // play() later resumes: new RTP can precede its SCTP response event.
  if (state_->reader) state_->reader->discardBuffered();
  state_->level->reset();
  state_->paused = true;
}

void HTMLAudioElement::clearBufferedAudio() const {
  if (!state_) return;
  if (state_->reader) state_->reader->discardBuffered();
  state_->draining = false;
  state_->level->reset();
}

double HTMLAudioElement::audioLevel() const {
  if (!state_ || state_->paused) return 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return state_->level->read(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()));
}

void HTMLAudioElement::beginDrain() const {
  if (!state_ || !state_->reader || state_->draining) return;
  state_->draining = true;
  state_->reader->beginDrain();
}

bool HTMLAudioElement::drained() const {
  if (!state_ || state_->paused) return true;
  return state_->draining && state_->reader && state_->reader->pendingSamples() == 0 &&
    gea::platform::audio::AudioSystem::pcmStreamSettled(state_->output);
}

MediaStream HTMLAudioElement::srcObject() const { return state_ ? state_->source : MediaStream{}; }
void HTMLAudioElement::setSrcObject(MediaStream stream) const {
  if (!state_) throw std::logic_error("null HTMLAudioElement");
  if (stream.nativeHandle == state_->source.nativeHandle) return;
  const bool resume = !state_->paused || state_->autoplay;
  pause();
  state_->reader.reset();
  state_->source = stream;
  state_->freshSource = true;
  if (stream.nativeHandle && resume) (void)play();
}
bool HTMLAudioElement::autoplay() const { return state_ && state_->autoplay; }
bool HTMLAudioElement::paused() const { return !state_ || state_->paused; }
void HTMLAudioElement::setAutoplay(bool enabled) const {
  if (!state_) throw std::logic_error("null HTMLAudioElement");
  state_->autoplay = enabled;
  if (enabled && state_->source.nativeHandle && state_->paused) (void)play();
}

} // namespace gea::host

namespace gea::host {
namespace {
std::uint32_t pcmNowMs() {
  return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

struct PcmAudioStream::State {
  explicit State(int rate)
      : capture(gea::platform::audio::deviceSampleRate, rate),
        playback(rate, gea::platform::audio::deviceSampleRate), wireRate(rate) {}
  ~State() { if (output) gea::platform::audio::AudioSystem::stopPcmStream(output); }
  std::shared_ptr<media::TrackPcmReader> reader;
  std::mutex captureMutex;
  std::atomic<std::uint32_t> capturePackets{0};
  unsigned receivedPackets = 0;
  pcm::Resampler capture, playback;
  int wireRate;
  std::mutex mutex;
  std::mutex playbackInputMutex;
  using Block = std::vector<std::int16_t, pcm::QueueAllocator<std::int16_t>>;
  std::deque<Block> queue;
  std::size_t queuedSamples = 0, readOffset = 0;
  AudioLevelMeter level;
  pcm::PlaybackStartGate startGate;
  std::uint64_t output = 0, pulled = 0;
  std::uint32_t lastPull = 0;
  std::atomic<bool> closed{false};
};

PcmAudioStream PcmAudioStream::create(double sampleRate) {
  if (sampleRate != 16000 && sampleRate != 24000 && sampleRate != 44100 && sampleRate != 48000)
    throw std::invalid_argument("PCM sample rate must be 16000, 24000, 44100 or 48000");
  PcmAudioStream result;
  result.state_ = std::make_shared<State>(static_cast<int>(sampleRate));
  const std::weak_ptr<State> weak = result.state_;
  result.state_->output = gea::platform::audio::AudioSystem::playPcmStream(
      [weak](std::int16_t *out, std::size_t capacity) {
        const auto state = weak.lock();
        if (!state) return std::size_t{0};
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->startGate.ready(state->queuedSamples, pcmNowMs())) return std::size_t{0};
        const auto count = std::min(capacity, state->queuedSamples);
        std::size_t copied = 0;
        while (copied < count) {
          const auto &block = state->queue.front();
          const auto n = std::min(count - copied, block.size() - state->readOffset);
          std::copy_n(block.data() + state->readOffset, n, out + copied);
          state->readOffset += n;
          copied += n;
          if (state->readOffset == block.size()) {
            state->queue.pop_front();
            state->readOffset = 0;
          }
        }
        state->queuedSamples -= count;
        if (count) {
          state->pulled += count;
          state->lastPull = pcmNowMs();
          state->level.update(out, count, state->lastPull);
        }
        return count;
      });
  if (!result.state_->output) throw std::runtime_error("PCM output unavailable");
  return result;
}

void PcmAudioStream::setInput(MediaStream stream) const {
  if (!state_) throw std::logic_error("Closed PCM stream");
  std::lock_guard<std::mutex> lock(state_->captureMutex);
  if (state_->closed) throw std::logic_error("Closed PCM stream");
  const auto tracks = stream.getAudioTracks();
  if (tracks.empty()) throw std::invalid_argument("PCM input needs an audio track");
  state_->reader = std::make_shared<media::TrackPcmReader>(tracks.front().nativeHandle);
  state_->capture.reset();
}

void PcmAudioStream::pipeTo(WebSocket socket, const std::string &prefix, const std::string &suffix) const {
  if (!state_ || state_->closed) throw std::logic_error("Closed PCM stream");
  const std::weak_ptr<State> weak = state_;
  websocket::setTextProducer(socket.nativeHandle, [weak, prefix, suffix]() -> std::string {
    PcmAudioStream stream;
    stream.state_ = weak.lock();
    if (!stream.state_ || stream.state_->closed) return {};
    if (stream.captureDroppedSamples() > 0)
      throw std::runtime_error("Microphone capture overflowed during native upload");
    auto pcm = stream.readBase64();
    if (pcm.empty() || stream.state_->closed) return {};
    // Base64 has no JSON quote/backslash characters; framing needs no JSON
    // serializer. The provider-specific envelope is supplied by TypeScript.
    std::string packet;
    packet.reserve(prefix.size() + pcm.size() + suffix.size());
    packet.append(prefix).append(pcm).append(suffix);
    const auto sent = ++stream.state_->capturePackets;
#ifdef ESP_PLATFORM
    if (sent % 50 == 0) ESP_LOGI("gea_pcm", "native capture packets=%u pending_ms=%u dropped=%u",
        unsigned(sent), unsigned(stream.capturePendingMs()), unsigned(stream.captureDroppedSamples()));
#endif
    return packet;
  });
}

void PcmAudioStream::receiveFrom(WebSocket socket, const std::string& prefix, const std::string& suffix, double startupMs) const {
  if (!std::isfinite(startupMs) || startupMs < 0 || startupMs > 10000)
    throw std::invalid_argument("PCM startup delay must be between 0 and 10000 ms");
  if (!state_ || state_->closed) throw std::logic_error("Closed PCM stream");
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    // Keep the startup clock across turns. A sender that coordinates both
    // media streams can opt out instead of stacking a second audio-only wait.
    state_->startGate.enable(static_cast<std::uint32_t>(startupMs));
    if (state_->queuedSamples) state_->startGate.pushed(pcmNowMs());
  }
  const std::weak_ptr<State> weak = state_;
  websocket::setTextConsumer(socket.nativeHandle, [weak, prefix, suffix](const std::string& message) {
    if (message.size() < prefix.size() + suffix.size() ||
        !message.starts_with(prefix) || !message.ends_with(suffix)) return false;
    PcmAudioStream stream;
    stream.state_ = weak.lock();
    if (!stream.state_ || stream.state_->closed) return true;
    const auto size = message.size() - prefix.size() - suffix.size();
    if (size > 4096) throw std::runtime_error("PCM packet exceeded native receive bound");
    stream.writeBase64(message.substr(prefix.size(), size));
    return true;
  });
}

std::string PcmAudioStream::readBase64() const {
  const auto started = pcmNowMs();
  if (!state_) return {};
  std::lock_guard<std::mutex> lock(state_->captureMutex);
  // Match the capture driver's 20 ms cadence at its configured device rate.
  constexpr std::size_t captureFrames = gea::platform::audio::deviceSampleRate / 50;
  if (state_->closed || !state_->reader || state_->reader->pendingSamples() < captureFrames) return {};
  std::int16_t input[captureFrames];
  const auto count = state_->reader->read(input, captureFrames);
  const auto readAt = pcmNowMs();
  // When the network and device clocks match, encode the capture block
  // directly rather than allocating and visiting a resampler for each sample.
  if (state_->wireRate == gea::platform::audio::deviceSampleRate) return pcm::encode(input, count);
  std::vector<std::int16_t> wire;
  wire.reserve(1920);
  for (std::size_t i = 0; i < count; ++i)
    state_->capture.push(input[i], [&](std::int16_t sample) { wire.push_back(sample); });
  const auto resampledAt = pcmNowMs();
  auto encoded = pcm::encode(wire);
#ifdef ESP_PLATFORM
  if (pcmNowMs() - started > 15) ESP_LOGW("gea_pcm", "capture read_ms=%u resample_ms=%u encode_ms=%u", readAt-started, resampledAt-readAt, pcmNowMs()-resampledAt);
#endif
  return encoded;
}

void PcmAudioStream::writeBase64(const std::string &data) const {
  if (!state_) return;
  std::lock_guard inputLock(state_->playbackInputMutex);
  if (state_->closed) return;
  const auto started = pcmNowMs();
  const auto wire = pcm::decode(data);
  const auto decodedAt = pcmNowMs();
  // Resampling and allocation never hold the real-time mixer's queue mutex.
  State::Block block;
  if (state_->wireRate == gea::platform::audio::deviceSampleRate) {
    block.assign(wire.begin(), wire.end());
  } else {
    block.reserve(wire.size() * gea::platform::audio::deviceSampleRate / state_->wireRate + 2);
    for (auto sample : wire)
      state_->playback.push(sample, [&](std::int16_t value) { block.push_back(value); });
  }
  unsigned packets, queuedMs;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    // Generated output may run faster than playback. Never silently overwrite
    // the unplayed tail (MediaStream's live capture ring has different semantics).
    if (state_->queuedSamples + block.size() > gea::platform::audio::deviceSampleRate * 30)
      throw std::runtime_error("PCM playback exceeded 30 second queue limit");
    if (!block.empty()) {
      state_->startGate.pushed(pcmNowMs());
      state_->queuedSamples += block.size();
      state_->queue.push_back(std::move(block));
    }
    packets = ++state_->receivedPackets;
    queuedMs = unsigned(state_->queuedSamples * 1000 / gea::platform::audio::deviceSampleRate);
  }
  // Console formatting and sinks must never hold the speaker's queue lock.
#ifdef ESP_PLATFORM
  if (packets == 1 || packets % 100 == 0)
    ESP_LOGI("gea_pcm", "native receive packets=%u queued_ms=%u", packets, queuedMs);
  if (pcmNowMs()-started > 15) ESP_LOGW("gea_pcm", "playback samples=%u decode_ms=%u resample_queue_ms=%u", unsigned(wire.size()), decodedAt-started, pcmNowMs()-decodedAt);
#endif
}

void PcmAudioStream::resetPlayback(bool interrupted) const {
  if (!state_) return;
  std::lock_guard inputLock(state_->playbackInputMutex);
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->queue.clear();
  state_->queuedSamples = state_->readOffset = 0;
  state_->playback.reset();
  state_->startGate.reset();
  state_->pulled = 0;
  state_->lastPull = 0;
  state_->level.reset();
  if (interrupted) gea::platform::audio::AudioSystem::flushPlayback();
}

void PcmAudioStream::close() const {
  if (!state_ || state_->closed.exchange(true)) return;
  if (state_->output) gea::platform::audio::AudioSystem::stopPcmStream(state_->output);
  state_->output = 0;
  gea::platform::audio::AudioSystem::flushPlayback();
  {
    std::lock_guard<std::mutex> lock(state_->captureMutex);
    state_->reader.reset();
  }
  resetPlayback();
}

double PcmAudioStream::queuedMs() const {
  if (!state_) return 0;
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->queuedSamples * 1000.0 / gea::platform::audio::deviceSampleRate;
}

double PcmAudioStream::playedMs() const {
  if (!state_) return 0;
  std::lock_guard<std::mutex> lock(state_->mutex);
  // Pulling submits up to 32ms to the mixer, ahead of up to 90ms of I2S DMA.
  // This conservative bound never reports queued network data as heard audio.
  const auto elapsed = static_cast<std::uint32_t>(pcmNowMs() - state_->lastPull);
  const double inFlight = elapsed >= 122 ? 0 : 122 - elapsed;
  return std::max(0.0, state_->pulled * 1000.0 / gea::platform::audio::deviceSampleRate - inFlight);
}

bool PcmAudioStream::drained() const {
  if (!state_ || state_->closed) return true;
  bool empty;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    empty = state_->queue.empty() && (!state_->pulled ||
        static_cast<std::uint32_t>(pcmNowMs() - state_->lastPull) >= 122);
  }
  return empty && gea::platform::audio::AudioSystem::pcmStreamSettled(state_->output);
}

double PcmAudioStream::audioLevel() const { return state_ ? state_->level.read(pcmNowMs()) : 0; }
double PcmAudioStream::capturePendingMs() const {
  if (!state_) return 0;
  std::lock_guard<std::mutex> lock(state_->captureMutex);
  return state_->reader ? state_->reader->pendingSamples() * 1000.0 / gea::platform::audio::deviceSampleRate : 0;
}
double PcmAudioStream::captureDroppedSamples() const {
  if (!state_) return 0;
  std::lock_guard<std::mutex> lock(state_->captureMutex);
  return state_->reader ? state_->reader->droppedSamples() : 0;
}
double PcmAudioStream::capturePackets() const { return state_ ? state_->capturePackets.load() : 0; }
} // namespace gea::host
