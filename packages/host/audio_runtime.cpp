// SPDX-License-Identifier: Apache-2.0
#define GEA_AUDIO_DRIVER_INTERNAL 1
#include "audio.h"
#include "audio_stream.h"
#include "audio_oscillator.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "platform/file_cache.h"

// Optional build-embedded asset registry supplied by the native app pipeline.
extern "C" bool gea_embedded_asset_lookup(const char*, const unsigned char**, unsigned long*)
    __attribute__((weak));

namespace audio = gea::platform::audio;

namespace {

constexpr int kSampleRate = audio::deviceSampleRate;
constexpr int kChannels = 2;
constexpr int kQueueDepth = 16;
// ESP-IDF sizes a task stack in BYTES (`StackType_t` is `uint8_t` on this
// port), not in FreeRTOS "words". This constant was named and sized as words:
// the tone task ran on 4 KB, and the first `OutputDriver::open()` on
// ESP-IDF 6 -- I2S channel creation, the ES8311 bring-up over the I2C master
// driver and the log lines that pass through the diagnostics vprintf sink,
// all on this task -- overran it before a single sample played
// ("A stack overflow in task gea_audio has been detected"; button-tetris on
// the 2.06 AMOLED rebooted every 3 seconds). The task logs its high-water
// mark once after the driver opens so the next reader sizes it from a number.
constexpr int kTaskStackBytes = 8192;
// PCM feeding has a hardware deadline. The AMOLED UI runs at priority 23;
// letting it preempt this task produced 142 ms writes for 32 ms chunks even
// with PCM available, exceeding the 90 ms DMA ring. Mixing is bounded and
// the I2S write blocks for pacing, so audio can safely preempt rendering.
constexpr UBaseType_t kTaskPriority = configMAX_PRIORITIES - 1;
constexpr int kChunkSamples = 512;
constexpr int kAmplitude = 5200;
constexpr int kMaxOscillators = 8;
constexpr int kMaxActiveTones = 12;
constexpr int kMaxToneMs = 2500;
constexpr int kWriteTimeoutMs = 250;
constexpr int64_t kDropLogIntervalUs = 1000000;
constexpr int kPlaybackChunkFrames = 512;
constexpr int kPlaybackWriteTimeoutMs = 250;
constexpr int kChunkDurationMs = (kChunkSamples * 1000) / kSampleRate;
// Notes are scheduled back-to-back with no built-in gap, but render-thread
// jitter (line clears, board redraws) can momentarily empty activeTones_
// between notes. Closing the codec on every such gap and reopening it for the
// next note reinitializes the ES8311 over I2C and retoggles the PA GPIO each
// time (see chip_bindings/audio/es8311.cpp open()/close()) — audible as
// clicking/stutter through a whole melody. Ride out short gaps with silence
// instead of tearing the codec down.
constexpr int kCloseGraceChunks = 10;

#ifndef GEA_AUDIO_DEBUG_TIMING
#define GEA_AUDIO_DEBUG_TIMING 0
#endif

#ifndef GEA_AUDIO_DEBUG_TIMING_THRESHOLD_US
#define GEA_AUDIO_DEBUG_TIMING_THRESHOLD_US 500
#endif

#ifndef GEA_EMBEDDED_AUDIO_DISABLED
#define GEA_EMBEDDED_AUDIO_DISABLED 0
#endif

class AudioEngine {
public:
  static AudioEngine &instance() {
    static AudioEngine engine;
    return engine;
  }

  double currentTime() const {
    return static_cast<double>(esp_timer_get_time()) / 1000000.0;
  }

  audio::NativeAudioHandle destination() const {
    return 0;
  }

  audio::NativeAudioHandle createOscillator() {
    const int slot = nextOscillatorSlot_;
    nextOscillatorSlot_ = (nextOscillatorSlot_ + 1) % kMaxOscillators;
    auto &oscillator = oscillators_[slot];
    oscillator = OscillatorState{};
    oscillator.inUse = true;
    return nativeHandle(oscillator);
  }

  audio::OscillatorType oscillatorType(audio::NativeAudioHandle handle) const {
    const auto *osc = oscillator(handle);
    return osc ? osc->type : audio::OscillatorType::Sine;
  }

  void setOscillatorType(audio::NativeAudioHandle handle, audio::OscillatorType type) {
    auto *osc = oscillator(handle);
    if (!osc) return;
    osc->type = normalizeType(type);
  }

  double oscillatorFrequency(audio::NativeAudioHandle handle) const {
    const auto *osc = oscillator(handle);
    return osc ? osc->frequencyHz : 0.0;
  }

  void setOscillatorFrequency(audio::NativeAudioHandle handle, double frequencyHz) {
    auto *osc = oscillator(handle);
    if (!osc) return;
    osc->frequencyHz = frequencyHz;
  }

  void setOscillatorFrequencyAtTime(audio::NativeAudioHandle handle, double frequencyHz, double startTime) {
    (void)startTime;
    setOscillatorFrequency(handle, frequencyHz);
  }

  void connectOscillator(audio::NativeAudioHandle handle, audio::NativeAudioHandle destination) {
    (void)destination;
    auto *osc = oscillator(handle);
    if (!osc) return;
    osc->connected = true;
  }

  void startOscillator(audio::NativeAudioHandle handle, double when) {
    auto *osc = oscillator(handle);
    if (!osc) return;
    osc->schedule.start(when, currentTime());
  }

  void stopOscillator(audio::NativeAudioHandle handle, double when) {
    auto *osc = oscillator(handle);
    if (!osc || !osc->connected) return;

    const auto timing = osc->schedule.stop(when, currentTime());
    if (!timing) return;

    submitTone(Tone{
        osc->type,
        osc->frequencyHz,
        timing->durationMs,
        timing->delayMs,
    });
  }

  int volume() const {
    return audio::OutputDriver::volume();
  }

  void setVolume(int volumePercent) {
    audio::OutputDriver::setVolume(volumePercent);
  }

  void beginExclusivePlayback() {
    exclusivePlayback_.store(true, std::memory_order_release);
    clearTones();
    if (driverOpen_.exchange(false, std::memory_order_acq_rel)) {
      audio::OutputDriver::close();
    }
  }

  void endExclusivePlayback() {
    clearTones();
    exclusivePlayback_.store(false, std::memory_order_release);
  }

  void stopPlayback() {
    streams_.clear();
    clearTones();
    driverOpen_.store(false, std::memory_order_release);
    audio::OutputDriver::close();
  }

  std::uint64_t playPcmStream(audio::PcmStreamMixer::Pull pull) {
#if GEA_EMBEDDED_AUDIO_DISABLED
    return 0;
#else
    ensureTask();
    if (!audioTask_ || !toneQueue_) return 0;
    // Half-duplex capture can close the speaker between replies. Revalidate
    // its hardware state when the first playback source attaches again.
    if (!streams_.active()) driverOpen_.store(false, std::memory_order_release);
    auto handle = streams_.add(std::move(pull));
    // Wake a task waiting for a tone without manufacturing an audible note.
    Tone wake;
    wake.wakeOnly = true;
    xQueueSend(toneQueue_, &wake, 0);
    return handle;
#endif
  }
  void flushPlayback() {
    // The sole output consumer cancels DMA before its next pull. A write
    // already in progress finishes BEFORE that cancellation. Never wait for
    // a driver/mixer lock on the microphone's audio-worklet thread.
    flushPending_.store(true, std::memory_order_release);
  }
  void stopPcmStream(std::uint64_t handle) { streams_.remove(handle); }
  bool pcmStreamSettled(std::uint64_t handle) { return streams_.settled(handle); }

private:
  struct Tone {
    audio::OscillatorType type = audio::OscillatorType::Sine;
    double frequencyHz = 440.0;
    int durationMs = 0;
    int delayMs = 0;
    bool wakeOnly = false;
  };

  struct OscillatorState {
    bool inUse = false;
    audio::OscillatorType type = audio::OscillatorType::Sine;
    double frequencyHz = 440.0;
    bool connected = false;
    audio::FiniteOscillatorSchedule schedule;
  };

  struct ActiveTone {
    bool inUse = false;
    audio::OscillatorType type = audio::OscillatorType::Sine;
    std::uint32_t phase = 0;
    std::uint32_t phaseStep = 0;
    int totalSamples = 0;
    int sampleIndex = 0;
    int delaySamples = 0;
  };

  AudioEngine() = default;

  static audio::OscillatorType normalizeType(audio::OscillatorType type) {
    switch (type) {
      case audio::OscillatorType::Square:
      case audio::OscillatorType::Sawtooth:
      case audio::OscillatorType::Triangle:
      case audio::OscillatorType::Sine: return type;
    }
    return audio::OscillatorType::Sine;
  }

  static int envelope(int sampleIndex, int remainingSamples) {
    constexpr int attackSamples = 80;
    constexpr int releaseSamples = 180;
    int level = 256;
    if (sampleIndex < attackSamples) level = (sampleIndex * 256) / attackSamples;
    if (remainingSamples < releaseSamples) {
      const int releaseLevel = (remainingSamples * 256) / releaseSamples;
      if (releaseLevel < level) level = releaseLevel;
    }
    if (level < 0) return 0;
    if (level > 256) return 256;
    return level;
  }

  static int waveformSample(audio::OscillatorType type, std::uint32_t phase) {
    if (type == audio::OscillatorType::Square) {
      return (phase & 0x80000000u) ? kAmplitude : -kAmplitude;
    }

    if (type == audio::OscillatorType::Sine) {
      constexpr double kTau = 6.28318530717958647692;
      const double cycle = static_cast<double>(phase) / 4294967296.0;
      return static_cast<int>(std::sin(cycle * kTau) * static_cast<double>(kAmplitude));
    }

    const std::uint32_t pos = phase >> 16;
    if (type == audio::OscillatorType::Sawtooth) {
      return ((static_cast<int>(pos) - 32768) * kAmplitude) / 32768;
    }

    if (pos < 32768) {
      return -kAmplitude + (static_cast<int>(pos) * kAmplitude * 2) / 32768;
    }
    return kAmplitude - ((static_cast<int>(pos) - 32768) * kAmplitude * 2) / 32768;
  }

  static int clampSample(int sample) {
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return sample;
  }

  static void normalizeTone(Tone &tone) {
    tone.type = normalizeType(tone.type);
    if (tone.frequencyHz < 40.0) tone.frequencyHz = 40.0;
    if (tone.frequencyHz > 4000.0) tone.frequencyHz = 4000.0;
    if (tone.durationMs < 10) tone.durationMs = 10;
    if (tone.durationMs > kMaxToneMs) tone.durationMs = kMaxToneMs;
    if (tone.delayMs < 0) tone.delayMs = 0;
  }

  static audio::NativeAudioHandle nativeHandle(OscillatorState &oscillator) {
    return reinterpret_cast<audio::NativeAudioHandle>(&oscillator);
  }

  OscillatorState *oscillator(audio::NativeAudioHandle handle) {
    auto *osc = reinterpret_cast<OscillatorState *>(handle);
    if (!owns(osc) || !osc->inUse) return nullptr;
    return osc;
  }

  const OscillatorState *oscillator(audio::NativeAudioHandle handle) const {
    const auto *osc = reinterpret_cast<const OscillatorState *>(handle);
    if (!owns(osc) || !osc->inUse) return nullptr;
    return osc;
  }

  bool owns(const OscillatorState *oscillator) const {
    const auto address = reinterpret_cast<audio::NativeAudioHandle>(oscillator);
    const auto begin = reinterpret_cast<audio::NativeAudioHandle>(oscillators_);
    const auto end = reinterpret_cast<audio::NativeAudioHandle>(oscillators_ + kMaxOscillators);
    return address >= begin && address < end;
  }

  int activeToneRemaining(const ActiveTone &tone) const {
    if (!tone.inUse) return 0;
    int remaining = tone.totalSamples - tone.sampleIndex;
    if (remaining < 0) remaining = 0;
    return tone.delaySamples + remaining;
  }

  bool hasActiveTones() const {
    for (const auto &tone : activeTones_) {
      if (tone.inUse) return true;
    }
    return false;
  }

  void clearTones() {
    for (auto &tone : activeTones_) tone = ActiveTone{};
    if (!toneQueue_) return;
    Tone dropped;
    while (xQueueReceive(toneQueue_, &dropped, 0) == pdTRUE) {}
  }

  void addActiveTone(Tone tone) {
    if (tone.wakeOnly) return;
    normalizeTone(tone);

    const int totalSamples = (kSampleRate * tone.durationMs) / 1000;
    if (totalSamples <= 0) return;

    int slot = -1;
    int shortestRemaining = INT_MAX;
    for (int i = 0; i < kMaxActiveTones; i++) {
      if (!activeTones_[i].inUse) {
        slot = i;
        break;
      }
      const int remaining = activeToneRemaining(activeTones_[i]);
      if (remaining < shortestRemaining) {
        shortestRemaining = remaining;
        slot = i;
      }
    }

    activeTones_[slot] = ActiveTone{
        true,
        tone.type,
        0,
        static_cast<std::uint32_t>((tone.frequencyHz * 4294967296.0) / kSampleRate),
        totalSamples,
        0,
        (kSampleRate * tone.delayMs) / 1000,
    };
  }

  void renderChunk(std::int16_t *pcm, int samples) {
    for (int i = 0; i < samples; i++) {
      int mixed = 0;
      for (auto &tone : activeTones_) {
        if (!tone.inUse) continue;

        if (tone.delaySamples > 0) {
          tone.delaySamples--;
          continue;
        }

        const int remaining = tone.totalSamples - tone.sampleIndex;
        if (remaining <= 0) {
          tone.inUse = false;
          continue;
        }

        const int level = envelope(tone.sampleIndex, remaining);
        mixed += (waveformSample(tone.type, tone.phase) * level) / 256;
        tone.phase += tone.phaseStep;
        tone.sampleIndex++;
        if (tone.sampleIndex >= tone.totalSamples) {
          tone.inUse = false;
        }
      }

      const auto sample = static_cast<std::int16_t>(clampSample(mixed));
      for (int ch = 0; ch < kChannels; ch++) {
        pcm[i * kChannels + ch] = sample;
      }
    }
  }

  void runAudioTask() {
    Tone tone;
    std::int16_t pcm[kChunkSamples * kChannels];
    int idleChunks = 0;
    int64_t previousWriteEnd = 0, cadenceStart = 0, maxSupplyGap = 0, maxWriteUs = 0;
    std::size_t cadenceFrames = 0, emptyPulls = 0;

    while (true) {
      if (flushPending_.exchange(false, std::memory_order_acq_rel) &&
          driverOpen_.load(std::memory_order_acquire) && !audio::OutputDriver::flush())
        ESP_LOGW(kTag, "Unable to discard speaker DMA samples");
      if (exclusivePlayback_.load(std::memory_order_acquire)) {
        clearTones();
        idleChunks = 0;
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      while (xQueueReceive(toneQueue_, &tone, 0) == pdTRUE) {
        addActiveTone(tone);
        idleChunks = 0;
      }

      const bool streaming = streams_.active();
      if (!hasActiveTones() && !streaming) {
        previousWriteEnd = cadenceStart = maxSupplyGap = maxWriteUs = 0;
        cadenceFrames = emptyPulls = 0;
        if (!driverOpen_.load(std::memory_order_acquire)) {
          if (xQueueReceive(toneQueue_, &tone, portMAX_DELAY) == pdTRUE) {
            addActiveTone(tone);
            idleChunks = 0;
          }
          continue;
        }

        // Driver is open but momentarily idle between notes — wait one
        // chunk's worth of time for the next tone instead of tearing the
        // codec down immediately, so back-to-back notes stay gapless.
        if (xQueueReceive(toneQueue_, &tone, pdMS_TO_TICKS(kChunkDurationMs)) == pdTRUE) {
          addActiveTone(tone);
          idleChunks = 0;
        } else {
          idleChunks++;
          if (idleChunks >= kCloseGraceChunks) {
            audio::OutputDriver::close();
            driverOpen_.store(false, std::memory_order_release);
            idleChunks = 0;
          } else {
            std::memset(pcm, 0, sizeof(pcm));
            audio::OutputDriver::write(pcm, kChunkSamples * kChannels, kWriteTimeoutMs);
          }
          continue;
        }
      }

      // Open before consuming PCM, so codec initialization cannot discard
      // the first packet of a reply when capture is still relinquishing I2S.
      if (!driverOpen_.load(std::memory_order_acquire)) {
        if (!audio::OutputDriver::open(kSampleRate, kChannels, 16)) {
          vTaskDelay(pdMS_TO_TICKS(50));
          continue;
        }
        driverOpen_.store(true, std::memory_order_release);
        if (!stackReported_) {
          stackReported_ = true;
          ESP_LOGI(kTag,
                   "Audio task stack: %u of %d bytes still free after the driver opened",
                   static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                   kTaskStackBytes);
        }
      }

      const bool tones = hasActiveTones();
      // Streaming speech has no oscillator voices to render. Avoid walking
      // every tone slot for every sample, including the empty pulls between
      // incoming packets, in this highest-priority task.
      if (tones) renderChunk(pcm, kChunkSamples);
      else std::memset(pcm, 0, sizeof(pcm));
      const auto streamFrames = streams_.mix(pcm, kChunkSamples);
      const auto outputFrames = tones ? std::size_t(kChunkSamples) : streamFrames;
      if (outputFrames == 0) {
        ++emptyPulls;
        // Empty between RTP packets means wait for data, never end a stream.
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      const int64_t writeStart = esp_timer_get_time();
      if (!cadenceStart) cadenceStart = writeStart;
      if (previousWriteEnd) maxSupplyGap = std::max(maxSupplyGap, writeStart - previousWriteEnd);
      const bool wrote = audio::OutputDriver::write(pcm, outputFrames * kChannels, kWriteTimeoutMs);
      previousWriteEnd = esp_timer_get_time();
      maxWriteUs = std::max(maxWriteUs, previousWriteEnd - writeStart);
      cadenceFrames += outputFrames;
      if (streaming && previousWriteEnd - cadenceStart >= 2000000) {
        ESP_LOGI(kTag, "PCM cadence span_ms=%lld frames=%u empty_pulls=%u max_supply_gap_ms=%lld max_write_ms=%lld",
                 static_cast<long long>((previousWriteEnd - cadenceStart) / 1000),
                 static_cast<unsigned>(cadenceFrames), static_cast<unsigned>(emptyPulls),
                 static_cast<long long>(maxSupplyGap / 1000), static_cast<long long>(maxWriteUs / 1000));
        cadenceStart = previousWriteEnd;
        cadenceFrames = emptyPulls = 0;
        maxSupplyGap = maxWriteUs = 0;
      }
      streams_.didWrite(wrote);
      if (!wrote) {
        ESP_LOGW(kTag, "PCM write failed");
        driverOpen_.store(false, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
  }

  void ensureTask() {
#if GEA_AUDIO_DEBUG_TIMING
    const int64_t totalStartUs = esp_timer_get_time();
    int64_t queueUs = 0;
    int64_t taskUs = 0;
    bool createdQueue = false;
    bool attemptedTask = false;
    bool startedTask = false;
#endif

    if (!toneQueue_) {
#if GEA_AUDIO_DEBUG_TIMING
      const int64_t queueStartUs = esp_timer_get_time();
#endif
      toneQueue_ = xQueueCreateStatic(kQueueDepth, sizeof(Tone), toneQueueBuffer_, &toneQueueStorage_);
#if GEA_AUDIO_DEBUG_TIMING
      queueUs = esp_timer_get_time() - queueStartUs;
      createdQueue = toneQueue_ != nullptr;
#endif
      if (!toneQueue_) {
        ESP_LOGE(kTag, "Tone queue creation failed");
        return;
      }
    }

    if (!audioTask_) {
#if GEA_AUDIO_DEBUG_TIMING
      const int64_t taskStartUs = esp_timer_get_time();
      attemptedTask = true;
#endif
      audioTask_ = xTaskCreateStatic(
          &AudioEngine::audioTaskEntry,
          "gea_audio",
          kTaskStackBytes,
          this,
          kTaskPriority,
          audioTaskStack_,
          &audioTaskStorage_);
#if GEA_AUDIO_DEBUG_TIMING
      taskUs = esp_timer_get_time() - taskStartUs;
      startedTask = audioTask_ != nullptr;
#endif
      if (!audioTask_) {
        if (!audioTaskFailureLogged_) {
          audioTaskFailureLogged_ = true;
          ESP_LOGE(kTag, "Audio task creation failed");
        }
      } else {
        audioTaskFailureLogged_ = false;
      }
    }

#if GEA_AUDIO_DEBUG_TIMING
    const int64_t totalUs = esp_timer_get_time() - totalStartUs;
    const bool shouldLogTiming = totalUs >= GEA_AUDIO_DEBUG_TIMING_THRESHOLD_US;
    if (createdQueue || attemptedTask || shouldLogTiming) {
      ESP_LOGI(kTag,
               "debug: ensure_audio_task queue=%lldus task=%lldus total=%lldus created_queue=%d attempted_task=%d started_task=%d",
               static_cast<long long>(queueUs),
               static_cast<long long>(taskUs),
               static_cast<long long>(totalUs),
               createdQueue ? 1 : 0,
               attemptedTask ? 1 : 0,
               startedTask ? 1 : 0);
    }
#endif
  }

  void submitTone(const Tone &tone) {
#if GEA_EMBEDDED_AUDIO_DISABLED
    (void)tone;
    return;
#else
    if (exclusivePlayback_.load(std::memory_order_acquire)) return;
    ensureTask();
    if (!toneQueue_) return;

    if (xQueueSend(toneQueue_, &tone, 0) != pdPASS) {
      const int64_t nowUs = esp_timer_get_time();
      if (nowUs - lastDropLogUs_ > kDropLogIntervalUs) {
        lastDropLogUs_ = nowUs;
        ESP_LOGW(kTag, "Tone queue full; dropping tone");
      }
    }
#endif
  }

  static void audioTaskEntry(void *arg) {
    static_cast<AudioEngine *>(arg)->runAudioTask();
  }

  static constexpr const char *kTag = "gea_audio";

  QueueHandle_t toneQueue_ = nullptr;
  audio::PcmStreamMixer streams_;
  TaskHandle_t audioTask_ = nullptr;
  OscillatorState oscillators_[kMaxOscillators]{};
  ActiveTone activeTones_[kMaxActiveTones]{};
  int nextOscillatorSlot_ = 0;
  int64_t lastDropLogUs_ = 0;
  StaticQueue_t toneQueueStorage_{};
  std::uint8_t toneQueueBuffer_[kQueueDepth * sizeof(Tone)]{};
  StaticTask_t audioTaskStorage_{};
  StackType_t audioTaskStack_[kTaskStackBytes]{};
  std::atomic<bool> exclusivePlayback_{false};
  std::atomic<bool> flushPending_{false};
  std::atomic<bool> driverOpen_{false};
  bool audioTaskFailureLogged_ = false;
  bool stackReported_ = false;
};

}  // namespace

namespace {

std::uint16_t readLe16(FILE *file)
{
  std::uint8_t bytes[2] = {};
  if (std::fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return 0;
  return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t readLe32(FILE *file)
{
  std::uint8_t bytes[4] = {};
  if (std::fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) return 0;
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool readTag(FILE *file, const char *expected)
{
  char tag[4] = {};
  return std::fread(tag, 1, sizeof(tag), file) == sizeof(tag) && std::memcmp(tag, expected, sizeof(tag)) == 0;
}

struct ExclusivePlaybackScope {
  ExclusivePlaybackScope() { AudioEngine::instance().beginExclusivePlayback(); }
  ~ExclusivePlaybackScope() { AudioEngine::instance().endExclusivePlayback(); }
};

struct OutputCloseScope {
  bool active = false;
  ~OutputCloseScope()
  {
    if (active) audio::OutputDriver::close();
  }
};

bool playPcmSync(const std::int16_t *samples, std::size_t sampleCount, int sampleRate, int channels)
{
  if (!samples || sampleCount == 0) return false;
  if (sampleRate <= 0) sampleRate = kSampleRate;
  if (channels <= 0) channels = 1;
  ExclusivePlaybackScope playbackScope;

  if (!audio::OutputDriver::open(sampleRate, 2, 16)) return false;
  OutputCloseScope closeOutput{true};

  std::vector<std::int16_t> stereo(kPlaybackChunkFrames * 2);
  std::size_t frame = 0;
  const std::size_t frameCount = sampleCount / static_cast<std::size_t>(channels);
  while (frame < frameCount) {
    const std::size_t frames = std::min<std::size_t>(kPlaybackChunkFrames, frameCount - frame);
    if (channels == 2) {
      const std::size_t src = frame * 2;
      std::copy_n(samples + src, frames * 2, stereo.data());
      if (!audio::OutputDriver::write(stereo.data(), frames * 2, kPlaybackWriteTimeoutMs)) return false;
    } else {
      for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t src = (frame + i) * static_cast<std::size_t>(channels);
        stereo[i * 2] = samples[src];
        stereo[i * 2 + 1] = channels == 1 ? samples[src] : samples[src + 1];
      }
      if (!audio::OutputDriver::write(stereo.data(), frames * 2, kPlaybackWriteTimeoutMs)) return false;
    }
    frame += frames;
  }
  return true;
}

// Play imported WAV assets without opening a filesystem path or writing into
// read-only flash. Codec software volume may modify its input buffer.
bool playEmbeddedWav(const unsigned char* bytes, std::size_t length) {
  if (length < 12 || std::memcmp(bytes, "RIFF", 4) || std::memcmp(bytes + 8, "WAVE", 4)) return false;
  const auto le16 = [](const unsigned char* p) { return unsigned(p[0]) | unsigned(p[1]) << 8; };
  const auto le32 = [](const unsigned char *p) {
	  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
			 std::uint32_t(p[3]) << 24;
  };
  unsigned channels = 0, format = 0, bits = 0;
  std::uint32_t rate = 0;
  const unsigned char *pcm = nullptr;
  std::size_t pcmBytes = 0;
  for (std::size_t offset = 12; offset <= length && length - offset >= 8;) {
	  const auto size = le32(bytes + offset + 4);
	  const auto payload = offset + 8;
	  if (size > length - payload) {
		  return false;
	  }
	  if (!std::memcmp(bytes + offset, "fmt ", 4) && size >= 16) {
		  format = le16(bytes + payload);
		  channels = le16(bytes + payload + 2);
		  rate = le32(bytes + payload + 4);
		  bits = le16(bytes + payload + 14);
	  } else if (!std::memcmp(bytes + offset, "data", 4)) {
		  pcm = bytes + payload;
		  pcmBytes = size;
	  }
	  offset = payload + size + (size & 1u);
  }
  if (!pcm || format != 1 || bits != 16 || channels < 1 || channels > 2 || !rate || pcmBytes == 0 ||
	  pcmBytes % (channels * 2)) {
	  return false;
  }
  ExclusivePlaybackScope playbackScope;
  if (!audio::OutputDriver::open(static_cast<int>(rate), 2, 16)) {
	  return false;
  }
  OutputCloseScope closeOutput{true};
  std::vector<std::int16_t> stereo(kPlaybackChunkFrames * 2);
  const auto frameCount = pcmBytes / (channels * 2);
  for (std::size_t frame = 0; frame < frameCount;) {
	  const auto count = std::min<std::size_t>(kPlaybackChunkFrames, frameCount - frame);
	  for (std::size_t index = 0; index < count; ++index) {
		  const auto *source = pcm + (frame + index) * channels * 2;
		  stereo[index * 2] = static_cast<std::int16_t>(le16(source));
		  stereo[index * 2 + 1] =
			  channels == 1 ? stereo[index * 2] : static_cast<std::int16_t>(le16(source + 2));
	  }
	  if (!audio::OutputDriver::write(stereo.data(), count * 2, kPlaybackWriteTimeoutMs)) {
		  return false;
	  }
	  frame += count;
  }
  return true;
}

// Stream a 16-bit PCM WAV from disk to the output driver in bounded chunks.
// Playback memory stays ~constant (one small chunk buffer) regardless of file
// length. The previous path loaded the ENTIRE file into a std::vector first,
// which OOM-crashed on long recordings — a 20-minute 16 kHz mono clip is
// ~38 MB, far past available PSRAM. Parses the header (must be PCM 16-bit),
// then reads+plays the data chunk incrementally.
bool streamPcm16File(const std::string &path) {
	if (path.empty() || !gea::platform::storage::ensureMounted()) {
		return false;
	}
	FILE *file = std::fopen(path.c_str(), "rb");
	if (!file) {
		return false;
	}

	bool ok = false;
	do {
		if (!readTag(file, "RIFF")) {
			break;
		}
		(void)readLe32(file);
		if (!readTag(file, "WAVE")) {
			break;
		}

		std::uint16_t audioFormat = 0;
		std::uint16_t channels = 0;
		std::uint32_t sampleRate = 0;
		std::uint16_t bitsPerSample = 0;
		long dataStart = -1;
		std::uint32_t dataSize = 0;

		while (!std::feof(file)) {
			char chunkId[4] = {};
			if (std::fread(chunkId, 1, sizeof(chunkId), file) != sizeof(chunkId))
				break;
			const std::uint32_t chunkSize = readLe32(file);
			const long payloadStart = std::ftell(file);
			if (payloadStart < 0)
				break;
			if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
				audioFormat = readLe16(file);
				channels = readLe16(file);
				sampleRate = readLe32(file);
				(void)readLe32(file);
				(void)readLe16(file);
				bitsPerSample = readLe16(file);
			} else if (std::memcmp(chunkId, "data", 4) == 0 && chunkSize >= 2) {
				dataStart = payloadStart;
				dataSize = chunkSize;
				break; // stream the data chunk below rather than loading it
			}
			const long next =
				payloadStart + static_cast<long>(chunkSize) + static_cast<long>(chunkSize & 1u);
			if (std::fseek(file, next, SEEK_SET) != 0)
				break;
		}

		if (audioFormat != 1 || bitsPerSample != 16 || dataStart < 0 || dataSize < 2)
			break;
		if (channels == 0)
			channels = 1;
		const int outSampleRate = sampleRate == 0 ? kSampleRate : static_cast<int>(sampleRate);
		if (std::fseek(file, dataStart, SEEK_SET) != 0)
			break;

		ExclusivePlaybackScope playbackScope;
		if (!audio::OutputDriver::open(outSampleRate, 2, 16))
			break;
		OutputCloseScope closeOutput{true};

		const std::size_t ch = static_cast<std::size_t>(channels);
		std::size_t framesLeft = dataSize / (sizeof(std::int16_t) * ch);
		std::vector<std::int16_t> in(kPlaybackChunkFrames * ch);
		std::vector<std::int16_t> stereo(kPlaybackChunkFrames * 2);
		bool wrote = true;
		while (framesLeft > 0) {
			const std::size_t frames = std::min<std::size_t>(kPlaybackChunkFrames, framesLeft);
			const std::size_t want = frames * ch;
			// WAV PCM is little-endian and the ESP32 is little-endian, so raw int16
			// reads need no byte swap.
			const std::size_t got = std::fread(in.data(), sizeof(std::int16_t), want, file);
			const std::size_t framesGot = got / ch;
			if (framesGot == 0)
				break;
			if (channels == 2) {
				if (!audio::OutputDriver::write(in.data(), framesGot * 2,
												kPlaybackWriteTimeoutMs)) {
					wrote = false;
					break;
				}
			} else {
				for (std::size_t i = 0; i < framesGot; ++i) {
					const std::size_t src = i * ch;
					stereo[i * 2] = in[src];
					stereo[i * 2 + 1] = channels == 1 ? in[src] : in[src + 1];
				}
				if (!audio::OutputDriver::write(stereo.data(), framesGot * 2,
												kPlaybackWriteTimeoutMs)) {
					wrote = false;
					break;
				}
			}
			framesLeft -= framesGot;
			if (got < want)
				break; // short read = EOF
		}
		ok = wrote;
	} while (false);

	std::fclose(file);
	return ok;
}

}  // namespace

audio::AudioParam::AudioParam(audio::NativeAudioHandle oscillator) : oscillator_(oscillator) {}

double audio::AudioParam::value() const {
  return AudioEngine::instance().oscillatorFrequency(oscillator_);
}

void audio::AudioParam::setValue(double value) {
  AudioEngine::instance().setOscillatorFrequency(oscillator_, value);
}

void audio::AudioParam::setValueAtTime(double value, double startTime) {
  AudioEngine::instance().setOscillatorFrequencyAtTime(oscillator_, value, startTime);
}

audio::AudioNode::AudioNode(audio::NativeAudioHandle native) : native_(native) {}

audio::NativeAudioHandle audio::AudioNode::nativeId() const {
  return native_;
}

audio::AudioDestinationNode::AudioDestinationNode(audio::NativeAudioHandle native) : AudioNode(native) {}

audio::OscillatorNode::OscillatorNode(audio::NativeAudioHandle native) : AudioNode(native), frequency(native) {}

audio::OscillatorType audio::OscillatorNode::type() const {
  return AudioEngine::instance().oscillatorType(nativeId());
}

void audio::OscillatorNode::setType(audio::OscillatorType type) {
  AudioEngine::instance().setOscillatorType(nativeId(), type);
}

void audio::OscillatorNode::connect(const audio::AudioDestinationNode &destination) {
  AudioEngine::instance().connectOscillator(nativeId(), destination.nativeId());
}

void audio::OscillatorNode::start(double when) {
  AudioEngine::instance().startOscillator(nativeId(), when);
}

void audio::OscillatorNode::stop(double when) {
  AudioEngine::instance().stopOscillator(nativeId(), when);
}

double audio::AudioContext::currentTime() const {
  return AudioEngine::instance().currentTime();
}

audio::AudioDestinationNode audio::AudioContext::destination() const {
  return audio::AudioDestinationNode(AudioEngine::instance().destination());
}

audio::OscillatorNode audio::AudioContext::createOscillator() const {
  return audio::OscillatorNode(AudioEngine::instance().createOscillator());
}

// Drivers without a latency estimate retain the standard unknown value, zero.
__attribute__((weak)) double audio::OutputDriver::outputLatency(int) { return 0; }
__attribute__((weak)) bool audio::OutputDriver::flush() { return false; }
double audio::AudioSystem::processingLatency() { return double(kChunkSamples) / kSampleRate; }
double audio::AudioSystem::outputLatency() { return audio::OutputDriver::outputLatency(kSampleRate); }

audio::AudioContext audio::AudioSystem::sharedContext() {
  return audio::AudioContext{};
}

int audio::AudioSystem::volume() {
  return AudioEngine::instance().volume();
}

void audio::AudioSystem::setVolume(int volumePercent) {
  AudioEngine::instance().setVolume(volumePercent);
}

bool audio::AudioSystem::playFile(const std::string &path) {
  // Stream from disk in bounded chunks — never load the whole file (a long
  // recording is tens of MB and would exhaust PSRAM).
  const unsigned char* embedded = nullptr;
  unsigned long embeddedBytes = 0;
  if (gea_embedded_asset_lookup &&
      gea_embedded_asset_lookup(path.c_str(), &embedded, &embeddedBytes)) {
    return playEmbeddedWav(embedded, embeddedBytes);
  }
  if (!streamPcm16File(path)) {
    ESP_LOGW("gea_audio", "Unable to play WAV file: %s", path.c_str());
    return false;
  }
  return true;
}

bool audio::AudioSystem::playPcm(const std::int16_t *samples, std::size_t sampleCount, int sampleRate, int channels) {
  return playPcmSync(samples, sampleCount, sampleRate, channels);
}

void audio::AudioSystem::stopPlayback() {
  AudioEngine::instance().stopPlayback();
}
void audio::AudioSystem::flushPlayback() {
  AudioEngine::instance().flushPlayback();
}

std::uint64_t audio::AudioSystem::playPcmStream(audio::PcmStreamMixer::Pull pull) {
  return AudioEngine::instance().playPcmStream(std::move(pull));
}
void audio::AudioSystem::stopPcmStream(std::uint64_t stream) {
  AudioEngine::instance().stopPcmStream(stream);
}
bool audio::AudioSystem::pcmStreamSettled(std::uint64_t stream) {
  return AudioEngine::instance().pcmStreamSettled(stream);
}
