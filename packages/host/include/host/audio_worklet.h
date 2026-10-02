// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/media.h"
#include "host/worker.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gea::host {
using workers::MessagePort;
struct AudioContext;
struct AudioDestinationNode;
struct AudioDestinationProperty;
class AudioWorkletNode;

using AudioWorkletChannels = std::vector<std::vector<float>>;
using AudioWorkletBus = std::vector<AudioWorkletChannels>;
using AudioWorkletParameters = std::unordered_map<std::string, std::vector<float>>;

namespace audio_worklet {
struct ContextState;
struct NodeState;
struct NodeControl;
constexpr std::size_t renderQuantum = 128;
std::shared_ptr<ContextState> createContext(double sampleRate);
double contextBaseLatency(const std::shared_ptr<ContextState>& context);
double contextTime(const std::shared_ptr<ContextState>&);
std::string contextState(const std::shared_ptr<ContextState>&);
void resume(const std::shared_ptr<ContextState>&);
void suspend(const std::shared_ptr<ContextState>&);
void close(const std::shared_ptr<ContextState>&);
// Module entries and processor factories are generated C++; no JS evaluator.
void registerModule(const std::string& url, std::function<void()> entry);
double sampleRate();
double currentTime();
double currentFrame();
workers::MessagePort processorPort();
void flushOutput();
}

class AudioWorkletProcessor {
 public:
  AudioWorkletProcessor();
  virtual ~AudioWorkletProcessor() = default;
  workers::MessagePort port;
  virtual bool process(const AudioWorkletBus& inputs, AudioWorkletBus& outputs,
                       const AudioWorkletParameters& parameters) { return false; }
};

namespace audio_worklet {
using ProcessorFactory = std::function<std::shared_ptr<AudioWorkletProcessor>()>;
void registerProcessor(const std::string& name, ProcessorFactory factory);
}

class AudioWorklet {
 public:
  AudioWorklet() = default;
  explicit AudioWorklet(std::shared_ptr<audio_worklet::ContextState> context) : context_(std::move(context)) {}
  void addModule(const std::string& url) const;
 private:
  std::shared_ptr<audio_worklet::ContextState> context_;
};

struct AudioWorkletNodeOptions {
  unsigned numberOfInputs = 1;
  unsigned numberOfOutputs = 1;
  unsigned channelCount = 1;
};

class AudioWorkletNode {
 public:
  AudioWorkletNode() = default;
  AudioWorkletNode(const AudioContext& context, const std::string& name);
  AudioWorkletNode(const AudioContext& context, const std::string& name, const AudioWorkletNodeOptions& options);
  workers::MessagePort port;
  AudioDestinationNode connect(AudioDestinationNode destination) const;
  AudioDestinationNode connect(AudioDestinationProperty destination) const;
  void disconnect() const;
  void setOnProcessorError(std::function<void(const std::string&)> callback) const;
  explicit operator bool() const { return static_cast<bool>(state_); }
  bool operator==(std::nullptr_t) const { return !state_; }
 private:
  std::shared_ptr<audio_worklet::NodeState> state_;
  std::shared_ptr<audio_worklet::NodeControl> control_;
  friend class MediaStreamAudioSourceNode;
};

class MediaStreamAudioSourceNode {
 public:
  MediaStreamAudioSourceNode() = default;
  explicit operator bool() const { return static_cast<bool>(state_); }
  bool operator==(std::nullptr_t) const { return !state_; }
  MediaStreamAudioSourceNode(std::shared_ptr<audio_worklet::ContextState> context, MediaStream stream);
  AudioWorkletNode connect(AudioWorkletNode destination) const;
  void disconnect() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace gea::host
