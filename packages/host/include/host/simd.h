// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

// ESP32-S3 PIE (Processor Instruction Extensions) vector kernels for per-frame
// byte-grid effects. Every other target compiles the scalar loop, which is also
// the reference the vector path must match bit for bit.
#if defined(__XTENSA__) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define GEA_HOST_SIMD_PIE 1
#include "freertos/FreeRTOS.h"
#else
#define GEA_HOST_SIMD_PIE 0
#endif

namespace gea::host::simd {

inline constexpr int kMaxWindows = 8;

inline void sumWindowsRowScalar(std::uint8_t *dst, const std::uint8_t *const *rows, int windows, int count, int multiplier,
                                int shift) {
  for (int x = 0; x < count; ++x) {
    int sum = 0;
    for (int k = 0; k < windows; ++k) sum += rows[k][x];
    const int value = (sum * multiplier) >> shift;
    dst[x] = static_cast<std::uint8_t>(value < 255 ? value : 255);
  }
}

#if GEA_HOST_SIMD_PIE
// 16 cells per group: each window's 16 source bytes come from two aligned
// loads joined by EE.SRC.Q (sources sit at any byte offset), are widened to
// u16 by zipping with zero and accumulated with saturating s16 adds. The sum
// is then scaled by `multiplier >> SAR`, clamped to 255 and narrowed back by
// keeping the low bytes. `dst` must be 16-byte aligned; each source read
// reaches up to 31 bytes past its group's start.
//
// q registers have been seen corrupted by a mid-kernel preemption (rgb_panel's
// blend kernel), so the block runs with this core's interrupts masked; a row is
// a few microseconds.
inline void sumWindowsGroupsPie(std::uint8_t *dst, const std::uint8_t **rows, int windows, int groups, int multiplier, int shift) {
  alignas(16) std::uint16_t constants[2] = {static_cast<std::uint16_t>(multiplier), 255};
  const std::uint16_t *limit = constants + 1;
  int column = 0;
  const std::uint8_t **cursor;
  const std::uint8_t *source;
  int remaining;
  const UBaseType_t interrupts = portSET_INTERRUPT_MASK_FROM_ISR();
  asm volatile(
      "wsr.sar %[shift]\n"
      "ee.vldbc.16 q6, %[constants]\n"
      "ee.vldbc.16 q7, %[limit]\n"
      "loopnez %[groups], 3f\n"
      "ee.zero.q q4\n"
      "ee.zero.q q5\n"
      "mov %[cursor], %[rows]\n"
      "mov %[remaining], %[windows]\n"
      "2:\n"
      "l32i %[source], %[cursor], 0\n"
      "add %[source], %[source], %[column]\n"
      "ee.ld.128.usar.ip q0, %[source], 16\n"
      "ee.ld.128.usar.ip q1, %[source], 0\n"
      "ee.src.q q2, q0, q1\n"
      "ee.zero.q q3\n"
      "ee.vzip.8 q2, q3\n"
      "ee.vadds.s16 q4, q4, q2\n"
      "ee.vadds.s16 q5, q5, q3\n"
      "addi %[cursor], %[cursor], 4\n"
      "addi %[remaining], %[remaining], -1\n"
      "bnez %[remaining], 2b\n"
      "ee.vmul.s16 q4, q4, q6\n"
      "ee.vmul.s16 q5, q5, q6\n"
      "ee.vmin.s16 q4, q4, q7\n"
      "ee.vmin.s16 q5, q5, q7\n"
      "ee.vunzip.8 q4, q5\n"
      "ee.vst.128.ip q4, %[dst], 16\n"
      "addi %[column], %[column], 16\n"
      "3:\n"
      : [dst] "+&r"(dst), [column] "+&r"(column), [cursor] "=&r"(cursor), [source] "=&r"(source), [remaining] "=&r"(remaining)
      : [rows] "r"(rows), [windows] "r"(windows), [groups] "r"(groups), [shift] "r"(shift), [constants] "r"(constants),
        [limit] "r"(limit)
      : "memory");
  portCLEAR_INTERRUPT_MASK_FROM_ISR(interrupts);
}
#endif

// Whether start + (rows - 1) * stride + width <= length, without overflowing.
inline bool spanFits(std::size_t start, std::size_t stride, int rows, int width, std::size_t length) {
  if (start > length) return false;
  std::size_t room = length - start;
  if (static_cast<std::size_t>(width) > room) return false;
  room -= static_cast<std::size_t>(width);
  const std::size_t steps = static_cast<std::size_t>(rows - 1);
  return steps == 0 || stride == 0 || steps <= room / stride;
}

// dst[dstStart + r*dstStride + x] =
//   min(255, ((Σ_k src[offsets[k] + r*srcStride + x]) * multiplier) >> shift)
// for r in [0, rows), x in [0, width). Any read or write outside the arrays
// makes the whole call a no-op rather than a partial write.
inline void sumWindowsU8(std::uint8_t *dst, std::size_t dstLength, int dstStart, int dstStride, const std::uint8_t *src,
                         std::size_t srcLength, const std::int32_t *offsets, std::size_t offsetsLength, int windows, int srcStride,
                         int width, int rows, int multiplier, int shift) {
  if (!dst || !src || !offsets || windows <= 0 || windows > kMaxWindows || static_cast<std::size_t>(windows) > offsetsLength) return;
  if (width <= 0 || rows <= 0 || multiplier < 0 || shift < 0 || shift > 30 || dstStart < 0 || dstStride < 0 || srcStride < 0) return;
  if (!spanFits(static_cast<std::size_t>(dstStart), static_cast<std::size_t>(dstStride), rows, width, dstLength)) return;
  for (int k = 0; k < windows; ++k) {
    if (offsets[k] < 0 || !spanFits(static_cast<std::size_t>(offsets[k]), static_cast<std::size_t>(srcStride), rows, width, srcLength)) return;
  }
#if GEA_HOST_SIMD_PIE
  // Sums stay in s16 lanes and the scaled value must not overflow one before
  // the clamp; outside that range the scalar loop is exact.
  const bool vector = multiplier <= 32767 && ((windows * 255 * multiplier) >> shift) <= 32767;
#endif
  const std::uint8_t *rowSources[kMaxWindows];
  for (int r = 0; r < rows; ++r) {
    std::uint8_t *out = dst + dstStart + static_cast<std::ptrdiff_t>(r) * dstStride;
    for (int k = 0; k < windows; ++k) rowSources[k] = src + offsets[k] + static_cast<std::ptrdiff_t>(r) * srcStride;
    int count = width;
#if GEA_HOST_SIMD_PIE
    if (vector && count >= 32) {
      const int head = static_cast<int>((16 - (reinterpret_cast<std::uintptr_t>(out) & 15u)) & 15u);
      sumWindowsRowScalar(out, rowSources, windows, head, multiplier, shift);
      out += head;
      count -= head;
      for (int k = 0; k < windows; ++k) rowSources[k] += head;
      int groups = count >> 4;
      // The last group's loads reach 31 bytes past its start; keep them inside
      // every source array by leaving trailing groups to the scalar tail.
      std::ptrdiff_t slack = static_cast<std::ptrdiff_t>(srcLength);
      for (int k = 0; k < windows; ++k) {
        const std::ptrdiff_t room = static_cast<std::ptrdiff_t>(srcLength) - (rowSources[k] - src);
        if (room < slack) slack = room;
      }
      while (groups > 0 && static_cast<std::ptrdiff_t>(groups - 1) * 16 + 32 > slack) --groups;
      if (groups > 0) {
        sumWindowsGroupsPie(out, rowSources, windows, groups, multiplier, shift);
        out += groups * 16;
        count -= groups * 16;
        for (int k = 0; k < windows; ++k) rowSources[k] += groups * 16;
      }
    }
#endif
    sumWindowsRowScalar(out, rowSources, windows, count, multiplier, shift);
  }
}

}  // namespace gea::host::simd

namespace gea::host {

namespace simd {
// The program's typed array, whether the call site passes the array view
// itself or a handle to one; null for an absent handle.
template <typename Array>
auto *viewOf(const Array &array) {
  if constexpr (requires { array.data(); array.size(); }) {
    return &const_cast<Array &>(array);
  } else {
    return array ? &*array : nullptr;
  }
}
}  // namespace simd

struct SimdFacade {
  // Typed-array arguments are bound by reference to the program's own views:
  // no copy, and no dependency here on the runtime that defines them. A view
  // is a window onto shared bytes, so writing through it is the JS semantics.
  template <typename Dst, typename Src, typename Offsets>
  void sumWindowsU8(const Dst &dstArray, double dstStart, double dstStride, const Src &srcArray, const Offsets &offsetsArray,
                    double windows, double srcStride, double width, double rows, double multiplier, double shift) const {
    auto *dst = simd::viewOf(dstArray);
    auto *src = simd::viewOf(srcArray);
    auto *offsets = simd::viewOf(offsetsArray);
    if (!dst || !src || !offsets) return;
    static_assert(sizeof(*dst->data()) == 1 && sizeof(*src->data()) == 1 && sizeof(*offsets->data()) == 4);
    simd::sumWindowsU8(reinterpret_cast<std::uint8_t *>(dst->data()), dst->size(), static_cast<int>(dstStart),
                       static_cast<int>(dstStride), reinterpret_cast<const std::uint8_t *>(src->data()), src->size(),
                       reinterpret_cast<const std::int32_t *>(offsets->data()), offsets->size(), static_cast<int>(windows),
                       static_cast<int>(srcStride), static_cast<int>(width), static_cast<int>(rows), static_cast<int>(multiplier),
                       static_cast<int>(shift));
  }
};

inline constexpr SimdFacade Simd{};

}  // namespace gea::host
