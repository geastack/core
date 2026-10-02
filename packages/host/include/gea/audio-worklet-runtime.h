// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "gea/worker-runtime.h"
#include "host/audio.h"

namespace gea::runtime::hostaudio {

template <typename Action>
gea::Promise<void> workletPromise(Action action) {
  try { action(); return gea::Promise<void>::settled_value(); }
  catch (...) { return gea::Promise<void>::rejected_with(std::current_exception()); }
}
inline gea::host::AudioContext createAudioContext() {
  hostworker::installRealmRuntime();
  return gea::host::AudioContext(16000.0);
}
template <typename Options>
gea::host::AudioContext createAudioContext(const Options& options) {
  hostworker::installRealmRuntime();
  return gea::host::AudioContext(options);
}
inline gea::Promise<void> workletAddModule(const gea::host::AudioWorklet& worklet, const std::string& url) {
  hostworker::installRealmRuntime();
  return workletPromise([&] { worklet.addModule(url); });
}
inline gea::Promise<void> contextResume(const gea::host::AudioContext& context) {
  hostworker::installRealmRuntime();
  return workletPromise([&] { context.resume(); });
}
inline gea::Promise<void> contextSuspend(const gea::host::AudioContext& context) {
  hostworker::installRealmRuntime();
  return workletPromise([&] { context.suspend(); });
}
inline gea::Promise<void> contextClose(const gea::host::AudioContext& context) {
  return workletPromise([&] { context.close(); });
}
template <typename T>
double optionNumber(const T& value, double fallback) {
  if constexpr (requires { value.has_value(); *value; }) return value.has_value() ? optionNumber(*value, fallback) : fallback;
  else if constexpr (std::is_convertible_v<T, double>) return static_cast<double>(value);
  else return fallback;
}
template <typename Options>
gea::host::AudioWorkletNodeOptions workletOptions(const Options& options) {
  gea::host::AudioWorkletNodeOptions result;
  if constexpr (requires { options.has_value(); *options; }) {
    if (options.has_value()) return workletOptions(*options);
  } else if constexpr (requires { options.get(); *options; }) {
    if (options.get()) return workletOptions(*options);
  } else {
    auto count = [](double n) {
      if (!std::isfinite(n) || n < 0 || n > 32 || n != std::floor(n))
        throw std::invalid_argument("Invalid AudioWorklet channel count");
      return static_cast<unsigned>(n);
    };
    if constexpr (requires { options.numberOfInputs; }) result.numberOfInputs = count(optionNumber(options.numberOfInputs, 1));
    if constexpr (requires { options.numberOfOutputs; }) result.numberOfOutputs = count(optionNumber(options.numberOfOutputs, 1));
    if constexpr (requires { options.channelCount; }) result.channelCount = count(optionNumber(options.channelCount, 1));
    if constexpr (requires { options.outputChannelCount; }) {
      hostworker::present(options.outputChannelCount, [&](const auto& counts) {
        const auto& values = [&]() -> const auto& { if constexpr (requires { counts->size(); }) return *counts; else return counts; }();
        if (values.size() != result.numberOfOutputs || values.size() != 1 || values.at(0) != 1)
          throw std::invalid_argument("Embedded AudioWorklet requires one mono output");
      });
    }
  }
  return result;
}
inline gea::host::AudioWorkletNode createAudioWorkletNode(const gea::host::AudioContext& context, const std::string& name) {
  return gea::host::AudioWorkletNode(context, name);
}
template <typename Options>
gea::host::AudioWorkletNode createAudioWorkletNode(const gea::host::AudioContext& context, const std::string& name, const Options& options) {
  return gea::host::AudioWorkletNode(context, name, workletOptions(options));
}
template <typename Handler>
void setOnProcessorError(const gea::host::AudioWorkletNode& node, const Handler& handler) {
  node.setOnProcessorError({});
  hostworker::present(handler, [&](const auto& fn) {
    node.setOnProcessorError([fn](const std::string& error) { hostworker::deliverError(fn, error); });
  });
}

using WorkletFloat32Array = gea::Ref<gea::TypedArray<float>>;
using WorkletChannels = gea::Ref<gea::ArrayObject<WorkletFloat32Array>>;
using WorkletBus = gea::Ref<gea::ArrayObject<WorkletChannels>>;

template <typename Function, typename... Args>
decltype(auto) invokeWorklet(Function& function, Args&&... args) {
  if constexpr (requires { function.call(std::forward<Args>(args)...); })
    return function.call(std::forward<Args>(args)...);
  else return function(std::forward<Args>(args)...);
}

// The compiler supplies the concrete factory and process invocation; generated
// method spellings and object shapes never enter the hardware host. Float32
// views persist between quanta. Copying 128 samples crosses the native buffer
// boundary without allocating a JS array or boxing a sample each time.
template <typename Processor, typename Process>
class TypedProcessorAdapter final : public gea::host::AudioWorkletProcessor {
 public:
  TypedProcessorAdapter(Processor processor, Process process)
      : processor_(std::move(processor)), process_(std::move(process)),
        inputs_(gea::makeRef<gea::ArrayObject<WorkletChannels>>()),
        outputs_(gea::makeRef<gea::ArrayObject<WorkletChannels>>()) {}
  bool process(const gea::host::AudioWorkletBus& inputs, gea::host::AudioWorkletBus& outputs,
               const gea::host::AudioWorkletParameters& parameters) override {
    if (!parameters.empty()) throw std::invalid_argument("AudioParam automation is not implemented for this processor");
    prepare(inputs_, inputs, false);
    prepare(outputs_, outputs, true);
    const bool keep = [&] {
      try { return invokeWorklet(process_, processor_, inputs_, outputs_); }
      catch (const gea::Value& error) {
        // A thrown JS value is a genuine dynamic boundary. Preserve its text
        // across the typed/runtime bridge for native logs and processorerror.
        throw std::runtime_error(gea::host::detail::toString(error));
      }
    }();
    for (std::size_t bus = 0; bus < outputs.size(); ++bus)
      for (std::size_t channel = 0; channel < outputs[bus].size(); ++channel) {
        const auto& data = outputs_->at(bus)->at(channel);
        if (data->size() != outputs[bus][channel].size()) throw std::runtime_error("AudioWorklet detached or resized its output buffer");
        std::copy_n(data->data(), data->size(), outputs[bus][channel].data());
      }
    return keep;
  }
 private:
  static void prepare(WorkletBus& target, const gea::host::AudioWorkletBus& native, bool zero) {
    // Geometry changes only at connect/disconnect. A retained input buffer may
    // have been transferred by user code, so replace detached views as well.
    if (target->size() != native.size()) {
      target = gea::makeRef<gea::ArrayObject<WorkletChannels>>();
      for (std::size_t i = 0; i < native.size(); ++i) target->push(gea::makeRef<gea::ArrayObject<WorkletFloat32Array>>());
    }
    for (std::size_t bus = 0; bus < native.size(); ++bus) {
      auto channels = target->at(bus);
      if (channels->size() != native[bus].size()) {
        channels = gea::makeRef<gea::ArrayObject<WorkletFloat32Array>>();
        for (const auto& samples : native[bus]) channels->push(gea::makeRef<gea::TypedArray<float>>(samples.size()));
        target->setElement(static_cast<double>(bus), channels);
      }
      for (std::size_t channel = 0; channel < native[bus].size(); ++channel) {
        auto data = channels->at(channel);
        if (data->size() != native[bus][channel].size()) {
          data = gea::makeRef<gea::TypedArray<float>>(native[bus][channel].size());
          channels->setElement(static_cast<double>(channel), data);
        }
        if (zero) std::fill_n(data->data(), data->size(), 0.0f);
        else std::copy_n(native[bus][channel].data(), data->size(), data->data());
      }
    }
  }
  Processor processor_;
  Process process_;
  WorkletBus inputs_, outputs_;
};

template <typename Factory, typename Process>
void registerTypedProcessor(const std::string& name, Factory factory, Process process) {
  gea::host::audio_worklet::registerProcessor(name, [factory, process]() mutable {
    using Processor = decltype(invokeWorklet(factory));
    return std::make_shared<TypedProcessorAdapter<Processor, Process>>(invokeWorklet(factory), process);
  });
}
} // namespace gea::runtime::hostaudio
