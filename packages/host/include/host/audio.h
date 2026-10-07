// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <audio.h>
#include "backends.h"
#include "host/media.h"
#include "host/audio_worklet.h"
#include <memory>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gea {
template <typename T> struct Ref;
template <typename T> class TypedArray;
} // namespace gea

namespace gea::embedded::ui {
class NodeHandle;
}

namespace gea::host {

using NativeAudioHandle = gea::platform::audio::NativeAudioHandle;

namespace detail {
inline double oscillator_type_from_name(const char *type) {
  if (type == nullptr) return 0.0;
  if (std::strcmp(type, "square") == 0) return 1.0;
  if (std::strcmp(type, "sawtooth") == 0) return 2.0;
  if (std::strcmp(type, "triangle") == 0) return 3.0;
  return 0.0;
}
}  // namespace detail

struct AudioDestinationNode {
  NativeAudioHandle nativeHandle = 0;
  std::shared_ptr<audio_worklet::ContextState> context;

  AudioDestinationNode() = default;
  explicit AudioDestinationNode(NativeAudioHandle destination) : nativeHandle(destination) {}
  explicit AudioDestinationNode(double destination) : nativeHandle(static_cast<NativeAudioHandle>(destination)) {}

  constexpr operator double() const { return static_cast<double>(nativeHandle); }
};

struct AudioDestinationProperty {
  std::shared_ptr<audio_worklet::ContextState> context;
  operator AudioDestinationNode() const;
  operator double() const;
};

struct AudioContextCurrentTimeProperty {
  std::shared_ptr<audio_worklet::ContextState> context;
  operator double() const;
};

struct AudioBufferStorage;

struct AudioBuffer {
  std::vector<std::int16_t> samples;
  int sampleRate = 16000;
  int channels = 1;
  mutable std::shared_ptr<AudioBufferStorage> storage;

  AudioBuffer() = default;
  AudioBuffer(std::vector<std::int16_t> pcm, int rate, int channelCount)
      : samples(std::move(pcm)), sampleRate(rate), channels(channelCount) {
    ensureStorage();
  }

  double getLength() const;
  double getDuration() const;
  double getNumberOfChannels() const;
  gea::Ref<gea::TypedArray<float>> getChannelData(double channel) const;
  const std::vector<std::int16_t> &pcmSamples() const;

  template <typename Source>
  void copyToChannel(const Source &source, double channel,
                     double offset = 0) const {
    if constexpr (requires {
                    source.get();
                    *source;
                  }) {
      copyToChannel(*source, channel, offset);
    } else {
      copyToChannelValues(source.data(), source.size(), channel, offset);
    }
  }

  template <typename Destination>
  void copyFromChannel(const Destination &destination, double channel,
                       double offset = 0) const {
    if constexpr (requires {
                    destination.get();
                    *destination;
                  }) {
      copyFromChannelValues(destination->data(), destination->size(), channel,
                            offset);
    } else {
      copyFromChannelValues(destination.data(), destination.size(), channel,
                            offset);
    }
  }

private:
  void ensureStorage() const;
  void copyToChannelValues(const float *source, std::size_t count,
                           double channel, double offset) const;
  void copyFromChannelValues(float *destination, std::size_t count,
                             double channel, double offset) const;
};

struct AudioParamValueProperty {
  NativeAudioHandle oscillatorHandle = 0;

  constexpr AudioParamValueProperty() = default;
  explicit constexpr AudioParamValueProperty(NativeAudioHandle handle) : oscillatorHandle(handle) {}
  explicit constexpr AudioParamValueProperty(double handle) : oscillatorHandle(static_cast<NativeAudioHandle>(handle)) {}

  const AudioParamValueProperty &operator=(double frequency_hz) const;
  operator double() const;
};

struct AudioParam {
  NativeAudioHandle oscillatorHandle = 0;
  mutable AudioParamValueProperty value;

  constexpr AudioParam() = default;
  explicit constexpr AudioParam(NativeAudioHandle handle) : oscillatorHandle(handle), value(handle) {}
  explicit constexpr AudioParam(double handle) : oscillatorHandle(static_cast<NativeAudioHandle>(handle)), value(handle) {}

  const AudioParam &operator=(double frequency_hz) const;
  void setValueAtTime(double frequency_hz, double start_time) const;
};

struct OscillatorTypeProperty {
  NativeAudioHandle oscillatorHandle = 0;

  constexpr OscillatorTypeProperty() = default;
  explicit constexpr OscillatorTypeProperty(NativeAudioHandle handle) : oscillatorHandle(handle) {}
  explicit constexpr OscillatorTypeProperty(double handle) : oscillatorHandle(static_cast<NativeAudioHandle>(handle)) {}

  const OscillatorTypeProperty &operator=(double type) const;
  const OscillatorTypeProperty &operator=(const char *type) const;
  const OscillatorTypeProperty &operator=(const std::string &type) const;
};

struct OscillatorNode {
  NativeAudioHandle nativeHandle = 0;
  std::shared_ptr<audio_worklet::ContextState> context;
  mutable double clockOffset = 0;
  mutable bool clockAligned = false;
  mutable AudioParam frequency;
  mutable OscillatorTypeProperty type;

  OscillatorNode() = default;
  explicit OscillatorNode(NativeAudioHandle oscillator) : nativeHandle(oscillator), frequency(oscillator), type(oscillator) {}
  explicit OscillatorNode(double oscillator) : nativeHandle(static_cast<NativeAudioHandle>(oscillator)), frequency(oscillator), type(oscillator) {}

  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  void connect(AudioDestinationNode destination) const;
  void connect(AudioDestinationProperty destination) const;
  void connect(double destinationHandle) const;
  void start(double when = 0.0) const;
  void stop(double when = 0.0) const;
};

struct AudioBufferSourceNode {
private:
  bool present_ = false;
  explicit AudioBufferSourceNode(bool present) : present_(present) {}
  friend struct AudioContext;

public:
  AudioBufferSourceNode() = default;
  explicit operator bool() const { return present_; }
  bool operator==(std::nullptr_t) const { return !present_; }
  mutable AudioBuffer buffer;
  mutable bool connected = false;

  AudioDestinationNode connect(AudioDestinationNode destination) const;
  AudioDestinationNode connect(AudioDestinationProperty destination) const;
  AudioDestinationNode connect(double destinationHandle) const;
  void start(double when = 0.0) const;
  void stop(double when = 0.0) const;
};

struct AudioContext {
 private:
  std::shared_ptr<audio_worklet::ContextState> state_;
 public:
  // The default native carrier represents a missing/undefined JS handle.
  // Actual construction is performed by createAudioContext or the rate ctor.
  AudioContext() = default;
  explicit AudioContext(double rate);
  explicit operator bool() const { return static_cast<bool>(state_); }
  bool operator==(std::nullptr_t) const { return !state_; }
  template <typename Options>
  explicit AudioContext(const Options& options) : AudioContext(sampleRateOption(options)) {}
  double sampleRate = gea::platform::audio::deviceSampleRate;
  AudioDestinationProperty destination;
  AudioContextCurrentTimeProperty currentTime;
  AudioWorklet audioWorklet;

  std::string state() const;
  double baseLatency() const;
  double outputLatency() const;
  void resume() const;
  void suspend() const;
  void close() const;
  MediaStreamAudioSourceNode createMediaStreamSource(MediaStream stream) const;
  const std::shared_ptr<audio_worklet::ContextState>& workletContext() const { return state_; }

  OscillatorNode createOscillator() const;
  AudioBufferSourceNode createBufferSource() const;
  AudioBuffer createBuffer(double channels, double length, double rate) const;
  AudioBuffer decodeAudioData(const std::vector<std::uint8_t> &bytes) const;
 private:
  template <typename Options>
  static double sampleRateOption(const Options& options) {
    if constexpr (requires {
                    options.has_value();
                    *options;
                  })
      return options.has_value() ? sampleRateOption(*options)
                                 : gea::platform::audio::deviceSampleRate;
    else if constexpr (requires {
                         options.get();
                         *options;
                       })
      return options.get() ? sampleRateOption(*options) : gea::platform::audio::deviceSampleRate;
    else if constexpr (requires { options.sampleRate; })
      return sampleRateOption(options.sampleRate);
    else if constexpr (requires { static_cast<double>(options); })
      return static_cast<double>(options);
    else return gea::platform::audio::deviceSampleRate;
  }
};

class HTMLAudioElement {
 public:
  HTMLAudioElement() = default;
  static HTMLAudioElement create();
  static HTMLAudioElement create(const std::string &src);
  explicit HTMLAudioElement(const char *src);
  explicit HTMLAudioElement(const std::string &src);
  explicit HTMLAudioElement(const gea::embedded::ui::NodeHandle &node);

  std::string src() const;
  void setSrc(const std::string &src);
  bool play() const;
  void pause() const;
  void beginDrain() const;
  void clearBufferedAudio() const;
  bool drained() const;
  double audioLevel() const;
  MediaStream srcObject() const;
  void setSrcObject(MediaStream stream) const;
  void setSrcObject(std::nullptr_t) const { setSrcObject(MediaStream{}); }
  bool autoplay() const;
  void setAutoplay(bool enabled) const;
  bool paused() const;
  explicit operator bool() const { return static_cast<bool>(state_); }
  bool operator==(std::nullptr_t) const { return !state_; }
  bool operator==(const HTMLAudioElement& other) const { return state_ == other.state_; }

 private:
  struct State;
  std::shared_ptr<State> state_;
};

// Streaming mono PCM transport independent of any speech provider. Wire PCM
// uses sampleRate; capture/playback use the configured device audio clock.
class WebSocket;
class PcmAudioStream {
 public:
  PcmAudioStream() = default;
  static PcmAudioStream create(double sampleRate);
  void setInput(MediaStream stream) const;
  void pipeTo(WebSocket socket, const std::string &prefix, const std::string &suffix) const;
  void receiveFrom(WebSocket socket, const std::string &prefix, const std::string &suffix, double startupMs = 200) const;
  std::string readBase64() const;
  void writeBase64(const std::string &data) const;
  void resetPlayback(bool interrupted = false) const;
  void close() const;
  double queuedMs() const;
  double playedMs() const;
  double capturePendingMs() const;
  double captureDroppedSamples() const;
  double capturePackets() const;
  double audioLevel() const;
  bool drained() const;
  explicit operator bool() const { return static_cast<bool>(state_); }
  bool operator==(std::nullptr_t) const { return !state_; }
  bool operator==(const PcmAudioStream &other) const { return state_ == other.state_; }
 private:
  struct State;
  std::shared_ptr<State> state_;
};

struct AudioFacade {
  double getVolume() const { return gea::framework::audio::AudioBackend::volume(); }
  void setVolume(double volume) const { gea::framework::audio::AudioBackend::setVolume(volume); }
};

inline const AudioContext& sharedAudioContext() {
  static const AudioContext context{double(gea::platform::audio::deviceSampleRate)};
  return context;
}
inline constexpr AudioFacade Audio{};

}  // namespace gea::host
