// SPDX-License-Identifier: Apache-2.0
#pragma once
#if defined(ESP_PLATFORM) && __has_include("esp_opus_enc.h")
#define GEA_RTC_ESP_OPUS 1
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#else
#include <opus.h>
#endif
#include <array>
#include <cstdint>
#include <stdexcept>

namespace gea::host::rtc {
// esp_peer transports encoded RTP payloads. The media-track boundary is PCM.
// Decode at the track's 16 kHz rate; Opus's RTP clock remains 48 kHz.
class OpusAudio {
 public:
  // Amortize SRTP/socket work across two 20 ms Opus frames. At 50 sends/sec
  // the S3 spent ~11 ms encoding plus ~10 ms sending each 20 ms of capture,
  // accumulating old microphone audio. This adds only 20 ms packetization.
  static constexpr int sampleRate = 16000, frameDurationMs = 40;
  static constexpr int frameSamples = sampleRate * frameDurationMs / 1000;
  static constexpr int maxDecodeSamples = sampleRate * 120 / 1000;
  enum class Application { LowDelay, Voice };
  enum class Direction { Both, Encode, Decode };
  explicit OpusAudio(Application application = Application::LowDelay,
                     Direction direction = Direction::Both) {
#if GEA_RTC_ESP_OPUS
    if (direction != Direction::Decode) {
      esp_opus_enc_config_t config{};
      config.sample_rate = sampleRate;
      config.channel = 1;
      config.bits_per_sample = 16;
      config.bitrate = 24000;
      config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_40_MS;
      // CELT-only low-delay mode avoids SILK's analysis work competing with
      // AEC on the S3. The RTP codec, PCM rate and 24 kbps bitrate are unchanged.
      config.application_mode = application == Application::LowDelay
          ? ESP_OPUS_ENC_APPLICATION_LOWDELAY : ESP_OPUS_ENC_APPLICATION_VOIP;
      config.complexity = 0;
      config.enable_vbr = true;
      if (esp_opus_enc_open(&config, sizeof(config), &encoder) != ESP_AUDIO_ERR_OK)
        throw std::runtime_error("RTC Opus encoder allocation failed");
    }
    if (direction != Direction::Encode) {
      esp_opus_dec_cfg_t receive{};
      receive.sample_rate = sampleRate;
      receive.channel = 1;
      receive.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
      receive.self_delimited = false;
      if (esp_opus_dec_open(&receive, sizeof(receive), &decoder) != ESP_AUDIO_ERR_OK) {
        if (encoder) esp_opus_enc_close(encoder);
        throw std::runtime_error("RTC Opus decoder allocation failed");
      }
    }
#else
    int error;
    if (direction != Direction::Decode) {
      encoder = opus_encoder_create(sampleRate, 1, application == Application::LowDelay
          ? OPUS_APPLICATION_RESTRICTED_LOWDELAY : OPUS_APPLICATION_VOIP, &error);
      if (!encoder) throw std::runtime_error("RTC Opus encoder allocation failed");
    }
    if (direction != Direction::Encode) {
      decoder = opus_decoder_create(sampleRate, 1, &error);
      if (!decoder) {
        if (encoder) opus_encoder_destroy(encoder);
        throw std::runtime_error("RTC Opus decoder allocation failed");
      }
    }
    if (encoder) {
      opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
      // On S3 the complexity-3 encoder missed its 20 ms deadline alongside
      // incoming RTP. Keep the 24 kbps wire format, with real-time CPU cost.
      opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
      opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
    }
#endif
  }
  ~OpusAudio() {
#if GEA_RTC_ESP_OPUS
    if (encoder) esp_opus_enc_close(encoder);
    if (decoder) esp_opus_dec_close(decoder);
#else
    if (encoder) opus_encoder_destroy(encoder);
    if (decoder) opus_decoder_destroy(decoder);
#endif
  }
  OpusAudio(const OpusAudio&) = delete;
  OpusAudio& operator=(const OpusAudio&) = delete;
  int encode(const int16_t* pcm) {
    if (!encoder || !pcm) return -1;
#if GEA_RTC_ESP_OPUS
    esp_audio_enc_in_frame_t input{};
    input.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
    input.len = frameSamples * sizeof(int16_t);
    esp_audio_enc_out_frame_t output{};
    output.buffer = packet.data();
    output.len = packet.size();
    return esp_opus_enc_process(encoder, &input, &output) == ESP_AUDIO_ERR_OK
        ? int(output.encoded_bytes) : -1;
#else
    return opus_encode(encoder, pcm, frameSamples, packet.data(), packet.size());
#endif
  }
  int decode(const uint8_t* bytes, int size) {
    if (!decoder || !bytes || size <= 0) return -1;
#if GEA_RTC_ESP_OPUS
    esp_audio_dec_in_raw_t input{};
    input.buffer = const_cast<uint8_t*>(bytes);
    input.len = size;
    esp_audio_dec_out_frame_t output{};
    output.buffer = reinterpret_cast<uint8_t*>(decoded.data());
    output.len = sizeof(decoded);
    esp_audio_dec_info_t info{};
    const auto status = esp_opus_dec_decode(decoder, &input, &output, &info);
    if (status != ESP_AUDIO_ERR_OK || input.consumed != unsigned(size) ||
        output.decoded_size > sizeof(decoded) || info.channel != 1 || info.sample_rate != sampleRate)
      return -1;
    return int(output.decoded_size / sizeof(int16_t));
#else
    return opus_decode(decoder, bytes, size, decoded.data(), decoded.size(), 0);
#endif
  }
  alignas(16) std::array<uint8_t, 1275> packet{};
  alignas(16) std::array<int16_t, maxDecodeSamples> decoded{};
 private:
#if GEA_RTC_ESP_OPUS
  void* encoder = nullptr;
  void* decoder = nullptr;
#else
  OpusEncoder* encoder = nullptr;
  OpusDecoder* decoder = nullptr;
#endif
};
}
