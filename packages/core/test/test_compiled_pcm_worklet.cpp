// Runs the actual emitted application's AudioWorklet processor without audio I/O.
// GEA_COMPILED_WORKLET and GEA_COMPILED_WORKLET_ENTRY are supplied by the build.
#include GEA_COMPILED_WORKLET
#include <cassert>
#include <chrono>
#include <cstdio>

namespace {
gea::host::audio_worklet::ProcessorFactory processorFactory;
gea::host::workers::MessageChannel processorChannel;
}
namespace gea::host {
AudioWorkletProcessor::AudioWorkletProcessor() : port(processorChannel.port2) {}
namespace audio_worklet {
workers::MessagePort processorPort() { return processorChannel.port2; }
void registerProcessor(const std::string&, ProcessorFactory factory) { processorFactory = std::move(factory); }
double sampleRate() { return 24000; }
double currentTime() { return 0; }
double currentFrame() { return 0; }
}
}

// This declaration intentionally exposes a missing platform global mapping as a
// link error. The production processor must call audio_worklet::sampleRate().
int main() {
  using namespace gea::host;
  GEA_COMPILED_WORKLET_ENTRY();
  assert(processorFactory);
  auto processor = processorFactory();
  workers::MessageChannel network;
  unsigned packets = 0;
  network.port1.setOnMessage([&](workers::MessageEvent& event) {
    if (auto bytes = std::get_if<std::vector<std::uint8_t>>(&event.data.value)) {
      ++packets;
      network.port1.postMessage(std::string("recycle"));
      network.port1.postMessage(std::move(*bytes));
    }
  });
  processorChannel.port1.postMessage(std::string("connect"), {network.port2});
  auto pump = [] { workers::Context::main()->runPending(100); };
  pump();
  network.port1.postMessage(std::string("capture"));
  pump();
  AudioWorkletBus input{{std::vector<float>(128, 0.2f)}};
  AudioWorkletBus output{{std::vector<float>(128)}};
  AudioWorkletParameters parameters;
  constexpr unsigned iterations = 10000;
  double processMicros = 0, maximumMicros = 0;
  std::vector<double> times;
  times.reserve(iterations);
  for (unsigned index = 0; index < iterations; ++index) {
    if (index % 4 == 0) {
      std::vector<std::uint8_t> pcm(512 * sizeof(std::int16_t));
      for (unsigned frame = 0; frame < 512; ++frame) {
        pcm[frame * 2] = 0;
        pcm[frame * 2 + 1] = 32;
      }
      network.port1.postMessage(std::move(pcm));
      pump();
    }
    const auto start = std::chrono::steady_clock::now();
    assert(processor->process(input, output, parameters));
    const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    times.push_back(elapsed);
    processMicros += elapsed;
    maximumMicros = std::max(maximumMicros, elapsed);
    assert(output[0][0][0] == 0.25f);
    pump();
  }
  assert(packets > 1000);
  std::sort(times.begin(), times.end());
  std::printf("actual PCM processor: %u quanta, %u transferred capture packets, mean %.2fus, p99 %.2fus, max %.2fus (desktop)\n",
    iterations, packets, processMicros / iterations, times[iterations * 99 / 100], maximumMicros);
  network.port1.close();
  processorChannel.port1.close();
  processorChannel.port2.close();
  processor.reset();
  processorFactory = {};
  gea::collectCycles();
}
