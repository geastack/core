// SPDX-License-Identifier: Apache-2.0
#define GEA_HOST_DECLARED 1
#define GEA_RUNTIME_REALMS 1
#include "gea/embedded.h"
#include "gea/audio-worklet-runtime.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

namespace {
using namespace gea::runtime::hostaudio;
std::atomic<unsigned> typedCalls{0}, typedDestroyed{0}, typedFinished{0}, factoryDestroyed{0}, processorDestroyed{0};
struct Owned {
  explicit Owned(bool isProcessor = false) : isProcessor(isProcessor) {}
  bool isProcessor;
  std::thread::id owner = std::this_thread::get_id();
  std::uint64_t realm = gea::detail::currentRuntimeRealm().identity();
  ~Owned() {
    assert(owner == std::this_thread::get_id());
    assert(realm == gea::detail::currentRuntimeRealm().identity());
    ++typedDestroyed;
    if (isProcessor) ++processorDestroyed; else ++factoryDestroyed;
  }
};
struct TypedProcessor : Owned {
  TypedProcessor() : Owned(true) {}
  gea::host::workers::MessagePort port = gea::host::audio_worklet::processorPort();
  gea::host::workers::MessagePort network;
  const void* outputIdentity = nullptr;
  bool process(WorkletBus inputs, WorkletBus outputs) {
    assert(inputs->size() == 1 && inputs->at(0)->empty());
    assert(outputs->size() == 1 && outputs->at(0)->size() == 1);
    auto out = outputs->at(0)->at(0);
    assert(out->size() == 128);
    if (outputIdentity) assert(outputIdentity == out.get());
    outputIdentity = out.get();
    for (std::size_t i = 0; i < out->size(); ++i) {
      assert(out->data()[i] == 0.0f); // Reused channel zeroed before every call.
      out->data()[i] = 0.5f;
    }
    ++typedCalls;
    return true;
  }
};
bool invokeTyped(void*, gea::Ref<TypedProcessor> processor, WorkletBus input, WorkletBus output) {
  return processor->process(input, output);
}
}

void testAudioWorkletTypedBridge() {
  gea::runtime::hostworker::installRealmRuntime();
  gea::host::audio_worklet::registerModule("/typed-audio.js", [] {
    // This Ref capture represents a compiled constructor's closure. Both it and
    // the processor must die before the worklet's allocation pools are cleared.
    const auto capture = gea::makeRef<Owned>();
    registerTypedProcessor("typed", [capture] {
      auto processor = gea::makeRef<TypedProcessor>();
      const auto raw = processor.get();
      gea::host::workers::Context::current()->addCleanup([raw] {
        ++typedFinished;
        if (!processorDestroyed)
          std::fprintf(stderr, "processor retained after port cleanup: strong=%u\n", gea::detail::refCountsOf(raw)->strong);
      });
      gea::host::workers::MessageChannel direct;
      processor->network = direct.port1;
      // Opaque native port callbacks can form a processor Ref cycle. Context
      // finish must close that port on this realm before its pools are cleared.
      processor->network.setOnMessage([processor](gea::host::workers::MessageEvent&) {});
      return processor;
    },
      gea::CallableObject<bool(gea::Ref<TypedProcessor>, WorkletBus, WorkletBus)>(&invokeTyped, nullptr));
  });
  gea::host::AudioContext context(24000.0);
  context.audioWorklet.addModule("/typed-audio.js");
  gea::host::AudioWorkletNode node(context, "typed");
  node.connect(context.destination);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (typedCalls < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(typedCalls >= 3);
  context.close();
  while (typedDestroyed < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (typedDestroyed != 2) std::fprintf(stderr, "typed teardown: destroyed=%u calls=%u finished=%u factory=%u processor=%u\n", typedDestroyed.load(), typedCalls.load(), typedFinished.load(), factoryDestroyed.load(), processorDestroyed.load());
  assert(typedDestroyed == 2);
}
