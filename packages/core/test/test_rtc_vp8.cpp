// SPDX-License-Identifier: Apache-2.0
#include "host/rtc_vp8_decoder.h"
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include <cassert>
#include <iostream>

using namespace gea::host::rtc::vp8;

int main() {
  // Renderer-held immutable pictures must survive replacement, pool exhaustion,
  // resolution changes and decoder teardown. No leased buffer may be reused.
  gea::host::media::VideoFramePool pixels;
  assert(!pixels.acquire(0, 4));
  auto firstPicture = pixels.acquire(4, 2);
  std::weak_ptr<const gea::host::media::VideoFrame> firstLifetime = firstPicture;
  firstPicture->rgb565.assign(8, 11);
  auto* firstPixels = firstPicture->rgb565.data();
  std::shared_ptr<const gea::host::media::VideoFrame> retained = firstPicture;
  firstPicture.reset();
  auto secondPicture = pixels.acquire(4, 2);
  auto thirdPicture = pixels.acquire(4, 2);
  assert(secondPicture && thirdPicture && !pixels.acquire(4, 2));
  assert(secondPicture->rgb565.data() != firstPixels);
  assert(thirdPicture->rgb565.data() != secondPicture->rgb565.data());
  secondPicture->rgb565.assign(8, 22);
  thirdPicture->rgb565.assign(8, 33);
  assert(retained->rgb565.front() == 11);
  retained.reset();
  assert(firstLifetime.expired());
  auto reusedPicture = pixels.acquire(4, 2);
  assert(reusedPicture && reusedPicture->rgb565.data() == firstPixels);
  reusedPicture->rgb565.assign(8, 44);
  assert(secondPicture->rgb565.front() == 22 && thirdPicture->rgb565.front() == 33);
  reusedPicture.reset();
  auto resizedPicture = pixels.acquire(7, 3);
  assert(resizedPicture && resizedPicture->rgb565.size() == 21);
  assert(secondPicture->width == 4 && secondPicture->rgb565.front() == 22);
  pixels.reset();
  assert(thirdPicture->rgb565.front() == 33 && resizedPicture->rgb565.size() == 21);
  assert(pixels.acquire(4, 2));
  auto independentCopy = secondPicture->rgb565;
  independentCopy.front() = 55;
  assert(secondPicture->rgb565.front() == 22);
  // Growing a lease detaches it; it must not free the pool's retained storage.
  secondPicture->rgb565.resize(64);
  assert(secondPicture->rgb565.front() == 22 && independentCopy.front() == 55);

  const std::vector<uint8_t> extensions{0xd8, 0xf0, 0x81, 0x23, 0x11, 0x60, 0x00};
  // Reserved descriptor bits are ignored; both T and K use the last byte.
  const auto parsed = parsePayload(extensions);
  assert(parsed && parsed->startsFrame && parsed->bytes.size() == 1);
  for (size_t length = 0; length < extensions.size(); ++length)
    assert(!parsePayload(std::span(extensions).first(length)));
  const uint8_t partition[]{0x11, 0x00};
  assert(!parsePayload(partition)->startsFrame);

  Assembler assembler;
  const uint8_t first[]{0x10, 0x00, 0x01};
  const uint8_t tail[]{0x00, 0x02, 0x03};
  assert(!assembler.push(65535, 9000, false, first));
  assert(!assembler.push(65535, 9000, false, first)); // duplicate
  auto frame = assembler.push(0, 9000, true, tail);
  assert(frame && frame->keyframe && frame->timestamp == 9000);
  assert((frame->bytes == EncodedFrame::Bytes{0, 1, 2, 3}));
  const auto* allocation = frame->bytes.data();
  auto workerBuffer = std::move(frame->bytes);
  assert(workerBuffer.data() == allocation && frame->bytes.empty());
  const uint8_t predicted[]{0x10, 0x01};
  assert(!assembler.push(1, 18000, false, predicted));
  assert(!assembler.push(3, 18000, true, tail)); // lost packet, no partial output
  assert(assembler.needsKeyframe());
  assert(!assembler.push(4, 27000, true, predicted));
  assert(assembler.push(5, 36000, true, first));
  assert(!assembler.needsKeyframe());
  assert(!assembler.push(6, 45000, false, predicted));
  assert(!assembler.push(7, 54000, true, predicted)); // lost previous marker
  assert(assembler.needsKeyframe());
  assert(assembler.push(8, 63000, true, first));
  std::vector<uint8_t> oversized(Assembler::maxFrameBytes + 2, 0);
  oversized[0] = 0x10;
  assert(!assembler.push(9, 72000, true, oversized));
  assert(assembler.needsKeyframe());
  assert(assembler.push(10, 81000, true, first));
  assert(!assembler.push(11, 90000, true, tail)); // missing frame-start flag
  assert(assembler.needsKeyframe());
  assert(!assembler.push(12, 99000, true, predicted));

  // A new partition is not a new frame, and timestamps cannot be mixed.
  assert(!assembler.push(13, 108000, false, first));
  frame = assembler.push(14, 108000, true, partition);
  assert(frame && frame->bytes.size() == 3);
  assert(!assembler.push(15, 117000, false, predicted));
  assert(!assembler.push(16, 126000, true, tail));
  assert(assembler.needsKeyframe());

  // Real independently encoded VP8, including predicted frames. This also
  // checks padded decoder strides, a non-macroblock-aligned visible height,
  // fragmentation, RGB565 conversion and preserved output timestamps.
  vpx_codec_enc_cfg_t config{};
  assert(vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &config, 0) == VPX_CODEC_OK);
  config.g_w = 32;
  config.g_h = 18;
  config.g_timebase = {1, 30};
  config.g_threads = 1;
  config.g_lag_in_frames = 0;
  config.rc_target_bitrate = 500;
  config.kf_mode = VPX_KF_DISABLED;
  vpx_codec_ctx_t encoder{};
  assert(vpx_codec_enc_init(&encoder, vpx_codec_vp8_cx(), &config, 0) == VPX_CODEC_OK);
  auto* input = vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, config.g_w, config.g_h, 1);
  assert(input);
  Decoder decoder;
  Decoder coalesced;
  Decoder rateLimited;
  Decoder latestKeyframe;
  Decoder slowSink;
  std::vector<std::shared_ptr<gea::host::media::VideoFrame>> heldPictures;
  assembler.reset();
  uint16_t sequence = 0;
  std::vector<uint8_t> keyframe;
  for (unsigned index = 0; index < 5; ++index) {
    const uint8_t luminance = index == 1 ? 235 : index >= 2 ? 81 : 16;
    for (unsigned y = 0; y < config.g_h; ++y)
      std::fill_n(input->planes[0] + y * input->stride[0], config.g_w, luminance);
    for (unsigned plane = 1; plane < 3; ++plane)
      for (unsigned y = 0; y < config.g_h / 2; ++y)
        std::fill_n(input->planes[plane] + y * input->stride[plane], config.g_w / 2,
                    index >= 2 ? (plane == 1 ? 90 : 240) : 128);
    const bool forceKeyframe = index == 0 || index == 3;
    assert(vpx_codec_encode(&encoder, input, index, 1, forceKeyframe ? VPX_EFLAG_FORCE_KF : 0,
                            VPX_DL_REALTIME) == VPX_CODEC_OK);
    vpx_codec_iter_t iterator = nullptr;
    bool produced = false;
    while (const auto* packet = vpx_codec_get_cx_data(&encoder, &iterator)) {
      if (packet->kind != VPX_CODEC_CX_FRAME_PKT) continue;
      produced = true;
      const auto* bytes = static_cast<const uint8_t*>(packet->data.frame.buf);
      const size_t size = packet->data.frame.sz;
      assert(bool(bytes[0] & 1) == !forceKeyframe);
      if (!index) keyframe.assign(bytes, bytes + size);
      std::optional<EncodedFrame> assembled;
      for (size_t offset = 0; offset < size;) {
        const auto count = std::min(size - offset, size_t(7));
        std::vector<uint8_t> payload{uint8_t(offset ? 0 : 0x10)};
        payload.insert(payload.end(), bytes + offset, bytes + offset + count);
        offset += count;
        assembled = assembler.push(sequence++, index * 3000, offset == size, payload);
        assert(bool(assembled) == (offset == size));
      }
      assert(assembled && assembled->bytes.size() == size);
      auto decoded = decoder.decode(assembled->bytes, 1000 + index * 33);
      if (index == 4) heldPictures.erase(heldPictures.begin());
      auto held = slowSink.decode(assembled->bytes, 1000 + index * 33);
      if (index == 3) {
        // All three pixel buffers are leased, but the codec must still accept
        // the reference. The following interframe can resume without a PLI.
        assert(!held && !slowSink.needsKeyframe());
      } else {
        assert(held && held->rgb565 == decoded->rgb565);
        heldPictures.push_back(std::move(held));
      }
      if (index >= 1 && index < 4) assert(heldPictures.front()->rgb565.front() == 0);
      // A new keyframe is independently decodable even after discarding all
      // intervening encoded references from an older queued chain.
      if (forceKeyframe) {
        auto newest = latestKeyframe.decode(assembled->bytes, 1000 + index * 33);
        assert(newest && newest->rgb565 == decoded->rgb565);
      }
      auto display = coalesced.decode(assembled->bytes, 1000 + index * 33, index != 1);
      // A real decode consumes a short presentation interval. An interval
      // that cannot expire must still decode references without allocating;
      // the next due picture must match the unthrottled decoder exactly.
      auto limited = rateLimited.decode(assembled->bytes, 1000 + index * 33, true,
          index == 1 ? UINT64_MAX : 1);
      if (index == 1) {
        assert(!limited && !rateLimited.needsKeyframe());
        assert(rateLimited.timing().allocateUs == 0);
      } else {
        assert(limited && limited->rgb565 == decoded->rgb565);
      }
      if (index == 1) {
        assert(!display && !coalesced.needsKeyframe());
        assert(coalesced.timing().allocateUs == 0);
      } else {
        assert(display && display->rgb565 == decoded->rgb565);
      }
      assert(decoded && decoded->width == 32 && decoded->height == 18);
      assert(decoded->timestampMs == 1000 + index * 33);
#if defined(GEA_EMBEDDED_PIXEL_PANEL_ENDIAN) && GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
      assert(decoded->panelEndian);
#else
      assert(!decoded->panelEndian);
#endif
      for (auto pixel : decoded->rgb565) {
        if (decoded->panelEndian) pixel = uint16_t((pixel << 8) | (pixel >> 8));
        if (index >= 2) {
          // Red distinguishes the panel's byte order; black/white cannot.
          assert((pixel & 0xf800) >= 0xe000 && ((pixel >> 5) & 63) <= 2 && (pixel & 31) <= 2);
        } else assert(pixel == (index == 1 ? 0xffff : 0));
      }
    }
    assert(produced);
  }
  const uint8_t corrupt[]{0};
  assert(!decoder.decode(corrupt, 0) && decoder.needsKeyframe());
  const uint8_t inter[]{1, 0, 0};
  assert(!decoder.decode(inter, 0) && decoder.needsKeyframe());
  assert(decoder.decode(keyframe, 10));
  // Header claims a huge frame: reject before libvpx allocates reference planes.
  keyframe[6] = keyframe[7] = keyframe[8] = keyframe[9] = 0xff;
  assert(!decoder.decode(keyframe, 11) && decoder.needsKeyframe());
  vpx_img_free(input);
  assert(vpx_codec_destroy(&encoder) == VPX_CODEC_OK);
  std::cout << "VP8 descriptor bounds, loss recovery, reference decoding and RGB565 passed\n";
}
