#include "host/pcm_stream.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  using namespace gea::host::pcm;
  const std::vector<std::int16_t> extremes{0, -32768, 32767, -1, 1};
  assert(decode(encode(extremes)) == extremes);
  assert(encode({0}) == "AAA=");
  assert(encode({-1}) == "//8=");
  // Reduced-ratio integer interpolation must remain bit exact, including
  // opposite full-scale samples and phase carried across chunk boundaries.
  for (int inputRate : {16000, 24000, 48000}) for (int outputRate : {16000, 24000, 48000}) {
    Resampler converter(inputRate, outputRate);
    std::vector<std::int16_t> actual, expected;
    int phase = 0, previous = 0;
    for (int i = 0; i < 1000; ++i) {
      const std::int16_t sample = i % 2 ? -32768 : 32767;
      if (!i) previous = sample;
      phase += outputRate;
      while (phase >= inputRate) {
        phase -= inputRate;
        expected.push_back(static_cast<std::int16_t>(previous +
            (std::int64_t(sample) - previous) * (outputRate - phase) / outputRate));
      }
      previous = sample;
      converter.push(sample, [&](auto value) { actual.push_back(value); });
    }
    assert(actual == expected);
  }
  for (const auto& invalid : {"!AAA", "AAA", "AA==", "AAAA", "A===", "AA=A", "AAB="}) {
    bool rejected = false;
    try { decode(invalid); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  for (const int rate : {16000, 24000, 48000}) {
    std::vector<std::int16_t> input(16000), wire, output;
    for (int i = 0; i < 16000; ++i) input[i] = 12000 * std::sin(i * 440.0 * 6.28318530718 / 16000);
    Resampler up(16000, rate), down(rate, 16000);
    for (const auto sample : input) up.push(sample, [&](auto value) { wire.push_back(value); });
    assert(wire.size() == static_cast<unsigned>(rate));
    // Network chunks can be any sample-aligned size; retain phase between all.
    for (std::size_t i = 0; i < wire.size(); i += 137) {
      const std::vector<std::int16_t> chunk(wire.begin() + i, wire.begin() + std::min(i + 137, wire.size()));
      for (auto sample : decode(encode(chunk))) down.push(sample, [&](auto value) { output.push_back(value); });
    }
    assert(output.size() == input.size());
    double energy = 0;
    for (auto v : output) energy += double(v) * v;
    assert(energy / output.size() > 65000000);
    up.reset();
    std::vector<std::int16_t> silence;
    for (int i = 0; i < 640; ++i) up.push(0, [&](auto v) { silence.push_back(v); });
    assert(silence.size() == static_cast<unsigned>(rate / 25));
    for (auto v : silence) assert(v == 0);
  }
  std::cout << "PCM base64, sample counts, cross-chunk resampling and reset passed\n";
}
