// SPDX-License-Identifier: Apache-2.0
#include "host/audio.h"
#include "host/pcm_stream.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
#include <thread>
#endif

namespace gea::host::audio_worklet {
namespace {
using Clock = std::chrono::steady_clock;
std::mutex moduleMutex;
std::unordered_map<std::string, std::function<void()>>& modules() {
  static std::unordered_map<std::string, std::function<void()>> entries;
  return entries;
}
thread_local ContextState* active = nullptr;
thread_local MessagePort constructingPort;
// Enough for the output driver's 512-frame pulls, but bounded independently of
// network rate. The producer never waits for the mixer or a network socket.
constexpr std::size_t outputCapacity = 2048;
constexpr std::size_t maxNodes = 16;
}

// JS-facing handlers live only in the node's creating realm. The audio task
// keeps a weak handle and posts native error text; it never copies a Ref capture.
struct NodeControl {
  std::shared_ptr<workers::Context> owner;
  std::function<void(const std::string&)> error;
};

struct NodeState {
  explicit NodeState(int rate) : capture(16000, rate) {}
  std::weak_ptr<ContextState> context;
  std::shared_ptr<AudioWorkletProcessor> processor;
  MessagePort processorPort;
  std::shared_ptr<workers::Context> owner;
  std::weak_ptr<NodeControl> control;
  std::shared_ptr<media::TrackPcmReader> reader;
  MediaStreamTrack inputTrack;
  bool captureInputLive = false;
  std::weak_ptr<void> source;
  pcm::Resampler capture;
  AudioWorkletBus inputs{AudioWorkletChannels{}}, outputs{AudioWorkletChannels{std::vector<float>(renderQuantum)}};
  AudioWorkletParameters parameters;
  std::array<float, 512> inputPending{};
  std::size_t inputRead = 0, inputCount = 0;
  std::uint64_t captureDropped = 0;
  bool connected = false, alive = true;
  unsigned numberOfInputs = 1;
};

struct ContextState : std::enable_shared_from_this<ContextState> {
  explicit ContextState(int rate) : rate(rate), playback(rate, 16000) {}
  ~ContextState() {
    if (output) gea::platform::audio::AudioSystem::stopPcmStream(output);
    if (realm) realm->stop();
  }
  const int rate;
  std::mutex startMutex;
  std::mutex readyMutex;
  std::condition_variable readySignal;
  std::atomic<bool> ready{false};
  std::shared_ptr<workers::Context> realm;
  std::atomic<bool> closed{false}, running{true};
  std::atomic<std::uint64_t> frames{0};
  bool inputClockActive = false;
  std::unordered_map<std::string, ProcessorFactory> factories;
  std::unordered_set<std::string> loaded;
  std::vector<std::shared_ptr<NodeState>> nodes;
  std::mutex outputMutex;
  std::vector<std::int16_t, pcm::QueueAllocator<std::int16_t>> outputPcm;
  std::size_t outputRead = 0, outputCount = 0;
  std::uint64_t output = 0;
  pcm::Resampler playback;
  std::array<float, renderQuantum> mix{};
#ifdef ESP_PLATFORM
  std::int64_t statsStart = 0, maxRenderUs = 0, maxProcessUs = 0, maxLagUs = 0;
  std::uint64_t statsQuanta = 0, capturedSamples = 0, lostCaptureSamples = 0;
  std::uint64_t inputWaits = 0, realInputFrames = 0;
  std::uint64_t processSumUs = 0, renderSumUs = 0, processCalls = 0, renderCalls = 0;
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
  decltype(TaskStatus_t{}.ulRunTimeCounter) statsCpuStart = 0;
#endif
  std::size_t maxCapturePending = 0, maxOutputQueued = 0, emptyOutputPulls = 0, outputBackpressureWaits = 0;
  void reportStats() {
    const auto now = esp_timer_get_time();
    if (statsStart && now - statsStart < 2000000) return;
    std::int64_t cpuUs = -1;
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
    TaskStatus_t task{};
    // Only this task, and no stack high-water scan: report actual scheduled
    // CPU separately from render/process wall time, which includes preemption.
    vTaskGetInfo(nullptr, &task, pdFALSE, eInvalid);
    cpuUs = static_cast<std::int64_t>(task.ulRunTimeCounter - statsCpuStart);
    statsCpuStart = task.ulRunTimeCounter;
#endif
    if (!statsStart) { statsStart = now; return; }
    std::size_t queued, peak, empty;
    { std::lock_guard lock(outputMutex);
      queued = outputCount; peak = maxOutputQueued; empty = emptyOutputPulls;
      maxOutputQueued = outputCount; emptyOutputPulls = 0;
    }
    ESP_LOGI("gea_worklet", "span_ms=%lld cpu_us=%lld rate=%d quanta=%llu render_avg_us=%llu process_avg_us=%llu render_max_us=%lld process_max_us=%lld lag_max_us=%lld capture_samples=%llu capture_pending_max=%u capture_lost=%llu input_waits=%llu input_real_frames=%llu output_queued=%u output_peak=%u empty_pulls=%u backpressure_waits=%u",
      static_cast<long long>((now - statsStart) / 1000), static_cast<long long>(cpuUs), rate,
      static_cast<unsigned long long>(statsQuanta),
      static_cast<unsigned long long>(renderCalls ? renderSumUs / renderCalls : 0),
      static_cast<unsigned long long>(processCalls ? processSumUs / processCalls : 0), static_cast<long long>(maxRenderUs),
      static_cast<long long>(maxProcessUs), static_cast<long long>(maxLagUs),
      static_cast<unsigned long long>(capturedSamples), static_cast<unsigned>(maxCapturePending),
      static_cast<unsigned long long>(lostCaptureSamples),
      static_cast<unsigned long long>(inputWaits), static_cast<unsigned long long>(realInputFrames),
      static_cast<unsigned>(queued),
      static_cast<unsigned>(peak), static_cast<unsigned>(empty), static_cast<unsigned>(outputBackpressureWaits));
    statsStart = now; statsQuanta = capturedSamples = lostCaptureSamples = 0;
    inputWaits = realInputFrames = 0;
    maxRenderUs = maxProcessUs = maxLagUs = 0; maxCapturePending = outputBackpressureWaits = 0;
    processSumUs = renderSumUs = processCalls = renderCalls = 0;
  }
#endif

  void clearOutput() {
    std::lock_guard lock(outputMutex);
    outputRead = outputCount = 0;
    playback.reset();
  }
  void stopNodes() {
    for (auto& node : nodes) {
      node->processorPort.close();
      node->processor.reset();
      node->reader.reset();
      node->alive = false;
    }
    nodes.clear();
    // Generated factory captures and processor Ref fields die in their realm.
    factories.clear();
    loaded.clear();
  }
  void fail(const std::shared_ptr<NodeState>& node, const std::string& message) {
    std::fprintf(stderr, "[AudioWorklet] processorerror: %s\n", message.c_str());
    node->alive = false;
    node->reader.reset();
    node->processorPort.close();
    if (node->owner) {
      const auto control = node->control;
      node->owner->post([control, message] {
        if (const auto target = control.lock(); target && target->error) target->error(message);
      }, message.size());
    }
  }
  bool render() {
    if (!running || closed) { inputClockActive = false; return true; }
    const bool connected = std::any_of(nodes.begin(), nodes.end(),
        [](const auto& node) { return node->alive && node->connected; });
    if (connected) {
      // Reserve a whole hardware quantum before process() advances playback
      // or capture. Codec startup and mixer scheduling may temporarily fill
      // this queue; yield until the consumer pulls, without dropping samples.
      const auto required = (renderQuantum * 16000 + rate - 1) / rate;
      std::lock_guard lock(outputMutex);
      if (required > outputPcm.size() - outputCount) {
#ifdef ESP_PLATFORM
        ++outputBackpressureWaits;
#endif
        return false;
      }
    }
    // Capture arrives on the hardware clock in AEC batches. Prepare a complete
    // quantum for EVERY live input before invoking any processor. A transient
    // shortage retains the partial input and deadline instead of inserting
    // artificial silence and stretching the microphone timeline.
    bool waitingForInput = false;
    inputClockActive = false;
    for (const auto& node : nodes) {
      if (!node->alive || !node->connected || !node->reader) continue;
      node->captureInputLive = node->inputTrack.enabled() && node->inputTrack.readyState() == "live";
      if (!node->captureInputLive) {
        // Disabled/ended tracks represent silence, not a stalled device clock.
        node->reader->discardBuffered();
        node->inputRead = node->inputCount = 0;
        node->capture.reset();
        continue;
      }
      inputClockActive = true;
      {
#ifdef ESP_PLATFORM
        maxCapturePending = std::max(maxCapturePending, node->reader->pendingSamples());
#endif
        const auto dropped = node->reader->droppedSamples();
        if (dropped != node->captureDropped) {
          // Only the track's actual bounded-ring overwrite is a loss. A slow
          // consumer must not silently discard a perfectly recoverable batch.
          std::fprintf(stderr, "[AudioWorklet] capture ring overrun: %llu samples lost\n",
              static_cast<unsigned long long>(dropped - node->captureDropped));
#ifdef ESP_PLATFORM
          lostCaptureSamples += dropped - node->captureDropped;
#endif
          node->captureDropped = dropped;
        }
        // AEC delivers batches (32 ms on ES8311), not one hardware sample at a
        // time. Keep a partial converted quantum until it is complete; padding
        // that partial quantum would insert silence inside captured speech.
        while (node->inputCount < renderQuantum) {
          std::array<std::int16_t, 128> pcm{};
          const auto wanted = std::min<std::size_t>(pcm.size(),
              ((renderQuantum - node->inputCount) * 16000 + rate - 1) / rate);
          const auto count = node->reader->read(pcm.data(), wanted);
          if (!count) break;
#ifdef ESP_PLATFORM
          capturedSamples += count;
#endif
          for (std::size_t i = 0; i < count; ++i)
            node->capture.push(pcm[i], [&](std::int16_t value) {
              node->inputPending[(node->inputRead + node->inputCount++) % node->inputPending.size()] = value / 32768.0f;
            });
        }
        waitingForInput |= node->inputCount < renderQuantum;
      }
    }
    if (waitingForInput) {
#ifdef ESP_PLATFORM
      ++inputWaits;
#endif
      return false;
    }
    mix.fill(0);
    bool hasDestination = false;
    for (const auto& node : nodes) {
      if (!node->alive || !node->connected) continue;
      hasDestination = true;
      for (auto& output : node->outputs)
        for (auto& channel : output) std::fill(channel.begin(), channel.end(), 0.0f);
      if (node->reader) {
        auto& input = node->inputs[0][0];
        if (node->captureInputLive) {
          for (auto& sample : input) {
            sample = node->inputPending[node->inputRead];
            node->inputRead = (node->inputRead + 1) % node->inputPending.size();
            --node->inputCount;
          }
#ifdef ESP_PLATFORM
          realInputFrames += renderQuantum;
#endif
        } else {
          std::fill(input.begin(), input.end(), 0.0f);
        }
      }
      try {
#ifdef ESP_PLATFORM
        const auto processStart = esp_timer_get_time();
#endif
        const bool keep = node->processor->process(node->inputs, node->outputs, node->parameters);
#ifdef ESP_PLATFORM
        const auto processUs = esp_timer_get_time() - processStart;
        maxProcessUs = std::max(maxProcessUs, processUs);
        processSumUs += processUs;
        ++processCalls;
#endif
        const auto& channel = node->outputs.at(0).at(0);
        if (channel.size() != renderQuantum) throw std::runtime_error("AudioWorklet changed render quantum geometry");
        for (std::size_t i = 0; i < renderQuantum; ++i) mix[i] += channel[i];
        if (!keep) node->alive = false;
      } catch (const std::exception& error) { fail(node, error.what()); }
      catch (...) { fail(node, "AudioWorklet processor failed"); }
    }
    frames.fetch_add(renderQuantum);
    if (!hasDestination) return true;
    std::array<std::int16_t, 256> block{};
    std::size_t count = 0;
    for (float sample : mix) {
      const auto value = !std::isfinite(sample) ? 0 : static_cast<int>(std::clamp(sample, -1.0f, 1.0f) * 32768.0f);
      playback.push(static_cast<std::int16_t>(std::clamp(value, -32768, 32767)),
          [&](std::int16_t converted) { block[count++] = converted; });
    }
    bool overflow = false;
    {
      std::lock_guard lock(outputMutex);
      overflow = count > outputPcm.size() - outputCount;
      if (!overflow) {
        for (std::size_t i = 0; i < count; ++i)
          outputPcm[(outputRead + outputCount++) % outputPcm.size()] = block[i];
#ifdef ESP_PLATFORM
        maxOutputQueued = std::max(maxOutputQueued, outputCount);
#endif
      }
    }
    if (overflow) {
      // Only the audio task produces samples, so preflight reservation makes
      // this an internal invariant failure, not a temporary device delay.
      for (const auto& node : nodes) if (node->alive) fail(node, "AudioWorklet output reservation was exceeded");
      clearOutput();
    }
    return true;
  }
  void ensureStarted() {
    std::lock_guard lock(startMutex);
    if (closed) throw std::runtime_error("AudioContext is closed");
    if (realm) return;
#ifdef ESP_PLATFORM
    // ESP32-S3 pins an unpinned task to its current core on first FPU use.
    // Choose explicitly so startup timing cannot pin the worklet beside the
    // AEC/capture task on core 0. Single-core targets retain their only core.
    constexpr int core = portNUM_PROCESSORS > 1 ? 1 : 0;
    realm = workers::Context::create("gea_worklet", configMAX_PRIORITIES - 1, core);
#else
    realm = workers::Context::create("gea_worklet", 24);
#endif
    auto self = shared_from_this();
    const std::weak_ptr<ContextState> weak = self;
    realm->setErrorHandler([weak](const std::string& message) {
      if (const auto context = weak.lock()) {
        for (const auto& node : context->nodes)
          if (node->alive) context->fail(node, message);
      }
    });
    // Context supplies the native task, ownership scope, timers and cleanup.
    // This entry owns its periodic render loop; runPending services only its
    // bounded messages between process calls, never on the UI or mixer task.
    realm->start([self] {
      active = self.get();
      { std::lock_guard lock(self->readyMutex); self->ready = true; }
      self->readySignal.notify_all();
      const auto quantum = std::chrono::nanoseconds(128000000000LL / self->rate);
      auto next = Clock::now();
      while (!self->closed && !self->realm->stopped()) {
        self->realm->runPending(0.5);
        const auto now = Clock::now();
        unsigned catchup = 0;
        while ((self->inputClockActive || now >= next) && catchup++ < 4) {
#ifdef ESP_PLATFORM
          const auto renderStart = esp_timer_get_time();
          const auto framesBefore = self->frames.load();
          self->maxLagUs = std::max<std::int64_t>(self->maxLagUs,
              std::chrono::duration_cast<std::chrono::microseconds>(now - next).count());
#endif
          if (!self->render()) break;
#ifdef ESP_PLATFORM
          const auto renderUs = esp_timer_get_time() - renderStart;
          self->maxRenderUs = std::max(self->maxRenderUs, renderUs);
          self->renderSumUs += renderUs;
          ++self->renderCalls;
          if (self->frames.load() != framesBefore) ++self->statsQuanta;
#endif
          // A live microphone supplies the graph's hardware clock. Do not
          // constrain it with a second wall clock or accumulate timer debt
          // while waiting for a batched ADC/AEC delivery. Output backpressure
          // and the bounded loop still protect the DAC and lower-priority tasks.
          if (self->inputClockActive) next = Clock::now() + quantum;
          else next += quantum;
          // A slow processor must not monopolize this high-priority task with
          // four overdue calls. Retain remaining deadlines for the next wake.
          if (Clock::now() - now >= quantum) break;
        }
        // Output-only graphs retain missed timer deadlines. Capture graphs
        // resume with the next complete hardware quantum on a later wake.
#ifdef ESP_PLATFORM
        self->reportStats();
        vTaskDelay(1);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
      }
      self->stopNodes();
      self->realm->finish();
      active = nullptr;
    });
    std::unique_lock readyLock(readyMutex);
    if (!readySignal.wait_for(readyLock, std::chrono::seconds(5), [&] { return ready.load(); }))
      throw std::runtime_error("AudioWorklet task did not initialize");
  }
  void invoke(std::function<void()> action) {
    ensureStarted();
    if (active == this) { action(); return; }
    struct Completion { std::mutex mutex; std::condition_variable ready; bool done = false; std::exception_ptr error; };
    auto completion = std::make_shared<Completion>();
    if (!realm->post([completion, action = std::move(action)] {
      try { action(); } catch (...) { completion->error = std::current_exception(); }
      { std::lock_guard lock(completion->mutex); completion->done = true; }
      completion->ready.notify_one();
    })) throw std::runtime_error("AudioWorklet control queue is full");
    std::unique_lock lock(completion->mutex);
    if (!completion->ready.wait_for(lock, std::chrono::seconds(5), [&] { return completion->done; }))
      throw std::runtime_error("AudioWorklet task did not respond");
    if (completion->error) std::rethrow_exception(completion->error);
  }
  void ensureOutput() {
    if (output) return;
    outputPcm.resize(outputCapacity);
    const std::weak_ptr<ContextState> weak = shared_from_this();
    output = gea::platform::audio::AudioSystem::playPcmStream([weak](std::int16_t* out, std::size_t capacity) {
      const auto state = weak.lock();
      if (!state || state->closed || !state->running) return std::size_t{0};
      std::lock_guard lock(state->outputMutex);
      const auto count = std::min(capacity, state->outputCount);
#ifdef ESP_PLATFORM
      if (!count) ++state->emptyOutputPulls;
#endif
      for (std::size_t i = 0; i < count; ++i) {
        out[i] = state->outputPcm[state->outputRead];
        state->outputRead = (state->outputRead + 1) % state->outputPcm.size();
      }
      state->outputCount -= count;
      return count;
    });
    if (!output) throw std::runtime_error("AudioContext output unavailable");
  }
};

std::shared_ptr<ContextState> createContext(double rate) {
  if (!std::isfinite(rate) || rate < 8000 || rate > 48000 || rate != std::floor(rate))
    throw std::invalid_argument("AudioContext sampleRate must be an integer from 8000 to 48000");
  return std::make_shared<ContextState>(static_cast<int>(rate));
}
// Report a conservative processing delay from the configured queue capacity.
// currentTime is a render clock; neither property claims a DAC timestamp.
double contextBaseLatency(const std::shared_ptr<ContextState>& context) {
  return context ? double(outputCapacity) / 16000 + gea::platform::audio::AudioSystem::processingLatency() : 0;
}
double contextTime(const std::shared_ptr<ContextState>& context) {
  // Legacy oscillator-only contexts retain their platform scheduler's clock.
  // A worklet has an independent render clock once its audio realm starts.
  if (context && !context->ready) return gea::platform::audio::AudioSystem::sharedContext().currentTime();
  return context ? double(context->frames.load()) / context->rate : 0;
}
std::string contextState(const std::shared_ptr<ContextState>& context) {
  return !context || context->closed ? "closed" : context->running ? "running" : "suspended";
}
void resume(const std::shared_ptr<ContextState>& context) {
  if (!context || context->closed) throw std::runtime_error("AudioContext is closed");
  context->invoke([context] { context->running = true; });
}
void suspend(const std::shared_ptr<ContextState>& context) {
  if (!context || context->closed) throw std::runtime_error("AudioContext is closed");
  context->invoke([context] {
    context->running = false;
    context->clearOutput();
    for (const auto& node : context->nodes) if (node->reader) node->reader->discardBuffered();
  });
}
void close(const std::shared_ptr<ContextState>& context) {
  if (!context || context->closed) return;
  if (!context->realm) { context->closed = true; return; }
  context->invoke([context] {
    context->closed = true;
    context->running = false;
    context->stopNodes();
    context->clearOutput();
    if (context->output) gea::platform::audio::AudioSystem::stopPcmStream(context->output);
    context->output = 0;
  });
}
void registerModule(const std::string& url, std::function<void()> entry) {
  if (url.empty() || !entry) throw std::invalid_argument("Invalid AudioWorklet module");
  std::lock_guard lock(moduleMutex);
  if (!modules().emplace(url, std::move(entry)).second) throw std::invalid_argument("Duplicate AudioWorklet module URL");
}
void registerProcessor(const std::string& name, ProcessorFactory factory) {
  if (!active) throw std::logic_error("registerProcessor must run in an AudioWorklet module");
  if (name.empty() || !factory) throw std::invalid_argument("Invalid AudioWorklet processor");
  if (!active->factories.emplace(name, std::move(factory)).second) throw std::invalid_argument("Duplicate AudioWorklet processor name");
}
double sampleRate() { if (!active) throw std::logic_error("Not an AudioWorklet realm"); return active->rate; }
double currentTime() { return currentFrame() / sampleRate(); }
double currentFrame() { if (!active) throw std::logic_error("Not an AudioWorklet realm"); return double(active->frames.load()); }
MessagePort processorPort() {
  if (!active || constructingPort.detached()) throw std::logic_error("AudioWorkletProcessor must be constructed by AudioWorkletNode");
  return constructingPort;
}
void flushOutput() {
  if (!active) throw std::logic_error("Output cancellation must run in the audio realm");
  // This message boundary precedes the next process quantum. Clear converted
  // PCM/history, then discard submitted output without suspending capture.
  active->clearOutput();
  gea::platform::audio::AudioSystem::flushPlayback();
}
} // namespace gea::host::audio_worklet

namespace gea::host {
AudioWorkletProcessor::AudioWorkletProcessor() : port(audio_worklet::processorPort()) {}
void AudioWorklet::addModule(const std::string& url) const {
  if (!context_) throw std::logic_error("AudioWorklet has no AudioContext");
  std::function<void()> entry;
  {
    std::lock_guard lock(audio_worklet::moduleMutex);
    const auto found = audio_worklet::modules().find(url);
    if (found == audio_worklet::modules().end()) throw std::invalid_argument("AudioWorklet module was not compiled: " + url);
    entry = found->second;
  }
  const auto context = context_;
  context->invoke([context, url, entry] {
    if (context->loaded.count(url)) return;
    entry();
    context->loaded.insert(url);
  });
}
AudioWorkletNode::AudioWorkletNode(const AudioContext& context, const std::string& name)
    : AudioWorkletNode(context, name, AudioWorkletNodeOptions{}) {}
AudioWorkletNode::AudioWorkletNode(const AudioContext& context, const std::string& name, const AudioWorkletNodeOptions& options) {
  if (options.numberOfInputs > 1 || options.numberOfOutputs != 1 || options.channelCount != 1)
    throw std::invalid_argument("Embedded AudioWorklet supports zero or one mono input and one mono output");
  const auto owner = context.workletContext();
  workers::MessageChannel channel;
  port = channel.port1;
  auto node = std::make_shared<audio_worklet::NodeState>(static_cast<int>(context.sampleRate));
  node->context = owner;
  node->owner = workers::Context::current();
  control_ = std::make_shared<audio_worklet::NodeControl>();
  control_->owner = node->owner;
  node->control = control_;
  node->processorPort = channel.port2.transfer();
  node->numberOfInputs = options.numberOfInputs;
  if (!options.numberOfInputs) node->inputs.clear();
  owner->invoke([owner, node, name] {
    if (owner->nodes.size() >= audio_worklet::maxNodes) throw std::runtime_error("AudioContext node limit exceeded");
    const auto factory = owner->factories.find(name);
    if (factory == owner->factories.end()) throw std::invalid_argument("AudioWorklet processor is not registered: " + name);
    node->processorPort.adopt(owner->realm);
    audio_worklet::constructingPort = node->processorPort;
    try { node->processor = factory->second(); }
    catch (...) { audio_worklet::constructingPort = {}; throw; }
    audio_worklet::constructingPort = {};
    if (!node->processor) throw std::runtime_error("AudioWorklet processor constructor returned null");
    owner->nodes.push_back(node);
  });
  state_ = std::move(node);
}
AudioDestinationNode AudioWorkletNode::connect(AudioDestinationNode destination) const {
  if (!state_) throw std::logic_error("Null AudioWorkletNode");
  auto context = state_->context.lock();
  if (!context || context != destination.context) throw std::invalid_argument("Audio nodes belong to different contexts");
  const auto node = state_;
  context->invoke([context, node] { context->ensureOutput(); node->connected = true; });
  return destination;
}
AudioDestinationNode AudioWorkletNode::connect(AudioDestinationProperty destination) const { return connect(static_cast<AudioDestinationNode>(destination)); }
void AudioWorkletNode::disconnect() const {
  if (!state_) return;
  const auto context = state_->context.lock();
  if (!context || context->closed) return;
  const auto node = state_;
  context->invoke([context, node] { node->connected = false; context->clearOutput(); });
}
void AudioWorkletNode::setOnProcessorError(std::function<void(const std::string&)> callback) const {
  if (!control_) return;
  if (workers::Context::current() != control_->owner)
    throw std::logic_error("AudioWorkletNode handlers belong to their creating realm");
  control_->error = std::move(callback);
}

struct MediaStreamAudioSourceNode::State {
  std::shared_ptr<audio_worklet::ContextState> context;
  MediaStream stream;
};
MediaStreamAudioSourceNode::MediaStreamAudioSourceNode(std::shared_ptr<audio_worklet::ContextState> context, MediaStream stream)
    : state_(std::make_shared<State>(State{std::move(context), stream})) {
  if (stream.getAudioTracks().empty()) throw std::invalid_argument("MediaStreamAudioSourceNode needs an audio track");
}
AudioWorkletNode MediaStreamAudioSourceNode::connect(AudioWorkletNode destination) const {
  if (!state_ || !destination.state_) throw std::logic_error("Null audio node");
  const auto context = destination.state_->context.lock();
  if (context != state_->context) throw std::invalid_argument("Audio nodes belong to different contexts");
  const auto node = destination.state_;
  const auto source = state_;
  context->invoke([node, source] {
    if (!node->numberOfInputs) throw std::invalid_argument("AudioWorkletNode has no inputs");
    if (node->reader && node->source.lock() != source) throw std::invalid_argument("AudioWorklet mono input already connected");
    const auto tracks = source->stream.getAudioTracks();
    if (tracks.empty() || tracks.front().readyState() == "ended") throw std::invalid_argument("MediaStream has no live audio track");
    if (!node->reader) node->reader = std::make_shared<media::TrackPcmReader>(tracks.front().nativeHandle);
    node->inputTrack = tracks.front();
    node->source = source;
    node->inputs[0] = AudioWorkletChannels{std::vector<float>(audio_worklet::renderQuantum)};
    node->capture.reset();
    node->inputRead = node->inputCount = 0;
    node->captureDropped = 0;
  });
  return destination;
}
void MediaStreamAudioSourceNode::disconnect() const {
  if (!state_ || state_->context->closed) return;
  const auto source = state_;
  source->context->invoke([source] {
    for (const auto& node : source->context->nodes) {
      if (node->source.lock() != source) continue;
      node->reader.reset();
      node->inputTrack = {};
      node->captureInputLive = false;
      node->source.reset();
      if (node->numberOfInputs) node->inputs[0].clear();
      node->inputRead = node->inputCount = 0;
      node->captureDropped = 0;
      node->capture.reset();
    }
  });
}
} // namespace gea::host
