#include "host/rtc_opus.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
  gea::host::rtc::OpusAudio codec;
  constexpr int frameSamples = gea::host::rtc::OpusAudio::frameSamples;
  static_assert(frameSamples == 640);
  std::array<int16_t, frameSamples> input;
  double energy = 0;
  for (int frame = 0; frame < 25; ++frame) {
    for (int i = 0; i < frameSamples; ++i) input[i] = 12000 * std::sin((frame * frameSamples + i) * 440.0 * 6.28318530718 / 16000);
    int bytes = codec.encode(input.data());
    assert(bytes > 0 && bytes < 640); // compressed packet, never raw PCM
    int samples = codec.decode(codec.packet.data(), bytes);
    assert(samples == frameSamples);
    assert(opus_packet_get_nb_samples(codec.packet.data(), bytes, 48000) == 1920);
    for (int i = 0; i < samples; ++i) energy += double(codec.decoded[i]) * codec.decoded[i];
  }
  assert(energy / (25 * frameSamples) > 1000000);
  const uint8_t malformed[] = {255};
  assert(codec.decode(malformed, 1) < 0);
  using Codec = gea::host::rtc::OpusAudio;
  Codec sender(Codec::Application::LowDelay, Codec::Direction::Encode);
  Codec receiver(Codec::Application::LowDelay, Codec::Direction::Decode);
  int bytes = sender.encode(input.data());
  assert(bytes > 0);
  assert(receiver.decode(sender.packet.data(), bytes) == frameSamples);
  assert(sender.decode(sender.packet.data(), bytes) < 0);
  assert(receiver.encode(input.data()) < 0);
  assert(sender.encode(nullptr) < 0);
  assert(receiver.decode(nullptr, bytes) < 0);
  std::cout << "RTC Opus: 40ms PCM/packet round trip and malformed packet rejection passed\n";
}
