// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/video_frame_pool.h"
#include "host/h264_layout.h"
#include <bit>
#include <charconv>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gea::host::video {
using JpegBytes = std::vector<uint8_t, media::VideoAllocator<uint8_t>>;
struct JpegPacket {
  JpegBytes bytes;
  uint32_t timestampMs = 0;
  bool h264 = false;
  bool h264Keyframe = false;
  bool resetDecoder = false;
  // Raw transport writes directly into the final pooled pixel allocation.
  std::shared_ptr<media::VideoFrame> rawFrame;
};

// Length-delimited multipart parsing survives arbitrary TCP fragmentation.
// Never search the JPEG body for boundaries or allocate from untrusted sizes.
class MjpegParser {
 public:
  bool push(std::span<const uint8_t> bytes, const std::function<void(JpegPacket)>& emit) {
    while (!bytes.empty()) {
      if (remaining_) {
        const auto count = std::min(remaining_, bytes.size());
        if (format_ == Format::Jpeg || format_ == Format::H264) {
          packet_.bytes.insert(packet_.bytes.end(), bytes.begin(), bytes.begin() + count);
        } else if (packet_.rawFrame) {
          std::memcpy(reinterpret_cast<uint8_t*>(packet_.rawFrame->rgb565.data()) + length_ - remaining_,
              bytes.data(), count);
        }
        bytes = bytes.subspan(count);
        remaining_ -= count;
        if (!remaining_) {
          if (format_ == Format::Jpeg) {
            const auto& jpeg = packet_.bytes;
            if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
                jpeg[jpeg.size() - 2] != 0xff || jpeg.back() != 0xd9) return false;
          } else if (format_ == Format::H264) {
            const auto& bytes = packet_.bytes;
            if (!rtc::h264::inspectAccessUnit(bytes.data(), bytes.size(), h264Metadata_) ||
                h264Metadata_.visibleWidth != int(width_) || h264Metadata_.visibleHeight != int(height_)) return false;
            unsigned prefix = 0;
            bool sps = false, pps = false, idr = false, predictive = false;
            for (size_t start = rtc::h264::startCode(bytes.data(), bytes.size(), 0, prefix);
                 start < bytes.size();) {
              const auto nal = start + prefix;
              if (nal >= bytes.size()) return false;
              const auto type = bytes[nal] & 31;
              predictive |= type == 1;
              sps |= type == 7; pps |= type == 8; idr |= type == 5;
              start = rtc::h264::startCode(bytes.data(), bytes.size(), nal, prefix);
            }
            if (idr) {
              if (!sps || !pps) return false;
              h264HasIdr_ = true;
            } else if (!h264HasIdr_ || !predictive) return false;
            packet_.h264 = true;
            packet_.h264Keyframe = idr;
          }
          // Exhausted pool means a slow sink: consume/drop this whole picture,
          // never overwrite a published buffer or grow a frame backlog.
          if (format_ == Format::Jpeg || format_ == Format::H264 || packet_.rawFrame) emit(std::move(packet_));
          packet_ = {};
          length_ = width_ = height_ = 0;
          format_ = Format::None;
          inHeaders_ = false;
        }
        continue;
      }
      const auto byte = bytes.front();
      bytes = bytes.subspan(1);
      if (line_.size() >= 1024 || ++headerBytes_ > 2048) return false;
      line_ += char(byte);
      if (byte != '\n') continue;
      if (line_.size() < 2 || line_[line_.size() - 2] != '\r') return false;
      line_.resize(line_.size() - 2);
      if (line_.empty()) {
        if (inHeaders_) {
          if (!length_ || format_ == Format::None) return false;
          if (format_ == Format::Jpeg || format_ == Format::H264) {
            if (length_ > (format_ == Format::Jpeg ? 65536 : 262144)) return false;
            if (format_ == Format::H264 && (!width_ || !height_)) return false;
            packet_.bytes.reserve(length_);
          } else {
            if (!width_ || !height_ || length_ != size_t(width_) * height_ * 2) return false;
            packet_.rawFrame = frames_.acquire(width_, height_);
            if (packet_.rawFrame) {
              packet_.rawFrame->timestampMs = packet_.timestampMs;
              packet_.rawFrame->panelEndian = (format_ == Format::Rgb565Be) !=
                  (std::endian::native == std::endian::big);
            }
          }
          remaining_ = length_;
          headerBytes_ = 0;
        }
      } else if (!inHeaders_) {
        if (boundary_.empty()) {
          if (!line_.starts_with("--") || line_.size() < 3 || line_.size() > 72) return false;
          boundary_ = line_;
        }
        if (line_ != boundary_) return false;
        inHeaders_ = true;
      } else {
        const auto colon = line_.find(':');
        if (colon == line_.npos) return false;
        auto name = line_.substr(0, colon);
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        auto value = std::string_view(line_).substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (name == "content-length" || name == "x-timestamp-ms" || name == "x-width" || name == "x-height") {
          uint32_t number = 0;
          const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
          if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
          if (name == "content-length") {
            if (length_ || number < 4 || number > 512 * 512 * 2) return false;
            length_ = number;
          } else if (name == "x-width" || name == "x-height") {
            auto& dimension = name == "x-width" ? width_ : height_;
            if (dimension || !number || number > 512) return false;
            dimension = number;
          } else packet_.timestampMs = number;
        } else if (name == "content-type") {
          if (format_ != Format::None) return false;
          if (value == "image/jpeg") format_ = Format::Jpeg;
          else if (value == "video/h264") format_ = Format::H264;
          else if (value == "application/x-rgb565-be") format_ = Format::Rgb565Be;
          else if (value == "application/x-rgb565-le") format_ = Format::Rgb565Le;
          else return false;
        }
      }
      line_.clear();
    }
    return true;
  }
 private:
  enum class Format { None, Jpeg, H264, Rgb565Le, Rgb565Be };
  std::string line_, boundary_;
  JpegPacket packet_;
  media::VideoFramePool frames_;
  rtc::h264::Metadata h264Metadata_;
  bool h264HasIdr_ = false;
  uint32_t width_ = 0, height_ = 0;
  size_t remaining_ = 0, length_ = 0, headerBytes_ = 0;
  bool inHeaders_ = false;
  Format format_ = Format::None;
};
} // namespace gea::host::video
