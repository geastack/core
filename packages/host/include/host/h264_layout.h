// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace gea::host::rtc::h264 {
struct Metadata {
    int width = 0, height = 0, left = 0, top = 0;
    int visibleWidth = 0, visibleHeight = 0;
    bool fullRange = false, bt709 = false;
};

// Read RBSP in place, skipping emulation-prevention bytes without another
// compressed-frame allocation. Only progressive 8-bit baseline streams are accepted.
class Bits {
public:
    Bits(const std::uint8_t *data, std::size_t length) : data_(data), length_(length) {}
    std::uint32_t read(unsigned count) {
        if (count > 32) { ok = false; return 0; }
        std::uint32_t value = 0;
        while (count--) {
            if (!left_) {
                if (position_ >= length_) { ok = false; return 0; }
                byte_ = data_[position_++];
                if (zeros_ >= 2 && byte_ == 3) {
                    if (position_ >= length_ || data_[position_] > 3) { ok = false; return 0; }
                    byte_ = data_[position_++];
                    zeros_ = 0;
                }
                zeros_ = byte_ == 0 ? zeros_ + 1 : 0;
                left_ = 8;
            }
            value = (value << 1) | ((byte_ >> --left_) & 1);
        }
        return value;
    }
    std::uint32_t ue() {
        unsigned zeros = 0;
        while (ok && !read(1)) {
            if (++zeros > 30) { ok = false; return 0; }
        }
        return ok ? ((1u << zeros) - 1) + read(zeros) : 0;
    }
    bool ok = true;
private:
    const std::uint8_t *data_;
    std::size_t length_, position_ = 0;
    unsigned byte_ = 0, left_ = 0, zeros_ = 0;
};

inline bool parseSps(const std::uint8_t *data, std::size_t length, Metadata &out) {
    Bits bits(data, length);
    if (bits.read(8) != 66) return false; // constrained/baseline, 8-bit 4:2:0
    bits.read(8); bits.read(8); // constraint flags, level
    if (bits.ue() != 0) return false; // one SPS/PPS set per stream
    if (bits.ue() > 12) return false; // log2_max_frame_num_minus4
    const auto poc = bits.ue();
    if (poc == 0) { if (bits.ue() > 12) return false; }
    else if (poc == 1) {
        bits.read(1); bits.ue(); bits.ue(); // signed Exp-Golomb values have the same extent
        const auto cycle = bits.ue();
        if (cycle > 255) return false;
        for (unsigned i = 0; i < cycle; ++i) bits.ue();
    } else if (poc != 2) return false;
    if (bits.ue() > 16) return false; // max_num_ref_frames
    bits.read(1); // gaps_in_frame_num_value_allowed_flag
    const auto mbWidth = bits.ue(), mbHeight = bits.ue();
    if (mbWidth > 63 || mbHeight > 63 || bits.read(1) != 1) return false;
    bits.read(1); // direct_8x8_inference_flag
    std::uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (bits.read(1)) { left = bits.ue(); right = bits.ue(); top = bits.ue(); bottom = bits.ue(); }
    const int width = (mbWidth + 1) * 16, height = (mbHeight + 1) * 16;
    if (!bits.ok || left > 512 || right > 512 || top > 512 || bottom > 512 ||
        width <= 2 * int(left + right) || height <= 2 * int(top + bottom))
        return false;
    Metadata next{width, height, int(left * 2), int(top * 2),
                  width - int(2 * (left + right)), height - int(2 * (top + bottom))};
    if (bits.read(1)) { // vui_parameters_present_flag; colour fields precede timing/HRD
        if (bits.read(1) && bits.read(8) == 255) { bits.read(16); bits.read(16); }
        if (bits.read(1)) bits.read(1); // overscan
        if (bits.read(1)) {
            bits.read(3); next.fullRange = bits.read(1);
            if (bits.read(1)) {
                bits.read(8); bits.read(8); // primaries, transfer characteristics
                const auto matrix = bits.read(8);
                if (matrix != 1 && matrix != 2 && matrix != 5 && matrix != 6) return false;
                next.bt709 = matrix == 1;
            }
        }
    }
    if (!bits.ok) return false;
    out = next;
    return true;
}

inline std::size_t startCode(const std::uint8_t *data, std::size_t length, std::size_t from, unsigned &prefix) {
    for (std::size_t i = from; i + 3 <= length; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { prefix = 3; return i; }
        if (i + 4 <= length && data[i + 2] == 0 && data[i + 3] == 1) { prefix = 4; return i; }
    }
    prefix = 0;
    return length;
}

inline bool inspectAccessUnit(const std::uint8_t *data, std::size_t length, Metadata &metadata) {
    unsigned prefix = 0;
    std::size_t position = startCode(data, length, 0, prefix);
    if (position == length) return false;
    for (std::size_t i = 0; i < position; ++i) if (data[i]) return false;
    while (position < length) {
        const auto nal = position + prefix;
        unsigned nextPrefix = 0;
        const auto next = startCode(data, length, nal, nextPrefix);
        if (nal >= next || (data[nal] & 128)) return false;
        const auto type = data[nal] & 31;
        if (type == 7 && !parseSps(data + nal + 1, next - nal - 1, metadata)) return false;
        if (type == 8) {
            Bits bits(data + nal + 1, next - nal - 1);
            if (bits.ue() != 0 || bits.ue() != 0 || !bits.ok) return false;
        }
        if (type == 1 || type == 5) {
            Bits bits(data + nal + 1, next - nal - 1);
            bits.ue(); bits.ue();
            if (bits.ue() != 0 || !bits.ok || !metadata.width) return false;
        }
        position = next; prefix = nextPrefix;
    }
    return metadata.width != 0;
}

// Four output pixels share one U/V pair. The frame stays I420 inside the
// reference decoder; only visible pixels are converted into the owned image.
inline void convertI420(const std::uint8_t *source, const Metadata &meta, std::uint16_t *output) {
    const int visibleWidth = meta.visibleWidth, visibleHeight = meta.visibleHeight;
    const int stride = meta.width, chromaStride = stride / 2;
    const auto *uPlane = source + meta.width * meta.height;
    const auto *vPlane = uPlane + (meta.width * meta.height) / 4;
    const int yScale = meta.fullRange ? 256 : 298;
    const int yBias = meta.fullRange ? 0 : 16;
    const int rV = meta.bt709 ? (meta.fullRange ? 403 : 459) : (meta.fullRange ? 359 : 409);
    const int gU = meta.bt709 ? (meta.fullRange ? 48 : 55) : (meta.fullRange ? 88 : 100);
    const int gV = meta.bt709 ? (meta.fullRange ? 120 : 136) : (meta.fullRange ? 183 : 208);
    const int bU = meta.bt709 ? (meta.fullRange ? 475 : 541) : (meta.fullRange ? 454 : 516);
    for (int y = 0; y < visibleHeight; y += 2) {
        const auto *row0 = source + (meta.top + y) * stride + meta.left;
        const auto *row1 = row0 + stride;
        const int chroma = ((meta.top + y) / 2) * chromaStride + meta.left / 2;
        auto *dst0 = output + y * visibleWidth;
        auto *dst1 = dst0 + visibleWidth;
        for (int x = 0; x < visibleWidth; x += 2) {
            const int u = int(uPlane[chroma + x / 2]) - 128;
            const int v = int(vPlane[chroma + x / 2]) - 128;
            const int red = rV * v, green = -gU * u - gV * v, blue = bU * u;
            const auto pack = [&](int luma) {
                const int value = yScale * (luma - yBias) + 128;
                const auto r = std::clamp((value + red) >> 8, 0, 255);
                const auto g = std::clamp((value + green) >> 8, 0, 255);
                const auto b = std::clamp((value + blue) >> 8, 0, 255);
                return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            };
            dst0[x] = pack(row0[x]); dst0[x + 1] = pack(row0[x + 1]);
            dst1[x] = pack(row1[x]); dst1[x + 1] = pack(row1[x + 1]);
        }
    }
}


} // namespace gea::host::rtc::h264
