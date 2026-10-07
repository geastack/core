#include "gea/embedded-host.h"
namespace gea::embedded::ui {
template <typename T> class Signal;
}
#define GEA_HOST_DECLARED 1
#include "gea_runtime.h"
#include <cassert>
#include <cmath>
#include <limits>

namespace {
std::vector<std::int16_t> played;
int playedRate = 0;
int playedChannels = 0;
} // namespace
namespace gea::platform::audio {
bool AudioSystem::playPcm(const std::int16_t *data, std::size_t count, int rate,
                          int channels) {
  played.assign(data, data + count);
  playedRate = rate;
  playedChannels = channels;
  return true;
}
void AudioSystem::stopPlayback() {}
} // namespace gea::platform::audio
int main() {
  gea::host::AudioContext context;
  auto buffer = context.createBuffer(2, 4, 44100);
  assert(buffer.getLength() == 4 && buffer.getNumberOfChannels() == 2);
  assert(buffer.getDuration() == 4.0 / 44100);
  auto left = buffer.getChannelData(0);
  auto right = buffer.getChannelData(1);
  for (std::size_t i = 0; i < 4; ++i)
    assert(left->data()[i] == 0);
  left->data()[0] = -1;
  left->data()[1] = 123.0f / 32768;
  gea::host::AudioBufferSourceNode missing;
  assert(!static_cast<bool>(missing) && missing == nullptr);
  auto source = context.createBufferSource();
  assert(static_cast<bool>(source) && source != nullptr);
  assert(!source.connected);
  auto sourceCopy = source;
  assert(static_cast<bool>(sourceCopy));
  source.buffer = buffer;
  left->data()[2] = 2;
  left->data()[3] = std::numeric_limits<float>::quiet_NaN();
  auto values = gea::makeRef<gea::TypedArray<float>>(2);
  values->data()[0] = 0.5f;
  values->data()[1] = -0.5f;
  buffer.copyToChannel(values, 1, 2);
  auto destination = gea::makeRef<gea::TypedArray<float>>(3);
  destination->data()[2] = 7;
  buffer.copyFromChannel(destination, 1, 2);
  assert(destination->data()[0] == 0.5f && destination->data()[1] == -0.5f &&
         destination->data()[2] == 7);
  source.connect(gea::host::AudioDestinationNode(1.0));
  source.start();
  assert(playedRate == 44100 && playedChannels == 2);
  assert((played == std::vector<std::int16_t>{-32768, 0, 123, 0, 32767, 16384,
                                              0, -16384}));
  gea::host::AudioBuffer decoded({-123, 123}, 44100, 1);
  auto decodedCopy = decoded;
  decoded.getChannelData(0)->data()[0] = 321.0f / 32768;
  assert(decodedCopy.pcmSamples()[0] == 321);
  source.stop();
  assert(static_cast<bool>(source) && source != nullptr);
  bool threw = false;
  try {
    buffer.getChannelData(2);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw);
  threw = false;
  try {
    context.createBuffer(0, 4, 44100);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  threw = false;
  try {
    buffer.copyToChannel(values, 0, 4);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw);
}
