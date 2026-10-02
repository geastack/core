// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>
#include <numeric>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace gea::host::pcm {

// Small deque blocks would otherwise prefer internal RAM on ESP-IDF. Audio
// queued from a faster-than-realtime producer must not starve I2S DMA memory.
template <typename T>
struct QueueAllocator {
  using value_type = T;
  QueueAllocator() = default;
  template <typename U> QueueAllocator(const QueueAllocator<U>&) {}
  T *allocate(std::size_t count) {
#ifdef ESP_PLATFORM
    auto *memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_8BIT);
    if (!memory) throw std::bad_alloc();
    return static_cast<T *>(memory);
#else
    return std::allocator<T>{}.allocate(count);
#endif
  }
  void deallocate(T *memory, std::size_t count) {
#ifdef ESP_PLATFORM
    (void)count;
    heap_caps_free(memory);
#else
    std::allocator<T>{}.deallocate(memory, count);
#endif
  }
  template <typename U> bool operator==(const QueueAllocator<U>&) const { return true; }
};


// Native network PCM needs a small startup reservoir before DMA consumes it.
// This is a one-time gate, never a minimum queue retained during playback:
// short tails are released after the bounded wait and no sample is discarded.
class PlaybackStartGate {
 public:
  void enable(std::uint32_t delayMs) { delayMs_ = delayMs; reset(); }
  void pushed(std::uint32_t nowMs) {
    if (!received_) { firstPacketMs_ = nowMs; received_ = true; }
  }
  bool ready(std::size_t queuedSamples, std::uint32_t nowMs) {
    if (!delayMs_ || started_) return true;
    if (!queuedSamples) return false;
    // A TCP burst is not a realtime cadence: even a large first batch must
    // wait for the arrival timeline to establish its lead over playback.
    if (!received_ || std::uint32_t(nowMs - firstPacketMs_) < delayMs_) return false;
    started_ = true;
    return true;
  }
  void reset() { received_ = started_ = false; firstPacketMs_ = 0; }
 private:
  std::uint32_t delayMs_ = 0, firstPacketMs_ = 0;
  bool received_ = false, started_ = false;
};

// Causal linear conversion: preserve fractional position across every network
// chunk. Input/output are signed mono PCM, never independently resampled frames.
class Resampler {
 public:
  Resampler(int inputRate, int outputRate) : input_(inputRate), output_(outputRate) {
    if (inputRate <= 0 || outputRate <= 0) throw std::invalid_argument("Invalid PCM rate");
    const auto divisor = std::gcd(input_, output_);
    input_ /= divisor;
    output_ /= divisor;
  }
  template <typename Write>
  void push(std::int16_t sample, Write write) {
    if (input_ == output_) {
      previous_ = sample;
      started_ = true;
      write(sample);
      return;
    }
    if (!started_) { previous_ = sample; started_ = true; }
    phase_ += output_;
    while (phase_ >= input_) {
      phase_ -= input_;
      const auto delta = static_cast<std::int32_t>(sample) - previous_;
      // Supported PCM rates reduce to ratios no larger than 3. Avoid a
      // software 64-bit divide for every sample on 32-bit devices, retaining
      // the wide fallback for arbitrary large coprime sample rates.
      const auto interpolated = output_ <= 32768
          ? delta * (output_ - phase_) / output_
          : static_cast<std::int32_t>(std::int64_t(delta) * (output_ - phase_) / output_);
      write(static_cast<std::int16_t>(previous_ + interpolated));
    }
    previous_ = sample;
  }
  void reset() { phase_ = previous_ = 0; started_ = false; }
 private:
  int input_, output_, phase_ = 0, previous_ = 0;
  bool started_ = false;
};

inline std::string encode(const std::int16_t* pcm, std::size_t samples) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto bytes = samples * 2;
  // Size once: appending every base64 character repeatedly reads/writes the
  // string metadata in PSRAM on the realtime network worker.
  std::string result((bytes + 2) / 3 * 4, '=');
  const auto byte = [&](size_t index) {
    return (static_cast<std::uint16_t>(pcm[index / 2]) >> ((index % 2) * 8)) & 255u;
  };
  for (size_t input = 0, output = 0; input < bytes; input += 3, output += 4) {
    const auto first = byte(input);
    const auto second = input + 1 < bytes ? byte(input + 1) : 0;
    const auto third = input + 2 < bytes ? byte(input + 2) : 0;
    result[output] = alphabet[first >> 2];
    result[output + 1] = alphabet[((first & 3) << 4) | (second >> 4)];
    if (input + 1 < bytes) result[output + 2] = alphabet[((second & 15) << 2) | (third >> 6)];
    if (input + 2 < bytes) result[output + 3] = alphabet[third & 63];
  }
  return result;
}

inline std::string encode(const std::vector<std::int16_t>& pcm) {
  return encode(pcm.data(), pcm.size());
}

inline std::vector<std::int16_t> decode(const std::string& encoded) {
  if (encoded.size() % 4 || encoded.size() > 1024 * 1024)
    throw std::invalid_argument("Invalid PCM base64 length");
  if (encoded.empty()) return {};
  static constexpr auto values = [] {
    std::array<std::int8_t, 256> table{};
    table.fill(-1);
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int index = 0; index < 64; ++index) table[static_cast<unsigned char>(alphabet[index])] = index;
    return table;
  }();
  const size_t padding = (encoded.back() == '=') + (encoded[encoded.size() - 2] == '=');
  const size_t bytes = encoded.size() / 4 * 3 - padding;
  if (bytes % 2) throw std::invalid_argument("Incomplete PCM sample");
  std::vector<std::int16_t> result(bytes / 2);
  size_t output = 0;
  int low = -1;
  const auto write = [&](unsigned byte) {
    if (low < 0) low = byte;
    else { result[output++] = static_cast<std::int16_t>(low | (byte << 8)); low = -1; }
  };
  for (size_t input = 0; input < encoded.size(); input += 4) {
    const bool last = input + 4 == encoded.size();
    const auto first = values[static_cast<unsigned char>(encoded[input])];
    const auto second = values[static_cast<unsigned char>(encoded[input + 1])];
    const auto third = last && padding == 2 ? 0 : values[static_cast<unsigned char>(encoded[input + 2])];
    const auto fourth = last && padding ? 0 : values[static_cast<unsigned char>(encoded[input + 3])];
    if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
        (last && padding == 2 && (second & 15)) ||
        (last && padding == 1 && (third & 3)))
      throw std::invalid_argument("Invalid PCM base64");
    write((first << 2) | (second >> 4));
    if (!last || padding < 2) write(((second & 15) << 4) | (third >> 2));
    if (!last || !padding) write(((third & 3) << 6) | fourth);
  }
  return result;
}
} // namespace gea::host::pcm
