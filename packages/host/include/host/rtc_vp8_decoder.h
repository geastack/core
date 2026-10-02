// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/media.h"
#include "host/rtc_vp8.h"
#include "host/video_color.h"
#include "host/video_frame_pool.h"
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <algorithm>
#include <memory>
#include <chrono>
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "vp8_s3.h"
#include "vp8_profile_s3.h"
#endif

namespace gea::host::rtc::vp8 {

// Decode every reference frame on one worker; only decoded display frames may
// be coalesced. Dropping encoded interframes would corrupt following pictures.
class Decoder {
 public:
  static constexpr size_t maxPixels = 512 * 768;
  Decoder() {
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
    // Keep only the hot SIMD intermediates internal. Reference planes and the
    // decoder stack remain external; this bounded allocation is 672 bytes.
    workspace_ = heap_caps_aligned_alloc(16, GEA_VP8_WORKSPACE_BYTES,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
  }
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  ~Decoder() {
    reset();
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
    heap_caps_free(workspace_);
#endif
  }

  void reset() {
    if (open_) vpx_codec_destroy(&codec_);
    codec_ = {};
    open_ = false;
    frames_.reset();
  }

  bool needsKeyframe() const { return !open_; }
  struct Timing { uint64_t setupUs = 0, decodeUs = 0, allocateUs = 0, convertUs = 0;
    uint64_t tokenUs = 0, macroblockUs = 0, filterUs = 0;
    unsigned macroblocks = 0, tokenBlocks = 0, filterRows = 0;
    unsigned allocationFailedWidth = 0, allocationFailedHeight = 0; };
  Timing timing() const { return timing_; }
  size_t internalWorkspaceBytes() const {
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
    return workspace_ ? GEA_VP8_WORKSPACE_BYTES : 0;
#else
    return 0;
#endif
  }

  std::shared_ptr<media::VideoFrame> decode(std::span<const uint8_t> bytes, uint32_t timestampMs,
                                          bool present = true, uint64_t presentationDelayUs = 0) {
    timing_ = {};
    const auto started = nowUs();
    if (bytes.empty() || bytes.size() > Assembler::maxFrameBytes) return fail();
    if (!(bytes[0] & 1)) {
      vpx_codec_stream_info_t info{};
      info.sz = sizeof(info);
      if (vpx_codec_peek_stream_info(vpx_codec_vp8_dx(), bytes.data(),
                                     static_cast<unsigned>(bytes.size()), &info) != VPX_CODEC_OK ||
          !info.is_kf || !validSize(info.w, info.h)) return fail();
      if (!open_) {
        vpx_codec_dec_cfg_t config{};
        config.threads = 1;
        config.w = info.w;
        config.h = info.h;
        if (vpx_codec_dec_init(&codec_, vpx_codec_vp8_dx(), &config, 0) != VPX_CODEC_OK) return fail();
        open_ = true;
      }
    } else if (!open_) return nullptr;

    const auto decodeStarted = nowUs();
    timing_.setupUs = decodeStarted - started;
    vpx_codec_err_t result;
    {
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
      struct WorkspaceScope {
        void* previous;
        ~WorkspaceScope() { gea_vp8_bind_workspace_s3(previous); }
      } workspaceScope{gea_vp8_bind_workspace_s3(workspace_)};
      struct ProfileScope {
        gea_vp8_profile_s3 data{};
        gea_vp8_profile_s3* previous;
        ProfileScope() : previous(gea_vp8_profile_bind_s3(
            gea_vp8_profile_requested_s3() ? &data : nullptr)) {}
        ~ProfileScope() { gea_vp8_profile_bind_s3(previous); }
      } profileScope;
#endif
      result = vpx_codec_decode(&codec_, bytes.data(), static_cast<unsigned>(bytes.size()), nullptr, 0);
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
      timing_.tokenUs = profileScope.data.us[GEA_VP8_PROFILE_TOKENS];
      timing_.macroblockUs = profileScope.data.us[GEA_VP8_PROFILE_MACROBLOCK];
      timing_.filterUs = profileScope.data.us[GEA_VP8_PROFILE_LOOPFILTER];
      timing_.macroblocks = profileScope.data.calls[GEA_VP8_PROFILE_MACROBLOCK];
      timing_.tokenBlocks = profileScope.data.calls[GEA_VP8_PROFILE_TOKENS];
      timing_.filterRows = profileScope.data.calls[GEA_VP8_PROFILE_LOOPFILTER];
#endif
    }
    const auto convertStarted = nowUs();
    timing_.decodeUs = convertStarted - decodeStarted;
    if (result != VPX_CODEC_OK)
      return fail();
    std::shared_ptr<media::VideoFrame> latest;
    vpx_codec_iter_t iterator = nullptr;
    while (const auto* image = vpx_codec_get_frame(&codec_, &iterator)) {
      if (image->fmt != VPX_IMG_FMT_I420 || !validSize(image->d_w, image->d_h)) return fail();
      // Preserve every encoded reference, but avoid allocating/converting a
      // display image already superseded by a newer queued picture.
      // Rate limiting is evaluated after the expensive codec work. Decoding
      // can itself consume the remaining interval; deciding before it hides
      // every other frame when the decoder is slower than the display limit.
      if (!present || (presentationDelayUs && nowUs() - started < presentationDelayUs)) continue;
      const auto allocateStarted = nowUs();
      latest = frames_.acquire(image->d_w, image->d_h);
      if (!latest) {
        timing_.allocationFailedWidth = image->d_w;
        timing_.allocationFailedHeight = image->d_h;
        latest.reset();
        // The encoded picture decoded successfully. Retain its codec references;
        // memory pressure is not a bitstream error and must not force a keyframe.
        continue;
      }
      timing_.allocateUs += nowUs() - allocateStarted;
      latest->timestampMs = timestampMs;
#if defined(GEA_EMBEDDED_PIXEL_PANEL_ENDIAN) && GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
      latest->panelEndian = true;
#endif
      media::i420ToRgb565(image->planes[0], image->stride[0],
                         image->planes[1], image->stride[1],
                         image->planes[2], image->stride[2],
                         image->d_w, image->d_h, latest->rgb565.data(), latest->panelEndian);
    }
    timing_.convertUs = nowUs() - convertStarted;
    return latest;
  }

 private:
  static uint64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  static bool validSize(unsigned width, unsigned height) {
    return width && height && width <= 1024 && height <= 1024 && size_t(width) * height <= maxPixels;
  }

  std::shared_ptr<media::VideoFrame> fail() { reset(); return nullptr; }
  vpx_codec_ctx_t codec_{};
  bool open_ = false;
  Timing timing_;
  media::VideoFramePool frames_;
#if defined(GEA_VIDEO_SIMD_ESP32S3) && GEA_VIDEO_SIMD_ESP32S3 && defined(ESP_PLATFORM)
  void* workspace_ = nullptr;
#endif
};

} // namespace gea::host::rtc::vp8
