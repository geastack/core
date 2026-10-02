// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/h264_layout.h"
#include "host/media.h"
#include "host/video_frame_pool.h"
#include "host/video_color.h"
#include "pixel.h"
#include "esp_h264_dec_sw.h"
#include "esp_timer.h"
#include <memory>
#include <limits>

namespace gea::host::rtc::h264 {

// Owned by the decode worker, never by the RTP/audio callback thread.
class Decoder {
 public:
  Decoder() = default;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  ~Decoder() { reset(); }

  int64_t lastDecodeUs() const { return decodeUs_; }
  int64_t lastConvertUs() const { return convertUs_; }
  bool simdAligned() const { return simdAligned_; }

  void reset() {
    if (decoder_) { esp_h264_dec_close(decoder_); esp_h264_dec_del(decoder_); }
    decoder_ = nullptr;
    parameters_ = nullptr;
    metadata_ = {};
  }

  std::shared_ptr<media::VideoFrame> decode(const uint8_t* bytes, size_t length, uint32_t timestamp) {
    decodeUs_ = convertUs_ = 0;
    if (!length || length > std::numeric_limits<uint32_t>::max() ||
        !inspectAccessUnit(bytes, length, metadata_) || !open()) {
      reset(); return nullptr;
    }
    esp_h264_dec_in_frame_t input{};
    input.raw_data.buffer = const_cast<uint8_t*>(bytes);
    input.raw_data.len = static_cast<uint32_t>(length);
    std::shared_ptr<media::VideoFrame> latest;
    while (input.raw_data.len) {
      input.consume = 0;
      esp_h264_dec_out_frame_t decoded{};
      const auto decodeStarted = esp_timer_get_time();
      const auto rc = esp_h264_dec_process(decoder_, &input, &decoded);
      decodeUs_ += esp_timer_get_time() - decodeStarted;
      if (rc != ESP_H264_ERR_OK || !input.consume || input.consume > input.raw_data.len) {
        reset(); return nullptr;
      }
      if (decoded.out_size) {
        esp_h264_resolution_t resolution{};
        const auto expected = size_t(metadata_.width) * metadata_.height * 3 / 2;
        if (esp_h264_dec_get_resolution(parameters_, &resolution) != ESP_H264_ERR_OK || !decoded.outbuf ||
            resolution.width != metadata_.width || resolution.height != metadata_.height || decoded.out_size != expected) {
          reset(); return nullptr;
        }
        latest = frames_.acquire(metadata_.visibleWidth, metadata_.visibleHeight);
        if (latest) {
          const auto convertStarted = esp_timer_get_time();
          simdAligned_ = !(uintptr_t(decoded.outbuf) & 7) &&
              !(uintptr_t(latest->rgb565.data()) & 7) && !(metadata_.width & 7) &&
              !(metadata_.visibleWidth & 3) && !metadata_.top && !metadata_.left &&
              !metadata_.fullRange && !metadata_.bt709;
          latest->timestampMs = timestamp;
          latest->panelEndian = bool(GEA_EMBEDDED_PIXEL_PANEL_ENDIAN);
          if (!metadata_.fullRange && !metadata_.bt709) {
            // The server emits limited-range BT.601. Reuse the S3 PIE kernel
            // directly into the pooled panel buffer, without scalar conversion.
            const auto* u = decoded.outbuf + metadata_.width * metadata_.height;
            const auto* v = u + metadata_.width * metadata_.height / 4;
            media::i420ToRgb565(decoded.outbuf + metadata_.top * metadata_.width + metadata_.left,
                metadata_.width, u + metadata_.top / 2 * (metadata_.width / 2) + metadata_.left / 2,
                metadata_.width / 2, v + metadata_.top / 2 * (metadata_.width / 2) + metadata_.left / 2,
                metadata_.width / 2, latest->width, latest->height, latest->rgb565.data(), latest->panelEndian);
          } else {
            convertI420(decoded.outbuf, metadata_, latest->rgb565.data());
            if (latest->panelEndian) media::rgb565SwapBytes(latest->rgb565.data(), latest->rgb565.size());
          }
          convertUs_ += esp_timer_get_time() - convertStarted;
        }
      }
      input.raw_data.buffer += input.consume;
      input.raw_data.len -= input.consume;
    }
    return latest;
  }

 private:
  bool open() {
    if (decoder_) return true;
    esp_h264_dec_cfg_sw_t config{};
    config.pic_type = ESP_H264_RAW_FMT_I420;
    auto rc = esp_h264_dec_sw_new(&config, &decoder_);
    if (rc == ESP_H264_ERR_OK) rc = esp_h264_dec_open(decoder_);
    if (rc == ESP_H264_ERR_OK) rc = esp_h264_dec_sw_get_param_hd(decoder_, &parameters_);
    return rc == ESP_H264_ERR_OK;
  }
  esp_h264_dec_handle_t decoder_ = nullptr;
  esp_h264_dec_param_sw_handle_t parameters_ = nullptr;
  Metadata metadata_;
  media::VideoFramePool frames_;
  int64_t decodeUs_ = 0, convertUs_ = 0;
  bool simdAligned_ = false;
};
} // namespace gea::host::rtc::h264
