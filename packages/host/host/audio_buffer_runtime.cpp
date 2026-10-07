// SPDX-License-Identifier: Apache-2.0
// Compiler-backed typed channel views belong to the app-sensitive host layer.
#include "gea/embedded.h"
#define GEA_HOST_DECLARED 1
#include "gea_runtime.h"
#include "host/audio.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gea::host {

struct AudioBufferStorage {
  std::vector<gea::Ref<gea::TypedArray<float>>> planes;
  std::vector<std::int16_t> pcm;
  std::size_t length = 0;
};

void AudioBuffer::ensureStorage() const {
  if (storage)
    return;
  if (channels <= 0)
    throw std::out_of_range("AudioBuffer channel count is invalid");
  auto next = std::make_shared<AudioBufferStorage>();
  next->length = samples.size() / static_cast<std::size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    auto plane = gea::makeRef<gea::TypedArray<float>>(next->length);
    for (std::size_t index = 0; index < next->length; ++index)
      plane->data()[index] =
          static_cast<float>(samples[index * channels + channel]) / 32768.0f;
    next->planes.push_back(std::move(plane));
  }
  storage = std::move(next);
}

double AudioBuffer::getLength() const {
  return storage
             ? static_cast<double>(storage->length)
             : (channels > 0 ? static_cast<double>(samples.size() / channels)
                             : 0.0);
}

double AudioBuffer::getDuration() const { return getLength() / sampleRate; }

double AudioBuffer::getNumberOfChannels() const { return channels; }

gea::Ref<gea::TypedArray<float>>
AudioBuffer::getChannelData(double channel) const {
  if (!std::isfinite(channel) || channel < 0 || std::trunc(channel) >= channels)
    throw std::out_of_range("AudioBuffer channel is out of range");
  ensureStorage();
  return storage->planes[static_cast<std::size_t>(channel)];
}

void AudioBuffer::copyToChannelValues(const float *source, std::size_t count,
                                      double channel, double offset) const {
  auto plane = getChannelData(channel);
  if (!std::isfinite(offset) || offset < 0 ||
      std::trunc(offset) >= plane->size())
    throw std::out_of_range("AudioBuffer channel offset is out of range");
  const auto first = static_cast<std::size_t>(offset);
  const auto copied = std::min(count, plane->size() - first);
  if (copied)
    std::memmove(plane->data() + first, source, copied * sizeof(float));
}

void AudioBuffer::copyFromChannelValues(float *destination, std::size_t count,
                                        double channel, double offset) const {
  auto plane = getChannelData(channel);
  if (!std::isfinite(offset) || offset < 0 ||
      std::trunc(offset) >= plane->size())
    throw std::out_of_range("AudioBuffer channel offset is out of range");
  const auto first = static_cast<std::size_t>(offset);
  const auto copied = std::min(count, plane->size() - first);
  if (copied)
    std::memmove(destination, plane->data() + first, copied * sizeof(float));
}

const std::vector<std::int16_t> &AudioBuffer::pcmSamples() const {
  if (!storage)
    return samples;
  storage->pcm.resize(storage->length * channels);
  for (std::size_t index = 0; index < storage->length; ++index) {
    for (int channel = 0; channel < channels; ++channel) {
      const float value = storage->planes[channel]->data()[index];
      const float bounded =
          std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
      const auto sample = std::lround(bounded * 32768.0f);
      storage->pcm[index * channels + channel] =
          static_cast<std::int16_t>(std::clamp(sample, -32768L, 32767L));
    }
  }
  return storage->pcm;
}

AudioBuffer AudioContext::createBuffer(double channelCount, double length,
                                       double rate) const {
  if (!std::isfinite(channelCount) || channelCount < 1 || channelCount >= 33 ||
      !std::isfinite(length) || length < 1 ||
      length > std::numeric_limits<int>::max() || !std::isfinite(rate) ||
      rate < 8000 || rate > 96000 || std::trunc(rate) != rate)
    throw std::invalid_argument(
        "AudioBuffer dimensions or sample rate are unsupported");
  AudioBuffer buffer;
  buffer.channels = static_cast<int>(channelCount);
  buffer.sampleRate = static_cast<int>(rate);
  buffer.storage = std::make_shared<AudioBufferStorage>();
  buffer.storage->length = static_cast<std::size_t>(length);
  for (int channel = 0; channel < buffer.channels; ++channel)
    buffer.storage->planes.push_back(
        gea::makeRef<gea::TypedArray<float>>(buffer.storage->length));
  return buffer;
}

} // namespace gea::host
