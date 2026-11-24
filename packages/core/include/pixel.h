#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ESP32-S3 PIE (Processor Instruction Extensions): 128-bit vector stores for
// the RGB565 span-fill hot path. Guarded on the IDF target so every other
// target (wasm, apple, geaos, host tools) compiles the portable path.
#if defined(__XTENSA__) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define GEA_PIXEL_PIE_FILL16 1
#else
#define GEA_PIXEL_PIE_FILL16 0
#endif

#ifndef GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
#define GEA_EMBEDDED_PIXEL_PANEL_ENDIAN 0
#endif

// Per-target native framebuffer pixel format, fixed at COMPILE TIME. A target
// has exactly one format, so this is a build directive — never a runtime branch.
// esp32 / geaos panels are RGB565 (16-bit); iOS is RGBA8888 (32-bit, full
// colour); Android is ARGB8888 to match Bitmap.Config.ARGB_8888. Each target compiles to one native pixel type + native pixel ops via
// the `native_t` typedef + `#if` below — no per-pixel colour conversion.
#define GEA_PIXEL_RGB565   0
#define GEA_PIXEL_RGBA8888 1
#define GEA_PIXEL_ARGB8888 2
// GRAY4: 16-level grayscale, ONE nibble (4 bits) per pixel. For panels that
// display exactly 16 gray levels (e.g. the M5Paper IT8951 in GL16 mode). The
// pixel VALUE type is a byte holding 0..15, so every colour/blend helper is a
// plain scalar op; the 4-bit STORAGE packing (2 pixels per byte) lives entirely
// in the Canvas framebuffer accessors. This is what lets a grayscale target
// render straight into the panel's own format — no RGB565 buffer, no per-frame
// repack, quarter-size framebuffer. Additive + compile-time gated: RGB565/8888
// targets never compile any GRAY4 branch, so their output is unchanged.
#define GEA_PIXEL_GRAY4    3
// GRAY2: 4-level grayscale, TWO bits per pixel (4 px/byte). Same idea as GRAY4,
// one step further down: for panels that can only show four levels anyway (the
// SSD1677's dual RAM planes give exactly black / dark / light / white) on parts
// with no PSRAM. A 528x792 framebuffer costs 102 KB at 2bpp against 204 KB at
// 4bpp — the difference between fitting and not fitting in an ESP32-C3's ~380 KB
// of SRAM. Like GRAY4 the pixel VALUE type is a byte (0..3); only the
// framebuffer STORAGE is packed.
#define GEA_PIXEL_GRAY2    4
#ifndef GEA_EMBEDDED_PIXEL_FORMAT
#define GEA_EMBEDDED_PIXEL_FORMAT GEA_PIXEL_RGB565
#endif
#define GEA_PIXEL_FORMAT_IS_8888 \
	(GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888 || GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888)
#define GEA_PIXEL_FORMAT_IS_GRAY4 (GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_GRAY4)
#define GEA_PIXEL_FORMAT_IS_GRAY2 (GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_GRAY2)
// "A grayscale-level format" — the pixel value is a small integer level in a
// single byte rather than packed colour channels. Everything that treats GRAY4
// as "not an RGB565 pack and not an 8888" applies verbatim to GRAY2.
#define GEA_PIXEL_FORMAT_IS_GRAY (GEA_PIXEL_FORMAT_IS_GRAY4 || GEA_PIXEL_FORMAT_IS_GRAY2)
// The grayscale formats store the framebuffer in the panel's own sub-byte
// packing, so it goes straight to the controller with no per-frame repack. The
// Canvas span/pixel primitives handle the read-modify-write; the pixel VALUE
// type stays a full byte.
//
// GEA_PIXEL_STORAGE_PACKED_BITS is the ONE knob: bits of framebuffer storage per
// pixel when the format is sub-byte packed (4 for GRAY4 = 2 px/byte, 2 for GRAY2
// = 4 px/byte), and 0 for every byte-addressed format. Packed code is written
// against this constant rather than against a per-depth macro, so a new depth is
// a value here plus the value helpers below — not a second copy of every
// framebuffer primitive.
#if GEA_PIXEL_FORMAT_IS_GRAY4
#define GEA_PIXEL_STORAGE_PACKED_BITS 4
#elif GEA_PIXEL_FORMAT_IS_GRAY2
#define GEA_PIXEL_STORAGE_PACKED_BITS 2
#else
#define GEA_PIXEL_STORAGE_PACKED_BITS 0
#endif
#define GEA_PIXEL_STORAGE_PACKED (GEA_PIXEL_STORAGE_PACKED_BITS != 0)

namespace gea::framework::graphics::pixel {

enum class Format : std::uint8_t {
	Rgb565,
	Rgb888,
	Rgba8888,
	Argb8888
};

constexpr int bitsPerPixel(Format format)
{
	switch (format) {
	case Format::Rgb565:
		return 16;
	case Format::Rgb888:
		return 24;
	case Format::Rgba8888:
	case Format::Argb8888:
		return 32;
	}
	return 16;
}

constexpr int bytesPerPixel(Format format)
{
	return bitsPerPixel(format) / 8;
}

constexpr const char *name(Format format)
{
	switch (format) {
	case Format::Rgb565:
		return "rgb565";
	case Format::Rgb888:
		return "rgb888";
	case Format::Rgba8888:
		return "rgb8888";
	case Format::Argb8888:
		return "argb8888";
	}
	return "rgb565";
}

inline Format parseFormat(const char *value, Format fallback = Format::Rgb565)
{
	if (!value) return fallback;
	const char *expected = name(Format::Rgb565);
	const char *cursor = value;
	while (*cursor && *expected && *cursor == *expected) {
		++cursor;
		++expected;
	}
	if (!*cursor && !*expected) return Format::Rgb565;
	expected = name(Format::Rgb888);
	cursor = value;
	while (*cursor && *expected && *cursor == *expected) {
		++cursor;
		++expected;
	}
	if (!*cursor && !*expected) return Format::Rgb888;
	expected = name(Format::Rgba8888);
	cursor = value;
	while (*cursor && *expected && *cursor == *expected) {
		++cursor;
		++expected;
	}
	if (!*cursor && !*expected) return Format::Rgba8888;
	expected = name(Format::Argb8888);
	cursor = value;
	while (*cursor && *expected && *cursor == *expected) {
		++cursor;
		++expected;
	}
	if (!*cursor && !*expected) return Format::Argb8888;
	return fallback;
}

constexpr std::uint16_t byteSwap16(std::uint16_t value)
{
	return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

constexpr std::uint16_t fromRgb565(std::uint16_t rgb565)
{
#if GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
	return byteSwap16(rgb565);
#else
	return rgb565;
#endif
}

constexpr std::uint16_t toRgb565(std::uint16_t pixel)
{
#if GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
	return byteSwap16(pixel);
#else
	return pixel;
#endif
}

constexpr std::uint16_t rgb565FromRgb888(int r, int g, int b)
{
	return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

constexpr std::uint16_t fromRgb888(int r, int g, int b)
{
	return fromRgb565(rgb565FromRgb888(r, g, b));
}

inline void unpackRgb565(std::uint16_t pixel, int *r, int *g, int *b)
{
	std::uint16_t rgb565 = toRgb565(pixel);
	*r = (rgb565 >> 11) & 0x1F;
	*g = (rgb565 >> 5) & 0x3F;
	*b = rgb565 & 0x1F;
}

constexpr int expand5To8(int value)
{
	return (value * 255 + 15) / 31;
}

constexpr int expand6To8(int value)
{
	return (value * 255 + 31) / 63;
}

constexpr int quantize8To5(int value)
{
	return (value * 31 + 127) / 255;
}

constexpr int quantize8To6(int value)
{
	return (value * 63 + 127) / 255;
}

constexpr std::uint16_t packRgb565Components(int r, int g, int b)
{
	return fromRgb565(static_cast<std::uint16_t>((r << 11) | (g << 5) | b));
}

// ---- Grayscale-level value helpers ------------------------------------------
// A grayscale pixel VALUE is a byte holding a small level — 0..15 for GRAY4,
// 0..3 for GRAY2, in both cases 0 = black and max = white. These convert to/from
// the framework's RGB565 authoring model and blend two gray values. Sub-byte
// PACKING (2 or 4 px/byte in the framebuffer) is separate and lives in Canvas —
// see GEA_PIXEL_STORAGE_PACKED_BITS.
//
// Luma is depth-independent, so both depths share it.
constexpr int grayLuma8(int r, int g, int b)
{
	// Rec.601 luma on 8-bit channels: (77 R + 150 G + 29 B) / 256.
	return (77 * r + 150 * g + 29 * b) >> 8;
}
constexpr std::uint8_t gray4FromRgb888(int r, int g, int b)
{
	// 8-bit luma (0..255) -> 4-bit level (0..15), rounded.
	return static_cast<std::uint8_t>((grayLuma8(r, g, b) * 15 + 127) / 255);
}
constexpr std::uint8_t gray4FromRgb565(std::uint16_t pixel)
{
	const std::uint16_t rgb = fromRgb565(pixel);  // undo any panel byte-swap
	const int r5 = (rgb >> 11) & 0x1F, g6 = (rgb >> 5) & 0x3F, b5 = rgb & 0x1F;
	return gray4FromRgb888((r5 * 255 + 15) / 31, (g6 * 255 + 31) / 63, (b5 * 255 + 15) / 31);
}
constexpr int gray4Expand8(std::uint8_t v) { return (v & 0x0F) * 255 / 15; }  // 0..15 -> 0..255
constexpr std::uint16_t gray4ToRgb565(std::uint8_t v)
{
	const int y = gray4Expand8(v);
	return packRgb565Components(quantize8To5(y), quantize8To6(y), quantize8To5(y));
}
inline std::uint8_t gray4Blend(std::uint8_t fg, std::uint8_t bg, int alpha)
{
	// Native 4-bit blend: the values ARE 4-bit levels, so expanding to 8-bit,
	// mixing with two /255 divides and requantizing with a third bought nothing
	// but cycles on a per-pixel path (text AA / translucent fills — Xtensa has
	// no integer divide). alpha 0..255 → 0..16, one fused multiply-accumulate
	// in the 4-bit domain: exact at the endpoints, within half a level in the
	// mid-tones the atlas already quantized.
	const int a = (alpha + 8) >> 4;  // 0..16
	return static_cast<std::uint8_t>(((fg & 0x0F) * a + (bg & 0x0F) * (16 - a) + 8) >> 4);
}

// GRAY2 (4-level grayscale, 2 bits/pixel). Same shape as the GRAY4 helpers, one
// depth down: 0 = black, 3 = white.
constexpr std::uint8_t gray2FromRgb888(int r, int g, int b)
{
	// 8-bit luma (0..255) -> 2-bit level (0..3), rounded.
	return static_cast<std::uint8_t>((grayLuma8(r, g, b) * 3 + 127) / 255);
}
constexpr std::uint8_t gray2FromRgb565(std::uint16_t pixel)
{
	const std::uint16_t rgb = fromRgb565(pixel);  // undo any panel byte-swap
	const int r5 = (rgb >> 11) & 0x1F, g6 = (rgb >> 5) & 0x3F, b5 = rgb & 0x1F;
	return gray2FromRgb888((r5 * 255 + 15) / 31, (g6 * 255 + 31) / 63, (b5 * 255 + 15) / 31);
}
constexpr int gray2Expand8(std::uint8_t v) { return (v & 0x03) * 255 / 3; }  // 0..3 -> 0..255
constexpr std::uint16_t gray2ToRgb565(std::uint8_t v)
{
	const int y = gray2Expand8(v);
	return packRgb565Components(quantize8To5(y), quantize8To6(y), quantize8To5(y));
}
inline std::uint8_t gray2Blend(std::uint8_t fg, std::uint8_t bg, int alpha)
{
	// Mirror of gray4Blend, and for the same reason: the RISC-V core on an
	// ESP32-C3 has no fast divide either (RV32IMC's DIV is multi-cycle and the
	// -march we build with may not even include M), so the blend must stay
	// divide-free. alpha 0..255 → 0..16 and ONE fused multiply-accumulate, kept
	// at 16 weight steps rather than 4: the extra steps cost nothing (same two
	// multiplies, same shift) and keep the rounding of an AA edge from snapping
	// to the endpoints, while a=0/a=16 remain exactly bg/fg. Result is 0..3
	// because 3*16 + 8 >> 4 == 3.
	const int a = (alpha + 8) >> 4;  // 0..16
	return static_cast<std::uint8_t>(((fg & 0x03) * a + (bg & 0x03) * (16 - a) + 8) >> 4);
}

namespace detail {
// Compile-time lookup tables for the 5/6-bit<->8-bit conversions, exact copies of
// expand5To8/expand6To8/quantize8To5/quantize8To6. The Xtensa LX7 has no integer
// divide, so the original blend cost ~12 software __divsi3 per pixel (4x/31, 2x/63,
// 3x/255 mix, 3x/255 quantize) — the dominant per-pixel cost of EVERY translucent
// fill/gradient/text pixel. These tables move all of that to compile time.
struct Lut5To8 { std::uint8_t v[32]; constexpr Lut5To8() : v{} { for (int i = 0; i < 32; i++) v[i] = static_cast<std::uint8_t>((i * 255 + 15) / 31); } };
struct Lut6To8 { std::uint8_t v[64]; constexpr Lut6To8() : v{} { for (int i = 0; i < 64; i++) v[i] = static_cast<std::uint8_t>((i * 255 + 31) / 63); } };
struct Lut8To5 { std::uint8_t v[256]; constexpr Lut8To5() : v{} { for (int i = 0; i < 256; i++) v[i] = static_cast<std::uint8_t>((i * 31 + 127) / 255); } };
struct Lut8To6 { std::uint8_t v[256]; constexpr Lut8To6() : v{} { for (int i = 0; i < 256; i++) v[i] = static_cast<std::uint8_t>((i * 63 + 127) / 255); } };
// On ESP32 targets the tables live in internal RAM, not flash rodata: every
// blended pixel reads them, and a display upload streaming the framebuffer
// through the data cache keeps evicting them, so from flash each blend could
// wait on a flash cache refill (measured on the ESP32-S31: ~300 ns per
// translucent sprite pixel while an async present streamed the frame).
#if defined(ESP_PLATFORM)
#define GEA_PIXEL_LUT_SECTION __attribute__((section(".dram1.gea_pixel_lut")))
#else
#define GEA_PIXEL_LUT_SECTION
#endif
GEA_PIXEL_LUT_SECTION inline constexpr Lut5To8 kExp5{};
GEA_PIXEL_LUT_SECTION inline constexpr Lut6To8 kExp6{};
GEA_PIXEL_LUT_SECTION inline constexpr Lut8To5 kQuant5{};
GEA_PIXEL_LUT_SECTION inline constexpr Lut8To6 kQuant6{};
}  // namespace detail

// ---- RGBA8888 (32-bit) helpers ---------------------------------------------
// Used by the iOS target, which renders into a full-colour RGBA8888 framebuffer
// instead of RGB565. Byte layout in memory is R,G,B,A (matches a little-endian
// uint32 of (A<<24)|(B<<16)|(G<<8)|R and an iOS CGImage configured RGBA8 little).
// esp32/geaos never touch these — they stay on the RGB565 path above.

constexpr std::uint32_t packRgba8888(int r, int g, int b, int a = 255)
{
	return static_cast<std::uint32_t>((r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) |
	                                  ((a & 0xFF) << 24));
}

constexpr std::uint32_t packArgb8888(int r, int g, int b, int a = 255)
{
	return static_cast<std::uint32_t>(((a & 0xFF) << 24) | ((r & 0xFF) << 16) |
	                                  ((g & 0xFF) << 8) | (b & 0xFF));
}

// Expand an RGB565 (panel-endian-aware) pixel to full RGBA8888. The 5/6-bit
// channels are expanded to 8-bit; this is lossless within 565's gamut but lets
// subsequent 8-bit blends (gradients, alpha, AA) accumulate without re-banding.
inline std::uint32_t rgba8888FromRgb565(std::uint16_t pixel)
{
	int r, g, b;
	unpackRgb565(pixel, &r, &g, &b);
	return packRgba8888(detail::kExp5.v[r], detail::kExp6.v[g], detail::kExp5.v[b], 255);
}

inline std::uint32_t argb8888FromRgb565(std::uint16_t pixel)
{
	int r, g, b;
	unpackRgb565(pixel, &r, &g, &b);
	return packArgb8888(detail::kExp5.v[r], detail::kExp6.v[g], detail::kExp5.v[b], 255);
}

constexpr std::uint32_t rgba8888FromRgb888(int r, int g, int b, int a = 255)
{
	return packRgba8888(r, g, b, a);
}

inline void unpackRgba8888(std::uint32_t pixel, int *r, int *g, int *b, int *a)
{
	*r = static_cast<int>(pixel & 0xFF);
	*g = static_cast<int>((pixel >> 8) & 0xFF);
	*b = static_cast<int>((pixel >> 16) & 0xFF);
	*a = static_cast<int>((pixel >> 24) & 0xFF);
}

inline void unpackArgb8888(std::uint32_t pixel, int *r, int *g, int *b, int *a)
{
	*a = static_cast<int>((pixel >> 24) & 0xFF);
	*r = static_cast<int>((pixel >> 16) & 0xFF);
	*g = static_cast<int>((pixel >> 8) & 0xFF);
	*b = static_cast<int>(pixel & 0xFF);
}

// Alpha-composite fg over bg in full 8-bit per channel (no 565 re-quantization),
// so gradients / translucent fills / anti-aliased edges stay smooth on iOS.
inline std::uint32_t blend8888(std::uint32_t fg, std::uint32_t bg, int alpha)
{
	if (alpha >= 255) return fg | 0xFF000000u;
	if (alpha <= 0) return bg;
	const int inv = 255 - alpha;
	const int fr = static_cast<int>(fg & 0xFF);
	const int fgc = static_cast<int>((fg >> 8) & 0xFF);
	const int fb = static_cast<int>((fg >> 16) & 0xFF);
	const int br = static_cast<int>(bg & 0xFF);
	const int bgc = static_cast<int>((bg >> 8) & 0xFF);
	const int bb = static_cast<int>((bg >> 16) & 0xFF);
	const int r = ((fr * alpha + br * inv) + 128) * 257 >> 16;
	const int g = ((fgc * alpha + bgc * inv) + 128) * 257 >> 16;
	const int b = ((fb * alpha + bb * inv) + 128) * 257 >> 16;
	return packRgba8888(r, g, b, 255);
}

inline std::uint32_t blendArgb8888(std::uint32_t fg, std::uint32_t bg, int alpha)
{
	if (alpha >= 255) return fg | 0xFF000000u;
	if (alpha <= 0) return bg;
	const int inv = 255 - alpha;
	const int fr = static_cast<int>((fg >> 16) & 0xFF);
	const int fgc = static_cast<int>((fg >> 8) & 0xFF);
	const int fb = static_cast<int>(fg & 0xFF);
	const int br = static_cast<int>((bg >> 16) & 0xFF);
	const int bgc = static_cast<int>((bg >> 8) & 0xFF);
	const int bb = static_cast<int>(bg & 0xFF);
	const int r = ((fr * alpha + br * inv) + 128) * 257 >> 16;
	const int g = ((fgc * alpha + bgc * inv) + 128) * 257 >> 16;
	const int b = ((fb * alpha + bb * inv) + 128) * 257 >> 16;
	return packArgb8888(r, g, b, 255);
}

inline std::uint16_t blend(std::uint16_t fg, std::uint16_t bg, int alpha)
{
	int fr, fg6, fb;
	int br, bg6, bb;
	unpackRgb565(fg, &fr, &fg6, &fb);
	unpackRgb565(bg, &br, &bg6, &bb);
	const int inverse = 255 - alpha;
	// 5/6->8 expand via LUT, /255 via multiply-shift (((v+128)*257)>>16 == round(v/255)
	// for v in [0,65025]), 8->5/6 quantize via LUT. Bit-identical to the divide-based
	// version but with zero per-pixel software divides.
	const int vr = detail::kExp5.v[fr] * alpha + detail::kExp5.v[br] * inverse;
	const int vg = detail::kExp6.v[fg6] * alpha + detail::kExp6.v[bg6] * inverse;
	const int vb = detail::kExp5.v[fb] * alpha + detail::kExp5.v[bb] * inverse;
	const int r8 = ((vr + 128) * 257) >> 16;
	const int g8 = ((vg + 128) * 257) >> 16;
	const int b8 = ((vb + 128) * 257) >> 16;
	return packRgb565Components(
		detail::kQuant5.v[r8],
		detail::kQuant6.v[g8],
		detail::kQuant5.v[b8]);
}

// blend(fg, bg, alpha) for ONE fg and alpha, as three per-channel tables. The
// RGB565 channels blend independently, so a table per channel indexed by the
// background's channel value gives exactly blend()'s result: three loads and
// two ORs per pixel instead of six table lookups and six multiplies. A
// translucent fill (glass, a halo disc, a span of one coverage) blends every
// pixel at the same fg and alpha, so the 128 entries are built once per
// colour/alpha change, not per pixel.
struct ConstBlend565
{
	std::uint16_t r[32];
	std::uint16_t g[64];
	std::uint16_t b[32];
	std::uint16_t fg = 0;
	int alpha = -1;

	void prepare(std::uint16_t fgPixel, int a)
	{
		if (fgPixel == fg && a == alpha) return;
		fg = fgPixel;
		alpha = a;
		int fr, fg6, fb;
		unpackRgb565(fgPixel, &fr, &fg6, &fb);
		const int inverse = 255 - a;
		const int tr = detail::kExp5.v[fr] * a + 128;
		const int tg = detail::kExp6.v[fg6] * a + 128;
		const int tb = detail::kExp5.v[fb] * a + 128;
		for (int i = 0; i < 32; i++) {
			const int bg8 = detail::kExp5.v[i] * inverse;
			r[i] = static_cast<std::uint16_t>(detail::kQuant5.v[((tr + bg8) * 257) >> 16] << 11);
			b[i] = detail::kQuant5.v[((tb + bg8) * 257) >> 16];
		}
		for (int i = 0; i < 64; i++) {
			const int bg8 = detail::kExp6.v[i] * inverse;
			g[i] = static_cast<std::uint16_t>(detail::kQuant6.v[((tg + bg8) * 257) >> 16] << 5);
		}
	}

	std::uint16_t apply(std::uint16_t bgPixel) const
	{
		const std::uint16_t v = toRgb565(bgPixel);
		return fromRgb565(static_cast<std::uint16_t>(r[v >> 11] | g[(v >> 5) & 0x3F] | b[v & 0x1F]));
	}
};

// ---- Compile-time native pixel type ----------------------------------------
// `native_t` is THIS target's framebuffer/storage pixel — RGB565 (16-bit) or
// RGBA8888 (32-bit) per GEA_EMBEDDED_PIXEL_FORMAT. Canvas and the renderer
// operate solely on native_t; there is no runtime format branch and no
// per-pixel colour conversion. The framework colour model is RGB565
// (std::uint16_t); `toNative` converts a colour to the native pixel ONCE per
// draw call (identity on RGB565 targets, so it compiles away).
#if GEA_PIXEL_FORMAT_IS_8888
using native_t = std::uint32_t;
#elif GEA_PIXEL_FORMAT_IS_GRAY
// A grayscale pixel value is a single byte holding the gray level (0..15 GRAY4,
// 0..3 GRAY2). Storage in the framebuffer is sub-byte packed (2 or 4 px/byte),
// but the value carried through style, display commands and blends is this
// scalar byte.
using native_t = std::uint8_t;
#else
using native_t = std::uint16_t;
#endif

struct NativeColor {
	native_t value;

	constexpr NativeColor(native_t native = native_t{}) : value(native) {}
	constexpr operator native_t() const { return value; }
};

inline native_t toNative(std::uint16_t rgb565)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	return rgba8888FromRgb565(rgb565);
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	return argb8888FromRgb565(rgb565);
#elif GEA_PIXEL_FORMAT_IS_GRAY4
	return gray4FromRgb565(rgb565);
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	return gray2FromRgb565(rgb565);
#else
	return rgb565;
#endif
}

constexpr std::uint32_t rgba8888ToArgb8888(std::uint32_t rgba)
{
	return (rgba & 0xFF000000u) | ((rgba & 0x000000FFu) << 16) |
	       (rgba & 0x0000FF00u) | ((rgba & 0x00FF0000u) >> 16);
}

// ---- The ONE colour conversion: authoring RGBA8888 -> this board's pixel -----
// gea apps author colours in RGBA8888 (bytes R,G,B,A — `packRgba8888`). This is
// the single place a colour becomes a pixel, and it is `constexpr`: a colour that
// is known at compile time (CSS literal folded by geatsc, a constant) is lowered
// to the board's native pixel AT BUILD TIME (esp32 -> RGB565, iOS -> RGBA8888) and
// costs nothing at runtime. A colour computed at runtime (the imperative
// `fillStyle = …` path) runs the same function once at the set-site. It never
// appears in the render path: style/IR/Canvas all carry `native_t`, born native.
constexpr native_t nativeFromRgba8888(std::uint32_t rgba)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	return rgba;
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	return rgba8888ToArgb8888(rgba);
#elif GEA_PIXEL_FORMAT_IS_GRAY4
	return gray4FromRgb888(static_cast<int>(rgba & 0xFFu),
	                       static_cast<int>((rgba >> 8) & 0xFFu),
	                       static_cast<int>((rgba >> 16) & 0xFFu));
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	return gray2FromRgb888(static_cast<int>(rgba & 0xFFu),
	                       static_cast<int>((rgba >> 8) & 0xFFu),
	                       static_cast<int>((rgba >> 16) & 0xFFu));
#else
	return fromRgb888(static_cast<int>(rgba & 0xFFu),
	                  static_cast<int>((rgba >> 8) & 0xFFu),
	                  static_cast<int>((rgba >> 16) & 0xFFu));
#endif
}

// Author a colour from 8-bit channels (alpha defaults opaque). Same build-time /
// runtime folding behaviour as nativeFromRgba8888.
constexpr native_t nativeColor(int r, int g, int b, int a = 255)
{
	return nativeFromRgba8888(packRgba8888(r, g, b, a));
}

constexpr NativeColor nativeColorValue(int r, int g, int b, int a = 255)
{
	return NativeColor(nativeColor(r, g, b, a));
}

constexpr NativeColor nativeColorFromRgba8888(std::uint32_t rgba)
{
	return NativeColor(nativeFromRgba8888(rgba));
}

// Author a colour from a STANDARD 0xRRGGBBAA value (red in the HIGH byte — the
// natural order an app writes, e.g. 0xff8800ff for orange / `rgb(r,g,b)` /
// `rgba(r,g,b,a)`). This is the canonical numeric colour the canvas API accepts;
// geatsc lowers a runtime canvas-colour number through here. (packRgba8888 /
// nativeFromRgba8888 use the in-memory R,G,B,A byte order, i.e. 0xAABBGGRR as a
// hex constant — an internal representation, not what apps type.)
constexpr native_t nativeFromRrggbbaa(std::uint32_t rrggbbaa)
{
	return nativeColor(static_cast<int>((rrggbbaa >> 24) & 0xFFu),
	                   static_cast<int>((rrggbbaa >> 16) & 0xFFu),
	                   static_cast<int>((rrggbbaa >> 8) & 0xFFu),
	                   static_cast<int>(rrggbbaa & 0xFFu));
}

constexpr NativeColor nativeColorFromRrggbbaa(std::uint32_t rrggbbaa)
{
	return NativeColor(nativeFromRrggbbaa(rrggbbaa));
}

// PRE-panel-swap authoring form, for the style pipeline that applies the panel
// byte-swap downstream (StyleValues::pixelFromStyleValue). On 16-bit panels this
// is the RAW (unswapped) RGB565 — the swap happens once on write; on full-colour
// boards there is no panel concept, so it equals the final native pixel.
constexpr native_t nativeStyleValueFromRgba8888(std::uint32_t rgba)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	return rgba;
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	return rgba8888ToArgb8888(rgba);
#elif GEA_PIXEL_FORMAT_IS_GRAY4
	// GRAY4 has no panel byte-swap, so — like the full-colour boards — the style
	// value IS the final native pixel (a 0..15 gray level). pixelFromStyleValue is
	// therefore identity for GRAY4 (it must NOT re-run gray4FromRgb565, or the gray
	// value gets re-read as an RGB565 and white collapses to black).
	return gray4FromRgb888(static_cast<int>(rgba & 0xFFu),
	                       static_cast<int>((rgba >> 8) & 0xFFu),
	                       static_cast<int>((rgba >> 16) & 0xFFu));
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	// Same for GRAY2: no panel byte-swap, so the style value IS the final native
	// pixel (a 0..3 gray level) and pixelFromStyleValue stays identity.
	return gray2FromRgb888(static_cast<int>(rgba & 0xFFu),
	                       static_cast<int>((rgba >> 8) & 0xFFu),
	                       static_cast<int>((rgba >> 16) & 0xFFu));
#else
	return rgb565FromRgb888(static_cast<int>(rgba & 0xFFu),
	                       static_cast<int>((rgba >> 8) & 0xFFu),
	                       static_cast<int>((rgba >> 16) & 0xFFu));
#endif
}

constexpr native_t nativeStyleValue(int r, int g, int b, int a = 255)
{
	return nativeStyleValueFromRgba8888(packRgba8888(r, g, b, a));
}

// Device builds blend RGB565 pixels with a branch-free SWAR form instead of
// blend()'s nine table lookups: the red and blue fields share one multiply
// (6-bit weight; blue's product stays below red's field), green takes its own
// (8-bit weight). Within 1 LSB per channel of blend() over every fg, bg and
// alpha (checked exhaustively); alpha 0 gives bg and 255 gives fg exactly.
// On the ESP32-S31 blend() compiled to ~62 instructions per pixel, ~67 cycles
// on its in-order core; this is ~25. Host builds keep the exact blend, which
// the pixel tests compare against.
#ifndef GEA_PIXEL_FAST_BLEND
#if defined(ESP_PLATFORM)
#define GEA_PIXEL_FAST_BLEND 1
#else
#define GEA_PIXEL_FAST_BLEND 0
#endif
#endif

inline std::uint16_t blendFast565(std::uint16_t fg, std::uint16_t bg, int alpha)
{
	const std::int32_t a = alpha + (alpha >> 7);  // 0..256
	const std::int32_t bgRb = bg & 0xF81F;
	const std::int32_t bgG = bg & 0x07E0;
	const std::int32_t rb = bgRb + ((((fg & 0xF81F) - bgRb) * (a >> 2)) >> 6);
	const std::int32_t g = bgG + ((((fg & 0x07E0) - bgG) * a) >> 8);
	return static_cast<std::uint16_t>((rb & 0xF81F) | (g & 0x07E0));
}

inline native_t blendNative(native_t fg, native_t bg, int alpha)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	return blend8888(fg, bg, alpha);
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	return blendArgb8888(fg, bg, alpha);
#elif GEA_PIXEL_FORMAT_IS_GRAY4
	return gray4Blend(fg, bg, alpha);
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	return gray2Blend(fg, bg, alpha);
#elif GEA_PIXEL_FAST_BLEND
	return fromRgb565(blendFast565(toRgb565(fg), toRgb565(bg), alpha));
#else
	return blend(fg, bg, alpha);
#endif
}

// Unpack a native pixel to full 8-bit channels. On RGB565 targets the 5/6-bit
// channels are expanded to 8-bit (alpha is always opaque). Used by paths that
// must do channel math on the source — e.g. bilinear image scaling.
inline void unpackNative8(native_t pixel, int *r, int *g, int *b, int *a)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	unpackRgba8888(pixel, r, g, b, a);
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	unpackArgb8888(pixel, r, g, b, a);
#else
#if GEA_PIXEL_FORMAT_IS_GRAY4
	const int y = gray4Expand8(pixel);
	*r = y; *g = y; *b = y; *a = 255;
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	const int y = gray2Expand8(pixel);
	*r = y; *g = y; *b = y; *a = 255;
#else
	int r5 = 0, g6 = 0, b5 = 0;
	unpackRgb565(pixel, &r5, &g6, &b5);
	*r = detail::kExp5.v[r5];
	*g = detail::kExp6.v[g6];
	*b = detail::kExp5.v[b5];
	*a = 255;
#endif
#endif
}

// Pack full 8-bit channels into a native pixel (quantizes to 5/6-bit on RGB565).
inline native_t packNative8(int r, int g, int b, int a = 255)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	return packRgba8888(r, g, b, a);
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	return packArgb8888(r, g, b, a);
#else
	(void)a;
#if GEA_PIXEL_FORMAT_IS_GRAY4
	return gray4FromRgb888(r & 0xFF, g & 0xFF, b & 0xFF);
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	return gray2FromRgb888(r & 0xFF, g & 0xFF, b & 0xFF);
#else
	return packRgb565Components(detail::kQuant5.v[r & 0xFF], detail::kQuant6.v[g & 0xFF], detail::kQuant5.v[b & 0xFF]);
#endif
#endif
}

// Downconvert a native framebuffer pixel back to an RGB565 colour. Identity on
// RGB565 targets (compiles away). Used by the few RGB565-internal scratch
// subsystems (filter-blur layers, recolor) that read framebuffer pixels into a
// 16-bit working buffer; on RGBA8888 targets those subsystems are inactive but
// must still type-check.
inline std::uint16_t fromNative(native_t pixel)
{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGBA8888
	int r = 0, g = 0, b = 0, a = 0;
	unpackRgba8888(pixel, &r, &g, &b, &a);
	return packRgb565Components(detail::kQuant5.v[r & 0xFF], detail::kQuant6.v[g & 0xFF], detail::kQuant5.v[b & 0xFF]);
#elif GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_ARGB8888
	int r = 0, g = 0, b = 0, a = 0;
	unpackArgb8888(pixel, &r, &g, &b, &a);
	return packRgb565Components(detail::kQuant5.v[r & 0xFF], detail::kQuant6.v[g & 0xFF], detail::kQuant5.v[b & 0xFF]);
#elif GEA_PIXEL_FORMAT_IS_GRAY4
	return gray4ToRgb565(pixel);
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	return gray2ToRgb565(pixel);
#else
	return pixel;
#endif
}

// Non-overlapping native pixel spans, including cropped rows whose source and
// destination have different vector alignment. Keep every load within the span:
// a scalar head precedes the first aligned-down load, and the scalar tail leaves
// room for the unaligned vector load's second aligned block.
inline void copyNative(native_t *dst, const native_t *src, int count)
{
	if (count <= 0) return;
#if GEA_PIXEL_PIE_FILL16 && !GEA_PIXEL_FORMAT_IS_8888 && !GEA_PIXEL_FORMAT_IS_GRAY
	if (count >= 32) {
		for (int i = 0; i < 8; ++i) { *dst++ = *src++; --count; }
		while ((reinterpret_cast<std::uintptr_t>(dst) & 15u) != 0) {
			*dst++ = *src++;
			--count;
		}
		const int vectors = (count - 8) >> 3;
		if (vectors > 0) {
			const native_t *cursor = src;
			asm volatile(
				"ee.ld.128.usar.ip q0, %[src], 16\n"
				"loopnez %[vectors], 1f\n"
				"ee.ld.128.usar.ip q1, %[src], 16\n"
				"ee.src.q.qup q2, q0, q1\n"
				"ee.vst.128.ip q2, %[dst], 16\n"
				"ee.orq q0, q1, q1\n"
				"1:\n"
				: [src] "+&r"(cursor), [dst] "+&r"(dst)
				: [vectors] "r"(vectors)
				: "memory");
			src += vectors * 8;
			count -= vectors * 8;
		}
	}
#endif
	std::memcpy(dst, src, static_cast<std::size_t>(count) * sizeof(native_t));
}

// Row copies across the RGB565-scratch <-> native-framebuffer boundary. RGB565
// uses the native span copy above; other framebuffer formats convert per pixel.
inline void copyNativeToRgb565(std::uint16_t *dst, const native_t *src, int count)
{
	if (count <= 0) return;
#if GEA_PIXEL_FORMAT_IS_8888 || GEA_PIXEL_FORMAT_IS_GRAY
	for (int i = 0; i < count; i++) dst[i] = fromNative(src[i]);
#else
	copyNative(dst, src, count);
#endif
}

inline void copyRgb565ToNative(native_t *dst, const std::uint16_t *src, int count)
{
	if (count <= 0) return;
#if GEA_PIXEL_FORMAT_IS_8888 || GEA_PIXEL_FORMAT_IS_GRAY
	for (int i = 0; i < count; i++) dst[i] = toNative(src[i]);
#else
	copyNative(dst, src, count);
#endif
}

// Fill `count` native pixels with a native colour. Keeps the RGB565 fast path
// (memset for black, uint32-pair fill) on 16-bit targets.
#if GEA_PIXEL_PIE_FILL16 && !GEA_PIXEL_FORMAT_IS_8888
// PIE span fill: 8 RGB565 pixels per EE.VST.128.IP through a zero-overhead
// loop, vs 2 per store on the portable 32-bit-pair path. The q registers are
// not compiler-allocated (q0 is free to clobber), and the IDF FreeRTOS
// xtensa port lazily context-switches PIE state like the FPU, so this is
// task-safe. Scalar head to reach 16-byte alignment (EE.VST.128 requires
// it), vector body, scalar tail.
inline void fillNativePie(native_t *dst, int count, native_t color)
{
	while (count > 0 && (reinterpret_cast<std::uintptr_t>(dst) & 0xFu) != 0) {
		*dst++ = color;
		count--;
	}
	const int vec = count >> 3;
	if (vec > 0) {
		alignas(16) native_t pattern[8] = {color, color, color, color, color, color, color, color};
		const native_t *pat = pattern;
		asm volatile(
			"ee.vld.128.ip q0, %[pat], 0\n"
			"loopnez %[vec], 1f\n"
			"ee.vst.128.ip q0, %[dst], 16\n"
			"1:\n"
			: [dst] "+r"(dst), [pat] "+r"(pat)
			: [vec] "r"(vec)
			: "memory");
		count -= vec << 3;
	}
	while (count-- > 0) *dst++ = color;
}
#endif

inline void fillNative(native_t *dst, int count, native_t color)
{
	if (count <= 0) return;
#if GEA_PIXEL_FORMAT_IS_8888
	std::fill_n(dst, count, color);
#elif GEA_PIXEL_FORMAT_IS_GRAY
	// native_t is one byte, so an UNPACKED gray-value run is a plain memset.
	// (The sub-byte-PACKED framebuffer is filled by Canvas, not here.)
	std::memset(dst, color, static_cast<std::size_t>(count));
#else
#if GEA_PIXEL_PIE_FILL16
	// Worth the head/tail bookkeeping from two vector stores up.
	if (count >= 16) {
		fillNativePie(dst, count, color);
		return;
	}
#endif
	if (color == 0) {
		std::memset(dst, 0, static_cast<std::size_t>(count) * sizeof(native_t));
		return;
	}
	if ((reinterpret_cast<std::uintptr_t>(dst) & 0x2u) != 0) {
		*dst++ = color;
		count--;
	}
	const std::uint32_t pair = static_cast<std::uint32_t>(color) | (static_cast<std::uint32_t>(color) << 16);
	const int pairCount = count / 2;
	std::fill_n(reinterpret_cast<std::uint32_t *>(dst), pairCount, pair);
	if ((count & 1) != 0) dst[pairCount * 2] = color;
#endif
}

// ---- Sub-byte packed STORAGE primitives -------------------------------------
// The framebuffer packs 8/Bits pixels per byte, MSB first — the order both the
// IT8951 (4bpp) and the SSD1677 (2bpp) read their RAM in. For Bits = 4 that is
// byte[b] = (px[2b] << 4) | px[2b+1]; for Bits = 2 it is
// byte[b] = (px[4b] << 6) | (px[4b+1] << 4) | (px[4b+2] << 2) | px[4b+3].
// `row` points at the first packed byte of a physical row; `x` is a pixel index
// within that row — always >= 0, since every Canvas caller has already clipped to
// the framebuffer (the / and % below truncate toward zero, so a negative index
// would not address the byte a floor-division would).
//
// These are pure byte-buffer ops (no format guard) parameterised on the bit
// depth, so a host test can exercise every depth in one binary; the Canvas
// framebuffer accessors instantiate exactly one of them, via the `packed` alias
// below, under GEA_PIXEL_STORAGE_PACKED. Bits is a compile-time constant, so
// every / and % here folds to a shift or a mask — the 4bpp code generated is the
// same as the hand-written nibble version it replaces.
template <int Bits>
struct PackedPixels {
	static_assert(Bits == 2 || Bits == 4, "packed pixel storage supports 2 or 4 bits per pixel");

	static constexpr int kBits = Bits;
	static constexpr int kPxPerByte = 8 / Bits;
	static constexpr std::uint8_t kMask = static_cast<std::uint8_t>((1u << Bits) - 1u);

	static constexpr int byteIndex(int x) { return x / kPxPerByte; }
	// MSB-first: pixel 0 of a byte occupies the TOP `Bits` bits.
	static constexpr int shiftFor(int x) { return 8 - Bits - (x % kPxPerByte) * Bits; }
	// Bytes needed to hold a row of `widthPx` pixels (partial trailing byte counts).
	static constexpr int rowBytes(int widthPx) { return (widthPx + kPxPerByte - 1) / kPxPerByte; }
	// A byte holding kPxPerByte copies of `v` — the value a solid span memsets with.
	static constexpr std::uint8_t replicate(std::uint8_t v)
	{
		std::uint8_t out = 0;
		for (int i = 0; i < kPxPerByte; i++) out = static_cast<std::uint8_t>((out << Bits) | (v & kMask));
		return out;
	}
	// Blend in the native value domain — see gray4Blend / gray2Blend.
	static std::uint8_t blendValue(std::uint8_t fg, std::uint8_t bg, int alpha)
	{
		if constexpr (Bits == 4) return gray4Blend(fg, bg, alpha);
		else return gray2Blend(fg, bg, alpha);
	}

	static std::uint8_t get(const std::uint8_t *row, int x)
	{
		return static_cast<std::uint8_t>((row[byteIndex(x)] >> shiftFor(x)) & kMask);
	}

	static void set(std::uint8_t *row, int x, std::uint8_t v)
	{
		std::uint8_t &byte = row[byteIndex(x)];
		const int shift = shiftFor(x);
		byte = static_cast<std::uint8_t>((byte & ~(kMask << shift)) | ((v & kMask) << shift));
	}

	// Fill a run of `count` pixels starting at pixel x0 with gray value v.
	static void fillSpan(std::uint8_t *row, int x0, int count, std::uint8_t v)
	{
		if (count <= 0) return;
		v &= kMask;
		int x = x0;
		int n = count;
		// Leading pixels sharing a byte with pixels BEFORE the span: RMW so their
		// neighbours already in that byte survive.
		while (n > 0 && (x % kPxPerByte) != 0) { set(row, x, v); x++; n--; }
		// Aligned middle: whole bytes hold kPxPerByte pixels of the same value.
		const std::uint8_t rep = replicate(v);
		const int bytes = n / kPxPerByte;
		if (bytes > 0) std::memset(row + byteIndex(x), rep, static_cast<std::size_t>(bytes));
		x += bytes * kPxPerByte;
		n -= bytes * kPxPerByte;
		// Trailing partial byte.
		while (n > 0) { set(row, x, v); x++; n--; }
	}

	// Write `count` UNPACKED gray values (one level per byte) into packed pixels.
	static void writeUnpacked(std::uint8_t *row, int x0, const std::uint8_t *src, int count)
	{
		for (int i = 0; i < count; i++) set(row, x0 + i, src[i]);
	}

	// Read `count` packed pixels into an UNPACKED buffer (one value per byte).
	static void readUnpacked(const std::uint8_t *row, int x0, std::uint8_t *dst, int count)
	{
		for (int i = 0; i < count; i++) dst[i] = get(row, x0 + i);
	}

	// Blend one pixel: dst = blendValue(fg, dst, alpha).
	static void blendPixel(std::uint8_t *row, int x, std::uint8_t fg, int alpha)
	{
		if (alpha <= 0) return;
		if (alpha >= 255) { set(row, x, fg); return; }
		set(row, x, blendValue(fg, get(row, x), alpha));
	}

	// Copy `count` pixels between two packed rows (e.g. scrollRect). Falls back to
	// per-pixel; both endpoints may be unaligned. Overlap-safe, including when
	// dstRow and srcRow are the SAME row.
	static void copyPacked(std::uint8_t *dstRow, int dstX, const std::uint8_t *srcRow, int srcX, int count)
	{
		if (count <= 0) return;

		// Same phase within the byte ⇒ the aligned middle is a byte-wise memmove.
		if ((dstX % kPxPerByte) == (srcX % kPxPerByte)) {
			// Split into `lead` pixels up to the first dst byte boundary, `mid`
			// whole bytes, then the trailing remainder.
			int lead = 0;
			while (lead < count && ((dstX + lead) % kPxPerByte) != 0) lead++;
			const int mid = (count - lead) / kPxPerByte;
			const int tail = lead + mid * kPxPerByte;

			// ORDER MATTERS when the rows alias and the ranges overlap. That case
			// is live, not theoretical: Canvas::scrollRect passes dstRow ==
			// srcRow for a horizontal pan (dy == 0, dx != 0), reached from
			// tree_render.cpp's Display::scrollRect(vx, vy, vw, vh, panDx, 0).
			//
			// memmove is itself overlap-safe, but the partial-byte fixups around
			// it are not — they read whole pixels out of bytes the memmove may
			// already have rewritten, and they write bytes the memmove may still
			// need to read. Doing lead-then-mid-then-tail unconditionally (as
			// this did before) corrupts a rightward overlapping move on both
			// counts.
			//
			// The fix is just to work from the far end of the move direction
			// inward. It is sound because same-phase forces |dstX - srcX| to be a
			// whole number of bytes, so the partial work on the far side is
			// always at least one byte clear of the memmove's range. Keeping the
			// memmove means scrolling does not lose its fast path.
			if (dstX > srcX) {
				for (int i = count - 1; i >= tail; i--) set(dstRow, dstX + i, get(srcRow, srcX + i));
				if (mid > 0)
					std::memmove(dstRow + byteIndex(dstX + lead), srcRow + byteIndex(srcX + lead),
					             static_cast<std::size_t>(mid));
				for (int i = lead - 1; i >= 0; i--) set(dstRow, dstX + i, get(srcRow, srcX + i));
			} else {
				for (int i = 0; i < lead; i++) set(dstRow, dstX + i, get(srcRow, srcX + i));
				if (mid > 0)
					std::memmove(dstRow + byteIndex(dstX + lead), srcRow + byteIndex(srcX + lead),
					             static_cast<std::size_t>(mid));
				for (int i = tail; i < count; i++) set(dstRow, dstX + i, get(srcRow, srcX + i));
			}
			return;
		}
		// Mismatched phase: shift pixel by pixel. Iterate in a safe direction.
		if (dstX <= srcX)
			for (int i = 0; i < count; i++) set(dstRow, dstX + i, get(srcRow, srcX + i));
		else
			for (int i = count - 1; i >= 0; i--) set(dstRow, dstX + i, get(srcRow, srcX + i));
	}
};

#if GEA_PIXEL_STORAGE_PACKED
// THIS target's packed-storage op set. Canvas writes `pixel::packed::set(...)`
// and gets the right depth for the board it is being compiled for.
using packed = PackedPixels<GEA_PIXEL_STORAGE_PACKED_BITS>;
#endif

}  // namespace gea::framework::graphics::pixel
