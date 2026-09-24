// SPDX-License-Identifier: Apache-2.0
#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "pixel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// For the compile-gated GEA_EMBEDDED_TEXT_SPRITE_DIAG counters (see the diag
// block above Canvas::drawRasterizedText). Includes must stay at file scope.
#ifndef GEA_EMBEDDED_TEXT_SPRITE_DIAG
#define GEA_EMBEDDED_TEXT_SPRITE_DIAG 0
#endif
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
#ifdef ESP_PLATFORM
#include <esp_timer.h>
#include <esp_cpu.h>
#else
#include <chrono>
#endif
#endif

// For the text-sprite hot staging buffer (internal SRAM; see HotTextSprite).
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define GEA_CANVAS_HAS_FREERTOS 1
#else
#define GEA_CANVAS_HAS_FREERTOS 0
#endif

#if __has_include("pico.h")
#include "pico.h"  // provides get_core_num(); pico/platform.h can't be included directly
#define GEA_CANVAS_HAS_PICO 1
#else
#define GEA_CANVAS_HAS_PICO 0
#endif

namespace gea::framework::graphics {

namespace {
// Per-scanline coverage bitmask for fillTrianglesOpaqueOccluded. Bit x set = a
// nearer triangle already owns pixel x on that chunk-local row. Two banks so two
// Canvases on different cores can occlude concurrently (cross-core band raster).
constexpr int kOcclMaxRows = 48;  // chunk-local rows (gea3d half-res chunk ≤ 40)
constexpr int kOcclWords = 8;     // 256 px wide (half-res 205 fits; wider falls back)
std::uint32_t s_occlBits[2][kOcclMaxRows][kOcclWords];

inline int occlCoreBank()
{
#if GEA_CANVAS_HAS_FREERTOS
	return xPortGetCoreID() & 1;
#elif GEA_CANVAS_HAS_PICO
	// RP2350: the present splits each chunk across both M33 cores, which raster the
	// same triangle batch into disjoint row bands concurrently — each needs its own
	// coverage bank or they stomp each other's occlusion mask (dropped triangles).
	return get_core_num() & 1;
#else
	return 0;
#endif
}
}  // namespace

constexpr int kBitmapFontWidth = BitmapFont8x16::kWidth;
constexpr int kBitmapFontHeight = BitmapFont8x16::kHeight;
int gAntialiasSamples = 0;
// DIAG: temporarily default-on to measure, with GEA_EMBEDDED_TEXT_SPRITE_DIAG
// counters, whether the sprite cache engages on device for the FPS badge. Revert
// to false (opt-in via Display.setTextRasterCache) before shipping.
bool gTextRasterCacheEnabled = true;

// Pre-packed glyph stamping (opt-in via Display.setTextSolidBackdrop): the app
// declares that text draws over ONE solid color, so each glyph is pre-blended
// against that backdrop ONCE — natively in 4-bit — and stored as packed 4bpp
// rows in both x-parities. Drawing a glyph becomes row memcpys with at most two
// edge RMW bytes: no per-pixel destination read, no blend. Valid because glyph
// boxes don't overlap (no kerning). Glyphs whose color EQUALS the backdrop
// (e.g. white text on an inverted row) skip the stamp and take the true blend.
//
// Deliberately 4bpp-ONLY, not generalised with the rest of the packed path: the
// cache stores one pre-packed copy per possible x-phase, so a 2bpp target would
// need FOUR copies of every glyph instead of two — double the RAM of the 4bpp
// cache on the part that has the least of it (the C3 e-reader has no PSRAM,
// which is why it is 2bpp in the first place). GRAY2 takes the ordinary
// per-pixel blend below; setTextSolidBackdrop is a no-op there.
#if GEA_PIXEL_STORAGE_PACKED_BITS == 4
bool gTextStampEnabled = false;
std::uint8_t gTextStampBackdrop = 15;
#endif

namespace {
int nextUtf8Codepoint(const char *&p);  // defined below (same anon namespace); used by the text sprite cache
#if GEA_PIXEL_STORAGE_PACKED_BITS == 4
void glyphStampCacheReset();  // defined below (same anon namespace); used by setTextSolidBackdrop
#endif
}

// ---- Large-static-text sprite cache ----------------------------------------
//
// Big HUD/badge/clock text (e.g. a 26vmin FPS badge ≈ 106px tall) is *static*
// content — it changes a couple of times a second at most — yet the dirty-region
// replay re-rasterizes it on (nearly) every frame because moving foreground
// content keeps dirtying the chunks it sits in. Each re-raster walks the glyphs
// and, per pixel, does a scattered read into a large flash-resident font atlas
// (`RasterizedFont::coverage`), which thrashes the flash cache.
//
// This cache bakes a glyph-coverage sprite into a sequential heap buffer (PSRAM
// on ESP, where the global allocator is SPIRAM-preferred — kept board-independent
// by using plain malloc/realloc, no heap_caps here) once the same large text has
// been seen twice, then blits that sprite (sequential read + opaque-run fast
// fill) instead of re-running the rasterizer. Coverage is colour/position/
// global-alpha independent, so only the text content + font identity key it; the
// colour and pen position are applied at blit time. Small text (the common UI
// case) never enters the cache, so dynamic text pays nothing but a hash compare.
#ifndef GEA_EMBEDDED_TEXT_SPRITE_CACHE
#define GEA_EMBEDDED_TEXT_SPRITE_CACHE 1
#endif

#if GEA_EMBEDDED_TEXT_SPRITE_CACHE
namespace {

constexpr int kTextSpriteSlots = 4;
constexpr int kTextSpriteMaxLen = 63;
// Only cache genuinely large text. The per-frame re-raster of small UI text is
// cheap, and caching it would risk churn (alloc/blit overhead) on dynamic lists.
constexpr int kTextSpriteMinSizePx = 40;
// Skip caching anything whose sprite would be larger than this (a runaway title).
constexpr std::size_t kTextSpriteMaxBytes = 768u * 1024u;

struct TextSprite {
	enum State { Empty, Seen, Cached };
	char text[kTextSpriteMaxLen + 1] = {0};
	const RasterizedFontData *fontData = nullptr;
	int sizePx = -1;
	State state = Empty;
	int minLX = 0;
	int minLY = 0;
	int w = 0;
	int h = 0;
	std::uint8_t *cov = nullptr;     // w*h coverage bytes
	std::int16_t *rowX0 = nullptr;   // per-row ink start (w if empty)
	std::int16_t *rowX1 = nullptr;   // per-row ink end (-1 if empty)
	std::size_t covCap = 0;
	int extCap = 0;
	// Run-length-encoded coverage, derived from cov at build time. Each row r is
	// the runs [rleRowStart[r], rleRowStart[r+1]); each run is (rleVal, rleLen)
	// covering rleLen contiguous columns of value rleVal. Blitting from this reads
	// a few thousand run entries instead of W*H coverage bytes out of PSRAM (the
	// measured ~1ms/frame cost for a large badge), and zero-runs cost no per-pixel
	// work at all. rleRowStart==nullptr means "no RLE" → blit falls back to cov.
	std::uint8_t *rleVal = nullptr;
	std::uint16_t *rleLen = nullptr;
	std::int32_t *rleRowStart = nullptr;
	std::size_t rleCap = 0;   // capacity of rleVal/rleLen (in runs)
	int rleRowCap = 0;        // capacity of rleRowStart (in rows)
	bool rleValid = false;    // true only when the current sprite has a valid RLE
};

TextSprite gTextSprites[kTextSpriteSlots];

// Internal-SRAM staging for the sprite currently being blitted. The RLE blit walks
// rowX0/rowX1/rleRowStart/rleVal/rleLen several times per frame (once per DMA chunk
// the text spans under a fused chunked flush). Those arrays live in PSRAM, and the
// walk's short dependent reads ride a cache the frame's full-screen flush traffic
// keeps evicting — measured ~350ns per run entry on the ESP32-S3 amoled (TSDIAG:
// walk ≈ 200µs of a 474µs blit for a 375×81 badge, as slow as a full re-raster).
// Staging the on-screen sprite's arrays here makes the walk cache-cheap. Ownership
// follows the blit: the first blit of a non-owner sprite copies its arrays in
// (~8KB memcpy, once per text change), so a badge cycling a handful of cached
// strings always blits the hot copy. cov is not staged — the RLE path never reads
// it — and a sprite that doesn't fit simply keeps blitting from PSRAM.
#ifndef GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES
#define GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES (12 * 1024)
#endif
#if GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES > 0
struct HotTextSprite {
	const TextSprite *owner = nullptr;
	const std::int16_t *rowX0 = nullptr;
	const std::int16_t *rowX1 = nullptr;
	const std::int32_t *rleRowStart = nullptr;
	const std::uint8_t *rleVal = nullptr;
	const std::uint16_t *rleLen = nullptr;
	std::uint8_t *buf = nullptr;  // lazily allocated; null = no internal headroom
	bool allocFailed = false;
};
HotTextSprite gHotTextSprite;

void adoptHotTextSprite(const TextSprite &s)
{
	HotTextSprite &hot = gHotTextSprite;
	hot.owner = nullptr;
	if (!s.rleValid || !s.rleRowStart || s.h <= 0) return;
	if (!hot.buf) {
		if (hot.allocFailed) return;
#ifdef ESP_PLATFORM
		// MALLOC_CAP_8BIT is load-bearing: INTERNAL alone lets the allocator hand
		// back IRAM (its most-constrained matching heap) on the classic ESP32, and
		// IRAM only supports 32-bit access — the RLE blit's byte reads then panic
		// with LoadStoreError (crashed the M5Paper e-reader whenever a >=40px
		// cached sprite blitted through the hot stage). The S3, where this staging
		// was built, has byte-accessible IRAM and never showed it.
		hot.buf = static_cast<std::uint8_t *>(
				heap_caps_malloc(GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
		hot.buf = static_cast<std::uint8_t *>(std::malloc(GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES));
#endif
		if (!hot.buf) { hot.allocFailed = true; return; }
	}
	const std::size_t h = static_cast<std::size_t>(s.h);
	const std::size_t runs = static_cast<std::size_t>(s.rleRowStart[s.h]);
	// Packed in descending alignment: rleRowStart(4), rowX0/rowX1/rleLen(2), rleVal(1).
	const std::size_t needStart = sizeof(std::int32_t) * (h + 1);
	const std::size_t needX = sizeof(std::int16_t) * h;
	const std::size_t needLen = sizeof(std::uint16_t) * runs;
	if (needStart + needX * 2 + needLen + runs > static_cast<std::size_t>(GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES))
		return;
	std::uint8_t *p = hot.buf;
	auto place = [&p](const void *src, std::size_t bytes) -> const void * {
		std::memcpy(p, src, bytes);
		const void *out = p;
		p += bytes;
		return out;
	};
	hot.rleRowStart = static_cast<const std::int32_t *>(place(s.rleRowStart, needStart));
	hot.rowX0 = static_cast<const std::int16_t *>(place(s.rowX0, needX));
	hot.rowX1 = static_cast<const std::int16_t *>(place(s.rowX1, needX));
	hot.rleLen = static_cast<const std::uint16_t *>(place(s.rleLen, needLen));
	hot.rleVal = static_cast<const std::uint8_t *>(place(s.rleVal, runs));
	hot.owner = &s;
}
#endif  // GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES > 0

std::uint32_t hashTextContent(const char *s)
{
	std::uint32_t h = 2166136261u;  // FNV-1a
	for (; *s; ++s) {
		h ^= static_cast<std::uint8_t>(*s);
		h *= 16777619u;
	}
	return h;
}

// Rasterize `text` into `sprite`'s coverage buffer in text-relative coordinates
// (origin = pen position). Returns false (leaving the slot uncacheable) on an
// empty or too-large sprite, or on allocation failure.
bool buildTextSprite(TextSprite &sprite, const char *text, const RasterizedFont &font)
{
	const int lineH = font.lineHeight();
	const int ascender = font.ascender();
	const int fallbackAdvance = font.sizePx() / 2;

	// Pass 1: bbox of all glyph ink relative to the pen origin (0,0).
	int penX = 0, penY = 0;
	int minLX = 0x7fffffff, minLY = 0x7fffffff, maxLX = -0x7fffffff, maxLY = -0x7fffffff;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') { penX = 0; penY += lineH; continue; }
		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) { penX += fallbackAdvance; continue; }
		if (glyph.width > 0 && glyph.height > 0) {
			const int gx = penX + glyph.bearingX;
			const int gy = penY + ascender - glyph.bearingY;
			if (gx < minLX) minLX = gx;
			if (gy < minLY) minLY = gy;
			if (gx + glyph.width - 1 > maxLX) maxLX = gx + glyph.width - 1;
			if (gy + glyph.height - 1 > maxLY) maxLY = gy + glyph.height - 1;
		}
		penX += glyph.advance;
	}
	if (maxLX < minLX || maxLY < minLY) return false;  // no ink

	const int W = maxLX - minLX + 1;
	const int H = maxLY - minLY + 1;
	const std::size_t need = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
	if (need > kTextSpriteMaxBytes) return false;

	if (need > sprite.covCap) {
		std::uint8_t *grown = static_cast<std::uint8_t *>(std::realloc(sprite.cov, need));
		if (!grown) return false;
		sprite.cov = grown;
		sprite.covCap = need;
	}
	if (H > sprite.extCap) {
		std::int16_t *gx0 = static_cast<std::int16_t *>(std::realloc(sprite.rowX0, sizeof(std::int16_t) * H));
		std::int16_t *gx1 = static_cast<std::int16_t *>(std::realloc(sprite.rowX1, sizeof(std::int16_t) * H));
		if (!gx0 || !gx1) {
			// Keep whichever grew; capacity tracks the smaller so we retry next time.
			if (gx0) sprite.rowX0 = gx0;
			if (gx1) sprite.rowX1 = gx1;
			return false;
		}
		sprite.rowX0 = gx0;
		sprite.rowX1 = gx1;
		sprite.extCap = H;
	}

	std::memset(sprite.cov, 0, need);
	for (int r = 0; r < H; r++) {
		sprite.rowX0[r] = static_cast<std::int16_t>(W);
		sprite.rowX1[r] = -1;
	}

	// Pass 2: write coverage + per-row ink extents.
	penX = 0; penY = 0;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') { penX = 0; penY += lineH; continue; }
		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) { penX += fallbackAdvance; continue; }
		const int gx = penX + glyph.bearingX;
		const int gy = penY + ascender - glyph.bearingY;
		for (int row = 0; row < glyph.height; row++) {
			const int by = gy + row - minLY;
			if (by < 0 || by >= H) continue;
			std::uint8_t *covRow = sprite.cov + static_cast<std::size_t>(by) * W;
			for (int col = 0; col < glyph.width; col++) {
				const int bx = gx + col - minLX;
				if (bx < 0 || bx >= W) continue;
				const std::uint8_t a = font.coverage(glyph, row, col);
				if (a == 0) continue;
				if (a > covRow[bx]) covRow[bx] = a;
				if (bx < sprite.rowX0[by]) sprite.rowX0[by] = static_cast<std::int16_t>(bx);
				if (bx > sprite.rowX1[by]) sprite.rowX1[by] = static_cast<std::int16_t>(bx);
			}
		}
		penX += glyph.advance;
	}

	sprite.minLX = minLX;
	sprite.minLY = minLY;
	sprite.w = W;
	sprite.h = H;

	// Pass 3: run-length-encode each row of cov over [0,W). The blit reads these
	// runs instead of W*H PSRAM bytes. Count first, then size the buffers exactly.
	sprite.rleValid = false;
	std::size_t runCount = 0;
	for (int r = 0; r < H; r++) {
		const std::uint8_t *covRow = sprite.cov + static_cast<std::size_t>(r) * W;
		int c = 0;
		while (c < W) {
			const std::uint8_t v = covRow[c];
			while (c < W && covRow[c] == v) c++;
			runCount++;
		}
	}
	bool rleOk = runCount > 0;
	if (rleOk && runCount > sprite.rleCap) {
		std::uint8_t *nv = static_cast<std::uint8_t *>(std::realloc(sprite.rleVal, runCount));
		std::uint16_t *nl = static_cast<std::uint16_t *>(std::realloc(sprite.rleLen, sizeof(std::uint16_t) * runCount));
		if (nv) sprite.rleVal = nv;
		if (nl) sprite.rleLen = nl;
		if (nv && nl) sprite.rleCap = runCount; else rleOk = false;
	}
	if (rleOk && (H + 1) > sprite.rleRowCap) {
		std::int32_t *nr = static_cast<std::int32_t *>(std::realloc(sprite.rleRowStart, sizeof(std::int32_t) * (H + 1)));
		if (nr) { sprite.rleRowStart = nr; sprite.rleRowCap = H + 1; } else rleOk = false;
	}
	if (rleOk) {
		std::size_t run = 0;
		for (int r = 0; r < H; r++) {
			sprite.rleRowStart[r] = static_cast<std::int32_t>(run);
			const std::uint8_t *covRow = sprite.cov + static_cast<std::size_t>(r) * W;
			int c = 0;
			while (c < W) {
				const std::uint8_t v = covRow[c];
				const int s = c;
				while (c < W && covRow[c] == v) c++;
				sprite.rleVal[run] = v;
				sprite.rleLen[run] = static_cast<std::uint16_t>(c - s);
				run++;
			}
		}
		sprite.rleRowStart[H] = static_cast<std::int32_t>(run);
		sprite.rleValid = true;
#ifndef ESP_PLATFORM
		// Host builds (native tests / WASM simulator): verify the RLE decodes back
		// to cov exactly, so a pixel-correctness regression is caught off-device.
		for (int r = 0; r < H; r++) {
			const std::uint8_t *covRow = sprite.cov + static_cast<std::size_t>(r) * W;
			int c = 0;
			for (std::int32_t k = sprite.rleRowStart[r]; k < sprite.rleRowStart[r + 1]; k++) {
				for (int n = 0; n < sprite.rleLen[k]; n++) {
					if (covRow[c] != sprite.rleVal[k]) { std::abort(); }
					c++;
				}
			}
			if (c != W) { std::abort(); }
		}
#endif
	}
	return true;
}

}  // namespace
#endif  // GEA_EMBEDDED_TEXT_SPRITE_CACHE

static inline int normalizeAntialiasSamples(int samples)
{
	if (samples >= 4) return 4;
	if (samples >= 2) return 2;
	return 0;
}

static inline float antialiasKernelWidth(int samples)
{
	(void)samples;
	return 1.0f;
}

static inline float roundedRectAntialiasKernelWidth(int samples, int w, int h)
{
	if (samples < 4) return 1.0f;
	return std::min(w, h) >= 64 ? 2.0f : 1.0f;
}

static inline bool roundedRectIsCircleLike(int w, int h, int tl, int tr, int br, int bl)
{
	const int minSide = std::min(w, h);
	if (minSide < 16 || std::abs(w - h) > 1) return false;
	const int halfMin = minSide / 2;
	const int minRadius = std::max(1, halfMin - 1);
	return tl >= minRadius && tr >= minRadius && br >= minRadius && bl >= minRadius;
}

static inline float roundedRectAntialiasKernelWidth(int samples, int w, int h, int tl, int tr, int br, int bl)
{
	if (samples < 2) return 1.0f;
	if (roundedRectIsCircleLike(w, h, tl, tr, br, bl)) {
		return 1.0f;
	}
	if (samples < 4) return 1.0f;
	return roundedRectAntialiasKernelWidth(samples, w, h);
}

static inline bool roundedRectShouldCoverageSampleFill(int w, int h)
{
	return w > 0 && h > 0 && w * h <= 4096;
}

static inline float antialiasOffsetWithKernel(int sample, int samples, float width)
{
	return (1.0f - width) * 0.5f +
	       (static_cast<float>(sample) + 0.5f) * width / static_cast<float>(samples);
}

static inline float antialiasOffset(int sample, int samples)
{
	return antialiasOffsetWithKernel(sample, samples, antialiasKernelWidth(samples));
}

static inline int antialiasEdgePad(float kernelWidth)
{
	if (kernelWidth <= 1.0f) return 1;
	return static_cast<int>(std::ceil(kernelWidth * 0.5f)) + 1;
}

void Canvas::setAntialiasSamples(int samples)
{
	gAntialiasSamples = normalizeAntialiasSamples(samples);
}

int Canvas::antialiasSamples()
{
	return gAntialiasSamples;
}

void Canvas::setTextRasterCacheEnabled(bool enabled)
{
	gTextRasterCacheEnabled = enabled;
}

bool Canvas::textRasterCacheEnabled()
{
	return gTextRasterCacheEnabled;
}

void Canvas::setTextSolidBackdrop(int rrggbb)
{
#if GEA_PIXEL_STORAGE_PACKED_BITS == 4
	if (rrggbb < 0) {
		gTextStampEnabled = false;
		return;
	}
	const std::uint8_t level = static_cast<std::uint8_t>(
			pixel::nativeColor((rrggbb >> 16) & 0xFF, (rrggbb >> 8) & 0xFF, rrggbb & 0xFF) & 0x0F);
	if (gTextStampEnabled && level == gTextStampBackdrop) return;
	gTextStampBackdrop = level;
	gTextStampEnabled = true;
	// A backdrop change invalidates every pre-blended glyph.
	glyphStampCacheReset();
#else
	(void)rrggbb;
#endif
}

static inline bool roundedRectSampleContains(int x,
                                             int y,
                                             int w,
                                             int h,
                                             int tl,
                                             int tr,
                                             int br,
                                             int bl,
                                             float sx,
                                             float sy)
{
	const float left = static_cast<float>(x);
	const float top = static_cast<float>(y);
	const float right = left + static_cast<float>(w);
	const float bottom = top + static_cast<float>(h);
	if (sx < left || sy < top || sx >= right || sy >= bottom) return false;

	auto cornerContains = [](float cx, float cy, float r, float px, float py) {
		if (r <= 0.0f) return true;
		const float dx = (px - cx) / r;
		const float dy = (py - cy) / r;
		return dx * dx + dy * dy <= 1.0f;
	};

	if (tl > 0 && sx < left + static_cast<float>(tl) && sy < top + static_cast<float>(tl))
		return cornerContains(left + static_cast<float>(tl), top + static_cast<float>(tl), static_cast<float>(tl), sx, sy);
	if (tr > 0 && sx >= right - static_cast<float>(tr) && sy < top + static_cast<float>(tr))
		return cornerContains(right - static_cast<float>(tr), top + static_cast<float>(tr), static_cast<float>(tr), sx, sy);
	if (br > 0 && sx >= right - static_cast<float>(br) && sy >= bottom - static_cast<float>(br))
		return cornerContains(right - static_cast<float>(br), bottom - static_cast<float>(br), static_cast<float>(br), sx, sy);
	if (bl > 0 && sx < left + static_cast<float>(bl) && sy >= bottom - static_cast<float>(bl))
		return cornerContains(left + static_cast<float>(bl), bottom - static_cast<float>(bl), static_cast<float>(bl), sx, sy);
	return true;
}

static inline int roundedRectCoverage(int x,
                                      int y,
                                      int w,
                                      int h,
                                      int tl,
                                      int tr,
                                      int br,
                                      int bl,
                                      int px,
                                      int py,
                                      int samples,
                                      float kernelWidth)
{
	int coverage = 0;
	for (int iy = 0; iy < samples; ++iy) {
		const float oy = antialiasOffsetWithKernel(iy, samples, kernelWidth);
		for (int ix = 0; ix < samples; ++ix) {
			const float ox = antialiasOffsetWithKernel(ix, samples, kernelWidth);
			if (roundedRectSampleContains(x, y, w, h, tl, tr, br, bl,
			                              static_cast<float>(px) + ox,
			                              static_cast<float>(py) + oy))
				++coverage;
		}
	}
	return coverage;
}

static inline int roundedRectStrokeCoverage(int x,
                                            int y,
                                            int w,
                                            int h,
                                            int tl,
                                            int tr,
                                            int br,
                                            int bl,
                                            int lineWidth,
                                            int px,
                                            int py,
                                            int samples,
                                            float kernelWidth)
{
	const int innerX = x + lineWidth;
	const int innerY = y + lineWidth;
	const int innerW = w - lineWidth * 2;
	const int innerH = h - lineWidth * 2;
	const int innerTl = std::max(0, tl - lineWidth);
	const int innerTr = std::max(0, tr - lineWidth);
	const int innerBr = std::max(0, br - lineWidth);
	const int innerBl = std::max(0, bl - lineWidth);
	const bool hasInner = innerW > 0 && innerH > 0;
	int coverage = 0;
	for (int iy = 0; iy < samples; ++iy) {
		const float oy = antialiasOffsetWithKernel(iy, samples, kernelWidth);
		for (int ix = 0; ix < samples; ++ix) {
			const float ox = antialiasOffsetWithKernel(ix, samples, kernelWidth);
			const float sx = static_cast<float>(px) + ox;
			const float sy = static_cast<float>(py) + oy;
			if (!roundedRectSampleContains(x, y, w, h, tl, tr, br, bl, sx, sy)) continue;
			if (hasInner &&
			    roundedRectSampleContains(innerX, innerY, innerW, innerH,
			                              innerTl, innerTr, innerBr, innerBl,
			                              sx, sy))
				continue;
			++coverage;
		}
	}
	return coverage;
}

static inline int coverageAlpha(int alpha, int coverage, int sampleCount)
{
	if (alpha <= 0 || coverage <= 0) return 0;
	if (coverage >= sampleCount) return alpha;
	return (alpha * coverage + sampleCount / 2) / sampleCount;
}

static inline void addUnclippedSpan(int left, int right, int *x0, int *x1)
{
	if (left > right) return;
	if (left < *x0) *x0 = left;
	if (right > *x1) *x1 = right;
}

static inline void roundedRectAnyCoverageSpan(int x,
                                              int y,
                                              int w,
                                              int h,
                                              int tl,
                                              int tr,
                                              int br,
                                              int bl,
                                              int py,
                                              int samples,
                                              float kernelWidth,
                                              int *spanX0,
                                              int *spanX1)
{
	const int topR = tl > tr ? tl : tr;
	const int bottomR = bl > br ? bl : br;
	const float minSample = antialiasOffsetWithKernel(0, samples, kernelWidth);
	const float maxSample = antialiasOffsetWithKernel(samples - 1, samples, kernelWidth);
	const float rowMin = static_cast<float>(py) + minSample;
	const float rowMax = static_cast<float>(py) + maxSample;
	const float left = static_cast<float>(x);
	const float top = static_cast<float>(y);
	const float right = left + static_cast<float>(w);
	const float bottom = top + static_cast<float>(h);

	if (rowMax < top || rowMin >= bottom) return;

	auto addRectSpan = [&](int sx0, int sx1, float sy0, float sy1) {
		if (rowMax < sy0 || rowMin >= sy1) return;
		addUnclippedSpan(sx0, sx1, spanX0, spanX1);
	};
	addRectSpan(x, x + w - 1, top + static_cast<float>(topR), bottom - static_cast<float>(bottomR));
	addRectSpan(x + tl, x + w - tr - 1, top, top + static_cast<float>(topR));
	addRectSpan(x + bl, x + w - br - 1, bottom - static_cast<float>(bottomR), bottom);
	if (tl > 0 && tl < topR) addRectSpan(x, x + tl - 1, top + static_cast<float>(tl), top + static_cast<float>(topR));
	if (tr > 0 && tr < topR) addRectSpan(x + w - tr, x + w - 1, top + static_cast<float>(tr), top + static_cast<float>(topR));
	if (bl > 0 && bl < bottomR) addRectSpan(x, x + bl - 1, bottom - static_cast<float>(bottomR), bottom - static_cast<float>(bl));
	if (br > 0 && br < bottomR) addRectSpan(x + w - br, x + w - 1, bottom - static_cast<float>(bottomR), bottom - static_cast<float>(br));

	auto addCornerSpan = [&](int radius, float cx, float cy, bool leftCorner, bool topCorner) {
		if (radius <= 0) return;
		const float r = static_cast<float>(radius);
		if (topCorner) {
			if (rowMax < cy - r || rowMin >= cy) return;
		} else {
			if (rowMax < cy || rowMin >= cy + r) return;
		}
		float sampleY = cy;
		if (sampleY < rowMin) sampleY = rowMin;
		if (sampleY > rowMax) sampleY = rowMax;
		const float dy = sampleY - cy;
		const float dx2 = r * r - dy * dy;
		if (dx2 < 0.0f) return;
		const float dx = std::sqrt(dx2);
		if (leftCorner) {
			const int sx0 = static_cast<int>(std::ceil(cx - dx - maxSample));
			const int sx1 = static_cast<int>(std::floor(cx - minSample));
			addUnclippedSpan(sx0, sx1, spanX0, spanX1);
		} else {
			const int sx0 = static_cast<int>(std::ceil(cx - maxSample));
			const int sx1 = static_cast<int>(std::floor(cx + dx - minSample));
			addUnclippedSpan(sx0, sx1, spanX0, spanX1);
		}
	};

	addCornerSpan(tl, left + static_cast<float>(tl), top + static_cast<float>(tl), true, true);
	addCornerSpan(tr, right - static_cast<float>(tr), top + static_cast<float>(tr), false, true);
	addCornerSpan(bl, left + static_cast<float>(bl), bottom - static_cast<float>(bl), true, false);
	addCornerSpan(br, right - static_cast<float>(br), bottom - static_cast<float>(br), false, false);
}

namespace {

int nextUtf8Codepoint(const char *&p)
{
	const unsigned char c0 = static_cast<unsigned char>(*p++);
	if (c0 < 0x80) return c0;

	const auto isContinuation = [](unsigned char c) {
		return (c & 0xc0) == 0x80;
	};
	if ((c0 & 0xe0) == 0xc0) {
		const unsigned char c1 = static_cast<unsigned char>(*p);
		if (c1 && isContinuation(c1)) {
			++p;
			return ((c0 & 0x1f) << 6) | (c1 & 0x3f);
		}
		return c0;
	}
	if ((c0 & 0xf0) == 0xe0) {
		const unsigned char c1 = static_cast<unsigned char>(p[0]);
		const unsigned char c2 = static_cast<unsigned char>(p[1]);
		if (c1 && c2 && isContinuation(c1) && isContinuation(c2)) {
			p += 2;
			return ((c0 & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (c2 & 0x3f);
		}
		return c0;
	}
	if ((c0 & 0xf8) == 0xf0) {
		const unsigned char c1 = static_cast<unsigned char>(p[0]);
		const unsigned char c2 = static_cast<unsigned char>(p[1]);
		const unsigned char c3 = static_cast<unsigned char>(p[2]);
		if (c1 && c2 && c3 && isContinuation(c1) && isContinuation(c2) && isContinuation(c3)) {
			p += 3;
			return ((c0 & 0x07) << 18) | ((c1 & 0x3f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
		}
	}
	return c0;
}

}  // namespace

#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
namespace {

struct SolidGlyphRow {
	int row = 0;
	int start = 0;
	int width = 0;
	int offset = 0;
};

struct SolidGlyphCacheEntry {
	const RasterizedFontData *font = nullptr;
	pixel::native_t color = 0;
	pixel::native_t background = 0;
	int codepoint = 0;
	Glyph glyph{};
	std::vector<SolidGlyphRow> rows;
	std::vector<pixel::native_t> pixels;
	std::uint32_t lastUsed = 0;
};

constexpr int kSolidGlyphCacheSize = 48;
SolidGlyphCacheEntry gSolidGlyphCache[kSolidGlyphCacheSize];
std::uint32_t gSolidGlyphClock = 1;

SolidGlyphCacheEntry *solidGlyphCacheEntry(const RasterizedFont &font,
                                           pixel::native_t color,
                                           pixel::native_t background,
                                           int codepoint)
{
	const RasterizedFontData *data = font.data();
	if (!data) return nullptr;

	for (auto &entry : gSolidGlyphCache) {
		if (entry.font == data && entry.color == color && entry.background == background &&
		    entry.codepoint == codepoint) {
			entry.lastUsed = ++gSolidGlyphClock;
			return &entry;
		}
	}

	Glyph glyph{};
	if (!font.glyph(codepoint, &glyph)) return nullptr;

	SolidGlyphCacheEntry *victim = &gSolidGlyphCache[0];
	for (auto &entry : gSolidGlyphCache) {
		if (!entry.font) {
			victim = &entry;
			break;
		}
		if (entry.lastUsed < victim->lastUsed) victim = &entry;
	}

	victim->font = data;
	victim->color = color;
	victim->background = background;
	victim->codepoint = codepoint;
	victim->glyph = glyph;
	victim->rows.clear();
	victim->pixels.clear();
	victim->rows.reserve(static_cast<std::size_t>(std::max(0, glyph.height)));

	for (int row = 0; row < glyph.height; row++) {
		int first = glyph.width;
		int last = -1;
		for (int col = 0; col < glyph.width; col++) {
			if (font.coverage(glyph, row, col) == 0) continue;
			if (col < first) first = col;
			last = col;
		}
		if (last < first) continue;

		SolidGlyphRow span{};
		span.row = row;
		span.start = first;
		span.width = last - first + 1;
		span.offset = static_cast<int>(victim->pixels.size());
		victim->rows.push_back(span);

		for (int col = first; col <= last; col++) {
			const int alpha = font.coverage(glyph, row, col);
			if (alpha <= 0) {
				victim->pixels.push_back(background);
			} else if (alpha >= 255) {
				victim->pixels.push_back(color);
			} else {
				victim->pixels.push_back(pixel::blendNative(color, background, alpha));
			}
		}
	}

	victim->lastUsed = ++gSolidGlyphClock;
	return victim;
}

}  // namespace
#endif

class CanvasMath {
public:
#ifndef GEA_EMBEDDED_CANVAS_CIRCLE_RADIUS_MAX
#define GEA_EMBEDDED_CANVAS_CIRCLE_RADIUS_MAX 63
#endif
#ifndef GEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_MAX
#define GEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_MAX 128
#endif
#ifndef GEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_SLOTS
#define GEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_SLOTS 16
#endif

	static constexpr int kCircleSpanMax = 32;
	static constexpr int kCircleSpanSlotCount = (kCircleSpanMax / 2) + 1;
	static constexpr int kCircleRadiusMax = GEA_EMBEDDED_CANVAS_CIRCLE_RADIUS_MAX;
	static constexpr int kCircleRadiusSpanRows = (kCircleRadiusMax * 2) + 1;
	static constexpr int kCircleBoxSpanMax = GEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_MAX;
	static constexpr int kCircleBoxSpanSlots = GEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_SLOTS;
	static_assert(kCircleRadiusMax > 0 && kCircleRadiusMax < 64);
	static_assert(kCircleBoxSpanMax > 0 && kCircleBoxSpanMax <= 255);
	static_assert(kCircleBoxSpanSlots > 0);

	static int min(int a, int b) { return a < b ? a : b; }

	static int integerSqrt(int n)
	{
		if (n <= 0) return 0;
		int x = n;
		int y = (x + 1) / 2;
		while (y < x) {
			x = y;
			y = (x + n / x) / 2;
		}
		return x;
	}

	const std::uint8_t (*circleSpans(int size))[2]
	{
		if (size <= 0 || size > kCircleSpanMax || (size & 1)) return nullptr;
		int slot = size / 2;
		std::uint32_t bit = 1u << slot;
		if (!(circleSpanCacheReady_ & bit)) {
			int radius2 = size * size;
			for (int row = 0; row < size; row++) {
				int dy2 = row * 2 + 1 - size;
				int dx2 = integerSqrt(radius2 - dy2 * dy2);
				circleSpanCache_[slot][row][0] = (std::uint8_t)((size - dx2) / 2);
				circleSpanCache_[slot][row][1] = (std::uint8_t)((size + dx2 - 1) / 2);
			}
			circleSpanCacheReady_ |= bit;
		}
		return circleSpanCache_[slot];
	}

	const std::uint8_t (*circleBoxSpans(int size))[2]
	{
		if (size <= 0 || size > kCircleBoxSpanMax) return nullptr;
		for (int slot = 0; slot < kCircleBoxSpanSlots; ++slot) {
			if (circleBoxSpanSize_[slot] == size)
				return circleBoxSpanCache_[slot];
		}
		const int slot = circleBoxSpanNext_;
		circleBoxSpanNext_ = (circleBoxSpanNext_ + 1) % kCircleBoxSpanSlots;
		const long long ss = static_cast<long long>(size) * static_cast<long long>(size);
		for (int row = 0; row < size; row++) {
			const long long dy2 = static_cast<long long>(row * 2 + 1 - size);
			long long inside = ss - dy2 * dy2;
			if (inside < 0) inside = 0;
			const int dx2 = integerSqrt(static_cast<int>(inside));
			int sx0 = (size - dx2) / 2;
			int sx1 = (size + dx2 - 1) / 2;
			if (sx0 < 0) sx0 = 0;
			if (sx1 >= size) sx1 = size - 1;
			circleBoxSpanCache_[slot][row][0] = static_cast<std::uint8_t>(sx0);
			circleBoxSpanCache_[slot][row][1] = static_cast<std::uint8_t>(sx1);
		}
		circleBoxSpanSize_[slot] = size;
		return circleBoxSpanCache_[slot];
	}

	const std::uint8_t (*circleRadiusSpans(int radius))[2]
	{
		if (radius <= 0 || radius > kCircleRadiusMax) return nullptr;
		std::uint64_t bit = 1ull << radius;
		if (!(circleRadiusSpanCacheReady_ & bit)) {
			const int diameter = radius * 2 + 1;
			const int rr = radius * radius;
			for (int row = 0; row < diameter; row++) {
				const int dy = row - radius;
				const int dx = integerSqrt(rr - dy * dy);
				circleRadiusSpanCache_[radius][row][0] = (std::uint8_t)(radius - dx);
				circleRadiusSpanCache_[radius][row][1] = (std::uint8_t)(radius + dx);
			}
			circleRadiusSpanCacheReady_ |= bit;
		}
		return circleRadiusSpanCache_[radius];
	}

	static CanvasMath &instance()
	{
		static CanvasMath math;
		return math;
	}

private:
	std::uint8_t circleSpanCache_[kCircleSpanSlotCount][kCircleSpanMax][2]{};
	std::uint32_t circleSpanCacheReady_ = 0;
	std::uint8_t circleRadiusSpanCache_[kCircleRadiusMax + 1][kCircleRadiusSpanRows][2]{};
	std::uint64_t circleRadiusSpanCacheReady_ = 0;
	std::uint8_t circleBoxSpanCache_[kCircleBoxSpanSlots][kCircleBoxSpanMax][2]{};
	std::uint8_t circleBoxSpanSize_[kCircleBoxSpanSlots]{};
	int circleBoxSpanNext_ = 0;
};

pixel::native_t Canvas::readPixelNative(int x, int y) const
{
	if (!pixels_ || x < 0 || x >= width_ || y < 0 || y >= height_) return 0;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) return rotGet(x, y);
#endif
#if GEA_PIXEL_STORAGE_PACKED
	return pixel::packed::get(packedRow(rowToPhysical(y)), x);
#else
	return pixels_[rowToPhysical(y) * stride_ + x];
#endif
}

void Canvas::writePixelNativeExact(int x, int y, pixel::native_t value)
{
	if (!pixels_ || x < 0 || x >= width_ || y < 0 || y >= height_) return;
	if (x < clipStack_[clipDepth_].x0 || x > clipStack_[clipDepth_].x1 ||
	    y < clipStack_[clipDepth_].y0 || y > clipStack_[clipDepth_].y1)
		return;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) {
		rotSet(x, y, value);
		markDirty(x, y, x, y);
		return;
	}
#endif
#if GEA_PIXEL_STORAGE_PACKED
	pixel::packed::set(packedRow(rowToPhysical(y)), x, value);
#else
	pixels_[rowToPhysical(y) * stride_ + x] = value;
#endif
	markDirty(x, y, x, y);
}

void Canvas::writePixelNative(int x, int y, pixel::native_t c)
{
	if (!pixels_) return;
	if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
	if (x < clipStack_[clipDepth_].x0 || x > clipStack_[clipDepth_].x1 ||
	    y < clipStack_[clipDepth_].y0 || y > clipStack_[clipDepth_].y1)
		return;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) {
		if (globalAlpha_ == 255) rotSet(x, y, c);
		else if (globalAlpha_ != 0) rotBlend(x, y, c, globalAlpha_);
		return;
	}
#endif
#if GEA_PIXEL_STORAGE_PACKED
	std::uint8_t *row = packedRow(rowToPhysical(y));
	if (globalAlpha_ == 255)
		pixel::packed::set(row, x, c);
	else if (globalAlpha_ != 0)
		pixel::packed::blendPixel(row, x, c, globalAlpha_);
#else
	pixel::native_t *p = &pixels_[rowToPhysical(y) * stride_ + x];
	if (globalAlpha_ == 255) {
		*p = c;
	} else if (globalAlpha_ != 0) {
		*p = pixel::blendNative(c, *p, globalAlpha_);
	}
#endif
}

void Canvas::writePixelNativeAlpha(int x, int y, pixel::native_t c, std::uint8_t alpha)
{
	if (!pixels_) return;
	if (alpha == 0 || globalAlpha_ == 0) return;
	if (alpha == 255) {
		writePixelNative(x, y, c);
		return;
	}
	if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
	if (x < clipStack_[clipDepth_].x0 || x > clipStack_[clipDepth_].x1 ||
	    y < clipStack_[clipDepth_].y0 || y > clipStack_[clipDepth_].y1)
		return;
	int a = globalAlpha_ == 255 ? alpha : (alpha * globalAlpha_) / 255;
	if (a <= 0) return;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) {
		if (a >= 255) rotSet(x, y, c);
		else rotBlend(x, y, c, a);
		return;
	}
#endif
#if GEA_PIXEL_STORAGE_PACKED
	pixel::packed::blendPixel(packedRow(rowToPhysical(y)), x, c, a);
#else
	pixel::native_t *p = &pixels_[rowToPhysical(y) * stride_ + x];
	*p = (a >= 255) ? c : pixel::blendNative(c, *p, a);
#endif
}

void Canvas::writePixel(int x, int y, pixel::native_t fg)
{
	writePixelNative(x, y, fg);
}

void Canvas::writePixelAlpha(int x, int y, pixel::native_t fg, std::uint8_t alpha)
{
	writePixelNativeAlpha(x, y, fg, alpha);
}

// Fill a horizontal span at physical row `phys` with a colour (converted once
// to the native pixel). Native fill — no per-pixel conversion, no branch.
void Canvas::fillSpanColor(int phys, int x0, int count, pixel::native_t color)
{
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	// Rotated: `phys` is the logical row == physical COLUMN; the logical horizontal
	// span [x0, x0+count) becomes a physical vertical run (one write per row).
	if (landscapeRot_) {
		const int ly = phys;
		for (int i = 0; i < count; ++i) rotSet(x0 + i, ly, color);
		return;
	}
#endif
#if GEA_PIXEL_STORAGE_PACKED
	pixel::packed::fillSpan(packedRow(phys), x0, count, color);
#else
	pixel::fillNative(&pixels_[phys * stride_ + x0], count, color);
#endif
}

void Canvas::fillSpanGlobalAlpha(int phys, int x0, int count, pixel::native_t color)
{
	if (count <= 0) return;
	if (globalAlpha_ == 255) {
		fillSpanColor(phys, x0, count, color);
		return;
	}
	if (globalAlpha_ == 0) return;
	const int a = globalAlpha_;
	const pixel::native_t c = color;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) {
		const int ly = phys;
		for (int i = 0; i < count; i++) rotBlend(x0 + i, ly, c, a);
		return;
	}
#endif
#if GEA_PIXEL_STORAGE_PACKED
	std::uint8_t *row = packedRow(phys);
	for (int i = 0; i < count; i++) pixel::packed::blendPixel(row, x0 + i, c, a);
#else
	pixel::native_t *dst = &pixels_[phys * stride_ + x0];
	for (int i = 0; i < count; i++) dst[i] = pixel::blendNative(c, dst[i], a);
#endif
}

void Canvas::bindPixels(pixel::native_t *pixels, int width, int height)
{
	bindPixels(pixels, width, height, width);
}

void Canvas::bindPixels(pixel::native_t *pixels, int width, int height, int stridePixels)
{
	width_ = width;
	height_ = height;
	stride_ = stridePixels > 0 ? stridePixels : width;
#if GEA_PIXEL_STORAGE_PACKED
	// Framebuffer packs kPxPerByte pixels per byte (2 on GRAY4, 4 on GRAY2), so a
	// physical row is that many bytes, rounded up for a partial trailing byte.
	strideBytes_ = stride_ > 0 ? pixel::packed::rowBytes(stride_) : 0;
#else
	strideBytes_ = stride_ > 0 ? stride_ * (int)sizeof(pixel::native_t) : 0;
#endif
	regionY_ = 0;
	regionH_ = 0;
	scrollOffsetY_ = 0;
	pixels_ = pixels;
	globalAlpha_ = 255;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	landscapeRot_ = false;
#endif
	resetDirty();
	resetClip();
}

#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
void Canvas::bindPixelsRotatedLandscape(pixel::native_t *physPixels, int logicalWidth, int logicalHeight)
{
	// Present a LOGICAL portrait coordinate system (width_ x height_) while the
	// backing store is the PHYSICAL landscape buffer (logicalHeight x logicalWidth).
	// strideBytes_ is the PHYSICAL row stride (rowBytes(physicalWidth) =
	// rowBytes(logicalHeight)); packedRow(physRow) then steps whole physical rows
	// and rotSet/rotBlend/rotGet index the physical column. See the transform note
	// on the helpers in canvas.h.
	width_ = logicalWidth;
	height_ = logicalHeight;
	stride_ = logicalHeight;
#if GEA_PIXEL_STORAGE_PACKED
	strideBytes_ = logicalHeight > 0 ? pixel::packed::rowBytes(logicalHeight) : 0;
#else
	strideBytes_ = logicalHeight > 0 ? logicalHeight * static_cast<int>(sizeof(pixel::native_t)) : 0;
#endif
	regionY_ = 0;
	regionH_ = 0;
	scrollOffsetY_ = 0;
	pixels_ = physPixels;
	globalAlpha_ = 255;
	landscapeRot_ = true;
	resetDirty();
	resetClip();
}
#endif

void Canvas::setScrollRegion(int regionY, int regionH, int scrollOffsetY)
{
	if (regionH <= 0 || height_ <= 0) {
		regionY_ = 0;
		regionH_ = 0;
		scrollOffsetY_ = 0;
		return;
	}
	regionY_ = regionY;
	regionH_ = regionH;
	int o = scrollOffsetY % regionH;
	if (o < 0) o += regionH;
	scrollOffsetY_ = o;
}

pixel::native_t *Canvas::pixels() { return pixels_; }
const pixel::native_t *Canvas::pixels() const { return pixels_; }
int Canvas::width() const { return width_; }
int Canvas::height() const { return height_; }
int Canvas::strideBytes() const { return strideBytes_; }

void Canvas::resetDirty()
{
	dirtyX0_ = width_;
	dirtyY0_ = height_;
	dirtyX1_ = -1;
	dirtyY1_ = -1;
	dirtyRectCount_ = 0;
}

void Canvas::addDirtyRect(int x0, int y0, int x1, int y1)
{
	if (x0 > x1 || y0 > y1) return;
	CanvasDirtyRect rect{x0, y0, x1, y1};

	for (int i = 0; i < dirtyRectCount_; i++) {
		CanvasDirtyRect &existing = dirtyRects_[i];
		if (existing.x0 <= rect.x1 + 1 && existing.x1 + 1 >= rect.x0 &&
		    existing.y0 <= rect.y1 + 1 && existing.y1 + 1 >= rect.y0) {
			if (rect.x0 < existing.x0) existing.x0 = rect.x0;
			if (rect.y0 < existing.y0) existing.y0 = rect.y0;
			if (rect.x1 > existing.x1) existing.x1 = rect.x1;
			if (rect.y1 > existing.y1) existing.y1 = rect.y1;
			return;
		}
	}

	if (dirtyRectCount_ < kMaxDirtyRects) {
		dirtyRects_[dirtyRectCount_++] = rect;
		return;
	}

	int best = 0;
	int bestCost = 0x7fffffff;
	for (int i = 0; i < dirtyRectCount_; i++) {
		const CanvasDirtyRect &existing = dirtyRects_[i];
		const int ux0 = existing.x0 < rect.x0 ? existing.x0 : rect.x0;
		const int uy0 = existing.y0 < rect.y0 ? existing.y0 : rect.y0;
		const int ux1 = existing.x1 > rect.x1 ? existing.x1 : rect.x1;
		const int uy1 = existing.y1 > rect.y1 ? existing.y1 : rect.y1;
		const int existingArea = (existing.x1 - existing.x0 + 1) * (existing.y1 - existing.y0 + 1);
		const int mergedArea = (ux1 - ux0 + 1) * (uy1 - uy0 + 1);
		const int cost = mergedArea - existingArea;
		if (cost < bestCost) {
			bestCost = cost;
			best = i;
		}
	}

	CanvasDirtyRect &existing = dirtyRects_[best];
	if (rect.x0 < existing.x0) existing.x0 = rect.x0;
	if (rect.y0 < existing.y0) existing.y0 = rect.y0;
	if (rect.x1 > existing.x1) existing.x1 = rect.x1;
	if (rect.y1 > existing.y1) existing.y1 = rect.y1;
}

void Canvas::markDirty(int x0, int y0, int x1, int y1)
{
	if (width_ <= 0 || height_ <= 0) return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width_) x1 = width_ - 1;
	if (y1 >= height_) y1 = height_ - 1;
	if (x0 > x1 || y0 > y1) return;
	if (x0 < dirtyX0_) dirtyX0_ = x0;
	if (y0 < dirtyY0_) dirtyY0_ = y0;
	if (x1 > dirtyX1_) dirtyX1_ = x1;
	if (y1 > dirtyY1_) dirtyY1_ = y1;
	addDirtyRect(x0, y0, x1, y1);
}

void Canvas::markDirtyClipped(int x0, int y0, int x1, int y1)
{
	const ClipRect *clip = &clipStack_[clipDepth_];
	if (x0 < clip->x0) x0 = clip->x0;
	if (y0 < clip->y0) y0 = clip->y0;
	if (x1 > clip->x1) x1 = clip->x1;
	if (y1 > clip->y1) y1 = clip->y1;
	markDirty(x0, y0, x1, y1);
}

bool Canvas::dirty(int *x0, int *y0, int *x1, int *y1) const
{
	if (dirtyX0_ > dirtyX1_ || dirtyY0_ > dirtyY1_) return false;
	if (x0) *x0 = dirtyX0_;
	if (y0) *y0 = dirtyY0_;
	if (x1) *x1 = dirtyX1_;
	if (y1) *y1 = dirtyY1_;
	return true;
}

int Canvas::dirtyRects(CanvasDirtyRect *rects, int capacity) const
{
	if (!rects || capacity <= 0 || dirtyRectCount_ <= 0) return 0;
	int count = dirtyRectCount_;
	if (count > capacity) count = capacity;
	for (int i = 0; i < count; i++) rects[i] = dirtyRects_[i];
	return count;
}

void Canvas::resetClip()
{
	clipDepth_ = 0;
	clipStack_[0].x0 = 0;
	clipStack_[0].y0 = 0;
	clipStack_[0].x1 = width_ > 0 ? width_ - 1 : 0;
	clipStack_[0].y1 = height_ > 0 ? height_ - 1 : 0;
}

void Canvas::pushClip(int x, int y, int w, int h)
{
	int nx0 = x, ny0 = y, nx1 = x + w - 1, ny1 = y + h - 1;
	const ClipRect *current = &clipStack_[clipDepth_];
	if (nx0 < current->x0) nx0 = current->x0;
	if (ny0 < current->y0) ny0 = current->y0;
	if (nx1 > current->x1) nx1 = current->x1;
	if (ny1 > current->y1) ny1 = current->y1;
	if (clipDepth_ < Canvas::kMaxClipDepth - 1) clipDepth_++;
	clipStack_[clipDepth_].x0 = nx0;
	clipStack_[clipDepth_].y0 = ny0;
	clipStack_[clipDepth_].x1 = nx1;
	clipStack_[clipDepth_].y1 = ny1;
}

void Canvas::popClip()
{
	if (clipDepth_ > 0) clipDepth_--;
}

void Canvas::currentClip(int *x0, int *y0, int *x1, int *y1) const
{
	const ClipRect *c = &clipStack_[clipDepth_];
	if (x0) *x0 = c->x0;
	if (y0) *y0 = c->y0;
	if (x1) *x1 = c->x1;
	if (y1) *y1 = c->y1;
}

void Canvas::setGlobalAlpha(std::uint8_t alpha)
{
	globalAlpha_ = alpha;
}

std::uint8_t Canvas::globalAlpha() const
{
	return globalAlpha_;
}

void Canvas::clear(pixel::native_t color)
{
	if (!pixels_) return;
#if GEA_PIXEL_STORAGE_PACKED
	const std::uint8_t rep = pixel::packed::replicate(static_cast<std::uint8_t>(color));
	// A solid replicated byte fills every nibble identically, so a rotated buffer
	// needs no per-pixel transform — only the correct byte count. The physical
	// landscape buffer is (strideBytes_ x width_), NOT (strideBytes_ x height_).
	std::size_t clearBytes = static_cast<std::size_t>(strideBytes_) * height_;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) clearBytes = static_cast<std::size_t>(strideBytes_) * width_;
#endif
	std::memset(reinterpret_cast<std::uint8_t *>(pixels_), rep, clearBytes);
#else
	std::size_t clearPixels = static_cast<std::size_t>(stride_) * height_;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) clearPixels = static_cast<std::size_t>(stride_) * width_;
#endif
	std::fill_n(pixels_, clearPixels, color);
#endif
	markDirty(0, 0, width_ - 1, height_ - 1);
}

void Canvas::clearRect(int x, int y, int w, int h)
{
	std::uint8_t previousAlpha = globalAlpha_;
	globalAlpha_ = 255;
	fillRect(x, y, w, h, 0);
	globalAlpha_ = previousAlpha;
}

void Canvas::scrollRect(int x, int y, int w, int h, int dx, int dy)
{
	if (!pixels_ || (dx == 0 && dy == 0) || w <= 0 || h <= 0) return;

	const ClipRect *clip = &clipStack_[clipDepth_];
	int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;

	if (x0 < clip->x0) x0 = clip->x0;
	if (y0 < clip->y0) y0 = clip->y0;
	if (x1 > clip->x1) x1 = clip->x1;
	if (y1 > clip->y1) y1 = clip->y1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width_) x1 = width_ - 1;
	if (y1 >= height_) y1 = height_ - 1;
	if (x0 > x1 || y0 > y1) return;

	int rect_w = x1 - x0 + 1;
	int rect_h = y1 - y0 + 1;
	if (dx <= -rect_w || dx >= rect_w) return;
	if (dy <= -rect_h || dy >= rect_h) return;

	// Choose row iteration order so that we never overwrite a source row before reading it:
	// dy > 0 (move down): iterate rows bottom-up
	// dy <= 0 (move up or pure horizontal): iterate rows top-down
	int row_start, row_end, row_step;
	if (dy > 0) {
		row_start = y1; row_end = y0 - 1; row_step = -1;
	} else {
		row_start = y0; row_end = y1 + 1; row_step = 1;
	}

	for (int row = row_start; row != row_end; row += row_step) {
		// Source row is row - dy if shifting down (dy>0), or row + |dy| if shifting up.
		int src_row = row - dy;
		if (src_row < y0 || src_row > y1) continue;

#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		// Rotated: copy per pixel through the transform. When dy==0 (row==src_row)
		// the shift aliases the same physical column, so pick the iteration
		// direction that reads each source before it is overwritten.
		if (landscapeRot_) {
			if (dx > 0) {
				for (int i = rect_w - dx - 1; i >= 0; --i) rotSet(x0 + dx + i, row, rotGet(x0 + i, src_row));
			} else if (dx < 0) {
				const int n = rect_w + dx;
				for (int i = 0; i < n; ++i) rotSet(x0 + i, row, rotGet(x0 - dx + i, src_row));
			} else {
				for (int i = 0; i < rect_w; ++i) rotSet(x0 + i, row, rotGet(x0 + i, src_row));
			}
			continue;
		}
#endif
		// Within-row horizontal shift by dx pixels; overlap handled internally.
#if GEA_PIXEL_STORAGE_PACKED
		std::uint8_t *dstRow = packedRow(rowToPhysical(row));
		const std::uint8_t *srcRow = packedRow(rowToPhysical(src_row));
		if (dx > 0) {
			pixel::packed::copyPacked(dstRow, x0 + dx, srcRow, x0, rect_w - dx);
		} else if (dx < 0) {
			pixel::packed::copyPacked(dstRow, x0, srcRow, x0 - dx, rect_w + dx);
		} else {
			pixel::packed::copyPacked(dstRow, x0, srcRow, x0, rect_w);
		}
#else
		pixel::native_t *dst = &pixels_[rowToPhysical(row) * stride_ + x0];
		pixel::native_t *src = &pixels_[rowToPhysical(src_row) * stride_ + x0];
		if (dx > 0) {
			memmove(dst + dx, src, (size_t)(rect_w - dx) * sizeof(pixel::native_t));
		} else if (dx < 0) {
			memmove(dst, src - dx, (size_t)(rect_w + dx) * sizeof(pixel::native_t));
		} else {
			memmove(dst, src, (size_t)rect_w * sizeof(pixel::native_t));
		}
#endif
	}

	markDirty(x0, y0, x1, y1);
}

void Canvas::fillRect(int x, int y, int w, int h, pixel::native_t color)
{
	if (!pixels_) return;

	const ClipRect *clip = &clipStack_[clipDepth_];
	int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;

	if (x0 < clip->x0) x0 = clip->x0;
	if (y0 < clip->y0) y0 = clip->y0;
	if (x1 > clip->x1) x1 = clip->x1;
	if (y1 > clip->y1) y1 = clip->y1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width_) x1 = width_ - 1;
	if (y1 >= height_) y1 = height_ - 1;
	if (x0 > x1 || y0 > y1) return;

	const pixel::native_t c = color;
	if (globalAlpha_ == 255) {
		const int count = x1 - x0 + 1;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		// Rotated: a logical row is a physical column, so the byte/word occlusion
		// scan below (which assumes contiguous packed bytes) does not apply. Walk
		// logical columns instead: each one is a contiguous native framebuffer row.
		// This avoids turning a small logical fill into one PSRAM cache miss per pixel.
		if (landscapeRot_) {
#if GEA_PIXEL_STORAGE_PACKED
			for (int col = x0; col <= x1; ++col)
				for (int row = y0; row <= y1; ++row) rotSet(col, row, c);
#else
			for (int col = x0; col <= x1; ++col)
				pixel::fillNative(&pixels_[rotPhysRow(col) * stride_ + y0], y1 - y0 + 1, c);
#endif
			markDirty(x0, y0, x1, y1);
			return;
		}
#endif
#if !GEA_PIXEL_STORAGE_PACKED
		// Large opaque fills (e.g. a full-screen page/paper background) skip the
		// per-pixel occlusion scan below. That scan READS every pixel just to
		// minimize the dirty rect — worth it for a small changed region, but pure
		// overhead for a page-sized fill that gets repainted wholesale anyway. Here
		// we memset each row (fillSpanColor) and mark the whole rect dirty once.
		// (Packed targets take the word-scan below for EVERY size: on e-paper the
		// erase fill repaints a mostly-unchanged page, and skipping the matching
		// words costs one sequential read instead of a full write-allocate pass —
		// measured 42→~25 ms for the 540×960 page-turn eraser.)
		if (static_cast<long long>(count) * (y1 - y0 + 1) >= static_cast<long long>(width_) * height_ / 4) {
			for (int row = y0; row <= y1; row++) fillSpanColor(rowToPhysical(row), x0, count, c);
			markDirty(x0, y0, x1, y1);
			return;
		}
#endif
		for (int row = y0; row <= y1; row++) {
#if GEA_PIXEL_STORAGE_PACKED
			// Byte/word-wise occlusion scan: a packed byte holds kPxPerByte pixels,
			// so comparing whole bytes against the replicated value (and aligned
			// 32-bit words against it ×4) skips already-correct spans without
			// per-pixel bit work. Only differing bytes are rewritten and marked
			// dirty (kPxPerByte-px run granularity). The per-pixel version of this
			// scan read the whole rect one pixel at a time from PSRAM and dominated
			// an e-reader page turn (~200 ms of the frame).
			constexpr int kPxPerByte = pixel::packed::kPxPerByte;
			std::uint8_t *dstRow = packedRow(rowToPhysical(row));
			const std::uint8_t lvl = static_cast<std::uint8_t>(c & pixel::packed::kMask);
			const std::uint8_t rep = pixel::packed::replicate(lvl);
			const std::uint32_t rep4 = 0x01010101u * rep;
			int runStart = -1;  // pixel coords; runs close inside this branch
			int xs = x0, xe = x1;
			// Leading pixels that share their byte with pixels BEFORE the span.
			while (xs <= xe && (xs % kPxPerByte) != 0) {
				if (pixel::packed::get(dstRow, xs) != lvl) {
					pixel::packed::set(dstRow, xs, lvl);
					markDirty(xs, row, xs, row);
				}
				xs++;
			}
			// Trailing pixels that share their byte with pixels AFTER the span.
			while (xe >= xs && (xe % kPxPerByte) != kPxPerByte - 1) {
				if (pixel::packed::get(dstRow, xe) != lvl) {
					pixel::packed::set(dstRow, xe, lvl);
					markDirty(xe, row, xe, row);
				}
				xe--;
			}
			// Whole-byte middle. Guarded because the trims above can consume the
			// entire span, and an empty [xs, xe] must not touch a byte.
			if (xs <= xe) {
				const int bEnd = xe / kPxPerByte;  // inclusive whole-byte range
				for (int bi = xs / kPxPerByte; bi <= bEnd;) {
					if (bi + 3 <= bEnd &&
					    (reinterpret_cast<std::uintptr_t>(dstRow + bi) & 3u) == 0 &&
					    *reinterpret_cast<const std::uint32_t *>(dstRow + bi) == rep4) {
						if (runStart >= 0) {
							markDirty(runStart, row, bi * kPxPerByte - 1, row);
							runStart = -1;
						}
						bi += 4;
						continue;
					}
					if (dstRow[bi] == rep) {
						if (runStart >= 0) {
							markDirty(runStart, row, bi * kPxPerByte - 1, row);
							runStart = -1;
						}
					} else {
						if (runStart < 0) runStart = bi * kPxPerByte;
						dstRow[bi] = rep;
					}
					bi++;
				}
			}
			if (runStart >= 0) {
				markDirty(runStart, row, xe, row);
				runStart = -1;  // keep the shared tail below a no-op
			}
#else
			pixel::native_t *dst = &pixels_[rowToPhysical(row) * stride_ + x0];
			int runStart = -1;
			for (int i = 0; i < count; i++) {
				if (dst[i] == c) {
					if (runStart >= 0) {
						markDirty(x0 + runStart, row, x0 + i - 1, row);
						runStart = -1;
					}
					continue;
				}
				if (runStart < 0) runStart = i;
				dst[i] = c;
			}
#endif
			if (runStart >= 0) markDirty(x0 + runStart, row, x1, row);
		}
	} else if (globalAlpha_ != 0) {
		const int a = globalAlpha_;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		if (landscapeRot_) {
#if GEA_PIXEL_STORAGE_PACKED
			for (int col = x0; col <= x1; ++col)
				for (int row = y0; row <= y1; ++row) rotBlend(col, row, c, a);
#else
			for (int col = x0; col <= x1; ++col) {
				pixel::native_t *dst = &pixels_[rotPhysRow(col) * stride_ + y0];
				for (int row = y0; row <= y1; ++row, ++dst)
					*dst = pixel::blendNative(c, *dst, a);
			}
#endif
			markDirty(x0, y0, x1, y1);
			return;
		}
#endif
		for (int row = y0; row <= y1; row++) {
#if GEA_PIXEL_STORAGE_PACKED
			std::uint8_t *dstRow = packedRow(rowToPhysical(row));
			for (int col = x0; col <= x1; col++) pixel::packed::blendPixel(dstRow, col, c, a);
#else
			pixel::native_t *dst = &pixels_[rowToPhysical(row) * stride_ + x0];
			for (int col = x0; col <= x1; col++, dst++) {
				*dst = pixel::blendNative(c, *dst, a);
			}
#endif
		}
		markDirty(x0, y0, x1, y1);
	}
}

void Canvas::fillRectOpaque(int x, int y, int w, int h, pixel::native_t color)
{
	if (!pixels_) return;

	const ClipRect *clip = &clipStack_[clipDepth_];
	int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;

	if (x0 < clip->x0) x0 = clip->x0;
	if (y0 < clip->y0) y0 = clip->y0;
	if (x1 > clip->x1) x1 = clip->x1;
	if (y1 > clip->y1) y1 = clip->y1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= width_) x1 = width_ - 1;
	if (y1 >= height_) y1 = height_ - 1;
	if (x0 > x1 || y0 > y1) return;

#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	if (landscapeRot_) {
#if GEA_PIXEL_STORAGE_PACKED
		for (int col = x0; col <= x1; ++col)
			for (int row = y0; row <= y1; ++row) rotSet(col, row, color);
#else
		for (int col = x0; col <= x1; ++col)
			pixel::fillNative(&pixels_[rotPhysRow(col) * stride_ + y0], y1 - y0 + 1, color);
#endif
		markDirty(x0, y0, x1, y1);
		return;
	}
#endif

	const int count = x1 - x0 + 1;
	for (int row = y0; row <= y1; row++) {
		fillSpanColor(rowToPhysical(row), x0, count, color);
	}
	markDirty(x0, y0, x1, y1);
}

void Canvas::strokeRect(int x, int y, int w, int h, pixel::native_t color)
{
	if (w <= 0 || h <= 0) return;
	fillRect(x, y, w, 1, color);
	fillRect(x, y + h - 1, w, 1, color);
	if (h > 2) {
		fillRect(x, y + 1, 1, h - 2, color);
		fillRect(x + w - 1, y + 1, 1, h - 2, color);
	}
}

void Canvas::fillCircle(int cx, int cy, int r, pixel::native_t color)
{
	if (fillCircleNoDirty(cx, cy, r, color)) {
		markDirtyClipped(cx - r, cy - r, cx + r, cy + r);
	}
}

bool Canvas::fillCircleNoDirty(int cx, int cy, int r, pixel::native_t color)
{
	if (!pixels_ || r <= 0) return false;
	const ClipRect *clip = &clipStack_[clipDepth_];
	if (cx + r < clip->x0 || cx - r > clip->x1 || cy + r < clip->y0 || cy - r > clip->y1) return false;

	const std::uint8_t (*spans)[2] = CanvasMath::instance().circleRadiusSpans(r);
	if (!spans) {
		const int rr = r * r;
		int dy0 = -r;
		int dy1 = r;
		if (cy + dy0 < clip->y0) dy0 = clip->y0 - cy;
		if (cy + dy1 > clip->y1) dy1 = clip->y1 - cy;
		if (cy + dy0 < 0) dy0 = -cy;
		if (cy + dy1 >= height_) dy1 = height_ - 1 - cy;
		if (dy0 > dy1) return false;
		for (int dy = dy0; dy <= dy1; dy++) {
			int py = cy + dy;
			int dx = CanvasMath::integerSqrt(rr - dy * dy);
			int x0 = cx - dx;
			int x1 = cx + dx;
			if (x0 < clip->x0) x0 = clip->x0;
			if (x1 > clip->x1) x1 = clip->x1;
			if (x0 < 0) x0 = 0;
			if (x1 >= width_) x1 = width_ - 1;
			if (x0 > x1) continue;

			fillSpanGlobalAlpha(rowToPhysical(py), x0, x1 - x0 + 1, color);
		}
		return true;
	}

	const int x = cx - r;
	const int y = cy - r;
	const int diameter = r * 2 + 1;
	int row0 = 0;
	int row1 = diameter - 1;
	if (y + row0 < clip->y0) row0 = clip->y0 - y;
	if (y + row1 > clip->y1) row1 = clip->y1 - y;
	if (y + row0 < 0) row0 = -y;
	if (y + row1 >= height_) row1 = height_ - 1 - y;
	if (row0 > row1) return false;
	for (int row = row0; row <= row1; row++) {
		int py = y + row;
		int x0 = x + spans[row][0];
		int x1 = x + spans[row][1];
		if (x0 < clip->x0) x0 = clip->x0;
		if (x1 > clip->x1) x1 = clip->x1;
		if (x0 < 0) x0 = 0;
		if (x1 >= width_) x1 = width_ - 1;
		if (x0 > x1) continue;

		fillSpanGlobalAlpha(rowToPhysical(py), x0, x1 - x0 + 1, color);
	}
	return true;
}

void Canvas::fillCirclesRgb565(const std::uint16_t *xs,
                               const std::uint16_t *ys,
                               int count,
                               int r,
                               const pixel::native_t *colors)
{
	if (!pixels_ || !xs || !ys || !colors || count <= 0 || r <= 0) return;
	const ClipRect *clip = &clipStack_[clipDepth_];
	int dirtyX0 = width_;
	int dirtyY0 = height_;
	int dirtyX1 = -1;
	int dirtyY1 = -1;

	for (int i = 0; i < count; i++) {
		const int cx = xs[i];
		const int cy = ys[i];
		int x0 = cx - r;
		int y0 = cy - r;
		int x1 = cx + r;
		int y1 = cy + r;
		if (x1 < clip->x0 || x0 > clip->x1 || y1 < clip->y0 || y0 > clip->y1) continue;
		if (x0 < clip->x0) x0 = clip->x0;
		if (y0 < clip->y0) y0 = clip->y0;
		if (x1 > clip->x1) x1 = clip->x1;
		if (y1 > clip->y1) y1 = clip->y1;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 >= width_) x1 = width_ - 1;
		if (y1 >= height_) y1 = height_ - 1;
		if (x0 > x1 || y0 > y1) continue;
		if (x0 < dirtyX0) dirtyX0 = x0;
		if (y0 < dirtyY0) dirtyY0 = y0;
		if (x1 > dirtyX1) dirtyX1 = x1;
		if (y1 > dirtyY1) dirtyY1 = y1;
		fillCircleNoDirty(cx, cy, r, colors[i]);
	}

	markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
}

void Canvas::fillCirclesRgb565Uniform(const std::uint16_t *xs,
                                      const std::uint16_t *ys,
                                      int count,
                                      int r,
                                      pixel::native_t color)
{
	if (!pixels_ || !xs || !ys || count <= 0 || r <= 0) return;
	const ClipRect *clip = &clipStack_[clipDepth_];
	int dirtyX0 = width_;
	int dirtyY0 = height_;
	int dirtyX1 = -1;
	int dirtyY1 = -1;

	for (int i = 0; i < count; i++) {
		const int cx = xs[i];
		const int cy = ys[i];
		int x0 = cx - r;
		int y0 = cy - r;
		int x1 = cx + r;
		int y1 = cy + r;
		if (x1 < clip->x0 || x0 > clip->x1 || y1 < clip->y0 || y0 > clip->y1) continue;
		if (x0 < clip->x0) x0 = clip->x0;
		if (y0 < clip->y0) y0 = clip->y0;
		if (x1 > clip->x1) x1 = clip->x1;
		if (y1 > clip->y1) y1 = clip->y1;
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 >= width_) x1 = width_ - 1;
		if (y1 >= height_) y1 = height_ - 1;
		if (x0 > x1 || y0 > y1) continue;
		if (x0 < dirtyX0) dirtyX0 = x0;
		if (y0 < dirtyY0) dirtyY0 = y0;
		if (x1 > dirtyX1) dirtyX1 = x1;
		if (y1 > dirtyY1) dirtyY1 = y1;
		fillCircleNoDirty(cx, cy, r, color);
	}

	markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
}

// Batched fill optimized for the per-chunk rasterizer:
//   * Inputs are in WORLD coordinates; xOffset/yOffset are subtracted to
//     produce canvas-local positions.
//   * ys must be sorted ascending — skip leading circles whose bottom edge
//     is before yBandMinWorld, break once the top edge passes
//     yBandMaxWorld, so the iteration only touches in-band circles.
//   * Chunk Canvas instances are fresh: no scroll region, alpha=255, clip
//     == full canvas. We inline fillCircleNoDirty's hot path here and skip
//     every per-circle clip-stack / globalAlpha / rowToPhysical lookup.
void Canvas::fillCirclesRgb565WorldYSorted(const CircleEntry *circles,
                                            int count,
                                            int r,
                                            int yBandMinWorld,
                                            int yBandMaxWorld,
                                            int xOffset,
                                            int yOffset)
{
	if (!pixels_ || !circles || count <= 0 || r <= 0) return;
	const std::uint8_t (*spans)[2] = CanvasMath::instance().circleRadiusSpans(r);
	const int diameter = r * 2 + 1;
	const int w = width_;
	const int h = height_;
	const int minCenterY = yBandMinWorld - r;
	const int maxCenterY = yBandMaxWorld + r;
	const CircleEntry *begin = std::lower_bound(circles, circles + count, minCenterY,
	                                            [](const CircleEntry &entry, int y) {
		                                            return static_cast<int>(entry.y) < y;
	                                            });
	const CircleEntry *end = std::upper_bound(begin, circles + count, maxCenterY,
	                                          [](int y, const CircleEntry &entry) {
		                                          return y < static_cast<int>(entry.y);
	                                          });
	for (const CircleEntry *it = begin; it != end; ++it) {
		const CircleEntry &entry = *it;
		const int worldCy = static_cast<int>(entry.y);
		const int cx = static_cast<int>(entry.x) - xOffset;
		const int cy = worldCy - yOffset;
		const pixel::native_t color = entry.color;
		const int xOriginCanvas = cx - r;
		const int yOriginCanvas = cy - r;
		int row0 = 0;
		int row1 = diameter - 1;
		if (yOriginCanvas + row0 < 0) row0 = -yOriginCanvas;
		if (yOriginCanvas + row1 >= h) row1 = h - 1 - yOriginCanvas;
		if (row0 > row1) continue;
		if (spans) {
			for (int row = row0; row <= row1; row++) {
				const int py = yOriginCanvas + row;
				int x0 = xOriginCanvas + spans[row][0];
				int x1 = xOriginCanvas + spans[row][1];
				if (x0 < 0) x0 = 0;
				if (x1 >= w) x1 = w - 1;
				if (x0 > x1) continue;
				fillSpanColor(py, x0, x1 - x0 + 1, color);
			}
		} else {
			const int rr = r * r;
			for (int row = row0; row <= row1; row++) {
				const int dy = row - r;
				const int dx = CanvasMath::integerSqrt(rr - dy * dy);
				const int py = yOriginCanvas + row;
				int x0 = cx - dx;
				int x1 = cx + dx;
				if (x0 < 0) x0 = 0;
				if (x1 >= w) x1 = w - 1;
				if (x0 > x1) continue;
				fillSpanColor(py, x0, x1 - x0 + 1, color);
			}
		}
	}
}

void Canvas::strokeCircle(int cx, int cy, int r, pixel::native_t color)
{
	if (!pixels_ || r <= 0) return;
	int x = r, y = 0, d = 1 - r;
	while (x >= y) {
		writePixel(cx + x, cy + y, color);
		writePixel(cx - x, cy + y, color);
		writePixel(cx + x, cy - y, color);
		writePixel(cx - x, cy - y, color);
		writePixel(cx + y, cy + x, color);
		writePixel(cx - y, cy + x, color);
		writePixel(cx + y, cy - x, color);
		writePixel(cx - y, cy - x, color);
		y++;
		if (d <= 0) d += 2 * y + 1;
		else { x--; d += 2 * (y - x) + 1; }
	}
	markDirtyClipped(cx - r, cy - r, cx + r, cy + r);
}

void Canvas::drawLine(int x0, int y0, int x1, int y1, pixel::native_t color)
{
	if (!pixels_) return;
	int dx = abs(x1 - x0);
	int dy = -abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int mx0 = x0 < x1 ? x0 : x1, my0 = y0 < y1 ? y0 : y1;
	int mx1 = x0 > x1 ? x0 : x1, my1 = y0 > y1 ? y0 : y1;
	markDirtyClipped(mx0, my0, mx1, my1);
	while (1) {
		writePixel(x0, y0, color);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

void Canvas::drawArc(int cx, int cy, int r, int start_deg, int end_deg, pixel::native_t color)
{
	if (!pixels_ || r <= 0) return;
	start_deg = ((start_deg % 360) + 360) % 360;
	end_deg = ((end_deg % 360) + 360) % 360;
	markDirtyClipped(cx - r, cy - r, cx + r, cy + r);
	int x = r, y = 0, d = 1 - r;
	while (x >= y) {
		int points[8][2] = {
			{cx+x, cy-y}, {cx+y, cy-x}, {cx-y, cy-x}, {cx-x, cy-y},
			{cx-x, cy+y}, {cx-y, cy+x}, {cx+y, cy+x}, {cx+x, cy+y},
		};
		for (int i = 0; i < 8; i++) {
			int px = points[i][0], py = points[i][1];
			if (px < 0 || px >= width_ || py < 0 || py >= height_) continue;
			float a = atan2f(-(float)(py - cy), (float)(px - cx));
			a = a * (180.0f / 3.14159265f);
			if (a < 0) a += 360.0f;
			int deg = (int)(a + 0.5f);
			if (deg >= 360) deg -= 360;
			int in_range = (start_deg <= end_deg) ? (deg >= start_deg && deg <= end_deg) : (deg >= start_deg || deg <= end_deg);
			if (in_range) writePixel(px, py, color);
		}
		y++;
		if (d <= 0) d += 2 * y + 1;
		else { x--; d += 2 * (y - x) + 1; }
	}
}

void Canvas::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, pixel::native_t color)
{
	if (!pixels_) return;
	if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
	if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); }
	if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }
	int total_h = y2 - y0;
	if (total_h == 0) {
		int mn = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
		int mx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
		fillRect(mn, y0, mx - mn + 1, 1, color);
		return;
	}
	int min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
	int max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	const ClipRect *clip = &clipStack_[clipDepth_];
	int draw_y0 = y0;
	int draw_y1 = y2;
	if (draw_y0 < clip->y0) draw_y0 = clip->y0;
	if (draw_y1 > clip->y1) draw_y1 = clip->y1;
	if (draw_y0 < 0) draw_y0 = 0;
	if (draw_y1 >= height_) draw_y1 = height_ - 1;
	if (draw_y0 > draw_y1) return;
	if (globalAlpha_ != 255 && globalAlpha_ != 0) markDirtyClipped(min_x, y0, max_x, y2);
	for (int y = draw_y0; y <= draw_y1; y++) {
		int second = (y >= y1);
		int seg_h = second ? (y2 - y1) : (y1 - y0);
		if (seg_h == 0) seg_h = 1;
		float al = (float)(y - y0) / total_h;
		float beta = second ? (float)(y - y1) / seg_h : (float)(y - y0) / seg_h;
		int ax = x0 + (int)((x2 - x0) * al);
		int bx = second ? x1 + (int)((x2 - x1) * beta) : x0 + (int)((x1 - x0) * beta);
		if (ax > bx) { int t = ax; ax = bx; bx = t; }
		if (ax < clip->x0) ax = clip->x0;
		if (bx > clip->x1) bx = clip->x1;
		if (ax < 0) ax = 0;
		if (bx >= width_) bx = width_ - 1;
		if (ax > bx) continue;
		const pixel::native_t c = color;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		if (landscapeRot_) {
			if (globalAlpha_ == 255) {
				for (int px = ax; px <= bx; px++) rotSet(px, y, c);
				markDirty(ax, y, bx, y);
			} else if (globalAlpha_ != 0) {
				const int a = globalAlpha_;
				for (int px = ax; px <= bx; px++) rotBlend(px, y, c, a);
			}
			continue;
		}
#endif
#if GEA_PIXEL_STORAGE_PACKED
		std::uint8_t *dnib = packedRow(rowToPhysical(y));
		if (globalAlpha_ == 255) {
			const int count = bx - ax + 1;
			int runStart = -1;
			for (int i = 0; i < count; i++) {
				if (pixel::packed::get(dnib, ax + i) == c) {
					if (runStart >= 0) { markDirty(ax + runStart, y, ax + i - 1, y); runStart = -1; }
					continue;
				}
				if (runStart < 0) runStart = i;
				pixel::packed::set(dnib, ax + i, c);
			}
			if (runStart >= 0) markDirty(ax + runStart, y, bx, y);
		} else if (globalAlpha_ != 0) {
			const int a = globalAlpha_;
			for (int px = ax; px <= bx; px++) pixel::packed::blendPixel(dnib, px, c, a);
		}
#else
		pixel::native_t *dst = &pixels_[rowToPhysical(y) * stride_ + ax];
		if (globalAlpha_ == 255) {
			const int count = bx - ax + 1;
			int runStart = -1;
			for (int i = 0; i < count; i++) {
				if (dst[i] == c) {
					if (runStart >= 0) {
						markDirty(ax + runStart, y, ax + i - 1, y);
						runStart = -1;
					}
					continue;
				}
				if (runStart < 0) runStart = i;
				dst[i] = c;
			}
			if (runStart >= 0) markDirty(ax + runStart, y, bx, y);
		} else if (globalAlpha_ != 0) {
			const int a = globalAlpha_;
			for (int px = ax; px <= bx; px++, dst++) *dst = pixel::blendNative(c, *dst, a);
		}
#endif
	}
}

// Present-replay variant: each flush chunk starts from scratch, so fillTriangle's
// persistent-surface bookkeeping (per-pixel dst-compare + run-based markDirty)
// is pure overhead there — flat span fills, one bbox markDirty.
// Edges step incrementally in 48.16 fixed point: one divide per edge instead of
// two float divides per scanline. The present replay rasterizes thousands of
// triangle rows per frame and those divides dominated the raster time. 64-bit
// accumulators keep arbitrary int coordinates overflow-safe.
void Canvas::fillTriangleOpaque(int x0, int y0, int x1, int y1, int x2, int y2, pixel::native_t color)
{
	if (!pixels_) return;
	if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
	if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); }
	if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }
	const int total_h = y2 - y0;
	if (total_h == 0) {
		int mn = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
		int mx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
		fillRectOpaque(mn, y0, mx - mn + 1, 1, color);
		return;
	}
	const ClipRect *clip = &clipStack_[clipDepth_];
	int draw_y0 = y0;
	int draw_y1 = y2;
	if (draw_y0 < clip->y0) draw_y0 = clip->y0;
	if (draw_y1 > clip->y1) draw_y1 = clip->y1;
	if (draw_y0 < 0) draw_y0 = 0;
	if (draw_y1 >= height_) draw_y1 = height_ - 1;
	if (draw_y0 > draw_y1) return;
	int clip_x0 = clip->x0 < 0 ? 0 : clip->x0;
	int clip_x1 = clip->x1 >= width_ ? width_ - 1 : clip->x1;
	if (clip_x0 > clip_x1) return;

	// Long edge y0→y2 spans both segments with one continuous accumulator, so
	// the two mesh triangles sharing it rasterize identical columns (no cracks).
	const std::int64_t stepA = ((static_cast<std::int64_t>(x2) - x0) << 16) / total_h;
	std::int64_t accA = (static_cast<std::int64_t>(x0) << 16) + stepA * (draw_y0 - y0);

	int dirty_x0 = width_;
	int dirty_x1 = -1;
	auto rasterRows = [&](int yStart, int yEnd, std::int64_t accB, std::int64_t stepB) {
		for (int y = yStart; y <= yEnd; y++) {
			int ax = static_cast<int>(accA >> 16);
			int bx = static_cast<int>(accB >> 16);
			accA += stepA;
			accB += stepB;
			if (ax > bx) { const int t = ax; ax = bx; bx = t; }
			if (ax < clip_x0) ax = clip_x0;
			if (bx > clip_x1) bx = clip_x1;
			if (ax > bx) continue;
			fillSpanColor(rowToPhysical(y), ax, bx - ax + 1, color);
			if (ax < dirty_x0) dirty_x0 = ax;
			if (bx > dirty_x1) dirty_x1 = bx;
		}
	};

	if (draw_y0 < y1) {
		// Upper segment rows [y0, y1): draw_y0 < y1 with draw_y0 >= y0 implies y1 > y0.
		const int yEnd = draw_y1 < y1 - 1 ? draw_y1 : y1 - 1;
		const std::int64_t stepB = ((static_cast<std::int64_t>(x1) - x0) << 16) / (y1 - y0);
		const std::int64_t accB = (static_cast<std::int64_t>(x0) << 16) + stepB * (draw_y0 - y0);
		rasterRows(draw_y0, yEnd, accB, stepB);
	}
	if (draw_y1 >= y1) {
		// Lower segment rows [y1, y2]; accA is already positioned at yStart.
		const int yStart = draw_y0 > y1 ? draw_y0 : y1;
		const int segh = y2 - y1;
		const std::int64_t stepB = segh > 0 ? ((static_cast<std::int64_t>(x2) - x1) << 16) / segh : 0;
		const std::int64_t accB = (static_cast<std::int64_t>(x1) << 16) + stepB * (yStart - y1);
		rasterRows(yStart, draw_y1, accB, stepB);
	}
	if (dirty_x1 >= dirty_x0) markDirty(dirty_x0, draw_y0, dirty_x1, draw_y1);
}

void Canvas::fillTrianglesOpaqueOccluded(const TriangleEntry *tris, int count, int ox, int oy)
{
	if (!pixels_ || count <= 0) return;
	const int rows = height_;
	// Fall back to painter's back-to-front when the chunk exceeds the coverage
	// scratch (only gea3d's small half-res chunks take the occluded path).
	if (rows <= 0 || rows > kOcclMaxRows || width_ > kOcclWords * 32) {
		for (int i = 0; i < count; i++) {
			const TriangleEntry &t = tris[i];
			fillTriangleOpaque(t.x0 - ox, t.y0 - oy, t.x1 - ox, t.y1 - oy,
			                   t.x2 - ox, t.y2 - oy, t.color);
		}
		return;
	}
	const int bank = occlCoreBank();
	std::uint32_t (*cov)[kOcclWords] = s_occlBits[bank];
	std::memset(cov, 0, sizeof(std::uint32_t) * static_cast<std::size_t>(rows) * kOcclWords);

	const ClipRect *clip = &clipStack_[clipDepth_];
	const int clip_x0 = clip->x0 < 0 ? 0 : clip->x0;
	const int clip_x1 = clip->x1 >= width_ ? width_ - 1 : clip->x1;
	if (clip_x0 > clip_x1) return;
	const int clip_y0 = clip->y0 < 0 ? 0 : clip->y0;
	const int clip_y1 = clip->y1 >= rows ? rows - 1 : clip->y1;

	int dirty_x0 = width_, dirty_x1 = -1, dirty_y0 = rows, dirty_y1 = -1;

	// Fill only the uncovered pixels of chunk-local span [ax,bx] on row yl, then
	// mark [ax,bx] covered. Word-scan the row bitmask: each contiguous 0-bit run
	// becomes one fillSpanColor; occluded pixels cost only the bit test.
	auto fillCovered = [&](int yl, int ax, int bx, pixel::native_t color) {
		std::uint32_t *bits = cov[yl];
		const int phys = rowToPhysical(yl);
		int x = ax;
		while (x <= bx) {
			const int wi = x >> 5;
			const int wordLast = ((wi + 1) << 5) - 1;  // last x index in this word
			const int hi = wordLast < bx ? wordLast : bx;
			const int nbits = hi - x + 1;
			const std::uint32_t rangeMask =
				(nbits >= 32) ? 0xFFFFFFFFu : (((1u << nbits) - 1u) << (x & 31));
			const std::uint32_t word = bits[wi];
			if ((word & rangeMask) == 0) {
				fillSpanColor(phys, x, nbits, color);  // whole word-chunk uncovered
			} else if ((~word & rangeMask) != 0) {
				// mixed: fill the uncovered bit-runs within [x, hi]
				int xx = x;
				while (xx <= hi) {
					while (xx <= hi && (word & (1u << (xx & 31)))) xx++;
					if (xx > hi) break;
					const int rs = xx;
					while (xx <= hi && !(word & (1u << (xx & 31)))) xx++;
					fillSpanColor(phys, rs, xx - rs, color);
				}
			}  // else: fully covered — nothing to fill
			bits[wi] = word | rangeMask;
			x = hi + 1;
		}
		if (ax < dirty_x0) dirty_x0 = ax;
		if (bx > dirty_x1) dirty_x1 = bx;
		if (yl < dirty_y0) dirty_y0 = yl;
		if (yl > dirty_y1) dirty_y1 = yl;
	};

	// Nearest-first: the batch is farthest-first (painter's), so walk it in reverse.
	for (int ti = count - 1; ti >= 0; ti--) {
		int x0 = tris[ti].x0 - ox, y0 = tris[ti].y0 - oy;
		int x1 = tris[ti].x1 - ox, y1 = tris[ti].y1 - oy;
		int x2 = tris[ti].x2 - ox, y2 = tris[ti].y2 - oy;
		const pixel::native_t color = tris[ti].color;
		if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
		if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); }
		if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }
		int dy0 = y0 < clip_y0 ? clip_y0 : y0;
		int dy1 = y2 > clip_y1 ? clip_y1 : y2;
		if (dy0 > dy1) continue;
		const int total_h = y2 - y0;
		if (total_h == 0) {
			int mn = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
			int mx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
			if (mn < clip_x0) mn = clip_x0;
			if (mx > clip_x1) mx = clip_x1;
			if (mn <= mx) fillCovered(y0, mn, mx, color);
			continue;
		}
		const std::int64_t stepA = ((static_cast<std::int64_t>(x2) - x0) << 16) / total_h;
		std::int64_t accA = (static_cast<std::int64_t>(x0) << 16) + stepA * (dy0 - y0);
		auto walk = [&](int yStart, int yEnd, std::int64_t accB, std::int64_t stepB) {
			for (int y = yStart; y <= yEnd; y++) {
				int ax = static_cast<int>(accA >> 16);
				int bx = static_cast<int>(accB >> 16);
				accA += stepA;
				accB += stepB;
				if (ax > bx) { const int t = ax; ax = bx; bx = t; }
				if (ax < clip_x0) ax = clip_x0;
				if (bx > clip_x1) bx = clip_x1;
				if (ax > bx) continue;
				fillCovered(y, ax, bx, color);
			}
		};
		if (dy0 < y1) {
			const int yEnd = dy1 < y1 - 1 ? dy1 : y1 - 1;
			const std::int64_t stepB = ((static_cast<std::int64_t>(x1) - x0) << 16) / (y1 - y0);
			const std::int64_t accB = (static_cast<std::int64_t>(x0) << 16) + stepB * (dy0 - y0);
			walk(dy0, yEnd, accB, stepB);
		}
		if (dy1 >= y1) {
			const int yStart = dy0 > y1 ? dy0 : y1;
			const int segh = y2 - y1;
			const std::int64_t stepB = segh > 0 ? ((static_cast<std::int64_t>(x2) - x1) << 16) / segh : 0;
			const std::int64_t accB = (static_cast<std::int64_t>(x1) << 16) + stepB * (yStart - y1);
			walk(yStart, dy1, accB, stepB);
		}
	}
	if (dirty_x1 >= dirty_x0 && dirty_y1 >= dirty_y0) markDirty(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
}

void Canvas::drawText(const char *text, int x, int y, pixel::native_t color, float scale)
{
	if (!pixels_ || !text) return;
	if (scale < 0.1f) scale = 1.0f;
	int glyph_w = (int)(kBitmapFontWidth * scale + 0.5f);
	int glyph_h = (int)(kBitmapFontHeight * scale + 0.5f);
	if (glyph_w < 1) glyph_w = 1;
	if (glyph_h < 1) glyph_h = 1;

	const ClipRect *clip = &clipStack_[clipDepth_];
	int pen_x = x;
	// One cell per CODEPOINT. Walking bytes drew a 3-byte UTF-8 sequence as three
	// cells, each byte an out-of-range codepoint the font substitutes with '?' --
	// so a lone em dash painted "???". Every measurement path already charges one
	// cell per codepoint (`measureBitmap` wraps through `nextWrappedLine`), so the
	// byte loop also disagreed with the layout that placed the text.
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') { pen_x = x; y += glyph_h; continue; }
		if (pen_x > clip->x1 || y > clip->y1 ||
		    pen_x + glyph_w - 1 < clip->x0 || y + glyph_h - 1 < clip->y0) {
			pen_x += glyph_w;
			continue;
		}
		const std::uint8_t *glyph = FontRegistry::bitmap8x16().glyphRows(cp);

		int dy_start = 0, dy_end = glyph_h - 1;
		int dx_start = 0, dx_end = glyph_w - 1;
		if (y + dy_start < clip->y0) dy_start = clip->y0 - y;
		if (y + dy_end > clip->y1)   dy_end = clip->y1 - y;
		if (pen_x + dx_start < clip->x0) dx_start = clip->x0 - pen_x;
		if (pen_x + dx_end > clip->x1)   dx_end = clip->x1 - pen_x;
		if (y + dy_start < 0) dy_start = -y;
		if (pen_x + dx_start < 0) dx_start = -pen_x;
		if (y + dy_end >= height_) dy_end = height_ - 1 - y;
		if (pen_x + dx_end >= width_) dx_end = width_ - 1 - pen_x;

		if (globalAlpha_ == 255) {
			for (int srow = 0; srow < kBitmapFontHeight; srow++) {
				std::uint8_t bits = glyph[srow];
				if (!bits) continue;
				int by0 = srow * glyph_h / kBitmapFontHeight;
				int by1 = (srow + 1) * glyph_h / kBitmapFontHeight - 1;
				if (by0 > dy_end || by1 < dy_start) continue;
				if (by0 < dy_start) by0 = dy_start;
				if (by1 > dy_end) by1 = dy_end;
				for (int scol = 0; scol < kBitmapFontWidth; scol++) {
					if (!(bits & (0x80 >> scol))) continue;
					int bx0 = scol * glyph_w / kBitmapFontWidth;
					int bx1 = (scol + 1) * glyph_w / kBitmapFontWidth - 1;
					if (bx0 > dx_end || bx1 < dx_start) continue;
					if (bx0 < dx_start) bx0 = dx_start;
					if (bx1 > dx_end) bx1 = dx_end;
					int span = bx1 - bx0 + 1;
					for (int by = by0; by <= by1; by++) {
						fillSpanColor(rowToPhysical(y + by), pen_x + bx0, span, color);
					}
				}
			}
		} else if (globalAlpha_ != 0) {
			for (int srow = 0; srow < kBitmapFontHeight; srow++) {
				std::uint8_t bits = glyph[srow];
				if (!bits) continue;
				int by0 = srow * glyph_h / kBitmapFontHeight;
				int by1 = (srow + 1) * glyph_h / kBitmapFontHeight - 1;
				if (by0 > dy_end || by1 < dy_start) continue;
				if (by0 < dy_start) by0 = dy_start;
				if (by1 > dy_end) by1 = dy_end;
				for (int scol = 0; scol < kBitmapFontWidth; scol++) {
					if (!(bits & (0x80 >> scol))) continue;
					int bx0 = scol * glyph_w / kBitmapFontWidth;
					int bx1 = (scol + 1) * glyph_w / kBitmapFontWidth - 1;
					if (bx0 > dx_end || bx1 < dx_start) continue;
					if (bx0 < dx_start) bx0 = dx_start;
					if (bx1 > dx_end) bx1 = dx_end;
					for (int by = by0; by <= by1; by++) {
						fillSpanGlobalAlpha(rowToPhysical(y + by), pen_x + bx0, bx1 - bx0 + 1, color);
					}
				}
			}
		}
		markDirty(pen_x + dx_start, y + dy_start,
		                        pen_x + dx_end, y + dy_end);
		pen_x += glyph_w;
	}
}

#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
/* ---- Rasterized font rendering ---- */

void Canvas::drawTextFont(const char *text, int x, int y, pixel::native_t color, int font_id)
{
	drawRasterizedText(text, x, y, color, FontRegistry::rasterized(font_id));
}

void Canvas::drawTextFontFamily(const char *text, int x, int y, pixel::native_t color, int family_id, int size_px)
{
	drawRasterizedText(text, x, y, color, FontRegistry::rasterizedFamily(family_id, size_px));
}

// Sum the glyph advances for a string in a family/size, so callers can centre
// or right-align text. The canvas API has no measureText, which forced callers
// to hand-tune a width factor per string -- workable for one fixed readout,
// wrong for anything variable ("VLC" vs "Pocket Casts").
void Canvas::drawTextFontFamilyRotated90(const char *text, int lx, int ly, pixel::native_t color, int family_id, int size_px)
{
	if (!pixels_ || !text) return;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	// This primitive already rotates landscape-authored text onto a PORTRAIT panel
	// buffer, so it is meaningless (and out-of-bounds) against a landscape-native
	// rotated binding. It is only used by the P4 DSI targets, never a rotated one.
	if (landscapeRot_) return;
#endif
	RasterizedFont font = FontRegistry::rasterizedFamily(family_id, size_px);
	if (!font.valid()) return;

	int pen_x = lx;
	int pen_y = ly;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') { pen_x = lx; pen_y += font.lineHeight(); continue; }
		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) { pen_x += font.sizePx() / 2; continue; }
		const int gx = pen_x + glyph.bearingX;
		const int gy = pen_y + font.ascender() - glyph.bearingY;
		const pixel::native_t c = color;
		for (int row = 0; row < glyph.height; row++) {
			const int l_y = gy + row;
			// landscape x advances along glyph columns; transform per pixel.
			for (int col = 0; col < glyph.width; col++) {
				const int l_x = gx + col;
				const int panel_x = l_y;
				const int panel_y = height_ - 1 - l_x;
				if (panel_x < 0 || panel_x >= width_ || panel_y < 0 || panel_y >= height_) continue;
				int alpha = font.coverage(glyph, row, col);
				if (alpha == 0) continue;
				std::uint8_t a = (globalAlpha_ == 255) ? alpha : (alpha * globalAlpha_) / 255;
				if (a == 0) continue;
#if GEA_PIXEL_STORAGE_PACKED
				pixel::packed::blendPixel(packedRow(rowToPhysical(panel_y)), panel_x, c, a);
#else
				pixel::native_t *pixel = &pixels_[rowToPhysical(panel_y) * stride_ + panel_x];
				*pixel = (a == 255) ? c : pixel::blendNative(c, *pixel, a);
#endif
			}
		}
		pen_x += glyph.advance;
	}
}

void Canvas::drawTextFontFamilyOnBackground(const char *text, int x, int y, pixel::native_t color,
                                            int family_id, int size_px, pixel::native_t background)
{
	if (!pixels_ || !text) return;
	if (globalAlpha_ != 255) {
		drawTextFontFamily(text, x, y, color, family_id, size_px);
		return;
	}

	RasterizedFont font = FontRegistry::rasterizedFamily(family_id, size_px);
	if (!font.valid()) return;

	const ClipRect *clip = &clipStack_[clipDepth_];
	const int clipX0 = std::max(0, clip->x0);
	const int clipY0 = std::max(0, clip->y0);
	const int clipX1 = std::min(width_ - 1, clip->x1);
	const int clipY1 = std::min(height_ - 1, clip->y1);
	if (clipX0 > clipX1 || clipY0 > clipY1) return;

	int penX = x;
	int penY = y;

	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') {
			penX = x;
			penY += font.lineHeight();
			continue;
		}

		SolidGlyphCacheEntry *entry = solidGlyphCacheEntry(font, color, background, cp);
		if (!entry) {
			penX += font.sizePx() / 2;
			continue;
		}

		const Glyph &glyph = entry->glyph;
		const int gx = penX + glyph.bearingX;
		const int gy = penY + font.ascender() - glyph.bearingY;
		if (gx > clipX1 || gy > clipY1 ||
		    gx + glyph.width - 1 < clipX0 || gy + glyph.height - 1 < clipY0) {
			penX += glyph.advance;
			continue;
		}

		int dirtyX0 = width_;
		int dirtyY0 = height_;
		int dirtyX1 = -1;
		int dirtyY1 = -1;

		for (const SolidGlyphRow &span : entry->rows) {
			const int py = gy + span.row;
			if (py < clipY0 || py > clipY1) continue;

			const int sx0 = gx + span.start;
			const int sx1 = sx0 + span.width - 1;
			if (sx0 > clipX1 || sx1 < clipX0) continue;

			const int copyX0 = std::max(sx0, clipX0);
			const int copyX1 = std::min(sx1, clipX1);
			const int copyCount = copyX1 - copyX0 + 1;
			const int srcOffset = span.offset + (copyX0 - sx0);
			const pixel::native_t *src = &entry->pixels[static_cast<std::size_t>(srcOffset)];
			// Glyph cache is native pixels — a straight native row copy unless the
			// framebuffer is bound through the landscape transform.
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) {
				for (int i = 0; i < copyCount; ++i) rotSet(copyX0 + i, py, src[i]);
			} else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
			pixel::packed::writeUnpacked(packedRow(rowToPhysical(py)), copyX0, src, copyCount);
#else
			std::memcpy(&pixels_[rowToPhysical(py) * stride_ + copyX0], src,
			            static_cast<std::size_t>(copyCount) * sizeof(pixel::native_t));
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			}
#endif

			if (copyX0 < dirtyX0) dirtyX0 = copyX0;
			if (copyX1 > dirtyX1) dirtyX1 = copyX1;
			if (py < dirtyY0) dirtyY0 = py;
			if (py > dirtyY1) dirtyY1 = py;
		}

		if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1) markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
		penX += glyph.advance;
	}
}

// DIAG (compile-gated, off by default; define GEA_EMBEDDED_TEXT_SPRITE_DIAG=1):
// per-path counters + us split for the text sprite cache, printed once per ~300
// calls. Answers, on device, whether the cache engages for the bouncing-balls FPS
// badge and how blit time compares to raster.
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
#ifdef ESP_PLATFORM
static inline std::int64_t tsDiagNowUs() { return esp_timer_get_time(); }
static inline std::uint32_t tsDiagCyc() { return esp_cpu_get_cycle_count(); }
#else
static inline std::int64_t tsDiagNowUs()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline std::uint32_t tsDiagCyc() { return 0; }
#endif
namespace {
struct TsDiag {
	int calls = 0, gateOff = 0, small = 0, tooLong = 0, blit = 0, built = 0, seen = 0, raster = 0, hot = 0;
	long long blitUs = 0, rasterUs = 0;
	// Last-blit sprite state: is the RLE fast path live, and how big is the sprite?
	int rleValid = -1, w = 0, h = 0, runs = 0;
	// Inside-blit phase accounting (cycle counter; cheap enough per run/row).
	long long rows = 0, rowsClip = 0, runsWalked = 0, fillPx = 0, blendPx = 0;
	long long walkCyc = 0, fillCyc = 0, blendCyc = 0;
};
TsDiag gTsDiag;
}
#endif

#if GEA_PIXEL_STORAGE_PACKED_BITS == 4
namespace {

// Direct-mapped pre-packed glyph cache (see gTextStampEnabled above). Each entry
// holds the glyph pre-blended over the declared backdrop, packed 2 px/byte, in
// both x-parities so a stamp at any x is straight byte copies.
constexpr int kGlyphStampSlots = 256;

struct PackedGlyphStamp {
	const RasterizedFontData *fontData = nullptr;
	int sizePx = 0;
	int codepoint = 0;
	std::uint8_t fg = 0xFF;        // native 0..15 level; 0xFF = empty slot
	std::uint8_t backdrop = 0xFF;
	std::int16_t w = 0, h = 0;
	std::uint8_t *rows[2] = {nullptr, nullptr};  // [parity of gx]
	std::int16_t rowBytes[2] = {0, 0};
	std::size_t cap[2] = {0, 0};
};

PackedGlyphStamp gGlyphStamps[kGlyphStampSlots];

inline int glyphStampSlot(const RasterizedFontData *fd, int sizePx, int cp)
{
	std::uint32_t h = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(fd));
	h = h * 31u + static_cast<std::uint32_t>(sizePx);
	h = h * 31u + static_cast<std::uint32_t>(cp);
	h ^= h >> 13;
	return static_cast<int>(h % kGlyphStampSlots);
}

void glyphStampCacheReset()
{
	for (auto &e : gGlyphStamps) e.fg = 0xFF;  // buffers stay for reuse
}

bool buildPackedGlyphStamp(PackedGlyphStamp &e, const gea::framework::graphics::RasterizedFont &font,
                           const gea::framework::graphics::Glyph &glyph, int cp,
                           std::uint8_t fg, std::uint8_t backdrop)
{
	const int w = glyph.width, h = glyph.height;
	if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;
	for (int parity = 0; parity < 2; parity++) {
		const int rb = (w + parity + 1) >> 1;
		const std::size_t need = static_cast<std::size_t>(rb) * h;
		if (e.cap[parity] < need) {
			std::uint8_t *grown = static_cast<std::uint8_t *>(std::realloc(e.rows[parity], need));
			if (!grown) return false;
			e.rows[parity] = grown;
			e.cap[parity] = need;
		}
		e.rowBytes[parity] = static_cast<std::int16_t>(rb);
		for (int row = 0; row < h; row++) {
			std::uint8_t *out = e.rows[parity] + static_cast<std::size_t>(row) * rb;
			if (parity) out[0] = 0;
			for (int col = 0; col < w; col++) {
				const std::uint8_t cov = font.coverage(glyph, row, col);
				const std::uint8_t v = cov == 0 ? backdrop
				                     : cov >= 248 ? fg
				                     : gea::framework::graphics::pixel::gray4Blend(fg, backdrop, cov);
				const int px = col + parity;
				std::uint8_t &byte = out[px >> 1];
				if (px & 1)
					byte = static_cast<std::uint8_t>((byte & 0xF0) | (v & 0x0F));
				else
					byte = static_cast<std::uint8_t>(v << 4);
			}
		}
	}
	e.fontData = font.data();
	e.sizePx = font.sizePx();
	e.codepoint = cp;
	e.fg = fg;
	e.backdrop = backdrop;
	e.w = static_cast<std::int16_t>(w);
	e.h = static_cast<std::int16_t>(h);
	return true;
}

// Stamp one packed row at pixel gx: interior bytes are straight copies; only a
// byte shared with a pixel OUTSIDE the glyph box (odd leading / even trailing
// edge) merges with the destination.
inline void stampPackedGlyphRow(std::uint8_t *dstRow, int gx, const std::uint8_t *src, int w)
{
	const int parity = gx & 1;
	std::uint8_t *dst = dstRow + (gx >> 1);
	const int rb = (w + parity + 1) >> 1;
	int lead = 0;
	int trail = rb;
	const bool trailPartial = ((gx + w) & 1) != 0;
	if (parity) {
		dst[0] = static_cast<std::uint8_t>((dst[0] & 0xF0) | (src[0] & 0x0F));
		lead = 1;
	}
	if (trailPartial) trail = rb - 1;
	if (trail > lead) std::memcpy(dst + lead, src + lead, static_cast<std::size_t>(trail - lead));
	if (trailPartial && trail >= lead)
		dst[rb - 1] = static_cast<std::uint8_t>((dst[rb - 1] & 0x0F) | (src[rb - 1] & 0xF0));
}

}  // namespace
#endif  // GEA_PIXEL_STORAGE_PACKED_BITS == 4

void Canvas::drawRasterizedText(const char *text, int x, int y, pixel::native_t color, const RasterizedFont &font)
{
	if (!pixels_ || !text || !font.valid()) return;
	const ClipRect *clip = &clipStack_[clipDepth_];

#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
	gTsDiag.calls++;
	if (!gTextRasterCacheEnabled) gTsDiag.gateOff++;
	else if (font.sizePx() < kTextSpriteMinSizePx) gTsDiag.small++;
	if ((gTsDiag.calls % 300) == 0)
		std::printf("TSDIAG calls=%d gateOff=%d small=%d tooLong=%d blit=%d hot=%d built=%d seen=%d raster=%d blitUs=%lld rasterUs=%lld sz=%d len=%d rle=%d w=%d h=%d runs=%d rows=%lld rowsClip=%lld runsW=%lld fillPx=%lld blendPx=%lld walkUs=%lld fillUs=%lld blendUs=%lld\n",
		            gTsDiag.calls, gTsDiag.gateOff, gTsDiag.small, gTsDiag.tooLong, gTsDiag.blit, gTsDiag.hot,
		            gTsDiag.built, gTsDiag.seen, gTsDiag.raster, gTsDiag.blitUs, gTsDiag.rasterUs,
		            font.sizePx(), (int)std::strlen(text), gTsDiag.rleValid, gTsDiag.w, gTsDiag.h, gTsDiag.runs,
		            gTsDiag.rows, gTsDiag.rowsClip, gTsDiag.runsWalked, gTsDiag.fillPx, gTsDiag.blendPx,
		            gTsDiag.walkCyc / 240, gTsDiag.fillCyc / 240, gTsDiag.blendCyc / 240);
#endif

#if GEA_EMBEDDED_TEXT_SPRITE_CACHE
	// Large static text: blit a cached coverage sprite instead of re-rasterizing
	// the glyphs from the flash atlas every frame. See the cache comment above.
	// Opt-in per app via Display.setTextRasterCache(true).
	if (gTextRasterCacheEnabled && font.sizePx() >= kTextSpriteMinSizePx) {
		const std::size_t len = std::strlen(text);
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
		if (len == 0 || len > static_cast<std::size_t>(kTextSpriteMaxLen)) gTsDiag.tooLong++;
#endif
		if (len > 0 && len <= static_cast<std::size_t>(kTextSpriteMaxLen)) {
			TextSprite &slot = gTextSprites[hashTextContent(text) % kTextSpriteSlots];
			const bool idMatch = slot.fontData == font.data() &&
			                     slot.sizePx == font.sizePx() &&
			                     std::strcmp(slot.text, text) == 0;
			if (idMatch && slot.state == TextSprite::Cached) {
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
				gTsDiag.blit++;
				gTsDiag.rleValid = slot.rleValid ? 1 : 0;
				gTsDiag.w = slot.w;
				gTsDiag.h = slot.h;
				gTsDiag.runs = (slot.rleValid && slot.rleRowStart) ? (int)slot.rleRowStart[slot.h] : 0;
				const std::int64_t __t0 = tsDiagNowUs();
#endif
				const std::int16_t *bRowX0 = slot.rowX0;
				const std::int16_t *bRowX1 = slot.rowX1;
				const std::uint8_t *bVal = slot.rleValid ? slot.rleVal : nullptr;
				const std::uint16_t *bLen = slot.rleValid ? slot.rleLen : nullptr;
				const std::int32_t *bStart = slot.rleValid ? slot.rleRowStart : nullptr;
#if GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES > 0
				if (slot.rleValid) {
					if (gHotTextSprite.owner != &slot) adoptHotTextSprite(slot);
					if (gHotTextSprite.owner == &slot) {
						bRowX0 = gHotTextSprite.rowX0;
						bRowX1 = gHotTextSprite.rowX1;
						bVal = gHotTextSprite.rleVal;
						bLen = gHotTextSprite.rleLen;
						bStart = gHotTextSprite.rleRowStart;
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
						gTsDiag.hot++;
#endif
					}
				}
#endif
				blitGlyphSprite(slot.cov, bRowX0, bRowX1, bVal, bLen, bStart,
				                slot.minLX, slot.minLY, slot.w, slot.h, x, y, color);
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
				gTsDiag.blitUs += tsDiagNowUs() - __t0;
#endif
				return;
			}
			if (idMatch && slot.state == TextSprite::Seen) {
#if GEA_EMBEDDED_TEXT_SPRITE_HOT_STAGE_BYTES > 0
				// buildTextSprite may realloc the arrays a stale staging copy points at.
				if (gHotTextSprite.owner == &slot) gHotTextSprite.owner = nullptr;
#endif
				// Second sighting: promote to a cached sprite and blit from it.
				if (buildTextSprite(slot, text, font)) {
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
					gTsDiag.built++;
#endif
					slot.state = TextSprite::Cached;
					blitGlyphSprite(slot.cov, slot.rowX0, slot.rowX1,
					                slot.rleValid ? slot.rleVal : nullptr,
					                slot.rleValid ? slot.rleLen : nullptr,
					                slot.rleValid ? slot.rleRowStart : nullptr,
					                slot.minLX, slot.minLY, slot.w, slot.h, x, y, color);
					return;
				}
				slot.state = TextSprite::Empty;  // too large / OOM: stop trying for this text
			} else if (!idMatch) {
				// First sighting: claim the slot and render normally this time.
				slot.fontData = font.data();
				slot.sizePx = font.sizePx();
				std::strncpy(slot.text, text, kTextSpriteMaxLen);
				slot.text[kTextSpriteMaxLen] = '\0';
				slot.state = TextSprite::Seen;
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
				gTsDiag.seen++;
#endif
			}
		}
	}
#endif

#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
	gTsDiag.raster++;
	const std::int64_t __rasterT0 = tsDiagNowUs();
	struct __RasterUsScope {
		std::int64_t t0;
		~__RasterUsScope() { gTsDiag.rasterUs += tsDiagNowUs() - t0; }
	} __rasterScope{__rasterT0};
#endif

	int pen_x = x;
	int pen_y = y;

	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') { pen_x = x; pen_y += font.lineHeight(); continue; }
		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) { pen_x += font.sizePx() / 2; continue; }

		int gx = pen_x + glyph.bearingX;
		int gy = pen_y + font.ascender() - glyph.bearingY;

		/* Skip glyph entirely if outside clip */
		if (gx > clip->x1 || gy > clip->y1 ||
		    gx + glyph.width - 1 < clip->x0 || gy + glyph.height - 1 < clip->y0) {
			pen_x += glyph.advance;
			continue;
		}

		/* Clip glyph row/col ranges */
		int row_start = 0, row_end = glyph.height - 1;
		int col_start = 0, col_end = glyph.width - 1;
		if (gy + row_start < clip->y0) row_start = clip->y0 - gy;
		if (gy + row_end > clip->y1)   row_end = clip->y1 - gy;
		if (gx + col_start < clip->x0) col_start = clip->x0 - gx;
		if (gx + col_end > clip->x1)   col_end = clip->x1 - gx;
		if (gy + row_start < 0) row_start = -gy;
		if (gx + col_start < 0) col_start = -gx;
		if (gy + row_end >= height_) row_end = height_ - 1 - gy;
		if (gx + col_end >= width_)  col_end = width_ - 1 - gx;

#if GEA_PIXEL_STORAGE_PACKED_BITS == 4
		// Pre-packed stamp fast path: fully-visible glyph, full alpha, and a text
		// color distinct from the declared backdrop (equal = inverted row → true
		// blend below renders it correctly over the real background).
		if (gTextStampEnabled && globalAlpha_ == 255 &&
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		    // The stamp fast path writes contiguous packed bytes per physical row,
		    // which the 90° rotation breaks; fall through to the per-pixel blend.
		    !landscapeRot_ &&
#endif
		    (color & 0x0F) != gTextStampBackdrop &&
		    row_start == 0 && col_start == 0 &&
		    row_end == glyph.height - 1 && col_end == glyph.width - 1) {
			const std::uint8_t fg4 = static_cast<std::uint8_t>(color & 0x0F);
			PackedGlyphStamp &e = gGlyphStamps[glyphStampSlot(font.data(), font.sizePx(), cp)];
			bool ready = e.fg == fg4 && e.backdrop == gTextStampBackdrop &&
			             e.fontData == font.data() && e.sizePx == font.sizePx() &&
			             e.codepoint == cp;
			if (!ready) {
				e.fg = 0xFF;
				ready = buildPackedGlyphStamp(e, font, glyph, cp, fg4, gTextStampBackdrop);
			}
			if (ready) {
				const int parity = gx & 1;
				const std::uint8_t *src = e.rows[parity];
				const int rb = e.rowBytes[parity];
				for (int row = 0; row < glyph.height; row++)
					stampPackedGlyphRow(packedRow(rowToPhysical(gy + row)), gx,
					                    src + static_cast<std::size_t>(row) * rb, glyph.width);
				markDirty(gx, gy, gx + glyph.width - 1, gy + glyph.height - 1);
				pen_x += glyph.advance;
				continue;
			}
		}
#endif

		const pixel::native_t c = color;
		for (int row = row_start; row <= row_end; row++) {
			int py = gy + row;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) {
				for (int col = col_start; col <= col_end; col++) {
					int alpha = font.coverage(glyph, row, col);
					if (alpha == 0) continue;
					std::uint8_t a = (globalAlpha_ == 255) ? alpha : (alpha * globalAlpha_) / 255;
					if (a == 0) continue;
					rotBlend(gx + col, py, c, a);
				}
				continue;
			}
#endif
#if GEA_PIXEL_STORAGE_PACKED
			std::uint8_t *fbNib = packedRow(rowToPhysical(py));
#else
			pixel::native_t *fb_row = &pixels_[rowToPhysical(py) * stride_ + gx];
#endif
			for (int col = col_start; col <= col_end; col++) {
				int alpha = font.coverage(glyph, row, col);
				if (alpha == 0) continue;

				std::uint8_t a = (globalAlpha_ == 255) ? alpha : (alpha * globalAlpha_) / 255;
				if (a == 0) continue;

#if GEA_PIXEL_STORAGE_PACKED
				pixel::packed::blendPixel(fbNib, gx + col, c, a);
#else
				pixel::native_t *pixel = &fb_row[col];
				*pixel = (a == 255) ? c : pixel::blendNative(c, *pixel, a);
#endif
			}
		}
		markDirty(gx + col_start, gy + row_start,
		                        gx + col_end, gy + row_end);
		pen_x += glyph.advance;
	}
}

void Canvas::blitGlyphSprite(const std::uint8_t *cov, const std::int16_t *rowX0, const std::int16_t *rowX1,
                             const std::uint8_t *rleVal, const std::uint16_t *rleLen, const std::int32_t *rleRowStart,
                             int minLX, int minLY, int w, int h, int x, int y, pixel::native_t color)
{
	if (!pixels_ || !cov || globalAlpha_ == 0) return;
	const ClipRect *clip = &clipStack_[clipDepth_];
	const std::uint8_t ga = globalAlpha_;
	const int baseX = x + minLX;  // screen x of sprite column 0
	const bool useRle = rleRowStart != nullptr && rleVal != nullptr && rleLen != nullptr;

	int dMinX = 0x7fffffff, dMinY = 0x7fffffff, dMaxX = -0x7fffffff, dMaxY = -0x7fffffff;
	for (int r = 0; r < h; r++) {
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
		gTsDiag.rows++;
#endif
		int cStart = rowX0[r];
		int cEnd = rowX1[r];
		if (cEnd < cStart) continue;  // empty row

		const int py = y + minLY + r;
		if (py < clip->y0 || py > clip->y1 || py < 0 || py >= height_) continue;
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
		gTsDiag.rowsClip++;
#endif

		// Clip the sprite column range to the clip rect and the framebuffer width.
		if (cStart < clip->x0 - baseX) cStart = clip->x0 - baseX;
		if (cStart < -baseX) cStart = -baseX;
		if (cEnd > clip->x1 - baseX) cEnd = clip->x1 - baseX;
		if (cEnd > width_ - 1 - baseX) cEnd = width_ - 1 - baseX;
		if (cEnd < cStart) continue;

		const int phys = rowToPhysical(py);
#if !GEA_PIXEL_STORAGE_PACKED
		pixel::native_t *fbRow = &pixels_[phys * stride_ + baseX];
#endif
		int touched0 = -1, touched1 = -1;

		if (useRle) {
			// Walk this row's runs, clipped to [cStart, cEnd]. Zero-runs cost nothing;
			// solid (255) runs fast-fill; only AA-edge runs blend per pixel.
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
			const std::uint32_t __rowC0 = tsDiagCyc();
			std::uint32_t __rowFillCyc = 0, __rowBlendCyc = 0;
#endif
			std::int32_t k = rleRowStart[r];
			const std::int32_t kEnd = rleRowStart[r + 1];
			int runCol = 0;
			while (k < kEnd && runCol + rleLen[k] <= cStart) { runCol += rleLen[k]; k++; }
			for (; k < kEnd && runCol <= cEnd; k++) {
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
				gTsDiag.runsWalked++;
#endif
				const std::uint8_t v = rleVal[k];
				int segS = runCol;
				int segE = runCol + rleLen[k] - 1;
				runCol += rleLen[k];
				if (v == 0) continue;
				if (segS < cStart) segS = cStart;
				if (segE > cEnd) segE = cEnd;
				if (segS > segE) continue;
				if (ga == 255 && v == 255) {
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
					const std::uint32_t __c0 = tsDiagCyc();
#endif
					fillSpanColor(phys, baseX + segS, segE - segS + 1, color);
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
					__rowFillCyc += tsDiagCyc() - __c0;
					gTsDiag.fillPx += segE - segS + 1;
#endif
				} else {
					const std::uint8_t aa = (ga == 255) ? v : static_cast<std::uint8_t>((static_cast<int>(v) * ga) / 255);
					if (aa == 0) continue;
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
					const std::uint32_t __c0 = tsDiagCyc();
#endif
					for (int cc = segS; cc <= segE; cc++) {
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
						if (landscapeRot_) rotBlend(baseX + cc, py, color, aa);
						else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
						pixel::packed::blendPixel(packedRow(phys), baseX + cc, color, aa);
#else
						pixel::native_t *p = &fbRow[cc];
						*p = (aa >= 255) ? color : pixel::blendNative(color, *p, aa);
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
						}
#endif
					}
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
					__rowBlendCyc += tsDiagCyc() - __c0;
					gTsDiag.blendPx += segE - segS + 1;
#endif
				}
				if (touched0 < 0) touched0 = segS;
				touched1 = segE;
			}
#if GEA_EMBEDDED_TEXT_SPRITE_DIAG
			gTsDiag.fillCyc += __rowFillCyc;
			gTsDiag.blendCyc += __rowBlendCyc;
			gTsDiag.walkCyc += (tsDiagCyc() - __rowC0) - __rowFillCyc - __rowBlendCyc;
#endif
		} else {
			const std::uint8_t *covRow = cov + static_cast<std::size_t>(r) * w;
			int c = cStart;
			while (c <= cEnd) {
				const std::uint8_t a = covRow[c];
				if (a == 0) { c++; continue; }
				if (ga == 255 && a == 255) {
					const int runStart = c;
					while (c <= cEnd && covRow[c] == 255) c++;
					fillSpanColor(phys, baseX + runStart, c - runStart, color);
					if (touched0 < 0) touched0 = runStart;
					touched1 = c - 1;
				} else {
					const std::uint8_t aa = (ga == 255) ? a : static_cast<std::uint8_t>((static_cast<int>(a) * ga) / 255);
					if (aa != 0) {
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
						if (landscapeRot_) rotBlend(baseX + c, py, color, aa);
						else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
						pixel::packed::blendPixel(packedRow(phys), baseX + c, color, aa);
#else
						pixel::native_t *p = &fbRow[c];
						*p = (aa >= 255) ? color : pixel::blendNative(color, *p, aa);
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
						}
#endif
						if (touched0 < 0) touched0 = c;
						touched1 = c;
					}
					c++;
				}
			}
		}

		if (touched0 >= 0) {
			const int sx0 = baseX + touched0;
			const int sx1 = baseX + touched1;
			if (sx0 < dMinX) dMinX = sx0;
			if (sx1 > dMaxX) dMaxX = sx1;
			if (py < dMinY) dMinY = py;
			if (py > dMaxY) dMaxY = py;
		}
	}
	if (dMaxX >= dMinX) markDirty(dMinX, dMinY, dMaxX, dMaxY);
}

void Canvas::measureTextFont(const char *text, int max_width, int font_id, int *out_w, int *out_h)
{
	*out_w = 0;
	*out_h = 0;
	if (!text || !text[0]) return;

	RasterizedFont font = FontRegistry::rasterized(font_id);
	if (!font.valid()) return;

	int line_w = 0, max_line_w = 0, lines = 1;
	if (max_width <= 0) max_width = 32767;

	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') {
			if (line_w > max_line_w) max_line_w = line_w;
			line_w = 0;
			lines++;
			continue;
		}
		Glyph glyph{};
		int adv = font.glyph(cp, &glyph) ? glyph.advance : (font.sizePx() / 2);
		int next_w = line_w + adv;
		if (next_w > max_width && line_w > 0) {
			if (line_w > max_line_w) max_line_w = line_w;
			line_w = adv;
			lines++;
		} else {
			line_w = next_w;
		}
	}
	if (line_w > max_line_w) max_line_w = line_w;
	*out_w = max_line_w;
	*out_h = lines * font.lineHeight();
}
#else
void Canvas::drawTextFont(const char *text, int x, int y, pixel::native_t color, int /*font_id*/)
{
	drawText(text, x, y, color, 1.0f);
}

void Canvas::drawTextFontFamily(const char *text, int x, int y, pixel::native_t color, int family_id, int size_px)
{
#if GEA_EMBEDDED_TTF_RUNTIME_FONTS
	// No baked atlas linked, but the runtime TTF rasterizer can still serve the
	// family (desktop targets feed it the bundle's font bytes). Blit its glyphs
	// directly — same loop as the rotated fallback below, minus the rotation —
	// so canvas labels render with the app's real font instead of the scaled
	// bitmap console font. Falls through to the bitmap font for families the
	// runtime rasterizer doesn't know.
	if (!pixels_ || !text) return;
	RasterizedFont font = FontRegistry::rasterizedFamily(family_id, size_px);
	if (font.valid()) {
		int pen_x = x;
		int pen_y = y;
		for (const char *p = text; *p;) {
			const int cp = nextUtf8Codepoint(p);
			if (cp == '\n') { pen_x = x; pen_y += font.lineHeight(); continue; }
			Glyph glyph{};
			if (!font.glyph(cp, &glyph)) { pen_x += font.sizePx() / 2; continue; }
			const int gx = pen_x + glyph.bearingX;
			const int gy = pen_y + font.ascender() - glyph.bearingY;
			const pixel::native_t c = color;
			for (int row = 0; row < glyph.height; row++) {
				const int py = gy + row;
				if (py < 0 || py >= height_) continue;
				for (int col = 0; col < glyph.width; col++) {
					const int px = gx + col;
					if (px < 0 || px >= width_) continue;
					int alpha = font.coverage(glyph, row, col);
					if (alpha == 0) continue;
					std::uint8_t a = (globalAlpha_ == 255) ? alpha : (alpha * globalAlpha_) / 255;
					if (a == 0) continue;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
					if (landscapeRot_) {
						rotBlend(px, py, c, a);
						continue;
					}
#endif
#if GEA_PIXEL_STORAGE_PACKED4
					pixel::gray4::blendPixel(gray4Row(rowToPhysical(py)), px, c, a);
#else
					pixel::native_t *pixel = &pixels_[rowToPhysical(py) * stride_ + px];
					*pixel = (a == 255) ? c : pixel::blendNative(c, *pixel, a);
#endif
				}
			}
			pen_x += glyph.advance;
		}
		return;
	}
#endif
	drawText(text, x, y, color, size_px > 0 ? static_cast<float>(size_px) / 16.0f : 1.0f);
}

void Canvas::drawTextFontFamilyRotated90(const char *text, int lx, int ly, pixel::native_t color, int family_id, int size_px)
{
	if (!pixels_ || !text) return;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	// This primitive already rotates landscape-authored text onto a PORTRAIT panel
	// buffer, so it is meaningless (and out-of-bounds) against a landscape-native
	// rotated binding. It is only used by the P4 DSI targets, never a rotated one.
	if (landscapeRot_) return;
#endif
	RasterizedFont font = FontRegistry::rasterizedFamily(family_id, size_px);
	if (!font.valid()) return;

	int pen_x = lx;
	int pen_y = ly;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') { pen_x = lx; pen_y += font.lineHeight(); continue; }
		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) { pen_x += font.sizePx() / 2; continue; }
		const int gx = pen_x + glyph.bearingX;
		const int gy = pen_y + font.ascender() - glyph.bearingY;
		const pixel::native_t c = color;
		for (int row = 0; row < glyph.height; row++) {
			const int l_y = gy + row;
			// landscape x advances along glyph columns; transform per pixel.
			for (int col = 0; col < glyph.width; col++) {
				const int l_x = gx + col;
				const int panel_x = l_y;
				const int panel_y = height_ - 1 - l_x;
				if (panel_x < 0 || panel_x >= width_ || panel_y < 0 || panel_y >= height_) continue;
				int alpha = font.coverage(glyph, row, col);
				if (alpha == 0) continue;
				std::uint8_t a = (globalAlpha_ == 255) ? alpha : (alpha * globalAlpha_) / 255;
				if (a == 0) continue;
#if GEA_PIXEL_STORAGE_PACKED
				pixel::packed::blendPixel(packedRow(rowToPhysical(panel_y)), panel_x, c, a);
#else
				pixel::native_t *pixel = &pixels_[rowToPhysical(panel_y) * stride_ + panel_x];
				*pixel = (a == 255) ? c : pixel::blendNative(c, *pixel, a);
#endif
			}
		}
		pen_x += glyph.advance;
	}
}

void Canvas::drawTextFontFamilyOnBackground(const char *text, int x, int y, pixel::native_t color,
                                            int family_id, int size_px, pixel::native_t /*background*/)
{
	drawTextFontFamily(text, x, y, color, family_id, size_px);
}
#endif /* GEA_EMBEDDED_HAS_GENERATED_FONTS */

/* ---- Text measurement ---- */

// Outside the baked-atlas conditional on purpose. `canvas.h` declares both of
// these unconditionally and `CanvasRenderingContext2D::measureText` calls them
// unconditionally, so a target with no generated font atlas -- every windowed
// desktop build, which rasterizes the bundle's TTFs at runtime instead -- linked
// against a declaration nothing defined and failed at `ld`. Neither body needs
// the atlas: both ask `FontRegistry::rasterizedFamily`, which is exactly what the
// no-atlas `drawTextFontFamily` above asks, and an invalid font answers 0 either
// way.
int Canvas::measureTextFontFamily(const char *text, int family_id, int size_px)
{
	if (!text) return 0;
	RasterizedFont font = FontRegistry::rasterizedFamily(family_id, size_px);
	if (!font.valid()) return 0;
	int width = 0;
	int lineWidth = 0;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') {
			if (lineWidth > width) width = lineWidth;
			lineWidth = 0;
			continue;
		}
		Glyph glyph{};
		// Same fallback the draw loop uses for a missing glyph, so measure and
		// draw agree rather than drifting apart on unmapped codepoints.
		lineWidth += font.glyph(cp, &glyph) ? glyph.advance : font.sizePx() / 2;
	}
	return lineWidth > width ? lineWidth : width;
}

// Vertical offset from the text's TOP-LEFT anchor to the centre of its actual
// ink, for a family/size. fillText anchors the top of the LINE BOX, so text
// centred by subtracting half the font size sags: the line box carries the
// ascender and descender, which the glyphs themselves do not fill. Measuring
// the real ink box makes `y = centreY - measureTextInkCenter(text)` exact.
int Canvas::measureTextInkCenterFontFamily(const char *text, int family_id, int size_px)
{
	if (!text) return 0;
	RasterizedFont font = FontRegistry::rasterizedFamily(family_id, size_px);
	if (!font.valid()) return 0;
	int top = INT32_MAX;
	int bottom = INT32_MIN;
	for (const char *p = text; *p;) {
		const int cp = nextUtf8Codepoint(p);
		if (cp == '\n') continue;
		Glyph glyph{};
		if (!font.glyph(cp, &glyph)) continue;
		if (glyph.height <= 0) continue;  // spaces carry no ink
		const int gTop = font.ascender() - glyph.bearingY;
		if (gTop < top) top = gTop;
		if (gTop + glyph.height > bottom) bottom = gTop + glyph.height;
	}
	if (top > bottom) return font.ascender() / 2;  // no ink (all spaces)
	return top + (bottom - top) / 2;
}

void Canvas::fillQuarterCircle(int cx, int cy, int r, int quadrant, pixel::native_t color)
{
	if (!pixels_ || r <= 0) return;
	const ClipRect *clip = &clipStack_[clipDepth_];

	int qx0 = cx;
	int qx1 = cx;
	int qy0 = cy;
	int qy1 = cy;
	switch (quadrant) {
	case 0: qx1 = cx + r; qy0 = cy - r; break;
	case 1: qx0 = cx - r; qy0 = cy - r; break;
	case 2: qx0 = cx - r; qy1 = cy + r; break;
	case 3: qx1 = cx + r; qy1 = cy + r; break;
	default: return;
	}
	if (qx0 > clip->x1 || qx1 < clip->x0 || qy0 > clip->y1 || qy1 < clip->y0) return;

	int visibleY0 = clip->y0;
	int visibleY1 = clip->y1;
	if (visibleY0 < 0) visibleY0 = 0;
	if (visibleY1 >= height_) visibleY1 = height_ - 1;
	if (visibleY0 > visibleY1) return;

	int dy0 = 0;
	int dy1 = r;
	if (quadrant == 0 || quadrant == 1) {
		if (cy - dy0 > visibleY1) dy0 = cy - visibleY1;
		if (cy - dy1 < visibleY0) dy1 = cy - visibleY0;
	} else {
		if (cy + dy0 < visibleY0) dy0 = visibleY0 - cy;
		if (cy + dy1 > visibleY1) dy1 = visibleY1 - cy;
	}
	if (dy0 < 0) dy0 = 0;
	if (dy1 > r) dy1 = r;
	if (dy0 > dy1) return;

	const int rr = r * r;
	for (int dy = dy0; dy <= dy1; dy++) {
		int dx = CanvasMath::integerSqrt(rr - dy * dy);
		int sx0, sx1, sy;
		switch (quadrant) {
		case 0: sx0 = cx;      sx1 = cx + dx; sy = cy - dy; break;
		case 1: sx0 = cx - dx; sx1 = cx;      sy = cy - dy; break;
		case 2: sx0 = cx - dx; sx1 = cx;      sy = cy + dy; break;
		case 3: sx0 = cx;      sx1 = cx + dx; sy = cy + dy; break;
		default: return;
		}
		if (sx0 < clip->x0) sx0 = clip->x0;
		if (sx1 > clip->x1) sx1 = clip->x1;
		if (sx0 < 0) sx0 = 0;
		if (sx1 >= width_) sx1 = width_ - 1;
		if (sx0 > sx1) continue;

		fillSpanGlobalAlpha(rowToPhysical(sy), sx0, sx1 - sx0 + 1, color);
	}
}

void Canvas::fillCircleBox(int x, int y, int size, pixel::native_t color)
{
	if (!pixels_) return;
	const std::uint8_t (*spans)[2] = CanvasMath::instance().circleSpans(size);
	if (!spans) return;
	const ClipRect *clip = &clipStack_[clipDepth_];

	if (x > clip->x1 || y > clip->y1 || x + size - 1 < clip->x0 || y + size - 1 < clip->y0) return;

	int row0 = 0;
	int row1 = size - 1;
	if (y + row0 < clip->y0) row0 = clip->y0 - y;
	if (y + row1 > clip->y1) row1 = clip->y1 - y;
	if (y + row0 < 0) row0 = -y;
	if (y + row1 >= height_) row1 = height_ - 1 - y;
	if (row0 > row1) return;
	for (int row = row0; row <= row1; row++) {
		int sy = y + row;

		int sx0 = x + spans[row][0];
		int sx1 = x + spans[row][1];
		if (sx0 < clip->x0) sx0 = clip->x0;
		if (sx1 > clip->x1) sx1 = clip->x1;
		if (sx0 < 0) sx0 = 0;
		if (sx1 >= width_) sx1 = width_ - 1;
		if (sx0 > sx1) continue;

		fillSpanGlobalAlpha(rowToPhysical(sy), sx0, sx1 - sx0 + 1, color);
	}
	markDirtyClipped(x, y, x + size - 1, y + size - 1);
}

void Canvas::fillRoundedRectBoxesRgb565(const std::int16_t *xs,
                                        const std::int16_t *ys,
                                        int count,
                                        int w,
                                        int h,
                                        int tl,
                                        int tr,
                                        int br,
                                        int bl,
                                        const pixel::native_t *colors)
{
	if (!pixels_ || !xs || !ys || !colors || count <= 0 || w <= 0 || h <= 0) return;

	// AA-off same-size CSS circles: draw the whole batch in one tight loop sharing
	// row spans, with ONE union markDirty. The JSX bubble-grid reaches here as
	// transformed rounded boxes; its scaled sizes are often odd and radii are rounded,
	// so the old exact even-size test missed the hot path and fell into the generic
	// four-corner rounded-rect raster.
	if (Canvas::antialiasSamples() < 2 && roundedRectIsCircleLike(w, h, tl, tr, br, bl) && h <= 256) {
		const std::uint8_t (*cachedSpans)[2] =
			(w == h) ? CanvasMath::instance().circleBoxSpans(w) : nullptr;
		std::uint16_t spanX0[256];
		std::uint16_t spanX1[256];
		if (!cachedSpans) {
			const long long ww = static_cast<long long>(w) * static_cast<long long>(w);
			const long long hh = static_cast<long long>(h) * static_cast<long long>(h);
			for (int row = 0; row < h; row++) {
				const long long dy2 = static_cast<long long>(row * 2 + 1 - h);
				long long inside = hh - dy2 * dy2;
				if (inside < 0) inside = 0;
				const int dx2 = CanvasMath::integerSqrt(static_cast<int>((ww * inside) / hh));
				int sx0 = (w - dx2) / 2;
				int sx1 = (w + dx2 - 1) / 2;
				if (sx0 < 0) sx0 = 0;
				if (sx1 >= w) sx1 = w - 1;
				spanX0[row] = static_cast<std::uint16_t>(sx0);
				spanX1[row] = static_cast<std::uint16_t>(sx1);
			}
		}

		const ClipRect *clip = &clipStack_[clipDepth_];
		int dirtyX0 = width_;
		int dirtyY0 = height_;
		int dirtyX1 = -1;
		int dirtyY1 = -1;
		for (int i = 0; i < count; i++) {
			const int x = xs[i];
			const int y = ys[i];
			if (x > clip->x1 || y > clip->y1 || x + w - 1 < clip->x0 || y + h - 1 < clip->y0) continue;
			int boxX0 = x < clip->x0 ? clip->x0 : x;
			int boxY0 = y < clip->y0 ? clip->y0 : y;
			int boxX1 = (x + w - 1 > clip->x1) ? clip->x1 : x + w - 1;
			int boxY1 = (y + h - 1 > clip->y1) ? clip->y1 : y + h - 1;
			if (boxX0 < 0) boxX0 = 0;
			if (boxY0 < 0) boxY0 = 0;
			if (boxX1 >= width_) boxX1 = width_ - 1;
			if (boxY1 >= height_) boxY1 = height_ - 1;
			if (boxX0 > boxX1 || boxY0 > boxY1) continue;
			if (boxX0 < dirtyX0) dirtyX0 = boxX0;
			if (boxY0 < dirtyY0) dirtyY0 = boxY0;
			if (boxX1 > dirtyX1) dirtyX1 = boxX1;
			if (boxY1 > dirtyY1) dirtyY1 = boxY1;

			int row0 = 0;
			int row1 = h - 1;
			if (y + row0 < clip->y0) row0 = clip->y0 - y;
			if (y + row1 > clip->y1) row1 = clip->y1 - y;
			if (y + row0 < 0) row0 = -y;
			if (y + row1 >= height_) row1 = height_ - 1 - y;
			const pixel::native_t color = colors[i];
			for (int row = row0; row <= row1; row++) {
				const int sy = y + row;
				int sx0 = x + static_cast<int>(cachedSpans ? cachedSpans[row][0] : spanX0[row]);
				int sx1 = x + static_cast<int>(cachedSpans ? cachedSpans[row][1] : spanX1[row]);
				if (sx0 < clip->x0) sx0 = clip->x0;
				if (sx1 > clip->x1) sx1 = clip->x1;
				if (sx0 < 0) sx0 = 0;
				if (sx1 >= width_) sx1 = width_ - 1;
				if (sx0 > sx1) continue;
				fillSpanGlobalAlpha(rowToPhysical(sy), sx0, sx1 - sx0 + 1, color);
			}
		}
		if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
			markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
		return;
	}

	for (int i = 0; i < count; i++)
		fillRoundedRect(xs[i], ys[i], w, h, tl, tr, br, bl, colors[i]);
}

void Canvas::fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, pixel::native_t color)
{
	if (!pixels_ || w <= 0 || h <= 0) return;
	int max_r = CanvasMath::min(w / 2, h / 2);
	if (tl > max_r) tl = max_r;
	if (tr > max_r) tr = max_r;
	if (br > max_r) br = max_r;
	if (bl > max_r) bl = max_r;
	if (tl < 0) tl = 0;
	if (tr < 0) tr = 0;
	if (br < 0) br = 0;
	if (bl < 0) bl = 0;
	if ((tl | tr | br | bl) == 0) {
		fillRect(x, y, w, h, color);
		return;
	}
	// AA-off circle fast fill: an opaque circle (square box, radius = half) with AA
	// disabled goes straight to the precomputed circleSpans lookup + tight word-fill,
	// skipping the general per-row addSpan/sqrt scanline below. This is golden's
	// 62fps circle path (its poles are marginally rounder than the integer scanline).
	if (Canvas::antialiasSamples() < 2 && w == h &&
			tl == w / 2 && tr == w / 2 && br == w / 2 && bl == w / 2 &&
			CanvasMath::instance().circleSpans(w)) {
		fillCircleBox(x, y, w, color);
		return;
	}
	int top_r = tl > tr ? tl : tr;
	int bot_r = bl > br ? bl : br;
	if (globalAlpha_ == 0) return;

	const int aaSamples = Canvas::antialiasSamples();
	const bool useAntialias = aaSamples >= 2;
	const int aaSampleCount = useAntialias ? aaSamples * aaSamples : 1;
	const float kernelWidth = useAntialias ? roundedRectAntialiasKernelWidth(aaSamples, w, h, tl, tr, br, bl) : 1.0f;
	const int edgePad = useAntialias ? antialiasEdgePad(kernelWidth) : 0;
	const bool coverageSampleFill = useAntialias && roundedRectShouldCoverageSampleFill(w, h);

	const ClipRect *clip = &clipStack_[clipDepth_];
	int row0 = y;
	int row1 = y + h - 1;
	if (row0 < clip->y0) row0 = clip->y0;
	if (row1 > clip->y1) row1 = clip->y1;
	if (row0 < 0) row0 = 0;
	if (row1 >= height_) row1 = height_ - 1;
	if (row0 > row1) return;

	int dirtyX0 = width_;
	int dirtyY0 = height_;
	int dirtyX1 = -1;
	int dirtyY1 = -1;
	const int a = globalAlpha_;

#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	// Small AA rounded boxes (including the 16px JSX balls) normally sample the
	// whole shape. Traverse logical columns on a rotated canvas so those exact
	// same coverage writes are contiguous in the native framebuffer.
	if (landscapeRot_ && coverageSampleFill) {
		int col0 = x;
		int col1 = x + w - 1;
		if (col0 < clip->x0) col0 = clip->x0;
		if (col1 > clip->x1) col1 = clip->x1;
		if (col0 < 0) col0 = 0;
		if (col1 >= width_) col1 = width_ - 1;
		if (col0 > col1) return;
		for (int sx = col0; sx <= col1; ++sx) {
#if !GEA_PIXEL_STORAGE_PACKED
			pixel::native_t *dst = &pixels_[rotPhysRow(sx) * stride_ + row0];
#endif
			for (int sy = row0; sy <= row1; ++sy) {
				const int coverage = roundedRectCoverage(
					x, y, w, h, tl, tr, br, bl, sx, sy, aaSamples, kernelWidth);
				if (coverage <= 0) {
#if !GEA_PIXEL_STORAGE_PACKED
					++dst;
#endif
					continue;
				}
				const int effectiveAlpha = coverageAlpha(a, coverage, aaSampleCount);
				if (effectiveAlpha > 0) {
#if GEA_PIXEL_STORAGE_PACKED
					if (effectiveAlpha >= 255) rotSet(sx, sy, color);
					else rotBlend(sx, sy, color, effectiveAlpha);
#else
					*dst = effectiveAlpha >= 255
						? color
						: pixel::blendNative(color, *dst, effectiveAlpha);
#endif
				}
#if !GEA_PIXEL_STORAGE_PACKED
				++dst;
#endif
			}
		}
		markDirty(col0, row0, col1, row1);
		return;
	}

	// Rotated canvas, antialiasing off: the row loop below fills each row with
	// `fillSpanColor`, and on this binding a logical row is a native COLUMN --
	// one store per native row, a PSRAM cache line each. Measured at ~1.6us per
	// pixel on a pedalboard app's 502x410 root background, which is 330ms of a
	// 451ms frame; the same area filled contiguously is single-digit ms.
	//
	// The coverage sampler above already walks columns, but it is reached only
	// for antialiased shapes of at most 4096px, so a full-screen background --
	// the one shape where this costs the most -- never sees it.
	//
	// A rounded rect is convex in every column exactly as it is in every row, so
	// the shape is walked column by column here and each column becomes one
	// contiguous native run, which is what `fillRect` already does in this
	// binding. The corner solve is the transpose of the row loop's: a column
	// inside a corner's radius is inset from the top (or the bottom) by the same
	// `integerSqrt` chord that insets the row loop's span from the side.
	//
	// Opaque only. A translucent fill has to read what it blends over, so it
	// gains nothing from a contiguous run and keeps the path it had.
	if (landscapeRot_ && !useAntialias && a >= 255) {
		int col0 = x;
		int col1 = x + w - 1;
		if (col0 < clip->x0) col0 = clip->x0;
		if (col1 > clip->x1) col1 = clip->x1;
		if (col0 < 0) col0 = 0;
		if (col1 >= width_) col1 = width_ - 1;
		if (col0 > col1) return;
		int dirtyCol0 = width_;
		int dirtyCol1 = -1;
		int dirtyRow0 = height_;
		int dirtyRow1 = -1;
		for (int sx = col0; sx <= col1; sx++) {
			int top = y;
			int bottom = y + h - 1;
			if (tl > 0 && sx < x + tl) {
				const int dx = x + tl - sx;
				const int edge = y + tl - CanvasMath::integerSqrt(tl * tl - dx * dx);
				if (edge > top) top = edge;
			}
			if (tr > 0 && sx > x + w - 1 - tr) {
				const int dx = sx - (x + w - 1 - tr);
				const int edge = y + tr - CanvasMath::integerSqrt(tr * tr - dx * dx);
				if (edge > top) top = edge;
			}
			if (bl > 0 && sx < x + bl) {
				const int dx = x + bl - sx;
				const int edge = y + h - 1 - bl + CanvasMath::integerSqrt(bl * bl - dx * dx);
				if (edge < bottom) bottom = edge;
			}
			if (br > 0 && sx > x + w - 1 - br) {
				const int dx = sx - (x + w - 1 - br);
				const int edge = y + h - 1 - br + CanvasMath::integerSqrt(br * br - dx * dx);
				if (edge < bottom) bottom = edge;
			}
			if (top < row0) top = row0;
			if (bottom > row1) bottom = row1;
			if (top > bottom) continue;
#if GEA_PIXEL_STORAGE_PACKED
			for (int sy = top; sy <= bottom; ++sy) rotSet(sx, sy, color);
#else
			pixel::fillNative(&pixels_[rotPhysRow(sx) * stride_ + top], bottom - top + 1, color);
#endif
			if (sx < dirtyCol0) dirtyCol0 = sx;
			if (sx > dirtyCol1) dirtyCol1 = sx;
			if (top < dirtyRow0) dirtyRow0 = top;
			if (bottom > dirtyRow1) dirtyRow1 = bottom;
		}
		if (dirtyCol0 <= dirtyCol1 && dirtyRow0 <= dirtyRow1)
			markDirty(dirtyCol0, dirtyRow0, dirtyCol1, dirtyRow1);
		return;
	}
#endif

	for (int sy = row0; sy <= row1; sy++) {
		int sx0 = width_;
		int sx1 = -1;
		auto addSpan = [&](int left, int right) {
			if (left > right) return;
			if (left < clip->x0) left = clip->x0;
			if (right > clip->x1) right = clip->x1;
			if (left < 0) left = 0;
			if (right >= width_) right = width_ - 1;
			if (left > right) return;
			if (left < sx0) sx0 = left;
			if (right > sx1) sx1 = right;
		};

		if (h - top_r - bot_r > 0 && sy >= y + top_r && sy < y + h - bot_r)
			addSpan(x, x + w - 1);
		if (top_r > 0 && sy >= y && sy < y + top_r)
			addSpan(x + tl, x + w - tr - 1);
		if (bot_r > 0 && sy >= y + h - bot_r && sy <= y + h - 1)
			addSpan(x + bl, x + w - br - 1);
		if (tl > 0 && tl < top_r && sy >= y + tl && sy < y + top_r)
			addSpan(x, x + tl - 1);
		if (tr > 0 && tr < top_r && sy >= y + tr && sy < y + top_r)
			addSpan(x + w - tr, x + w - 1);
		if (bl > 0 && bl < bot_r && sy >= y + h - bot_r && sy < y + h - bl)
			addSpan(x, x + bl - 1);
		if (br > 0 && br < bot_r && sy >= y + h - bot_r && sy < y + h - br)
			addSpan(x + w - br, x + w - 1);

		if (tl > 0 && sy >= y && sy <= y + tl) {
			const int dy = y + tl - sy;
			const int dx = CanvasMath::integerSqrt(tl * tl - dy * dy);
			addSpan(x + tl - dx, x + tl);
		}
		if (tr > 0 && sy >= y && sy <= y + tr) {
			const int dy = y + tr - sy;
			const int dx = CanvasMath::integerSqrt(tr * tr - dy * dy);
			addSpan(x + w - 1 - tr, x + w - 1 - tr + dx);
		}
		if (bl > 0 && sy >= y + h - 1 - bl && sy <= y + h - 1) {
			const int dy = sy - (y + h - 1 - bl);
			const int dx = CanvasMath::integerSqrt(bl * bl - dy * dy);
			addSpan(x + bl - dx, x + bl);
		}
		if (br > 0 && sy >= y + h - 1 - br && sy <= y + h - 1) {
			const int dy = sy - (y + h - 1 - br);
			const int dx = CanvasMath::integerSqrt(br * br - dy * dy);
			addSpan(x + w - 1 - br, x + w - 1 - br + dx);
		}

		int anyX0 = sx0;
		int anyX1 = sx1;
		if (useAntialias) {
			anyX0 = width_;
			anyX1 = -1;
			roundedRectAnyCoverageSpan(x, y, w, h, tl, tr, br, bl, sy, aaSamples, kernelWidth, &anyX0, &anyX1);
			if (anyX0 < clip->x0) anyX0 = clip->x0;
			if (anyX1 > clip->x1) anyX1 = clip->x1;
			if (anyX0 < x) anyX0 = x;
			if (anyX1 > x + w - 1) anyX1 = x + w - 1;
			if (anyX0 < 0) anyX0 = 0;
			if (anyX1 >= width_) anyX1 = width_ - 1;
			if (sx0 < anyX0) sx0 = anyX0;
			if (sx1 > anyX1) sx1 = anyX1;
		}
		if (anyX0 > anyX1) continue;

			auto paintCoverageEdge = [&](int px) {
			if (px < clip->x0 || px > clip->x1 || px < 0 || px >= width_) return;
			const int coverage = useAntialias
			    ? roundedRectCoverage(x, y, w, h, tl, tr, br, bl, px, sy, aaSamples, kernelWidth)
			    : (roundedRectSampleContains(x,
			                                 y,
			                                 w,
			                                 h,
			                                 tl,
			                                 tr,
			                                 br,
			                                 bl,
			                                 static_cast<float>(px) + 0.5f,
			                                 static_cast<float>(sy) + 0.5f)
			           ? 1
			           : 0);
			if (coverage <= 0) return;
			const int effectiveAlpha = coverageAlpha(a, coverage, aaSampleCount);
			if (effectiveAlpha <= 0) return;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) rotBlend(px, sy, color, effectiveAlpha);
			else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
			pixel::packed::blendPixel(packedRow(rowToPhysical(sy)), px, color, effectiveAlpha);
#else
			pixel::native_t &dst = pixels_[rowToPhysical(sy) * stride_ + px];
			const pixel::native_t c = color;
			dst = (effectiveAlpha >= 255) ? c : pixel::blendNative(c, dst, effectiveAlpha);
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			}
#endif
			if (px < dirtyX0) dirtyX0 = px;
			if (sy < dirtyY0) dirtyY0 = sy;
			if (px > dirtyX1) dirtyX1 = px;
			if (sy > dirtyY1) dirtyY1 = sy;
		};

		if (coverageSampleFill) {
			for (int px = anyX0; px <= anyX1; ++px) paintCoverageEdge(px);
			continue;
		} else if (useAntialias) {
			if (sx0 <= sx1) {
				const int leftEdgeEnd = std::min(anyX1, sx0 + edgePad);
				const int rightEdgeStart = std::max(anyX0, sx1 - edgePad);
				if (leftEdgeEnd >= rightEdgeStart) {
					for (int px = anyX0; px <= anyX1; ++px) paintCoverageEdge(px);
				} else {
					for (int px = anyX0; px <= leftEdgeEnd; ++px) paintCoverageEdge(px);
					for (int px = rightEdgeStart; px <= anyX1; ++px) paintCoverageEdge(px);
				}
			} else {
				for (int px = anyX0; px <= anyX1; ++px) paintCoverageEdge(px);
			}
		} else if (sx0 > sx1) {
			continue;
		}

		// AA off: the integer span [sx0, sx1] from the integerSqrt corner solve is the
		// full opaque shape — fill it directly, with no per-pixel edge coverage
		// (roundedRectSampleContains). AA on carves the antialiased edge band (edgePad)
		// out of the solid fill and leaves those edges to the coverage passes above.
		int fillX0 = useAntialias ? sx0 + edgePad + 1 : sx0;
		int fillX1 = useAntialias ? sx1 - edgePad - 1 : sx1;
		if (fillX0 < clip->x0) fillX0 = clip->x0;
		if (fillX1 > clip->x1) fillX1 = clip->x1;
		if (fillX0 < 0) fillX0 = 0;
		if (fillX1 >= width_) fillX1 = width_ - 1;
		if (fillX0 <= fillX1) {
			if (a >= 255) {
				fillSpanColor(rowToPhysical(sy), fillX0, fillX1 - fillX0 + 1, color);
			} else {
				const pixel::native_t c = color;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
				if (landscapeRot_) {
					for (int px = fillX0; px <= fillX1; ++px) rotBlend(px, sy, c, a);
				} else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
				std::uint8_t *dstNib = packedRow(rowToPhysical(sy));
				for (int px = fillX0; px <= fillX1; ++px) pixel::packed::blendPixel(dstNib, px, c, a);
#else
				pixel::native_t *dst = &pixels_[rowToPhysical(sy) * stride_ + fillX0];
				for (int px = fillX0; px <= fillX1; ++px, ++dst)
					*dst = pixel::blendNative(c, *dst, a);
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
				}
#endif
			}
			if (fillX0 < dirtyX0) dirtyX0 = fillX0;
			if (sy < dirtyY0) dirtyY0 = sy;
			if (fillX1 > dirtyX1) dirtyX1 = fillX1;
			if (sy > dirtyY1) dirtyY1 = sy;
		}
	}
	if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
		markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
}

void Canvas::strokeRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, pixel::native_t color)
{
	if (!pixels_ || w <= 0 || h <= 0 || lw <= 0) return;
	const ClipRect *clip = &clipStack_[clipDepth_];
	int max_r = CanvasMath::min(w / 2, h / 2);
	if (tl > max_r) tl = max_r;
	if (tr > max_r) tr = max_r;
	if (br > max_r) br = max_r;
	if (bl > max_r) bl = max_r;
	if (tl < 0) tl = 0;
	if (tr < 0) tr = 0;
	if (br < 0) br = 0;
	if (bl < 0) bl = 0;
	if (globalAlpha_ == 0) return;
	const int aaSamples = Canvas::antialiasSamples();
	if (aaSamples >= 2) {
		const float kernelWidth = roundedRectAntialiasKernelWidth(aaSamples, w, h, tl, tr, br, bl);
		const int edgePad = antialiasEdgePad(kernelWidth);
		int row0 = y - edgePad;
		int row1 = y + h - 1 + edgePad;
		if (row0 < clip->y0) row0 = clip->y0;
		if (row1 > clip->y1) row1 = clip->y1;
		if (row0 < 0) row0 = 0;
		if (row1 >= height_) row1 = height_ - 1;
		int col0 = x - edgePad;
		int col1 = x + w - 1 + edgePad;
		if (col0 < clip->x0) col0 = clip->x0;
		if (col1 > clip->x1) col1 = clip->x1;
		if (col0 < 0) col0 = 0;
		if (col1 >= width_) col1 = width_ - 1;
		if (row0 > row1 || col0 > col1) return;

		int dirtyX0 = width_;
		int dirtyY0 = height_;
		int dirtyX1 = -1;
		int dirtyY1 = -1;
		const int aaSampleCount = aaSamples * aaSamples;
		const int alpha = globalAlpha_;
		for (int sy = row0; sy <= row1; ++sy) {
			for (int sx = col0; sx <= col1; ++sx) {
				const int coverage = roundedRectStrokeCoverage(x, y, w, h,
				                                               tl, tr, br, bl,
				                                               lw,
				                                               sx, sy,
				                                               aaSamples,
				                                               kernelWidth);
				const int effectiveAlpha = coverageAlpha(alpha, coverage, aaSampleCount);
				if (effectiveAlpha <= 0) continue;
				const pixel::native_t c = color;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
				if (landscapeRot_) rotBlend(sx, sy, c, effectiveAlpha);
				else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
				pixel::packed::blendPixel(packedRow(rowToPhysical(sy)), sx, c, effectiveAlpha);
#else
				pixel::native_t &dst = pixels_[rowToPhysical(sy) * stride_ + sx];
				dst = (effectiveAlpha >= 255) ? c : pixel::blendNative(c, dst, effectiveAlpha);
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
				}
#endif
				if (sx < dirtyX0) dirtyX0 = sx;
				if (sy < dirtyY0) dirtyY0 = sy;
				if (sx > dirtyX1) dirtyX1 = sx;
				if (sy > dirtyY1) dirtyY1 = sy;
			}
		}
		if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
			markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
		return;
	}
	fillRect(x + tl, y, w - tl - tr, lw, color);
	fillRect(x + bl, y + h - lw, w - bl - br, lw, color);
	fillRect(x, y + tl, lw, h - tl - bl, color);
	fillRect(x + w - lw, y + tr, lw, h - tr - br, color);
	for (int q = 0; q < 4; q++) {
		int r, cx, cy;
		switch (q) {
		case 0: r = tr; cx = x + w - 1 - r; cy = y + r; break;
		case 1: r = tl; cx = x + r;         cy = y + r; break;
		case 2: r = bl; cx = x + r;         cy = y + h - 1 - r; break;
		case 3: r = br; cx = x + w - 1 - r; cy = y + h - 1 - r; break;
		default: continue;
		}
		if (r <= 0) continue;
		if (cx + r < clip->x0 || cx - r > clip->x1 ||
		    cy + r < clip->y0 || cy - r > clip->y1)
			continue;
		int ri = r - lw;
		if (ri < 0) ri = 0;
		int dy0 = 0;
		int dy1 = r;
		if (q == 0 || q == 1) {
			if (cy - dy0 > clip->y1) dy0 = cy - clip->y1;
			if (cy - dy1 < clip->y0) dy1 = cy - clip->y0;
		} else {
			if (cy + dy0 < clip->y0) dy0 = clip->y0 - cy;
			if (cy + dy1 > clip->y1) dy1 = clip->y1 - cy;
		}
		if (dy0 < 0) dy0 = 0;
		if (dy1 > r) dy1 = r;
		if (dy0 > dy1) continue;
		for (int dy = dy0; dy <= dy1; dy++) {
			int dx_outer = CanvasMath::integerSqrt(r * r - dy * dy);
			int dx_inner = (dy <= ri) ? CanvasMath::integerSqrt(ri * ri - dy * dy) : 0;
			int sx0, sx1, sy;
			switch (q) {
			case 0: sx0 = cx + dx_inner; sx1 = cx + dx_outer; sy = cy - dy; break;
			case 1: sx0 = cx - dx_outer; sx1 = cx - dx_inner; sy = cy - dy; break;
			case 2: sx0 = cx - dx_outer; sx1 = cx - dx_inner; sy = cy + dy; break;
			case 3: sx0 = cx + dx_inner; sx1 = cx + dx_outer; sy = cy + dy; break;
			default: continue;
			}
			if (sy < clip->y0 || sy > clip->y1) continue;
			if (sx0 < clip->x0) sx0 = clip->x0;
			if (sx1 > clip->x1) sx1 = clip->x1;
			if (sx0 < 0) sx0 = 0;
			if (sx1 >= width_) sx1 = width_ - 1;
			if (sx0 > sx1 || sy < 0 || sy >= height_) continue;

			fillSpanGlobalAlpha(rowToPhysical(sy), sx0, sx1 - sx0 + 1, color);
		}
	}
	markDirtyClipped(x, y, x + w - 1, y + h - 1);
}

#if GEA_PIXEL_STORAGE_PACKED
void Canvas::blitPacked(const std::uint8_t *src, int src_w, int src_h, int dx, int dy)
{
	if (!pixels_ || !src || src_w <= 0 || src_h <= 0) return;
	const ClipRect *clip = &clipStack_[clipDepth_];
	const int srcRowBytes = pixel::packed::rowBytes(src_w);
	for (int r = 0; r < src_h; r++) {
		const int py = dy + r;
		if (py < clip->y0 || py > clip->y1 || py < 0 || py >= height_) continue;
		int srcX = 0;
		int dstX0 = dx;
		int dstX1 = dx + src_w - 1;
		if (dstX0 < clip->x0) { srcX += clip->x0 - dstX0; dstX0 = clip->x0; }
		if (dstX0 < 0) { srcX += -dstX0; dstX0 = 0; }
		if (dstX1 > clip->x1) dstX1 = clip->x1;
		if (dstX1 >= width_) dstX1 = width_ - 1;
		if (dstX0 > dstX1) continue;
		const int count = dstX1 - dstX0 + 1;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		if (landscapeRot_) {
			const std::uint8_t *srcRow = src + static_cast<std::size_t>(r) * srcRowBytes;
			for (int i = 0; i < count; ++i) rotSet(dstX0 + i, py, pixel::packed::get(srcRow, srcX + i));
		} else
#endif
		pixel::packed::copyPacked(packedRow(rowToPhysical(py)), dstX0, src + static_cast<std::size_t>(r) * srcRowBytes, srcX, count);
	}
	markDirty(dx, dy, dx + src_w - 1, dy + src_h - 1);
}
#endif

void Canvas::drawImage(
	const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	int dx, int dy)
{
	if (!pixels_ || !src) return;
	if (src_w <= 0 || src_h <= 0) return;

	// Clip the source rect once instead of per-pixel.
	const ClipRect *clip = &clipStack_[clipDepth_];
	int clip_x0 = clip->x0 > 0 ? clip->x0 : 0;
	int clip_y0 = clip->y0 > 0 ? clip->y0 : 0;
	int clip_x1 = clip->x1 < width_  ? clip->x1 : width_  - 1;
	int clip_y1 = clip->y1 < height_ ? clip->y1 : height_ - 1;

	int sx0 = 0, sy0 = 0, sx1 = src_w - 1, sy1 = src_h - 1;
	if (dx + sx0 < clip_x0) sx0 = clip_x0 - dx;
	if (dx + sx1 > clip_x1) sx1 = clip_x1 - dx;
	if (dy + sy0 < clip_y0) sy0 = clip_y0 - dy;
	if (dy + sy1 > clip_y1) sy1 = clip_y1 - dy;
	if (sx0 > sx1 || sy0 > sy1) return;

	int row_w = sx1 - sx0 + 1;
	int dst_x = dx + sx0;
	std::uint8_t global_alpha = globalAlpha_;
	if (global_alpha == 0) return;

	// Fast path: fully opaque drawImage (no per-pixel mask, no global alpha) -
	// becomes a row-level memcpy, dropping the per-pixel function-call and
	// clip-check overhead that dominated when many small tiles were drawn.
	if (!alpha && global_alpha == 255) {
		for (int sy = sy0; sy <= sy1; sy++) {
			const pixel::native_t *src_row = &src[sy * src_w + sx0];
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) { for (int i = 0; i < row_w; ++i) rotSet(dst_x + i, dy + sy, src_row[i]); }
			else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
			pixel::packed::writeUnpacked(packedRow(rowToPhysical(dy + sy)), dst_x, src_row, row_w);
#else
			pixel::native_t *dst_row = &pixels_[rowToPhysical(dy + sy) * stride_ + dst_x];
			std::memcpy(dst_row, src_row, static_cast<std::size_t>(row_w) * sizeof(pixel::native_t));
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			}
#endif
		}
		markDirty(dst_x, dy + sy0, dx + sx1, dy + sy1);
		return;
	}

	// Global-alpha blend without per-pixel mask.
	if (!alpha) {
		for (int sy = sy0; sy <= sy1; sy++) {
			const pixel::native_t *src_row = &src[sy * src_w + sx0];
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) {
				for (int i = 0; i < row_w; i++) rotBlend(dst_x + i, dy + sy, src_row[i], global_alpha);
			} else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
			std::uint8_t *dnib = packedRow(rowToPhysical(dy + sy));
			for (int i = 0; i < row_w; i++)
				pixel::packed::blendPixel(dnib, dst_x + i, src_row[i], global_alpha);
#else
			pixel::native_t *dst_row = &pixels_[rowToPhysical(dy + sy) * stride_ + dst_x];
			for (int i = 0; i < row_w; i++)
				dst_row[i] = pixel::blendNative(src_row[i], dst_row[i], global_alpha);
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			}
#endif
		}
		markDirty(dst_x, dy + sy0, dx + sx1, dy + sy1);
		return;
	}

	// Per-pixel alpha mask, optionally combined with global alpha.
	// For the common global_alpha == 255 case, walk each row as a run-length
	// pass: skip transparent runs (a == 0), memcpy opaque runs (a == 255), and
	// per-pixel blend any in-between values. With the opaque hint at decode
	// time, tiles boil down to {0, 255} alpha and this becomes "memcpy the
	// solid spans, skip the holes" - far cheaper than per-pixel branching for
	// every pixel of every row.
	for (int sy = sy0; sy <= sy1; sy++) {
		const pixel::native_t *src_row = &src[sy * src_w + sx0];
		const std::uint8_t  *alpha_row = &alpha[sy * src_w + sx0];
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
		if (landscapeRot_) {
			const int py = dy + sy;
			if (global_alpha == 255) {
				for (int i = 0; i < row_w; i++) {
					const std::uint8_t a = alpha_row[i];
					if (a == 0) continue;
					if (a == 255) rotSet(dst_x + i, py, src_row[i]);
					else rotBlend(dst_x + i, py, src_row[i], a);
				}
			} else {
				for (int i = 0; i < row_w; i++) {
					const std::uint8_t a = alpha_row[i];
					if (a == 0) continue;
					const int eff = (a * global_alpha) / 255;
					if (eff <= 0) continue;
					rotBlend(dst_x + i, py, src_row[i], eff);
				}
			}
			continue;
		}
#endif
#if GEA_PIXEL_STORAGE_PACKED
		std::uint8_t *dnib = packedRow(rowToPhysical(dy + sy));
		if (global_alpha == 255) {
			int i = 0;
			while (i < row_w) {
				while (i < row_w && alpha_row[i] == 0) i++;
				int run_start = i;
				while (i < row_w && alpha_row[i] == 255) i++;
				if (i > run_start)
					pixel::packed::writeUnpacked(dnib, dst_x + run_start, src_row + run_start, i - run_start);
				if (i < row_w && alpha_row[i] != 0 && alpha_row[i] != 255) {
					pixel::packed::blendPixel(dnib, dst_x + i, src_row[i], alpha_row[i]);
					i++;
				}
			}
			continue;
		}
		for (int i = 0; i < row_w; i++) {
			std::uint8_t a = alpha_row[i];
			if (a == 0) continue;
			int eff = (a * global_alpha) / 255;
			if (eff <= 0) continue;
			pixel::packed::blendPixel(dnib, dst_x + i, src_row[i], eff);
		}
#else
		pixel::native_t *dst_row = &pixels_[rowToPhysical(dy + sy) * stride_ + dst_x];

		if (global_alpha == 255) {
			int i = 0;
			while (i < row_w) {
				while (i < row_w && alpha_row[i] == 0) i++;
				int run_start = i;
				while (i < row_w && alpha_row[i] == 255) i++;
				if (i > run_start) {
					std::memcpy(dst_row + run_start, src_row + run_start, (size_t)(i - run_start) * sizeof(pixel::native_t));
				}
				if (i < row_w && alpha_row[i] != 0 && alpha_row[i] != 255) {
					dst_row[i] = pixel::blendNative(src_row[i], dst_row[i], alpha_row[i]);
					i++;
				}
			}
			continue;
		}

		for (int i = 0; i < row_w; i++) {
			std::uint8_t a = alpha_row[i];
			if (a == 0) continue;
			int eff = (a * global_alpha) / 255;
			if (eff <= 0) continue;
			if (eff >= 255) dst_row[i] = src_row[i];
			else dst_row[i] = pixel::blendNative(src_row[i], dst_row[i], (std::uint8_t)eff);
		}
#endif
	}
	markDirty(dst_x, dy + sy0, dx + sx1, dy + sy1);
}

void Canvas::drawImage(
	const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	int dx, int dy, int dst_w, int dst_h)
{
	if (!pixels_ || !src) return;
	if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

	// 1:1 scale: route to the unscaled blit and its opaque row-level memcpy
	// fast path. Callers (e.g. a map at an integer zoom) often pass dst == src
	// dimensions through the scaling overload; the bilinear loop below costs
	// ~70ms for a full 720x1280 composite that memcpy covers in ~2ms.
	if (dst_w == src_w && dst_h == src_h) {
		drawImage(src, alpha, src_w, src_h, dx, dy);
		return;
	}

	// Opaque nearest-neighbor fast path: map tiles (and other photos) load
	// opaque with no alpha buffer, and mid-gesture scaling doesn't need
	// bilinear filtering — pure int index mapping with a precomputed column
	// table is ~8x faster than the 4-sample float-bilinear loop below, which is
	// the difference between ~14fps and ~60fps while pinch-zooming a full
	// screen of tiles.
	// An alpha plane used to disqualify this fast path outright, dropping every
	// masked image onto the bilinear loop below. That is what a watchOS-style
	// icon grid is: a square sprite whose corners are transparent. Measured on
	// bubble-grid (30 icons, 410x502, esp32-s3), panning: PNG-with-alpha ran
	// 17-23fps against 59.6fps for the same icons flattened to opaque JPEG --
	// entirely this branch. Nearest sampling works exactly the same way for the
	// mask as for the colour (same index map), so the alpha case only adds a
	// per-pixel test on an already-sampled byte: skip a==0, copy a==255, blend
	// the rest. That keeps the run-length behaviour the 1:1 blit above already
	// relies on, where an anti-aliased disc is {0,255} apart from a thin edge.
	//
	// MINIFICATION ONLY. When the destination is no larger than the source, every
	// destination pixel already lands on a distinct source texel: nearest DROPS
	// texels, and the 4-tap bilinear below cannot put the dropped energy back
	// either (that needs a box/mip filter over the whole footprint), so the two
	// differ by well under a texel while bilinear costs ~8x. That decimation case
	// is what the fast path was measured on — a full screen of map tiles fitted to
	// the panel, and an icon grid whose sprites are drawn smaller than they load.
	// MAGNIFYING is the opposite: each source texel then covers more than one
	// destination pixel, so nearest replicates it into a visibly blocky run, and
	// interpolating between the neighbouring texels is the whole point of asking
	// for a bigger destination. A 2x2 image blitted at 4x4 must not come back as
	// four flat quadrants of the exact source colours. The row loop below is the
	// filtered path; its per-column source mapping is hoisted into a table there so
	// magnifying costs one bilinear blend per pixel rather than that plus an
	// integer divide per axis per pixel.
	if (globalAlpha_ == 255 && dst_w <= src_w && dst_h <= src_h) {
		const ClipRect *clip = &clipStack_[clipDepth_];
		const int clip_x0 = clip->x0 > 0 ? clip->x0 : 0;
		const int clip_y0 = clip->y0 > 0 ? clip->y0 : 0;
		const int clip_x1 = clip->x1 < width_ ? clip->x1 : width_ - 1;
		const int clip_y1 = clip->y1 < height_ ? clip->y1 : height_ - 1;
		const int x0 = dx > clip_x0 ? dx : clip_x0;
		const int y0 = dy > clip_y0 ? dy : clip_y0;
		const int x1 = dx + dst_w - 1 < clip_x1 ? dx + dst_w - 1 : clip_x1;
		const int y1 = dy + dst_h - 1 < clip_y1 ? dy + dst_h - 1 : clip_y1;
		const int count = x1 - x0 + 1;
		if (count <= 0 || y0 > y1) return;
		if (count <= 2048) {
			std::int16_t sxMap[2048];
			for (int x = x0; x <= x1; x++) {
				int sx = (((x - dx) * 2 + 1) * src_w) / (dst_w * 2);
				if (sx > src_w - 1) sx = src_w - 1;
				sxMap[x - x0] = static_cast<std::int16_t>(sx);
			}
			for (int y = y0; y <= y1; y++) {
				int sy = (((y - dy) * 2 + 1) * src_h) / (dst_h * 2);
				if (sy > src_h - 1) sy = src_h - 1;
				const pixel::native_t *src_row = &src[sy * src_w];
				const std::uint8_t *alpha_row = alpha ? &alpha[sy * src_w] : nullptr;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
				if (landscapeRot_) {
					if (!alpha_row) {
						for (int i = 0; i < count; i++) rotSet(x0 + i, y, src_row[sxMap[i]]);
					} else {
						for (int i = 0; i < count; i++) {
							const int sx = sxMap[i];
							const std::uint8_t a = alpha_row[sx];
							if (a == 0) continue;
							if (a == 255) rotSet(x0 + i, y, src_row[sx]);
							else rotBlend(x0 + i, y, src_row[sx], a);
						}
					}
				} else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
				std::uint8_t *dnib = packedRow(rowToPhysical(y));
				if (!alpha_row) {
					for (int i = 0; i < count; i++) pixel::packed::set(dnib, x0 + i, src_row[sxMap[i]]);
				} else {
					for (int i = 0; i < count; i++) {
						const int sx = sxMap[i];
						const std::uint8_t a = alpha_row[sx];
						if (a == 0) continue;
						if (a == 255) pixel::packed::set(dnib, x0 + i, src_row[sx]);
						else pixel::packed::blendPixel(dnib, x0 + i, src_row[sx], a);
					}
				}
#else
				pixel::native_t *dst_row = &pixels_[rowToPhysical(y) * stride_ + x0];
				if (!alpha_row) {
					for (int i = 0; i < count; i++) dst_row[i] = src_row[sxMap[i]];
				} else {
					for (int i = 0; i < count; i++) {
						const int sx = sxMap[i];
						const std::uint8_t a = alpha_row[sx];
						if (a == 0) continue;
						if (a == 255) dst_row[i] = src_row[sx];
						else dst_row[i] = pixel::blendNative(src_row[sx], dst_row[i], a);
					}
				}
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
				}
#endif
			}
			markDirtyClipped(dx, dy, dx + dst_w - 1, dy + dst_h - 1);
			return;
		}
	}

	auto sourceAxis = [](int dest, int srcSize, int dstSize, int *i0, int *i1, int *frac) {
		const int coord = (((dest * 2 + 1) * srcSize * 256) / (dstSize * 2)) - 128;
		if (coord <= 0 || srcSize == 1) {
			*i0 = 0;
			*i1 = 0;
			*frac = 0;
			return;
		}
		const int last = srcSize - 1;
		const int lastCoord = last * 256;
		if (coord >= lastCoord) {
			*i0 = last;
			*i1 = last;
			*frac = 0;
			return;
		}
		*i0 = coord / 256;
		*i1 = *i0 + 1;
		*frac = coord - *i0 * 256;
	};

	const ClipRect *clip = &clipStack_[clipDepth_];
	const int clip_x0 = clip->x0 > 0 ? clip->x0 : 0;
	const int clip_y0 = clip->y0 > 0 ? clip->y0 : 0;
	const int clip_x1 = clip->x1 < width_ ? clip->x1 : width_ - 1;
	const int clip_y1 = clip->y1 < height_ ? clip->y1 : height_ - 1;

	// The x-axis source mapping depends only on the destination COLUMN, so
	// recomputing it inside the row loop paid an integer multiply + divide per
	// destination pixel (Xtensa has no hardware divide). Precompute it once, the
	// same way the nearest path above precomputes its column table. `sourceAxis`
	// only ever reports i1 == i0 + 1 when frac > 0 (the three early returns all set
	// frac = 0), so the second index is recoverable from the fraction alone and the
	// table stays 3 bytes per column.
	constexpr int kScaledAxisTableMax = 2048;
	std::int16_t sxMap[kScaledAxisTableMax];
	std::uint8_t fxMap[kScaledAxisTableMax];
	const bool haveColumnTable = dst_w <= kScaledAxisTableMax && src_w <= 32767;
	if (haveColumnTable) {
		for (int x = 0; x < dst_w; x++) {
			int i0 = 0, i1 = 0, frac = 0;
			sourceAxis(x, src_w, dst_w, &i0, &i1, &frac);
			sxMap[x] = static_cast<std::int16_t>(i0);
			fxMap[x] = static_cast<std::uint8_t>(frac);
		}
	}

	for (int y = 0; y < dst_h; y++) {
		int py = dy + y;
		if (py < clip_y0 || py > clip_y1) continue;
		int sy0 = 0, sy1 = 0, fy = 0;
		sourceAxis(y, src_h, dst_h, &sy0, &sy1, &fy);
		const int wy0 = 256 - fy;
		const int wy1 = fy;
		for (int x = 0; x < dst_w; x++) {
			int px = dx + x;
			if (px < clip_x0 || px > clip_x1) continue;
			int sx0 = 0, sx1 = 0, fx = 0;
			if (haveColumnTable) {
				sx0 = sxMap[x];
				fx = fxMap[x];
				sx1 = fx > 0 ? sx0 + 1 : sx0;
			} else {
				sourceAxis(x, src_w, dst_w, &sx0, &sx1, &fx);
			}
			const int wx0 = 256 - fx;
			const int wx1 = fx;
			// FLOAT accumulators (no int64 anywhere). The previous int64 version cost
			// ~160ms for a 220x220 icon on this board: Xtensa LX7 has NO hardware
			// 64-bit divide AND no hardware int64->float, so both int64 divides and
			// int64->float conversions are software routines. Here `a*weight` fits in
			// int32 (<=255*65536), so every step is int32 multiply + hardware int32->
			// float (FLOAT.S) + FPU multiply/add/divide. The result is a 0..255 channel
			// and rSum == channel*alphaSum, so float rounding stays far below 1 LSB.
			float rSum = 0.0f;
			float gSum = 0.0f;
			float bSum = 0.0f;
			float alphaSum = 0.0f;
			auto addSample = [&](int sx, int sy, int weight) {
				if (weight <= 0) return;
				const int index = sy * src_w + sx;
				const int a = alpha ? alpha[index] : 255;
				if (a <= 0) return;
				int r = 0, g = 0, b = 0, sa = 0;
				pixel::unpackNative8(src[index], &r, &g, &b, &sa);
				const float aw = static_cast<float>(a * weight);  // a*weight fits int32
				rSum += static_cast<float>(r) * aw;
				gSum += static_cast<float>(g) * aw;
				bSum += static_cast<float>(b) * aw;
				alphaSum += aw;
			};
			addSample(sx0, sy0, wx0 * wy0);
			addSample(sx1, sy0, wx1 * wy0);
			addSample(sx0, sy1, wx0 * wy1);
			addSample(sx1, sy1, wx1 * wy1);
			if (alphaSum <= 0.0f) continue;
			const float invAlpha = 1.0f / alphaSum;
			const int outA = static_cast<int>(alphaSum * (1.0f / 65536.0f) + 0.5f);
			const int r = static_cast<int>(rSum * invAlpha + 0.5f);
			const int g = static_cast<int>(gSum * invAlpha + 0.5f);
			const int b = static_cast<int>(bSum * invAlpha + 0.5f);
			// Panel-endian-aware output: source samples were un-swapped by
			// unpackRgb565 above, so the averaged r,g,b are true channels. Emit in
			// framebuffer (byte-swapped) format; raw rgb565FromRgb888 made scaled
			// images / custom-font glyph atlases land un-swapped on PANEL_ENDIAN=1.
			const pixel::native_t c = pixel::packNative8(r, g, b, 255);
			if (outA >= 255) writePixel(px, py, c);
			else writePixelAlpha(px, py, c, static_cast<std::uint8_t>(outA));
		}
	}
	markDirtyClipped(dx, dy, dx + dst_w - 1, dy + dst_h - 1);
}

void Canvas::drawImageRotated90CW(
	const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	int dx, int dy, int dst_w, int dst_h)
{
	if (!pixels_ || !src) return;
	if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

	const ClipRect *clip = &clipStack_[clipDepth_];
	const int clip_x0 = clip->x0 > 0 ? clip->x0 : 0;
	const int clip_y0 = clip->y0 > 0 ? clip->y0 : 0;
	const int clip_x1 = clip->x1 < width_ ? clip->x1 : width_ - 1;
	const int clip_y1 = clip->y1 < height_ ? clip->y1 : height_ - 1;
	const int x0 = dx > clip_x0 ? dx : clip_x0;
	const int y0 = dy > clip_y0 ? dy : clip_y0;
	const int x1 = dx + dst_w - 1 < clip_x1 ? dx + dst_w - 1 : clip_x1;
	const int y1 = dy + dst_h - 1 < clip_y1 ? dy + dst_h - 1 : clip_y1;
	if (x0 > x1 || y0 > y1) return;

	const std::uint8_t globalAlpha = globalAlpha_;
	if (globalAlpha == 0) return;

	for (int py = y0; py <= y1; py++) {
		const int localY = py - dy;
		int sx = (((localY * 2 + 1) * src_w) / (dst_h * 2));
		if (sx < 0) sx = 0;
		if (sx >= src_w) sx = src_w - 1;
#if GEA_PIXEL_STORAGE_PACKED
		std::uint8_t *dnib = packedRow(rowToPhysical(py));
#else
		pixel::native_t *dstRow = &pixels_[rowToPhysical(py) * stride_ + x0];
#endif
		for (int px = x0; px <= x1; px++) {
			const int localX = px - dx;
			int sy = src_h - 1 - (((localX * 2 + 1) * src_h) / (dst_w * 2));
			if (sy < 0) sy = 0;
			if (sy >= src_h) sy = src_h - 1;
			const int srcIndex = sy * src_w + sx;
			const pixel::native_t source = src[srcIndex];
			int effectiveAlpha = globalAlpha;
			if (!alpha && globalAlpha == 255) effectiveAlpha = 255;
			else if (alpha) effectiveAlpha = (effectiveAlpha * alpha[srcIndex]) / 255;
			if (effectiveAlpha <= 0) continue;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) rotBlend(px, py, source, effectiveAlpha);
			else
#endif
#if GEA_PIXEL_STORAGE_PACKED
			pixel::packed::blendPixel(dnib, px, source, effectiveAlpha);
#else
			if (effectiveAlpha >= 255) dstRow[px - x0] = source;
			else dstRow[px - x0] = pixel::blendNative(source, dstRow[px - x0], static_cast<std::uint8_t>(effectiveAlpha));
#endif
		}
	}
	markDirtyClipped(dx, dy, dx + dst_w - 1, dy + dst_h - 1);
}

namespace {

int clampedCornerRadius(int radius, int w, int h)
{
	if (radius <= 0 || w <= 0 || h <= 0) return 0;
	return std::min(radius, std::min(w, h) / 2);
}

bool insideRoundedCorner(int offsetX, int offsetY, int radius)
{
	const int dx = radius - offsetX;
	const int dy = radius - offsetY;
	return dx * dx + dy * dy <= radius * radius;
}

bool insideRoundedImageRect(int x, int y, int w, int h, int tl, int tr, int br, int bl)
{
	tl = clampedCornerRadius(tl, w, h);
	tr = clampedCornerRadius(tr, w, h);
	br = clampedCornerRadius(br, w, h);
	bl = clampedCornerRadius(bl, w, h);
	if (tl > 0 && x < tl && y < tl) return insideRoundedCorner(x, y, tl);
	if (tr > 0 && x >= w - tr && y < tr) return insideRoundedCorner(w - 1 - x, y, tr);
	if (br > 0 && x >= w - br && y >= h - br) return insideRoundedCorner(w - 1 - x, h - 1 - y, br);
	if (bl > 0 && x < bl && y >= h - bl) return insideRoundedCorner(x, h - 1 - y, bl);
	return true;
}

}  // namespace

void Canvas::drawImageRounded(
	const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	int dx, int dy, int dst_w, int dst_h, int tl, int tr, int br, int bl)
{
	if (!pixels_ || !src) return;
	if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
	if (tl <= 0 && tr <= 0 && br <= 0 && bl <= 0) {
		drawImage(src, alpha, src_w, src_h, dx, dy, dst_w, dst_h);
		return;
	}

	const ClipRect *clip = &clipStack_[clipDepth_];
	const int x0 = std::max(std::max(dx, 0), clip->x0);
	const int y0 = std::max(std::max(dy, 0), clip->y0);
	const int x1 = std::min(std::min(dx + dst_w - 1, width_ - 1), clip->x1);
	const int y1 = std::min(std::min(dy + dst_h - 1, height_ - 1), clip->y1);
	if (x0 > x1 || y0 > y1) return;

	// CIRCLE fast path. A square destination of odd side 2r+1 with all four
	// radii at r is exactly the disc (lx-r)^2 + (ly-r)^2 <= r^2 -- identical to
	// what insideRoundedImageRect computes, corner by corner. A disc's row span
	// is analytic, so it can be resolved ONCE per row instead of testing every
	// pixel: the generic loop below re-clamps four corner radii and divides for
	// every pixel of the bounding box, including the ~21% that lie outside the
	// shape. Measured on bubble-grid (27 icons, esp32-s3) while dragging, which
	// re-rasterises the whole screen every frame: endBatch 85ms -> 11ms/frame,
	// 10.6fps -> 60fps.
	const int rr = clampedCornerRadius(tl, dst_w, dst_h);
	if (globalAlpha_ == 255 && tl == tr && tr == br && br == bl && dst_w == dst_h && dst_w == 2 * rr + 1 &&
	    dst_w <= 1024) {
		// The chunked present replays this command once per flush chunk, so the
		// column map is rebuilt several times per icon per frame -- one integer
		// division per destination column each time (~10k divisions/frame for a
		// screen of icons). At 1:1 the map is the identity and the row copy is a
		// memcpy, so skip building it entirely.
		const bool oneToOne = (dst_w == src_w && dst_h == src_h);
		std::int16_t sxMap[1024];
		if (!oneToOne) {
			for (int i = 0; i < dst_w; i++) {
				const int sx = (i * src_w) / dst_w;
				sxMap[i] = static_cast<std::int16_t>(sx < src_w - 1 ? sx : src_w - 1);
			}
		}
		// Reuse the circle rasteriser's memoised span table -- the same
		// CanvasMath::circleRadiusSpans() fillCircleNoDirty() uses. It already
		// holds [r-dx, r+dx] per row for this radius, so the disc costs a table
		// lookup per row instead of an integer sqrt, and repeat draws of the
		// same radius (every full-size icon, every chunk that touches it) pay
		// nothing at all.
		const std::uint8_t (*spans)[2] = CanvasMath::instance().circleRadiusSpans(rr);
		for (int py = y0; py <= y1; py++) {
			const int ly = py - dy;
			int lo, hi;
			if (spans) {
				lo = spans[ly][0];
				hi = spans[ly][1];
			} else {
				const int ddy = ly - rr;
				const int span2 = rr * rr - ddy * ddy;
				if (span2 < 0) continue;
				const int hw = CanvasMath::integerSqrt(span2);
				lo = rr - hw;
				hi = rr + hw;
			}
			int xa = dx + lo;
			int xb = dx + hi;
			if (xa < x0) xa = x0;
			if (xb > x1) xb = x1;
			if (xa > xb) continue;
			// At 1:1 the source row IS the destination row. (Measured neutral --
			// the compiler already strength-reduces it -- but it states the
			// invariant rather than rediscovering it by division.)
			const int sy = oneToOne ? ly : std::min((ly * src_h) / dst_h, src_h - 1);
			const pixel::native_t *src_row = &src[sy * src_w];
			const std::uint8_t *alpha_row = alpha ? &alpha[sy * src_w] : nullptr;
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) {
				for (int px = xa; px <= xb; px++) {
					const int sx = sxMap[px - dx];
					if (alpha_row) {
						const std::uint8_t a = alpha_row[sx];
						if (a == 0) continue;
						if (a == 255) rotSet(px, py, src_row[sx]);
						else rotBlend(px, py, src_row[sx], a);
					} else {
						rotSet(px, py, src_row[sx]);
					}
				}
			} else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
			std::uint8_t *dnib = packedRow(rowToPhysical(py));
			for (int px = xa; px <= xb; px++) {
				const int sx = sxMap[px - dx];
				if (alpha_row) {
					const std::uint8_t a = alpha_row[sx];
					if (a == 0) continue;
					if (a == 255) pixel::packed::set(dnib, px, src_row[sx]);
					else pixel::packed::blendPixel(dnib, px, src_row[sx], a);
				} else {
					pixel::packed::set(dnib, px, src_row[sx]);
				}
			}
#else
			pixel::native_t *dst_row = &pixels_[rowToPhysical(py) * stride_];
			// Hoist the mask test out of the pixel loop, and take a row memcpy
			// when the blit is 1:1 and opaque -- which is every full-size icon.
			// A per-pixel gather there costs ~5ms/frame more than memcpy on a
			// full-screen repaint.
			if (!alpha_row) {
				if (oneToOne) {
					// Inline word copy rather than std::memcpy: the disc is copied
					// one ROW SPAN at a time, so a screen of icons makes ~2.2k calls
					// per frame for ~140-byte runs, where the library call's setup is
					// a real fraction of the work. Both buffers are pixel-aligned, so
					// take 2 pixels per 32-bit move when they share 4-byte alignment.
					const pixel::native_t *sp = src_row + (xa - dx);
					pixel::native_t *dp = dst_row + xa;
					int n = xb - xa + 1;
					if (((reinterpret_cast<std::uintptr_t>(sp) | reinterpret_cast<std::uintptr_t>(dp)) & 3u) == 0) {
						std::uint32_t *d32 = reinterpret_cast<std::uint32_t *>(dp);
						const std::uint32_t *s32 = reinterpret_cast<const std::uint32_t *>(sp);
						const int words = n >> 1;
						for (int i = 0; i < words; i++) d32[i] = s32[i];
						if (n & 1) dp[n - 1] = sp[n - 1];
					} else {
						for (int i = 0; i < n; i++) dp[i] = sp[i];
					}
				} else {
					for (int px = xa; px <= xb; px++) dst_row[px] = src_row[sxMap[px - dx]];
				}
			} else {
				for (int px = xa; px <= xb; px++) {
					const int sx = sxMap[px - dx];
					const std::uint8_t a = alpha_row[sx];
					if (a == 0) continue;
					if (a == 255) dst_row[px] = src_row[sx];
					else dst_row[px] = pixel::blendNative(src_row[sx], dst_row[px], a);
				}
			}
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			}
#endif
		}
		markDirtyClipped(dx, dy, dx + dst_w - 1, dy + dst_h - 1);
		return;
	}

	for (int py = y0; py <= y1; py++) {
		const int localY = py - dy;
		const int sy = std::min((localY * src_h) / dst_h, src_h - 1);
		for (int px = x0; px <= x1; px++) {
			const int localX = px - dx;
			if (!insideRoundedImageRect(localX, localY, dst_w, dst_h, tl, tr, br, bl)) continue;
			const int sx = std::min((localX * src_w) / dst_w, src_w - 1);
			const int srcIndex = sy * src_w + sx;
			if (alpha)
				writePixelNativeAlpha(px, py, src[srcIndex], alpha[srcIndex]);
			else
				writePixelNative(px, py, src[srcIndex]);
		}
	}
	markDirtyClipped(dx, dy, dx + dst_w - 1, dy + dst_h - 1);
}

void Canvas::drawImageTiledX(
	const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	int dx, int dy, int width)
{
	if (!pixels_ || !src) return;
	if (src_w <= 0 || src_h <= 0 || width <= 0) return;

	const ClipRect *clip = &clipStack_[clipDepth_];
	const int clip_x0 = clip->x0 > 0 ? clip->x0 : 0;
	const int clip_y0 = clip->y0 > 0 ? clip->y0 : 0;
	const int clip_x1 = clip->x1 < width_ ? clip->x1 : width_ - 1;
	const int clip_y1 = clip->y1 < height_ ? clip->y1 : height_ - 1;

	int x0 = dx;
	int x1 = dx + width - 1;
	int y0 = dy;
	int y1 = dy + src_h - 1;
	if (x0 < clip_x0) x0 = clip_x0;
	if (x1 > clip_x1) x1 = clip_x1;
	if (y0 < clip_y0) y0 = clip_y0;
	if (y1 > clip_y1) y1 = clip_y1;
	if (x0 > x1 || y0 > y1) return;

	const std::uint8_t global_alpha = globalAlpha_;
	if (global_alpha == 0) return;

	for (int dest_y = y0; dest_y <= y1; dest_y++) {
		const int src_y = dest_y - dy;
		int dest_x = x0;
		while (dest_x <= x1) {
			int src_x = (dest_x - dx) % src_w;
			if (src_x < 0) src_x += src_w;
			const int run_w = std::min(src_w - src_x, x1 - dest_x + 1);
			const pixel::native_t *src_row = &src[src_y * src_w + src_x];
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			if (landscapeRot_) {
				if (!alpha && global_alpha == 255) {
					for (int i = 0; i < run_w; i++) rotSet(dest_x + i, dest_y, src_row[i]);
				} else if (!alpha) {
					for (int i = 0; i < run_w; i++) rotBlend(dest_x + i, dest_y, src_row[i], global_alpha);
				} else {
					const std::uint8_t *alpha_row = &alpha[src_y * src_w + src_x];
					for (int i = 0; i < run_w; i++) {
						const std::uint8_t a = alpha_row[i];
						if (a == 0) continue;
						const int eff = (global_alpha == 255) ? a : (a * global_alpha) / 255;
						if (eff <= 0) continue;
						if (eff >= 255) rotSet(dest_x + i, dest_y, src_row[i]);
						else rotBlend(dest_x + i, dest_y, src_row[i], eff);
					}
				}
			} else {
#endif
#if GEA_PIXEL_STORAGE_PACKED
			std::uint8_t *dnib = packedRow(rowToPhysical(dest_y));
			if (!alpha && global_alpha == 255) {
				pixel::packed::writeUnpacked(dnib, dest_x, src_row, run_w);
			} else if (!alpha) {
				for (int i = 0; i < run_w; i++) pixel::packed::blendPixel(dnib, dest_x + i, src_row[i], global_alpha);
			} else {
				const std::uint8_t *alpha_row = &alpha[src_y * src_w + src_x];
				if (global_alpha == 255) {
					int i = 0;
					while (i < run_w) {
						while (i < run_w && alpha_row[i] == 0) i++;
						const int run_start = i;
						while (i < run_w && alpha_row[i] == 255) i++;
						if (i > run_start) pixel::packed::writeUnpacked(dnib, dest_x + run_start, src_row + run_start, i - run_start);
						if (i < run_w && alpha_row[i] != 0 && alpha_row[i] != 255) {
							pixel::packed::blendPixel(dnib, dest_x + i, src_row[i], alpha_row[i]);
							i++;
						}
					}
				} else {
					for (int i = 0; i < run_w; i++) {
						const std::uint8_t a = alpha_row[i];
						if (a == 0) continue;
						const int eff = (a * global_alpha) / 255;
						if (eff <= 0) continue;
						pixel::packed::blendPixel(dnib, dest_x + i, src_row[i], eff);
					}
				}
			}
#else
			pixel::native_t *dst_row = &pixels_[rowToPhysical(dest_y) * stride_ + dest_x];

			if (!alpha && global_alpha == 255) {
				std::memcpy(dst_row, src_row, static_cast<std::size_t>(run_w) * sizeof(pixel::native_t));
			} else if (!alpha) {
				for (int i = 0; i < run_w; i++) dst_row[i] = pixel::blendNative(src_row[i], dst_row[i], global_alpha);
			} else {
				const std::uint8_t *alpha_row = &alpha[src_y * src_w + src_x];
				if (global_alpha == 255) {
					int i = 0;
					while (i < run_w) {
						while (i < run_w && alpha_row[i] == 0) i++;
						const int run_start = i;
						while (i < run_w && alpha_row[i] == 255) i++;
						if (i > run_start) {
							std::memcpy(dst_row + run_start, src_row + run_start,
							       static_cast<std::size_t>(i - run_start) * sizeof(pixel::native_t));
						}
						if (i < run_w && alpha_row[i] != 0 && alpha_row[i] != 255) {
							dst_row[i] = pixel::blendNative(src_row[i], dst_row[i], alpha_row[i]);
							i++;
						}
					}
				} else {
					for (int i = 0; i < run_w; i++) {
						const std::uint8_t a = alpha_row[i];
						if (a == 0) continue;
						const int eff = (a * global_alpha) / 255;
						if (eff <= 0) continue;
						if (eff >= 255) dst_row[i] = src_row[i];
						else dst_row[i] = pixel::blendNative(src_row[i], dst_row[i], static_cast<std::uint8_t>(eff));
					}
				}
			}
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
			}
#endif

			dest_x += run_w;
		}
	}

	markDirty(x0, y0, x1, y1);
}

}  // namespace gea::framework::graphics

namespace gea::platform::display {

void Display::setAA(int samples)
{
	gea::framework::graphics::Canvas::setAntialiasSamples(samples);
}

int Display::aa()
{
	return gea::framework::graphics::Canvas::antialiasSamples();
}

}  // namespace gea::platform::display
