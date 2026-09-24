// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "state_init.h"
#include "canvas.h"
#include "display.h"
#include "graphics/font.h"
#include "memory.h"
#include "tree_state.h"
#include "style_values.h"
#include <pixel.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "refresh_perf.h"

#ifdef quad
#undef quad
#endif

#ifndef GEA_EMBEDDED_ENABLE_NATIVE_TEXT_INPUT
#define GEA_EMBEDDED_ENABLE_NATIVE_TEXT_INPUT 0
#endif

#ifndef GEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS
#define GEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS 96
#endif

#ifndef GEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS
#define GEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS 65536
#endif
#ifndef GEA_EMBEDDED_RENDER_PARALLEL_DIRTY_REPLAY
#define GEA_EMBEDDED_RENDER_PARALLEL_DIRTY_REPLAY 1
#endif

#ifndef GEA_EMBEDDED_PIE_BLEND_SPAN8_GATHER
#define GEA_EMBEDDED_PIE_BLEND_SPAN8_GATHER 1
#endif

#ifndef GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_EMBEDDED_RENDER_HOT_SRAM 0
#endif

#ifndef GEA_EMBEDDED_DISPLAY_NODE_SCRATCH_IN_SRAM
#define GEA_EMBEDDED_DISPLAY_NODE_SCRATCH_IN_SRAM 0
#endif

#ifndef GEA_EMBEDDED_DISPLAY_REPLAY_CLIP_STACK_IN_SRAM
#define GEA_EMBEDDED_DISPLAY_REPLAY_CLIP_STACK_IN_SRAM 0
#endif

#ifndef GEA_EMBEDDED_DISPLAY_RECORD_CHILDREN_IN_SRAM
#define GEA_EMBEDDED_DISPLAY_RECORD_CHILDREN_IN_SRAM 0
#endif

#ifndef GEA_RP2350_REPROJECT_DEBUG
#define GEA_RP2350_REPROJECT_DEBUG 0
#endif

#ifndef GEA_EMBEDDED_REPROJECT_UNIFIED_DIRTY_RECT
#define GEA_EMBEDDED_REPROJECT_UNIFIED_DIRTY_RECT 0
#endif

#ifndef GEA_EMBEDDED_REPROJECT_MAX_DIRTY_PERCENT
#define GEA_EMBEDDED_REPROJECT_MAX_DIRTY_PERCENT 60
#endif

#ifndef GEA_EMBEDDED_PROJECTED_TEXT_CACHE_BANKS
#define GEA_EMBEDDED_PROJECTED_TEXT_CACHE_BANKS 2
#endif

#ifndef GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES
#define GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES 0
#endif

#if GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_RENDER_HOT_SRAM __attribute__((section(".time_critical.gea_render")))
#else
#define GEA_RENDER_HOT_SRAM
#endif

// Full-screen scratch for the static-background gradient cache (see
// LinearGradientDrawer). A platform with a slow (PSRAM) framebuffer provides a
// strong definition returning a buffer of >= *cap_px pixels in fast-enough RAM;
// the default returns nullptr (no caching — the gradient renders every frame).
extern "C" __attribute__((weak)) gea::framework::graphics::pixel::native_t *gea_bg_cache(int *cap_px)
{
	if (cap_px)
		*cap_px = 0;
	return nullptr;
}

// Full-screen scratch for the static-backdrop cache (see DisplayList::
// maybeBakeStaticBackdrop). Separate from gea_bg_cache so the Phase-1 gradient
// cache is undisturbed. Default returns nullptr (no backdrop cache).
extern "C" __attribute__((weak)) gea::framework::graphics::pixel::native_t *gea_backdrop_cache(int *cap_px)
{
	if (cap_px)
		*cap_px = 0;
	return nullptr;
}

// Fast-RAM scratch for the renderer's hot lookup tables. On a target whose
// default heap is slow (PSRAM), a plain `new` puts a table that is read ONCE PER
// PIXEL behind a cache miss. The transformed-gradient LUT bank used to live in
// .bss (internal SRAM) and was made lazily heap-allocated so apps that never draw
// a transformed gradient reserve nothing -- which silently moved it to PSRAM on
// every ESP32 board, because CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0 sends every
// allocation to the PSRAM heap first. This hook keeps both properties: nothing is
// reserved up front, and when the table IS needed the target hands back internal
// RAM. Returning nullptr (the default) falls back to the normal allocation, so a
// target that has no fast RAM to spare simply keeps the old behaviour.
// The buffer is allocated once and never freed; the caller zero-fills it.
extern "C" __attribute__((weak)) void *gea_render_fast_scratch(int bytes, int align)
{
	(void)bytes;
	(void)align;
	return nullptr;
}

// Optional fast command storage. Tiny targets can return a bounded SRAM buffer
// for the display-list command stream while keeping the normal PSRAM allocation
// as an overflow fallback. command_size/command_align describe DisplayCommand.
extern "C" __attribute__((weak)) void *gea_display_command_buffer(int *cap_commands, int command_size, int command_align)
{
	(void)command_size;
	(void)command_align;
	if (cap_commands)
		*cap_commands = 0;
	return nullptr;
}

// 2nd-core fill offload. The target may provide a worker pinned to the other CPU;
// submit() hands it a row band (returns false if there's no worker), wait() joins.
// Weak defaults = no worker, so the fill runs entirely on the calling core
// (correct on single-core/native targets). See parallelFillRows().
extern "C" __attribute__((weak)) bool gea_render_parallel_submit(void (*)(void *, int, int), void *, int, int)
{
	return false;
}
extern "C" __attribute__((weak)) bool gea_render_parallel_rows_submit(void (*fn)(void *, int, int), void *ctx, int y0, int y1)
{
	return gea_render_parallel_submit(fn, ctx, y0, y1);
}
extern "C" __attribute__((weak)) void gea_render_parallel_wait() {}
// After a coarse parallel band-replay joins, fold the worker core's dirty region
// into the primary canvas so the panel flush transmits both bands. Weak no-op on
// single-core/native targets (the whole replay ran on one canvas there).
extern "C" __attribute__((weak)) void gea_render_parallel_merge_dirty() {}
// Render core id (0/1) for indexing per-core scratch (e.g. the SIMD gather buffers)
// so the two band-replay cores don't share single-threaded static buffers. Weak
// default 0 on single-core/native targets.
extern "C" __attribute__((weak)) int gea_current_render_core() { return 0; }


// SRAM-staged dirty-region replay. On boards whose framebuffer raster is bus-bound
// (e.g. an RGB panel continuously scanning out of the same PSRAM the canvas blends
// against), the target can stage the region replay through an internal-SRAM strip:
// stage_begin binds the calling core's replay canvas to a strip window covering
// rows [y0..y1] (seeded with the current framebuffer content over [x0..x1], so
// alpha blends and partial paints meet the same dst bytes the direct path would),
// the replay rasters into SRAM, and stage_end copies the painted bbox back to the
// framebuffer, restores the binding, and transfers the dirty bbox. rows() is the
// strip row capacity (0 = no staging). Weak defaults = no staging, so the replay
// runs directly on the framebuffer canvas (all other targets).
extern "C" __attribute__((weak)) int gea_replay_stage_rows() { return 0; }
extern "C" __attribute__((weak)) bool gea_replay_stage_begin(int, int, int, int) { return false; }
extern "C" __attribute__((weak)) void gea_replay_stage_end() {}

// Share (permille of the split region's rows) the SUBMITTING core keeps in the
// coarse 2-core replay split. 500 = even split. A target whose render core pays a
// standing tax the worker core doesn't (e.g. an RGB panel's bounce-buffer fill ISR
// pinned to the render core) returns less, so both bands finish together instead
// of the taxed core gating the join.
extern "C" __attribute__((weak)) int gea_render_parallel_main_share_permille() { return 500; }

// SIMD over-blend for the translucent-face fast path. The target may provide a
// vector kernel; blend_span8 blends count8 groups of 8 px in place (dst = panel
// bg in, panel result out), fgN = normal-RGB565 source colors, a5 = per-pixel
// 0..32 alpha. available() gates the (otherwise wasted) gather. Weak defaults =
// no SIMD, so the drawer stays on its scalar path (native/single-core targets).
extern "C" __attribute__((weak)) bool gea_pie_blend_available()
{
	return false;
}
extern "C" __attribute__((weak)) void gea_pie_blend_span8(std::uint16_t *, const std::uint16_t *,
																													const std::int16_t *, int)
{
}
// FUSED gradient-colour + over-blend (8 px/op): computes the gradient colour inline
// via a per-channel integer DDA and over-blends straight into the framebuffer in one
// SIMD pass — no pieFg/pieA5 gather, no scratch round-trip. Valid for a 2-stop,
// constant-alpha linear gradient (the glassy cube faces). a5 = 0..32; *StartQ/*StepQ
// are Q8 (channel<<8) start + per-px delta. Weak default off (ESP32 backend provides it).
extern "C" __attribute__((weak)) bool gea_pie_grad_blend_available() { return false; }
extern "C" __attribute__((weak)) void gea_pie_grad_blend_span8(std::uint16_t *, int, int, int, int, int, int, int, int)
{
}
// OPAQUE gradient SIMD store (8 px/op): vectorizes the pure-scalar opaque overwrite
// path (row[x]=colorNative[bk]). Computes the colour via DDA and stores panel-order
// straight to the framebuffer — no blend, no dst read. *StartQ/*StepQ are Q8.
extern "C" __attribute__((weak)) bool gea_pie_grad_store_available() { return false; }
extern "C" __attribute__((weak)) void gea_pie_grad_store_span8(std::uint16_t *, int, int, int, int, int, int, int)
{
}
// [DIAG A/B] flips 0/1 each PRODFPS window so the fused path is measured ON vs OFF at
// identical board temperature (this long session's thermal drift swamps cross-build deltas).
extern "C" __attribute__((weak)) int gea_diag_ab_phase() { return 1; }

// Optional scalar-MCU fast path for translucent RGB565 gradient faces. This
// fuses LUT lookup + over-blend into one target-owned loop, avoiding the
// ESP32-oriented gather scratch used by gea_pie_blend_span8 on boards that do not
// have an 8-lane vector blend primitive. dst is storage-order RGB565 in/out;
// colorRgb565 is normal-order RGB565; a5 is 0..32 alpha.
extern "C" __attribute__((weak)) bool gea_rgb565_lut_blend_available()
{
	return false;
}
extern "C" __attribute__((weak)) void gea_rgb565_lut_blend_span(std::uint16_t *, const std::uint16_t *,
																																 const std::uint8_t *,
																																 std::int32_t, std::int32_t, int)
{
}
extern "C" __attribute__((weak)) bool gea_rgb565_lut_blend_const_alpha_available()
{
	return false;
}
extern "C" __attribute__((weak)) void gea_rgb565_lut_blend_const_alpha_span(std::uint16_t *, const std::uint16_t *,
																																						std::int16_t,
																																						std::int32_t, std::int32_t, int)
{
}

namespace gea::embedded::ui
{

bool gScrollStripReplayActive = false;
bool gScrollRanThisFrame = false;
bool gScrollImageHoldPending = false;
int gUiFrameCounter = 0;
int gLastScrollUiFrame = -1000;
	namespace
	{

#ifndef GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_SLOTS
#define GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_SLOTS 8
#endif
#ifndef GEA_EMBEDDED_TRANSFORMED_GRADIENT_A5_MIRROR
#define GEA_EMBEDDED_TRANSFORMED_GRADIENT_A5_MIRROR 1
#endif
#ifndef GEA_EMBEDDED_TRANSFORMED_GRADIENT_EDGE_CACHE
#define GEA_EMBEDDED_TRANSFORMED_GRADIENT_EDGE_CACHE 0
#endif
#ifndef GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_LOCK
#define GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_LOCK 0
#endif
#ifndef GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_BANKS
#define GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_BANKS 1
#endif
#ifndef GEA_EMBEDDED_RECOLOR_PIXEL_CACHE_SIZE
#define GEA_EMBEDDED_RECOLOR_PIXEL_CACHE_SIZE 512
#endif

			constexpr int kTransformedGradientLutSlots = GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_SLOTS;
			static_assert(kTransformedGradientLutSlots > 0);
			constexpr int kTransformedGradientLutBanks = GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_BANKS;
			static_assert(kTransformedGradientLutBanks > 0);
			constexpr bool kTransformedGradientA5Mirror = GEA_EMBEDDED_TRANSFORMED_GRADIENT_A5_MIRROR != 0;
		static_assert(kTransformedGradientA5Mirror || GEA_EMBEDDED_PIE_BLEND_SPAN8_GATHER == 0,
		              "GEA_EMBEDDED_PIE_BLEND_SPAN8_GATHER requires the transformed-gradient a5 mirror");
		constexpr int kProjectedTextCacheBanks = GEA_EMBEDDED_PROJECTED_TEXT_CACHE_BANKS;
		static_assert(kProjectedTextCacheBanks > 0);
		constexpr int kScratchDepth = 16;
		constexpr int kFilterBlurPasses = 5;
		constexpr int kAntialiasFullCoverageAreaLimit = 8192;
		constexpr int kRetainedRoundedRectFastInteriorArea = 32768;

#if GEA_RP2350_REPROJECT_DEBUG
		const char *displayCommandTypeName(DisplayCommandType type)
		{
			switch (type)
			{
				case DisplayCommandType::PushClip:
					return "PushClip";
				case DisplayCommandType::PopClip:
					return "PopClip";
				case DisplayCommandType::SetAlpha:
					return "SetAlpha";
				case DisplayCommandType::FillRect:
					return "FillRect";
				case DisplayCommandType::FillCircle:
					return "FillCircle";
				case DisplayCommandType::FillRoundedRect:
					return "FillRoundedRect";
				case DisplayCommandType::FillQuad:
					return "FillQuad";
				case DisplayCommandType::FillLinearGradient:
					return "FillLinearGradient";
				case DisplayCommandType::FillRadialGradient:
					return "FillRadialGradient";
				case DisplayCommandType::DrawLine:
					return "DrawLine";
				case DisplayCommandType::StrokeRect:
					return "StrokeRect";
				case DisplayCommandType::StrokeRoundedRect:
					return "StrokeRoundedRect";
				case DisplayCommandType::DrawProjectedText:
					return "DrawProjectedText";
				case DisplayCommandType::DrawText:
					return "DrawText";
				case DisplayCommandType::BlitImage:
					return "BlitImage";
				case DisplayCommandType::BlitImageScaled:
					return "BlitImageScaled";
				case DisplayCommandType::BeginFilterBlur:
					return "BeginFilterBlur";
				case DisplayCommandType::ApplyFilterBlur:
					return "ApplyFilterBlur";
				case DisplayCommandType::FillTransformedLinearGradient:
					return "FillTransformedLinearGradient";
				case DisplayCommandType::FillTransformedRoundedRect:
					return "FillTransformedRoundedRect";
			}
			return "unknown";
		}

		void logReprojectReject(const char *reason, int a = 0, int b = 0, int c = 0,
														DisplayCommandType type = DisplayCommandType::PushClip)
		{
			static int counter = 0;
			if ((counter++ % 30) != 0)
				return;
			std::printf("[reproject] reject reason=%s a=%d b=%d c=%d type=%s\n",
									reason, a, b, c, displayCommandTypeName(type));
		}
#endif

		void strokeSegmentBandCorners(float x0, float y0, float x1, float y1, float width, int16_t *xs, int16_t *ys)
		{
			const float dx = x1 - x0;
			const float dy = y1 - y0;
			const float len2 = dx * dx + dy * dy;
			if (len2 < 1e-6f)
			{
				for (int i = 0; i < 4; i++)
				{
					xs[i] = static_cast<int16_t>(std::lroundf(x0));
					ys[i] = static_cast<int16_t>(std::lroundf(y0));
				}
				return;
			}
			const float inv = 1.0f / std::sqrt(len2);
			const float half = width * 0.5f;
			const float nx = -dy * half * inv;
			const float ny = dx * half * inv;
			const float ex = dx * 0.75f * inv;
			const float ey = dy * 0.75f * inv;
			xs[0] = static_cast<int16_t>(std::lroundf(x0 - ex + nx));
			ys[0] = static_cast<int16_t>(std::lroundf(y0 - ey + ny));
			xs[1] = static_cast<int16_t>(std::lroundf(x1 + ex + nx));
			ys[1] = static_cast<int16_t>(std::lroundf(y1 + ey + ny));
			xs[2] = static_cast<int16_t>(std::lroundf(x1 + ex - nx));
			ys[2] = static_cast<int16_t>(std::lroundf(y1 + ey - ny));
			xs[3] = static_cast<int16_t>(std::lroundf(x0 - ex - nx));
			ys[3] = static_cast<int16_t>(std::lroundf(y0 - ey - ny));
		}

#if GEA_EMBEDDED_DISPLAY_NODE_SCRATCH_IN_SRAM
		alignas(4) int16_t gDisplayNodeDrawStart[kMaxNodes] __attribute__((section(".uninitialized_data")));
		alignas(4) int16_t gDisplayNodeDrawEnd[kMaxNodes] __attribute__((section(".uninitialized_data")));
		alignas(4) int16_t gDisplayDrawNodeOrder[kMaxNodes] __attribute__((section(".uninitialized_data")));
		alignas(4) int16_t gDisplayDrawNodeBBox[kMaxNodes * 4] __attribute__((section(".uninitialized_data")));
		alignas(4) std::int8_t gDisplaySubtreeDirty[kMaxNodes] __attribute__((section(".uninitialized_data")));
#endif
#if GEA_EMBEDDED_DISPLAY_REPLAY_CLIP_STACK_IN_SRAM
		alignas(4) std::uint8_t gDisplayReplayClipPushed[kMaxNodes] __attribute__((section(".uninitialized_data")));
#endif
#if GEA_EMBEDDED_DISPLAY_RECORD_CHILDREN_IN_SRAM
		alignas(4) int gDisplayRecordChildrenScratch[kScratchDepth * kMaxChildren] __attribute__((section(".uninitialized_data")));
#endif

		inline bool isAsciiWordChar(unsigned char c)
		{
			return std::isalnum(c) != 0;
		}

		int nextUtf8Codepoint(const char *&p)
		{
			const unsigned char c0 = static_cast<unsigned char>(*p++);
			if (c0 < 0x80)
				return c0;

			const auto isContinuation = [](unsigned char c)
			{
				return (c & 0xc0) == 0x80;
			};
			if ((c0 & 0xe0) == 0xc0)
			{
				const unsigned char c1 = static_cast<unsigned char>(*p);
				if (c1 && isContinuation(c1))
				{
					++p;
					return ((c0 & 0x1f) << 6) | (c1 & 0x3f);
				}
				return c0;
			}
			if ((c0 & 0xf0) == 0xe0)
			{
				const unsigned char c1 = static_cast<unsigned char>(p[0]);
				const unsigned char c2 = static_cast<unsigned char>(p[1]);
				if (c1 && c2 && isContinuation(c1) && isContinuation(c2))
				{
					p += 2;
					return ((c0 & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (c2 & 0x3f);
				}
				return c0;
			}
			if ((c0 & 0xf8) == 0xf0)
			{
				const unsigned char c1 = static_cast<unsigned char>(p[0]);
				const unsigned char c2 = static_cast<unsigned char>(p[1]);
				const unsigned char c3 = static_cast<unsigned char>(p[2]);
				if (c1 && c2 && c3 && isContinuation(c1) && isContinuation(c2) && isContinuation(c3))
				{
					p += 3;
					return ((c0 & 0x07) << 18) | ((c1 & 0x3f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
				}
			}
			return c0;
		}

		int transformedTextCodepoint(int cp, int textTransform, bool &wordStart)
		{
			if (cp < 0 || cp > 0x7f)
			{
				if (textTransform == 3)
					wordStart = true;
				return cp;
			}
			const unsigned char uc = static_cast<unsigned char>(cp);
			if (textTransform == 1)
				return std::toupper(uc);
			if (textTransform == 2)
				return std::tolower(uc);
			if (textTransform == 3)
			{
				if (!isAsciiWordChar(uc))
				{
					wordStart = true;
					return cp;
				}
				const int out = wordStart
														? std::toupper(uc)
														: std::tolower(uc);
				wordStart = false;
				return out;
			}
			return cp;
		}

		struct FilterBlurCacheEntry
		{
			// Colour buffers are native pixels — the blur runs in this board's colour
			// space (5/6-bit accumulation on RGB565, full 8-bit on RGBA8888), so a board
			// with an 8888 panel gets a true full-colour blur, not a 565-quantized one.
			gea::framework::graphics::pixel::native_t *pixels = nullptr;
			gea::framework::graphics::pixel::native_t *scratch = nullptr;
			gea::framework::graphics::pixel::native_t *background = nullptr;
			std::uint8_t *alpha = nullptr;
			std::uint8_t *scratchAlpha = nullptr;
			int capacity = 0;
			int x = 0;
			int y = 0;
			int w = 0;
			int h = 0;
			int radiusX = 0;
			int radiusY = 0;
			int sourceAlphaCap = -1;
			std::uint32_t serial = 0;
			bool valid = false;
			bool backgroundValid = false;
		};

		void releaseFilterBlurCache(FilterBlurCacheEntry &cache);

		struct FilterBlurCacheSlot
		{
			int16_t nodeId = -1;
			FilterBlurCacheEntry cache;
		};

		struct DisplayListState
		{
			static constexpr int kMaxCommands = 16384;
			// The command buffer grows on demand from this initial capacity, doubling
			// up to kMaxCommands. Reserving kMaxCommands (sizeof(DisplayCommand)=76B x
			// 16384 = ~1.25 MB) upfront wasted PSRAM on every app regardless of how
			// many commands the UI actually records; most screens use a few thousand.
			static constexpr int kInitialCommands = 2048;
			static constexpr int kNodeCapacityQuantum = 32;
			static constexpr int kFilterBlurCapacityQuantum = 4;
			static constexpr int kReplayClipInitialCapacity = 32;
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
			// Not inlined: with the constant member initializers folded in, the state
			// is a .data object (see state_init.h).
			__attribute__((noinline)) DisplayListState() {}
#endif

			DisplayCommand *commands = nullptr;
			int commandCapacity = 0;
			bool commandsExternal = false;
			int commandCount = 0;
			bool textClippedBackgrounds = false;
			// Sticky: an append() was dropped because the command buffer is full.
			// Reset by clear(); lets a caller detect a truncated record.
			bool commandOverflow = false;
			DisplayCommand *appendOverride = nullptr;
			int *appendOverrideCount = nullptr;
			int appendOverrideCapacity = 0;
			uint8_t *replayClipPushed = nullptr;
			int replayClipCapacity = 0;
			bool replayClipExternal = false;
			int16_t *nodeDrawStart = nullptr;
			int16_t *nodeDrawEnd = nullptr;
			int16_t *drawNodeOrder = nullptr;
			// Packed [x0,y0,x1,y1] per drawNodeOrder slot, baked at record time. The
			// per-chunk replay walk (replaySimpleClippedDirtyRegionFast) is O(nodes ×
			// chunks); reading node.layout from the scattered Node structs in PSRAM on
			// every chunk dominated the walk. This contiguous mirror lets each chunk
			// AABB-skip non-overlapping nodes without touching a Node at all, and only
			// the few nodes that overlap the chunk band pay the Node-struct read.
			int16_t *drawNodeBBox = nullptr;
			std::int8_t *subtreeDirty = nullptr;
			int nodeScratchCapacity = 0;
			bool nodeScratchExternal = false;
			int drawNodeOrderCount = 0;
			// Backdrop-cache record skip: when the static scene is already baked, recordNode
			// skips re-recording fully-static subtrees (they're never replayed — they're
			// blitted from the backdrop). subtreeDirty[id] = "id or a descendant is dirty".
			bool recordSkipStatic = false;
			int recordDepth = 0;
			int *recordChildrenScratch = nullptr;
			bool recordChildrenExternal = false;
			std::uint32_t displayListSerial = 1;
			// Transform-reproject fast path: when a transform-only frame leaves the display
			// list's command STRUCTURE unchanged (same nodes, same commands — true for a
			// spinning cube: no culling, no add/remove), re-project each command's corners
			// from its stored local rect into the existing command instead of clear()+
			// recordNode(). reprojectSerial = the displayListSerial of the last full record
			// that armed this path; a clear() bumps displayListSerial, disarming it.
			std::uint32_t reprojectSerial = 0;
			int recordExpandedClipNode = -1;
			int recordExpandedClipX0 = 0;
			int recordExpandedClipY0 = 0;
			int recordExpandedClipX1 = -1;
			int recordExpandedClipY1 = -1;
			// Union of (prev ∪ cur) screen bbox over the transformed commands a reproject
			// patched this frame — i.e. the spinning subtree's dirty rect, free because the
			// reproject already touched every corner. The dirty pass reuses it instead of
			// re-projecting via transformedSubtreeBoundsRect. Valid only on a reprojected frame.
			int reprojectDirtyX0 = 0, reprojectDirtyY0 = 0, reprojectDirtyX1 = -1, reprojectDirtyY1 = -1;
			struct RetainedBackgroundRecolor
			{
				int16_t node = -1;
				DisplayCommand command{};
			};
			static constexpr int kMaxRetainedBackgroundRecolors = 4;
			RetainedBackgroundRecolor retainedBackgroundRecolors[kMaxRetainedBackgroundRecolors]{};
			int retainedBackgroundRecolorCount = 0;
			FilterBlurCacheSlot *filterBlurCacheSlots = nullptr;
			int filterBlurCacheCount = 0;
			int filterBlurCacheCapacity = 0;
			int filterBlurCacheHits = 0;
			int filterBlurCacheMisses = 0;

			void *scratchAlloc(size_t size)
			{
				return gea::framework::memory::Allocator::allocatePreferSpiram(size);
			}

			void *scratchRealloc(void *ptr, size_t size)
			{
				return gea::framework::memory::Allocator::reallocatePreferSpiram(ptr, size);
			}

			static int roundedCapacity(int needed, int quantum)
			{
				if (needed <= 0)
					return 0;
				return ((needed + quantum - 1) / quantum) * quantum;
			}

			bool ensureReplayClipCapacity(int needed)
			{
				if (needed <= replayClipCapacity)
					return true;
				if (needed > kMaxCommands)
					return false;
#if GEA_EMBEDDED_DISPLAY_REPLAY_CLIP_STACK_IN_SRAM
				if (replayClipCapacity == 0 && needed <= kMaxNodes)
				{
					replayClipPushed = gDisplayReplayClipPushed;
					for (int i = 0; i < kMaxNodes; i++)
						replayClipPushed[i] = 0;
					replayClipCapacity = kMaxNodes;
					replayClipExternal = true;
					return true;
				}
#endif
				int nextCapacity = replayClipCapacity > 0 ? replayClipCapacity : kReplayClipInitialCapacity;
				while (nextCapacity < needed && nextCapacity < kMaxCommands)
					nextCapacity *= 2;
				if (nextCapacity > kMaxCommands)
					nextCapacity = kMaxCommands;
				std::uint8_t *next = nullptr;
				if (replayClipExternal)
				{
					next = static_cast<std::uint8_t *>(scratchAlloc(static_cast<size_t>(nextCapacity)));
					if (next && replayClipPushed)
						std::memcpy(next, replayClipPushed, static_cast<std::size_t>(replayClipCapacity));
				}
				else
				{
					next = static_cast<uint8_t *>(scratchRealloc(replayClipPushed, static_cast<size_t>(nextCapacity)));
				}
				if (!next)
					return false;
				for (int i = replayClipCapacity; i < nextCapacity; i++)
					next[i] = 0;
				replayClipPushed = next;
				replayClipCapacity = nextCapacity;
				replayClipExternal = false;
				return true;
			}

			bool ensureNodeScratchCapacity(int needed)
			{
				if (needed <= nodeScratchCapacity)
					return true;
				if (needed < 0 || needed > kMaxNodes)
					return false;
#if GEA_EMBEDDED_DISPLAY_NODE_SCRATCH_IN_SRAM
				if (nodeScratchCapacity == 0)
				{
					nodeDrawStart = gDisplayNodeDrawStart;
					nodeDrawEnd = gDisplayNodeDrawEnd;
					drawNodeOrder = gDisplayDrawNodeOrder;
					drawNodeBBox = gDisplayDrawNodeBBox;
					subtreeDirty = gDisplaySubtreeDirty;
					for (int i = 0; i < kMaxNodes; i++)
					{
						nodeDrawStart[i] = -1;
						nodeDrawEnd[i] = -1;
						drawNodeOrder[i] = -1;
						subtreeDirty[i] = 0;
					}
					nodeScratchCapacity = kMaxNodes;
					nodeScratchExternal = true;
					return true;
				}
#endif
				const int nextCapacity = roundedCapacity(needed, kNodeCapacityQuantum);
				auto *nextStart = static_cast<int16_t *>(scratchRealloc(nodeDrawStart, sizeof(int16_t) * static_cast<size_t>(nextCapacity)));
				if (!nextStart)
					return false;
				nodeDrawStart = nextStart;
				auto *nextEnd = static_cast<int16_t *>(scratchRealloc(nodeDrawEnd, sizeof(int16_t) * static_cast<size_t>(nextCapacity)));
				if (!nextEnd)
					return false;
				nodeDrawEnd = nextEnd;
				auto *nextOrder = static_cast<int16_t *>(scratchRealloc(drawNodeOrder, sizeof(int16_t) * static_cast<size_t>(nextCapacity)));
				if (!nextOrder)
					return false;
				drawNodeOrder = nextOrder;
				auto *nextBBox = static_cast<int16_t *>(scratchRealloc(drawNodeBBox, sizeof(int16_t) * 4 * static_cast<size_t>(nextCapacity)));
				if (!nextBBox)
					return false;
				drawNodeBBox = nextBBox;
				auto *nextDirty = static_cast<std::int8_t *>(scratchRealloc(subtreeDirty, sizeof(std::int8_t) * static_cast<size_t>(nextCapacity)));
				if (!nextDirty)
					return false;
				subtreeDirty = nextDirty;
				for (int i = nodeScratchCapacity; i < nextCapacity; i++)
				{
					nodeDrawStart[i] = -1;
					nodeDrawEnd[i] = -1;
					drawNodeOrder[i] = -1;
					subtreeDirty[i] = 0;
				}
				nodeScratchCapacity = nextCapacity;
				return true;
			}

			bool hasNodeScratchFor(int node) const
			{
				return node >= 0 && node < nodeScratchCapacity && nodeDrawStart && nodeDrawEnd && drawNodeOrder && subtreeDirty;
			}

			FilterBlurCacheEntry *filterBlurCacheForNode(int node)
			{
				if (node < 0 || node >= kMaxNodes)
					return nullptr;
				for (int i = 0; i < filterBlurCacheCount; i++)
				{
					if (filterBlurCacheSlots[i].nodeId == node)
						return &filterBlurCacheSlots[i].cache;
				}
				if (filterBlurCacheCount >= filterBlurCacheCapacity)
				{
					const int nextCapacity = filterBlurCacheCapacity > 0
																			 ? filterBlurCacheCapacity + kFilterBlurCapacityQuantum
																			 : kFilterBlurCapacityQuantum;
					auto *next = static_cast<FilterBlurCacheSlot *>(
							scratchRealloc(filterBlurCacheSlots, sizeof(FilterBlurCacheSlot) * static_cast<size_t>(nextCapacity)));
					if (!next)
						return nullptr;
					filterBlurCacheSlots = next;
					for (int i = filterBlurCacheCapacity; i < nextCapacity; i++)
						filterBlurCacheSlots[i] = FilterBlurCacheSlot{};
					filterBlurCacheCapacity = nextCapacity;
				}
				filterBlurCacheSlots[filterBlurCacheCount] = FilterBlurCacheSlot{};
				filterBlurCacheSlots[filterBlurCacheCount].nodeId = static_cast<int16_t>(node);
				return &filterBlurCacheSlots[filterBlurCacheCount++].cache;
			}

			void clearNodeRange(int node)
			{
				if (!hasNodeScratchFor(node))
					return;
				nodeDrawStart[node] = -1;
				nodeDrawEnd[node] = -1;
			}

			void resetStorage()
			{
				for (int i = 0; i < filterBlurCacheCount; i++)
					releaseFilterBlurCache(filterBlurCacheSlots[i].cache);
				gea::framework::memory::Allocator::free(filterBlurCacheSlots);
				filterBlurCacheSlots = nullptr;
				filterBlurCacheCount = 0;
				filterBlurCacheCapacity = 0;
				if (!nodeScratchExternal)
				{
					gea::framework::memory::Allocator::free(nodeDrawStart);
					gea::framework::memory::Allocator::free(nodeDrawEnd);
					gea::framework::memory::Allocator::free(drawNodeOrder);
					gea::framework::memory::Allocator::free(drawNodeBBox);
					gea::framework::memory::Allocator::free(subtreeDirty);
				}
				nodeDrawStart = nullptr;
				nodeDrawEnd = nullptr;
				drawNodeOrder = nullptr;
				drawNodeBBox = nullptr;
				subtreeDirty = nullptr;
				nodeScratchCapacity = 0;
				nodeScratchExternal = false;
				if (!replayClipExternal)
					gea::framework::memory::Allocator::free(replayClipPushed);
				replayClipPushed = nullptr;
				replayClipCapacity = 0;
				replayClipExternal = false;
				if (!recordChildrenExternal)
					gea::framework::memory::Allocator::free(recordChildrenScratch);
				recordChildrenScratch = nullptr;
				recordChildrenExternal = false;
				if (!commandsExternal)
					gea::framework::memory::Allocator::free(commands);
				commands = nullptr;
				commandCapacity = 0;
				commandsExternal = false;
				commandCount = 0;
				textClippedBackgrounds = false;
				appendOverride = nullptr;
				appendOverrideCount = nullptr;
				appendOverrideCapacity = 0;
				drawNodeOrderCount = 0;
				recordSkipStatic = false;
				recordDepth = 0;
				displayListSerial = 1;
				reprojectSerial = 0;
				reprojectDirtyX0 = 0;
				reprojectDirtyY0 = 0;
				reprojectDirtyX1 = -1;
				reprojectDirtyY1 = -1;
				retainedBackgroundRecolorCount = 0;
				filterBlurCacheHits = 0;
				filterBlurCacheMisses = 0;
			}

			int *childrenForDepth(int depth)
			{
				if (depth < 0 || depth >= kScratchDepth)
					return nullptr;
				if (!recordChildrenScratch)
				{
#if GEA_EMBEDDED_DISPLAY_RECORD_CHILDREN_IN_SRAM
					recordChildrenScratch = gDisplayRecordChildrenScratch;
					recordChildrenExternal = true;
#else
					recordChildrenScratch = static_cast<int *>(scratchAlloc(sizeof(int) * kScratchDepth * kMaxChildren));
#endif
				}
				if (!recordChildrenScratch)
					return nullptr;
				return recordChildrenScratch + depth * kMaxChildren;
			}

			void shiftNodeDrawRangesAfter(int old_end, int delta, int skip_node)
			{
				if (delta == 0)
					return;
				for (int i = 0; i < Tree::instance().nodeCount(); i++)
				{
					if (!hasNodeScratchFor(i))
						continue;
					if (i == skip_node)
						continue;
					if (nodeDrawStart[i] >= old_end)
						nodeDrawStart[i] += delta;
					if (nodeDrawEnd[i] >= old_end)
						nodeDrawEnd[i] += delta;
				}
			}
		};

		DisplayListState state;

		int clampInt(int value, int minValue, int maxValue)
		{
			if (value < minValue)
				return minValue;
			if (value > maxValue)
				return maxValue;
			return value;
		}

		uint8_t multiplyAlpha(uint8_t a, uint8_t b)
		{
			return static_cast<uint8_t>((static_cast<int>(a) * static_cast<int>(b) + 127) / 255);
		}

		uint8_t rightFadeMaskAlphaForChild(const Node &parent, const Node &child)
		{
			const int fade = parent.style.mask_right_fade_width;
			if (fade <= 0 || parent.layout.width <= 0)
				return 255;
			if (child.layout.width <= 0 || child.layout.height <= 0)
				return 0;

			const int parentLeft = parent.layout.x;
			const int parentRight = parent.layout.x + parent.layout.width;
			const int fadeWidth = clampInt(fade, 1, parent.layout.width);
			const int fadeStart = parentRight - fadeWidth;
			const int visibleLeft = std::max(parentLeft, static_cast<int>(child.layout.x));
			const int visibleRight = std::min(parentRight, static_cast<int>(child.layout.x + child.layout.width));
			if (visibleLeft >= visibleRight)
				return 0;
			if (visibleRight <= fadeStart)
				return 255;

			int sum = 0;
			const int count = visibleRight - visibleLeft;
			for (int x = visibleLeft; x < visibleRight; ++x)
			{
				const int centerTimes2 = x * 2 + 1;
				if (centerTimes2 <= fadeStart * 2)
				{
					sum += 255;
					continue;
				}
				const int distanceTimes2 = std::max(0, parentRight * 2 - centerTimes2);
				sum += clampInt((distanceTimes2 * 255 + fadeWidth) / (2 * fadeWidth), 0, 255);
			}
			return static_cast<uint8_t>((sum + count / 2) / count);
		}

		void appendAlphaCommand(DisplayList &list, const Node &node, uint8_t alpha, uint8_t recordParentAlpha)
		{
			DisplayCommand *cmd = list.append();
			if (!cmd)
				return;
			cmd->type = DisplayCommandType::SetAlpha;
			cmd->bx = node.layout.x;
			cmd->by = node.layout.y;
			cmd->bw = node.layout.width;
			cmd->bh = node.layout.height;
			cmd->alpha.alpha = alpha;
			cmd->alpha.recordParentAlpha = recordParentAlpha;
			cmd->alpha.nodeId = -1;
		}

		bool solveLinear8(double matrix[8][9], double out[8]);
		bool simpleFilterBlurBackgroundNode(const Node &node);
		int filterBlurCacheCapacityNeeded(const DisplayCommand &command, int pixelCount);

		void releaseFilterBlurCache(FilterBlurCacheEntry &cache)
		{
			if (cache.pixels)
				gea::framework::memory::Allocator::free(cache.pixels);
			if (cache.scratch)
				gea::framework::memory::Allocator::free(cache.scratch);
			if (cache.background)
				gea::framework::memory::Allocator::free(cache.background);
			if (cache.alpha)
				gea::framework::memory::Allocator::free(cache.alpha);
			if (cache.scratchAlpha)
				gea::framework::memory::Allocator::free(cache.scratchAlpha);
			cache.pixels = nullptr;
			cache.scratch = nullptr;
			cache.background = nullptr;
			cache.alpha = nullptr;
			cache.scratchAlpha = nullptr;
			cache.capacity = 0;
			cache.valid = false;
			cache.backgroundValid = false;
		}

		bool ensureFilterBlurCache(FilterBlurCacheEntry &cache, int pixelCount)
		{
			if (pixelCount <= 0)
				return false;
			if (cache.pixels && cache.scratch && cache.background && cache.alpha && cache.scratchAlpha &&
					cache.capacity >= pixelCount)
			{
				return true;
			}
			releaseFilterBlurCache(cache);
			using native_t = gea::framework::graphics::pixel::native_t;
			const std::size_t byteCount = static_cast<std::size_t>(pixelCount) * sizeof(native_t);
			cache.pixels = static_cast<native_t *>(
					gea::framework::memory::Allocator::allocatePreferSpiram(byteCount, alignof(native_t)));
			cache.scratch = static_cast<native_t *>(
					gea::framework::memory::Allocator::allocatePreferSpiram(byteCount, alignof(native_t)));
			cache.background = static_cast<native_t *>(
					gea::framework::memory::Allocator::allocatePreferSpiram(byteCount, alignof(native_t)));
			cache.alpha = static_cast<std::uint8_t *>(
					gea::framework::memory::Allocator::allocatePreferSpiram(static_cast<std::size_t>(pixelCount), alignof(std::uint8_t)));
			cache.scratchAlpha = static_cast<std::uint8_t *>(
					gea::framework::memory::Allocator::allocatePreferSpiram(static_cast<std::size_t>(pixelCount), alignof(std::uint8_t)));
			if (!cache.pixels || !cache.scratch || !cache.background || !cache.alpha || !cache.scratchAlpha)
			{
				releaseFilterBlurCache(cache);
				return false;
			}
			cache.capacity = pixelCount;
			return true;
		}

		// Blur accumulates in the native colour space's channel units: 5/6-bit on
		// RGB565 (byte-identical to the original), full 8-bit on 8888 targets.
		inline void unpackLayerChannels(gea::framework::graphics::pixel::native_t color, int *cr, int *cg, int *cb)
		{
#if GEA_PIXEL_FORMAT_IS_8888
			int ca = 0;
			gea::framework::graphics::pixel::unpackNative8(color, cr, cg, cb, &ca);
#else
			gea::framework::graphics::pixel::unpackRgb565(color, cr, cg, cb);
#endif
		}

		void addLayerSample(gea::framework::graphics::pixel::native_t color, std::uint8_t alpha, int &a, int &r, int &g, int &b)
		{
			int cr = 0, cg = 0, cb = 0;
			unpackLayerChannels(color, &cr, &cg, &cb);
			a += alpha;
			r += cr * alpha;
			g += cg * alpha;
			b += cb * alpha;
		}

		void subtractLayerSample(gea::framework::graphics::pixel::native_t color, std::uint8_t alpha, int &a, int &r, int &g, int &b)
		{
			int cr = 0, cg = 0, cb = 0;
			unpackLayerChannels(color, &cr, &cg, &cb);
			a -= alpha;
			r -= cr * alpha;
			g -= cg * alpha;
			b -= cb * alpha;
		}

		void writeLayerAverage(gea::framework::graphics::pixel::native_t *dstColor, std::uint8_t *dstAlpha, int divisor, int a, int r, int g, int b)
		{
			if (divisor <= 0)
				divisor = 1;
			const int outAlpha = (a + divisor / 2) / divisor;
			if (outAlpha <= 0 || a <= 0)
			{
				*dstAlpha = 0;
				*dstColor = 0;
				return;
			}
			*dstAlpha = static_cast<std::uint8_t>(outAlpha > 255 ? 255 : outAlpha);
#if GEA_PIXEL_FORMAT_IS_8888
			*dstColor = gea::framework::graphics::pixel::packNative8((r + a / 2) / a, (g + a / 2) / a, (b + a / 2) / a);
#else
			*dstColor = gea::framework::graphics::pixel::packRgb565Components((r + a / 2) / a,
																																				(g + a / 2) / a,
																																				(b + a / 2) / a);
#endif
		}

		void blurLayerHorizontal(const gea::framework::graphics::pixel::native_t *srcColor,
														 const std::uint8_t *srcAlpha,
														 gea::framework::graphics::pixel::native_t *dstColor,
														 std::uint8_t *dstAlpha,
														 int width,
														 int height,
														 int radius)
		{
			const int divisor = radius * 2 + 1;
			for (int y = 0; y < height; y++)
			{
				const gea::framework::graphics::pixel::native_t *colorRow = srcColor + y * width;
				const std::uint8_t *alphaRow = srcAlpha + y * width;
				gea::framework::graphics::pixel::native_t *outColor = dstColor + y * width;
				std::uint8_t *outAlpha = dstAlpha + y * width;
				int a = 0, r = 0, g = 0, b = 0;
				for (int k = -radius; k <= radius; k++)
				{
					if (k >= 0 && k < width)
						addLayerSample(colorRow[k], alphaRow[k], a, r, g, b);
				}
				for (int x = 0; x < width; x++)
				{
					writeLayerAverage(&outColor[x], &outAlpha[x], divisor, a, r, g, b);
					if (x + 1 >= width)
						continue;
					int ix = x - radius;
					if (ix >= 0 && ix < width)
						subtractLayerSample(colorRow[ix], alphaRow[ix], a, r, g, b);
					ix = x + radius + 1;
					if (ix >= 0 && ix < width)
						addLayerSample(colorRow[ix], alphaRow[ix], a, r, g, b);
				}
			}
		}

		void blurLayerVertical(const gea::framework::graphics::pixel::native_t *srcColor,
													 const std::uint8_t *srcAlpha,
													 gea::framework::graphics::pixel::native_t *dstColor,
													 std::uint8_t *dstAlpha,
													 int width,
													 int height,
													 int radius)
		{
			const int divisor = radius * 2 + 1;
			for (int x = 0; x < width; x++)
			{
				int a = 0, r = 0, g = 0, b = 0;
				for (int k = -radius; k <= radius; k++)
				{
					if (k >= 0 && k < height)
					{
						const int index = k * width + x;
						addLayerSample(srcColor[index], srcAlpha[index], a, r, g, b);
					}
				}
				for (int y = 0; y < height; y++)
				{
					const int outIndex = y * width + x;
					writeLayerAverage(&dstColor[outIndex], &dstAlpha[outIndex], divisor, a, r, g, b);
					if (y + 1 >= height)
						continue;
					int iy = y - radius;
					if (iy >= 0 && iy < height)
					{
						int index = iy * width + x;
						subtractLayerSample(srcColor[index], srcAlpha[index], a, r, g, b);
					}
					iy = y + radius + 1;
					if (iy >= 0 && iy < height)
					{
						int index = iy * width + x;
						addLayerSample(srcColor[index], srcAlpha[index], a, r, g, b);
					}
				}
			}
		}

		void blurAlpha16Horizontal(const gea::framework::graphics::pixel::native_t *srcAlpha,
															 gea::framework::graphics::pixel::native_t *dstAlpha,
															 int width,
															 int height,
															 int radius)
		{
			const int divisor = radius * 2 + 1;
			for (int y = 0; y < height; y++)
			{
				const gea::framework::graphics::pixel::native_t *alphaRow = srcAlpha + y * width;
				gea::framework::graphics::pixel::native_t *outAlpha = dstAlpha + y * width;
				std::uint32_t a = 0;
				for (int k = -radius; k <= radius; k++)
				{
					if (k >= 0 && k < width)
						a += alphaRow[k];
				}
				for (int x = 0; x < width; x++)
				{
					outAlpha[x] = static_cast<gea::framework::graphics::pixel::native_t>((a + static_cast<std::uint32_t>(divisor / 2)) / static_cast<std::uint32_t>(divisor));
					if (x + 1 >= width)
						continue;
					int ix = x - radius;
					if (ix >= 0 && ix < width)
						a -= alphaRow[ix];
					ix = x + radius + 1;
					if (ix >= 0 && ix < width)
						a += alphaRow[ix];
				}
			}
		}

		void blurAlpha16Vertical(const gea::framework::graphics::pixel::native_t *srcAlpha,
														 gea::framework::graphics::pixel::native_t *dstAlpha,
														 int width,
														 int height,
														 int radius)
		{
			const int divisor = radius * 2 + 1;
			for (int x = 0; x < width; x++)
			{
				std::uint32_t a = 0;
				for (int k = -radius; k <= radius; k++)
				{
					if (k >= 0 && k < height)
						a += srcAlpha[k * width + x];
				}
				for (int y = 0; y < height; y++)
				{
					const int outIndex = y * width + x;
					dstAlpha[outIndex] = static_cast<gea::framework::graphics::pixel::native_t>((a + static_cast<std::uint32_t>(divisor / 2)) / static_cast<std::uint32_t>(divisor));
					if (y + 1 >= height)
						continue;
					int iy = y - radius;
					if (iy >= 0 && iy < height)
						a -= srcAlpha[iy * width + x];
					iy = y + radius + 1;
					if (iy >= 0 && iy < height)
						a += srcAlpha[iy * width + x];
				}
			}
		}

		void filterBlurBounds(const DisplayCommand &command, int canvasWidth, int canvasHeight,
													int *x0, int *y0, int *x1, int *y1)
		{
			*x0 = clampInt(command.bx, 0, canvasWidth - 1);
			*y0 = clampInt(command.by, 0, canvasHeight - 1);
			*x1 = clampInt(command.bx + command.bw - 1, 0, canvasWidth - 1);
			*y1 = clampInt(command.by + command.bh - 1, 0, canvasHeight - 1);
		}

		bool filterBlurCacheGeometryMatches(const DisplayCommand &command, const FilterBlurCacheEntry &cache)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || canvas->width() <= 0 || canvas->height() <= 0)
				return false;
			int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
			filterBlurBounds(command, canvas->width(), canvas->height(), &x0, &y0, &x1, &y1);
			if (x0 > x1 || y0 > y1)
				return false;
			return cache.x == x0 &&
						 cache.y == y0 &&
						 cache.w == x1 - x0 + 1 &&
						 cache.h == y1 - y0 + 1 &&
						 cache.radiusX == command.filterBlur.radiusX &&
						 cache.radiusY == command.filterBlur.radiusY &&
						 cache.sourceAlphaCap == command.filterBlur.sourceAlphaCap;
		}

		int filterBlurCacheCapacityNeeded(const DisplayCommand &command, int pixelCount)
		{
			const int nodeId = command.filterBlur.nodeId;
			auto &tree = Tree::instance();
			if (nodeId < 0 || nodeId >= tree.nodeCount())
				return pixelCount;
			const Node &node = tree.node(nodeId);
			if (!simpleFilterBlurBackgroundNode(node))
				return pixelCount;
			const int spread = std::max(0, static_cast<int>(command.filterBlur.radius)) * kFilterBlurPasses;
			const int localW = node.layout.width + spread * 2;
			const int localH = node.layout.height + spread * 2;
			if (localW <= 0 || localH <= 0)
				return pixelCount;
			const long long localCount = static_cast<long long>(localW) * static_cast<long long>(localH);
			if (localCount <= 0 || localCount > 2147483647LL)
				return pixelCount;
			return std::max(pixelCount, static_cast<int>(localCount));
		}

		bool captureFilterBlurBackground(const DisplayCommand &command, FilterBlurCacheEntry &cache)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return false;

			int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
			filterBlurBounds(command, canvas->width(), canvas->height(), &x0, &y0, &x1, &y1);
			if (x0 > x1 || y0 > y1)
				return false;
			const int w = x1 - x0 + 1;
			const int h = y1 - y0 + 1;
			const int pixelCount = w * h;
			const int neededPixelCount = filterBlurCacheCapacityNeeded(command, pixelCount);
			const bool keepFiltered = cache.valid &&
																cache.serial == state.displayListSerial &&
																filterBlurCacheGeometryMatches(command, cache);
			if (!ensureFilterBlurCache(cache, neededPixelCount))
				return false;
			if (!keepFiltered)
				cache.valid = false;

			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			for (int row = 0; row < h; row++)
			{
				const gea::framework::graphics::pixel::native_t *src = canvas->pixels() + canvas->rowToPhysical(y0 + row) * stride + x0;
				std::memcpy(cache.background + row * w, src, static_cast<std::size_t>(w) * sizeof(gea::framework::graphics::pixel::native_t));
			}

			cache.x = x0;
			cache.y = y0;
			cache.w = w;
			cache.h = h;
			cache.radiusX = command.filterBlur.radiusX;
			cache.radiusY = command.filterBlur.radiusY;
			cache.sourceAlphaCap = command.filterBlur.sourceAlphaCap;
			cache.backgroundValid = true;
			return true;
		}

		int channelAlphaRequirement(int before, int after)
		{
			const int diff = after - before;
			if (diff == 0)
				return 0;
			const int denominator = diff > 0 ? (255 - before) : before;
			if (denominator <= 0)
				return 255;
			const int alpha = (std::abs(diff) * 255 + denominator / 2) / denominator;
			return alpha > 255 ? 255 : alpha;
		}

		int clampByte(int value)
		{
			if (value < 0)
				return 0;
			if (value > 255)
				return 255;
			return value;
		}

		void unpackRgb565To888(gea::framework::graphics::pixel::native_t pixel, int *r, int *g, int *b)
		{
			// Native pixel -> 8-bit channels. On RGB565 this expands 5/6-bit exactly as
			// before ((v*255+x)/d via the LUT); on RGBA8888 it reads the 8-bit bytes.
			int a = 0;
			gea::framework::graphics::pixel::unpackNative8(pixel, r, g, b, &a);
		}

		int orderedDither8(int x, int y)
		{
			static constexpr std::uint8_t kBayer8[8][8] = {
					{0, 48, 12, 60, 3, 51, 15, 63},
					{32, 16, 44, 28, 35, 19, 47, 31},
					{8, 56, 4, 52, 11, 59, 7, 55},
					{40, 24, 36, 20, 43, 27, 39, 23},
					{2, 50, 14, 62, 1, 49, 13, 61},
					{34, 18, 46, 30, 33, 17, 45, 29},
					{10, 58, 6, 54, 9, 57, 5, 53},
					{42, 26, 38, 22, 41, 25, 37, 21},
			};
			return kBayer8[y & 7][x & 7];
		}

		int quantizeDithered(int value, int maxValue, int threshold)
		{
			value = clampByte(value);
			const int scaled = value * maxValue;
			const int base = scaled / 255;
			if (base >= maxValue)
				return maxValue;
			const int remainder = scaled - base * 255;
			return base + (remainder * 64 > threshold * 255 ? 1 : 0);
		}

		gea::framework::graphics::pixel::native_t packRgb888Dithered(int r, int g, int b, int x, int y)
		{
			r = clampByte(r);
			g = clampByte(g);
			b = clampByte(b);
#if GEA_PIXEL_FORMAT_IS_8888
			// Full colour: no ordered dither (8888 doesn't band), pack the 8-bit channels.
			(void)x;
			(void)y;
			return gea::framework::graphics::pixel::packNative8(r, g, b);
#else
			const int threshold = orderedDither8(x, y);
			return gea::framework::graphics::pixel::packRgb565Components(
					quantizeDithered(r, 31, threshold),
					quantizeDithered(g, 63, threshold),
					quantizeDithered(b, 31, threshold));
#endif
		}

		gea::framework::graphics::pixel::native_t blendDithered(gea::framework::graphics::pixel::native_t fg, gea::framework::graphics::pixel::native_t bg, int alpha, int x, int y)
		{
			if (alpha <= 0)
				return bg;
			if (alpha >= 255)
				return fg;
			int fr = 0, fg8 = 0, fb = 0;
			int br = 0, bg8 = 0, bb = 0;
			unpackRgb565To888(fg, &fr, &fg8, &fb);
			unpackRgb565To888(bg, &br, &bg8, &bb);
			const int inverse = 255 - alpha;
			return packRgb888Dithered((fr * alpha + br * inverse + 127) / 255,
																(fg8 * alpha + bg8 * inverse + 127) / 255,
																(fb * alpha + bb * inverse + 127) / 255,
																x,
																y);
		}

		gea::framework::graphics::pixel::native_t deriveLayerColor(gea::framework::graphics::pixel::native_t before, gea::framework::graphics::pixel::native_t after, int alpha)
		{
			if (alpha <= 0)
				return after;
			int br = 0, bg = 0, bb = 0;
			int ar = 0, ag = 0, ab = 0;
			unpackRgb565To888(before, &br, &bg, &bb);
			unpackRgb565To888(after, &ar, &ag, &ab);
			const int inverse = 255 - alpha;
			const int sr = clampByte((ar * 255 - br * inverse + alpha / 2) / alpha);
			const int sg = clampByte((ag * 255 - bg * inverse + alpha / 2) / alpha);
			const int sb = clampByte((ab * 255 - bb * inverse + alpha / 2) / alpha);
			return gea::framework::graphics::pixel::nativeColor(sr, sg, sb);
		}

		bool simpleFilterBlurBackgroundNode(const Node &node)
		{
			return node.first_child < 0 &&
						 node.type == NodeType::View &&
						 node.style.has_bg &&
						 !styleHasBackgroundImage(node.style) && rstyle(node.style).bg_clip == 0 &&
						 !hasAnyBorder(node.style);
		}

		bool isFullyRoundedFilterShape(const Node &node)
		{
			const int halfMin = std::min(node.layout.width, node.layout.height) / 2;
			const int minRadius = std::max(1, halfMin - 1);
			return halfMin > 0 &&
						 node.style.border_radius[0] >= minRadius &&
						 node.style.border_radius[1] >= minRadius &&
						 node.style.border_radius[2] >= minRadius &&
						 node.style.border_radius[3] >= minRadius;
		}

		bool screenToNodeRectHomography(const Node &node, double out[8])
		{
			const int x = node.layout.x;
			const int y = node.layout.y;
			const int w = node.layout.width;
			const int h = node.layout.height;
			if (w <= 0 || h <= 0)
				return false;

			int16_t xs[4] = {};
			int16_t ys[4] = {};
			ViewRenderer::transformedRectCorners(node, false, x, y, w, h, xs, ys);
			const double dx[4] = {static_cast<double>(xs[0]), static_cast<double>(xs[1]), static_cast<double>(xs[2]), static_cast<double>(xs[3])};
			const double dy[4] = {static_cast<double>(ys[0]), static_cast<double>(ys[1]), static_cast<double>(ys[2]), static_cast<double>(ys[3])};
			const double sx[4] = {static_cast<double>(x), static_cast<double>(x + w), static_cast<double>(x + w), static_cast<double>(x)};
			const double sy[4] = {static_cast<double>(y), static_cast<double>(y), static_cast<double>(y + h), static_cast<double>(y + h)};

			double matrix[8][9]{};
			for (int i = 0; i < 4; i++)
			{
				const int rowX = i * 2;
				matrix[rowX][0] = dx[i];
				matrix[rowX][1] = dy[i];
				matrix[rowX][2] = 1.0;
				matrix[rowX][6] = -sx[i] * dx[i];
				matrix[rowX][7] = -sx[i] * dy[i];
				matrix[rowX][8] = sx[i];

				const int rowY = rowX + 1;
				matrix[rowY][3] = dx[i];
				matrix[rowY][4] = dy[i];
				matrix[rowY][5] = 1.0;
				matrix[rowY][6] = -sy[i] * dx[i];
				matrix[rowY][7] = -sy[i] * dy[i];
				matrix[rowY][8] = sy[i];
			}
			return solveLinear8(matrix, out);
		}

		bool screenToLocalRectHomography(const int16_t *xs, const int16_t *ys, int lx, int ly, int lw, int lh, double out[8])
		{
			if (!xs || !ys || lw <= 0 || lh <= 0)
				return false;
			const double dx[4] = {static_cast<double>(xs[0]), static_cast<double>(xs[1]), static_cast<double>(xs[2]), static_cast<double>(xs[3])};
			const double dy[4] = {static_cast<double>(ys[0]), static_cast<double>(ys[1]), static_cast<double>(ys[2]), static_cast<double>(ys[3])};
			const double sx[4] = {static_cast<double>(lx), static_cast<double>(lx + lw), static_cast<double>(lx + lw), static_cast<double>(lx)};
			const double sy[4] = {static_cast<double>(ly), static_cast<double>(ly), static_cast<double>(ly + lh), static_cast<double>(ly + lh)};

			double matrix[8][9]{};
			for (int i = 0; i < 4; i++)
			{
				const int rowX = i * 2;
				matrix[rowX][0] = dx[i];
				matrix[rowX][1] = dy[i];
				matrix[rowX][2] = 1.0;
				matrix[rowX][6] = -sx[i] * dx[i];
				matrix[rowX][7] = -sx[i] * dy[i];
				matrix[rowX][8] = sx[i];

				const int rowY = rowX + 1;
				matrix[rowY][3] = dx[i];
				matrix[rowY][4] = dy[i];
				matrix[rowY][5] = 1.0;
				matrix[rowY][6] = -sy[i] * dx[i];
				matrix[rowY][7] = -sy[i] * dy[i];
				matrix[rowY][8] = sy[i];
			}
			return solveLinear8(matrix, out);
		}

		bool mapHomography(const double *h, double px, double py, double *sx, double *sy)
		{
			const double denom = h[6] * px + h[7] * py + 1.0;
			if (std::fabs(denom) < 1e-9)
				return false;
			*sx = (h[0] * px + h[1] * py + h[2]) / denom;
			*sy = (h[3] * px + h[4] * py + h[5]) / denom;
			return std::isfinite(*sx) && std::isfinite(*sy);
		}

		bool roundedCornerContains(double cx, double cy, double rx, double ry, double sx, double sy)
		{
			if (rx <= 0.0 || ry <= 0.0)
				return true;
			const double dx = (sx - cx) / rx;
			const double dy = (sy - cy) / ry;
			return dx * dx + dy * dy <= 1.0;
		}

		bool roundedRectContains(int lx,
														 int ly,
														 int lw,
														 int lh,
														 int tlRx8,
														 int tlRy8,
														 int trRx8,
														 int trRy8,
														 int brRx8,
														 int brRy8,
														 int blRx8,
														 int blRy8,
														 double sx,
														 double sy)
		{
			if (lw <= 0 || lh <= 0)
				return false;
			const double left = static_cast<double>(lx);
			const double top = static_cast<double>(ly);
			const double right = left + static_cast<double>(lw);
			const double bottom = top + static_cast<double>(lh);
			if (sx < left || sy < top || sx >= right || sy >= bottom)
				return false;

			const double tlRx = std::max(0, tlRx8) / 8.0;
			const double tlRy = std::max(0, tlRy8) / 8.0;
			const double trRx = std::max(0, trRx8) / 8.0;
			const double trRy = std::max(0, trRy8) / 8.0;
			const double brRx = std::max(0, brRx8) / 8.0;
			const double brRy = std::max(0, brRy8) / 8.0;
			const double blRx = std::max(0, blRx8) / 8.0;
			const double blRy = std::max(0, blRy8) / 8.0;
			if ((tlRx8 | tlRy8 | trRx8 | trRy8 | brRx8 | brRy8 | blRx8 | blRy8) == 0)
				return true;

			if (tlRx > 0.0 && tlRy > 0.0 && sx < left + tlRx && sy < top + tlRy)
			{
				return roundedCornerContains(left + tlRx, top + tlRy, tlRx, tlRy, sx, sy);
			}
			if (trRx > 0.0 && trRy > 0.0 && sx >= right - trRx && sy < top + trRy)
			{
				return roundedCornerContains(right - trRx, top + trRy, trRx, trRy, sx, sy);
			}
			if (brRx > 0.0 && brRy > 0.0 && sx >= right - brRx && sy >= bottom - brRy)
			{
				return roundedCornerContains(right - brRx, bottom - brRy, brRx, brRy, sx, sy);
			}
			if (blRx > 0.0 && blRy > 0.0 && sx < left + blRx && sy >= bottom - blRy)
			{
				return roundedCornerContains(left + blRx, bottom - blRy, blRx, blRy, sx, sy);
			}
			return true;
		}

		using TransformedRoundedRectCommand = decltype(DisplayCommand{}.transformedRoundedRect);

		struct RoundedRectScreenSpan
		{
			int x;
			int y;
			int w;
			int h;
			int tlRx8;
			int tlRy8;
			int trRx8;
			int trRy8;
			int brRx8;
			int brRy8;
			int blRx8;
			int blRy8;
		};

		bool transformedRoundedRectLooksAffine(const TransformedRoundedRectCommand &r)
		{
			const int parallelogramX = static_cast<int>(r.x0) + static_cast<int>(r.x2) -
																 static_cast<int>(r.x1) - static_cast<int>(r.x3);
			const int parallelogramY = static_cast<int>(r.y0) + static_cast<int>(r.y2) -
																 static_cast<int>(r.y1) - static_cast<int>(r.y3);
			return std::abs(parallelogramX) <= 1 && std::abs(parallelogramY) <= 1;
		}

		bool transformedRoundedRectLooksAxisAligned(const TransformedRoundedRectCommand &r)
		{
			return std::abs(static_cast<int>(r.x0) - static_cast<int>(r.x3)) <= 1 &&
						 std::abs(static_cast<int>(r.x1) - static_cast<int>(r.x2)) <= 1 &&
						 std::abs(static_cast<int>(r.y0) - static_cast<int>(r.y1)) <= 1 &&
						 std::abs(static_cast<int>(r.y3) - static_cast<int>(r.y2)) <= 1 &&
						 std::min(r.x1, r.x2) > std::max(r.x0, r.x3) &&
						 std::min(r.y2, r.y3) > std::max(r.y0, r.y1);
		}

		int scaleRadius8(int radius8, float scale)
		{
			if (radius8 <= 0 || scale <= 0.0f)
				return 0;
			return static_cast<int>(std::lround(static_cast<float>(radius8) * scale));
		}

		bool transformedRoundedRectToScreenSpan(const TransformedRoundedRectCommand &r, RoundedRectScreenSpan *out)
		{
			if (!out || r.lw <= 0 || r.lh <= 0 || !transformedRoundedRectLooksAxisAligned(r))
				return false;
			const float left = 0.5f * static_cast<float>(static_cast<int>(r.x0) + static_cast<int>(r.x3));
			const float right = 0.5f * static_cast<float>(static_cast<int>(r.x1) + static_cast<int>(r.x2));
			const float top = 0.5f * static_cast<float>(static_cast<int>(r.y0) + static_cast<int>(r.y1));
			const float bottom = 0.5f * static_cast<float>(static_cast<int>(r.y2) + static_cast<int>(r.y3));
			const float width = right - left;
			const float height = bottom - top;
			if (width <= 0.0f || height <= 0.0f)
				return false;
			const float scaleX = width / static_cast<float>(r.lw);
			const float scaleY = height / static_cast<float>(r.lh);
			out->x = static_cast<int>(std::lround(left));
			out->y = static_cast<int>(std::lround(top));
			out->w = static_cast<int>(std::lround(width));
			out->h = static_cast<int>(std::lround(height));
			if (out->w <= 0 || out->h <= 0)
				return false;
			out->tlRx8 = scaleRadius8(r.tlRx8, scaleX);
			out->tlRy8 = scaleRadius8(r.tlRy8, scaleY);
			out->trRx8 = scaleRadius8(r.trRx8, scaleX);
			out->trRy8 = scaleRadius8(r.trRy8, scaleY);
			out->brRx8 = scaleRadius8(r.brRx8, scaleX);
			out->brRy8 = scaleRadius8(r.brRy8, scaleY);
			out->blRx8 = scaleRadius8(r.blRx8, scaleX);
			out->blRy8 = scaleRadius8(r.blRy8, scaleY);
			return true;
		}

		bool roundedRectScreenSpanFitsCanvas(const RoundedRectScreenSpan &r)
		{
			// Both direct replay and batching call a Canvas primitive that clamps
			// radii to half the box. Larger CSS curves require the exact shader.
			const int maxRadius8 = std::min(r.w, r.h) * 4;
			if (r.tlRx8 > maxRadius8 || r.trRx8 > maxRadius8 ||
			    r.brRx8 > maxRadius8 || r.blRx8 > maxRadius8) return false;
			return std::abs(r.tlRx8 - r.tlRy8) <= 1 &&
						 std::abs(r.trRx8 - r.trRy8) <= 1 &&
						 std::abs(r.brRx8 - r.brRy8) <= 1 &&
						 std::abs(r.blRx8 - r.blRy8) <= 1;
		}

		bool roundedRectScreenSpanIsCircle(const RoundedRectScreenSpan &r)
		{
			if (r.w <= 0 || r.h <= 0 || std::abs(r.w - r.h) > 1)
				return false;
			const int targetRx8 = r.w * 4;
			const int targetRy8 = r.h * 4;
			return std::abs(r.tlRx8 - targetRx8) <= 8 &&
						 std::abs(r.tlRy8 - targetRy8) <= 8 &&
						 std::abs(r.trRx8 - targetRx8) <= 8 &&
						 std::abs(r.trRy8 - targetRy8) <= 8 &&
						 std::abs(r.brRx8 - targetRx8) <= 8 &&
						 std::abs(r.brRy8 - targetRy8) <= 8 &&
						 std::abs(r.blRx8 - targetRx8) <= 8 &&
						 std::abs(r.blRy8 - targetRy8) <= 8;
		}

		int radius8ToPixels(int radius8)
		{
			if (radius8 <= 0)
				return 0;
			return static_cast<int>(std::lround(static_cast<float>(radius8) * 0.125f));
		}

		bool drawCircleLikeScreenSpan(const RoundedRectScreenSpan &span,
																	gea::framework::graphics::pixel::native_t color)
		{
			// Display::fillCircle is the hard-edged integer scanline circle on every
			// backend (they all forward to Canvas::fillCircle, which has no coverage
			// sampling at all). Taking it while antialiasing is ON silently drops the
			// AA a translated node would otherwise get: the same box drawn WITHOUT a
			// transform goes through Canvas::fillRoundedRect, whose own circle
			// shortcut (fillCircleBox) is itself gated on `antialiasSamples() < 2`
			// precisely so an AA-enabled circle keeps the antialiased rounded-rect
			// rasterizer. Carry that same gate here so a translation cannot change how
			// a shape is rasterized — the axis-aligned span below IS the direct call.
			if (gea::framework::graphics::Canvas::antialiasSamples() >= 2)
				return false;
			if (!roundedRectScreenSpanIsCircle(span))
				return false;
			const int radius = static_cast<int>(std::lround(static_cast<float>(std::min(span.w, span.h)) * 0.5f));
			if (radius <= 0)
				return false;
			const int cx = static_cast<int>(std::lround(static_cast<float>(span.x) + static_cast<float>(span.w) * 0.5f));
			const int cy = static_cast<int>(std::lround(static_cast<float>(span.y) + static_cast<float>(span.h) * 0.5f));
			const std::int64_t t0 = refreshPerfNowUs();
			gea::platform::display::Display::fillCircle(cx, cy, radius, color);
			auto &perf = refreshPerfStatsMutable();
			perf.treeReplayCircleCommands++;
			perf.treeReplayCircleUs += refreshPerfNowUs() - t0;
			return true;
		}

		bool drawAxisAlignedTransformedRoundedRect(const DisplayCommand &command)
		{
			const auto &r = command.transformedRoundedRect;
			RoundedRectScreenSpan span{};
			if (!transformedRoundedRectToScreenSpan(r, &span))
				return false;
			if (!roundedRectScreenSpanFitsCanvas(span))
				return false;

			if (drawCircleLikeScreenSpan(span, r.color))
				return true;

			gea::platform::display::Display::fillRoundedRect(span.x,
																											 span.y,
																											 span.w,
																											 span.h,
																											 radius8ToPixels(span.tlRx8),
																											 radius8ToPixels(span.trRx8),
																											 radius8ToPixels(span.brRx8),
																											 radius8ToPixels(span.blRx8),
																											 r.color);
			return true;
		}

		bool roundedRectRowSpan(const RoundedRectScreenSpan &r, int y, int *outX0, int *outX1)
		{
			if (!outX0 || !outX1 || r.w <= 0 || r.h <= 0 || y < r.y || y >= r.y + r.h)
				return false;
			const float left = static_cast<float>(r.x);
			const float top = static_cast<float>(r.y);
			const float right = left + static_cast<float>(r.w);
			const float bottom = top + static_cast<float>(r.h);
			const float sy = static_cast<float>(y) + 0.5f;
			float minX = left;
			float maxXExclusive = right;

			auto radius = [](int radius8) -> float
			{
				return static_cast<float>(std::max(0, radius8)) * 0.125f;
			};
			auto applyLeftCorner = [&](float rx, float ry, float cx, float cy) -> bool
			{
				if (rx <= 0.0f || ry <= 0.0f)
					return true;
				const float dy = (sy - cy) / ry;
				const float inside = 1.0f - dy * dy;
				if (inside < 0.0f)
					return false;
				const float edge = cx - rx * std::sqrt(inside);
				if (edge > minX)
					minX = edge;
				return true;
			};
			auto applyRightCorner = [&](float rx, float ry, float cx, float cy) -> bool
			{
				if (rx <= 0.0f || ry <= 0.0f)
					return true;
				const float dy = (sy - cy) / ry;
				const float inside = 1.0f - dy * dy;
				if (inside < 0.0f)
					return false;
				const float edge = cx + rx * std::sqrt(inside);
				if (edge < maxXExclusive)
					maxXExclusive = edge;
				return true;
			};

			const float tlRx = radius(r.tlRx8);
			const float tlRy = radius(r.tlRy8);
			if (tlRx > 0.0f && tlRy > 0.0f && sy < top + tlRy &&
					!applyLeftCorner(tlRx, tlRy, left + tlRx, top + tlRy))
				return false;

			const float trRx = radius(r.trRx8);
			const float trRy = radius(r.trRy8);
			if (trRx > 0.0f && trRy > 0.0f && sy < top + trRy &&
					!applyRightCorner(trRx, trRy, right - trRx, top + trRy))
				return false;

			const float brRx = radius(r.brRx8);
			const float brRy = radius(r.brRy8);
			if (brRx > 0.0f && brRy > 0.0f && sy >= bottom - brRy &&
					!applyRightCorner(brRx, brRy, right - brRx, bottom - brRy))
				return false;

			const float blRx = radius(r.blRx8);
			const float blRy = radius(r.blRy8);
			if (blRx > 0.0f && blRy > 0.0f && sy >= bottom - blRy &&
					!applyLeftCorner(blRx, blRy, left + blRx, bottom - blRy))
				return false;

			const int x0 = static_cast<int>(std::ceil(minX - 0.5f));
			const int x1 = static_cast<int>(std::ceil(maxXExclusive - 0.5f)) - 1;
			if (x0 > x1)
				return false;
			*outX0 = x0;
			*outX1 = x1;
			return true;
		}

		bool roundedCornerContainsFast(float cx, float cy, float rx, float ry, float sx, float sy)
		{
			if (rx <= 0.0f || ry <= 0.0f)
				return true;
			const float dx = (sx - cx) / rx;
			const float dy = (sy - cy) / ry;
			return dx * dx + dy * dy <= 1.0f;
		}

		bool convexQuadContainsFast(const int *xs, const int *ys, float sx, float sy)
		{
			bool hasPositive = false;
			bool hasNegative = false;
			for (int i = 0; i < 4; ++i)
			{
				const int j = (i + 1) & 3;
				const float cross = static_cast<float>(xs[j] - xs[i]) * (sy - static_cast<float>(ys[i])) -
														static_cast<float>(ys[j] - ys[i]) * (sx - static_cast<float>(xs[i]));
				if (cross > 1e-5f)
					hasPositive = true;
				else if (cross < -1e-5f)
					hasNegative = true;
				if (hasPositive && hasNegative)
					return false;
			}
			return true;
		}

		inline float antialiasOffsetWithKernel(int sample, int samples, float width)
		{
			return (1.0f - width) * 0.5f +
						 (static_cast<float>(sample) + 0.5f) * width / static_cast<float>(samples);
		}

		inline float antialiasOffset(int sample, int samples)
		{
			return antialiasOffsetWithKernel(sample, samples, 1.0f);
		}

		inline float roundedRectAntialiasKernelWidth(int samples, int w, int h)
		{
			if (samples < 4)
				return 1.0f;
			return std::min(w, h) >= 64 ? 2.0f : 1.0f;
		}

		inline bool transformedRoundedRectIsPillLike(const TransformedRoundedRectCommand &r)
		{
			const int minSide = std::min(static_cast<int>(r.lw), static_cast<int>(r.lh));
			if (minSide < 8)
				return false;
			const int minRadius8 = std::max(8, minSide * 4 - 8);
			return r.tlRx8 >= minRadius8 && r.tlRy8 >= minRadius8 &&
						 r.trRx8 >= minRadius8 && r.trRy8 >= minRadius8 &&
						 r.brRx8 >= minRadius8 && r.brRy8 >= minRadius8 &&
						 r.blRx8 >= minRadius8 && r.blRy8 >= minRadius8;
		}

		inline float transformedRoundedRectAntialiasKernelWidth(int samples, const TransformedRoundedRectCommand &r)
		{
			if (samples < 2)
				return 1.0f;
			if (transformedRoundedRectIsPillLike(r))
				return samples >= 4 ? 2.25f : 2.0f;
			if (samples < 4)
				return 1.0f;
			return roundedRectAntialiasKernelWidth(samples, r.lw, r.lh);
		}

		int fillQuadCoverage(const int *xs, const int *ys, int x, int y, int samples)
		{
			int coverage = 0;
			for (int iy = 0; iy < samples; ++iy)
			{
				const float oy = antialiasOffset(iy, samples);
				for (int ix = 0; ix < samples; ++ix)
				{
					const float ox = antialiasOffset(ix, samples);
					if (convexQuadContainsFast(xs, ys, static_cast<float>(x) + ox, static_cast<float>(y) + oy))
						++coverage;
				}
			}
			return coverage;
		}

		int combinedCoverageAlpha(int alpha, int coverage, int sampleCount)
		{
			if (coverage <= 0 || alpha <= 0)
				return 0;
			if (coverage >= sampleCount)
				return alpha;
			return (alpha * coverage + sampleCount / 2) / sampleCount;
		}

		void paintCoveragePixel(gea::framework::graphics::pixel::native_t &pixel, gea::framework::graphics::pixel::native_t color, int alpha, int coverage, int sampleCount)
		{
			const int effectiveAlpha = combinedCoverageAlpha(alpha, coverage, sampleCount);
			if (effectiveAlpha <= 0)
				return;
			if (effectiveAlpha >= 255)
				pixel = color;
			else
				pixel = gea::framework::graphics::pixel::blendNative(color, pixel, effectiveAlpha);
		}

		bool roundedRectContainsFast(const TransformedRoundedRectCommand &r, float sx, float sy)
		{
			const float left = static_cast<float>(r.lx);
			const float top = static_cast<float>(r.ly);
			const float right = left + static_cast<float>(r.lw);
			const float bottom = top + static_cast<float>(r.lh);
			if (sx < left || sy < top || sx >= right || sy >= bottom)
				return false;

			if ((r.tlRx8 | r.tlRy8 | r.trRx8 | r.trRy8 | r.brRx8 | r.brRy8 | r.blRx8 | r.blRy8) == 0)
				return true;
			const float tlRx = static_cast<float>(std::max(0, static_cast<int>(r.tlRx8))) * 0.125f;
			const float tlRy = static_cast<float>(std::max(0, static_cast<int>(r.tlRy8))) * 0.125f;
			const float trRx = static_cast<float>(std::max(0, static_cast<int>(r.trRx8))) * 0.125f;
			const float trRy = static_cast<float>(std::max(0, static_cast<int>(r.trRy8))) * 0.125f;
			const float brRx = static_cast<float>(std::max(0, static_cast<int>(r.brRx8))) * 0.125f;
			const float brRy = static_cast<float>(std::max(0, static_cast<int>(r.brRy8))) * 0.125f;
			const float blRx = static_cast<float>(std::max(0, static_cast<int>(r.blRx8))) * 0.125f;
			const float blRy = static_cast<float>(std::max(0, static_cast<int>(r.blRy8))) * 0.125f;

			if (tlRx > 0.0f && tlRy > 0.0f && sx < left + tlRx && sy < top + tlRy)
				return roundedCornerContainsFast(left + tlRx, top + tlRy, tlRx, tlRy, sx, sy);
			if (trRx > 0.0f && trRy > 0.0f && sx >= right - trRx && sy < top + trRy)
				return roundedCornerContainsFast(right - trRx, top + trRy, trRx, trRy, sx, sy);
			if (brRx > 0.0f && brRy > 0.0f && sx >= right - brRx && sy >= bottom - brRy)
				return roundedCornerContainsFast(right - brRx, bottom - brRy, brRx, brRy, sx, sy);
			if (blRx > 0.0f && blRy > 0.0f && sx < left + blRx && sy >= bottom - blRy)
				return roundedCornerContainsFast(left + blRx, bottom - blRy, blRx, blRy, sx, sy);
			return true;
		}

		int roundedRectCoverageFast(const TransformedRoundedRectCommand &r,
																float localX,
																float localY,
																float stepXForScreenX,
																float stepYForScreenX,
																float stepXForScreenY,
																float stepYForScreenY,
																int samples)
		{
			const float kernelWidth = transformedRoundedRectAntialiasKernelWidth(samples, r);
			int coverage = 0;
			for (int iy = 0; iy < samples; ++iy)
			{
				const float oy = antialiasOffsetWithKernel(iy, samples, kernelWidth);
				for (int ix = 0; ix < samples; ++ix)
				{
					const float ox = antialiasOffsetWithKernel(ix, samples, kernelWidth);
					const float dx = ox - 0.5f;
					const float dy = oy - 0.5f;
					if (roundedRectContainsFast(r,
																			localX + stepXForScreenX * dx + stepXForScreenY * dy,
																			localY + stepYForScreenX * dx + stepYForScreenY * dy))
						++coverage;
				}
			}
			return coverage;
		}

		bool roundedRectSolidCoreContainsFast(const TransformedRoundedRectCommand &r,
																					float localX,
																					float localY,
																					float marginX,
																					float marginY)
		{
			const float left = static_cast<float>(r.lx);
			const float top = static_cast<float>(r.ly);
			const float right = left + static_cast<float>(r.lw);
			const float bottom = top + static_cast<float>(r.lh);
			if (localX - marginX < left || localY - marginY < top ||
					localX + marginX >= right || localY + marginY >= bottom)
				return false;

			if ((r.tlRx8 | r.tlRy8 | r.trRx8 | r.trRy8 | r.brRx8 | r.brRy8 | r.blRx8 | r.blRy8) == 0)
				return true;

			const auto radius = [](int radius8)
			{
				return static_cast<float>(std::max(0, radius8)) * 0.125f;
			};
			const float tlRx = radius(r.tlRx8);
			const float tlRy = radius(r.tlRy8);
			const float trRx = radius(r.trRx8);
			const float trRy = radius(r.trRy8);
			const float brRx = radius(r.brRx8);
			const float brRy = radius(r.brRy8);
			const float blRx = radius(r.blRx8);
			const float blRy = radius(r.blRy8);

			const float leftCornerWidth = std::max(tlRx, blRx);
			const float rightCornerWidth = std::max(trRx, brRx);
			if (localX - marginX >= left + leftCornerWidth &&
					localX + marginX < right - rightCornerWidth)
				return true;

			const float topCornerHeight = std::max(tlRy, trRy);
			const float bottomCornerHeight = std::max(blRy, brRy);
			if (localY - marginY >= top + topCornerHeight &&
					localY + marginY < bottom - bottomCornerHeight)
				return true;

			return false;
		}

		int roundedRectCoverageHomography(const TransformedRoundedRectCommand &r, const double *homography, int x, int y, int samples)
		{
			const float kernelWidth = transformedRoundedRectAntialiasKernelWidth(samples, r);
			int coverage = 0;
			for (int iy = 0; iy < samples; ++iy)
			{
				const double oy = static_cast<double>(antialiasOffsetWithKernel(iy, samples, kernelWidth));
				for (int ix = 0; ix < samples; ++ix)
				{
					const double ox = static_cast<double>(antialiasOffsetWithKernel(ix, samples, kernelWidth));
					double sx = 0.0;
					double sy = 0.0;
					if (!mapHomography(homography, static_cast<double>(x) + ox, static_cast<double>(y) + oy, &sx, &sy))
						continue;
					if (roundedRectContains(r.lx,
																	r.ly,
																	r.lw,
																	r.lh,
																	r.tlRx8,
																	r.tlRy8,
																	r.trRx8,
																	r.trRy8,
																	r.brRx8,
																	r.brRy8,
																	r.blRx8,
																	r.blRy8,
																	sx,
																	sy))
						++coverage;
				}
			}
			return coverage;
		}

		bool nodeFilterShapeContains(const Node &node, double sx, double sy)
		{
			const double x = static_cast<double>(node.layout.x);
			const double y = static_cast<double>(node.layout.y);
			const double w = static_cast<double>(node.layout.width);
			const double h = static_cast<double>(node.layout.height);
			if (w <= 0.0 || h <= 0.0)
				return false;
			if (sx < x || sy < y || sx >= x + w || sy >= y + h)
				return false;

			if (isFullyRoundedFilterShape(node))
			{
				const double rx = w * 0.5;
				const double ry = h * 0.5;
				if (rx <= 0.0 || ry <= 0.0)
					return false;
				const double nx = (sx - (x + rx)) / rx;
				const double ny = (sy - (y + ry)) / ry;
				return nx * nx + ny * ny <= 1.0;
			}

			const double radius = static_cast<double>(std::min(std::min(node.style.border_radius[0], node.style.border_radius[1]),
																												 std::min(node.style.border_radius[2], node.style.border_radius[3])));
			const double r = std::max(0.0, std::min(radius, std::min(w, h) * 0.5));
			if (r <= 0.0)
				return true;
			const double cx = std::min(std::max(sx, x + r), x + w - r);
			const double cy = std::min(std::max(sy, y + r), y + h - r);
			const double dx = sx - cx;
			const double dy = sy - cy;
			return dx * dx + dy * dy <= r * r;
		}

		int analyticFilterShapeCoverage(const Node &node, const double *homography, double px, double py)
		{
			static constexpr double kOffsets[4][2] = {
					{-0.25, -0.25},
					{0.25, -0.25},
					{-0.25, 0.25},
					{0.25, 0.25},
			};
			int covered = 0;
			for (const auto &offset : kOffsets)
			{
				double sx = 0.0;
				double sy = 0.0;
				if (!mapHomography(homography, px + offset[0], py + offset[1], &sx, &sy))
					continue;
				if (nodeFilterShapeContains(node, sx, sy))
					covered++;
			}
			return (covered * 255 + 2) / 4;
		}

		int localFilterShapeCoverage(const Node &node, double sx, double sy)
		{
			static constexpr double kOffsets[4][2] = {
					{-0.25, -0.25},
					{0.25, -0.25},
					{-0.25, 0.25},
					{0.25, 0.25},
			};
			int covered = 0;
			for (const auto &offset : kOffsets)
			{
				if (nodeFilterShapeContains(node, sx + offset[0], sy + offset[1]))
					covered++;
			}
			return (covered * 255 + 2) / 4;
		}

		int sampleAlphaBilinear(const std::uint8_t *alpha, int width, int height, double x, double y)
		{
			if (!alpha || width <= 0 || height <= 0)
				return 0;
			if (x < 0.0 || y < 0.0 || x > static_cast<double>(width - 1) || y > static_cast<double>(height - 1))
				return 0;
			const int x0 = clampInt(static_cast<int>(std::floor(x)), 0, width - 1);
			const int y0 = clampInt(static_cast<int>(std::floor(y)), 0, height - 1);
			const int x1 = clampInt(x0 + 1, 0, width - 1);
			const int y1 = clampInt(y0 + 1, 0, height - 1);
			const double tx = x - static_cast<double>(x0);
			const double ty = y - static_cast<double>(y0);
			const double a00 = static_cast<double>(alpha[y0 * width + x0]);
			const double a10 = static_cast<double>(alpha[y0 * width + x1]);
			const double a01 = static_cast<double>(alpha[y1 * width + x0]);
			const double a11 = static_cast<double>(alpha[y1 * width + x1]);
			const double top = a00 + (a10 - a00) * tx;
			const double bottom = a01 + (a11 - a01) * tx;
			return clampInt(static_cast<int>(std::lround(top + (bottom - top) * ty)), 0, 255);
		}

		int sampleAlpha16BilinearToByte(const gea::framework::graphics::pixel::native_t *alpha, int width, int height, double x, double y)
		{
			if (!alpha || width <= 0 || height <= 0)
				return 0;
			if (x < 0.0 || y < 0.0 || x > static_cast<double>(width - 1) || y > static_cast<double>(height - 1))
				return 0;
			const int x0 = clampInt(static_cast<int>(std::floor(x)), 0, width - 1);
			const int y0 = clampInt(static_cast<int>(std::floor(y)), 0, height - 1);
			const int x1 = clampInt(x0 + 1, 0, width - 1);
			const int y1 = clampInt(y0 + 1, 0, height - 1);
			const double tx = x - static_cast<double>(x0);
			const double ty = y - static_cast<double>(y0);
			const double a00 = static_cast<double>(alpha[y0 * width + x0]);
			const double a10 = static_cast<double>(alpha[y0 * width + x1]);
			const double a01 = static_cast<double>(alpha[y1 * width + x0]);
			const double a11 = static_cast<double>(alpha[y1 * width + x1]);
			const double top = a00 + (a10 - a00) * tx;
			const double bottom = a01 + (a11 - a01) * tx;
			const int fixedAlpha = clampInt(static_cast<int>(std::lround(top + (bottom - top) * ty)), 0, 65025);
			return clampInt((fixedAlpha + 127) / 255, 0, 255);
		}

		double smoothStep(double edge0, double edge1, double x)
		{
			if (edge0 == edge1)
				return x < edge0 ? 0.0 : 1.0;
			double t = (x - edge0) / (edge1 - edge0);
			if (t < 0.0)
				t = 0.0;
			if (t > 1.0)
				t = 1.0;
			return t * t * (3.0 - 2.0 * t);
		}

		int softEllipseFilterCoverage(const Node &node, double sx, double sy)
		{
			const double x = static_cast<double>(node.layout.x);
			const double y = static_cast<double>(node.layout.y);
			const double w = static_cast<double>(node.layout.width);
			const double h = static_cast<double>(node.layout.height);
			const double rx = w * 0.5;
			const double ry = h * 0.5;
			if (rx <= 0.0 || ry <= 0.0)
				return 0;
			const double blur = std::max(1.0, static_cast<double>(rstyle(node.style).filter_blur_radius));
			const double dx = std::fabs(sx - (x + rx));
			const double dy = std::fabs(sy - (y + ry));
			const double alphaX = 1.0 - smoothStep(rx, rx + blur * 2.0, dx);
			const double alphaY = 1.0 - smoothStep(0.0, ry + blur * 2.0, dy);
			const double coverage = alphaX * alphaY;
			return clampInt(static_cast<int>(std::lround(coverage * 255.0)), 0, 255);
		}

		bool buildProjectedFilterBlurCache(const DisplayCommand &command, FilterBlurCacheEntry &cache)
		{
			const int nodeId = command.filterBlur.nodeId;
			auto &tree = Tree::instance();
			if (nodeId < 0 || nodeId >= tree.nodeCount())
				return false;
			const Node &node = tree.node(nodeId);
			if (!simpleFilterBlurBackgroundNode(node))
				return false;
			if (command.filterBlur.sourceAlphaCap <= 0)
				return false;

			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || canvas->width() <= 0 || canvas->height() <= 0 || !cache.backgroundValid)
				return false;
			int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
			filterBlurBounds(command, canvas->width(), canvas->height(), &x0, &y0, &x1, &y1);
			if (x0 > x1 || y0 > y1)
				return false;
			const int screenW = x1 - x0 + 1;
			const int screenH = y1 - y0 + 1;
			const int screenPixelCount = screenW * screenH;
			const int radius = std::max(0, static_cast<int>(command.filterBlur.radius));
			const int spread = radius * kFilterBlurPasses;
			const int localW = node.layout.width + spread * 2;
			const int localH = node.layout.height + spread * 2;
			if (localW <= 0 || localH <= 0)
				return false;
			const long long localPixelCount64 = static_cast<long long>(localW) * static_cast<long long>(localH);
			if (localPixelCount64 <= 0 || localPixelCount64 > 2147483647LL)
				return false;
			const int localPixelCount = static_cast<int>(localPixelCount64);
			if (!ensureFilterBlurCache(cache, std::max(screenPixelCount, localPixelCount)))
				return false;

			double homography[8]{};
			if (!screenToNodeRectHomography(node, homography))
				return false;

			const int sourceAlpha = clampInt(command.filterBlur.sourceAlphaCap, 0, 255);
			const double localX = static_cast<double>(node.layout.x - spread);
			const double localY = static_cast<double>(node.layout.y - spread);
			std::fill(cache.pixels, cache.pixels + localPixelCount, static_cast<gea::framework::graphics::pixel::native_t>(0));
			for (int row = 0; row < localH; row++)
			{
				for (int col = 0; col < localW; col++)
				{
					const double sx = localX + static_cast<double>(col) + 0.5;
					const double sy = localY + static_cast<double>(row) + 0.5;
					const int coverage = localFilterShapeCoverage(node, sx, sy);
					if (coverage <= 0)
						continue;
					cache.pixels[row * localW + col] = static_cast<gea::framework::graphics::pixel::native_t>(coverage * sourceAlpha);
				}
			}

			if (radius > 0)
			{
				for (int pass = 0; pass < kFilterBlurPasses; pass++)
				{
					blurAlpha16Horizontal(cache.pixels, cache.scratch, localW, localH, radius);
					blurAlpha16Vertical(cache.scratch, cache.pixels, localW, localH, radius);
				}
			}
			std::memcpy(cache.scratch, cache.pixels, static_cast<std::size_t>(localPixelCount) * sizeof(gea::framework::graphics::pixel::native_t));

			std::fill(cache.alpha, cache.alpha + screenPixelCount, static_cast<std::uint8_t>(0));
			std::fill(cache.pixels, cache.pixels + screenPixelCount, static_cast<gea::framework::graphics::pixel::native_t>(0));
			for (int row = 0; row < screenH; row++)
			{
				for (int col = 0; col < screenW; col++)
				{
					const double px = static_cast<double>(x0 + col) + 0.5;
					const double py = static_cast<double>(y0 + row) + 0.5;
					double sx = 0.0;
					double sy = 0.0;
					if (!mapHomography(homography, px, py, &sx, &sy))
						continue;
					const double sampleX = sx - localX - 0.5;
					const double sampleY = sy - localY - 0.5;
					const int alpha = sampleAlpha16BilinearToByte(cache.scratch, localW, localH, sampleX, sampleY);
					if (alpha <= 0)
						continue;
					const int index = row * screenW + col;
					cache.alpha[index] = static_cast<std::uint8_t>(alpha);
					cache.pixels[index] = node.style.bg_color;
				}
			}

			cache.x = x0;
			cache.y = y0;
			cache.w = screenW;
			cache.h = screenH;
			cache.radiusX = command.filterBlur.radiusX;
			cache.radiusY = command.filterBlur.radiusY;
			cache.sourceAlphaCap = command.filterBlur.sourceAlphaCap;
			cache.serial = state.displayListSerial;
			cache.valid = true;
			return true;
		}

		void restoreFilterBlurBackground(const FilterBlurCacheEntry &cache)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || !cache.backgroundValid)
				return;
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			for (int row = 0; row < cache.h; row++)
			{
				gea::framework::graphics::pixel::native_t *dst = canvas->pixels() + canvas->rowToPhysical(cache.y + row) * stride + cache.x;
				std::memcpy(dst, cache.background + row * cache.w, static_cast<std::size_t>(cache.w) * sizeof(gea::framework::graphics::pixel::native_t));
			}
		}

		void compositeFilterBlurLayer(const FilterBlurCacheEntry &cache)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || !cache.valid)
				return;

			int clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			const int drawX0 = std::max(cache.x, clipX0);
			const int drawY0 = std::max(cache.y, clipY0);
			const int drawX1 = std::min(cache.x + cache.w - 1, clipX1);
			const int drawY1 = std::min(cache.y + cache.h - 1, clipY1);
			if (drawX0 > drawX1 || drawY0 > drawY1)
				return;

			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			for (int y = drawY0; y <= drawY1; y++)
			{
				const int localY = y - cache.y;
				gea::framework::graphics::pixel::native_t *dst = canvas->pixels() + canvas->rowToPhysical(y) * stride + drawX0;
				for (int x = drawX0; x <= drawX1; x++, dst++)
				{
					const int index = localY * cache.w + (x - cache.x);
					const int alpha = cache.alpha[index];
					if (alpha <= 0)
						continue;
					if (alpha >= 255)
						*dst = cache.pixels[index];
					else
						*dst = blendDithered(cache.pixels[index], *dst, alpha, x, y);
				}
			}
			canvas->markDirty(drawX0, drawY0, drawX1, drawY1);
		}

		bool buildAnalyticFilterBlurCache(const DisplayCommand &command, FilterBlurCacheEntry &cache)
		{
			const int nodeId = command.filterBlur.nodeId;
			auto &tree = Tree::instance();
			if (nodeId < 0 || nodeId >= tree.nodeCount())
				return false;
			const Node &node = tree.node(nodeId);
			if (!simpleFilterBlurBackgroundNode(node))
				return false;
			if (command.filterBlur.sourceAlphaCap <= 0)
				return false;

			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || canvas->width() <= 0 || canvas->height() <= 0 || !cache.backgroundValid)
				return false;
			int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
			filterBlurBounds(command, canvas->width(), canvas->height(), &x0, &y0, &x1, &y1);
			if (x0 > x1 || y0 > y1)
				return false;
			const int w = x1 - x0 + 1;
			const int h = y1 - y0 + 1;
			const int pixelCount = w * h;
			if (!ensureFilterBlurCache(cache, pixelCount))
				return false;

			double homography[8]{};
			if (!screenToNodeRectHomography(node, homography))
				return false;
			std::fill(cache.alpha, cache.alpha + pixelCount, static_cast<std::uint8_t>(0));
			std::fill(cache.pixels, cache.pixels + pixelCount, static_cast<std::uint16_t>(0));

			const int sourceAlpha = clampInt(command.filterBlur.sourceAlphaCap, 0, 255);
			const int radiusX = command.filterBlur.radiusX;
			const int radiusY = command.filterBlur.radiusY;
			const bool hasBlur = radiusX > 0 || radiusY > 0;
			const bool softEllipse = isFullyRoundedFilterShape(node);
			const bool useSoftEllipse = softEllipse && !hasBlur;
			for (int row = 0; row < h; row++)
			{
				for (int col = 0; col < w; col++)
				{
					const double px = static_cast<double>(x0 + col) + 0.5;
					const double py = static_cast<double>(y0 + row) + 0.5;
					int coverage = 0;
					if (useSoftEllipse)
					{
						double sx = 0.0;
						double sy = 0.0;
						if (mapHomography(homography, px, py, &sx, &sy))
							coverage = softEllipseFilterCoverage(node, sx, sy);
					}
					else
					{
						coverage = analyticFilterShapeCoverage(node, homography, px, py);
					}
					if (coverage <= 0)
						continue;
					const int index = row * w + col;
					cache.alpha[index] = static_cast<std::uint8_t>((coverage * sourceAlpha + 127) / 255);
					cache.pixels[index] = node.style.bg_color;
				}
			}

			if (hasBlur)
			{
				for (int pass = 0; pass < kFilterBlurPasses; pass++)
				{
					blurLayerHorizontal(cache.pixels, cache.alpha, cache.scratch, cache.scratchAlpha, w, h, radiusX);
					blurLayerVertical(cache.scratch, cache.scratchAlpha, cache.pixels, cache.alpha, w, h, radiusY);
				}
			}

			cache.x = x0;
			cache.y = y0;
			cache.w = w;
			cache.h = h;
			cache.radiusX = radiusX;
			cache.radiusY = radiusY;
			cache.sourceAlphaCap = command.filterBlur.sourceAlphaCap;
			cache.serial = state.displayListSerial;
			cache.valid = true;
			return true;
		}

		bool buildFilterBlurCache(const DisplayCommand &command, FilterBlurCacheEntry &cache)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0 || !cache.backgroundValid)
				return false;

			int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
			filterBlurBounds(command, canvas->width(), canvas->height(), &x0, &y0, &x1, &y1);
			if (x0 > x1 || y0 > y1)
				return false;
			const int w = x1 - x0 + 1;
			const int h = y1 - y0 + 1;
			const int pixelCount = w * h;
			if (!ensureFilterBlurCache(cache, filterBlurCacheCapacityNeeded(command, pixelCount)))
				return false;
			if (buildProjectedFilterBlurCache(command, cache))
				return true;
			if (buildAnalyticFilterBlurCache(command, cache))
				return true;

			const int radiusX = command.filterBlur.radiusX;
			const int radiusY = command.filterBlur.radiusY;
			const bool hasBlur = radiusX > 0 || radiusY > 0;
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			for (int row = 0; row < h; row++)
			{
				const gea::framework::graphics::pixel::native_t *after = canvas->pixels() + canvas->rowToPhysical(y0 + row) * stride + x0;
				for (int col = 0; col < w; col++)
				{
					const int index = row * w + col;
					const gea::framework::graphics::pixel::native_t beforePixel = cache.background[index];
					const gea::framework::graphics::pixel::native_t afterPixel = after[col];
					if (beforePixel == afterPixel)
					{
						cache.alpha[index] = 0;
						cache.pixels[index] = 0;
						continue;
					}
					int br = 0, bg = 0, bb = 0;
					int ar = 0, ag = 0, ab = 0;
					unpackRgb565To888(beforePixel, &br, &bg, &bb);
					unpackRgb565To888(afterPixel, &ar, &ag, &ab);
					int alpha = channelAlphaRequirement(br, ar);
					alpha = std::max(alpha, channelAlphaRequirement(bg, ag));
					alpha = std::max(alpha, channelAlphaRequirement(bb, ab));
					if (command.filterBlur.sourceAlphaCap >= 0)
						alpha = std::min(alpha, static_cast<int>(command.filterBlur.sourceAlphaCap));
					cache.alpha[index] = static_cast<std::uint8_t>(alpha);
					cache.pixels[index] = deriveLayerColor(beforePixel, afterPixel, alpha);
				}
			}

			if (hasBlur)
			{
				for (int pass = 0; pass < kFilterBlurPasses; pass++)
				{
					blurLayerHorizontal(cache.pixels, cache.alpha, cache.scratch, cache.scratchAlpha, w, h, radiusX);
					blurLayerVertical(cache.scratch, cache.scratchAlpha, cache.pixels, cache.alpha, w, h, radiusY);
				}
			}

			cache.x = x0;
			cache.y = y0;
			cache.w = w;
			cache.h = h;
			cache.radiusX = radiusX;
			cache.radiusY = radiusY;
			cache.sourceAlphaCap = command.filterBlur.sourceAlphaCap;
			cache.serial = state.displayListSerial;
			cache.valid = true;
			return true;
		}

		bool filterBlurCacheMatches(const DisplayCommand &command, const FilterBlurCacheEntry &cache)
		{
			if (!cache.valid || cache.serial != state.displayListSerial)
				return false;
			return filterBlurCacheGeometryMatches(command, cache);
		}

		bool solveLinear8(double matrix[8][9], double out[8])
		{
			for (int col = 0; col < 8; col++)
			{
				int pivotRow = col;
				double pivotAbs = std::fabs(matrix[col][col]);
				for (int row = col + 1; row < 8; row++)
				{
					const double candidate = std::fabs(matrix[row][col]);
					if (candidate > pivotAbs)
					{
						pivotAbs = candidate;
						pivotRow = row;
					}
				}
				if (pivotAbs < 1e-9)
					return false;
				if (pivotRow != col)
				{
					for (int k = col; k < 9; k++)
						std::swap(matrix[col][k], matrix[pivotRow][k]);
				}

				const double pivot = matrix[col][col];
				for (int k = col; k < 9; k++)
					matrix[col][k] /= pivot;
				for (int row = 0; row < 8; row++)
				{
					if (row == col)
						continue;
					const double factor = matrix[row][col];
					if (std::fabs(factor) < 1e-12)
						continue;
					for (int k = col; k < 9; k++)
						matrix[row][k] -= factor * matrix[col][k];
				}
			}

			for (int i = 0; i < 8; i++)
				out[i] = matrix[i][8];
			return true;
		}

		bool screenToProjectedTextHomography(const DisplayCommand &command, double out[8])
		{
			const double dx[4] = {
					static_cast<double>(command.projectedText.x0),
					static_cast<double>(command.projectedText.x1),
					static_cast<double>(command.projectedText.x2),
					static_cast<double>(command.projectedText.x3),
			};
			const double dy[4] = {
					static_cast<double>(command.projectedText.y0),
					static_cast<double>(command.projectedText.y1),
					static_cast<double>(command.projectedText.y2),
					static_cast<double>(command.projectedText.y3),
			};
			const double sx[4] = {
					static_cast<double>(command.projectedText.srcX),
					static_cast<double>(command.projectedText.srcX + command.projectedText.srcW),
					static_cast<double>(command.projectedText.srcX + command.projectedText.srcW),
					static_cast<double>(command.projectedText.srcX),
			};
			const double sy[4] = {
					static_cast<double>(command.projectedText.srcY),
					static_cast<double>(command.projectedText.srcY),
					static_cast<double>(command.projectedText.srcY + command.projectedText.srcH),
					static_cast<double>(command.projectedText.srcY + command.projectedText.srcH),
			};

			double matrix[8][9]{};
			for (int i = 0; i < 4; i++)
			{
				const int rowX = i * 2;
				matrix[rowX][0] = dx[i];
				matrix[rowX][1] = dy[i];
				matrix[rowX][2] = 1.0;
				matrix[rowX][6] = -sx[i] * dx[i];
				matrix[rowX][7] = -sx[i] * dy[i];
				matrix[rowX][8] = sx[i];

				const int rowY = rowX + 1;
				matrix[rowY][3] = dx[i];
				matrix[rowY][4] = dy[i];
				matrix[rowY][5] = 1.0;
				matrix[rowY][6] = -sy[i] * dx[i];
				matrix[rowY][7] = -sy[i] * dy[i];
				matrix[rowY][8] = sy[i];
			}
			return solveLinear8(matrix, out);
		}

		int projectedTextCoverageAt(const DisplayCommand &command,
																const gea::framework::graphics::RasterizedFont &font,
																int sourceX,
																int sourceY)
		{
			if (sourceX < command.projectedText.srcX ||
					sourceY < command.projectedText.srcY ||
					sourceX >= command.projectedText.srcX + command.projectedText.srcW ||
					sourceY >= command.projectedText.srcY + command.projectedText.srcH)
			{
				return 0;
			}

			int penX = command.projectedText.srcX;
			bool wordStart = true;
			for (const char *p = command.projectedText.text; p && *p;)
			{
				gea::framework::graphics::Glyph glyph{};
				const int cp = transformedTextCodepoint(nextUtf8Codepoint(p), command.projectedText.textTransform, wordStart);
				if (!font.glyph(cp, &glyph))
				{
					penX += font.sizePx() / 2;
					continue;
				}

				const int gx = penX + glyph.bearingX;
				const int gy = command.projectedText.srcY + font.ascender() - glyph.bearingY;
				if (sourceX >= gx && sourceX < gx + glyph.width &&
						sourceY >= gy && sourceY < gy + glyph.height)
				{
					return font.coverage(glyph, sourceY - gy, sourceX - gx);
				}
				penX += glyph.advance;
			}
			return 0;
		}

		int projectedTextCoverageSample(const DisplayCommand &command,
																		const gea::framework::graphics::RasterizedFont &font,
																		float sourceX,
																		float sourceY)
		{
			if (sourceX < static_cast<float>(command.projectedText.srcX) ||
					sourceY < static_cast<float>(command.projectedText.srcY) ||
					sourceX >= static_cast<float>(command.projectedText.srcX + command.projectedText.srcW) ||
					sourceY >= static_cast<float>(command.projectedText.srcY + command.projectedText.srcH))
			{
				return 0;
			}

			const int x0 = static_cast<int>(std::floor(sourceX));
			const int y0 = static_cast<int>(std::floor(sourceY));
			const float fx = sourceX - static_cast<float>(x0);
			const float fy = sourceY - static_cast<float>(y0);
			const int c00 = projectedTextCoverageAt(command, font, x0, y0);
			const int c10 = projectedTextCoverageAt(command, font, x0 + 1, y0);
			const int c01 = projectedTextCoverageAt(command, font, x0, y0 + 1);
			const int c11 = projectedTextCoverageAt(command, font, x0 + 1, y0 + 1);
			const float top = static_cast<float>(c00) * (1.0f - fx) + static_cast<float>(c10) * fx;
			const float bottom = static_cast<float>(c01) * (1.0f - fx) + static_cast<float>(c11) * fx;
			const int coverage = static_cast<int>(std::lroundf(top * (1.0f - fy) + bottom * fy));
			if (coverage < 0)
				return 0;
			if (coverage > 255)
				return 255;
			return coverage;
		}

		// Rasterize the label to an axis-aligned coverage buffer (text space) ONCE, so
		// the per-screen-pixel projected loop samples a buffer instead of re-scanning
		// every glyph for every pixel (the old O(pixels * glyphs * 4)). Mirrors the
		// filter-blur cache idea; same layout math as projectedTextCoverageAt.
		// ink[4] (out): {minBx, minBy, maxBx, maxBy} of nonzero coverage in buffer
		// coords — the ink bbox the per-row screen windowing culls against. Empty ink
		// leaves maxBx < minBx.
		void buildProjectedTextAlpha(const DisplayCommand &command,
																 const gea::framework::graphics::RasterizedFont &font,
																 std::uint8_t *buf, int srcW, int srcH, int ink[4])
		{
			std::memset(buf, 0, static_cast<std::size_t>(srcW) * static_cast<std::size_t>(srcH));
			ink[0] = srcW;
			ink[1] = srcH;
			ink[2] = -1;
			ink[3] = -1;
			const int srcX = command.projectedText.srcX;
			const int srcY = command.projectedText.srcY;
			int penX = srcX;
			bool wordStart = true;
			for (const char *p = command.projectedText.text; p && *p;)
			{
				gea::framework::graphics::Glyph glyph{};
				const int cp = transformedTextCodepoint(nextUtf8Codepoint(p), command.projectedText.textTransform, wordStart);
				if (!font.glyph(cp, &glyph))
				{
					penX += font.sizePx() / 2;
					continue;
				}
				const int gx = penX + glyph.bearingX;
				const int gy = srcY + font.ascender() - glyph.bearingY;
				for (int row = 0; row < glyph.height; row++)
				{
					const int by = gy + row - srcY;
					if (by < 0 || by >= srcH)
						continue;
					std::uint8_t *bufRow = buf + static_cast<std::size_t>(by) * static_cast<std::size_t>(srcW);
					for (int col = 0; col < glyph.width; col++)
					{
						const int bx = gx + col - srcX;
						if (bx < 0 || bx >= srcW)
							continue;
						const int cov = font.coverage(glyph, row, col);
						if (cov > bufRow[bx])
						{
							bufRow[bx] = static_cast<std::uint8_t>(cov);
							if (bx < ink[0])
								ink[0] = bx;
							if (by < ink[1])
								ink[1] = by;
							if (bx > ink[2])
								ink[2] = bx;
							if (by > ink[3])
								ink[3] = by;
						}
					}
				}
				penX += glyph.advance;
			}
		}

		GEA_RENDER_HOT_SRAM void drawProjectedText(const DisplayCommand &authoredCommand,
		    uint8_t *coverageRow = nullptr, int coverageX = 0)
		{
			// Keep deferred commands pointing to authored node storage. Normalize
			// locally on replay, using the same content as layout and cache hashing.
			std::string preparedText;
			DisplayCommand command = authoredCommand;
			command.projectedText.text = TextRenderer::prepareText(command.projectedText.text,
			    command.projectedText.textTransform, command.projectedText.whiteSpace, preparedText);
			command.projectedText.textTransform = 0;
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return;
			if (!command.projectedText.text || !command.projectedText.text[0])
				return;
			if ((!coverageRow && command.projectedText.alpha == 0) || command.projectedText.srcW <= 0 || command.projectedText.srcH <= 0)
				return;

			// Back-face cull (CSS backface-visibility:hidden) — same winding test as the gradient
			// face: drop the label whose projected quad faces away, so it disappears with its
			// back-facing parent face instead of floating as mirrored text over the backdrop.
			if (command.projectedText.backfaceHidden)
			{
				const auto &p = command.projectedText;
				const long area2 = static_cast<long>(p.x0) * p.y1 - static_cast<long>(p.x1) * p.y0 +
													 static_cast<long>(p.x1) * p.y2 - static_cast<long>(p.x2) * p.y1 +
													 static_cast<long>(p.x2) * p.y3 - static_cast<long>(p.x3) * p.y2 +
													 static_cast<long>(p.x3) * p.y0 - static_cast<long>(p.x0) * p.y3;
				if (area2 <= 0)
					return;
			}

			gea::framework::graphics::RasterizedFont font =
					gea::framework::graphics::FontRegistry::rasterizedFamily(command.projectedText.fontId,
																																	 command.projectedText.fontSize);
			if (!font.valid())
				return;

			double h[8]{};
			if (!screenToProjectedTextHomography(command, h))
				return;

			int clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			const int drawX0 = std::max(std::max(static_cast<int>(command.bx), 0), clipX0);
			const int drawY0 = std::max(std::max(static_cast<int>(command.by), 0), clipY0);
			const int drawX1 = std::min(std::min(static_cast<int>(command.bx + command.bw - 1), canvas->width() - 1), clipX1);
			const int drawY1 = std::min(std::min(static_cast<int>(command.by + command.bh - 1), canvas->height() - 1), clipY1);
			if (drawX0 > drawX1 || drawY0 > drawY1)
				return;

			const int parentAlpha = gea::platform::display::Display::alpha();
			if (!coverageRow && parentAlpha == 0)
				return;

			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			int dirtyX0 = canvas->width();
			int dirtyY0 = canvas->height();
			int dirtyX1 = -1;
			int dirtyY1 = -1;

			// The inverse homography is applied per pixel; run it on the hardware FPU
			// (float). h[] is solved once in double; load it into float locals so the
			// hot loop never touches software-emulated double. This per-pixel divide was
			// the dominant cost of projected-text replay (the css-3d-cube faces).
			const float fh0 = static_cast<float>(h[0]), fh1 = static_cast<float>(h[1]), fh2 = static_cast<float>(h[2]);
			const float fh3 = static_cast<float>(h[3]), fh4 = static_cast<float>(h[4]), fh5 = static_cast<float>(h[5]);
			const float fh6 = static_cast<float>(h[6]), fh7 = static_cast<float>(h[7]);

			// Rasterize the label to a coverage buffer once (text space), then sample it
			// per screen pixel — replaces the per-pixel O(glyphs) scan that dominated
			// projected-text replay. Lazily grown, reused across frames (single render
			// thread). Falls back to the per-pixel scan if the label is implausibly large.
			const int srcW = command.projectedText.srcW;
			const int srcH = command.projectedText.srcH;
			const bool useBuffer = static_cast<long long>(srcW) * static_cast<long long>(srcH) <= (1LL << 19);
			const std::uint8_t *cb = nullptr;
			int inkX0 = 0, inkY0 = 0, inkX1 = srcW - 1, inkY1 = srcH - 1;
			if (useBuffer)
			{
				// Coverage buffer is text-space (independent of the 3D projection), so it's
				// identical frame-to-frame for a static label. The rebuild was ~half the
				// projected-text replay cost (e.g. css-3d-cube spins but its 6 face labels
				// never change). Cache by content hash + font + size + dims so a spinning
				// label is rasterized once, while dynamic text (same buffer, new content)
				// still rebuilds on a hash miss.
				std::uint32_t textHash = 2166136261u;
				for (const char *p = command.projectedText.text; *p; p++)
				{
					textHash ^= static_cast<unsigned char>(*p);
					textHash *= 16777619u;
				}
				struct CoverageCacheEntry
				{
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
					// Not inlined: the sentinel initializers would make the cache a .data
					// object (see state_init.h).
					__attribute__((noinline)) CoverageCacheEntry() {}
#endif
					std::uint32_t hash = 0;
					int fontId = -2, fontSize = -1, srcW = -1, srcH = -1, textTransform = -1;
					int bytes = 0;
					int ink[4] = {0, 0, -1, -1};
					bool valid = false;
					bool inSram = false;
					std::uint8_t *sramBuf = nullptr;
					int sramCapacity = 0;
					std::vector<std::uint8_t> buf;
				};
#if GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES > 0
				alignas(4) static std::uint8_t sramCoveragePool[kProjectedTextCacheBanks][GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES]
						__attribute__((section(".uninitialized_data")));
				static int sramCoverageUsed[kProjectedTextCacheBanks] __attribute__((section(".uninitialized_data")));
#endif
				// Per-core banks: the 2-core band split replays a label that straddles the
				// split line on BOTH cores concurrently — a single shared cache races on
				// the entry (concurrent buf.resize() vs data() read, valid=true published
				// before the fill) and flickers the labels. Same idiom as the pie-bench
				// per-core scratch: each core owns its bank, a straddling label simply
				// rasterizes once per core.
				static CoverageCacheEntry cacheBanks[kProjectedTextCacheBanks][8];
				const int cacheBank = gea_current_render_core() % kProjectedTextCacheBanks;
				CoverageCacheEntry(&cache)[8] = cacheBanks[cacheBank];
				const int fid = command.projectedText.fontId;
				const int fsz = command.projectedText.fontSize;
				const int ttf = command.projectedText.textTransform;
				const int needBytes = srcW * srcH;
				auto &perf = refreshPerfStatsMutable();
				perf.projectedTextCacheCalls++;
				if (needBytes > perf.projectedTextCacheMaxEntryBytes)
					perf.projectedTextCacheMaxEntryBytes = needBytes;
				CoverageCacheEntry *hit = nullptr;
				for (auto &e : cache)
				{
					if (e.valid && e.hash == textHash && e.fontId == fid && e.fontSize == fsz &&
							e.srcW == srcW && e.srcH == srcH && e.textTransform == ttf)
					{
						hit = &e;
						break;
					}
				}
				if (!hit)
				{
					perf.projectedTextCacheMisses++;
					// Prefer an empty slot (8 slots > the handful of distinct labels in a
					// scene), else evict the hash-mapped slot.
					int evict = textHash & 7;
					for (int i = 0; i < 8; i++)
						if (!cache[i].valid)
						{
							evict = i;
							break;
						}
					CoverageCacheEntry &e = cache[evict];
					e.valid = false;
					e.hash = textHash;
					e.fontId = fid;
					e.fontSize = fsz;
					e.srcW = srcW;
					e.srcH = srcH;
					e.textTransform = ttf;
					e.bytes = needBytes;
					std::uint8_t *dstBuf = nullptr;
#if GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES > 0
					if (e.sramBuf && e.sramCapacity >= needBytes)
					{
						dstBuf = e.sramBuf;
					}
					else
					{
						int &used = sramCoverageUsed[cacheBank];
						const int aligned = (used + 3) & ~3;
						if (needBytes <= GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES - aligned)
						{
							dstBuf = sramCoveragePool[cacheBank] + aligned;
							e.sramBuf = dstBuf;
							e.sramCapacity = needBytes;
							used = aligned + needBytes;
						}
					}
#endif
					if (dstBuf)
					{
						if (!e.buf.empty())
							std::vector<std::uint8_t>().swap(e.buf);
						e.inSram = true;
					}
					else
					{
						e.buf.resize(static_cast<std::size_t>(needBytes));
						dstBuf = e.buf.data();
						e.inSram = false;
					}
					buildProjectedTextAlpha(command, font, dstBuf, srcW, srcH, e.ink);
					e.valid = true;
					hit = &e;
				}
				else
				{
					perf.projectedTextCacheHits++;
				}
				int cachedBytes = 0;
				int sramBytes = 0;
				for (const auto &e : cache)
					if (e.valid)
					{
						cachedBytes += e.bytes;
						if (e.inSram)
							sramBytes += e.bytes;
					}
				if (cachedBytes > perf.projectedTextCacheBytes)
					perf.projectedTextCacheBytes = cachedBytes;
				if (sramBytes > perf.projectedTextCacheSramBytes)
					perf.projectedTextCacheSramBytes = sramBytes;
				if (hit->inSram)
				{
					perf.projectedTextCacheSramUses++;
					cb = hit->sramBuf;
				}
				else
				{
					perf.projectedTextCachePsramUses++;
					cb = hit->buf.data();
				}
				// A label with no ink at all draws nothing — skip the projected loop.
				if (hit->ink[2] < hit->ink[0])
					return;
				inkX0 = hit->ink[0];
				inkY0 = hit->ink[1];
				inkX1 = hit->ink[2];
				inkY1 = hit->ink[3];
			}
			else
			{
				refreshPerfStatsMutable().projectedTextCacheFallbacks++;
			}
			const float srcXf = static_cast<float>(command.projectedText.srcX);
			const float srcYf = static_cast<float>(command.projectedText.srcY);
			// Ink window in text-space absolute coords, padded 2px: bilinear sampling
			// reaches 1px past an ink cell, plus float slop. The per-row culling below
			// uses these as conservative bounds — pixels mapping outside can never blend.
			const bool inkWindow = cb != nullptr;
			const float inkSxMin = srcXf + static_cast<float>(inkX0) - 2.0f;
			const float inkSxMax = srcXf + static_cast<float>(inkX1) + 2.0f;
			const float inkSyMin = srcYf + static_cast<float>(inkY0) - 2.0f;
			const float inkSyMax = srcYf + static_cast<float>(inkY1) + 2.0f;

			const float rowX0 = static_cast<float>(drawX0) + 0.5f;
			// Perspective-correct subdivision. The inverse homography needs a per-pixel
			// reciprocal (1/denom) to recover text-space (sx,sy); on the LX7 FPU (no HW
			// divide) that reciprocal was the dominant projected-text cost. The cube's
			// per-face perspective is mild, so compute the exact mapping only at span
			// boundaries (every kSpan px) and linearly interpolate (sx,sy) between,
			// resyncing to the exact value at each boundary so error never accumulates.
			// ~kSpan× fewer divides, sub-pixel accurate here — mirrors the gradient-face
			// path, which dropped its own per-pixel divide for the same reason.
			constexpr int kSpan = 16;
			constexpr float kInvSpan = 1.0f / static_cast<float>(kSpan);
			const gea::framework::graphics::pixel::native_t textColorNative = command.projectedText.color;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
			int textR5 = 0, textG6 = 0, textB5 = 0;
			gea::framework::graphics::pixel::unpackRgb565(textColorNative, &textR5, &textG6, &textB5);
			const int textR8 = gea::framework::graphics::pixel::detail::kExp5.v[textR5];
			const int textG8 = gea::framework::graphics::pixel::detail::kExp6.v[textG6];
			const int textB8 = gea::framework::graphics::pixel::detail::kExp5.v[textB5];
			auto blendTextRgb565 = [&](gea::framework::graphics::pixel::native_t bg, int alpha) {
				int bgR5 = 0, bgG6 = 0, bgB5 = 0;
				gea::framework::graphics::pixel::unpackRgb565(bg, &bgR5, &bgG6, &bgB5);
				const int inverse = 255 - alpha;
				const int vr = textR8 * alpha + gea::framework::graphics::pixel::detail::kExp5.v[bgR5] * inverse;
				const int vg = textG8 * alpha + gea::framework::graphics::pixel::detail::kExp6.v[bgG6] * inverse;
				const int vb = textB8 * alpha + gea::framework::graphics::pixel::detail::kExp5.v[bgB5] * inverse;
				const int r8 = ((vr + 128) * 257) >> 16;
				const int g8 = ((vg + 128) * 257) >> 16;
				const int b8 = ((vb + 128) * 257) >> 16;
				return gea::framework::graphics::pixel::packRgb565Components(
						gea::framework::graphics::pixel::detail::kQuant5.v[r8],
						gea::framework::graphics::pixel::detail::kQuant6.v[g8],
						gea::framework::graphics::pixel::detail::kQuant5.v[b8]);
			};
#endif
			for (int y = drawY0; y <= drawY1; y++)
			{
				const float py = static_cast<float>(y) + 0.5f;
				float numSx = fh0 * rowX0 + fh1 * py + fh2;
				float numSy = fh3 * rowX0 + fh4 * py + fh5;
				float denom = fh6 * rowX0 + fh7 * py + 1.0f;
				// Per-row ink culling: most of the projected quad is blank (the labels'
				// ink is a small sub-rect of the label box), but the loop below pays the
				// homography DDA + bilinear sample for every bbox pixel. The row mapping
				// sx(t) = (numSx + fh0·t)/(denom + fh6·t) (t = x - drawX0) is linear-
				// fractional; while denom keeps one sign across the row, each ink-rect
				// bound is a single linear inequality in t, so the x-interval that can
				// possibly touch ink is exact. Rows with an empty interval are skipped
				// outright. A denom sign flip (extreme perspective) falls back to the
				// full row — correctness never depends on the window.
				int xLo = drawX0, xHi = drawX1;
				if (inkWindow)
				{
					const float tMax = static_cast<float>(drawX1 - drawX0);
					const float denomR = denom + fh6 * tMax;
					if (denom != 0.0f && denomR != 0.0f && ((denom > 0.0f) == (denomR > 0.0f)))
					{
						const float sgn = denom > 0.0f ? 1.0f : -1.0f;
						// sgn·(num − k·den)(t) ≥ 0 per bound: a·t + b ≥ 0 form.
						const float a[4] = {sgn * (fh0 - inkSxMin * fh6), sgn * (inkSxMax * fh6 - fh0),
																sgn * (fh3 - inkSyMin * fh6), sgn * (inkSyMax * fh6 - fh3)};
						const float b[4] = {sgn * (numSx - inkSxMin * denom), sgn * (inkSxMax * denom - numSx),
																sgn * (numSy - inkSyMin * denom), sgn * (inkSyMax * denom - numSy)};
						float tLo = 0.0f, tHi = tMax;
						bool rowEmpty = false;
						for (int i = 0; i < 4; i++)
						{
							if (a[i] > 1e-9f)
							{
								const float t = -b[i] / a[i];
								if (t > tLo)
									tLo = t;
							}
							else if (a[i] < -1e-9f)
							{
								const float t = -b[i] / a[i];
								if (t < tHi)
									tHi = t;
							}
							else if (b[i] < 0.0f)
							{
								rowEmpty = true;
								break;
							}
						}
						if (rowEmpty || tLo > tHi)
							continue;
						// ±1 px slop pad around the float interval.
						xLo = drawX0 + static_cast<int>(tLo) - 1;
						xHi = drawX0 + static_cast<int>(tHi) + 2;
						if (xLo < drawX0)
							xLo = drawX0;
						if (xHi > drawX1)
							xHi = drawX1;
						if (xLo > xHi)
							continue;
						const float t0 = static_cast<float>(xLo - drawX0);
						numSx += fh0 * t0;
						numSy += fh3 * t0;
						denom += fh6 * t0;
					}
				}
				gea::framework::graphics::pixel::native_t *dst = canvas->pixels() + canvas->rowToPhysical(y) * stride + xLo;
				// Exact mapping at the first pixel of the row (window).
				float sx, sy;
				{
					const float inv = std::fabs(denom) < 1e-6f ? 0.0f : 1.0f / denom;
					sx = numSx * inv;
					sy = numSy * inv;
				}
				int x = xLo;
				while (x <= xHi)
				{
					// Map [x, xEnd) this iteration; xEnd is the next exact boundary.
					const int xEnd = x + kSpan <= xHi ? x + kSpan : xHi + 1;
					const int segLen = xEnd - x;
					const float segLenF = static_cast<float>(segLen);
					const float numSxE = numSx + fh0 * segLenF;
					const float numSyE = numSy + fh3 * segLenF;
					const float denomE = denom + fh6 * segLenF;
					float sxE, syE;
					{
						const float invE = std::fabs(denomE) < 1e-6f ? 0.0f : 1.0f / denomE;
						sxE = numSxE * invE;
						syE = numSyE * invE;
					}
					const float invSeg = segLen == kSpan ? kInvSpan : 1.0f / segLenF;
					const float dsx = (sxE - sx) * invSeg;
					const float dsy = (syE - sy) * invSeg;
					// Fixed-point (Q16) bilinear DDA for the coverage-cache fast path. The
					// per-pixel float bilinear (≈6 fmul + 2 float→int) was the projected-text
					// hot cost; (sx,sy) advance linearly within a segment, so seed Q16 source
					// coords at the segment origin (synced to the exact float mapping) and step
					// by the Q16 delta — integer mul/shift only, no per-pixel float. The 8-bit
					// fractional weight matches the float sample within ≤1 LSB of coverage
					// (sub-perceptual; same tolerance the transformed-gradient DDA runs at).
					std::int32_t bxQ = static_cast<std::int32_t>((sx - srcXf) * 65536.0f);
					std::int32_t byQ = static_cast<std::int32_t>((sy - srcYf) * 65536.0f);
					const std::int32_t dbxQ = static_cast<std::int32_t>(dsx * 65536.0f);
					const std::int32_t dbyQ = static_cast<std::int32_t>(dsy * 65536.0f);
					for (int xi = x; xi < xEnd; xi++, dst++, sx += dsx, sy += dsy, bxQ += dbxQ, byQ += dbyQ)
					{
						int coverage;
						if (cb)
						{
							if (bxQ < 0 || byQ < 0)
								continue;
							const int bx0 = bxQ >> 16;
							const int by0 = byQ >> 16;
							if (bx0 >= srcW || by0 >= srcH)
								continue;
							const int fx = (bxQ >> 8) & 0xFF; // Q8 fractional weight (0..255)
							const int fy = (byQ >> 8) & 0xFF;
							const int bx1 = bx0 + 1 < srcW ? bx0 + 1 : bx0;
							const int by1 = by0 + 1 < srcH ? by0 + 1 : by0;
							const std::uint8_t *r0 = cb + static_cast<std::size_t>(by0) * srcW;
							const std::uint8_t *r1 = cb + static_cast<std::size_t>(by1) * srcW;
							const int a00 = r0[bx0], a10 = r0[bx1], a01 = r1[bx0], a11 = r1[bx1];
							const int top = a00 * (256 - fx) + a10 * fx; // ×256 (Q8 across x)
							const int bot = a01 * (256 - fx) + a11 * fx; // ×256
							coverage = (top * (256 - fy) + bot * fy + 32768) >> 16; // /65536, +0.5 round
						}
						else
						{
							coverage = projectedTextCoverageSample(command, font, sx, sy);
						}
						if (coverage <= 0)
							continue;

						if (coverageRow) {
							coverageRow[xi - coverageX] = std::max<int>(coverageRow[xi - coverageX], coverage);
							continue;
						}
						int alpha = (coverage * static_cast<int>(command.projectedText.alpha) + 127) / 255;
						if (parentAlpha != 255) // parentAlpha==255 makes the second scale an identity
							alpha = (alpha * parentAlpha + 127) / 255;
						if (alpha <= 0)
							continue;
						if (alpha >= 255)
							*dst = textColorNative;
						else
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
							*dst = blendTextRgb565(*dst, alpha);
#else
							*dst = gea::framework::graphics::pixel::blendNative(textColorNative, *dst, alpha);
#endif

						if (xi < dirtyX0)
							dirtyX0 = xi;
						if (xi > dirtyX1)
							dirtyX1 = xi;
						if (y < dirtyY0)
							dirtyY0 = y;
						if (y > dirtyY1)
							dirtyY1 = y;
					}
					// Resync to the exact mapping at the boundary (no accumulated drift).
					numSx = numSxE;
					numSy = numSyE;
					denom = denomE;
					sx = sxE;
					sy = syE;
					x = xEnd;
				}
			}

			if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
				canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
		}

		// Premultiplied-alpha color lerp between two RGB565 stops — byte-for-byte
		// identical to view.cpp's interpolatePremultipliedColor so the per-pixel
		// transformed gradient matches the colors the old strip path produced
		// (panel-endian aware via fromRgb888).
		uint16_t tgPremulColor(uint16_t fromColor, uint8_t fromAlpha, uint16_t toColor, uint8_t toAlpha, int permille)
		{
			if (permille <= 0)
				return fromColor;
			if (permille >= 1000)
				return toColor;
			int fr = 0, fg = 0, fb = 0, tr = 0, tg = 0, tb = 0;
			gea::framework::graphics::pixel::unpackRgb565(fromColor, &fr, &fg, &fb);
			gea::framework::graphics::pixel::unpackRgb565(toColor, &tr, &tg, &tb);
			fr = (fr * 255 + 15) / 31;
			fg = (fg * 255 + 31) / 63;
			fb = (fb * 255 + 15) / 31;
			tr = (tr * 255 + 15) / 31;
			tg = (tg * 255 + 31) / 63;
			tb = (tb * 255 + 15) / 31;
			const int fw = static_cast<int>(fromAlpha) * (1000 - permille);
			const int tw = static_cast<int>(toAlpha) * permille;
			const int wsum = fw + tw;
			if (wsum <= 0)
				return toColor;
			const int r = (fr * fw + tr * tw + wsum / 2) / wsum;
			const int g = (fg * fw + tg * tw + wsum / 2) / wsum;
			const int b = (fb * fw + tb * tw + wsum / 2) / wsum;
			return gea::framework::graphics::pixel::fromRgb888(r, g, b);
		}

		// Same premultiplied interpolation as tgPremulColor but returns the un-quantized
		// RGB888 (not packed to RGB565), so the face LUT can store the per-channel Bayer
		// dither floor+remainder and dither per pixel — otherwise dark transformed
		// gradients (the perspective floor) show hard RGB565 bands.
		void tgPremulRgb888(gea::framework::graphics::pixel::native_t fromColor, uint8_t fromAlpha, gea::framework::graphics::pixel::native_t toColor, uint8_t toAlpha, int permille, int *outR, int *outG, int *outB)
		{
			// Unpack the native gradient endpoints to 8-bit channels (5/6-bit expand on
			// RGB565 — byte-identical; direct bytes on RGBA8888 — true colour).
			int fr = 0, fg = 0, fb = 0, tr = 0, tg = 0, tb = 0;
			unpackRgb565To888(fromColor, &fr, &fg, &fb);
			unpackRgb565To888(toColor, &tr, &tg, &tb);
			if (permille <= 0)
			{
				*outR = fr;
				*outG = fg;
				*outB = fb;
				return;
			}
			if (permille >= 1000)
			{
				*outR = tr;
				*outG = tg;
				*outB = tb;
				return;
			}
			const int fw = static_cast<int>(fromAlpha) * (1000 - permille);
			const int tw = static_cast<int>(toAlpha) * permille;
			const int wsum = fw + tw;
			if (wsum <= 0)
			{
				*outR = tr;
				*outG = tg;
				*outB = tb;
				return;
			}
			*outR = (fr * fw + tr * tw + wsum / 2) / wsum;
			*outG = (fg * fw + tg * tw + wsum / 2) / wsum;
			*outB = (fb * fw + tb * tw + wsum / 2) / wsum;
		}

		uint8_t tgLerpAlpha(uint8_t from, uint8_t to, int permille)
		{
			if (permille <= 0)
				return from;
			if (permille >= 1000)
				return to;
			return static_cast<uint8_t>((static_cast<int>(from) * (1000 - permille) + static_cast<int>(to) * permille + 500) / 1000);
		}

		int tgStopRange(int permille, int start, int end)
		{
			if (permille <= start)
				return 0;
			if (permille >= end)
				return 1000;
			const int span = end - start;
			if (span <= 0)
				return 1000;
			return ((permille - start) * 1000 + span / 2) / span;
		}

		// Per-band dirty bbox for parallel scanline fills (one per core band, merged by
		// the caller). Inverted init (x0=width,x1=-1) so an empty band stays invalid.
		struct FillBandDirty
		{
			int x0;
			int y0;
			int x1;
			int y1;
			int fastPixels = 0;
			std::int64_t fillUs = 0;
			std::int64_t edgeUs = 0;
		};

		// Run body(yStart, yEnd, dirty) over rows [y0,y1]. When the band is tall enough
		// to amortize the cross-core hop, the bottom half is dispatched to the 2nd core
		// (gea_render_parallel_submit) while this core fills the top half, then joins.
		// body MUST be re-entrant: the two invocations run concurrently over disjoint row
		// ranges, each writing only its own framebuffer rows + its own FillBandDirty; all
		// other state body reads (LUT, quad, clip, blend flags) is fixed before the call.
		// Falls back to one in-line invocation when there's no worker or the band is short
		// (dWork is then left untouched).
		template <class Body>
		inline void parallelFillRows(int y0, int y1, FillBandDirty &dMain, FillBandDirty &dWork, Body &body)
		{
			constexpr int kMinRowsToSplit = 64;
			// DIAG: split rate + per-row cost (split vs inline) to see if (a) faces are too
			// short to split and (b) the two bands actually run concurrently.
			static std::int64_t gSplitUs = 0, gInlineUs = 0;
			static int gSplitN = 0, gInlineN = 0, gSplitRows = 0, gInlineRows = 0, gMaxRows = 0;
			const int rows = y1 - y0 + 1;
			if (rows > gMaxRows)
				gMaxRows = rows;
			auto diagReport = [&]()
			{
				if (gSplitN + gInlineN >= 300)
				{
					std::printf("[pfill] split=%d (%dus, %drows avg)  inline=%d (%dus, %drows avg)  maxrows=%d\n",
											gSplitN, gSplitN ? static_cast<int>(gSplitUs / gSplitN) : 0, gSplitN ? gSplitRows / gSplitN : 0,
											gInlineN, gInlineN ? static_cast<int>(gInlineUs / gInlineN) : 0, gInlineN ? gInlineRows / gInlineN : 0,
											gMaxRows);
					gSplitUs = gInlineUs = 0;
					gSplitN = gInlineN = gSplitRows = gInlineRows = gMaxRows = 0;
				}
			};
			if (rows >= kMinRowsToSplit)
			{
				const int mid = (y0 + y1) >> 1;
				struct Trampoline
				{
					Body *body;
					FillBandDirty *dirty;
				} t{&body, &dWork};
				auto run = +[](void *p, int a, int b)
				{
					Trampoline *tr = static_cast<Trampoline *>(p);
					(*tr->body)(a, b, *tr->dirty);
				};
				const std::int64_t t0 = refreshPerfNowUs();
				if (gea_render_parallel_rows_submit(run, &t, mid + 1, y1))
				{
					body(y0, mid, dMain);
					gea_render_parallel_wait();
					gSplitUs += refreshPerfNowUs() - t0;
					gSplitN++;
					gSplitRows += rows;
					diagReport();
					return;
				}
			}
			const std::int64_t t0 = refreshPerfNowUs();
			body(y0, y1, dMain);
			gInlineUs += refreshPerfNowUs() - t0;
			gInlineN++;
			gInlineRows += rows;
			diagReport();
		}

		GEA_RENDER_HOT_SRAM void drawTransformedLinearGradient(const DisplayCommand &command)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return;
			const auto &g = command.transformedGradient;
			if (g.lw <= 0 || g.lh <= 0)
				return;

			// Back-face cull (CSS backface-visibility:hidden). Corners are recorded TL,TR,BR,BL,
			// so in screen space (y down) a front-facing quad has positive shoelace area; once a
			// face rotates past edge-on its projected winding flips negative. A closed opaque
			// solid occludes its own back faces, so dropping them here removes the wasted
			// overdraw (and matches the CSS semantics for translucent faces too). <=0 also drops
			// the degenerate edge-on sliver (zero area — nothing to draw).
			if (g.backfaceHidden)
			{
				const long area2 = static_cast<long>(g.x0) * g.y1 - static_cast<long>(g.x1) * g.y0 +
													 static_cast<long>(g.x1) * g.y2 - static_cast<long>(g.x2) * g.y1 +
													 static_cast<long>(g.x2) * g.y3 - static_cast<long>(g.x3) * g.y2 +
													 static_cast<long>(g.x3) * g.y0 - static_cast<long>(g.x0) * g.y3;
				if (area2 <= 0)
					return;
			}

			// Gradient position is affine in local space, so compute its permille [0,1000]
			// at the 4 (untransformed) local rect corners; we then Gouraud-interpolate that
			// permille linearly across the PROJECTED quad in screen space — no per-pixel
			// perspective divide (the divide was ~85ms/frame for the 6 faces), exact scanline
			// clipping to the quad, and visually identical for the cube's mild per-face
			// perspective. Axis matches view.cpp recordLinearGradientBackground.
			const float angleRad = (static_cast<float>(g.angle) * 3.14159265358979f) / 1800.0f;
			const float gdx = std::sin(angleRad);
			const float gdy = -std::cos(angleRad);
			const float lx0 = static_cast<float>(g.lx), ly0 = static_cast<float>(g.ly);
			const float lx1 = static_cast<float>(g.lx + g.lw), ly1 = static_cast<float>(g.ly + g.lh);
			const float pc[4] = {lx0 * gdx + ly0 * gdy, lx1 * gdx + ly0 * gdy, lx1 * gdx + ly1 * gdy, lx0 * gdx + ly1 * gdy};
			float minP = pc[0], maxP = pc[0];
			for (int i = 1; i < 4; i++)
			{
				if (pc[i] < minP)
					minP = pc[i];
				if (pc[i] > maxP)
					maxP = pc[i];
			}
			const float span = maxP - minP;
			const float invSpan = span > 0.001f ? 1000.0f / span : 0.0f;

			// Per-permille color+alpha LUT (256 buckets), cached across calls (keyed on the
			// stops). The tiled replay path replays this command once per strip, so without
			// the cache each face's LUT (256 tgPremulColor /255 divides) would rebuild ~18x
			// per frame; the 8 slots hold every distinct face gradient in a scene.
			const int midStop = g.midStop;
			const int toStop = g.toStop > 0 ? g.toStop : 1000;
			const bool useMid = g.hasMid && midStop > 0 && midStop < toStop;
			// Per-bucket Bayer-dither LUT (251 buckets, indexed by permille>>2 — a shift,
			// no per-pixel `*255/1000` divide; the LX7 has no integer divide). Each entry
			// stores, per RGB565 channel, the quantize floor `q` and remainder `rem`, so the
			// per-pixel path adds one Bayer threshold compare and dithers — smoothing dark
			// transformed gradients (the perspective floor) that otherwise show hard RGB565
			// bands. Cached across calls (keyed on the stops); the 8 slots hold every
			// distinct face/floor gradient in a scene. ~14KB vs the old 24KB RGB565 LUT.
			// `color` is the pre-packed RGB565 (rounded) for the fast path used by
			// translucent gradients (the cube faces) which don't band; q/rem feed the Bayer
			// dither used only for opaque gradients (the floor), which do band when dark.
			struct FaceDitherStop
			{
				std::uint8_t qR, remR, qG, remG, qB, remB, alpha, a5;
			};
			struct FaceLutSlot
			{
				bool valid = false;
				std::uint16_t key[5] = {};
				std::uint8_t keyA[4] = {};
				FaceDitherStop e[251];
				// Hot-path SoA mirrors of e[]: the per-pixel SIMD gather reads ONLY the
				// pre-swapped normal-RGB565 color + the 0..32 alpha. Keeping them as flat
				// arrays kills the per-pixel struct-stride multiply AND the per-pixel
				// storage->normal byte swap (done once here, at LUT build) — both were
				// per-pixel costs in the dominant transformed-gradient fill.
				std::uint16_t colorRgb565[251];
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_A5_MIRROR
				std::uint8_t a5[251];
#endif
				// Storage-order colors for the opaque overwrite fast path (write-only,
				// no swap, no struct stride) and the scalar fallback paths.
				gea::framework::graphics::pixel::native_t colorNative[251];
			};
			// Lazily heap-allocated (once, thread-safe via the function-static guard)
			// so an app that never fills a transformed gradient reserves ZERO RAM for
			// this ~26 KB LUT bank scratch — it used to sit in .bss unconditionally for
			// every app. Allocated on the first transformed-gradient fill; never freed.
			using FaceLutBank = FaceLutSlot[kTransformedGradientLutSlots];
			static FaceLutBank *const slotsByBank = []() -> FaceLutBank *
			{
				constexpr std::size_t kLutBytes =
					sizeof(FaceLutSlot) * kTransformedGradientLutBanks * kTransformedGradientLutSlots;
				// Internal SRAM when the target has it: this table is gathered per pixel.
				if (void *fast = gea_render_fast_scratch(static_cast<int>(kLutBytes),
				                                        static_cast<int>(alignof(FaceLutSlot))))
				{
					std::memset(fast, 0, kLutBytes);  // FaceLutSlot is trivial; zero == !valid
					return static_cast<FaceLutBank *>(fast);
				}
				return new FaceLutSlot[kTransformedGradientLutBanks][kTransformedGradientLutSlots]();
			}();
			static int slotRRByBank[kTransformedGradientLutBanks] = {};
			// Flat-alpha flag per slot, kept OUTSIDE FaceLutSlot so the struct's hot LUT
			// arrays keep their cache layout (adding a field to the struct measurably
			// regressed the cache-sensitive gather). Computed once at build below.
			static std::uint8_t slotConstAlphaByBank[kTransformedGradientLutBanks][kTransformedGradientLutSlots] = {};
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_LOCK
			static volatile unsigned char slotLockByBank[kTransformedGradientLutBanks] = {};
#endif
			int lutBank = gea_current_render_core();
			if (lutBank < 0)
				lutBank = 0;
			lutBank %= kTransformedGradientLutBanks;
			auto &slots = slotsByBank[lutBank];
			int &slotRR = slotRRByBank[lutBank];
			std::uint8_t *const slotConstAlpha = slotConstAlphaByBank[lutBank];
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_LOCK
			volatile unsigned char &slotLock = slotLockByBank[lutBank];
			auto lockSlots = [&slotLock]()
			{
				while (__atomic_test_and_set(&slotLock, __ATOMIC_ACQUIRE))
				{
				}
			};
			auto unlockSlots = [&slotLock]()
			{
				__atomic_clear(&slotLock, __ATOMIC_RELEASE);
			};
			lockSlots();
#endif
			const std::uint16_t key[5] = {static_cast<std::uint16_t>(g.fromColor), static_cast<std::uint16_t>(g.midColor), static_cast<std::uint16_t>(g.toColor),
																		static_cast<std::uint16_t>(midStop), static_cast<std::uint16_t>(toStop)};
			const std::uint8_t keyA[4] = {g.fromAlpha, g.midAlpha, g.toAlpha, g.hasMid};
			FaceLutSlot *slot = nullptr;
			for (auto &s : slots)
				if (s.valid && std::memcmp(s.key, key, sizeof key) == 0 && std::memcmp(s.keyA, keyA, sizeof keyA) == 0)
				{
					slot = &s;
					break;
				}
			if (!slot)
			{
				slot = nullptr;
				for (auto &s : slots)
					if (!s.valid)
					{
						slot = &s;
						break;
					}
				if (!slot)
				{
					slot = &slots[slotRR];
					slotRR = (slotRR + 1) % kTransformedGradientLutSlots;
				}
				auto split = [](int value, int levels, std::uint8_t *q, std::uint8_t *rem)
				{
					if (value <= 0)
					{
						*q = 0;
						*rem = 0;
						return;
					}
					if (value >= 255)
					{
						*q = static_cast<std::uint8_t>(levels);
						*rem = 0;
						return;
					}
					const int scaled = value * levels;
					const int qq = scaled / 255;
					*q = static_cast<std::uint8_t>(qq);
					*rem = static_cast<std::uint8_t>(scaled - qq * 255);
				};
				for (int b = 0; b <= 250; b++)
				{
					const int permille = b * 4;
					int rr = 0, gg = 0, bb = 0;
					std::uint8_t al;
					if (useMid)
					{
						if (permille <= midStop)
						{
							const int t = tgStopRange(permille, 0, midStop);
							tgPremulRgb888(g.fromColor, g.fromAlpha, g.midColor, g.midAlpha, t, &rr, &gg, &bb);
							al = tgLerpAlpha(g.fromAlpha, g.midAlpha, t);
						}
						else
						{
							const int t = tgStopRange(permille, midStop, toStop);
							tgPremulRgb888(g.midColor, g.midAlpha, g.toColor, g.toAlpha, t, &rr, &gg, &bb);
							al = tgLerpAlpha(g.midAlpha, g.toAlpha, t);
						}
					}
					else
					{
						const int t = tgStopRange(permille, 0, toStop);
						tgPremulRgb888(g.fromColor, g.fromAlpha, g.toColor, g.toAlpha, t, &rr, &gg, &bb);
						al = tgLerpAlpha(g.fromAlpha, g.toAlpha, t);
					}
					split(rr, 31, &slot->e[b].qR, &slot->e[b].remR);
					split(gg, 63, &slot->e[b].qG, &slot->e[b].remG);
					split(bb, 31, &slot->e[b].qB, &slot->e[b].remB);
					const auto colorNative = gea::framework::graphics::pixel::nativeColor(rr, gg, bb);
					slot->e[b].alpha = al;
					// 0..32 alpha for the single-multiply parallel-channel blend fast path.
					slot->e[b].a5 = static_cast<std::uint8_t>((static_cast<int>(al) * 32 + 127) / 255);
					slot->colorRgb565[b] = gea::framework::graphics::pixel::toRgb565(colorNative);
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_A5_MIRROR
					slot->a5[b] = slot->e[b].a5;
#endif
					slot->colorNative[b] = colorNative;
				}
				// Flat-alpha detection (once per unique gradient): lets the hot gather skip
				// the per-pixel pieA5 store + a5 LUT load for flat-alpha gradients (the glassy
				// cube faces) — that load→store chain stalled the in-order LX7 fill.
				{
					bool flat = true;
					for (int b = 1; b <= 250; b++)
						if (slot->e[b].a5 != slot->e[0].a5)
						{
							flat = false;
							break;
						}
					slotConstAlpha[slot - slots] = flat ? 1 : 0;
				}
				std::memcpy(slot->key, key, sizeof key);
				std::memcpy(slot->keyA, keyA, sizeof keyA);
				slot->valid = true;
			}
			const bool slotIsConstAlpha = slotConstAlpha[slot - slots] != 0;
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_LOCK
			unlockSlots();
#endif

			const int parentAlpha = gea::platform::display::Display::alpha();
			if (parentAlpha == 0)
				return;
			// Per-node: a fully-opaque gradient (all stops + node opacity 255) is written
			// straight to the framebuffer via the scalar overwrite branch below (col =
			// colorNative: no destination read, no over-blend). Translucent faces (alpha < 255)
			// fall through to the SIMD blend exactly as before — both can coexist per node.
			const bool opaqueGrad = g.fromAlpha == 255 && g.toAlpha == 255 &&
															(!g.hasMid || g.midAlpha == 255) && parentAlpha == 255;
			// Keep the per-pixel Bayer dither only for opaque gradients with a DARK end —
			// those band in RGB565; bright faces don't, so they skip it and stay on the fast
			// overwrite. Endpoint brightness from the quantized LUT ends (endian-independent;
			// sum max 31+63+31 = 125).
			const int gradLo = slot->e[0].qR + slot->e[0].qG + slot->e[0].qB;
			const int gradHi = slot->e[250].qR + slot->e[250].qG + slot->e[250].qB;
			const bool ditherGrad = opaqueGrad && (gradLo < 24 || gradHi < 24);
			// Translucent faces (the common per-frame case): node opacity 255 → integer-DDA
			// + single-multiply parallel-channel SIMD blend fast path.
			const bool fastBlend = !opaqueGrad && parentAlpha == 255;
			const bool fastBlendUniformAlpha = fastBlend && g.fromAlpha == g.toAlpha &&
																				 (!g.hasMid || g.midAlpha == g.fromAlpha);
			const std::int16_t fastBlendUniformA5 =
					fastBlendUniformAlpha ? static_cast<std::int16_t>((static_cast<int>(g.fromAlpha) * 32 + 127) / 255) : 0;

			int clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			const int drawX0 = std::max(std::max(static_cast<int>(command.bx), 0), clipX0);
			const int drawY0 = std::max(std::max(static_cast<int>(command.by), 0), clipY0);
			const int drawX1 = std::min(std::min(static_cast<int>(command.bx + command.bw - 1), canvas->width() - 1), clipX1);
			const int drawY1 = std::min(std::min(static_cast<int>(command.by + command.bh - 1), canvas->height() - 1), clipY1);
			if (drawX0 > drawX1 || drawY0 > drawY1)
				return;

			// Screen-space quad vertices (TL,TR,BR,BL) and the gradient permille at each
			// (from its local corner). Scanline-rasterize the convex quad, interpolating
			// permille along the two crossed edges then across the span, blending each pixel
			// directly into the framebuffer (one loop; the dst reads are sequential so the
			// PSRAM cache prefetches the row).
			// Sub-pixel (1/8-px) screen corners: adjacent faces' shared 3D edge projects to
			// the same sub-pixel position from both faces, so the scanline rasterizer tiles
			// them watertight instead of cracking at the int16 round-to-nearest straddle.
			const float vx[4] = {g.fx0 * 0.125f, g.fx1 * 0.125f, g.fx2 * 0.125f, g.fx3 * 0.125f};
			const float vy[4] = {g.fy0 * 0.125f, g.fy1 * 0.125f, g.fy2 * 0.125f, g.fy3 * 0.125f};
			float vp[4];
			for (int i = 0; i < 4; i++)
			{
				int pm = static_cast<int>((pc[i] - minP) * invSpan + 0.5f);
				vp[i] = static_cast<float>(pm < 0 ? 0 : (pm > 1000 ? 1000 : pm));
			}
			static const int edges[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
			static const std::uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_EDGE_CACHE
			struct ScanEdge
			{
				float yMin = 0.0f;
				float yMax = 0.0f;
				float y0 = 0.0f;
				float x0 = 0.0f;
				float pm0 = 0.0f;
				float dxdy = 0.0f;
				float dpdy = 0.0f;
				bool active = false;
			};
			auto buildScanEdges = [](const float *xs, const float *ys, const float *pms, ScanEdge *out)
			{
				for (int e = 0; e < 4; e++)
				{
					const int a = edges[e][0], b = edges[e][1];
					const float ya = ys[a], yb = ys[b];
					const float dy = yb - ya;
					if (dy > -0.0001f && dy < 0.0001f)
					{
						out[e].active = false;
						continue;
					}
					const float invDy = 1.0f / dy;
					out[e].yMin = ya < yb ? ya : yb;
					out[e].yMax = ya < yb ? yb : ya;
					out[e].y0 = ya;
					out[e].x0 = xs[a];
					out[e].pm0 = pms ? pms[a] : 0.0f;
					out[e].dxdy = (xs[b] - xs[a]) * invDy;
					out[e].dpdy = pms ? (pms[b] - pms[a]) * invDy : 0.0f;
					out[e].active = true;
				}
			};
			ScanEdge outerEdges[4];
			buildScanEdges(vx, vy, vp, outerEdges);
#endif
			// Edge frame (the face's CSS border, folded into this command so it stays on
			// the reproject fast path). The border is the region between the projected
			// face quad and the projected image of the local rect inset by edgeWidth:
			// per scanline that is the outer span minus the inner-quad span, over-blended
			// AFTER the gradient fill (CSS background-clip: border-box — the background
			// extends under a translucent border). The inner corners are the bilinear
			// image of the inset local corners through the quad; that deviates from the
			// true projective image only to second order in edgeWidth/faceSize — far
			// sub-pixel here — and because it derives from the same (welded, reprojected)
			// sub-pixel corners as the fill, the frame always hugs the face it rides.
			int edgeA = g.edgeAlpha;
			if (parentAlpha != 255)
				edgeA = ((edgeA * parentAlpha + 128) * 257) >> 16;
			const bool drawEdge = g.edgeWidth > 0 && edgeA > 0;
			float evx[4], evy[4];
			if (drawEdge)
				{
					const float ku = static_cast<float>(g.edgeWidth) / static_cast<float>(g.lw);
					const float kv = static_cast<float>(g.edgeWidth) / static_cast<float>(g.lh);
					static const int hN[4] = {1, 0, 3, 2}; // horizontal local-space neighbor (TL↔TR, BL↔BR)
					static const int vN[4] = {3, 2, 1, 0}; // vertical local-space neighbor (TL↔BL, TR↔BR)
					for (int i = 0; i < 4; i++)
					{
						evx[i] = vx[i] + ku * (vx[hN[i]] - vx[i]) + kv * (vx[vN[i]] - vx[i]);
						evy[i] = vy[i] + ku * (vy[hN[i]] - vy[i]) + kv * (vy[vN[i]] - vy[i]);
					}
				}
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_EDGE_CACHE
			ScanEdge innerEdges[4];
			if (drawEdge)
				buildScanEdges(evx, evy, nullptr, innerEdges);
#endif
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
				const std::uint16_t edgeRgb565 = gea::framework::graphics::pixel::toRgb565(g.edgeColor);
				const std::uint32_t edgeF = (static_cast<std::uint32_t>(edgeRgb565) | (static_cast<std::uint32_t>(edgeRgb565) << 16)) & 0x07E0F81Fu;
			const std::uint32_t edgeA5 = (static_cast<std::uint32_t>(edgeA) * 32 + 127) / 255;
#endif
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			FillBandDirty dM{canvas->width(), canvas->height(), -1, -1};
			FillBandDirty dW{canvas->width(), canvas->height(), -1, -1};
			// Each scanline's span is computed independently from y (edge intersection), so
			// the rows split cleanly across the 2nd core: each band writes only its own
			// framebuffer rows + its own dirty bbox. Everything read below (vx/vy/vp, slot,
			// clip bounds, blend flags) is fixed above the fill, so the two bands run lock-free.
			const auto fillBand = [&](int bandY0, int bandY1, FillBandDirty &fd)
			{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
				const bool rgb565LutBlendAvail = kTransformedGradientA5Mirror && gea_rgb565_lut_blend_available();
				const bool rgb565LutBlendConstAlphaAvail = gea_rgb565_lut_blend_const_alpha_available();
#if GEA_EMBEDDED_PIE_BLEND_SPAN8_GATHER
				// SIMD-blend scratch for the aligned middle of each span: gathered LUT colors
				// (panel->normal) + per-pixel alpha. PER-CORE: with the 2-core coarse split the
				// worker fills the bottom band concurrently with this core's top band, so a
				// single shared (static) buffer would let the two gathers clobber each other
				// mid-span → wrong fg/alpha → horizontal banding. Indexed by render core id;
				// off-stack (static) to keep this deep call stack shallow.
				alignas(16) static std::uint16_t pieFgCore[2][512];
				alignas(16) static std::int16_t pieA5Core[2][512];
				const int pieCore = gea_current_render_core() & 1;
				std::uint16_t *const pieFg = pieFgCore[pieCore];
				std::int16_t *const pieA5 = pieA5Core[pieCore];
				const bool pieAvail = gea_pie_blend_available();
				// Constant-alpha gradient (e.g. the glassy cube faces: both stops α=0.5) →
				// the a5 LUT is flat (slot->constAlpha, computed once at build), so seed pieA5
				// once here and skip the per-pixel pieA5 store + a5 LUT load in the hot middle
				// gather. On the in-order LX7 that per-pixel load→store chain stalled the whole
				// fill: removing it lifts the glassy css-3d-cube fill with NO visual change
				// (pieA5 holds the identical flat value either way).
				const bool constA = pieAvail && slotIsConstAlpha;
				if (constA)
				{
					const std::int16_t av0 = static_cast<std::int16_t>(slot->a5[0]);
					for (int i = 0; i < 512; i++)
						pieA5[i] = av0;
				}
				// FUSED colour-DDA + blend path: for a 2-stop (linear colour), constant-alpha
				// gradient, the aligned in-range span can be blended in ONE SIMD pass with NO
				// pieFg gather (the proven bottleneck). Gated behind the A/B phase so it is
				// measured against the current gather+blend at identical board temperature.
				const int fusedA5 = constA ? static_cast<int>(slot->a5[0]) : 0;
				const bool fusedOk = constA && !g.hasMid && gea_pie_grad_blend_available() && gea_diag_ab_phase() != 0;
#endif
#endif
				for (int y = bandY0; y <= bandY1; y++)
				{
						const float yc = static_cast<float>(y) + 0.5f;
						float cx[2], cpm[2];
						int n = 0;
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_EDGE_CACHE
						for (int e = 0; e < 4 && n < 2; e++)
						{
							const ScanEdge &se = outerEdges[e];
							if (se.active && yc >= se.yMin && yc < se.yMax)
							{
								const float dy = yc - se.y0;
								cx[n] = se.x0 + dy * se.dxdy;
								cpm[n] = se.pm0 + dy * se.dpdy;
								n++;
							}
						}
#else
						for (int e = 0; e < 4 && n < 2; e++)
						{
							const int a = edges[e][0], b = edges[e][1];
							const float ya = vy[a], yb = vy[b];
							if ((yc >= ya && yc < yb) || (yc >= yb && yc < ya))
						{
							const float t = (yc - ya) / (yb - ya);
							cx[n] = vx[a] + t * (vx[b] - vx[a]);
							cpm[n] = vp[a] + t * (vp[b] - vp[a]);
								n++;
							}
						}
#endif
						if (n < 2)
							continue;
					int li = 0, ri = 1;
					if (cx[0] > cx[1])
					{
						li = 1;
						ri = 0;
					}
					const float xLf = cx[li], xRf = cx[ri];
					const float pmL = cpm[li], pmR = cpm[ri];
					int xL = static_cast<int>(std::ceil(xLf - 0.5f));
					int xR = static_cast<int>(std::floor(xRf - 0.5f));
					if (xL < drawX0)
						xL = drawX0;
					if (xR > drawX1)
						xR = drawX1;
					if (xL > xR)
						continue;
					const float dpm = (xRf - xLf > 0.001f) ? (pmR - pmL) / (xRf - xLf) : 0.0f;
					float pmf = pmL + (static_cast<float>(xL) + 0.5f - xLf) * dpm;
					gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
					// The whole span [xL,xR] is covered; extend the dirty bbox once per scanline.
					if (xL < fd.x0)
						fd.x0 = xL;
					if (xR > fd.x1)
						fd.x1 = xR;
					if (y < fd.y0)
						fd.y0 = y;
					if (y > fd.y1)
						fd.y1 = y;
					if (fastBlend)
					{
						// Integer 16.16 DDA over the LUT bucket; over-blend each pixel. The
						// aligned middle of the span is offloaded to the SIMD kernel (8 px/op,
						// same round-to-nearest); the head/tail (and short or no-SIMD spans) take
						// the scalar parallel-channel path. Endpoints pre-clamped so bucket∈[0,250].
						std::int32_t bucketFx = static_cast<std::int32_t>(pmf * (65536.0f / 4.0f) + 0.5f);
						const std::int32_t dBucketFx = static_cast<std::int32_t>(dpm * (65536.0f / 4.0f));
						auto clampBucket = [](std::int32_t bfx)
						{
							int bk = bfx >> 16;
							return bk < 0 ? 0 : (bk > 250 ? 250 : bk);
						};
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
						if (rgb565LutBlendConstAlphaAvail && fastBlendUniformAlpha)
						{
							gea_rgb565_lut_blend_const_alpha_span(&row[xL], slot->colorRgb565, fastBlendUniformA5, bucketFx, dBucketFx, xR - xL + 1);
						}
						else if (rgb565LutBlendAvail)
						{
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_A5_MIRROR
							gea_rgb565_lut_blend_span(&row[xL], slot->colorRgb565, slot->a5, bucketFx, dBucketFx, xR - xL + 1);
#endif
						}
						else
						{
							// Scalar over-blend of one pixel. The parallel-channel mask needs NORMAL
							// RGB565 bit layout, so convert LUT color + framebuffer from their storage
							// order (panel-endian on device, native-endian in the WASM sim) via
							// pixel::toRgb565, blend (round-to-nearest), then store back via
							// pixel::fromRgb565. NOTE: these were once an unconditional byte-swap, which
							// is correct only when storage IS panel order — on the native-endian sim it
							// blended byte-swapped (scrambled) channels, turning the gradient faces into
							// rainbow noise. On device (PANEL_ENDIAN=1) to/fromRgb565 are the same swap.
							auto scalarPx = [&](int x, int bucket)
							{
								const std::uint32_t a5 = static_cast<std::uint32_t>(slot->e[bucket].a5);
								if (a5 == 0)
									return;
								const std::uint16_t fc = slot->colorRgb565[bucket];
								const std::uint32_t f = (static_cast<std::uint32_t>(fc) | (static_cast<std::uint32_t>(fc) << 16)) & 0x07E0F81Fu;
								const std::uint16_t dpx = gea::framework::graphics::pixel::toRgb565(row[x]);
								const std::uint32_t b = (static_cast<std::uint32_t>(dpx) | (static_cast<std::uint32_t>(dpx) << 16)) & 0x07E0F81Fu;
								const std::uint32_t o = ((f * a5 + b * (32u - a5) + 0x02008010u) >> 5) & 0x07E0F81Fu;
								const std::uint16_t rn = static_cast<std::uint16_t>(o | (o >> 16));
								// fromRgb565, NOT toNative: the read above used toRgb565 (panel ->
								// normal), and fromRgb565 is its endian inverse. toNative is the
								// FORMAT converter (RGB565 -> native pixel) and is the identity on an
								// RGB565 target, so it applies no swap -- pairing it with toRgb565
								// wrote every blended pixel byte-swapped. Invisible on host (both are
								// the identity when PANEL_ENDIAN is 0) and invisible in the bulk of a
								// face on device, because the aligned middle of each span goes through
								// the PIE kernel; what showed was this path's span heads/tails -- the
								// cube's silhouette -- and thin geometry that never reaches the aligned
								// path at all, as magenta/orange fringes.
								row[x] = gea::framework::graphics::pixel::fromRgb565(rn);
							};
							int x = xL;
#if GEA_EMBEDDED_PIE_BLEND_SPAN8_GATHER
							// The PIE kernel uses 16-byte-aligned vector load/store, so &row[aStart]
							// must be 16-byte (8-px) aligned at its ABSOLUTE framebuffer address — not
							// just at a row-local multiple of 8. The framebuffer stride is packed
							// (410 px = 820 B, 820 % 16 == 4), so 3 of every 4 scanlines start at a
							// non-16-aligned byte. Aligning aStart only row-locally left &row[aStart]
							// misaligned on those rows; the aligned vld/vst then masked the low 4 bits
							// and read/wrote the wrong 16-byte block, producing a ~8-px comb along
							// edges. Derive the bounds from the absolute byte address instead.
							const std::uintptr_t rowAddr = reinterpret_cast<std::uintptr_t>(row);
							const std::uintptr_t aStartAddr =
									(rowAddr + static_cast<std::uintptr_t>(xL) * sizeof(std::uint16_t) + 15u) & ~static_cast<std::uintptr_t>(15u);
							const std::uintptr_t aEndAddr =
									(rowAddr + static_cast<std::uintptr_t>(xR + 1) * sizeof(std::uint16_t)) & ~static_cast<std::uintptr_t>(15u);
							const int aStart = static_cast<int>((aStartAddr - rowAddr) / sizeof(std::uint16_t)); // &row[aStart] is 16-byte aligned
							const int aEnd = static_cast<int>((aEndAddr - rowAddr) / sizeof(std::uint16_t));		 // (aEnd-aStart) is a multiple of 8
							if (pieAvail && aEnd > aStart)
							{
								for (; x < aStart; x++, bucketFx += dBucketFx)
									scalarPx(x, clampBucket(bucketFx));
								// FUSED fast path: 2-stop, constant-alpha gradient with the whole aligned
								// span in-range → compute colour inline + over-blend straight into the
								// framebuffer in ONE SIMD pass (no pieFg gather). Endpoints from the LUT,
								// linear in RGB565 between (≤1 LSB of the LUT, sub-perceptual).
								const int fusedRunLen = aEnd - aStart;
								const std::int64_t fusedEndFx =
										static_cast<std::int64_t>(bucketFx) + static_cast<std::int64_t>(fusedRunLen - 1) * dBucketFx;
								const int fusedBkS = bucketFx >> 16;
								const int fusedBkE = static_cast<int>(fusedEndFx >> 16);
								if (fusedOk && fusedRunLen >= 8 && fusedBkS >= 0 && fusedBkS <= 250 && fusedBkE >= 0 && fusedBkE <= 250)
								{
									const std::uint16_t cS = slot->colorRgb565[fusedBkS];
									const std::uint16_t cE = slot->colorRgb565[fusedBkE];
									const int rS = (cS >> 11) & 0x1F, gS = (cS >> 5) & 0x3F, bS = cS & 0x1F;
									const int rE = (cE >> 11) & 0x1F, gE = (cE >> 5) & 0x3F, bE = cE & 0x1F;
									const int denom = fusedRunLen - 1;
									gea_pie_grad_blend_span8(&row[aStart], fusedA5,
																					 rS << 8, ((rE - rS) << 8) / denom,
																					 gS << 8, ((gE - gS) << 8) / denom,
																					 bS << 8, ((bE - bS) << 8) / denom, fusedRunLen / 8);
									bucketFx = static_cast<std::int32_t>(
											static_cast<std::int64_t>(bucketFx) + static_cast<std::int64_t>(fusedRunLen) * dBucketFx);
									x = aEnd;
								}
								else
								{
								// Clamp splitting: the bucket DDA is monotonic across the run, so solve
								// ONCE for the segment where it lies inside [0,250] — the out-of-range
								// head/tail become CONSTANT endpoint fills (no LUT walk) and the middle
								// gathers unclamped (no per-pixel compares). One soft-divide per scanline
								// (the LX7 has no hardware divide) replaces two clamps per pixel.
								{
									const int runLen = aEnd - aStart;
									int linStart = aStart, linEnd = aEnd;
									if (dBucketFx == 0)
									{
										linEnd = linStart; // whole run constant at the (clamped) start bucket
									}
									else
									{
										const std::int64_t fx0 = bucketFx;
										const std::int64_t step = dBucketFx;
										const std::int64_t hiFx = 251LL << 16; // first fx past bucket 250
										auto ceilDiv = [](std::int64_t a, std::int64_t b) { return (a + b - 1) / b; };
										std::int64_t firstIn = 0, count = runLen;
										if (step > 0)
										{
											if (fx0 < 0)
												firstIn = ceilDiv(-fx0, step);
											if (fx0 > hiFx - 1)
												count = 0;
											else
												count = (hiFx - 1 - fx0) / step + 1;
										}
										else
										{
											if (fx0 >= hiFx)
												firstIn = ceilDiv(fx0 - (hiFx - 1), -step);
											if (fx0 < 0)
												count = 0;
											else
												count = fx0 / (-step) + 1;
										}
										if (firstIn > runLen)
											firstIn = runLen;
										if (count > runLen)
											count = runLen;
										linStart = aStart + static_cast<int>(firstIn);
										linEnd = aStart + static_cast<int>(count);
										if (linEnd < linStart)
											linEnd = linStart;
									}
									if (linStart > aStart)
									{
										// constant head: the clamped endpoint bucket at the run start
										const int bk = clampBucket(bucketFx);
										const std::uint16_t fc = slot->colorRgb565[bk];
									const std::int16_t av = static_cast<std::int16_t>(slot->a5[bk]);
										for (int gx = aStart; gx < linStart; gx++)
										{
											pieFg[gx - aStart] = fc;
											pieA5[gx - aStart] = av;
										}
									}
									{
										// unclamped middle: fx is proven inside [0, 251<<16)
										std::int32_t fx = static_cast<std::int32_t>(
												static_cast<std::int64_t>(bucketFx) + static_cast<std::int64_t>(linStart - aStart) * dBucketFx);
										if (constA)
										{
											// pieA5 is the flat seeded value — gather colour only (one store/px).
											for (int gx = linStart; gx < linEnd; gx++, fx += dBucketFx)
												pieFg[gx - aStart] = slot->colorRgb565[fx >> 16];
										}
										else
										{
											for (int gx = linStart; gx < linEnd; gx++, fx += dBucketFx)
											{
												const int bk = fx >> 16;
												pieFg[gx - aStart] = slot->colorRgb565[bk]; // pre-swapped at LUT build
												pieA5[gx - aStart] = static_cast<std::int16_t>(slot->a5[bk]);
											}
										}
									}
									if (linEnd < aEnd)
									{
										// constant tail: the clamped endpoint bucket at the run end
										const int bk = clampBucket(static_cast<std::int32_t>(
												static_cast<std::int64_t>(bucketFx) + static_cast<std::int64_t>(runLen - 1) * dBucketFx));
										const std::uint16_t fc = slot->colorRgb565[bk];
									const std::int16_t av = static_cast<std::int16_t>(slot->a5[bk]);
										for (int gx = linEnd; gx < aEnd; gx++)
										{
											pieFg[gx - aStart] = fc;
											pieA5[gx - aStart] = av;
										}
									}
									// advance the DDA past the whole gathered run (bit-identical, mod 2^32,
									// to the per-pixel adds the loop used to make)
									bucketFx = static_cast<std::int32_t>(
											static_cast<std::int64_t>(bucketFx) + static_cast<std::int64_t>(runLen) * dBucketFx);
								}
								gea_pie_blend_span8(&row[aStart], pieFg, pieA5, (aEnd - aStart) / 8);
								x = aEnd;
							}
							}
#endif
							for (; x <= xR; x++, bucketFx += dBucketFx)
								scalarPx(x, clampBucket(bucketFx));
						}
#else
						// Portable native over-blend (no RGB565 SIMD / dithering). This path is
						// only reached on full-colour targets, where 3D gradient faces normally
						// render through the native view tree rather than the framebuffer.
						for (int x = xL; x <= xR; x++, bucketFx += dBucketFx)
						{
							const int bucket = clampBucket(bucketFx);
							const FaceDitherStop &de = slot->e[bucket];
							if (de.a5 == 0)
								continue;
							const int a = static_cast<int>(de.a5) * 255 / 32;
							row[x] = gea::framework::graphics::pixel::blendNative(slot->colorNative[bucket], row[x], a);
						}
#endif
					}
					else if (opaqueGrad && !ditherGrad)
					{
						// Opaque non-dithered gradient (e.g. the tap-toggled solid cube faces):
						// pure overwrite — integer 16.16 DDA over the flat native-order LUT. (SIMD
						// vectorization of this loop was measured net-negative: the per-scanline
						// kernel setup + PIE critical section exceed the scalar per-pixel savings on
						// these short spans — same result as the translucent gather/fused attempts.)
						std::int32_t bucketFx = static_cast<std::int32_t>(pmf * (65536.0f / 4.0f) + 0.5f);
						const std::int32_t dBucketFx = static_cast<std::int32_t>(dpm * (65536.0f / 4.0f));
						for (int x = xL; x <= xR; x++, bucketFx += dBucketFx)
						{
							int bk = bucketFx >> 16;
							bk = bk < 0 ? 0 : (bk > 250 ? 250 : bk);
							row[x] = slot->colorNative[bk];
						}
					}
					else
					{
						const std::uint8_t *bRow = ditherGrad ? kBayer4[y & 3] : nullptr;
						for (int x = xL; x <= xR; x++, pmf += dpm)
						{
							int permille = static_cast<int>(pmf + 0.5f);
							if (permille < 0)
								permille = 0;
							else if (permille > 1000)
								permille = 1000;
							const FaceDitherStop &de = slot->e[permille >> 2];
							int a = de.alpha;
							if (parentAlpha != 255)
								a = ((a * parentAlpha + 128) * 257) >> 16;
							if (a <= 0)
								continue;
							gea::framework::graphics::pixel::native_t colN;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
							if (ditherGrad)
							{
								// Bayer-dither each RGB565 channel (q + 1 when the remainder beats the
								// ordered threshold) so dark opaque gradients don't band.
								const int threshold = bRow[x & 3] * 16 + 8;
								const int r5 = de.qR + (de.remR > threshold ? 1 : 0);
								const int g6 = de.qG + (de.remG > threshold ? 1 : 0);
								const int b5 = de.qB + (de.remB > threshold ? 1 : 0);
								colN = gea::framework::graphics::pixel::packRgb565Components(r5, g6, b5);
							}
							else
							{
								colN = slot->colorNative[permille >> 2];
							}
#else
							(void)bRow; // full colour: no dither (8888 doesn't band)
							colN = slot->colorNative[permille >> 2];
#endif
							row[x] = (a >= 255) ? colN : gea::framework::graphics::pixel::blendNative(colN, row[x], a);
						}
					}
					if (drawEdge)
					{
						// Inner-quad crossings on this scanline. No crossing (the rows between
						// the outer and inner top/bottom edges — where near-horizontal borders
						// live) → the whole span is border; otherwise border = the two runs
						// outside the inner span. Runs of zero length vanish via the loops.
						int bL1 = xR, bR0 = xR + 1; // defaults: one whole-span run, empty right run
						{
								float icx[2];
								int in = 0;
#if GEA_EMBEDDED_TRANSFORMED_GRADIENT_EDGE_CACHE
								for (int e = 0; e < 4 && in < 2; e++)
								{
									const ScanEdge &se = innerEdges[e];
									if (se.active && yc >= se.yMin && yc < se.yMax)
									{
										icx[in++] = se.x0 + (yc - se.y0) * se.dxdy;
									}
								}
#else
								for (int e = 0; e < 4 && in < 2; e++)
								{
									const int a = edges[e][0], b = edges[e][1];
									const float ya = evy[a], yb = evy[b];
									if ((yc >= ya && yc < yb) || (yc >= yb && yc < ya))
										icx[in++] = evx[a] + (yc - ya) / (yb - ya) * (evx[b] - evx[a]);
								}
#endif
								if (in == 2)
								{
								const float ixLf = icx[0] < icx[1] ? icx[0] : icx[1];
								const float ixRf = icx[0] < icx[1] ? icx[1] : icx[0];
								const int ixL = static_cast<int>(std::ceil(ixLf - 0.5f));
								const int ixR = static_cast<int>(std::floor(ixRf - 0.5f));
								if (ixL <= ixR)
								{
									bL1 = ixL - 1;
									bR0 = ixR + 1;
								}
							}
						}
						const int lA = xL, lB = bL1 < xR ? bL1 : xR;
						const int rA = bR0 > xL ? bR0 : xL, rB = xR;
						const auto edgeRun = [&](int x0, int x1)
						{
							if (x0 > x1)
								return;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
							if (edgeA >= 255)
							{
								for (int x = x0; x <= x1; x++)
									row[x] = g.edgeColor;
								return;
							}
							for (int x = x0; x <= x1; x++)
							{
								const std::uint16_t dpx = gea::framework::graphics::pixel::toRgb565(row[x]);
								const std::uint32_t b = (static_cast<std::uint32_t>(dpx) | (static_cast<std::uint32_t>(dpx) << 16)) & 0x07E0F81Fu;
								const std::uint32_t o = ((edgeF * edgeA5 + b * (32u - edgeA5) + 0x02008010u) >> 5) & 0x07E0F81Fu;
								// Endian inverse of the toRgb565 read above -- see scalarPx.
								row[x] = gea::framework::graphics::pixel::fromRgb565(static_cast<std::uint16_t>(o | (o >> 16)));
							}
#else
							for (int x = x0; x <= x1; x++)
								row[x] = (edgeA >= 255) ? g.edgeColor : gea::framework::graphics::pixel::blendNative(g.edgeColor, row[x], edgeA);
#endif
						};
						edgeRun(lA, lB);
						if (rA > lB) // guard the empty-inner overlap (whole span already painted above)
							edgeRun(rA, rB);
					}
				}
			};
			// Per-face 2nd-core split is net-negative: measured fps 31->23 because the
			// ~1ms/split dispatch overhead (startLat~500us + wait~500us) is paid 6-8x/frame
			// (once per face). The per-row fill itself parallelizes ~1.9x (19.7us/row split
			// vs 37.7us/row inline — CPU-bound, not PSRAM-bound), so the win is real but must
			// be captured with ONE coarse split per frame (whole-replay row-band split), not
			// per face. Fill inline here; the coarse split is driven a level up.
			fillBand(drawY0, drawY1, dM);
			const int dirtyX0 = std::min(dM.x0, dW.x0);
			const int dirtyY0 = std::min(dM.y0, dW.y0);
			const int dirtyX1 = std::max(dM.x1, dW.x1);
			const int dirtyY1 = std::max(dM.y1, dW.y1);
			if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
				canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
		}

		void drawFillQuad(const DisplayCommand &command)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return;

			const int xs[4] = {command.quad.x0, command.quad.x1, command.quad.x2, command.quad.x3};
			const int ys[4] = {command.quad.y0, command.quad.y1, command.quad.y2, command.quad.y3};
			int minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
			for (int i = 1; i < 4; i++)
			{
				if (xs[i] < minX)
					minX = xs[i];
				if (xs[i] > maxX)
					maxX = xs[i];
				if (ys[i] < minY)
					minY = ys[i];
				if (ys[i] > maxY)
					maxY = ys[i];
			}

			int clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			const int drawY0 = clampInt(minY, std::max(0, clipY0), std::min(canvas->height() - 1, clipY1));
			const int drawY1 = clampInt(maxY - 1, std::max(0, clipY0), std::min(canvas->height() - 1, clipY1));
			if (drawY0 > drawY1)
				return;

			const int alpha = gea::platform::display::Display::alpha();
			if (alpha == 0)
				return;
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			const int drawXMin = clampInt(minX - 1, std::max(0, clipX0), std::min(canvas->width() - 1, clipX1));
			const int drawXMax = clampInt(maxX, std::max(0, clipX0), std::min(canvas->width() - 1, clipX1));
			const int coverageArea = (drawXMax - drawXMin + 1) * (drawY1 - drawY0 + 1);
			const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
			const int aaSampleCount = aaSamples * aaSamples;
			if (aaSamples >= 2 && drawXMin <= drawXMax && coverageArea > 0 && coverageArea <= kAntialiasFullCoverageAreaLimit)
			{
				int dirtyX0 = canvas->width();
				int dirtyY0 = canvas->height();
				int dirtyX1 = -1;
				int dirtyY1 = -1;
				for (int y = drawY0; y <= drawY1; ++y)
				{
					gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
					for (int x = drawXMin; x <= drawXMax; ++x)
					{
						const int coverage = fillQuadCoverage(xs, ys, x, y, aaSamples);
						if (coverage <= 0)
							continue;
						paintCoveragePixel(row[x], command.quad.color, alpha, coverage, aaSampleCount);
						if (x < dirtyX0)
							dirtyX0 = x;
						if (x > dirtyX1)
							dirtyX1 = x;
						if (y < dirtyY0)
							dirtyY0 = y;
						if (y > dirtyY1)
							dirtyY1 = y;
					}
				}
				if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
					canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
				return;
			}

			if (aaSamples >= 2)
			{
				// Edge-only AA: solid interior fill + coverage AA on the 1-pixel boundary band
				// per scanline. Avoids the O(bbox) cost of full-coverage AA for large quads.
				int dirtyX0 = canvas->width();
				int dirtyY0 = canvas->height();
				int dirtyX1 = -1;
				int dirtyY1 = -1;
				const gea::framework::graphics::pixel::native_t quadColor = command.quad.color;
				const int canvasW = canvas->width();
				for (int y = drawY0; y <= drawY1; y++)
				{
					const float scanY = static_cast<float>(y) + 0.5f;
					float intersections[4];
					int count = 0;
					for (int i = 0; i < 4; i++)
					{
						const int j = (i + 1) & 3;
						if (ys[i] == ys[j])
							continue;
						const int lowY = std::min(ys[i], ys[j]);
						const int highY = std::max(ys[i], ys[j]);
						if (scanY < static_cast<float>(lowY) || scanY >= static_cast<float>(highY))
							continue;
						const float t = (scanY - static_cast<float>(ys[i])) / static_cast<float>(ys[j] - ys[i]);
						intersections[count++] = static_cast<float>(xs[i]) + static_cast<float>(xs[j] - xs[i]) * t;
					}
					if (count < 2)
						continue;
					for (int a = 1; a < count; a++)
					{
						const float key = intersections[a];
						int b = a - 1;
						while (b >= 0 && intersections[b] > key)
						{
							intersections[b + 1] = intersections[b];
							b--;
						}
						intersections[b + 1] = key;
					}

					const float xL = intersections[0];
					const float xR = intersections[count - 1];

					// Solid interior: pixels whose both subsamples (+0.25 and +0.75) are
					// strictly inside [xL, xR]. floor(xL)+1 guarantees x+0.25 > xL;
					// ceil(xR-0.75)-1 guarantees x+0.75 < xR.
					int solidX0 = static_cast<int>(std::floor(xL)) + 1;
					int solidX1 = static_cast<int>(std::ceil(xR - 0.75f)) - 1;
					solidX0 = std::max(solidX0, std::max(clipX0, 0));
					solidX1 = std::min(solidX1, std::min(clipX1, canvasW - 1));

					// Edge AA bands: pixels where at least one subsample may straddle an edge.
					// Left band: ceil(xL-0.75) … solidX0-1
					// Right band: solidX1+1 … floor(xR-0.25)
					const int aaLX0 = std::max(static_cast<int>(std::ceil(xL - 0.75f)), std::max(drawXMin, 0));
					const int aaLX1 = solidX0 - 1;
					const int aaRX0 = solidX1 + 1;
					const int aaRX1 = std::min(static_cast<int>(std::floor(xR - 0.25f)), std::min(drawXMax, canvasW - 1));

					gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;

					for (int x = aaLX0; x <= aaLX1; x++)
					{
						const int coverage = fillQuadCoverage(xs, ys, x, y, aaSamples);
						if (coverage > 0)
							paintCoveragePixel(row[x], quadColor, alpha, coverage, aaSampleCount);
					}

					if (solidX0 <= solidX1)
					{
						if (alpha >= 255)
						{
							for (int x = solidX0; x <= solidX1; x++)
								row[x] = quadColor;
						}
						else
						{
							for (int x = solidX0; x <= solidX1; x++)
								row[x] = gea::framework::graphics::pixel::blendNative(quadColor, row[x], alpha);
						}
					}

					for (int x = aaRX0; x <= aaRX1; x++)
					{
						const int coverage = fillQuadCoverage(xs, ys, x, y, aaSamples);
						if (coverage > 0)
							paintCoveragePixel(row[x], quadColor, alpha, coverage, aaSampleCount);
					}

					const int rowX0 = (aaLX0 <= aaLX1) ? aaLX0 : solidX0;
					const int rowX1 = (aaRX0 <= aaRX1) ? aaRX1 : solidX1;
					if (rowX0 <= rowX1)
					{
						if (rowX0 < dirtyX0) dirtyX0 = rowX0;
						if (rowX1 > dirtyX1) dirtyX1 = rowX1;
						if (y < dirtyY0) dirtyY0 = y;
						if (y > dirtyY1) dirtyY1 = y;
					}
				}
				if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
					canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
				return;
			}

			for (int y = drawY0; y <= drawY1; y++)
			{
				const float scanY = static_cast<float>(y) + 0.5f;
				float intersections[4];
				int count = 0;
				for (int i = 0; i < 4; i++)
				{
					const int j = (i + 1) & 3;
					if (ys[i] == ys[j])
						continue;
					const int lowY = std::min(ys[i], ys[j]);
					const int highY = std::max(ys[i], ys[j]);
					if (scanY < static_cast<float>(lowY) || scanY >= static_cast<float>(highY))
						continue;
					const float t = (scanY - static_cast<float>(ys[i])) / static_cast<float>(ys[j] - ys[i]);
					intersections[count++] = static_cast<float>(xs[i]) + static_cast<float>(xs[j] - xs[i]) * t;
				}
				if (count < 2)
					continue;
				// Insertion sort of <=4 edge intersections (float; hardware FPU).
				for (int a = 1; a < count; a++)
				{
					const float key = intersections[a];
					int b = a - 1;
					while (b >= 0 && intersections[b] > key)
					{
						intersections[b + 1] = intersections[b];
						b--;
					}
					intersections[b + 1] = key;
				}
				int drawX0 = static_cast<int>(std::ceil(intersections[0] - 0.5f));
				int drawX1 = static_cast<int>(std::floor(intersections[count - 1] - 0.5f));
				if (drawX0 < minX)
					drawX0 = minX;
				if (drawX1 > maxX - 1)
					drawX1 = maxX - 1;
				if (drawX0 < clipX0)
					drawX0 = clipX0;
				if (drawX1 > clipX1)
					drawX1 = clipX1;
				if (drawX0 < 0)
					drawX0 = 0;
				if (drawX1 >= canvas->width())
					drawX1 = canvas->width() - 1;
				if (drawX0 > drawX1)
					continue;

				gea::framework::graphics::pixel::native_t *dst = canvas->pixels() + canvas->rowToPhysical(y) * stride + drawX0;
				const gea::framework::graphics::pixel::native_t quadColor = command.quad.color;
				if (alpha == 255)
				{
					for (int x = drawX0; x <= drawX1; x++)
						*dst++ = quadColor;
				}
				else
				{
					for (int x = drawX0; x <= drawX1; x++)
					{
						*dst = gea::framework::graphics::pixel::blendNative(quadColor, *dst, alpha);
						dst++;
					}
				}
			}

			canvas->markDirty(std::max(0, std::max(minX, clipX0)),
												std::max(0, std::max(minY, clipY0)),
												std::min(canvas->width() - 1, std::min(maxX - 1, clipX1)),
												std::min(canvas->height() - 1, std::min(maxY - 1, clipY1)));
		}

		bool drawAffineTransformedRoundedRect(const DisplayCommand &command)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return true;
			const auto &r = command.transformedRoundedRect;
			if (!transformedRoundedRectLooksAffine(r))
				return false;

			const float invLocalW = r.lw != 0 ? (1.0f / static_cast<float>(r.lw)) : 0.0f;
			const float invLocalH = r.lh != 0 ? (1.0f / static_cast<float>(r.lh)) : 0.0f;
			const float a = static_cast<float>(r.x1 - r.x0) * invLocalW;
			const float b = static_cast<float>(r.x3 - r.x0) * invLocalH;
			const float c = static_cast<float>(r.y1 - r.y0) * invLocalW;
			const float d = static_cast<float>(r.y3 - r.y0) * invLocalH;
			const float det = a * d - b * c;
			if (std::fabs(det) < 1e-6f)
				return false;
			const float invDet = 1.0f / det;
			const float inv00 = d * invDet;
			const float inv01 = -b * invDet;
			const float inv10 = -c * invDet;
			const float inv11 = a * invDet;

			int clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			const int minClipX = std::max(0, clipX0);
			const int minClipY = std::max(0, clipY0);
			const int maxClipX = std::min(canvas->width() - 1, clipX1);
			const int maxClipY = std::min(canvas->height() - 1, clipY1);
			if (minClipX > maxClipX || minClipY > maxClipY)
				return true;
			const int drawX0 = clampInt(command.bx, minClipX, maxClipX);
			const int drawY0 = clampInt(command.by, minClipY, maxClipY);
			const int drawX1 = clampInt(command.bx + command.bw - 1, minClipX, maxClipX);
			const int drawY1 = clampInt(command.by + command.bh - 1, minClipY, maxClipY);
			if (drawX0 > drawX1 || drawY0 > drawY1)
				return true;

			const int alpha = gea::platform::display::Display::alpha();
			if (alpha == 0)
				return true;
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			int dirtyX0 = canvas->width();
			int dirtyY0 = canvas->height();
			int dirtyX1 = -1;
			int dirtyY1 = -1;
			const int coverageArea = (drawX1 - drawX0 + 1) * (drawY1 - drawY0 + 1);
			const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
			const bool useCoverageAntialias = aaSamples >= 2 && coverageArea > 0 && coverageArea <= kAntialiasFullCoverageAreaLimit;
			const int sampleCount = useCoverageAntialias ? aaSamples * aaSamples : 1;
			float sampleMarginX = 0.0f;
			float sampleMarginY = 0.0f;
			if (useCoverageAntialias)
			{
				const float kernelWidth = transformedRoundedRectAntialiasKernelWidth(aaSamples, r);
				const float minOffset = antialiasOffsetWithKernel(0, aaSamples, kernelWidth) - 0.5f;
				const float maxOffset = antialiasOffsetWithKernel(aaSamples - 1, aaSamples, kernelWidth) - 0.5f;
				const float offsetX[2] = {minOffset, maxOffset};
				const float offsetY[2] = {minOffset, maxOffset};
				for (float dx : offsetX)
				{
					for (float dy : offsetY)
					{
						sampleMarginX = std::max(sampleMarginX, std::fabs(inv00 * dx + inv01 * dy));
						sampleMarginY = std::max(sampleMarginY, std::fabs(inv10 * dx + inv11 * dy));
					}
				}
			}

			const float vx[4] = {
					static_cast<float>(r.x0),
					static_cast<float>(r.x1),
					static_cast<float>(r.x2),
					static_cast<float>(r.x3),
			};
			const float vy[4] = {
					static_cast<float>(r.y0),
					static_cast<float>(r.y1),
					static_cast<float>(r.y2),
					static_cast<float>(r.y3),
			};
			static const int quadEdges[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
			const int aaSpanPad = useCoverageAntialias ? 1 : 0;

			for (int y = drawY0; y <= drawY1; ++y)
			{
				int rowDrawX0 = drawX0;
				int rowDrawX1 = drawX1;
				float intersections[4];
				int intersectionCount = 0;
				const float scanY = static_cast<float>(y) + 0.5f;
				for (int e = 0; e < 4 && intersectionCount < 4; ++e)
				{
					const int ia = quadEdges[e][0];
					const int ib = quadEdges[e][1];
					const float ya = vy[ia];
					const float yb = vy[ib];
					if (ya == yb)
						continue;
					if ((scanY >= ya && scanY < yb) || (scanY >= yb && scanY < ya))
					{
						const float t = (scanY - ya) / (yb - ya);
						intersections[intersectionCount++] = vx[ia] + (vx[ib] - vx[ia]) * t;
					}
				}
				if (intersectionCount >= 2)
				{
					for (int i = 1; i < intersectionCount; ++i)
					{
						const float key = intersections[i];
						int j = i - 1;
						while (j >= 0 && intersections[j] > key)
						{
							intersections[j + 1] = intersections[j];
							--j;
						}
						intersections[j + 1] = key;
					}
					rowDrawX0 = static_cast<int>(std::ceil(intersections[0] - 0.5f)) - aaSpanPad;
					rowDrawX1 = static_cast<int>(std::floor(intersections[intersectionCount - 1] - 0.5f)) + aaSpanPad;
					if (rowDrawX0 < drawX0)
						rowDrawX0 = drawX0;
					if (rowDrawX1 > drawX1)
						rowDrawX1 = drawX1;
					if (rowDrawX0 > rowDrawX1)
						continue;
				}

				gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
				const float py = static_cast<float>(y) + 0.5f - static_cast<float>(r.y0);
				float localX = static_cast<float>(r.lx) + inv00 * (static_cast<float>(rowDrawX0) + 0.5f - static_cast<float>(r.x0)) + inv01 * py;
				float localY = static_cast<float>(r.ly) + inv10 * (static_cast<float>(rowDrawX0) + 0.5f - static_cast<float>(r.x0)) + inv11 * py;
				for (int x = rowDrawX0; x <= rowDrawX1; ++x)
				{
					const int coverage = useCoverageAntialias
																	 ? (roundedRectSolidCoreContainsFast(r, localX, localY, sampleMarginX, sampleMarginY)
																					? sampleCount
																					: roundedRectCoverageFast(r, localX, localY, inv00, inv10, inv01, inv11, aaSamples))
																	 : (roundedRectContainsFast(r, localX, localY) ? 1 : 0);
					if (coverage > 0)
					{
						paintCoveragePixel(row[x], r.color, alpha, coverage, sampleCount);
						if (x < dirtyX0)
							dirtyX0 = x;
						if (x > dirtyX1)
							dirtyX1 = x;
						if (y < dirtyY0)
							dirtyY0 = y;
						if (y > dirtyY1)
							dirtyY1 = y;
					}
					localX += inv00;
					localY += inv10;
				}
			}

			if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
				canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
			return true;
		}

		void cssRoundedBorderContours(const decltype(DisplayCommand{}.strokeRoundedRect) &s,
		                              TransformedRoundedRectCommand &outer,
		                              TransformedRoundedRectCommand &inner)
		{
			outer.lx = s.x; outer.ly = s.y; outer.lw = s.w; outer.lh = s.h;
			outer.tlRx8 = s.rx8[0]; outer.tlRy8 = s.ry8[0];
			outer.trRx8 = s.rx8[1]; outer.trRy8 = s.ry8[1];
			outer.brRx8 = s.rx8[2]; outer.brRy8 = s.ry8[2];
			outer.blRx8 = s.rx8[3]; outer.blRy8 = s.ry8[3];
			inner = outer;
			inner.lx += s.lineWidth; inner.ly += s.lineWidth;
			inner.lw = std::max(0, s.w - 2 * s.lineWidth);
			inner.lh = std::max(0, s.h - 2 * s.lineWidth);
			inner.tlRx8 = std::max(0, s.rx8[0] - 8 * s.lineWidth); inner.tlRy8 = std::max(0, s.ry8[0] - 8 * s.lineWidth);
			inner.trRx8 = std::max(0, s.rx8[1] - 8 * s.lineWidth); inner.trRy8 = std::max(0, s.ry8[1] - 8 * s.lineWidth);
			inner.brRx8 = std::max(0, s.rx8[2] - 8 * s.lineWidth); inner.brRy8 = std::max(0, s.ry8[2] - 8 * s.lineWidth);
			inner.blRx8 = std::max(0, s.rx8[3] - 8 * s.lineWidth); inner.blRy8 = std::max(0, s.ry8[3] - 8 * s.lineWidth);
		}

		int cssRoundedBorderCoverage(const TransformedRoundedRectCommand &outer,
		                             const TransformedRoundedRectCommand &inner,
		                             int x, int y, int samples)
		{
			int coverage = 0;
			for (int iy = 0; iy < samples; ++iy) for (int ix = 0; ix < samples; ++ix) {
				const float px = x + (ix + 0.5f) / samples, py = y + (iy + 0.5f) / samples;
				coverage += roundedRectContainsFast(outer, px, py) && !roundedRectContainsFast(inner, px, py);
			}
			return coverage;
		}

		void drawCssRoundedBorder(const DisplayCommand &command)
		{
			const auto &s = command.strokeRoundedRect;
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || s.w <= 0 || s.h <= 0 || s.lineWidth <= 0) return;
			TransformedRoundedRectCommand outer{}, inner{};
			cssRoundedBorderContours(s, outer, inner);
			int x0, y0, x1, y1;
			gea::platform::display::Display::clip(&x0, &y0, &x1, &y1);
			x0 = std::max({x0, 0, int(s.x)}); y0 = std::max({y0, 0, int(s.y)});
			x1 = std::min({x1, canvas->width() - 1, s.x + s.w - 1});
			y1 = std::min({y1, canvas->height() - 1, s.y + s.h - 1});
			if (x1 < x0 || y1 < y0) return;
			const int samples = std::max(1, gea::framework::graphics::Canvas::antialiasSamples());
			const int alpha = gea::platform::display::Display::alpha();
			if (!alpha) return;
			const int stride = canvas->strideBytes() / sizeof(gea::framework::graphics::pixel::native_t);
			for (int y = y0; y <= y1; ++y) {
				auto *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
				for (int x = x0; x <= x1; ++x) {
					const int coverage = cssRoundedBorderCoverage(outer, inner, x, y, samples);
					if (coverage) paintCoveragePixel(row[x], s.color, alpha, coverage, samples * samples);
				}
			}
			canvas->markDirty(x0, y0, x1, y1);
		}

		void drawTransformedRoundedRect(const DisplayCommand &command)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return;
			const auto &r = command.transformedRoundedRect;
			if (r.lw <= 0 || r.lh <= 0)
				return;

			if (r.backfaceHidden)
			{
				const long area2 = static_cast<long>(r.x0) * r.y1 - static_cast<long>(r.x1) * r.y0 +
													 static_cast<long>(r.x1) * r.y2 - static_cast<long>(r.x2) * r.y1 +
													 static_cast<long>(r.x2) * r.y3 - static_cast<long>(r.x3) * r.y2 +
													 static_cast<long>(r.x3) * r.y0 - static_cast<long>(r.x0) * r.y3;
				if (area2 <= 0)
					return;
			}

			if (drawAxisAlignedTransformedRoundedRect(command))
				return;
			if (drawAffineTransformedRoundedRect(command))
				return;

			const int16_t xs[4] = {r.x0, r.x1, r.x2, r.x3};
			const int16_t ys[4] = {r.y0, r.y1, r.y2, r.y3};
			double homography[8]{};
			if (!screenToLocalRectHomography(xs, ys, r.lx, r.ly, r.lw, r.lh, homography))
				return;

			int clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
			gea::platform::display::Display::clip(&clipX0, &clipY0, &clipX1, &clipY1);
			const int drawX0 = clampInt(command.bx, std::max(0, clipX0), std::min(canvas->width() - 1, clipX1));
			const int drawY0 = clampInt(command.by, std::max(0, clipY0), std::min(canvas->height() - 1, clipY1));
			const int drawX1 = clampInt(command.bx + command.bw - 1, std::max(0, clipX0), std::min(canvas->width() - 1, clipX1));
			const int drawY1 = clampInt(command.by + command.bh - 1, std::max(0, clipY0), std::min(canvas->height() - 1, clipY1));
			if (drawX0 > drawX1 || drawY0 > drawY1)
				return;

			const int alpha = gea::platform::display::Display::alpha();
			if (alpha == 0)
				return;
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			int dirtyX0 = canvas->width();
			int dirtyY0 = canvas->height();
			int dirtyX1 = -1;
			int dirtyY1 = -1;
			const int coverageArea = (drawX1 - drawX0 + 1) * (drawY1 - drawY0 + 1);
			const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
			const bool useCoverageAntialias = aaSamples >= 2 && coverageArea > 0 && coverageArea <= kAntialiasFullCoverageAreaLimit;
			const int sampleCount = useCoverageAntialias ? aaSamples * aaSamples : 1;

			for (int y = drawY0; y <= drawY1; ++y)
			{
				gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
				for (int x = drawX0; x <= drawX1; ++x)
				{
					int coverage = 0;
					if (useCoverageAntialias)
					{
						coverage = roundedRectCoverageHomography(r, homography, x, y, aaSamples);
					}
					else
					{
						double sx = 0.0;
						double sy = 0.0;
						if (!mapHomography(homography, static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, &sx, &sy))
							continue;
						coverage = roundedRectContains(r.lx,
																					 r.ly,
																					 r.lw,
																					 r.lh,
																					 r.tlRx8,
																					 r.tlRy8,
																					 r.trRx8,
																					 r.trRy8,
																					 r.brRx8,
																					 r.brRy8,
																					 r.blRx8,
																					 r.blRy8,
																					 sx,
																					 sy)
													 ? 1
													 : 0;
					}
					if (coverage <= 0)
						continue;
					paintCoveragePixel(row[x], r.color, alpha, coverage, sampleCount);
					if (x < dirtyX0)
						dirtyX0 = x;
					if (x > dirtyX1)
						dirtyX1 = x;
					if (y < dirtyY0)
						dirtyY0 = y;
					if (y > dirtyY1)
						dirtyY1 = y;
				}
			}

			if (dirtyX0 <= dirtyX1 && dirtyY0 <= dirtyY1)
				canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
		}

		bool isNativeTextInputView(const Node &node)
		{
#if GEA_EMBEDDED_ENABLE_NATIVE_TEXT_INPUT
			return node.type == NodeType::View && std::strcmp(tagFromId(node.tag_id), "input") == 0;
#else
			(void)node;
			return false;
#endif
		}

		bool hasRetainedTransformState(const Node &node)
		{
			const RareStyle &rs = rstyle(node.style); // one pool lookup, not 11
			const bool currentTransformable = ViewRenderer::isTransformableBox(node);
			return (currentTransformable && hasIndividualLinearTransform(rs)) ||
			       (node.render.previous_transformable_box && hadIndividualLinearTransform(node.render)) ||
			       (currentTransformable && (rs.transform_rotate != 0 || rs.transform_rotate_x != 0 ||
			        rs.transform_rotate_y != 0 || composedTranslateX(rs) != 0 || composedTranslateY(rs) != 0 ||
			        composedTranslateZ(rs) != 0 || composedTranslateXPercent(rs) != 0 ||
			        composedTranslateYPercent(rs) != 0 || rs.transform_scale_x != 1000 ||
			        rs.transform_scale_y != 1000 || rs.transform_scale_z != 1000 || rs.perspective > 0)) ||
			       (node.render.previous_transformable_box && (node.render.previous_transform_rotate != 0 ||
			        node.render.previous_transform_rotate_x != 0 || node.render.previous_transform_rotate_y != 0 ||
			        node.render.previous_transform_translate_x != 0 || node.render.previous_transform_translate_y != 0 ||
			        node.render.previous_transform_translate_z != 0 || node.render.previous_transform_translate_x_percent != 0 ||
			        node.render.previous_transform_translate_y_percent != 0 || node.render.previous_transform_scale_x != 1000 ||
			        node.render.previous_transform_scale_y != 1000 || node.render.previous_transform_scale_z != 1000 || node.render.previous_perspective > 0));
		}

		struct ClipMath
		{
			static int rectsOverlap(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1)
			{
				return ax0 <= bx1 && ay0 <= by1 && ax1 >= bx0 && ay1 >= by0;
			}

			static int nodeOverlapsClip(const Node &node, int cx0, int cy0, int cx1, int cy1)
			{
				int nx0, ny0, nx1, ny1;
				ViewRenderer::transformedBounds(node, false, &nx0, &ny0, &nx1, &ny1);
				return rectsOverlap(nx0, ny0, nx1, ny1, cx0, cy0, cx1, cy1);
			}

			static void clampToNode(const Node &node, int *cx0, int *cy0, int *cx1, int *cy1)
			{
				int nx0, ny0, w, h;
				overflowClipBounds(node, nx0, ny0, w, h);
				int nx1 = nx0 + w - 1, ny1 = ny0 + h - 1;
				if (*cx0 < nx0)
					*cx0 = nx0;
				if (*cy0 < ny0)
					*cy0 = ny0;
				if (*cx1 > nx1)
					*cx1 = nx1;
				if (*cy1 > ny1)
					*cy1 = ny1;
			}
		};

		struct ChildZSorter
		{
			struct Key { int phase, level; };
			static bool effectGroup(const Node &node)
			{
				const auto &s = node.style;
				const auto &rs = rstyle(s);
				const bool transformable = ViewRenderer::isTransformableBox(node);
				return s.opacity < 255 || s.mask_right_fade_width > 0 || rs.filter_present || rs.filter_blur_radius ||
				    (transformable && (rs.translate_present || rs.transform_present || rs.rotate_present || rs.scale_present ||
				     rs.transform_preserve_3d || rs.perspective || rs.transform_rotate || rs.transform_rotate_x || rs.transform_rotate_y ||
				     composedTranslateX(rs) || composedTranslateY(rs) || composedTranslateZ(rs) ||
				     composedTranslateXPercent(rs) || composedTranslateYPercent(rs) ||
				     rs.transform_scale_x != 1000 || rs.transform_scale_y != 1000 || rs.transform_scale_z != 1000)) || (rs.containment & (4 | 16));
			}
			static Key key(int id)
			{
				const Node *nodes = Tree::instance().nodes();
				const Node &n = nodes[id];
				const auto &s = n.style;
				const bool positioned = s.position != 0;
				const bool item = n.parent >= 0 && !isOutOfFlowPosition(s.position) &&
				    (nodes[n.parent].style.display == kDisplayFlex || isDisplayGrid(nodes[n.parent].style));
				const int level = !s.z_index_auto && (positioned || item) ? s.z_index : 0;
				if (level < 0) return {0, level};
				if (level > 0) return {5, level};
				if (positioned || effectGroup(n) || (item && !s.z_index_auto)) return {4, 0};
				if (item) return {3, 0};
				if (s.float_side) return {2, 0};
				return {LayoutEngine::isInlineLevelNode(n) ? 3 : 1, 0};
			}

			static int compareKeys(int first, int second)
			{
				const auto a = key(first), b = key(second);
				return a.phase != b.phase ? a.phase - b.phase : a.level - b.level;
			}

			static bool participatesIn3DContext(int node, int contextRoot)
			{
				const Node *nodes = Tree::instance().nodes();
				if (contextRoot < 0 || !ViewRenderer::isTransformableBox(nodes[contextRoot]) || !preserves3D(nodes[contextRoot].style))
					return false;
				// A 3D rendering context follows DOM ancestry, not containing-block
				// ancestry. Every parent between a participant and the context root
				// must preserve 3D; a flat parent composites its descendants as a
				// single plane and prevents abs/fixed descendants from being hoisted
				// into this context.
				for (int parent = nodes[node].parent; parent >= 0 && parent != contextRoot; parent = nodes[parent].parent)
					if (!ViewRenderer::isTransformableBox(nodes[parent]) || !preserves3D(nodes[parent].style))
						return false;
				for (int parent = nodes[node].parent; parent >= 0; parent = nodes[parent].parent)
					if (parent == contextRoot)
						return true;
				return false;
			}

			static int depth(int node)
			{
				Node *nodes = Tree::instance().nodes();
				return ViewRenderer::transformedDepth(nodes[node], false);
			}

			static int subtreeDepth(int node, int contextRoot)
			{
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				if (node < 0 || node >= tree.nodeCount() || !participatesIn3DContext(node, contextRoot))
					return 0;
				// Memoize per node per refresh. The child z-sort runs at every tree level,
				// so an ancestor's sort recurses a subtree that a descendant's sort then
				// re-walks. Caching collapses those repeated O(subtree) walks to O(1).
				static int memo[kMaxNodes];
				static uint64_t memoSerial[kMaxNodes];
				static int memoContextRoot[kMaxNodes];
				const uint64_t serial = tree.refreshSerial();
				if (node < kMaxNodes && memoSerial[node] == serial && memoContextRoot[node] == contextRoot)
					return memo[node];
				int maxDepth = depth(node);
				for (int child = nodes[node].first_child; child >= 0 && child < tree.nodeCount(); child = nodes[child].next_sibling)
				{
					if (participatesIn3DContext(child, contextRoot))
						maxDepth = std::max(maxDepth, subtreeDepth(child, contextRoot));
				}
				if (node < kMaxNodes)
				{
					memo[node] = maxDepth;
					memoSerial[node] = serial;
					memoContextRoot[node] = contextRoot;
				}
				return maxDepth;
			}

			static bool afterByDepth(int existing, int existingDepth, int candidate, int candidateDepth)
			{
				const int order = compareKeys(existing, candidate);
				if (order) return order > 0;
				return existingDepth > candidateDepth;
			}

			static void sort(int *children, int child_count, int contextRoot)
			{
				if (child_count < 2)
					return;
				// Fast path: when document order already equals paint order, skip the
				// sort entirely when every sibling has the same paint phase and stack
				// level and no transform/3D depth is active — the common 2D case
				// (typography, breakout). subtreeDepth() recurses the whole subtree and
				// each node costs 5 double-precision projections, so this avoids it
				// entirely for 2D trees.
				const bool transforms = contextRoot >= 0 && ViewRenderer::isTransformableBox(Tree::instance().nodes()[contextRoot]) &&
				                        preserves3D(Tree::instance().nodes()[contextRoot].style) &&
				                        ViewRenderer::anyTransformActive();
				if (!transforms)
				{
					bool mixed = false;
					for (int i = 1; i < child_count; i++)
					{
						if (compareKeys(children[0], children[i]))
						{
							mixed = true;
							break;
						}
					}
					if (!mixed)
						return;
				}
				// Precompute each child's painter's-depth key ONCE. Recomputing
				// subtreeDepth() on every comparison made the insertion sort
				// O(k^2 * subtree) — catastrophic for animated transform trees (e.g.
				// css-animation-showcase: 21 children → ~440 comparisons, each doing a
				// recursive subtree walk of 5 projections/node). Computing it up front is
				// O(k * subtree) + O(k^2) integer comparisons. Depth only varies under an
				// active transform; without one it's constant, so skip the walk. A
				// context may include hoisted descendants beyond the immediate-child
				// scratch limit, so size this buffer to its actual participant count.
				std::vector<int> depthKey(child_count);
				for (int i = 0; i < child_count; i++)
					depthKey[i] = transforms ? subtreeDepth(children[i], contextRoot) : 0;
				for (int i = 1; i < child_count; i++)
				{
					const int childKey = children[i];
					const int childDepth = depthKey[i];
					int j = i - 1;
					while (j >= 0 && afterByDepth(children[j], depthKey[j], childKey, childDepth))
					{
						children[j + 1] = children[j];
						depthKey[j + 1] = depthKey[j];
						j--;
					}
					children[j + 1] = childKey;
					depthKey[j + 1] = childDepth;
				}
			}
		};

		struct DisplayCommandTranslator
		{
			static void translate(DisplayCommand *c, int dx, int dy)
			{
				if (!c || (dx == 0 && dy == 0))
					return;
				c->bx += dx;
				c->by += dy;
				switch (c->type)
				{
				case DisplayCommandType::PushClip: {
					const int id = c->clip.nodeId;
					const Node *owner = id >= 0 && id < Tree::instance().nodeCount() ? &Tree::instance().nodes()[id] : nullptr;
					if (!owner || overflowX(owner->style)) c->clip.x += dx;
					else c->bx -= dx;
					if (!owner || overflowY(owner->style)) c->clip.y += dy;
					else c->by -= dy;
					break;
				}
				case DisplayCommandType::FillRect:
					c->fill.x += dx;
					c->fill.y += dy;
					break;
				case DisplayCommandType::FillCircle:
					c->fillCircle.cx += dx;
					c->fillCircle.cy += dy;
					break;
				case DisplayCommandType::FillRoundedRect:
					c->fillRoundedRect.x += dx;
					c->fillRoundedRect.y += dy;
					break;
				case DisplayCommandType::FillQuad:
					if (c->quad.reprojectMode == 1 || c->quad.reprojectMode == 3)
					{
						c->quad.lx += dx;
						c->quad.ly += dy;
					}
					else if (c->quad.reprojectMode == 2)
					{
						const int16_t dx8 = static_cast<int16_t>(dx * 8);
						const int16_t dy8 = static_cast<int16_t>(dy * 8);
						c->quad.lx += dx8;
						c->quad.ly += dy8;
						c->quad.lw += dx8;
						c->quad.lh += dy8;
					}
					c->quad.x0 += dx;
					c->quad.y0 += dy;
					c->quad.x1 += dx;
					c->quad.y1 += dy;
					c->quad.x2 += dx;
					c->quad.y2 += dy;
					c->quad.x3 += dx;
					c->quad.y3 += dy;
					break;
				case DisplayCommandType::FillTransformedRoundedRect:
					c->transformedRoundedRect.lx += dx;
					c->transformedRoundedRect.ly += dy;
					c->transformedRoundedRect.x0 += dx;
					c->transformedRoundedRect.y0 += dy;
					c->transformedRoundedRect.x1 += dx;
					c->transformedRoundedRect.y1 += dy;
					c->transformedRoundedRect.x2 += dx;
					c->transformedRoundedRect.y2 += dy;
					c->transformedRoundedRect.x3 += dx;
					c->transformedRoundedRect.y3 += dy;
					break;
				case DisplayCommandType::FillLinearGradient:
					c->gradient.x += dx;
					c->gradient.y += dy;
					break;
				case DisplayCommandType::FillRadialGradient:
					c->radialGradient.x += dx;
					c->radialGradient.y += dy;
					break;
				case DisplayCommandType::DrawLine:
					c->line.x0 += dx;
					c->line.y0 += dy;
					c->line.x1 += dx;
					c->line.y1 += dy;
					break;
				case DisplayCommandType::StrokeRect:
					c->stroke.x += dx;
					c->stroke.y += dy;
					break;
				case DisplayCommandType::StrokeRoundedRect:
					c->strokeRoundedRect.x += dx;
					c->strokeRoundedRect.y += dy;
					break;
				case DisplayCommandType::DrawText:
					c->text.x += dx;
					c->text.y += dy;
					break;
				case DisplayCommandType::DrawProjectedText:
					c->projectedText.srcX += dx;
					c->projectedText.srcY += dy;
					c->projectedText.x0 += dx;
					c->projectedText.y0 += dy;
					c->projectedText.x1 += dx;
					c->projectedText.y1 += dy;
					c->projectedText.x2 += dx;
					c->projectedText.y2 += dy;
					c->projectedText.x3 += dx;
					c->projectedText.y3 += dy;
					break;
				case DisplayCommandType::BlitImage:
					c->blit.dx += dx;
					c->blit.dy += dy;
					break;
				case DisplayCommandType::BlitImageScaled:
					c->scaledBlit.dx += dx;
					c->scaledBlit.dy += dy;
					break;
				case DisplayCommandType::BeginFilterBlur:
				case DisplayCommandType::ApplyFilterBlur:
					break;
				default:
					break;
				}
			}
		};

		// Reusable static-gradient bitmap cache. A gradient whose parameters and box are
		// unchanged for 2 frames is rendered once into PSRAM color+alpha buffers and blitted
		// on later frames — no per-pixel recompute. Unlike the older opaque-only bgCache
		// (which memcpy-blits and therefore skips any translucent gradient), this always
		// stores per-pixel alpha and blits via blitImage, so it caches transparent layers
		// too — the weather shell's vignette + warm-glow gradients re-rendered every frame
		// precisely because they're translucent. Blit per row through blitImage (a raw
		// framebuffer memcpy diverges from the flush's dirty pattern and wedges the panel
		// TX). One buffer set per instance, so distinct gradients sharing one fall back to
		// the normal render. `span(y,&x0,&x1)` narrows a row to the rounded-corner span;
		// `pixel(x,y,&color,&alpha)` computes one pixel (used only on the one-time bake).
		struct StaticGradientCache
		{
			gea::framework::graphics::pixel::native_t *color = nullptr;
			std::uint8_t *alpha = nullptr;
			int cap = 0, kw = 0, kh = 0;
			std::uint16_t key[24] = {0};
			std::uint16_t lastKey[24] = {0};
			int keyLen = 0;
			int stable = 0;
			bool valid = false;

			template <typename SpanFn>
			bool blit(const std::uint16_t *keyNow, int kl, int x0, int y0, int x1, int y1, int scrW, int scrH, SpanFn span)
			{
				if (!valid || kw != scrW || kh != scrH || keyLen != kl ||
						std::memcmp(keyNow, key, static_cast<std::size_t>(kl) * sizeof(std::uint16_t)) != 0)
					return false;
				for (int y = y0; y <= y1; ++y)
				{
					int rowX0 = x0, rowX1 = x1;
					span(y, &rowX0, &rowX1);
					if (rowX0 > rowX1)
						continue;
					int spanW = rowX1 - rowX0 + 1;
					const long long off = static_cast<long long>(y) * scrW + rowX0;
					gea::platform::display::Display::blitImage(color + off, alpha + off, spanW, 1, rowX0, y);
				}
				return true;
			}

			template <typename SpanFn, typename PixelFn>
			void bake(const std::uint16_t *keyNow, int kl, int boxX, int boxY, int boxW, int boxH, int scrW, int scrH, SpanFn span, PixelFn pixel)
			{
				if (keyLen == kl && std::memcmp(keyNow, lastKey, static_cast<std::size_t>(kl) * sizeof(std::uint16_t)) == 0)
				{
					stable++;
				}
				else
				{
					std::memcpy(lastKey, keyNow, static_cast<std::size_t>(kl) * sizeof(std::uint16_t));
					keyLen = kl;
					stable = 1;
					valid = false;
				}
				if (stable < 2 || valid)
					return;
				const int need = scrW * scrH;
				if (cap < need)
				{
					if (color)
						gea::framework::memory::Allocator::free(color);
					if (alpha)
						gea::framework::memory::Allocator::free(alpha);
					color = static_cast<gea::framework::graphics::pixel::native_t *>(gea::framework::memory::Allocator::allocatePreferSpiram(static_cast<std::size_t>(need) * sizeof(gea::framework::graphics::pixel::native_t)));
					alpha = static_cast<std::uint8_t *>(gea::framework::memory::Allocator::allocatePreferSpiram(static_cast<std::size_t>(need)));
					cap = (color && alpha) ? need : 0;
				}
				if (cap < need)
					return;
				const int fx0 = std::max(0, boxX), fy0 = std::max(0, boxY);
				const int fx1 = std::min(scrW - 1, boxX + boxW - 1), fy1 = std::min(scrH - 1, boxY + boxH - 1);
				for (int y = fy0; y <= fy1; ++y)
				{
					int rowX0 = fx0, rowX1 = fx1;
					span(y, &rowX0, &rowX1);
					if (rowX0 > rowX1)
						continue;
					for (int x = rowX0; x <= rowX1; ++x)
					{
						const long long off = static_cast<long long>(y) * scrW + x;
						pixel(x, y, &color[off], &alpha[off]);
					}
				}
				std::memcpy(key, keyNow, static_cast<std::size_t>(kl) * sizeof(std::uint16_t));
				kw = scrW;
				kh = scrH;
				valid = true;
			}

			// Row-callback variant of bake(): identical stability/allocation logic,
			// but hands the filler whole rows so drawers can use their span fast
			// paths (e.g. the vertical-gradient constant-row fill).
			template <typename SpanFn, typename RowFn>
			void bakeRows(const std::uint16_t *keyNow, int kl, int boxX, int boxY, int boxW, int boxH, int scrW, int scrH, SpanFn span, RowFn rowFn)
			{
				if (keyLen == kl && std::memcmp(keyNow, lastKey, static_cast<std::size_t>(kl) * sizeof(std::uint16_t)) == 0)
				{
					stable++;
				}
				else
				{
					std::memcpy(lastKey, keyNow, static_cast<std::size_t>(kl) * sizeof(std::uint16_t));
					keyLen = kl;
					stable = 1;
					valid = false;
				}
				if (stable < 2 || valid)
					return;
				const int need = scrW * scrH;
				if (cap < need)
				{
					if (color)
						gea::framework::memory::Allocator::free(color);
					if (alpha)
						gea::framework::memory::Allocator::free(alpha);
					color = static_cast<gea::framework::graphics::pixel::native_t *>(gea::framework::memory::Allocator::allocatePreferSpiram(static_cast<std::size_t>(need) * sizeof(gea::framework::graphics::pixel::native_t)));
					alpha = static_cast<std::uint8_t *>(gea::framework::memory::Allocator::allocatePreferSpiram(static_cast<std::size_t>(need)));
					cap = (color && alpha) ? need : 0;
				}
				if (cap < need)
					return;
				const int fx0 = std::max(0, boxX), fy0 = std::max(0, boxY);
				const int fx1 = std::min(scrW - 1, boxX + boxW - 1), fy1 = std::min(scrH - 1, boxY + boxH - 1);
				for (int y = fy0; y <= fy1; ++y)
				{
					int rowX0 = fx0, rowX1 = fx1;
					span(y, &rowX0, &rowX1);
					if (rowX0 > rowX1)
						continue;
					const long long off = static_cast<long long>(y) * scrW + rowX0;
					rowFn(y, rowX0, rowX1, &color[off], &alpha[off]);
				}
				std::memcpy(key, keyNow, static_cast<std::size_t>(kl) * sizeof(std::uint16_t));
				kw = scrW;
				kh = scrH;
				valid = true;
			}
		};

		struct LinearGradientDrawer
		{
			static constexpr double kPi = 3.14159265358979323846;

			static int clampPermille(int value)
			{
				if (value < 0)
					return 0;
				if (value > 1000)
					return 1000;
				return value;
			}

			static int gradientToStop(const DisplayCommand &c)
			{
				return c.gradient.toStop > 0 ? c.gradient.toStop : 1000;
			}

			static int stopRangePermille(int permille, int start, int end)
			{
				if (permille <= start)
					return 0;
				if (permille >= end)
					return 1000;
				const int span = end - start;
				if (span <= 0)
					return 1000;
				return ((permille - start) * 1000 + span / 2) / span;
			}

			static int interpolate(int from, int to, int permille)
			{
				return (from * (1000 - permille) + to * permille + 500) / 1000;
			}

			static uint8_t interpolateAlpha(uint8_t from, uint8_t to, int permille)
			{
				return static_cast<uint8_t>(interpolate(from, to, permille));
			}

			static void interpolatePremultipliedRgb888(gea::framework::graphics::pixel::native_t fromColor,
																								 uint8_t fromAlpha,
																								 gea::framework::graphics::pixel::native_t toColor,
																								 uint8_t toAlpha,
																								 int permille,
																								 int *r,
																								 int *g,
																								 int *b)
			{
				int fr, fg, fb, tr, tg, tb;
				unpackRgb888(fromColor, &fr, &fg, &fb);
				unpackRgb888(toColor, &tr, &tg, &tb);
				const int fromWeight = static_cast<int>(fromAlpha) * (1000 - permille);
				const int toWeight = static_cast<int>(toAlpha) * permille;
				const int weight = fromWeight + toWeight;
				if (weight <= 0)
				{
					*r = tr;
					*g = tg;
					*b = tb;
					return;
				}
				*r = (fr * fromWeight + tr * toWeight + weight / 2) / weight;
				*g = (fg * fromWeight + tg * toWeight + weight / 2) / weight;
				*b = (fb * fromWeight + tb * toWeight + weight / 2) / weight;
			}

			static void unpackRgb888(gea::framework::graphics::pixel::native_t color, int *r, int *g, int *b)
			{
				// Native pixel -> 8-bit channels (5/6-bit expand on RGB565, direct on RGBA8888).
				int a = 0;
				gea::framework::graphics::pixel::unpackNative8(color, r, g, b, &a);
			}

			static uint16_t packDitheredRgb888(int r, int g, int b, int x, int y)
			{
				static constexpr uint8_t bayer4[4][4] = {
						{0, 8, 2, 10},
						{12, 4, 14, 6},
						{3, 11, 1, 9},
						{15, 7, 13, 5},
				};
				const int threshold = bayer4[y & 3][x & 3] * 16 + 8;
				auto quantize = [threshold](int value, int levels)
				{
					if (value <= 0)
						return 0;
					if (value >= 255)
						return levels;
					const int scaled = value * levels;
					const int q = scaled / 255;
					return q + ((scaled - q * 255) > threshold ? 1 : 0);
				};
				return gea::framework::graphics::pixel::packRgb565Components(
						quantize(r, 31),
						quantize(g, 63),
						quantize(b, 31));
			}

			// One per-permille LUT entry. Holds the dither quantize floor `q` and the
			// remainder `rem` for each RGB565 channel, so the per-pixel dither is a
			// single compare (`rem > threshold`) with no divide. Bit-identical to the
			// old packDitheredRgb888 quantize.
			struct DitherStop
			{
				uint8_t qR, remR, qG, remG, qB, remB, alpha;
				// Un-dithered native pixel at this gradient stop. RGB565 boards use the
				// qR/remR Bayer-dither fields (avoids banding); full-colour boards use this
				// directly (8888 doesn't band), so each board renders its right algorithm.
				gea::framework::graphics::pixel::native_t color;
			};

			// Un-dithered RGB888 color at a gradient position. Carries every integer
			// divide in the gradient (mid-stop remap, RGB565->888 unpack, channel
			// interpolation). The S3 has no fast integer divide, so calling this per
			// pixel was the dominant cost of a full-screen gradient (~15 divides x
			// 206k px ~= 840ms->457ms even after the float-projection fix). replay() now
			// calls it once per permille bucket (x1001) to build a LUT and applies the
			// (x,y)-dependent bayer dither per pixel instead.
			static void rgb888At(const DisplayCommand &c, int permille, int *r, int *g, int *b)
			{
				permille = clampPermille(permille);
				gea::framework::graphics::pixel::native_t from = c.gradient.fromColor;
				gea::framework::graphics::pixel::native_t to = c.gradient.toColor;
				uint8_t fromAlpha = c.gradient.fromAlpha;
				uint8_t toAlpha = c.gradient.toAlpha;
				int localPermille = permille;
				const int toStop = gradientToStop(c);
				if (c.gradient.hasMid && c.gradient.midStop > 0 && c.gradient.midStop < toStop)
				{
					if (permille <= c.gradient.midStop)
					{
						to = c.gradient.midColor;
						toAlpha = c.gradient.midAlpha;
						localPermille = stopRangePermille(permille, 0, c.gradient.midStop);
					}
					else
					{
						from = c.gradient.midColor;
						fromAlpha = c.gradient.midAlpha;
						localPermille = stopRangePermille(permille, c.gradient.midStop, toStop);
					}
				}
				else
				{
					localPermille = stopRangePermille(permille, 0, toStop);
				}
				interpolatePremultipliedRgb888(from, fromAlpha, to, toAlpha, localPermille, r, g, b);
			}

			static uint8_t alphaAt(const DisplayCommand &c, int permille)
			{
				permille = clampPermille(permille);
				uint8_t from = c.gradient.fromAlpha;
				uint8_t to = c.gradient.toAlpha;
				int localPermille = permille;
				const int toStop = gradientToStop(c);
				if (c.gradient.hasMid && c.gradient.midStop > 0 && c.gradient.midStop < toStop)
				{
					if (permille <= c.gradient.midStop)
					{
						to = c.gradient.midAlpha;
						localPermille = stopRangePermille(permille, 0, c.gradient.midStop);
					}
					else
					{
						from = c.gradient.midAlpha;
						localPermille = stopRangePermille(permille, c.gradient.midStop, toStop);
					}
				}
				else
				{
					localPermille = stopRangePermille(permille, 0, toStop);
				}
				return interpolateAlpha(from, to, localPermille);
			}

			static uint8_t combineAlpha(uint8_t parentAlpha, uint8_t localAlpha)
			{
				return static_cast<uint8_t>((static_cast<int>(parentAlpha) * static_cast<int>(localAlpha) + 127) / 255);
			}

			static int integerSqrt(int n)
			{
				if (n <= 0)
					return 0;
				int x = n;
				int y = (x + 1) / 2;
				while (y < x)
				{
					x = y;
					y = (x + n / x) / 2;
				}
				return x;
			}

			static int normalizedRadius(int radius, int maxRadius)
			{
				if (radius < 0)
					return 0;
				return radius > maxRadius ? maxRadius : radius;
			}

			static void roundedRowSpan(const DisplayCommand &c, int y, int *x0, int *x1)
			{
				const int maxRadius = std::min<int>(c.gradient.w / 2, c.gradient.h / 2);
				const int tl = normalizedRadius(c.gradient.tl, maxRadius);
				const int tr = normalizedRadius(c.gradient.tr, maxRadius);
				const int br = normalizedRadius(c.gradient.br, maxRadius);
				const int bl = normalizedRadius(c.gradient.bl, maxRadius);
				if ((tl | tr | br | bl) == 0)
					return;

				const int left = c.gradient.x;
				const int right = c.gradient.x + c.gradient.w - 1;
				const int top = c.gradient.y;
				const int bottom = c.gradient.y + c.gradient.h - 1;
				int rowLeft = left;
				int rowRight = right;

				if (tl > 0 && y >= top && y <= top + tl)
				{
					const int dy = top + tl - y;
					const int dx = integerSqrt(tl * tl - dy * dy);
					rowLeft = std::max(rowLeft, left + tl - dx);
				}
				if (bl > 0 && y >= bottom - bl && y <= bottom)
				{
					const int dy = y - (bottom - bl);
					const int dx = integerSqrt(bl * bl - dy * dy);
					rowLeft = std::max(rowLeft, left + bl - dx);
				}
				if (tr > 0 && y >= top && y <= top + tr)
				{
					const int dy = top + tr - y;
					const int dx = integerSqrt(tr * tr - dy * dy);
					rowRight = std::min(rowRight, right - tr + dx);
				}
				if (br > 0 && y >= bottom - br && y <= bottom)
				{
					const int dy = y - (bottom - br);
					const int dx = integerSqrt(br * br - dy * dy);
					rowRight = std::min(rowRight, right - br + dx);
				}

				*x0 = std::max(*x0, rowLeft);
				*x1 = std::min(*x1, rowRight);
			}

			static void fillRun(int x, int y, int w, uint16_t color, uint8_t alpha, uint8_t baseAlpha)
			{
				const uint8_t effectiveAlpha = combineAlpha(baseAlpha, alpha);
				if (effectiveAlpha == 0 || w <= 0)
					return;
				if (effectiveAlpha != baseAlpha)
					gea::platform::display::Display::setAlpha(effectiveAlpha);
				gea::platform::display::Display::fillRect(x, y, w, 1, color);
				if (effectiveAlpha != baseAlpha)
					gea::platform::display::Display::setAlpha(baseAlpha);
			}

			static void replay(const DisplayCommand &c)
			{
				int cx0, cy0, cx1, cy1;
				gea::platform::display::Display::clip(&cx0, &cy0, &cx1, &cy1);
				const int x0 = std::max<int>(c.gradient.x, cx0);
				const int y0 = std::max<int>(c.gradient.y, cy0);
				const int x1 = std::min<int>(c.gradient.x + c.gradient.w - 1, cx1);
				const int y1 = std::min<int>(c.gradient.y + c.gradient.h - 1, cy1);
				if (x1 < x0 || y1 < y0)
					return;

				// Static-background gradient cache: a large opaque gradient (the app backdrop)
				// is re-rasterized over the moving content's dirty region every frame (~23ms
				// on css-3d-cube). Render it once into a full-screen buffer, then per frame
				// blit the dirty sub-region from the cache. The blit uses the SAME per-row
				// blitImage() path as the normal render below — identical canvas dirty-state
				// and framebuffer writes, so the panel flush sees exactly what it sees for a
				// normal bg render — it only skips the per-pixel gradient recompute by reading
				// pre-rasterized rows from the cache. (An earlier version raw-memcpy'd straight
				// into the framebuffer with one coarse markDirty(); that diverged from the
				// flush's expected per-row dirty pattern and hung the panel TX. Going through
				// blitImage() keeps the cache provably equivalent to the stable path.) Gated on
				// opaque + large + stable so animated/small/alpha gradients render normally.
				auto *bgCanvas = gea::platform::display::Display::canvas();
				int bgCap = 0;
				gea::framework::graphics::pixel::native_t *bgBuf = gea_bg_cache(&bgCap);
				const int scrW = bgCanvas ? bgCanvas->width() : 0;
				const int scrH = bgCanvas ? bgCanvas->height() : 0;
				const bool bgOpaque = c.gradient.fromAlpha == 255 && c.gradient.toAlpha == 255 &&
															(!c.gradient.hasMid || c.gradient.midAlpha == 255) &&
															gea::platform::display::Display::alpha() == 255 &&
															c.gradient.tl == 0 && c.gradient.tr == 0 && c.gradient.br == 0 && c.gradient.bl == 0;
				const bool bgLarge = static_cast<long long>(c.gradient.w) * c.gradient.h * 2 >=
														 static_cast<long long>(scrW) * scrH;
				const bool bgCacheable = bgBuf && bgCanvas && bgCanvas->pixels() && bgOpaque && bgLarge &&
																 scrW > 0 && scrH > 0 && bgCap >= scrW * scrH;
				const std::uint16_t bgKeyNow[14] = {
						static_cast<std::uint16_t>(c.gradient.fromColor), static_cast<std::uint16_t>(c.gradient.midColor), static_cast<std::uint16_t>(c.gradient.toColor), c.gradient.midStop,
						c.gradient.toStop, static_cast<std::uint16_t>(c.gradient.angle), c.gradient.fromAlpha,
						c.gradient.midAlpha, c.gradient.toAlpha, c.gradient.hasMid,
						static_cast<std::uint16_t>(c.gradient.x), static_cast<std::uint16_t>(c.gradient.y),
						static_cast<std::uint16_t>(c.gradient.w), static_cast<std::uint16_t>(c.gradient.h)};
				static std::uint16_t bgKey[14];
				static std::uint16_t bgLastKey[14];
				static int bgStable = 0;
				static bool bgValid = false;
				static int bgKW = 0, bgKH = 0;
				if (bgCacheable && bgValid && bgKW == scrW && bgKH == scrH &&
						std::memcmp(bgKeyNow, bgKey, sizeof bgKeyNow) == 0)
				{
					for (int y = y0; y <= y1; y++)
					{
						int rowX0 = x0, rowX1 = x1;
						roundedRowSpan(c, y, &rowX0, &rowX1);
						if (rowX0 > rowX1)
							continue;
						int spanW = rowX1 - rowX0 + 1;
						gea::platform::display::Display::blitImage(
								bgBuf + static_cast<long long>(y) * scrW + rowX0, nullptr, spanW, 1, rowX0, y);
					}
					return;
				}

				// Translucent static gradients (e.g. the shell's transparent->dark vignette)
				// can't use the opaque bgCache above (it memcpy-blits), so they re-rendered
				// every frame. Cache them via the alpha-capable StaticGradientCache. Gate on
				// ACTUAL alpha transparency, not !bgOpaque: a rounded-corner opaque gradient
				// (e.g. the shell's per-theme base layer) also fails bgOpaque, but it changes
				// every theme — letting it share this single-slot cache would thrash the
				// vignette's entry (alternating keys never stabilize). Requiring real
				// transparency keeps the dynamic base out, so the static vignette caches alone.
				const bool hasTransparency = c.gradient.fromAlpha < 255 || c.gradient.toAlpha < 255 ||
																		 (c.gradient.hasMid && c.gradient.midAlpha < 255);
				const bool transCacheable = bgCanvas && bgCanvas->pixels() && scrW > 0 && scrH > 0 && hasTransparency &&
																		static_cast<long long>(c.gradient.w) * c.gradient.h * 16 >= static_cast<long long>(scrW) * scrH;
				const std::uint16_t transKey[14] = {
						static_cast<std::uint16_t>(c.gradient.fromColor), static_cast<std::uint16_t>(c.gradient.midColor), static_cast<std::uint16_t>(c.gradient.toColor), c.gradient.midStop, c.gradient.toStop,
						static_cast<std::uint16_t>(c.gradient.angle), c.gradient.fromAlpha, c.gradient.midAlpha, c.gradient.toAlpha,
						c.gradient.hasMid, static_cast<std::uint16_t>(c.gradient.x), static_cast<std::uint16_t>(c.gradient.y),
						static_cast<std::uint16_t>(c.gradient.w), static_cast<std::uint16_t>(c.gradient.h)};
				static StaticGradientCache transCache;
				auto transSpan = [&](int y, int *a, int *b)
				{ roundedRowSpan(c, y, a, b); };
				if (transCacheable && transCache.blit(transKey, 14, x0, y0, x1, y1, scrW, scrH, transSpan))
					return;

				const double angleRadians = (static_cast<double>(c.gradient.angle) * kPi) / 1800.0;
				double dx = std::sin(angleRadians);
				double dy = -std::cos(angleRadians);
				// Snap axis-aligned angles to exact unit vectors: sin(pi) is 1.2e-16,
				// not 0, so a plain `to bottom` gradient would miss the constant-row
				// fast path below (and drag ~1e-16 noise through the projection).
				switch (c.gradient.angle % 3600)
				{
				case 0: dx = 0.0; dy = -1.0; break;
				case 900: dx = 1.0; dy = 0.0; break;
				case 1800: dx = 0.0; dy = 1.0; break;
				case 2700: dx = -1.0; dy = 0.0; break;
				default: break;
				}
				const double gx0 = static_cast<double>(c.gradient.x);
				const double gy0 = static_cast<double>(c.gradient.y);
				const double gx1 = static_cast<double>(c.gradient.x + c.gradient.w);
				const double gy1 = static_cast<double>(c.gradient.y + c.gradient.h);
				const double p0 = gx0 * dx + gy0 * dy;
				const double p1 = gx1 * dx + gy0 * dy;
				const double p2 = gx1 * dx + gy1 * dy;
				const double p3 = gx0 * dx + gy1 * dy;
				const double minProjection = std::min(std::min(p0, p1), std::min(p2, p3));
				const double maxProjection = std::max(std::max(p0, p1), std::max(p2, p3));
				const double projectionSpan = maxProjection - minProjection;
				if (projectionSpan <= 0.001)
				{
					const uint8_t baseAlpha = gea::platform::display::Display::alpha();
					for (int y = y0; y <= y1; y++)
					{
						int rowX0 = x0;
						int rowX1 = x1;
						roundedRowSpan(c, y, &rowX0, &rowX1);
						if (rowX0 <= rowX1)
							fillRun(rowX0, y, rowX1 - rowX0 + 1, c.gradient.fromColor, c.gradient.fromAlpha, baseAlpha);
					}
					return;
				}

				// Per-permille color LUT. colorAt/alphaAt are pure functions of `permille`
				// (1001 buckets); only the bayer dither depends on (x,y). Precompute the
				// dither quantize floor+remainder per channel per bucket so the per-pixel
				// work drops from ~15 integer divides to a table index + 3 compares. The
				// S3 has no fast integer divide, so this was the gradient's dominant cost.
				// Output is bit-identical to the old per-pixel colorAt/packDitheredRgb888.
				// Cache the LUT across calls: it depends only on the color stops (not
				// position/angle), so a static gradient rebuilds it once instead of every
				// frame — and the tiled replay path, which replays this command once per
				// strip, would otherwise rebuild all 1001 entries (each with /255 divides)
				// ~18x per frame.
				// Slot count is a target knob: a frame drawing N distinct gradients
				// through a tiled/chunked replay (fused replay+flush, SRAM strip
				// staging) rebuilds a single-slot LUT N times PER TILE — weather's 4
				// distinct gradients across ~65 flush chunks cost ~260ms/frame on the
				// rp2350 purely in rebuilds. Slots are ~10KB each; targets with the
				// RAM opt into more (rp2350: 6), everyone else keeps today's single
				// slot and footprint.
#ifndef GEA_EMBEDDED_GRADIENT_LUT_SLOTS
#define GEA_EMBEDDED_GRADIENT_LUT_SLOTS 1
#endif
// GEA_EMBEDDED_LAZY_GRADIENT_LUTS=1 allocates the dither tables on first use
// instead of as static arrays: the slots above plus the radial table below
// otherwise sit in .bss whether or not the app ever draws a gradient.
#ifndef GEA_EMBEDDED_LAZY_GRADIENT_LUTS
#define GEA_EMBEDDED_LAZY_GRADIENT_LUTS 0
#endif
				struct LutSlot
				{
					DitherStop lut[1001];
					std::uint16_t key[5];
					std::uint8_t keyA[4];
					std::uint32_t lastUse;
				};
#if GEA_EMBEDDED_LAZY_GRADIENT_LUTS
				static auto lutStorage = std::make_unique<std::array<LutSlot, GEA_EMBEDDED_GRADIENT_LUT_SLOTS>>();
				auto &lutSlots = *lutStorage;
#else
				static LutSlot lutSlots[GEA_EMBEDDED_GRADIENT_LUT_SLOTS] = {};
#endif
				static bool lutInit = false;
				static std::uint32_t lutUseCounter = 0;
				if (!lutInit)
				{
					for (auto &slot : lutSlots)
					{
						slot.key[0] = 0xFFFF;
						slot.keyA[0] = 0xFF;
						slot.keyA[3] = 0xFF;
					}
					lutInit = true;
				}
				LutSlot *lutSlot = nullptr;
				for (auto &slot : lutSlots)
				{
					if (slot.key[0] == c.gradient.fromColor && slot.key[1] == c.gradient.midColor &&
							slot.key[2] == c.gradient.toColor && slot.key[3] == c.gradient.midStop &&
							slot.key[4] == c.gradient.toStop && slot.keyA[0] == c.gradient.fromAlpha &&
							slot.keyA[1] == c.gradient.midAlpha && slot.keyA[2] == c.gradient.toAlpha &&
							slot.keyA[3] == c.gradient.hasMid)
					{
						lutSlot = &slot;
						break;
					}
				}
				const bool lutMatch = lutSlot != nullptr;
				if (!lutSlot)
				{
					lutSlot = &lutSlots[0];
					for (auto &slot : lutSlots)
					{
						if (slot.lastUse < lutSlot->lastUse) lutSlot = &slot;
					}
				}
				lutSlot->lastUse = ++lutUseCounter;
				DitherStop *lut = lutSlot->lut;
				std::uint16_t *lutKey = lutSlot->key;
				std::uint8_t *lutKeyA = lutSlot->keyA;
				if (!lutMatch)
				{
					for (int p = 0; p <= 1000; p++)
					{
						int r, g, b;
						rgb888At(c, p, &r, &g, &b);
						auto split = [](int value, int levels, uint8_t *q, uint8_t *rem)
						{
							if (value <= 0)
							{
								*q = 0;
								*rem = 0;
								return;
							}
							if (value >= 255)
							{
								*q = static_cast<uint8_t>(levels);
								*rem = 0;
								return;
							}
							const int scaled = value * levels;
							const int qq = scaled / 255;
							*q = static_cast<uint8_t>(qq);
							*rem = static_cast<uint8_t>(scaled - qq * 255);
						};
						split(r, 31, &lut[p].qR, &lut[p].remR);
						split(g, 63, &lut[p].qG, &lut[p].remG);
						split(b, 31, &lut[p].qB, &lut[p].remB);
						lut[p].color = gea::framework::graphics::pixel::packNative8(r, g, b);
						lut[p].alpha = alphaAt(c, p);
					}
					lutKey[0] = c.gradient.fromColor;
					lutKey[1] = c.gradient.midColor;
					lutKey[2] = c.gradient.toColor;
					lutKey[3] = c.gradient.midStop;
					lutKey[4] = c.gradient.toStop;
					lutKeyA[0] = c.gradient.fromAlpha;
					lutKeyA[1] = c.gradient.midAlpha;
					lutKeyA[2] = c.gradient.toAlpha;
					lutKeyA[3] = c.gradient.hasMid;
				}
				static constexpr uint8_t bayer4[4][4] = {
						{0, 8, 2, 10},
						{12, 4, 14, 6},
						{3, 11, 1, 9},
						{15, 7, 13, 5},
				};

				// Per-pixel projection on the hardware FPU. The gradient axis is linear in
				// x, so step `projection` by `stepX` each pixel (no per-pixel multiply) and
				// scale by a precomputed reciprocal (no per-pixel divide).
				//
				// Build each scanline into a contiguous RGB565 buffer and blit the whole
				// row in one call rather than coalescing into per-run fillRect()s. Bayer
				// dither makes adjacent pixels differ, so run-coalescing shattered every
				// row into ~1px runs, and each fillRect(w=1) paid a clip pass, a per-pixel
				// framebuffer read-back, and a markDirty()+addDirtyRect() list scan (~86k
				// calls/frame on a full-screen gradient). blitImage() clips once, memcpy's
				// the row, and markDirty's once — this per-call overhead, not the color
				// math, was the gradient's real cost (~840ms -> ~370ms from killing the
				// divides barely moved it; the row blit is the actual lever).
				const float stepX = static_cast<float>(dx);
				const float stepY = static_cast<float>(dy);
				const float fMinProjection = static_cast<float>(minProjection);
				const float invSpanPermille = 1000.0f / static_cast<float>(projectionSpan);
				static gea::framework::graphics::pixel::native_t rowColor[gea::platform::display::kWidth];
				static uint8_t rowAlpha[gea::platform::display::kWidth];
				for (int y = y0; y <= y1; y++)
				{
					int rowX0 = x0;
					int rowX1 = x1;
					roundedRowSpan(c, y, &rowX0, &rowX1);
					if (rowX0 > rowX1)
						continue;
					const uint8_t *bayerRow = bayer4[y & 3];
					// Keep the device-sized buffer, but cover wider simulator viewports in chunks.
					for (int chunkX = rowX0; chunkX <= rowX1; chunkX += gea::platform::display::kWidth)
					{
						const int spanW = std::min(gea::platform::display::kWidth, rowX1 - chunkX + 1);
						float projection = (static_cast<float>(chunkX) + 0.5f) * stepX + (static_cast<float>(y) + 0.5f) * stepY;
						bool rowOpaque = true;
						if (stepX == 0.0f)
						{
							// Vertical gradient (the common `to bottom` case): the projection —
							// hence the permille bucket, LUT entry, and alpha — is constant
							// across the row; only the 4-periodic bayer COLUMN pattern varies.
							// Compute the 4-pixel pattern once with exactly the per-pixel math
							// (bit-identical) and fill the row at store speed instead of paying
							// the DDA + clamp + LUT + dither chain per pixel.
							const int permille = clampPermille(static_cast<int>((projection - fMinProjection) * invSpanPermille + 0.5f));
							const DitherStop &e = lut[permille];
							gea::framework::graphics::pixel::native_t pattern[4];
							for (int k = 0; k < 4; k++)
							{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
								const int threshold = bayerRow[k] * 16 + 8;
								const int r5 = e.qR + ((e.remR > threshold) ? 1 : 0);
								const int g6 = e.qG + ((e.remG > threshold) ? 1 : 0);
								const int b5 = e.qB + ((e.remB > threshold) ? 1 : 0);
								pattern[k] = gea::framework::graphics::pixel::packRgb565Components(r5, g6, b5);
#else
								pattern[k] = e.color;
#endif
							}
							for (int i = 0; i < spanW; i++)
								rowColor[i] = pattern[(chunkX + i) & 3];
							std::memset(rowAlpha, e.alpha, static_cast<std::size_t>(spanW));
							rowOpaque = e.alpha == 255;
						}
						else
						for (int i = 0; i < spanW; i++, projection += stepX)
						{
							const int permille = clampPermille(static_cast<int>((projection - fMinProjection) * invSpanPermille + 0.5f));
							const DitherStop &e = lut[permille];
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
							const int threshold = bayerRow[(chunkX + i) & 3] * 16 + 8;
							const int r5 = e.qR + ((e.remR > threshold) ? 1 : 0);
							const int g6 = e.qG + ((e.remG > threshold) ? 1 : 0);
							const int b5 = e.qB + ((e.remB > threshold) ? 1 : 0);
							rowColor[i] = gea::framework::graphics::pixel::packRgb565Components(r5, g6, b5);
#else
							rowColor[i] = e.color; // full colour: no dither (8888 doesn't band)
#endif
							rowAlpha[i] = e.alpha;
							if (e.alpha != 255)
								rowOpaque = false;
						}
						// alpha=nullptr -> blitImage's opaque memcpy (baseAlpha==255) or whole-row
						// global blend (baseAlpha<255); the rowAlpha mask -> per-pixel combine
						// with baseAlpha. Equivalent to the old fillRun(combineAlpha(baseAlpha,
						// localAlpha)). baseAlpha is read from Display::alpha() inside blitImage.
						gea::platform::display::Display::blitImage(rowColor, rowOpaque ? nullptr : rowAlpha, spanW, 1, chunkX, y);
					}
				}

				// Once the gradient has been identical for 2 frames, render it in full into
				// the cache buffer (reusing the LUT/projection just computed); subsequent
				// frames hit the blit fast-path above. One-time ~full-screen render; writes
				// only into bgBuf (no framebuffer/dirty side effects).
				if (bgCacheable)
				{
					if (std::memcmp(bgKeyNow, bgLastKey, sizeof bgKeyNow) == 0)
					{
						bgStable++;
					}
					else
					{
						std::memcpy(bgLastKey, bgKeyNow, sizeof bgKeyNow);
						bgStable = 1;
						bgValid = false;
					}
					if (bgStable >= 2 && !bgValid)
					{
						const int fy0 = std::max(0, static_cast<int>(c.gradient.y));
						const int fy1 = std::min(scrH - 1, static_cast<int>(c.gradient.y + c.gradient.h - 1));
						const int fx0 = std::max(0, static_cast<int>(c.gradient.x));
						const int fx1 = std::min(scrW - 1, static_cast<int>(c.gradient.x + c.gradient.w - 1));
						for (int y = fy0; y <= fy1; y++)
						{
							const uint8_t *bayerRow = bayer4[y & 3];
							float projection = (static_cast<float>(fx0) + 0.5f) * stepX + (static_cast<float>(y) + 0.5f) * stepY;
							gea::framework::graphics::pixel::native_t *brow = bgBuf + static_cast<long long>(y) * scrW;
							if (stepX == 0.0f)
							{
								// Vertical gradient: constant permille across the row — fill the
								// 4-periodic bayer pattern at store speed (see the row loop above).
								const int permille = clampPermille(static_cast<int>((projection - fMinProjection) * invSpanPermille + 0.5f));
								const DitherStop &e = lut[permille];
								gea::framework::graphics::pixel::native_t pattern[4];
								for (int k = 0; k < 4; k++)
								{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
									const int threshold = bayerRow[k] * 16 + 8;
									pattern[k] = gea::framework::graphics::pixel::packRgb565Components(
											e.qR + ((e.remR > threshold) ? 1 : 0), e.qG + ((e.remG > threshold) ? 1 : 0),
											e.qB + ((e.remB > threshold) ? 1 : 0));
#else
									pattern[k] = e.color;
#endif
								}
								for (int x = fx0; x <= fx1; x++)
									brow[x] = pattern[x & 3];
								continue;
							}
							for (int x = fx0; x <= fx1; x++, projection += stepX)
							{
								const int permille = clampPermille(static_cast<int>((projection - fMinProjection) * invSpanPermille + 0.5f));
								const DitherStop &e = lut[permille];
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
								const int threshold = bayerRow[x & 3] * 16 + 8;
								const int r5 = e.qR + ((e.remR > threshold) ? 1 : 0);
								const int g6 = e.qG + ((e.remG > threshold) ? 1 : 0);
								const int b5 = e.qB + ((e.remB > threshold) ? 1 : 0);
								brow[x] = gea::framework::graphics::pixel::packRgb565Components(r5, g6, b5);
#else
								brow[x] = e.color; // full colour: no dither
#endif
							}
						}
						std::memcpy(bgKey, bgKeyNow, sizeof bgKeyNow);
						bgKW = scrW;
						bgKH = scrH;
						bgValid = true;
					}
				}

				// Bake the translucent static gradient into the alpha-capable cache once its
				// parameters have been stable for 2 frames.
				if (transCacheable)
				{
					// Row filler: vertical gradients fill the constant-permille 4-pattern
					// at store speed; other angles keep the closed-form per-pixel math the
					// old per-pixel callback used (bit-identical cache content).
					transCache.bakeRows(transKey, 14, c.gradient.x, c.gradient.y, c.gradient.w, c.gradient.h, scrW, scrH, transSpan,
													[&](int y, int rowX0, int rowX1, gea::framework::graphics::pixel::native_t *colRow, std::uint8_t *alpRow)
													{
														const uint8_t *bayerRow = bayer4[y & 3];
														const int spanW = rowX1 - rowX0 + 1;
														if (stepX == 0.0f)
														{
															const float projection = (static_cast<float>(rowX0) + 0.5f) * stepX + (static_cast<float>(y) + 0.5f) * stepY;
															const int permille = clampPermille(static_cast<int>((projection - fMinProjection) * invSpanPermille + 0.5f));
															const DitherStop &e = lut[permille];
															gea::framework::graphics::pixel::native_t pattern[4];
															for (int k = 0; k < 4; k++)
															{
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
																const int threshold = bayerRow[k] * 16 + 8;
																pattern[k] = gea::framework::graphics::pixel::packRgb565Components(
																		e.qR + ((e.remR > threshold) ? 1 : 0), e.qG + ((e.remG > threshold) ? 1 : 0),
																		e.qB + ((e.remB > threshold) ? 1 : 0));
#else
																pattern[k] = e.color;
#endif
															}
															for (int i = 0; i < spanW; i++)
																colRow[i] = pattern[(rowX0 + i) & 3];
															std::memset(alpRow, e.alpha, static_cast<std::size_t>(spanW));
															return;
														}
														for (int i = 0; i < spanW; i++)
														{
															const int x = rowX0 + i;
															const float projection = (static_cast<float>(x) + 0.5f) * stepX + (static_cast<float>(y) + 0.5f) * stepY;
															const int permille = clampPermille(static_cast<int>((projection - fMinProjection) * invSpanPermille + 0.5f));
															const DitherStop &e = lut[permille];
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
															const int threshold = bayerRow[x & 3] * 16 + 8;
															colRow[i] = gea::framework::graphics::pixel::packRgb565Components(
																	e.qR + ((e.remR > threshold) ? 1 : 0), e.qG + ((e.remG > threshold) ? 1 : 0),
																	e.qB + ((e.remB > threshold) ? 1 : 0));
#else
															colRow[i] = e.color;
#endif
															alpRow[i] = e.alpha;
														}
													});
				}
			}
		};

		struct RadialGradientDrawer
		{
			static int normalizedRadius(int radius, int maxRadius)
			{
				if (radius < 0)
					return 0;
				return radius > maxRadius ? maxRadius : radius;
			}

			static void roundedRowSpan(const DisplayCommand &c, int y, int *x0, int *x1)
			{
				const int maxRadius = std::min<int>(c.radialGradient.w / 2, c.radialGradient.h / 2);
				const int tl = normalizedRadius(c.radialGradient.tl, maxRadius);
				const int tr = normalizedRadius(c.radialGradient.tr, maxRadius);
				const int br = normalizedRadius(c.radialGradient.br, maxRadius);
				const int bl = normalizedRadius(c.radialGradient.bl, maxRadius);
				if ((tl | tr | br | bl) == 0)
					return;

				const int left = c.radialGradient.x;
				const int right = c.radialGradient.x + c.radialGradient.w - 1;
				const int top = c.radialGradient.y;
				const int bottom = c.radialGradient.y + c.radialGradient.h - 1;
				int rowLeft = left;
				int rowRight = right;

				if (tl > 0 && y >= top && y <= top + tl)
				{
					const int dy = top + tl - y;
					const int dx = LinearGradientDrawer::integerSqrt(tl * tl - dy * dy);
					rowLeft = std::max(rowLeft, left + tl - dx);
				}
				if (bl > 0 && y >= bottom - bl && y <= bottom)
				{
					const int dy = y - (bottom - bl);
					const int dx = LinearGradientDrawer::integerSqrt(bl * bl - dy * dy);
					rowLeft = std::max(rowLeft, left + bl - dx);
				}
				if (tr > 0 && y >= top && y <= top + tr)
				{
					const int dy = top + tr - y;
					const int dx = LinearGradientDrawer::integerSqrt(tr * tr - dy * dy);
					rowRight = std::min(rowRight, right - tr + dx);
				}
				if (br > 0 && y >= bottom - br && y <= bottom)
				{
					const int dy = y - (bottom - br);
					const int dx = LinearGradientDrawer::integerSqrt(br * br - dy * dy);
					rowRight = std::min(rowRight, right - br + dx);
				}

				*x0 = std::max(*x0, rowLeft);
				*x1 = std::min(*x1, rowRight);
			}

			static int localPermille(const DisplayCommand &c, int permille)
			{
				const int stop = LinearGradientDrawer::clampPermille(c.radialGradient.stopPermille);
				if (stop <= 0 || permille >= stop)
					return 1000;
				if (permille <= 0)
					return 0;
				return (permille * 1000 + stop / 2) / stop;
			}

			static uint16_t colorAt(const DisplayCommand &c, int permille, int x, int y)
			{
				const int local = localPermille(c, permille);
				int r = 0, g = 0, b = 0;
				LinearGradientDrawer::interpolatePremultipliedRgb888(c.radialGradient.fromColor,
																														 c.radialGradient.fromAlpha,
																														 c.radialGradient.toColor,
																														 c.radialGradient.toAlpha,
																														 local,
																														 &r,
																														 &g,
																														 &b);
				return LinearGradientDrawer::packDitheredRgb888(r,
																												g,
																												b,
																												x,
																												y);
			}

			static uint8_t alphaAt(const DisplayCommand &c, int permille)
			{
				return LinearGradientDrawer::interpolateAlpha(c.radialGradient.fromAlpha,
																											c.radialGradient.toAlpha,
																											localPermille(c, permille));
			}

			static void replay(const DisplayCommand &c)
			{
				int cx0, cy0, cx1, cy1;
				gea::platform::display::Display::clip(&cx0, &cy0, &cx1, &cy1);
				const int x0 = std::max<int>(c.radialGradient.x, cx0);
				const int y0 = std::max<int>(c.radialGradient.y, cy0);
				const int x1 = std::min<int>(c.radialGradient.x + c.radialGradient.w - 1, cx1);
				const int y1 = std::min<int>(c.radialGradient.y + c.radialGradient.h - 1, cy1);
				if (x1 < x0 || y1 < y0)
					return;

				const float centerX = static_cast<float>(c.radialGradient.x) +
															static_cast<float>(c.radialGradient.w) * static_cast<float>(c.radialGradient.cxPermille) / 1000.0f;
				const float centerY = static_cast<float>(c.radialGradient.y) +
															static_cast<float>(c.radialGradient.h) * static_cast<float>(c.radialGradient.cyPermille) / 1000.0f;
				const float radiusX = std::max(1.0f,
																			 static_cast<float>(c.radialGradient.w) * static_cast<float>(c.radialGradient.rxPermille) / 1000.0f);
				const float radiusY = std::max(1.0f,
																			 static_cast<float>(c.radialGradient.h) * static_cast<float>(c.radialGradient.ryPermille) / 1000.0f);
				const float invRadiusX = 1.0f / radiusX;
				const float invRadiusY = 1.0f / radiusY;

				// Static-gradient bitmap cache (shared StaticGradientCache). The shell's
				// warm-glow radial is static — fixed colors + box, identical across themes
				// (only the base linear layer changes) — so cache it once and blit instead of
				// recomputing sqrt + dither per pixel. Gated to reasonably large boxes.
				auto *rcanvas = gea::platform::display::Display::canvas();
				const int scrW = rcanvas ? rcanvas->width() : 0;
				const int scrH = rcanvas ? rcanvas->height() : 0;
				const bool cacheable = rcanvas && rcanvas->pixels() && scrW > 0 && scrH > 0 &&
															 static_cast<long long>(c.radialGradient.w) * c.radialGradient.h * 16 >=
																	 static_cast<long long>(scrW) * scrH;
				const std::uint16_t keyNow[17] = {
						static_cast<std::uint16_t>(c.radialGradient.x), static_cast<std::uint16_t>(c.radialGradient.y),
						static_cast<std::uint16_t>(c.radialGradient.w), static_cast<std::uint16_t>(c.radialGradient.h),
						static_cast<std::uint16_t>(c.radialGradient.tl), static_cast<std::uint16_t>(c.radialGradient.tr),
						static_cast<std::uint16_t>(c.radialGradient.br), static_cast<std::uint16_t>(c.radialGradient.bl),
						static_cast<std::uint16_t>(c.radialGradient.cxPermille), static_cast<std::uint16_t>(c.radialGradient.cyPermille),
						static_cast<std::uint16_t>(c.radialGradient.rxPermille), static_cast<std::uint16_t>(c.radialGradient.ryPermille),
						static_cast<std::uint16_t>(c.radialGradient.fromColor), static_cast<std::uint16_t>(c.radialGradient.toColor), c.radialGradient.stopPermille,
						c.radialGradient.fromAlpha, c.radialGradient.toAlpha};
				static StaticGradientCache cache;
				auto span = [&](int y, int *a, int *b)
				{ roundedRowSpan(c, y, a, b); };
				if (cacheable && cache.blit(keyNow, 17, x0, y0, x1, y1, scrW, scrH, span))
					return;

				// Divide-free dither LUT indexed by local permille (0..1000), cached across
				// calls keyed by the color stops (mirrors LinearGradientDrawer). The per-pixel
				// dither becomes a table read + 3 threshold compares — no divides.
#if GEA_EMBEDDED_LAZY_GRADIENT_LUTS
				static auto lutStorage = std::make_unique<std::array<LinearGradientDrawer::DitherStop, 1001>>();
				auto &lut = *lutStorage;
#else
				static LinearGradientDrawer::DitherStop lut[1001];
#endif
				static uint16_t lutKeyFrom = 0xFFFF, lutKeyTo = 0;
				static uint8_t lutKeyFromA = 0xFF, lutKeyToA = 0;
				static uint16_t lutKeyStop = 0xFFFF;
				if (lutKeyFrom != c.radialGradient.fromColor || lutKeyTo != c.radialGradient.toColor ||
						lutKeyFromA != c.radialGradient.fromAlpha || lutKeyToA != c.radialGradient.toAlpha ||
						lutKeyStop != c.radialGradient.stopPermille)
				{
					// Entries are pre-remapped through localPermille(), so the per-pixel
					// lookup is a single clamped index — no divide, no second hop. Same
					// values as lut[localPermille(c, p)] by construction (and p > 1000
					// clamps to 1000, which localPermille also maps to 1000).
					for (int p = 0; p <= 1000; ++p)
					{
						const int lp = localPermille(c, p);
						int r = 0, g = 0, b = 0;
						LinearGradientDrawer::interpolatePremultipliedRgb888(c.radialGradient.fromColor, c.radialGradient.fromAlpha,
																																 c.radialGradient.toColor, c.radialGradient.toAlpha, lp, &r, &g, &b);
						auto split = [](int value, int levels, uint8_t *q, uint8_t *rem)
						{
							if (value <= 0)
							{
								*q = 0;
								*rem = 0;
								return;
							}
							if (value >= 255)
							{
								*q = static_cast<uint8_t>(levels);
								*rem = 0;
								return;
							}
							const int scaled = value * levels;
							const int qq = scaled / 255;
							*q = static_cast<uint8_t>(qq);
							*rem = static_cast<uint8_t>(scaled - qq * 255);
						};
						split(r, 31, &lut[p].qR, &lut[p].remR);
						split(g, 63, &lut[p].qG, &lut[p].remG);
						split(b, 31, &lut[p].qB, &lut[p].remB);
						lut[p].color = gea::framework::graphics::pixel::packNative8(r, g, b);
						lut[p].alpha = LinearGradientDrawer::interpolateAlpha(c.radialGradient.fromAlpha, c.radialGradient.toAlpha, lp);
					}
					lutKeyFrom = c.radialGradient.fromColor;
					lutKeyTo = c.radialGradient.toColor;
					lutKeyFromA = c.radialGradient.fromAlpha;
					lutKeyToA = c.radialGradient.toAlpha;
					lutKeyStop = c.radialGradient.stopPermille;
				}
				static constexpr uint8_t bayer4[4][4] = {
						{0, 8, 2, 10},
						{12, 4, 14, 6},
						{3, 11, 1, 9},
						{15, 7, 13, 5},
				};
				// Build each scanline into a buffer and blit the whole row in one call. Bayer
				// dither makes adjacent pixels differ, so per-run fillRect() shattered every
				// row into ~1px writes (each paying a clip + framebuffer read-back + dirty-list
				// scan) — the radial's real cost, just like the linear drawer. blitImage clips
				// and marks dirty once per row. (baseAlpha is read from Display::alpha() inside.)
				static gea::framework::graphics::pixel::native_t rowColor[gea::platform::display::kWidth];
				static uint8_t rowAlpha[gea::platform::display::kWidth];
				// Mirror gate: a pixel xx and its mirror (mirrorSum - xx) have dx values
				// that are exact float negations of each other when 2*centerX lands on an
				// integer grid (e.g. the shell glow's `at 50%`), so dx*dx — and therefore
				// the permille and LUT entry — are bit-identical. The right half then
				// reuses the left half's entries and only re-runs the (x-dependent)
				// bayer dither, halving the sqrt work.
				const float mirrorSumF = 2.0f * centerX - 1.0f;
				const int mirrorSum = static_cast<int>(mirrorSumF);
				const bool mirrorable = mirrorSumF == static_cast<float>(mirrorSum);
				static std::uint16_t rowPermille[gea::platform::display::kWidth];
				for (int y = y0; y <= y1; ++y)
				{
					int rowX0 = x0;
					int rowX1 = x1;
					roundedRowSpan(c, y, &rowX0, &rowX1);
					if (rowX0 > rowX1)
						continue;
					const float dy = (static_cast<float>(y) + 0.5f - centerY) * invRadiusY;
					const uint8_t *bayerRow = bayer4[y & 3];
					// Keep the device-sized buffer, but cover wider simulator viewports in chunks.
					for (int chunkX = rowX0; chunkX <= rowX1; chunkX += gea::platform::display::kWidth)
					{
						const int spanW = std::min(gea::platform::display::kWidth, rowX1 - chunkX + 1);
						bool rowOpaque = true;
						for (int i = 0; i < spanW; ++i)
						{
							const int xx = chunkX + i;
							const int mi = mirrorSum - xx - chunkX;  // index of xx's mirror in this span
							int permille;
							if (mirrorable && mi >= 0 && mi < i)
							{
								permille = rowPermille[mi];
							}
							else
							{
								const float dx = (static_cast<float>(xx) + 0.5f - centerX) * invRadiusX;
								permille = static_cast<int>(std::sqrt(dx * dx + dy * dy) * 1000.0f + 0.5f);
							}
							rowPermille[i] = static_cast<std::uint16_t>(permille > 1000 ? 1000 : permille);
							const LinearGradientDrawer::DitherStop &e = lut[rowPermille[i]];
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
							const int threshold = bayerRow[xx & 3] * 16 + 8;
							const int r5 = e.qR + ((e.remR > threshold) ? 1 : 0);
							const int g6 = e.qG + ((e.remG > threshold) ? 1 : 0);
							const int b5 = e.qB + ((e.remB > threshold) ? 1 : 0);
							rowColor[i] = gea::framework::graphics::pixel::packRgb565Components(r5, g6, b5);
#else
							rowColor[i] = e.color; // full colour: no dither
#endif
							rowAlpha[i] = e.alpha;
							if (e.alpha != 255)
								rowOpaque = false;
						}
						gea::platform::display::Display::blitImage(rowColor, rowOpaque ? nullptr : rowAlpha, spanW, 1, chunkX, y);
					}
				}

				// Bake the full box into the cache once params are stable for 2 frames.
				if (cacheable)
				{
					cache.bake(keyNow, 17, c.radialGradient.x, c.radialGradient.y, c.radialGradient.w, c.radialGradient.h, scrW, scrH, span,
										 [&](int x, int y, gea::framework::graphics::pixel::native_t *col, std::uint8_t *alp)
										 {
											 const float dyv = (static_cast<float>(y) + 0.5f - centerY) * invRadiusY;
											 const float dxv = (static_cast<float>(x) + 0.5f - centerX) * invRadiusX;
											 const int permille = static_cast<int>(std::sqrt(dxv * dxv + dyv * dyv) * 1000.0f + 0.5f);
											 const LinearGradientDrawer::DitherStop &e = lut[permille > 1000 ? 1000 : permille];
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
											 const int threshold = bayer4[y & 3][x & 3] * 16 + 8;
											 *col = gea::framework::graphics::pixel::packRgb565Components(
													 e.qR + ((e.remR > threshold) ? 1 : 0), e.qG + ((e.remG > threshold) ? 1 : 0),
													 e.qB + ((e.remB > threshold) ? 1 : 0));
#else
				           *col = e.color;  // full colour: no dither
#endif
											 *alp = e.alpha;
										 });
				}
			}
		};

		struct DisplayCommandDrawer
		{
			struct TextInkCommand {
				const DisplayCommand *command;
				int x0, y0, x1, y1;
			};

			static void collectTextInk(int id, int owner, int x0, int y0, int x1, int y1,
			                           std::vector<TextInkCommand> &ink)
			{
				auto &tree = Tree::instance();
				if (id < 0 || id >= tree.nodeCount()) return;
				const auto &node = tree.nodes()[id];
				if (node.style.display == 1 || (id != owner && isOutOfFlowPosition(node.style.position))) return;
				if (state.hasNodeScratchFor(id) && node.style.visibility == 0) {
					const int begin = state.nodeDrawStart[id], end = state.nodeDrawEnd[id];
					for (int ci = std::max(0, begin); ci < end && ci < state.commandCount; ++ci) {
						const auto &c = state.commands[ci];
						if (c.type == DisplayCommandType::DrawText || c.type == DisplayCommandType::DrawProjectedText || c.textDecorationInk)
							ink.push_back({&c, x0, y0, x1, y1});
					}
				}
				// Match the overflow clip recorded for descendants of this node.
				// Positioned in-flow and stacking-context descendants still contribute;
				// their opacity does not change the shape of the mask.
				const int clipIndex = state.hasNodeScratchFor(id) ? state.nodeDrawEnd[id] : -1;
				if (clipIndex >= 0 && clipIndex < state.commandCount) {
					const auto &clip = state.commands[clipIndex];
					if (clip.type == DisplayCommandType::PushClip && clip.clip.nodeId == id) {
						x0 = std::max<int>(x0, clip.clip.x); y0 = std::max<int>(y0, clip.clip.y);
						x1 = std::min<int>(x1, clip.clip.x+clip.clip.w-1); y1 = std::min<int>(y1, clip.clip.y+clip.clip.h-1);
					}
				}
				if (x0 > x1 || y0 > y1) return;
				for (int child = node.first_child; child >= 0; child = tree.nodes()[child].next_sibling)
					collectTextInk(child, owner, x0, y0, x1, y1, ink);
			}

			static void replayTextClipped(const DisplayCommand &command)
			{
				using gea::platform::display::Display;
				using namespace gea::framework::graphics;
				auto *canvas = Display::canvas();
				if (!canvas) return;
				int x0, y0, x1, y1;
				Display::clip(&x0, &y0, &x1, &y1);
				x0 = std::max({x0, 0, static_cast<int>(command.bx)});
				y0 = std::max({y0, 0, static_cast<int>(command.by)});
				x1 = std::min({x1, canvas->width()-1, command.bx+command.bw-1});
				y1 = std::min({y1, canvas->height()-1, command.by+command.bh-1});
				if (x0 > x1 || y0 > y1) return;
				std::vector<TextInkCommand> ink;
				collectTextInk(command.textClipOwner, command.textClipOwner, x0, y0, x1, y1, ink);
				if (ink.empty()) return;
				DisplayCommand paint = command;
				paint.textClipOwner = -1;
				// Stack-local, bounded scratch remains safe if independent dirty rows
				// are replayed on separate workers. No viewport-sized backing store.
				constexpr int chunkSize = 128;
				pixel::native_t before[chunkSize];
				uint8_t coverage[chunkSize];
				for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; x += chunkSize) {
					const int width = std::min(chunkSize, x1-x+1);
					std::fill_n(coverage, width, 0);
					Display::pushClip(x, y, width, 1);
					for (const auto &entry : ink) {
						if (y < entry.y0 || y > entry.y1 || x > entry.x1 || x+width <= entry.x0) continue;
						const auto &c = *entry.command;
						Display::pushClip(entry.x0, entry.y0, entry.x1-entry.x0+1, entry.y1-entry.y0+1);
						if (c.type == DisplayCommandType::DrawText)
							TextRenderer::unionCoverageRow(c, y, x, width, coverage);
						else if (c.type == DisplayCommandType::DrawProjectedText)
							drawProjectedText(c, coverage, x);
						else if (c.textDecorationInk && c.type == DisplayCommandType::FillRect && y >= c.fill.y && y < c.fill.y+c.fill.h)
							for (int px = std::max({x, entry.x0, static_cast<int>(c.fill.x)}); px < std::min({x+width, entry.x1+1, c.fill.x+c.fill.w}); ++px)
								coverage[px-x] = 255;
						Display::popClip();
					}
					if (std::any_of(coverage, coverage+width, [](uint8_t a) { return a != 0; })) {
						for (int i = 0; i < width; ++i) before[i] = canvas->readPixelNative(x+i, y);
						replay(paint);
						for (int i = 0; i < width; ++i) if (coverage[i] != 255)
							canvas->writePixelNativeExact(x+i, y, coverage[i] == 0 ? before[i] :
							    pixel::blendNative(canvas->readPixelNative(x+i, y), before[i], coverage[i]));
					}
					Display::popClip();
				}
			}

			static void noteReplayCommand(const DisplayCommand &c)
			{
				auto &perf = refreshPerfStatsMutable();
				switch (c.type)
				{
				case DisplayCommandType::FillRect:
					perf.treeReplayFillRectCommands++;
					break;
				case DisplayCommandType::FillCircle:
					perf.treeReplayCircleCommands++;
					break;
				case DisplayCommandType::FillRoundedRect:
					perf.treeReplayRoundedRectCommands++;
					break;
				case DisplayCommandType::FillTransformedRoundedRect:
					perf.treeReplayTransformedRoundedRectCommands++;
					break;
				case DisplayCommandType::DrawText:
				case DisplayCommandType::DrawProjectedText:
					perf.treeReplayTextCommands++;
					break;
				default:
					perf.treeReplayOtherCommands++;
					break;
				}
			}

			static void GEA_RENDER_HOT_SRAM replay(const DisplayCommand &c)
			{
				if (c.textClipOwner >= 0 && c.type != DisplayCommandType::SetAlpha &&
				    c.type != DisplayCommandType::PushClip && c.type != DisplayCommandType::PopClip) {
					replayTextClipped(c);
					return;
				}
				noteReplayCommand(c);
				const std::int64_t __replayT0 = refreshPerfNowUs();
				switch (c.type)
				{
				case DisplayCommandType::SetAlpha:
					// Stateful scope command. The full replay() handles this in its own
					// switch (and never routes it here), but the direct dirty-region paths
					// replay command-by-command through this drawer — without a case here,
					// SetAlpha would be a silent no-op and alpha scopes (semi-transparent
					// gradients, grids, strokes) would render fully opaque.
					gea::platform::display::Display::setAlpha(c.alpha.alpha);
					break;
				case DisplayCommandType::FillRect:
					gea::platform::display::Display::fillRect(c.fill.x, c.fill.y, c.fill.w, c.fill.h, c.fill.color);
					break;
				case DisplayCommandType::FillCircle:
					gea::platform::display::Display::fillCircle(c.fillCircle.cx, c.fillCircle.cy, c.fillCircle.r, c.fillCircle.color);
					break;
				case DisplayCommandType::FillRoundedRect:
					gea::platform::display::Display::fillRoundedRect(c.fillRoundedRect.x, c.fillRoundedRect.y,
																													 c.fillRoundedRect.w, c.fillRoundedRect.h,
																													 c.fillRoundedRect.tl, c.fillRoundedRect.tr, c.fillRoundedRect.br, c.fillRoundedRect.bl,
																													 c.fillRoundedRect.color);
					break;
				case DisplayCommandType::FillQuad:
					drawFillQuad(c);
					break;
				case DisplayCommandType::FillLinearGradient:
					LinearGradientDrawer::replay(c);
					break;
				case DisplayCommandType::FillTransformedLinearGradient:
					drawTransformedLinearGradient(c);
					break;
				case DisplayCommandType::FillTransformedRoundedRect:
					drawTransformedRoundedRect(c);
					break;
				case DisplayCommandType::FillRadialGradient:
					RadialGradientDrawer::replay(c);
					break;
				case DisplayCommandType::DrawLine:
					gea::platform::display::Display::drawLine(c.line.x0, c.line.y0, c.line.x1, c.line.y1, c.line.color);
					break;
				case DisplayCommandType::StrokeRect:
					gea::platform::display::Display::strokeRect(c.stroke.x, c.stroke.y, c.stroke.w, c.stroke.h, c.stroke.color);
					break;
				case DisplayCommandType::StrokeRoundedRect:
					if (c.strokeRoundedRect.cssRadii) { drawCssRoundedBorder(c); break; }
					gea::platform::display::Display::strokeRoundedRect(c.strokeRoundedRect.x, c.strokeRoundedRect.y,
																														 c.strokeRoundedRect.w, c.strokeRoundedRect.h,
																														 c.strokeRoundedRect.tl, c.strokeRoundedRect.tr, c.strokeRoundedRect.br, c.strokeRoundedRect.bl,
																														 c.strokeRoundedRect.lineWidth, c.strokeRoundedRect.color);
					break;
				case DisplayCommandType::DrawProjectedText:
					drawProjectedText(c);
					break;
				case DisplayCommandType::DrawText:
					TextRenderer::drawWrapped(c.text.text, c.text.x, c.text.y,
																		c.text.maxWidth, c.text.color, c.text.scale,
																		c.text.align, c.text.containerWidth, c.text.fontId, c.text.textTransform, c.text.lineHeight,
																		c.text.whiteSpace, c.text.textOverflow, c.text.maxHeight, c.text.firstLineIndent);
					break;
				case DisplayCommandType::BlitImage:
#if GEA_PIXEL_STORAGE_PACKED
					// Scroll-time image hold (see gScrollStripReplayActive).
					if (gScrollStripReplayActive) {
						gScrollImageHoldPending = true;
						break;
					}
					if (c.blit.sourcePacked) {
						// Packed canvas surface into the packed framebuffer: a direct
						// same-depth copy, no unpack/repack.
						if (auto *__cv = gea::platform::display::Display::canvas())
							__cv->blitPacked(reinterpret_cast<const std::uint8_t *>(c.blit.pixels),
															 c.blit.sourceWidth, c.blit.sourceHeight, c.blit.dx, c.blit.dy);
						break;
					}
#endif
					gea::platform::display::Display::blitImage(c.blit.pixels, c.blit.alpha, c.blit.sourceWidth, c.blit.sourceHeight,
																										 c.blit.dx, c.blit.dy);
					break;
				case DisplayCommandType::BlitImageScaled:
#if GEA_PIXEL_STORAGE_PACKED
					if (gScrollStripReplayActive) {
						gScrollImageHoldPending = true;
						break;
					}
#endif
					if ((c.scaledBlit.tl > 0 || c.scaledBlit.tr > 0 || c.scaledBlit.br > 0 || c.scaledBlit.bl > 0) &&
							gea::platform::display::Display::canvas())
					{
						gea::platform::display::Display::canvas()->drawImageRounded(
								c.scaledBlit.pixels,
								c.scaledBlit.alpha,
								c.scaledBlit.sourceWidth,
								c.scaledBlit.sourceHeight,
								c.scaledBlit.dx,
								c.scaledBlit.dy,
								c.scaledBlit.dw,
								c.scaledBlit.dh,
								c.scaledBlit.tl,
								c.scaledBlit.tr,
								c.scaledBlit.br,
								c.scaledBlit.bl);
					}
					else
					{
						gea::platform::display::Display::blitImageScaled(c.scaledBlit.pixels, c.scaledBlit.alpha, c.scaledBlit.sourceWidth, c.scaledBlit.sourceHeight,
																														 c.scaledBlit.dx, c.scaledBlit.dy, c.scaledBlit.dw, c.scaledBlit.dh);
					}
					break;
				case DisplayCommandType::BeginFilterBlur:
				{
					const int nodeId = c.filterBlur.nodeId;
					if (nodeId < 0 || nodeId >= kMaxNodes || c.filterBlur.radius <= 0)
						break;
					FilterBlurCacheEntry *cache = state.filterBlurCacheForNode(nodeId);
					if (!cache)
						break;
					captureFilterBlurBackground(c, *cache);
					break;
				}
				case DisplayCommandType::ApplyFilterBlur:
				{
					const int nodeId = c.filterBlur.nodeId;
					if (nodeId < 0 || nodeId >= kMaxNodes || c.filterBlur.radius <= 0)
						break;
					FilterBlurCacheEntry *cache = state.filterBlurCacheForNode(nodeId);
					if (!cache)
						break;
					if (filterBlurCacheMatches(c, *cache))
					{
						state.filterBlurCacheHits++;
					}
					else
					{
						if (!buildFilterBlurCache(c, *cache))
							break;
						state.filterBlurCacheMisses++;
					}
					restoreFilterBlurBackground(*cache);
					compositeFilterBlurLayer(*cache);
					break;
				}
				default:
					break;
				}
				// Attribute this command's replay time to its type bucket (instrumentation
				// for the SLOW-frame log: gradient vs text vs image vs fill vs other).
				const std::int64_t __replayDt = refreshPerfNowUs() - __replayT0;
				auto &__perf = refreshPerfStatsMutable();
				switch (c.type)
				{
				case DisplayCommandType::FillRect:
				case DisplayCommandType::FillRoundedRect:
				case DisplayCommandType::FillQuad:
				case DisplayCommandType::FillTransformedRoundedRect:
					__perf.treeReplayFillUs += __replayDt;
					break;
				case DisplayCommandType::FillCircle:
					__perf.treeReplayCircleUs += __replayDt;
					break;
				case DisplayCommandType::DrawText:
				case DisplayCommandType::DrawProjectedText:
					__perf.treeReplayTextUs += __replayDt;
					break;
				case DisplayCommandType::FillLinearGradient:
				case DisplayCommandType::FillTransformedLinearGradient:
				case DisplayCommandType::FillRadialGradient:
					__perf.treeReplayGradientUs += __replayDt;
					break;
				case DisplayCommandType::BlitImage:
				case DisplayCommandType::BlitImageScaled:
					__perf.treeReplayImageUs += __replayDt;
					break;
				default:
					__perf.treeReplayOtherUs += __replayDt;
					break;
				}
			}
		};

		struct RenderRecorder
		{
			static double edgeLength(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
			{
				return std::hypot(static_cast<double>(x1) - static_cast<double>(x0),
													static_cast<double>(y1) - static_cast<double>(y0));
			}

			static int scaledBlurRadius(int baseRadius, double scale)
			{
				if (baseRadius <= 0)
					return 0;
				if (!std::isfinite(scale) || scale < 0.0)
					scale = 1.0;
				const int scaled = static_cast<int>(std::lround(static_cast<double>(baseRadius) * scale));
				return std::max(baseRadius, scaled > 0 ? scaled : 1);
			}

			static void filterBlurScreenRadii(const Node &node, int baseRadius, int *radiusX, int *radiusY)
			{
				const int w = node.layout.width;
				const int h = node.layout.height;
				if (baseRadius <= 0 || w <= 0 || h <= 0)
				{
					*radiusX = 0;
					*radiusY = 0;
					return;
				}

				int16_t xs[4] = {};
				int16_t ys[4] = {};
				ViewRenderer::transformedRectCorners(node, false, node.layout.x, node.layout.y, w, h, xs, ys);
				const double projectedWidth = (edgeLength(xs[0], ys[0], xs[1], ys[1]) +
																			 edgeLength(xs[3], ys[3], xs[2], ys[2])) *
																			0.5;
				const double projectedHeight = (edgeLength(xs[0], ys[0], xs[3], ys[3]) +
																				edgeLength(xs[1], ys[1], xs[2], ys[2])) *
																			 0.5;
				*radiusX = scaledBlurRadius(baseRadius, projectedWidth / static_cast<double>(w));
				*radiusY = scaledBlurRadius(baseRadius, projectedHeight / static_cast<double>(h));
			}

			static int filterBlurSourceAlphaCap(const Node &node, uint8_t parentAlpha)
			{
				const bool simpleBackgroundLeaf = node.first_child < 0 &&
																					node.type == NodeType::View &&
																					node.style.has_bg &&
																					!styleHasBackgroundImage(node.style) && rstyle(node.style).bg_clip == 0 &&
																					!hasAnyBorder(node.style);
				if (!simpleBackgroundLeaf)
					return -1;
				return (static_cast<int>(parentAlpha) * static_cast<int>(node.style.bg_alpha) + 127) / 255;
			}

			static void recordFilterBlur(int id, const Node &node, DisplayCommandType type, uint8_t parentAlpha)
			{
				if (rstyle(node.style).filter_blur_radius <= 0)
					return;
				if (node.layout.width <= 0 || node.layout.height <= 0)
					return;
				const bool sourceLayerBlur = filterBlurSourceAlphaCap(node, parentAlpha) >= 0;
				const int localSpread = sourceLayerBlur ? std::max(0, static_cast<int>(rstyle(node.style).filter_blur_radius)) * kFilterBlurPasses : 0;
				int16_t xs[4] = {};
				int16_t ys[4] = {};
				ViewRenderer::transformedRectCorners(node, false,
																						 node.layout.x - localSpread,
																						 node.layout.y - localSpread,
																						 node.layout.width + localSpread * 2,
																						 node.layout.height + localSpread * 2,
																						 xs, ys);
				int x0 = xs[0];
				int y0 = ys[0];
				int x1 = xs[0];
				int y1 = ys[0];
				for (int i = 1; i < 4; i++)
				{
					if (xs[i] < x0)
						x0 = xs[i];
					if (xs[i] > x1)
						x1 = xs[i];
					if (ys[i] < y0)
						y0 = ys[i];
					if (ys[i] > y1)
						y1 = ys[i];
				}
				if (x0 > x1 || y0 > y1)
					return;
				int radiusX = rstyle(node.style).filter_blur_radius;
				int radiusY = rstyle(node.style).filter_blur_radius;
				filterBlurScreenRadii(node, rstyle(node.style).filter_blur_radius, &radiusX, &radiusY);
				const int spreadX = sourceLayerBlur ? 0 : std::max(0, radiusX) * kFilterBlurPasses;
				const int spreadY = sourceLayerBlur ? 0 : std::max(0, radiusY) * kFilterBlurPasses;
				x0 -= spreadX;
				y0 -= spreadY;
				x1 += spreadX;
				y1 += spreadY;
				DisplayCommand *cmd = DisplayList::instance().append();
				if (!cmd)
					return;
				cmd->type = type;
				cmd->bx = static_cast<int16_t>(clampInt(x0, -32768, 32767));
				cmd->by = static_cast<int16_t>(clampInt(y0, -32768, 32767));
				cmd->bw = static_cast<int16_t>(clampInt(x1 - x0 + 1, 0, 32767));
				cmd->bh = static_cast<int16_t>(clampInt(y1 - y0 + 1, 0, 32767));
				cmd->filterBlur.nodeId = static_cast<int16_t>(id);
				cmd->filterBlur.radius = rstyle(node.style).filter_blur_radius;
				cmd->filterBlur.radiusX = static_cast<int16_t>(clampInt(radiusX, 0, 32767));
				cmd->filterBlur.radiusY = static_cast<int16_t>(clampInt(radiusY, 0, 32767));
				cmd->filterBlur.sourceAlphaCap = static_cast<int16_t>(filterBlurSourceAlphaCap(node, parentAlpha));
			}

			struct RecordClipFrame {
				const Node &node;
				const RecordClipFrame *parent;
			};

			// Fixed descendants are painted in place in z-order, but their viewport
			// containing block is outside the ordinary ancestors' overflow clips.
			struct SuspendedRecordClips {
				const RecordClipFrame *clips;
				explicit SuspendedRecordClips(const RecordClipFrame *value) : clips(value) {
					for (auto *frame = clips; frame; frame = frame->parent)
						ViewRenderer::recordClipEnd(frame->node);
				}
				static void restore(const RecordClipFrame *frame) {
					if (!frame) return;
					restore(frame->parent);
					ViewRenderer::recordClipBegin(frame->node);
				}
				~SuspendedRecordClips() { restore(clips); }
			};

			static void recordNode(int id, uint8_t parent_alpha, int cx0, int cy0, int cx1, int cy1, const Node *mask_node, bool insideDirty = false, const RecordClipFrame *clips = nullptr, bool groupRoot = true, bool includePositioned = true)
			{
				if (!state.hasNodeScratchFor(id))
					return;
				// Backdrop-cache record skip: skip recording a subtree only when it's fully
				// static — neither containing a dirty node (subtreeDirty) NOR sitting under one
				// (insideDirty, set once we descend into a dirty node so the cube's faces are
				// always recorded). Such subtrees are baked into the backdrop and blitted, not
				// replayed, so re-recording them (the transformed stage grids dominate the
				// static record cost) is wasted. Only active once the backdrop is baked.
				if (state.recordSkipStatic && !insideDirty && id >= 0 && id < kMaxNodes && !state.subtreeDirty[id])
				{
					state.clearNodeRange(id);
					return;
				}
				Node *n = &Tree::instance().nodes()[id];
				if (n->style.display == 1 || isCollapsedFlexSubtree(*n) || ViewRenderer::backfaceSubtreeHidden(*n))
				{
					state.clearNodeRange(id);
					return;
				}
				if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
				{
					state.clearNodeRange(id);
					return;
				}
				// Treat opacity:0 the same as display:none for recording purposes.
				// JSX patterns like `style={{ opacity: brick.opacity }}` where the
				// only values are 0 or 255 are used to toggle visibility (see e.g.
				// tilt-breakout's brick-killing path). Without this guard, an
				// opacity=0 node still records its FillRect/SetAlpha pair into the
				// display list. The SetAlpha + alpha=0 fill is a no-op at draw time
				// but the surrounding parent_alpha re-application leaves no command
				// at this node that overlaps the dirty region, so the previous
				// frame's pixels stayed on screen. Skipping the subtree entirely
				// guarantees the parent's background fill is what replays in the
				// dirty rect — same semantics as display:none, without forcing app
				// code to choose between the two patterns.
				bool contributesTextMask = false;
				if (n->style.opacity == 0) {
					const auto *nodes = Tree::instance().nodes();
					for (int ancestor = id; ancestor >= 0; ancestor = nodes[ancestor].parent) {
						if (isOutOfFlowPosition(nodes[ancestor].style.position)) break;
						const int parent = nodes[ancestor].parent;
						if (parent >= 0 && StyleValues::hasTextBackgroundClip(nodes[parent].style)) {
							contributesTextMask = true; break;
						}
					}
				}
				if (n->style.opacity == 0 && !contributesTextMask)
				{
					state.clearNodeRange(id);
					return;
				}

				const bool viewportFixed = LayoutEngine::isViewportFixed(*n);
				SuspendedRecordClips suspended(viewportFixed ? clips : nullptr);
				if (viewportFixed) {
					clips = nullptr;
					cx0 = cy0 = 0;
					const auto *canvas = gea::platform::display::Display::canvas();
					cx1 = (canvas ? canvas->width() : treeState().mountedWidth) - 1;
					cy1 = (canvas ? canvas->height() : treeState().mountedHeight) - 1;
				}
				int overlaps_clip = isDocumentCanvasRoot(*n) || ClipMath::nodeOverlapsClip(*n, cx0, cy0, cx1, cy1);
				if (!overlaps_clip && (!isViewLikeNodeType(n->type) || (overflowX(n->style) && overflowY(n->style))) &&
				    !LayoutEngine::containsViewportFixed(id))
				{
					state.clearNodeRange(id);
					return;
				}

				int x = n->layout.x;
				int y = n->layout.y;
				int w = n->layout.width;
				int h = n->layout.height;

				DisplayList &list = DisplayList::instance();
				// A culled ancestor may still be traversed to reach a fixed child.
				// Keep its clip active for ordinary children; the fixed child alone
				// suspends that clip when it enters viewport coordinates.
				int pushed_clip = 0;
				uint8_t cur_alpha = parent_alpha;
				const Node *active_mask = mask_node;

				if (n->style.opacity < 255)
				{
					cur_alpha = (parent_alpha * n->style.opacity) / 255;
					DisplayCommand *cmd = list.append();
					if (cmd)
					{
						cmd->type = DisplayCommandType::SetAlpha;
						cmd->bx = x;
						cmd->by = y;
						cmd->bw = w;
						cmd->bh = h;
						cmd->alpha.alpha = cur_alpha;
						cmd->alpha.recordParentAlpha = parent_alpha;
						cmd->alpha.nodeId = static_cast<int16_t>(id); // OPEN: patchable in place
					}
				}

				if (n->style.mask_right_fade_width > 0)
					active_mask = n;

				int draw_start = state.commandCount;
				const bool paintsOwnBox = overlaps_clip && n->style.visibility == 0;
				const bool nativeTextInput = isNativeTextInputView(*n);
				const bool filtered = overlaps_clip && rstyle(n->style).filter_blur_radius > 0;
				uint8_t draw_alpha = cur_alpha;
				bool pushed_mask_alpha = false;
				if (active_mask && active_mask->style.mask_right_fade_width > 0)
				{
					draw_alpha = multiplyAlpha(cur_alpha, rightFadeMaskAlphaForChild(*active_mask, *n));
					if (draw_alpha != cur_alpha)
					{
						appendAlphaCommand(list, *n, draw_alpha, cur_alpha);
						pushed_mask_alpha = true;
					}
				}

				if (filtered)
					recordFilterBlur(id, *n, DisplayCommandType::BeginFilterBlur, draw_alpha);

				if (paintsOwnBox && !nativeTextInput)
					ViewRenderer::recordBox(*n, draw_alpha);

				if (paintsOwnBox && n->type == NodeType::Text)
				{
					TextRenderer::record(*n, draw_alpha);
				}
				else if (paintsOwnBox && n->type == NodeType::Image)
				{
					ImageRenderer::record(*n);
				}
				else if (paintsOwnBox && n->type == NodeType::Canvas)
				{
					CanvasRenderer::record(*n);
				}
				else if (paintsOwnBox && n->type == NodeType::Camera)
				{
					CameraRenderer::record(*n);
				}
				else if (paintsOwnBox && n->type == NodeType::View)
				{
					// `<input>` JSX elements lower to a View node with tag_name "input".
					// On native text-input targets, UIKit/AppKit owns the full visual
					// control. Other targets paint framework input chrome plus content.
					if (!nativeTextInput && std::strcmp(tagFromId(n->tag_id), "input") == 0)
						InputRenderer::record(id);
				}
				if (pushed_mask_alpha)
					appendAlphaCommand(list, *n, cur_alpha, cur_alpha);
				int draw_end = state.commandCount;
				state.nodeDrawStart[id] = draw_start;
				state.nodeDrawEnd[id] = draw_end;
				if (draw_end > draw_start && state.drawNodeOrderCount < state.nodeScratchCapacity)
					state.drawNodeOrder[state.drawNodeOrderCount++] = id;

				// A box's own background, border and outline are outside its content clip.
				pushed_clip = ViewRenderer::recordClipBegin(*n);
				++state.recordDepth;
				const auto children = PaintOrder::collectChildren(id, groupRoot, includePositioned);

				int child_cx0 = cx0, child_cy0 = cy0, child_cx1 = cx1, child_cy1 = cy1;
				if (pushed_clip && id == state.recordExpandedClipNode)
				{
					child_cx0 = state.recordExpandedClipX0;
					child_cy0 = state.recordExpandedClipY0;
					child_cx1 = state.recordExpandedClipX1;
					child_cy1 = state.recordExpandedClipY1;
				}
				else if (pushed_clip)
					ClipMath::clampToNode(*n, &child_cx0, &child_cy0, &child_cx1, &child_cy1);

				const RecordClipFrame ownClip{*n, clips};
				for (int child : children) {
					// A positioned descendant can paint in this context while its
					// intervening non-stacking ancestors still supply overflow clips.
					Node *nodes = Tree::instance().nodes();
					std::vector<int> ancestors;
					bool dirty = insideDirty || n->render.dirty;
					for (int p = nodes[child].parent; p >= 0 && p != id; p = nodes[p].parent) {
						dirty |= nodes[p].render.dirty;
						if (nodes[p].style.overflow != 0) ancestors.push_back(p);
					}
					std::vector<RecordClipFrame> frames;
					frames.reserve(ancestors.size());
					const RecordClipFrame *childClips = pushed_clip ? &ownClip : clips;
					int x0 = child_cx0, y0 = child_cy0, x1 = child_cx1, y1 = child_cy1;
					for (auto p = ancestors.rbegin(); p != ancestors.rend(); ++p) {
						const Node &ancestor = nodes[*p];
						if (ViewRenderer::recordClipBegin(ancestor)) {
							frames.push_back({ancestor, childClips}); childClips = &frames.back();
							if (*p == state.recordExpandedClipNode) {
								x0 = state.recordExpandedClipX0; y0 = state.recordExpandedClipY0;
								x1 = state.recordExpandedClipX1; y1 = state.recordExpandedClipY1;
							} else ClipMath::clampToNode(ancestor, &x0, &y0, &x1, &y1);
						}
					}
					recordNode(child, cur_alpha, x0, y0, x1, y1, active_mask, dirty, childClips, PaintOrder::isGroup(child), PaintOrder::isContext(child));
					for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame) ViewRenderer::recordClipEnd(frame->node);
				}

				state.recordDepth--;

				if (paintsOwnBox && !nativeTextInput)
					ViewRenderer::recordScrollbar(*n);

				if (filtered && !nativeTextInput)
					recordFilterBlur(id, *n, DisplayCommandType::ApplyFilterBlur, cur_alpha);

				if (n->style.opacity < 255)
				{
					DisplayCommand *cmd = list.append();
					if (cmd)
					{
						cmd->type = DisplayCommandType::SetAlpha;
						cmd->bx = x;
						cmd->by = y;
						cmd->bw = w;
						cmd->bh = h;
						cmd->alpha.alpha = parent_alpha;
						cmd->alpha.recordParentAlpha = parent_alpha;
						cmd->alpha.nodeId = -1; // CLOSE: restore-only, not patchable
					}
				}

				if (pushed_clip)
					ViewRenderer::recordClipEnd(*n);
			}
		};

		struct UniformRoundedRectBatch
		{
			// In-flight buffer size, NOT a hard limit: append() flushes and starts a fresh
			// chunk when full, so any number of same-geometry rounded rects is handled — this
			// only bounds how many coalesce into one fillRoundedRectBoxesRgb565 call. Sizing it
			// to kMaxNodes (512, the worst case of every node being a same-geometry rounded
			// rect) reserved ~3KB of stack per construction for a batch that holds ~tens in
			// practice; 128 covers any realistic single-region count and cuts that 4x.
			static constexpr int kMax = 128;

			// Not value-initialized: only [0,count) is ever read, and append() writes each
			// slot before flush() reads it. Zeroing all kMax entries cost a memset per
			// construction, and this batch is stack-built per replaySimpleClippedDirtyRegion
			// call (~24x/frame) — pure waste on the dirty-region replay hot path.
			std::int16_t xs[kMax];
			std::int16_t ys[kMax];
			gea::framework::graphics::pixel::native_t colors[kMax];
			int count = 0;
			int w = 0;
			int h = 0;
			int tl = 0;
				int tr = 0;
				int br = 0;
				int bl = 0;
				int roundedCount = 0;
				int transformedRoundedCount = 0;

			bool sameGeometry(int nextW, int nextH, int nextTl, int nextTr, int nextBr, int nextBl) const
			{
				return nextW == w &&
							 nextH == h &&
							 nextTl == tl &&
							 nextTr == tr &&
							 nextBr == br &&
							 nextBl == bl;
			}

			void flush()
			{
					if (count <= 0)
						return;
					const std::int64_t t0 = refreshPerfNowUs();
					gea::platform::display::Display::fillRoundedRectBoxesRgb565(xs, ys, count, w, h, tl, tr, br, bl, colors);
					auto &perf = refreshPerfStatsMutable();
					perf.treeReplayFillUs += refreshPerfNowUs() - t0;
					perf.treeReplayRoundedRectCommands += roundedCount;
					perf.treeReplayTransformedRoundedRectCommands += transformedRoundedCount;
					count = 0;
					roundedCount = 0;
					transformedRoundedCount = 0;
					w = h = tl = tr = br = bl = 0;
				}

				bool appendBox(int x,
										 int y,
										 int nextW,
										 int nextH,
										 int nextTl,
											 int nextTr,
											 int nextBr,
											 int nextBl,
											 gea::framework::graphics::pixel::native_t color,
											 bool transformed)
				{
					if (nextW <= 0 || nextH <= 0)
						return false;
				if (count > 0 && !sameGeometry(nextW, nextH, nextTl, nextTr, nextBr, nextBl))
					flush();
				if (count >= kMax)
					flush();
				w = nextW;
				h = nextH;
				tl = nextTl;
				tr = nextTr;
				br = nextBr;
				bl = nextBl;
				xs[count] = static_cast<std::int16_t>(x);
					ys[count] = static_cast<std::int16_t>(y);
					colors[count] = color;
					count++;
					if (transformed)
						transformedRoundedCount++;
					else
						roundedCount++;
					return true;
				}

			bool append(const DisplayCommand &c)
			{
				if (c.textClipOwner >= 0) return false;
				if (c.type == DisplayCommandType::FillRoundedRect)
					return appendBox(c.fillRoundedRect.x,
													 c.fillRoundedRect.y,
													 c.fillRoundedRect.w,
													 c.fillRoundedRect.h,
														 c.fillRoundedRect.tl,
														 c.fillRoundedRect.tr,
														 c.fillRoundedRect.br,
														 c.fillRoundedRect.bl,
														 c.fillRoundedRect.color,
														 false);
				if (c.type != DisplayCommandType::FillTransformedRoundedRect)
					return false;

				const auto &r = c.transformedRoundedRect;
				if (r.backfaceHidden)
				{
					const long area2 = static_cast<long>(r.x0) * r.y1 - static_cast<long>(r.x1) * r.y0 +
														 static_cast<long>(r.x1) * r.y2 - static_cast<long>(r.x2) * r.y1 +
														 static_cast<long>(r.x2) * r.y3 - static_cast<long>(r.x3) * r.y2 +
														 static_cast<long>(r.x3) * r.y0 - static_cast<long>(r.x0) * r.y3;
					if (area2 <= 0)
						return true;
				}

				RoundedRectScreenSpan span{};
				if (!transformedRoundedRectToScreenSpan(r, &span) ||
						!roundedRectScreenSpanFitsCanvas(span))
					return false;
				if (roundedRectScreenSpanIsCircle(span))
				{
					flush();
					return drawCircleLikeScreenSpan(span, r.color);
				}
				return appendBox(span.x,
												 span.y,
												 span.w,
												 span.h,
												 radius8ToPixels(span.tlRx8),
													 radius8ToPixels(span.trRx8),
													 radius8ToPixels(span.brRx8),
													 radius8ToPixels(span.blRx8),
													 r.color,
													 true);
				}
		};

		// ---- Static-backdrop cache ----
		// Bakes every non-dirty (static) node into the gea_bg_cache buffer once the scene
		// is stable, so the dirty-region replay can blit that backdrop and replay only the
		// dynamic subtree (the spinning cube) on top — the static stage then costs nothing
		// per frame. Uses the same blit + dirty-region pattern as the Phase-1 gradient
		// cache (stable on-device), so it inherits that buffering correctness.
		bool gBackdropCacheValid = false;
		int gBackdropStableFrames = 0;
		bool gBackdropBaking = false;
		// Set right after a bake: the next frame must repaint the FULL viewport from the
		// cache (blit the whole baked backdrop + replay the cube), so the framebuffer AND
		// panel become exactly the baked backdrop everywhere. After that every per-region
		// blit matches its surroundings (both are the baked backdrop) and no dirty-region
		// boundary seam shows.
		bool gBackdropFullSyncPending = false;

		// A node is "dynamic" (replayed on top of the blitted backdrop) if it or any
		// ancestor is render-dirty this frame; everything else is static (in the bake).
		bool nodeOrAncestorRenderDirty(int node_id, const Node *nodes, int nodeCount)
		{
			int guard = 0;
			for (int id = node_id; id >= 0 && id < nodeCount && guard <= nodeCount; id = nodes[id].parent, ++guard)
				if (nodes[id].render.dirty)
					return true;
			return false;
		}

		// Blit the baked static backdrop over [x0,y0]-[x1,y1] one row at a time — the same
		// blitImage() path a normal render uses, so the panel flush sees the dirty pattern
		// it expects (the raw-memcpy variant is what caused the Phase-1 freeze).
		bool blitStaticBackdrop(int x0, int y0, int x1, int y1)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas)
				return false;
			int cap = 0;
			gea::framework::graphics::pixel::native_t *bg = gea_backdrop_cache(&cap);
			const int scrW = canvas->width();
			const int scrH = canvas->height();
			if (!bg || scrW <= 0 || scrH <= 0 || cap < scrW * scrH)
				return false;
			if (x0 < 0)
				x0 = 0;
			if (y0 < 0)
				y0 = 0;
			if (x1 > scrW - 1)
				x1 = scrW - 1;
			if (y1 > scrH - 1)
				y1 = scrH - 1;
			if (x0 > x1 || y0 > y1)
				return false;
			// The backdrop is an OPAQUE copy: Display::blitImage()/Canvas::drawImage blends
			// the source over the existing framebuffer when the global alpha is < 255
			// (pixel::blend), so if a prior SetAlpha scope (a translucent cube face/label/
			// border) left the alpha reduced, the blit would ghost stale pixels through
			// instead of replacing them — leaving dark trailing residue at the cube's old
			// positions. Force 255 for the copy, then restore.
			const std::uint8_t savedAlpha = gea::platform::display::Display::alpha();
			if (savedAlpha != 255)
				gea::platform::display::Display::setAlpha(255);
			const int spanW = x1 - x0 + 1;
			for (int y = y0; y <= y1; y++)
				gea::platform::display::Display::blitImage(bg + static_cast<long long>(y) * scrW + x0, nullptr, spanW, 1, x0, y);
			if (savedAlpha != 255)
				gea::platform::display::Display::setAlpha(savedAlpha);
			return true;
		}

		bool axisRoundedRectSampleContains(const decltype(DisplayCommand{}.fillRoundedRect) &r, float sx, float sy);

		bool commandsSameRetainedBackground(const DisplayCommand &a, const DisplayCommand &b)
		{
			if (a.type != b.type)
				return false;
			if (a.type == DisplayCommandType::FillRect)
			{
				return a.fill.x == b.fill.x &&
							 a.fill.y == b.fill.y &&
							 a.fill.w == b.fill.w &&
							 a.fill.h == b.fill.h &&
							 a.fill.color == b.fill.color;
			}
			if (a.type == DisplayCommandType::FillRoundedRect)
			{
				return a.fillRoundedRect.x == b.fillRoundedRect.x &&
							 a.fillRoundedRect.y == b.fillRoundedRect.y &&
							 a.fillRoundedRect.w == b.fillRoundedRect.w &&
							 a.fillRoundedRect.h == b.fillRoundedRect.h &&
							 a.fillRoundedRect.tl == b.fillRoundedRect.tl &&
							 a.fillRoundedRect.tr == b.fillRoundedRect.tr &&
							 a.fillRoundedRect.br == b.fillRoundedRect.br &&
							 a.fillRoundedRect.bl == b.fillRoundedRect.bl &&
							 a.fillRoundedRect.color == b.fillRoundedRect.color;
			}
			return false;
		}

		bool axisRoundedRectSampleContains(const decltype(DisplayCommand{}.fillRoundedRect) &r, float sx, float sy);

		bool regionInsideRetainedBackground(const DisplayCommand &command, int x0, int y0, int x1, int y1)
		{
			if (x0 > x1 || y0 > y1)
				return false;
			if (command.type == DisplayCommandType::FillRect)
			{
				return x0 >= command.fill.x &&
							 y0 >= command.fill.y &&
							 x1 < command.fill.x + command.fill.w &&
							 y1 < command.fill.y + command.fill.h;
			}
			if (command.type != DisplayCommandType::FillRoundedRect)
				return false;
			auto r = command.fillRoundedRect;
			const int maxRadius = std::min(r.w / 2, r.h / 2);
			r.tl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tl), 0, maxRadius));
			r.tr = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tr), 0, maxRadius));
			r.br = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.br), 0, maxRadius));
			r.bl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.bl), 0, maxRadius));
			if (x0 < r.x || y0 < r.y || x1 >= r.x + r.w || y1 >= r.y + r.h)
				return false;
			if ((r.tl | r.tr | r.br | r.bl) == 0)
				return true;
			const float left = static_cast<float>(x0) + 0.5f;
			const float top = static_cast<float>(y0) + 0.5f;
			const float right = static_cast<float>(x1) + 0.5f;
			const float bottom = static_cast<float>(y1) + 0.5f;
			return axisRoundedRectSampleContains(r, left, top) &&
						 axisRoundedRectSampleContains(r, right, top) &&
						 axisRoundedRectSampleContains(r, right, bottom) &&
						 axisRoundedRectSampleContains(r, left, bottom);
		}

		GEA_RENDER_HOT_SRAM bool restoreRetainedBackgroundInRegion(int x0, int y0, int x1, int y1)
		{
			for (int i = state.retainedBackgroundRecolorCount - 1; i >= 0; --i)
			{
				const auto &entry = state.retainedBackgroundRecolors[i];
				if (entry.node < 0)
					continue;
				if (!regionInsideRetainedBackground(entry.command, x0, y0, x1, y1))
					continue;
				const std::uint16_t color = entry.command.type == DisplayCommandType::FillRect
																				? entry.command.fill.color
																				: entry.command.fillRoundedRect.color;
				gea::platform::display::Display::fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);
				return true;
			}
			return false;
		}

		GEA_RENDER_HOT_SRAM bool retainedBackgroundCommandCanBeSkippedInRegion(const DisplayCommand &command, int x0, int y0, int x1, int y1)
		{
			for (int i = state.retainedBackgroundRecolorCount - 1; i >= 0; --i)
			{
				const auto &entry = state.retainedBackgroundRecolors[i];
				if (entry.node < 0)
					continue;
				if (!commandsSameRetainedBackground(command, entry.command))
					continue;
				return regionInsideRetainedBackground(entry.command, x0, y0, x1, y1);
			}
			return false;
		}

		GEA_RENDER_HOT_SRAM bool retainedBackgroundCommandCanBeSkippedInRegions(const DisplayCommand &command,
																												const DisplayReplayRegion *regions,
																												int count)
		{
			if (!regions || count <= 0)
				return false;
			int match = -1;
			for (int i = state.retainedBackgroundRecolorCount - 1; i >= 0; --i)
			{
				const auto &entry = state.retainedBackgroundRecolors[i];
				if (entry.node < 0)
					continue;
				if (!commandsSameRetainedBackground(command, entry.command))
					continue;
				match = i;
				break;
			}
			if (match < 0)
				return false;
			bool overlapsAny = false;
			for (int ri = 0; ri < count; ++ri)
			{
				const auto &region = regions[ri];
				if (region.x0 > region.x1 || region.y0 > region.y1)
					continue;
				if (!ClipMath::rectsOverlap(command.bx, command.by, command.bx + command.bw - 1, command.by + command.bh - 1,
																		region.x0, region.y0, region.x1, region.y1))
					continue;
				overlapsAny = true;
				if (!regionInsideRetainedBackground(state.retainedBackgroundRecolors[match].command,
																						region.x0, region.y0, region.x1, region.y1))
					return false;
			}
			return overlapsAny;
		}

		GEA_RENDER_HOT_SRAM bool retainedBackgroundNodeCanBeSkippedInRegion(int node, int x0, int y0, int x1, int y1)
		{
			if (!state.hasNodeScratchFor(node))
				return false;
			const int start = state.nodeDrawStart[node];
			const int end = state.nodeDrawEnd[node];
			if (start < 0 || end != start + 1 || end > state.commandCount)
				return false;
			const DisplayCommand &command = state.commands[start];
			for (int i = state.retainedBackgroundRecolorCount - 1; i >= 0; --i)
			{
				const auto &entry = state.retainedBackgroundRecolors[i];
				if (entry.node != node)
					continue;
				if (!commandsSameRetainedBackground(command, entry.command))
					continue;
				return regionInsideRetainedBackground(entry.command, x0, y0, x1, y1);
			}
			return false;
		}

		GEA_RENDER_HOT_SRAM bool retainedBackgroundNodeCanBeSkippedInRegions(int node, const DisplayReplayRegion *regions, int count)
		{
			if (!regions || count <= 0)
				return false;
			if (!state.hasNodeScratchFor(node))
				return false;
			const int start = state.nodeDrawStart[node];
			const int end = state.nodeDrawEnd[node];
			if (start < 0 || end != start + 1 || end > state.commandCount)
				return false;
			const DisplayCommand &command = state.commands[start];
			int match = -1;
			for (int i = state.retainedBackgroundRecolorCount - 1; i >= 0; --i)
			{
				const auto &entry = state.retainedBackgroundRecolors[i];
				if (entry.node != node)
					continue;
				if (!commandsSameRetainedBackground(command, entry.command))
					continue;
				match = i;
				break;
			}
			if (match < 0)
				return false;
			bool overlapsAny = false;
			for (int ri = 0; ri < count; ++ri)
			{
				const auto &region = regions[ri];
				if (region.x0 > region.x1 || region.y0 > region.y1)
					continue;
				if (!ClipMath::rectsOverlap(command.bx, command.by, command.bx + command.bw - 1, command.by + command.bh - 1,
																		region.x0, region.y0, region.x1, region.y1))
					continue;
				overlapsAny = true;
				if (!regionInsideRetainedBackground(state.retainedBackgroundRecolors[match].command,
																						region.x0, region.y0, region.x1, region.y1))
					return false;
			}
			return overlapsAny;
		}

			void restoreRetainedBackgroundsInRegions(const DisplayReplayRegion *regions, int count)
			{
				if (!regions || count <= 0)
					return;
				const std::int64_t t0 = refreshPerfNowUs();
				for (int ri = 0; ri < count; ++ri)
				{
					const auto &region = regions[ri];
					if (region.x0 <= region.x1 && region.y0 <= region.y1)
						restoreRetainedBackgroundInRegion(region.x0, region.y0, region.x1, region.y1);
				}
				refreshPerfStatsMutable().treeReplayBgRestoreUs += refreshPerfNowUs() - t0;
			}

			struct ReplayNodeWalkTimer
			{
				std::int64_t start = refreshPerfNowUs();
				~ReplayNodeWalkTimer()
				{
					refreshPerfStatsMutable().treeReplayNodeWalkUs += refreshPerfNowUs() - start;
				}
			};

			struct DisplayCommandReplayer
		{
			static bool transformedRoundedRectPaintsRegion(const DisplayCommand &command, int x0, int y0, int x1, int y1)
			{
				if (x0 > x1 || y0 > y1)
					return false;
				const auto &r = command.transformedRoundedRect;
				const int ix0 = std::max(static_cast<int>(command.bx), x0);
				const int iy0 = std::max(static_cast<int>(command.by), y0);
				const int ix1 = std::min(static_cast<int>(command.bx + command.bw - 1), x1);
				const int iy1 = std::min(static_cast<int>(command.by + command.bh - 1), y1);
				if (ix0 > ix1 || iy0 > iy1)
					return false;

				RoundedRectScreenSpan span{};
				if (transformedRoundedRectToScreenSpan(r, &span))
				{
					for (int y = iy0; y <= iy1; ++y)
					{
						int rowX0 = 0;
						int rowX1 = -1;
						if (!roundedRectRowSpan(span, y, &rowX0, &rowX1))
							continue;
						if (rowX0 <= ix1 && rowX1 >= ix0)
							return true;
					}
					return false;
				}

				if (!transformedRoundedRectLooksAffine(r))
					return true;
				const int area = (ix1 - ix0 + 1) * (iy1 - iy0 + 1);
				if (area > 4096)
					return true;
				const float invLocalW = r.lw != 0 ? (1.0f / static_cast<float>(r.lw)) : 0.0f;
				const float invLocalH = r.lh != 0 ? (1.0f / static_cast<float>(r.lh)) : 0.0f;
				const float a = static_cast<float>(r.x1 - r.x0) * invLocalW;
				const float b = static_cast<float>(r.x3 - r.x0) * invLocalH;
				const float c = static_cast<float>(r.y1 - r.y0) * invLocalW;
				const float d = static_cast<float>(r.y3 - r.y0) * invLocalH;
				const float det = a * d - b * c;
				if (std::fabs(det) < 1e-6f)
					return true;
				const float invDet = 1.0f / det;
				const float inv00 = d * invDet;
				const float inv01 = -b * invDet;
				const float inv10 = -c * invDet;
				const float inv11 = a * invDet;
				const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
				const bool useCoverage = aaSamples >= 2;
				for (int y = iy0; y <= iy1; ++y)
				{
					const float py = static_cast<float>(y) + 0.5f - static_cast<float>(r.y0);
					float localX = static_cast<float>(r.lx) + inv00 * (static_cast<float>(ix0) + 0.5f - static_cast<float>(r.x0)) + inv01 * py;
					float localY = static_cast<float>(r.ly) + inv10 * (static_cast<float>(ix0) + 0.5f - static_cast<float>(r.x0)) + inv11 * py;
					for (int x = ix0; x <= ix1; ++x)
					{
						const bool paints = useCoverage
																		? roundedRectCoverageFast(r, localX, localY, inv00, inv10, inv01, inv11, aaSamples) > 0
																		: roundedRectContainsFast(r, localX, localY);
						if (paints)
							return true;
						localX += inv00;
						localY += inv10;
					}
				}
				return false;
			}

				static bool commandPaintsRegion(const DisplayCommand &command, int x0, int y0, int x1, int y1)
				{
					if (!commandBBoxOverlapsRegion(command, x0, y0, x1, y1))
					{
						return false;
				}
					if (command.type == DisplayCommandType::FillTransformedRoundedRect)
						return transformedRoundedRectPaintsRegion(command, x0, y0, x1, y1);
					return true;
				}

				static bool GEA_RENDER_HOT_SRAM commandMayPaintRegion(const DisplayCommand &command, int x0, int y0, int x1, int y1)
				{
					return commandBBoxOverlapsRegion(command, x0, y0, x1, y1);
				}

				static bool GEA_RENDER_HOT_SRAM commandBBoxOverlapsRegion(const DisplayCommand &command, int x0, int y0, int x1, int y1)
				{
				return ClipMath::rectsOverlap(command.bx,
																			command.by,
																			command.bx + command.bw - 1,
																			command.by + command.bh - 1,
																			x0,
																			y0,
																			x1,
																			y1);
			}

			static bool commandRangeOverlaps(int start, int end, int x0, int y0, int x1, int y1)
			{
				if (start < 0 || end <= start || end > state.commandCount)
					return false;
					for (int ci = start; ci < end; ci++)
					{
						DisplayCommand *c = &state.commands[ci];
						auto &perf = refreshPerfStatsMutable();
						perf.treeReplayCommandChecks++;
						if (commandMayPaintRegion(*c, x0, y0, x1, y1))
							return true;
					}
					return false;
				}

			static bool GEA_RENDER_HOT_SRAM commandRangeBBoxOverlaps(int start, int end, int x0, int y0, int x1, int y1)
			{
				if (start < 0 || end <= start || end > state.commandCount)
					return false;
				for (int ci = start; ci < end; ci++)
				{
					DisplayCommand *c = &state.commands[ci];
					if (commandBBoxOverlapsRegion(*c, x0, y0, x1, y1))
						return true;
				}
				return false;
			}

			static bool nodeHasTransformChain(int node_id, Node *nodes, int nodeCount)
			{
				for (int cursor = node_id; cursor >= 0 && cursor < nodeCount; cursor = nodes[cursor].parent)
				{
					if (hasRetainedTransformState(nodes[cursor]))
						return true;
				}
				return false;
			}

			static bool nodeRecordsOverflowClip(int node_id, Node *nodes, int nodeCount)
			{
				if (node_id < 0 || node_id >= nodeCount)
					return false;
				const Node &node = nodes[node_id];
				if (!isViewLikeNodeType(node.type) || node.style.overflow == 0)
					return false;
				if (node.first_child < 0)
					return false;
				return !nodeHasTransformChain(node_id, nodes, nodeCount);
			}

			static std::uint8_t GEA_RENDER_HOT_SRAM effectiveAlphaForNode(int node_id, Node *nodes, int nodeCount)
			{
				int chain[kScratchDepth]{};
				int count = 0;
				for (int cursor = node_id; cursor >= 0 && cursor < nodeCount; cursor = nodes[cursor].parent)
				{
					if (count >= kScratchDepth)
						return 255;
					chain[count++] = cursor;
				}
				int alpha = 255;
				for (int i = count - 1; i >= 0; i--)
				{
					alpha = (alpha * static_cast<int>(nodes[chain[i]].style.opacity)) / 255;
				}
				if (alpha < 0)
					return 0;
				if (alpha > 255)
					return 255;
				return static_cast<std::uint8_t>(alpha);
			}

			static int GEA_RENDER_HOT_SRAM pushNodeReplayClips(int node_id, Node *nodes, int nodeCount)
			{
				int chain[kScratchDepth]{};
				int count = 0;
				for (int cursor = node_id; cursor >= 0 && cursor < nodeCount; cursor = nodes[cursor].parent)
				{
					if (count >= kScratchDepth)
						return -1;
					chain[count++] = cursor;
					if (LayoutEngine::isViewportFixed(nodes[cursor])) break;
				}
				int pushed = 0;
				for (int i = count - 1; i > 0; i--)
				{
					const int id = chain[i];
					if (!nodeRecordsOverflowClip(id, nodes, nodeCount))
						continue;
					const Node &clip = nodes[id];
					int x, y, w, h;
					overflowClipBounds(clip, x, y, w, h);
					gea::platform::display::Display::pushClip(x, y, w, h);
					pushed++;
				}
				return pushed;
			}

				static void replayCommandRange(int start, int end, int x0, int y0, int x1, int y1, UniformRoundedRectBatch &roundedRects)
				{
					if (start < 0 || end <= start || end > state.commandCount)
						return;
					for (int ci = start; ci < end; ci++)
					{
						DisplayCommand *c = &state.commands[ci];
						auto &perf = refreshPerfStatsMutable();
						perf.treeReplayCommandChecks++;
						// SetAlpha is a stateful scope command: it sets the alpha for the
						// following draws in this node's range and is restored by a matching
						// SetAlpha at the end of the range. It must be applied regardless of
					// region overlap — DisplayCommandDrawer (replayInner) has NO SetAlpha
					// case, so without this the alpha scope of transformed/gradient content
					// (grid lines, ellipse stroke, semi-transparent cube faces) is silently
					// dropped in direct dirty-region replay and everything renders opaque.
					if (c->type == DisplayCommandType::SetAlpha)
					{
						roundedRects.flush();
							gea::platform::display::Display::setAlpha(c->alpha.alpha);
							continue;
						}
						const std::int64_t filterStartUs = refreshPerfNowUs();
						const bool mayPaint = commandMayPaintRegion(*c, x0, y0, x1, y1);
						perf.treeReplayCommandFilterUs += refreshPerfNowUs() - filterStartUs;
						perf.treeReplayCommandFilterCalls++;
						if (!mayPaint)
							continue;
						if (retainedBackgroundCommandCanBeSkippedInRegion(*c, x0, y0, x1, y1))
							continue;
					if (replayContainedRoundedRectFillInRegion(*c, x0, y0, x1, y1))
						continue;
					if (roundedRects.append(*c))
						continue;
					roundedRects.flush();
					DisplayCommandDrawer::replay(*c);
				}
			}

			static void replaySimpleCommandRangeInRegions(int start, int end, const DisplayReplayRegion *regions, int count, UniformRoundedRectBatch &roundedRects)
			{
				if (!regions || count <= 0)
					return;
				if (start < 0 || end <= start || end > state.commandCount)
					return;
				for (int ci = start; ci < end; ci++)
				{
					DisplayCommand *c = &state.commands[ci];
					refreshPerfStatsMutable().treeReplayCommandChecks++;
					if (c->type == DisplayCommandType::PushClip || c->type == DisplayCommandType::PopClip)
						continue;
					if (!commandOverlapsAnyRegion(*c, regions, count))
						continue;
					if (retainedBackgroundCommandCanBeSkippedInRegions(*c, regions, count))
						continue;
					if (commandContainedInAnyRegion(*c, regions, count))
					{
						if (roundedRects.append(*c))
							continue;
						roundedRects.flush();
						DisplayCommandDrawer::replay(*c);
						continue;
					}
					roundedRects.flush();
					for (int ri = 0; ri < count; ri++)
						replayCommandInRegion(*c, regions[ri]);
				}
			}

			static void refreshSimpleReplayNodeBBoxes(Tree &tree, Node *nodes, int nodeCount)
			{
				// Keep the packed bbox mirror current. The record-time bake is skipped on
				// content-only frames (markNodeDisplayCommandsDirty patches a node's commands
				// without a full re-record), so a moved absolute leaf would carry a stale bbox
				// here and get wrongly AABB-skipped in the chunk it actually occupies.
				static std::uint64_t bboxRebuildSerial = ~0ull;
				const std::uint64_t serial = tree.refreshSerial();
				if (bboxRebuildSerial == serial)
					return;
				bboxRebuildSerial = serial;
				for (int oi = 0; oi < state.drawNodeOrderCount; oi++)
				{
					const int id = state.drawNodeOrder[oi];
					if (id < 0 || id >= nodeCount)
						continue;
					const Node &n = nodes[id];
					int16_t *bb = &state.drawNodeBBox[oi * 4];
					bb[0] = static_cast<int16_t>(n.layout.x);
					bb[1] = static_cast<int16_t>(n.layout.y);
					bb[2] = static_cast<int16_t>(n.layout.x + n.layout.width - 1);
					bb[3] = static_cast<int16_t>(n.layout.y + n.layout.height - 1);
				}
			}

			static bool clipCoversViewport(const DisplayCommand &command, int width, int height)
			{
				return width > 0 && height > 0 &&
							 command.clip.x <= 0 &&
							 command.clip.y <= 0 &&
							 command.clip.x + command.clip.w >= width &&
							 command.clip.y + command.clip.h >= height;
			}

			static bool canUseSimpleDirtyReplay(int width, int height)
			{
				if (DisplayList::instance().hasTextClippedBackgrounds()) return false;
#if GEA_EMBEDDED_SIMPLE_REPLAY_DEBUG
				static int dbgN = 0;
				const bool dbg = (dbgN++ % 600) == 0;
#define SRDBG(reason, val) do { if (dbg) std::printf("[simple-replay] DISABLED: %s (%d)\n", reason, (int)(val)); } while (0)
#else
#define SRDBG(reason, val) do {} while (0)
#endif
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				const int nodeCount = tree.nodeCount();
				for (int i = 0; i < nodeCount; i++)
				{
					const Node &node = nodes[i];
					if (rstyle(node.style).filter_blur_radius > 0)
						{ SRDBG("blur node", i); return false; }
					if (node.style.opacity != 255 && node.style.opacity != 0)
						{ SRDBG("opacity node", i); return false; }
					if (node.style.mask_right_fade_width > 0)
						{ SRDBG("mask node", i); return false; }
					if (hasRetainedTransformState(node))
						{ SRDBG("transform node", i); return false; }
				}
				for (int ci = 0; ci < state.commandCount; ci++)
				{
					const DisplayCommand &command = state.commands[ci];
					if (command.type == DisplayCommandType::SetAlpha ||
							command.type == DisplayCommandType::BeginFilterBlur ||
							command.type == DisplayCommandType::ApplyFilterBlur)
						{ SRDBG("alpha/blur cmd type", (int)command.type); return false; }
					if (command.type == DisplayCommandType::PushClip &&
							!clipCoversViewport(command, width, height))
						{ SRDBG("non-viewport clip cmd", ci); return false; }
				}
				return true;
#undef SRDBG
			}

			static void replaySimpleDirtyRegions(const DisplayReplayRegion *regions, int count)
			{
				UniformRoundedRectBatch roundedRects;
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				restoreRetainedBackgroundsInRegions(regions, count);
				// Occlusion cull (see occlusionCullStartIndex): skip nodes fully covered
				// by a later opaque full-bleed fill in EVERY region.
				int cullStart = 0x7fffffff;
				for (int ri = 0; ri < count && cullStart > 0; ri++)
				{
					if (regions[ri].x0 > regions[ri].x1 || regions[ri].y0 > regions[ri].y1)
						continue;
					const int s = occlusionCullStartIndex(regions[ri].x0, regions[ri].y0,
																								regions[ri].x1, regions[ri].y1,
																								nodes, tree.nodeCount());
					if (s < cullStart)
						cullStart = s;
				}
				if (cullStart == 0x7fffffff)
					cullStart = 0;
				for (int oi = cullStart; oi < state.drawNodeOrderCount; oi++)
				{
					const int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= tree.nodeCount())
						continue;
					if (retainedBackgroundNodeCanBeSkippedInRegions(node_id, regions, count))
						continue;
					const Node &node = nodes[node_id];
					if (node.style.display == 1)
						continue;
					if (node.style.blink_interval_ms > 0 && !node.style.blink_visible)
						continue;
					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					replaySimpleCommandRangeInRegions(start, end, regions, count, roundedRects);
				}
				roundedRects.flush();
			}

		static bool canReplaySimpleDirtyRegions(int width, int height)
		{
			return canUseSimpleDirtyReplay(width, height);
		}

		static int drawOrderIndexForSimpleReplay(int node_id)
		{
			for (int oi = 0; oi < state.drawNodeOrderCount; ++oi)
			{
				if (state.drawNodeOrder[oi] == node_id)
					return oi;
			}
			return -1;
		}

		static void replaySimpleNodeCommandsInRect(int node_id,
																							 int x0,
																							 int y0,
																							 int x1,
																							 int y1,
																							 UniformRoundedRectBatch &roundedRects,
																							 bool requireLayoutOverlap)
		{
			Tree &tree = Tree::instance();
			Node *nodes = tree.nodes();
			const int nodeCount = tree.nodeCount();
			if (node_id < 0 || node_id >= nodeCount)
				return;
			const Node &node = nodes[node_id];
			if (node.style.display == 1)
				return;
			if (node.style.blink_interval_ms > 0 && !node.style.blink_visible)
				return;

			if (requireLayoutOverlap)
			{
				const int nx0 = node.layout.x;
				const int ny0 = node.layout.y;
				const int nx1 = node.layout.x + node.layout.width - 1;
				const int ny1 = node.layout.y + node.layout.height - 1;
				if (!ClipMath::rectsOverlap(nx0, ny0, nx1, ny1, x0, y0, x1, y1))
					return;
			}

			const int start = state.nodeDrawStart[node_id];
			const int end = state.nodeDrawEnd[node_id];
			if (start < 0 || end <= start || end > state.commandCount)
				return;
			for (int ci = start; ci < end; ci++)
			{
				DisplayCommand *c = &state.commands[ci];
				if (c->type == DisplayCommandType::PushClip || c->type == DisplayCommandType::PopClip)
					continue;
				if (!ClipMath::rectsOverlap(c->bx, c->by, c->bx + c->bw - 1, c->by + c->bh - 1, x0, y0, x1, y1))
					continue;
				if (roundedRects.append(*c))
					continue;
				roundedRects.flush();
				DisplayCommandDrawer::replay(*c);
			}
		}

		// Retained JSX scenes without protected background recolor (bouncing-balls-jsx) stay on
			// the lightweight bbox replay path from the June 2026 62fps fix. The temperature-dial
			// recolor path adds retained-background restore/skip work that is unnecessary here.
			static void replaySimpleClippedDirtyRegionFast(int x0, int y0, int x1, int y1)
			{
				// Split instrumentation (gated by refreshPerfNowUs() == 0 when perf off):
				// raster = batched rounded-rect (ball) fills, dyn = other commands
				// (text/badge), bg = the per-node walk + overlap/clip checks.
				const std::int64_t __fastStart = refreshPerfNowUs();
				std::int64_t __rasterUs = 0;
				std::int64_t __dynUs = 0;
				UniformRoundedRectBatch roundedRects;
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				const int nodeCount = tree.nodeCount();
				const int16_t *bboxes = state.drawNodeBBox;
				refreshSimpleReplayNodeBBoxes(tree, nodes, nodeCount);
				// Occlusion cull (see occlusionCullStartIndex): start at the last opaque
				// full-bleed fill covering this region — everything under it is invisible.
				const int cullStart = occlusionCullStartIndex(x0, y0, x1, y1, nodes, nodeCount);
				for (int oi = cullStart; oi < state.drawNodeOrderCount; oi++)
				{
					// Contiguous AABB pre-skip: reject nodes outside this chunk band by
					// reading the packed bbox mirror, NOT the scattered Node struct. Only
					// nodes that actually overlap the chunk pay the Node-struct read below.
					const int16_t *bb = &bboxes[oi * 4];
					if (!ClipMath::rectsOverlap(bb[0], bb[1], bb[2], bb[3], x0, y0, x1, y1))
						continue;

					const int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= nodeCount)
						continue;
					const Node &node = nodes[node_id];
					if (node.style.display == 1)
						continue;
					if (node.style.blink_interval_ms > 0 && !node.style.blink_visible)
						continue;

					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					if (start < 0 || end <= start || end > state.commandCount)
						continue;
					for (int ci = start; ci < end; ci++)
					{
						DisplayCommand *c = &state.commands[ci];
						if (c->type == DisplayCommandType::PushClip || c->type == DisplayCommandType::PopClip)
							continue;
						if (!ClipMath::rectsOverlap(c->bx, c->by, c->bx + c->bw - 1, c->by + c->bh - 1, x0, y0, x1, y1))
							continue;
						if (roundedRects.append(*c))
							continue;
						const std::int64_t __r0 = refreshPerfNowUs();
						roundedRects.flush();
						__rasterUs += refreshPerfNowUs() - __r0;
						// dyn now counts ONLY text commands; fills/other stay in bg so we
						// can tell badge-text cost apart from background FillRect cost.
						const bool __isText = (c->type == DisplayCommandType::DrawText ||
						                       c->type == DisplayCommandType::DrawProjectedText);
						const std::int64_t __d0 = refreshPerfNowUs();
						DisplayCommandDrawer::replay(*c);
						if (__isText)
							__dynUs += refreshPerfNowUs() - __d0;
					}
				}
				const std::int64_t __r1 = refreshPerfNowUs();
				roundedRects.flush();
				__rasterUs += refreshPerfNowUs() - __r1;
				auto &perf = refreshPerfStatsMutable();
				perf.treeReplaySplitRasterUs += __rasterUs;
				perf.treeReplaySplitDynUs += __dynUs;
				perf.treeReplaySplitBgUs += (refreshPerfNowUs() - __fastStart) - __rasterUs - __dynUs;
			}

		static bool simpleReplayNodeInAncestorChain(int node_id, const int *chain, int chainLen)
		{
			for (int i = 0; i < chainLen; ++i)
			{
				if (chain[i] == node_id)
					return true;
			}
			return false;
		}

		static bool simpleOriginReplayCanSkipEarlierNodes(const int *chain,
																										 int chainLen,
																										 int originOrder,
																										 int x0,
																										 int y0,
																										 int x1,
																										 int y1)
		{
			if (!chain || chainLen <= 0 || originOrder < 0)
				return false;
			Tree &tree = Tree::instance();
			Node *nodes = tree.nodes();
			const int nodeCount = tree.nodeCount();
			for (int oi = 0; oi < originOrder; ++oi)
			{
				const int node_id = state.drawNodeOrder[oi];
				if (node_id < 0 || node_id >= nodeCount)
					continue;
				if (simpleReplayNodeInAncestorChain(node_id, chain, chainLen))
					continue;
				const Node &node = nodes[node_id];
				if (node.style.display == 1)
					continue;
				if (node.style.blink_interval_ms > 0 && !node.style.blink_visible)
					continue;
				const int start = state.nodeDrawStart[node_id];
				const int end = state.nodeDrawEnd[node_id];
				if (commandRangeBBoxOverlaps(start, end, x0, y0, x1, y1))
					return false;
			}
			return true;
		}

		static void replaySimpleClippedDirtyRegion(int x0, int y0, int x1, int y1, int origin = -1, bool allowSplit = true)
		{
			refreshPerfStatsMutable().treeReplaySimpleRegionCalls++;
			// Coarse 2-core band split, mirroring replayDirectDirtyRegion's: the
			// per-origin simple replay is the path a single full-screen dirty rect
			// takes every frame (e.g. the bouncing-balls field on a small panel),
			// and without this it always rasterized on one core while the other
			// idled. Both bands replay the same origin walk with disjoint row
			// clips into the same framebuffer via their per-core canvases.
			{
				constexpr int kMinRowsToSplitFrame = GEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS;
				constexpr int kMinPixelsToSplitFrame = GEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS;
				const int regionW = x1 - x0 + 1;
				const int regionH = y1 - y0 + 1;
				if (GEA_EMBEDDED_RENDER_PARALLEL_DIRTY_REPLAY && allowSplit && regionW > 0 && regionH >= kMinRowsToSplitFrame &&
						regionW * regionH >= kMinPixelsToSplitFrame)
				{
					int mainRows = (regionH * gea_render_parallel_main_share_permille()) / 1000;
					if (mainRows < 1)
						mainRows = 1;
					if (mainRows > regionH - 1)
						mainRows = regionH - 1;
					const int mid = y0 + mainRows - 1;
					struct Band
					{
						int x0, x1, origin;
					} band{x0, x1, origin};
					auto run = +[](void *p, int by0, int by1)
					{
						const Band *b = static_cast<const Band *>(p);
						replaySimpleClippedDirtyRegion(b->x0, by0, b->x1, by1, b->origin, /*allowSplit=*/false);
					};
					refreshPerfStatsMutable().treeReplayParallelAttempts++;
					if (gea_render_parallel_submit(run, &band, mid + 1, y1))
					{
						refreshPerfStatsMutable().treeReplayParallelSuccesses++;
						replaySimpleClippedDirtyRegion(x0, y0, x1, mid, origin, /*allowSplit=*/false); // top band, this core
						gea_render_parallel_wait();
						gea_render_parallel_merge_dirty();
						return;
					}
				}
			}
			if (state.retainedBackgroundRecolorCount == 0)
			{
				if (origin >= 0)
				{
					UniformRoundedRectBatch roundedRects;
					Tree &tree = Tree::instance();
					Node *nodes = tree.nodes();
					const int nodeCount = tree.nodeCount();
					if (origin < nodeCount)
					{
						int chain[32];
						int chainLen = 0;
						for (int id = origin; id >= 0 && chainLen < 32; id = nodes[id].parent)
							chain[chainLen++] = id;
						const int originOrder = drawOrderIndexForSimpleReplay(origin);
						if (!simpleOriginReplayCanSkipEarlierNodes(chain, chainLen, originOrder, x0, y0, x1, y1))
						{
							replaySimpleClippedDirtyRegionFast(x0, y0, x1, y1);
							return;
						}
						const std::int64_t __bgStart = refreshPerfNowUs();
						for (int i = chainLen - 1; i >= 0; --i)
							replaySimpleNodeCommandsInRect(chain[i], x0, y0, x1, y1, roundedRects, false);
						refreshPerfStatsMutable().treeReplaySplitBgUs += refreshPerfNowUs() - __bgStart;
						const std::int64_t __dynStart = refreshPerfNowUs();
						if (originOrder >= 0)
						{
							for (int oi = originOrder + 1; oi < state.drawNodeOrderCount; ++oi)
								replaySimpleNodeCommandsInRect(state.drawNodeOrder[oi], x0, y0, x1, y1, roundedRects, true);
						}
						refreshPerfStatsMutable().treeReplaySplitDynUs += refreshPerfNowUs() - __dynStart;
						const std::int64_t __rasterStart = refreshPerfNowUs();
						roundedRects.flush();
						refreshPerfStatsMutable().treeReplaySplitRasterUs += refreshPerfNowUs() - __rasterStart;
						return;
					}
				}
				replaySimpleClippedDirtyRegionFast(x0, y0, x1, y1);
				return;
			}

				UniformRoundedRectBatch roundedRects;
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				const int nodeCount = tree.nodeCount();
				restoreRetainedBackgroundInRegion(x0, y0, x1, y1);
				for (int oi = 0; oi < state.drawNodeOrderCount; oi++)
				{
					const int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= nodeCount)
						continue;
					if (retainedBackgroundNodeCanBeSkippedInRegion(node_id, x0, y0, x1, y1))
						continue;
					const Node &node = nodes[node_id];
					if (node.style.display == 1)
						continue;
					if (node.style.blink_interval_ms > 0 && !node.style.blink_visible)
						continue;

					const int nx0 = node.layout.x;
					const int ny0 = node.layout.y;
					const int nx1 = node.layout.x + node.layout.width - 1;
					const int ny1 = node.layout.y + node.layout.height - 1;
					if (!ClipMath::rectsOverlap(nx0, ny0, nx1, ny1, x0, y0, x1, y1))
						continue;

					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					if (start < 0 || end <= start || end > state.commandCount)
						continue;
					for (int ci = start; ci < end; ci++)
					{
						DisplayCommand *c = &state.commands[ci];
						auto &perf = refreshPerfStatsMutable();
						perf.treeReplayCommandChecks++;
						if (c->type == DisplayCommandType::PushClip || c->type == DisplayCommandType::PopClip)
							continue;
						const std::int64_t filterStartUs = refreshPerfNowUs();
						const bool mayPaint = commandMayPaintRegion(*c, x0, y0, x1, y1);
						perf.treeReplayCommandFilterUs += refreshPerfNowUs() - filterStartUs;
						perf.treeReplayCommandFilterCalls++;
						if (!mayPaint)
							continue;
						if (retainedBackgroundCommandCanBeSkippedInRegion(*c, x0, y0, x1, y1))
							continue;
						if (roundedRects.append(*c))
							continue;
						roundedRects.flush();
						DisplayCommandDrawer::replay(*c);
					}
				}
				roundedRects.flush();
			}

			static void GEA_RENDER_HOT_SRAM replayNodeCommandRange(int node_id, int x0, int y0, int x1, int y1, UniformRoundedRectBatch &roundedRects)
			{
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				const int nodeCount = tree.nodeCount();
				if (node_id < 0 || node_id >= nodeCount)
					return;
				if (retainedBackgroundNodeCanBeSkippedInRegion(node_id, x0, y0, x1, y1))
					return;
				if (!state.hasNodeScratchFor(node_id))
					return;
				const int start = state.nodeDrawStart[node_id];
				const int end = state.nodeDrawEnd[node_id];
				if (start < 0 || end <= start || end > state.commandCount)
					return;

				const int pushedClips = pushNodeReplayClips(node_id, nodes, nodeCount);
				if (pushedClips < 0)
					return;
				const std::uint8_t alpha = effectiveAlphaForNode(node_id, nodes, nodeCount);
				const bool stateChanged = pushedClips > 0 || alpha != 255;
				if (stateChanged)
				{
					roundedRects.flush();
					gea::platform::display::Display::setAlpha(alpha);
				}
				replayCommandRange(start, end, x0, y0, x1, y1, roundedRects);
				if (stateChanged)
					roundedRects.flush();
				for (int i = 0; i < pushedClips; i++)
					gea::platform::display::Display::popClip();
				if (stateChanged)
					gea::platform::display::Display::setAlpha(255);
			}

			static bool nodeInList(int node_id, const int *nodes, int count)
			{
				for (int i = 0; i < count; i++)
				{
					if (nodes[i] == node_id)
						return true;
				}
				return false;
			}

			static bool originCanOverpaint(int origin)
			{
				auto &state = treeState();
				return origin >= 0 && origin < state.nodeCount && state.nodeCommandDirtyCanOverpaint[origin] != 0;
			}

			static int drawOrderIndexForNode(int node_id)
			{
				for (int oi = 0; oi < state.drawNodeOrderCount; ++oi)
				{
					if (state.drawNodeOrder[oi] == node_id)
						return oi;
				}
				return -1;
			}

			static bool replayOverpaintOriginDirtyRegion(int x0, int y0, int x1, int y1, int origin)
			{
				if (!originCanOverpaint(origin))
					return false;
				Tree &tree = Tree::instance();
				if (origin < 0 || origin >= tree.nodeCount())
					return false;
				const int originOrder = drawOrderIndexForNode(origin);
				if (originOrder < 0)
					return false;
				Node *nodes = tree.nodes();
				UniformRoundedRectBatch roundedRects;
				gea::platform::display::Display::pushClip(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
				replayNodeCommandRange(origin, x0, y0, x1, y1, roundedRects);
				for (int oi = originOrder + 1; oi < state.drawNodeOrderCount; ++oi)
				{
					const int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= tree.nodeCount())
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
						continue;
					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					if (!commandRangeBBoxOverlaps(start, end, x0, y0, x1, y1))
						continue;
					replayNodeCommandRange(node_id, x0, y0, x1, y1, roundedRects);
				}
				roundedRects.flush();
				gea::platform::display::Display::popClip();
				return true;
			}

			static bool replayOriginDirtyRegion(int x0, int y0, int x1, int y1, int origin)
			{
				Tree &tree = Tree::instance();
				if (origin < 0 || origin >= tree.nodeCount())
					return false;
				if (replayOverpaintOriginDirtyRegion(x0, y0, x1, y1, origin))
					return true;
				Node *nodes = tree.nodes();

				int ancestors[kScratchDepth]{};
				int ancestorCount = 0;
				for (int node_id = origin; node_id >= 0 && node_id < tree.nodeCount(); node_id = nodes[node_id].parent)
				{
					if (ancestorCount >= kScratchDepth)
						return false;
					ancestors[ancestorCount++] = node_id;
				}
				if (ancestorCount <= 0)
					return false;

				// Clip every replayed command to the dirty rect. Ancestor nodes (e.g. the
				// root/app background fill) are replayed unconditionally and their bbox can
				// span the whole viewport; without this clip that fill repaints the entire
				// framebuffer and erases sibling content (e.g. tilt-breakout bricks) that
				// doesn't overlap this single dirty region. Mirrors the per-command clip in
				// the multi-region path (replayCommandInRegion).
				UniformRoundedRectBatch roundedRects;
				gea::platform::display::Display::pushClip(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
				// Static-backdrop cache: blit the baked backdrop (bg gradient + stage + the
				// dithered floor) over the region, then replay only the dynamic subtree (the
				// cube) on top. Static nodes are skipped — they're already in the blit.
				const bool useBackdrop = gBackdropCacheValid && !gBackdropBaking && blitStaticBackdrop(x0, y0, x1, y1);
				if (!useBackdrop)
					restoreRetainedBackgroundInRegion(x0, y0, x1, y1);
				const int cullStart = occlusionCullStartIndex(x0, y0, x1, y1, nodes, tree.nodeCount());
				for (int oi = cullStart; oi < state.drawNodeOrderCount; oi++)
				{
					int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= tree.nodeCount())
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
						continue;
					// Skip only nodes whose pixels the backdrop blit actually covers (baked at
					// bake time). Dirty nodes and bake-EXCLUDED nodes (recently-changed ticker
					// leaves, nodeBackdropCooldown) are NOT in the cache — skipping them here
					// erases them whenever their region is repainted (vanishing fps badge /
					// title until the first rebake).
					if (useBackdrop && node_id < kMaxNodes && treeState().nodeInBackdrop[node_id] &&
							!nodeOrAncestorRenderDirty(node_id, nodes, tree.nodeCount()))
						continue;

					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					const bool isAncestor = nodeInList(node_id, ancestors, ancestorCount);
					if (!isAncestor && !commandRangeBBoxOverlaps(start, end, x0, y0, x1, y1))
						continue;
					replayNodeCommandRange(node_id, x0, y0, x1, y1, roundedRects);
				}
				roundedRects.flush();
				gea::platform::display::Display::popClip();
				return true;
			}

			// Render every static (non-dirty) node full-viewport into the gea_bg_cache
			// buffer by temporarily rebinding the canvas to it. Safe w.r.t. the panel flush:
			// flush() reads the swapped-out buffer (justDrawn) directly, not canvas_, so
			// rebinding canvas_ here doesn't disturb an in-flight flush. One-time (per stable
			// scene); the dirty-region replay then blits this instead of re-rendering it.
			static void bakeStaticBackdrop(int width, int height)
			{
				auto *canvas = gea::platform::display::Display::canvas();
				if (!canvas || !canvas->pixels() || width <= 0 || height <= 0)
					return;
				int cap = 0;
				gea::framework::graphics::pixel::native_t *bg = gea_backdrop_cache(&cap);
				if (!bg || cap < width * height)
					return;
				const std::int64_t bakeStartUs = refreshPerfNowUs();
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();

				gea::framework::graphics::pixel::native_t *savedPixels = canvas->pixels();
				const int savedW = canvas->width();
				const int savedH = canvas->height();
				const int savedStride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
				const std::uint8_t savedAlpha = gea::platform::display::Display::alpha();

				gBackdropBaking = true;
				canvas->bindPixels(bg, width, height, width);
				canvas->resetDirty();
				gea::platform::display::Display::resetClip();
				gea::platform::display::Display::setAlpha(255);
				UniformRoundedRectBatch roundedRects;
				gea::platform::display::Display::pushClip(0, 0, width, height);
				auto &treeSt = treeState();
				for (int i = 0; i < tree.nodeCount() && i < kMaxNodes; i++)
					treeSt.nodeInBackdrop[i] = 0;
				for (int oi = 0; oi < state.drawNodeOrderCount; oi++)
				{
					int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= tree.nodeCount())
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
						continue;
					if (nodeOrAncestorRenderDirty(node_id, nodes, tree.nodeCount()))
						continue; // dynamic — not baked
					// Recently-changed leaves (ticking text badges) stay out of the bake:
					// they keep updating through the dirty-region replay, so baking their
					// pixels would force a cache drop on every tick.
					if (node_id < kMaxNodes && treeSt.nodeBackdropCooldown[node_id] > 0)
						continue;
					replayNodeCommandRange(node_id, 0, 0, width - 1, height - 1, roundedRects);
					if (node_id < kMaxNodes)
						treeSt.nodeInBackdrop[node_id] = 1;
				}
				roundedRects.flush();
				gea::platform::display::Display::popClip();

				canvas->bindPixels(savedPixels, savedW, savedH, savedStride);
				canvas->resetDirty();
				gea::platform::display::Display::resetClip();
				gea::platform::display::Display::setAlpha(savedAlpha);
				gBackdropBaking = false;
				gBackdropCacheValid = true;
				gBackdropFullSyncPending = true;
				std::printf("[bdrop] bake %dx%d took %lldus\n", width, height,
										static_cast<long long>(refreshPerfNowUs() - bakeStartUs));
			}

			static void GEA_RENDER_HOT_SRAM replayDirectDirtyRegion(int x0, int y0, int x1, int y1, int origin = -1, bool allowSplit = true)
			{
				refreshPerfStatsMutable().treeReplayDirectRegionCalls++;
				// Coarse 2-core split (ONE dispatch/frame). The worker core replays the bottom
				// row-band into its own band-canvas (private clip + dirty over the SAME
				// framebuffer — all DisplayBackend draw methods route through replayCanvas() so
				// they respect the worker's bottom-band clip and dirty); this core does the top
				// band; join; fold the worker's dirty into the primary for the flush.
				constexpr int kMinRowsToSplitFrame = GEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS;
				constexpr int kMinPixelsToSplitFrame = GEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS;
				const int regionW = x1 - x0 + 1;
				const int regionH = y1 - y0 + 1;
				if (GEA_EMBEDDED_RENDER_PARALLEL_DIRTY_REPLAY && allowSplit && regionW > 0 && regionH >= kMinRowsToSplitFrame &&
						regionW * regionH >= kMinPixelsToSplitFrame)
				{
					int mainRows = (regionH * gea_render_parallel_main_share_permille()) / 1000;
					if (mainRows < 1)
						mainRows = 1;
					if (mainRows > regionH - 1)
						mainRows = regionH - 1;
					const int mid = y0 + mainRows - 1;
					struct Band
					{
						int x0, x1, origin;
					} band{x0, x1, origin};
					auto run = +[](void *p, int by0, int by1)
					{
						const Band *b = static_cast<const Band *>(p);
						replayDirectDirtyRegion(b->x0, by0, b->x1, by1, b->origin, /*allowSplit=*/false);
					};
					refreshPerfStatsMutable().treeReplayParallelAttempts++;
					const bool __submitOk = gea_render_parallel_submit(run, &band, mid + 1, y1);
					if (__submitOk)
					{
						refreshPerfStatsMutable().treeReplayParallelSuccesses++;
						replayDirectDirtyRegion(x0, y0, x1, mid, origin, /*allowSplit=*/false); // top band, this core
						gea_render_parallel_wait();
						gea_render_parallel_merge_dirty();
						return;
					}
				}
				// SRAM-staged strip replay (see the gea_replay_stage_* hooks above): raster
				// the region strip-by-strip in internal SRAM instead of read-modify-writing a
				// scanout-contended framebuffer. Each strip seeds from the framebuffer,
				// replays the strip-clamped sub-region, and copies the painted bbox back —
				// byte-identical to the direct path for any scene. Composes with the 2-core
				// band split above: each band re-enters here with allowSplit=false and
				// strip-stages its own rows on its own core.
				const int stageRows = gea_replay_stage_rows();
				if (stageRows > 0)
				{
					for (int sy0 = y0; sy0 <= y1; sy0 += stageRows)
					{
						const int sy1 = std::min(y1, sy0 + stageRows - 1);
						if (!gea_replay_stage_begin(x0, sy0, x1, sy1))
						{
							// Stage unavailable (no strip buffer / clamp emptied the window):
							// finish the remaining rows directly on the framebuffer.
							replayDirectDirtyRegionBody(x0, sy0, x1, y1, origin);
							return;
						}
						replayDirectDirtyRegionBody(x0, sy0, x1, sy1, origin);
						gea_replay_stage_end();
					}
					return;
				}
				replayDirectDirtyRegionBody(x0, y0, x1, y1, origin);
			}

			// Does some command paint the WHOLE region opaquely, so that replaying the
			// region rebuilds every pixel in it from the bottom up?
			//
			// Tree::mount() clears the framebuffer and then paints the display list on top,
			// so the base under any pixel the list does not paint is the clear colour. That
			// is fine for a pixel no command touches -- nothing ever repaints it. It is NOT
			// fine for an ANTIALIASED EDGE lying on unpainted background: replaying its
			// region blends the edge over the previous frame's blend instead of over the
			// clear colour, and the edge creeps a little further outward on every replay.
			// (Measured on the temperature dial, whose root has no background: replaying the
			// whole panel as a dirty region over a canvas that already held a full replay
			// moved 892 pixels on the dial circle's perimeter; from a cleared canvas the same
			// replay was pixel-identical to the full replay.)
			//
			// Walk forward: a root background is the first thing drawn, so the common case
			// answers on the first node.
			static bool GEA_RENDER_HOT_SRAM regionHasOpaqueBase(int x0, int y0, int x1, int y1, Node *nodes, int nodeCount)
			{
				for (int oi = 0; oi < state.drawNodeOrderCount; ++oi)
				{
					const int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= nodeCount)
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
						continue;
					if (!state.hasNodeScratchFor(node_id))
						continue;
					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					if (start < 0 || end <= start || end > state.commandCount)
						continue;
					bool covers = false;
					for (int ci = start; ci < end && !covers; ++ci)
					{
						const DisplayCommand &c = state.commands[ci];
						if (c.type == DisplayCommandType::FillRect)
							covers = regionInsideRetainedBackground(c, x0, y0, x1, y1);
						else if (c.type == DisplayCommandType::FillRoundedRect)
							// One pixel of slack on every side: a pixel whose centre falls inside
							// the shape can still be an antialiased edge pixel.
							covers = regionInsideRetainedBackground(c, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
					}
					if (!covers)
						continue;
					if (effectiveAlphaForNode(node_id, nodes, nodeCount) != 255)
						continue;
					// An ancestor overflow clip smaller than the region shrinks what the
					// command actually paints (same guard the occlusion cull uses).
					bool clipped = false;
					for (int cursor = node_id; cursor >= 0 && cursor < nodeCount; cursor = nodes[cursor].parent)
					{
						if (!nodeRecordsOverflowClip(cursor, nodes, nodeCount))
							continue;
						const Node &clip = nodes[cursor];
						if (clip.layout.x > x0 || clip.layout.y > y0 ||
								clip.layout.x + clip.layout.width - 1 < x1 ||
								clip.layout.y + clip.layout.height - 1 < y1)
						{
							clipped = true;
							break;
						}
					}
					if (clipped)
						continue;
					return true;
				}
				return false;
			}

			// Re-establish the mount-time clear under a region the display list does not
			// fully paint, so a replay of that region reproduces the full replay instead of
			// re-blending antialiased edges over their own previous blend. Display::clear()
			// leaves 0 on every backend (see each target's clearNoFlush), and Tree::mount()
			// is the only place it runs. HTML documents instead use the white UA
			// canvas recorded below their root background.
			static void GEA_RENDER_HOT_SRAM restoreClearBaseInRegion(int x0, int y0, int x1, int y1)
			{
				if (x0 > x1 || y0 > y1)
					return;
				Tree &tree = Tree::instance();
				if (regionHasOpaqueBase(x0, y0, x1, y1, tree.nodes(), tree.nodeCount()))
					return;
				const int root = tree.mountedRoot();
				const auto base = root >= 0 && root < tree.nodeCount() && isDocumentCanvasRoot(tree.nodes()[root])
				    ? gea::framework::graphics::pixel::nativeColor(255, 255, 255) : 0;
				gea::platform::display::Display::fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, base);
			}

			// Painter's-algorithm occlusion cull: the LAST node (in draw order) whose
			// first command is an opaque FillRect fully covering the region occludes
			// everything drawn before it — the replay walk can start there. A page-sized
			// UI (e-reader page turn) stacks several full-bleed paper fills under the
			// content; each is a full PSRAM write of the region, so skipping the covered
			// ones removes ~100 ms/turn on a 540×960 GRAY4 panel. Conservative: the
			// cover must be untransformed (a plain FillRect), at full effective alpha,
			// its node's first command, never blinking, and not shrunk by any ancestor
			// overflow clip smaller than the region.
			static int GEA_RENDER_HOT_SRAM occlusionCullStartIndex(int x0, int y0, int x1, int y1, Node *nodes, int nodeCount)
			{
				for (int oi = state.drawNodeOrderCount - 1; oi > 0; --oi)
				{
					const int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= nodeCount)
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0)
						continue;
					if (!state.hasNodeScratchFor(node_id))
						continue;
					const int start = state.nodeDrawStart[node_id];
					const int end = state.nodeDrawEnd[node_id];
					if (start < 0 || end <= start || end > state.commandCount)
						continue;
					const DisplayCommand &c = state.commands[start];
					if (c.type != DisplayCommandType::FillRect)
						continue;
					if (c.fill.x > x0 || c.fill.y > y0 ||
							c.fill.x + c.fill.w - 1 < x1 || c.fill.y + c.fill.h - 1 < y1)
						continue;
					// The recorded paint bbox must also cover the region (guards against
					// any recording that paints less than the command rect suggests).
					if (c.bx > x0 || c.by > y0 || c.bx + c.bw - 1 < x1 || c.by + c.bh - 1 < y1)
						continue;
					if (effectiveAlphaForNode(node_id, nodes, nodeCount) != 255)
						continue;
					bool clipped = false;
					for (int cursor = node_id; cursor >= 0 && cursor < nodeCount; cursor = nodes[cursor].parent)
					{
						if (!nodeRecordsOverflowClip(cursor, nodes, nodeCount))
							continue;
						const Node &clip = nodes[cursor];
						if (clip.layout.x > x0 || clip.layout.y > y0 ||
								clip.layout.x + clip.layout.width - 1 < x1 ||
								clip.layout.y + clip.layout.height - 1 < y1)
						{
							clipped = true;
							break;
						}
					}
					if (clipped)
						continue;
					return oi;
				}
				return 0;
			}

			static void GEA_RENDER_HOT_SRAM replayDirectDirtyRegionBody(int x0, int y0, int x1, int y1, int origin)
			{
				if (replayOriginDirtyRegion(x0, y0, x1, y1, origin))
					return;

				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				// Same dirty-rect clip as replayOriginDirtyRegion: keep full-viewport
				// command bboxes (e.g. a root background fill) from painting outside the
				// region and erasing non-overlapping siblings.
				UniformRoundedRectBatch roundedRects;
				gea::platform::display::Display::pushClip(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
				// Static-backdrop cache: this is the origin<0 path (e.g. a coalesced
				// full-viewport sync rect, which loses its single ancestor chain). Blit the
				// baked backdrop over the region and replay only the dynamic subtree on top —
				// every static node is already in the blit. Without this, the post-bake full
				// sync would paint ONLY the (skip-static, cube-only) display list and leave the
				// surroundings stale, so per-region blits later show a seam against them.
				const bool useBackdrop = gBackdropCacheValid && !gBackdropBaking && blitStaticBackdrop(x0, y0, x1, y1);
				if (!useBackdrop)
					restoreRetainedBackgroundInRegion(x0, y0, x1, y1);
				const int cullStart = occlusionCullStartIndex(x0, y0, x1, y1, nodes, tree.nodeCount());
				for (int oi = cullStart; oi < state.drawNodeOrderCount; oi++)
				{
					int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= tree.nodeCount())
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
						continue;
					// Skip only nodes whose pixels the backdrop blit actually covers (baked at
					// bake time). Dirty nodes and bake-EXCLUDED nodes (recently-changed ticker
					// leaves, nodeBackdropCooldown) are NOT in the cache — skipping them here
					// erases them whenever their region is repainted (vanishing fps badge /
					// title until the first rebake).
					if (useBackdrop && node_id < kMaxNodes && treeState().nodeInBackdrop[node_id] &&
							!nodeOrAncestorRenderDirty(node_id, nodes, tree.nodeCount()))
						continue;

					replayNodeCommandRange(node_id, x0, y0, x1, y1, roundedRects);
				}
				roundedRects.flush();
				gea::platform::display::Display::popClip();
			}

			static bool commandOverlapsRegion(const DisplayCommand &c, const DisplayReplayRegion &region)
			{
				if (region.x0 > region.x1 || region.y0 > region.y1)
					return false;
				return commandMayPaintRegion(c, region.x0, region.y0, region.x1, region.y1);
			}

			static bool commandContainedInRegion(const DisplayCommand &c, const DisplayReplayRegion &region)
			{
				if (region.x0 > region.x1 || region.y0 > region.y1)
					return false;
				const int x1 = c.bx + c.bw - 1;
				const int y1 = c.by + c.bh - 1;
				return c.bx >= region.x0 && c.by >= region.y0 && x1 <= region.x1 && y1 <= region.y1;
			}

			static bool commandOverlapsAnyRegion(const DisplayCommand &c, const DisplayReplayRegion *regions, int count)
			{
				const std::int64_t t0 = refreshPerfNowUs();
				auto &perf = refreshPerfStatsMutable();
				perf.treeReplayCommandFilterCalls++;
				for (int ri = 0; ri < count; ri++)
				{
					if (commandOverlapsRegion(c, regions[ri]))
					{
						perf.treeReplayCommandFilterUs += refreshPerfNowUs() - t0;
						return true;
					}
				}
				perf.treeReplayCommandFilterUs += refreshPerfNowUs() - t0;
				return false;
			}

			static bool commandContainedInAnyRegion(const DisplayCommand &c, const DisplayReplayRegion *regions, int count)
			{
				const std::int64_t t0 = refreshPerfNowUs();
				auto &perf = refreshPerfStatsMutable();
				perf.treeReplayCommandFilterCalls++;
				for (int ri = 0; ri < count; ri++)
				{
					if (commandContainedInRegion(c, regions[ri]))
					{
						perf.treeReplayCommandFilterUs += refreshPerfNowUs() - t0;
						return true;
					}
				}
				perf.treeReplayCommandFilterUs += refreshPerfNowUs() - t0;
				return false;
			}

			static bool replayFillRectInRegion(const DisplayCommand &c, const DisplayReplayRegion &region)
			{
				if (c.textClipOwner >= 0) return false;
				if (c.type != DisplayCommandType::FillRect || !commandOverlapsRegion(c, region))
					return false;
				int x0 = c.fill.x;
				int y0 = c.fill.y;
				int x1 = c.fill.x + c.fill.w - 1;
				int y1 = c.fill.y + c.fill.h - 1;
				if (x0 < region.x0)
					x0 = region.x0;
				if (y0 < region.y0)
					y0 = region.y0;
				if (x1 > region.x1)
					x1 = region.x1;
				if (y1 > region.y1)
					y1 = region.y1;
				if (x0 > x1 || y0 > y1)
					return true;
				refreshPerfStatsMutable().treeReplayFillRectCommands++;
				gea::platform::display::Display::fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c.fill.color);
				return true;
			}

			static bool replayContainedRoundedRectFillInRegion(const DisplayCommand &c, int x0, int y0, int x1, int y1)
			{
				if (c.textClipOwner >= 0) return false;
				if (c.type != DisplayCommandType::FillRoundedRect)
					return false;
				if (!regionInsideRetainedBackground(c, x0, y0, x1, y1))
					return false;
				refreshPerfStatsMutable().treeReplayRoundedRectCommands++;
				gea::platform::display::Display::fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c.fillRoundedRect.color);
				return true;
			}

			static void replayCommandInRegion(const DisplayCommand &c, const DisplayReplayRegion &region)
			{
				const std::int64_t t0 = refreshPerfNowUs();
				auto &perf = refreshPerfStatsMutable();
				perf.treeReplayCommandClipCalls++;
				if (!commandOverlapsRegion(c, region))
				{
					perf.treeReplayCommandClipUs += refreshPerfNowUs() - t0;
					return;
				}
				if (retainedBackgroundCommandCanBeSkippedInRegion(c, region.x0, region.y0, region.x1, region.y1))
				{
					perf.treeReplayCommandClipUs += refreshPerfNowUs() - t0;
					return;
				}
				if (replayFillRectInRegion(c, region))
				{
					perf.treeReplayCommandClipUs += refreshPerfNowUs() - t0;
					return;
				}
				if (replayContainedRoundedRectFillInRegion(c, region.x0, region.y0, region.x1, region.y1))
				{
					perf.treeReplayCommandClipUs += refreshPerfNowUs() - t0;
					return;
				}
				gea::platform::display::Display::pushClip(region.x0, region.y0,
																									region.x1 - region.x0 + 1, region.y1 - region.y0 + 1);
				DisplayCommandDrawer::replay(c);
				gea::platform::display::Display::popClip();
				perf.treeReplayCommandClipUs += refreshPerfNowUs() - t0;
			}

			static bool commandRangeOverlapsAnyRegion(int start, int end, const DisplayReplayRegion *regions, int count)
			{
				if (!regions || count <= 0)
					return false;
				if (start < 0 || end <= start || end > state.commandCount)
					return false;
				for (int ri = 0; ri < count; ri++)
				{
					const DisplayReplayRegion &region = regions[ri];
					if (region.x0 > region.x1 || region.y0 > region.y1)
						continue;
					if (commandRangeBBoxOverlaps(start, end, region.x0, region.y0, region.x1, region.y1))
						return true;
				}
				return false;
			}

			static void GEA_RENDER_HOT_SRAM replayNodeCommandRangeInRegions(int node_id, const DisplayReplayRegion *regions, int count, UniformRoundedRectBatch &roundedRects)
			{
				ReplayNodeWalkTimer nodeWalkTimer;
				if (!regions || count <= 0)
					return;
				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				const int nodeCount = tree.nodeCount();
				if (node_id < 0 || node_id >= nodeCount)
					return;
				if (!state.hasNodeScratchFor(node_id))
					return;
				const int start = state.nodeDrawStart[node_id];
				const int end = state.nodeDrawEnd[node_id];
				if (!commandRangeOverlapsAnyRegion(start, end, regions, count))
					return;

				const int pushedClips = pushNodeReplayClips(node_id, nodes, nodeCount);
				if (pushedClips < 0)
					return;
				const std::uint8_t alpha = effectiveAlphaForNode(node_id, nodes, nodeCount);
				const bool stateChanged = pushedClips > 0 || alpha != 255;
				bool alphaTouched = false;
				if (stateChanged)
				{
					roundedRects.flush();
					gea::platform::display::Display::setAlpha(alpha);
					alphaTouched = true;
				}
				for (int ci = start; ci < end; ci++)
				{
					DisplayCommand *c = &state.commands[ci];
					refreshPerfStatsMutable().treeReplayCommandChecks++;
					if (c->type == DisplayCommandType::SetAlpha)
					{
						roundedRects.flush();
						gea::platform::display::Display::setAlpha(c->alpha.alpha);
						alphaTouched = true;
						continue;
					}
					if (!commandOverlapsAnyRegion(*c, regions, count))
						continue;
					if (retainedBackgroundCommandCanBeSkippedInRegions(*c, regions, count))
						continue;
					if (commandContainedInAnyRegion(*c, regions, count))
					{
						if (roundedRects.append(*c))
							continue;
						roundedRects.flush();
						DisplayCommandDrawer::replay(*c);
						continue;
					}
					roundedRects.flush();
					for (int ri = 0; ri < count; ri++)
						replayCommandInRegion(*c, regions[ri]);
				}
				if (stateChanged || alphaTouched)
					roundedRects.flush();
				for (int i = 0; i < pushedClips; i++)
					gea::platform::display::Display::popClip();
				if (stateChanged || alphaTouched)
					gea::platform::display::Display::setAlpha(255);
			}

			static void GEA_RENDER_HOT_SRAM replayDirectDirtyRegions(const DisplayReplayRegion *regions, int count, bool allowSplit = true)
			{
				refreshPerfStatsMutable().treeReplayDirectRegionsCalls++;
				if (!regions || count <= 0)
					return;
				auto *canvas = gea::platform::display::Display::canvas();
				const int width = canvas ? canvas->width() : 0;
				const int height = canvas ? canvas->height() : 0;
				if (canUseSimpleDirtyReplay(width, height))
				{
					replaySimpleDirtyRegions(regions, count);
					return;
				}
				if (count == 1)
				{
					replayDirectDirtyRegion(regions[0].x0,
																	regions[0].y0,
																	regions[0].x1,
																	regions[0].y1,
																	regions[0].origin);
					return;
				}
				constexpr int kMaxRegionSplit = 32;
				if (count <= kMaxRegionSplit)
				{
					DisplayReplayRegion normalRegions[kMaxRegionSplit];
					DisplayReplayRegion overpaintRegions[kMaxRegionSplit];
					int normalCount = 0;
					int overpaintCount = 0;
					for (int i = 0; i < count; ++i)
					{
						if (originCanOverpaint(regions[i].origin))
							overpaintRegions[overpaintCount++] = regions[i];
						else
							normalRegions[normalCount++] = regions[i];
					}
					if (overpaintCount == 1)
					{
						if (normalCount > 0)
							replayDirectDirtyRegions(normalRegions, normalCount, allowSplit);
						const auto &region = overpaintRegions[0];
						replayDirectDirtyRegion(region.x0, region.y0, region.x1, region.y1, region.origin);
						return;
					}
				}

				// Coarse 2-core split for the multi-region path (the transformed cube emits
				// several regions). Split the regions' union Y at the midline: the worker core
				// replays every region clipped to the bottom band into its band-canvas (draw
				// methods routed through replayCanvas() → bottom-band clip + dirty) while this
				// core does the top band; join; fold the worker's dirty into the primary. Each
				// region clipped empty (y0>y1) is skipped by the per-region overlap test.
				constexpr int kMaxSplitRegions = 32;
				constexpr int kMinRowsToSplitFrame = GEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS;
				constexpr int kMinPixelsToSplitFrame = GEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS;
				if (GEA_EMBEDDED_RENDER_PARALLEL_DIRTY_REPLAY && allowSplit && count <= kMaxSplitRegions)
				{
					int minY = 0x7fffffff, maxY = -0x7fffffff;
					int totalArea = 0;
					for (int i = 0; i < count; i++)
					{
						if (regions[i].x0 > regions[i].x1 || regions[i].y0 > regions[i].y1)
							continue;
						if (regions[i].y0 < minY)
							minY = regions[i].y0;
						if (regions[i].y1 > maxY)
							maxY = regions[i].y1;
						totalArea += (regions[i].x1 - regions[i].x0 + 1) * (regions[i].y1 - regions[i].y0 + 1);
					}
					if (maxY >= minY && maxY - minY + 1 >= kMinRowsToSplitFrame && totalArea >= kMinPixelsToSplitFrame)
					{
						const int unionH = maxY - minY + 1;
						int mainRows = (unionH * gea_render_parallel_main_share_permille()) / 1000;
						if (mainRows < 1)
							mainRows = 1;
						if (mainRows > unionH - 1)
							mainRows = unionH - 1;
						const int mid = minY + mainRows - 1;
						DisplayReplayRegion topR[kMaxSplitRegions], botR[kMaxSplitRegions];
						for (int i = 0; i < count; i++)
						{
							topR[i] = regions[i];
							if (topR[i].y1 > mid)
								topR[i].y1 = mid;
							botR[i] = regions[i];
							if (botR[i].y0 < mid + 1)
								botR[i].y0 = mid + 1;
						}
						struct Ctx
						{
							const DisplayReplayRegion *regions;
							int count;
						} ctx{botR, count};
						auto run = +[](void *p, int, int)
						{
							const Ctx *c = static_cast<const Ctx *>(p);
							replayDirectDirtyRegions(c->regions, c->count, /*allowSplit=*/false);
						};
						refreshPerfStatsMutable().treeReplayParallelAttempts++;
						if (gea_render_parallel_submit(run, &ctx, mid + 1, maxY))
						{
							refreshPerfStatsMutable().treeReplayParallelSuccesses++;
							replayDirectDirtyRegions(topR, count, /*allowSplit=*/false); // top band, this core
							gea_render_parallel_wait();
							gea_render_parallel_merge_dirty();
							return;
						}
					}
				}

				Tree &tree = Tree::instance();
				Node *nodes = tree.nodes();
				UniformRoundedRectBatch roundedRects;
				// Static-backdrop cache (multi-region path — the transformed cube produces
				// several regions): blit the baked backdrop into each region, then replay only
				// the dynamic subtree on top, same as the single-region path. Without this a
				// record-skipped static scene has nothing to restore the backdrop here, so the
				// previous frame's cube isn't erased (ghosting/dirt around the cube).
				const bool useBackdrop = gBackdropCacheValid && !gBackdropBaking;
				if (useBackdrop)
					for (int ri = 0; ri < count; ri++)
						blitStaticBackdrop(regions[ri].x0, regions[ri].y0, regions[ri].x1, regions[ri].y1);
				else
					restoreRetainedBackgroundsInRegions(regions, count);
				// Occlusion cull across ALL regions: a node may only be skipped if it is
				// occluded in every region, so take the earliest per-region start.
				int cullStart = 0x7fffffff;
				for (int ri = 0; ri < count && cullStart > 0; ri++)
				{
					if (regions[ri].x0 > regions[ri].x1 || regions[ri].y0 > regions[ri].y1)
						continue;
					const int s = occlusionCullStartIndex(regions[ri].x0, regions[ri].y0,
																								regions[ri].x1, regions[ri].y1,
																								nodes, tree.nodeCount());
					if (s < cullStart)
						cullStart = s;
				}
				if (cullStart == 0x7fffffff)
					cullStart = 0;
				for (int oi = cullStart; oi < state.drawNodeOrderCount; oi++)
				{
					int node_id = state.drawNodeOrder[oi];
					if (node_id < 0 || node_id >= tree.nodeCount())
						continue;
					if (!useBackdrop && retainedBackgroundNodeCanBeSkippedInRegions(node_id, regions, count))
						continue;
					Node *n = &nodes[node_id];
					if (n->style.display == 1)
						continue;
					if (n->style.blink_interval_ms > 0 && !n->style.blink_visible)
						continue;
					// Skip only nodes whose pixels the backdrop blit actually covers (baked at
					// bake time). Dirty nodes and bake-EXCLUDED nodes (recently-changed ticker
					// leaves, nodeBackdropCooldown) are NOT in the cache — skipping them here
					// erases them whenever their region is repainted (vanishing fps badge /
					// title until the first rebake).
					if (useBackdrop && node_id < kMaxNodes && treeState().nodeInBackdrop[node_id] &&
							!nodeOrAncestorRenderDirty(node_id, nodes, tree.nodeCount()))
						continue;

					replayNodeCommandRangeInRegions(node_id, regions, count, roundedRects);
				}
				roundedRects.flush();
			}

			static void GEA_RENDER_HOT_SRAM replay()
			{
				int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
				int clip_dirty = 1;
				int clip_stack_depth = 0;
				UniformRoundedRectBatch roundedRects;

				// Occlusion cull against the CALLER's clip (the conservative tree_render
				// path pushes each dirty rect around a full replay): start at the last
				// opaque full-cover fill's first command. Unlike the node walks, this
				// stream carries clip state INLINE, and an orphan PopClip at depth 0
				// pops the caller's region clip — so only cull when the cover has no
				// clipping ancestors (whose Pop would run unmatched), the skipped
				// prefix is Push/Pop balanced, and no filter-blur pair could straddle
				// the cut.
				int startCi = 0;
				{
					int rx0 = 0, ry0 = 0, rx1 = -1, ry1 = -1;
					gea::platform::display::Display::clip(&rx0, &ry0, &rx1, &ry1);
					if (rx1 >= rx0 && ry1 >= ry0)
					{
						// Stream scan with effective-clip tracking: a FillRect at alpha 255
						// whose rect AND the intersected clip in force both cover the region
						// occludes every draw before it. Draws before the cull point are
						// skipped in the main loop below while Push/Pop/SetAlpha still
						// replay, so clip/alpha state stays exact — no balance
						// preconditions needed. Any filter-blur pair aborts the cull (its
						// backdrop capture depends on the skipped draws).
						constexpr int kCullClipDepth = 16;
						int clipX0[kCullClipDepth], clipY0[kCullClipDepth], clipX1[kCullClipDepth], clipY1[kCullClipDepth];
						int depth = 0;
						int curX0 = rx0, curY0 = ry0, curX1 = rx1, curY1 = ry1;
						int alpha = 255;
						for (int i = 0; i < state.commandCount; ++i)
						{
							const DisplayCommand &c = state.commands[i];
							if (c.type == DisplayCommandType::PushClip)
							{
								if (depth >= kCullClipDepth)
								{
									startCi = 0;
									break;
								}
								clipX0[depth] = curX0;
								clipY0[depth] = curY0;
								clipX1[depth] = curX1;
								clipY1[depth] = curY1;
								depth++;
								curX0 = std::max(curX0, static_cast<int>(c.clip.x));
								curY0 = std::max(curY0, static_cast<int>(c.clip.y));
								curX1 = std::min(curX1, static_cast<int>(c.clip.x) + static_cast<int>(c.clip.w) - 1);
								curY1 = std::min(curY1, static_cast<int>(c.clip.y) + static_cast<int>(c.clip.h) - 1);
							}
							else if (c.type == DisplayCommandType::PopClip)
							{
								if (--depth < 0)
								{
									startCi = 0;
									break;
								}
								curX0 = clipX0[depth];
								curY0 = clipY0[depth];
								curX1 = clipX1[depth];
								curY1 = clipY1[depth];
							}
							else if (c.type == DisplayCommandType::SetAlpha)
							{
								alpha = c.alpha.alpha;
							}
							else if (c.type == DisplayCommandType::BeginFilterBlur ||
											 c.type == DisplayCommandType::ApplyFilterBlur)
							{
								startCi = 0;
								break;
							}
							else if (c.type == DisplayCommandType::FillRect && alpha == 255 &&
											 curX0 <= rx0 && curY0 <= ry0 && curX1 >= rx1 && curY1 >= ry1 &&
											 c.fill.x <= rx0 && c.fill.y <= ry0 &&
											 c.fill.x + c.fill.w - 1 >= rx1 && c.fill.y + c.fill.h - 1 >= ry1 &&
											 c.bx <= rx0 && c.by <= ry0 &&
											 c.bx + c.bw - 1 >= rx1 && c.by + c.bh - 1 >= ry1)
							{
								startCi = i;
							}
						}
					}
				}

				for (int i = 0; i < state.commandCount; i++)
				{
					DisplayCommand *c = &state.commands[i];

					// Occlusion cull: draws before the covering fill are invisible; only
					// the state commands (clip/alpha) replay so state stays exact.
					if (i < startCi && c->type != DisplayCommandType::PushClip &&
							c->type != DisplayCommandType::PopClip && c->type != DisplayCommandType::SetAlpha)
						continue;

					switch (c->type)
					{
					case DisplayCommandType::PushClip:
					{
						roundedRects.flush();
						if (clip_dirty)
						{
							gea::platform::display::Display::clip(&cx0, &cy0, &cx1, &cy1);
							clip_dirty = 0;
						}
						int nx0 = c->clip.x;
						int ny0 = c->clip.y;
						int nx1 = c->clip.x + c->clip.w - 1;
						int ny1 = c->clip.y + c->clip.h - 1;
						if (nx0 < cx0)
							nx0 = cx0;
						if (ny0 < cy0)
							ny0 = cy0;
						if (nx1 > cx1)
							nx1 = cx1;
						if (ny1 > cy1)
							ny1 = cy1;

						int changed = nx0 != cx0 || ny0 != cy0 || nx1 != cx1 || ny1 != cy1;
						if (state.ensureReplayClipCapacity(clip_stack_depth + 1))
							state.replayClipPushed[clip_stack_depth++] = (uint8_t)changed;
						if (changed)
						{
							gea::platform::display::Display::pushClip(c->clip.x, c->clip.y, c->clip.w, c->clip.h);
							cx0 = nx0;
							cy0 = ny0;
							cx1 = nx1;
							cy1 = ny1;
						}
						continue;
					}
					case DisplayCommandType::PopClip:
						roundedRects.flush();
						if (clip_stack_depth <= 0 || state.replayClipPushed[--clip_stack_depth])
						{
							gea::platform::display::Display::popClip();
							clip_dirty = 1;
						}
						continue;
					case DisplayCommandType::SetAlpha:
						roundedRects.flush();
						gea::platform::display::Display::setAlpha(c->alpha.alpha);
						continue;
					default:
						break;
					}

					if (clip_dirty)
					{
						gea::platform::display::Display::clip(&cx0, &cy0, &cx1, &cy1);
						clip_dirty = 0;
					}
					if (c->bx > cx1 || c->by > cy1 ||
							c->bx + c->bw - 1 < cx0 || c->by + c->bh - 1 < cy0)
						continue;

					if (roundedRects.append(*c))
						continue;
					roundedRects.flush();
					DisplayCommandDrawer::replay(*c);
				}
				roundedRects.flush();
			}
		};

	} // namespace

	void PaintOrder::sortChildren(int *children, int count, int contextRoot)
	{
		ChildZSorter::sort(children, count, contextRoot);
	}

	bool PaintOrder::isContext(int id)
	{
		const auto *nodes = Tree::instance().nodes();
		const auto &n = nodes[id];
		const bool item = n.parent >= 0 && !isOutOfFlowPosition(n.style.position) &&
		    (nodes[n.parent].style.display == kDisplayFlex || isDisplayGrid(nodes[n.parent].style));
		return n.parent < 0 || n.style.position == kPositionFixed || ChildZSorter::effectGroup(n) ||
		    (!n.style.z_index_auto && (n.style.position != 0 || item));
	}

	bool PaintOrder::isGroup(int id)
	{
		const auto *nodes = Tree::instance().nodes();
		const auto &n = nodes[id];
		const bool item = n.parent >= 0 &&
		    (nodes[n.parent].style.display == kDisplayFlex || isDisplayGrid(nodes[n.parent].style));
		return isContext(id) || n.style.position != 0 || n.style.float_side || item;
	}

	std::vector<int> PaintOrder::collectChildren(int root, bool groupRoot, bool includePositioned)
	{
		const auto *nodes = Tree::instance().nodes();
		std::vector<int> result;
		if (!groupRoot) return result; // Ordinary boxes contribute only their own paint.
		// CSS painting follows stacking contexts rather than DOM parentage.
		// Float, flex/grid-item and positioned-auto groups keep their ordinary
		// contents together, while their positioned descendants participate in
		// the enclosing real context. Ordinary block/inline wrappers contribute
		// separate entries so block backgrounds remain below floats and text.
		auto walk = [&](auto &&self, int parent, bool withinGroup) -> void {
			std::vector<int> children;
			for (int child = nodes[parent].first_child; child >= 0; child = nodes[child].next_sibling)
				if (!isDisplayNone(nodes[child].style)) children.push_back(child);
			if (nodes[parent].style.display == kDisplayFlex || isDisplayGrid(nodes[parent].style)) {
				std::stable_sort(children.begin(), children.end(), [&](int a, int b) {
					const int oa = isOutOfFlowPosition(nodes[a].style.position) ? 0 : nodes[a].style.order;
					const int ob = isOutOfFlowPosition(nodes[b].style.position) ? 0 : nodes[b].style.order;
					return oa < ob;
				});
			}
			for (int child : children) {
				const auto &n = nodes[child];
				if (n.style.blink_interval_ms > 0 && !n.style.blink_visible) continue;
				const bool context = isContext(child), positioned = n.style.position != 0 || context;
				const bool group = isGroup(child);
				if (positioned) {
					if (includePositioned) result.push_back(child);
					if (!context && includePositioned) self(self, child, true);
				} else {
					if (!withinGroup) result.push_back(child);
					self(self, child, withinGroup || group);
				}
			}
		};
		walk(walk, root, false);
		sortChildren(result.data(), static_cast<int>(result.size()), root);
		return result;
	}

	int PaintOrder::compareNodes(int first, int second)
	{
		// This is also the recorder's traversal, so retained draws cannot invent a
		// second hierarchy or lose document-order ties after a depth crossing.
		static int ranks[kMaxNodes];
		static uint64_t serial = ~0ull;
		static uint32_t recordSerial = 0;
		auto &tree = Tree::instance();
		if (serial != tree.refreshSerial() || recordSerial != state.displayListSerial) {
			std::fill(std::begin(ranks), std::end(ranks), -1);
			int next = 0;
			auto visit = [&](auto &&self, int node, bool groupRoot, bool positioned) -> void {
				ranks[node] = next++;
				for (int child : collectChildren(node, groupRoot, positioned))
					self(self, child, isGroup(child), isContext(child));
			};
			for (int id = 0; id < tree.nodeCount(); ++id)
				if (tree.nodes()[id].parent < 0) visit(visit, id, true, true);
			serial = tree.refreshSerial(); recordSerial = state.displayListSerial;
		}
		return ranks[first] - ranks[second];
	}

	DisplayList &DisplayList::instance()
	{
		static DisplayList displayList;
		return displayList;
	}

	static int recordingTextClipOwner = -1;

	int DisplayList::setRecordingTextClipOwner(int owner)
	{
		const int previous = recordingTextClipOwner;
		recordingTextClipOwner = owner;
		return previous;
	}

	DisplayCommand *DisplayList::append()
	{
		if (recordingTextClipOwner >= 0) state.textClippedBackgrounds = true;
		if (state.appendOverride)
		{
			if (!state.appendOverrideCount || *state.appendOverrideCount >= state.appendOverrideCapacity)
				return nullptr;
			auto *command = &state.appendOverride[(*state.appendOverrideCount)++];
			command->textClipOwner = recordingTextClipOwner;
			command->textDecorationInk = false;
			return command;
		}
		if (!state.commands)
		{
			int fastCapacity = 0;
			void *fast = gea_display_command_buffer(&fastCapacity,
			                                        static_cast<int>(sizeof(DisplayCommand)),
			                                        static_cast<int>(alignof(DisplayCommand)));
			if (fast && fastCapacity > 0)
			{
				state.commands = static_cast<DisplayCommand *>(fast);
				state.commandCapacity = std::min(fastCapacity, DisplayListState::kMaxCommands);
				state.commandsExternal = true;
			}
		}
		if (!state.commands)
		{
			state.commands = static_cast<DisplayCommand *>(state.scratchAlloc(sizeof(DisplayCommand) * DisplayListState::kInitialCommands));
			state.commandCapacity = state.commands ? DisplayListState::kInitialCommands : 0;
			state.commandsExternal = false;
		}
		if (!state.commands)
			return nullptr;
		// Grow on demand (double up to kMaxCommands) when the current buffer fills.
		// Handles both cases: an external/fast board buffer that overflowed (copy into
		// a new owned buffer) and an owned PSRAM buffer (realloc bigger). Retained
		// across frames — clear() keeps the buffer — so growth only happens while the
		// UI's peak command count climbs, then stops.
		if (state.commandCount >= state.commandCapacity && state.commandCapacity < DisplayListState::kMaxCommands)
		{
			int nextCapacity = state.commandCapacity > 0 ? state.commandCapacity * 2 : DisplayListState::kInitialCommands;
			if (nextCapacity > DisplayListState::kMaxCommands)
				nextCapacity = DisplayListState::kMaxCommands;
			DisplayCommand *grown = nullptr;
			if (state.commandsExternal)
			{
				grown = static_cast<DisplayCommand *>(state.scratchAlloc(sizeof(DisplayCommand) * static_cast<size_t>(nextCapacity)));
				if (grown)
					std::memcpy(grown, state.commands, sizeof(DisplayCommand) * static_cast<std::size_t>(state.commandCount));
			}
			else
			{
				grown = static_cast<DisplayCommand *>(state.scratchRealloc(state.commands, sizeof(DisplayCommand) * static_cast<size_t>(nextCapacity)));
			}
			if (!grown)
			{
				state.commandOverflow = true;
				return nullptr;
			}
			state.commands = grown;
			state.commandCapacity = nextCapacity;
			state.commandsExternal = false;
		}
		if (state.commandCount >= state.commandCapacity)
		{
			state.commandOverflow = true;
			return nullptr;
		}
		auto *command = &state.commands[state.commandCount++];
		command->textClipOwner = recordingTextClipOwner;
		command->textDecorationInk = false;
		return command;
	}

	void DisplayList::clear()
	{
		Tree &tree = Tree::instance();
		state.displayListSerial++;
		if (state.displayListSerial == 0)
			state.displayListSerial = 1;
		state.filterBlurCacheHits = 0;
		state.filterBlurCacheMisses = 0;
		state.commandCount = 0;
		state.textClippedBackgrounds = false;
		state.commandOverflow = false;
		state.drawNodeOrderCount = 0;
		if (!state.ensureNodeScratchCapacity(tree.nodeCount()))
			return;
		for (int i = 0; i < tree.nodeCount(); i++)
		{
			state.nodeDrawStart[i] = -1;
			state.nodeDrawEnd[i] = -1;
		}
	}

	void DisplayList::resetStorage()
	{
		state.resetStorage();
		invalidateStaticBackdrop();
	}

	void DisplayList::armTransformReproject(bool eligible)
	{
		state.reprojectSerial = eligible ? state.displayListSerial : 0;
	}

	bool DisplayList::reprojectDirtyRect(int *x0, int *y0, int *x1, int *y1) const
	{
		if (state.reprojectDirtyX1 < state.reprojectDirtyX0 || state.reprojectDirtyY1 < state.reprojectDirtyY0)
			return false;
		if (x0)
			*x0 = state.reprojectDirtyX0;
		if (y0)
			*y0 = state.reprojectDirtyY0;
		if (x1)
			*x1 = state.reprojectDirtyX1;
		if (y1)
			*y1 = state.reprojectDirtyY1;
		return true;
	}

	// ---- Watertight vertex weld ----
	// Adjacent transformed-gradient faces project their shared 3D edge through
	// independent transform chains. Measured on the cube, the two projections of a
	// shared vertex disagree by ~0.3-0.5px — far above float rounding (the 3D points
	// are bit-coincident through the face transforms), so it's an inherent artifact of
	// projecting the same vertex two ways. The int16 + 1/8-px corners then land a
	// fraction of a pixel apart and the backdrop shows through as a shimmering crack
	// along shared edges (worst on near-horizontal edges; the 1/8-px grid only closed
	// edges whose disagreement happened to round into one cell). Those vertices ARE
	// geometrically coincident, so snap any tight cluster of projected corners to its
	// shared centroid: faces then tile watertight. Truthful (welds coincident points,
	// no silhouette bloat, no AA), general (any touching-quad mesh), cheap (corners are
	// few — the dynamic subtree's gradient faces). Threshold 1.5px welds only
	// true-coincident vertices; distinct geometry projects far further apart, and the
	// degenerate edge-on case (two real vertices foreshortened together) is a zero-area
	// sliver where merging is visually harmless.
	// MUST run after every path that (re)creates face commands — tryReprojectTransformed
	// calls it after patching, and tree_render calls it after each full record. A full
	// record that skips the weld paints unwelded faces for that one frame: the shared
	// edges crack open and the backdrop flashes through until the next reproject.
	// The dirty pointers, when non-null, are extended to cover the welded geometry.
	void DisplayList::weldTransformedFaces(int *dirtyX0, int *dirtyY0, int *dirtyX1, int *dirtyY1)
	{
		static constexpr int kMaxWeld = 256; // 64 gradient faces; beyond this the tail is left un-welded
		int16_t *fxr[kMaxWeld], *fyr[kMaxWeld], *xir[kMaxWeld], *yir[kMaxWeld];
		int wn = 0;
		// Only weld faces in the DYNAMIC (transform-dirty) subtree — the animated cube's
		// faces, whose shared edges crack and need coincident-vertex welding. A statically
		// transformed decor node (the perspective stage floor/wall/horizon) must NOT be pulled
		// into a weld cluster: at some cube rotations a cube-face corner passes within the 1.5px
		// weld threshold of a static corner, snapping that STATIC corner onto the cube and
		// warping it. Live that warp is invisible (every frame re-projects the static corners,
		// undoing it) but it becomes PERMANENT once the static scene is baked into the backdrop
		// — the "baked base-plate gradient looks wrong until you toggle" bug (the toggle re-bakes
		// at a rotation with no coincidence). Restricting to the dynamic subtree matches the
		// weld's intent (close the cube's own cracks) and leaves the static decor untouched.
		Tree &weldTree = Tree::instance();
		Node *weldNodes = weldTree.nodes();
		const int weldNodeCount = weldTree.nodeCount();
		for (int id = 0; id < weldNodeCount && wn + 4 <= kMaxWeld; id++)
		{
			bool nodeInDynamicSubtree = false;
			int weldSteps = 0;
			for (int a = id; a >= 0 && a < weldNodeCount; a = weldNodes[a].parent)
			{
				// A parent chain longer than the node count can only be a cycle; walking
				// it unbounded wedges the frame task until the watchdog fires.
				if (++weldSteps > weldNodeCount)
				{
					static unsigned weldCycleHits = 0;
					if ((weldCycleHits++ & 0x3fu) == 0)
					{
						std::printf("[weld] parent cycle from node=%d count=%d chain:", id, weldNodeCount);
						int c = id;
						for (int n = 0; n < 24 && c >= 0 && c < weldNodeCount; n++, c = weldNodes[c].parent)
							std::printf(" %d", c);
						std::printf("\n");
					}
					break;
				}
				if (weldNodes[a].render.transform_dirty)
				{
					nodeInDynamicSubtree = true;
					break;
				}
			}
			if (!nodeInDynamicSubtree)
				continue;
			const int start = state.nodeDrawStart[id];
			const int end = state.nodeDrawEnd[id];
			if (start < 0 || end <= start)
				continue;
			for (int ci = start; ci < end && ci < state.commandCount && wn + 4 <= kMaxWeld; ci++)
			{
				DisplayCommand &c = state.commands[ci];
				if (c.type != DisplayCommandType::FillTransformedLinearGradient)
					continue;
				auto &g = c.transformedGradient;
				int16_t *fxs[4] = {&g.fx0, &g.fx1, &g.fx2, &g.fx3};
				int16_t *fys[4] = {&g.fy0, &g.fy1, &g.fy2, &g.fy3};
				int16_t *xis[4] = {&g.x0, &g.x1, &g.x2, &g.x3};
				int16_t *yis[4] = {&g.y0, &g.y1, &g.y2, &g.y3};
				for (int k = 0; k < 4; k++)
				{
					fxr[wn] = fxs[k];
					fyr[wn] = fys[k];
					xir[wn] = xis[k];
					yir[wn] = yis[k];
					wn++;
				}
			}
		}
		if (wn == 0)
			return;
		constexpr int kWeldThresh8 = 12; // 1.5px, in 1/8-px units
		for (int i = 0; i < wn; i++)
		{
			if (fxr[i] == nullptr)
				continue; // already consumed into an earlier cluster
			long sx = *fxr[i], sy = *fyr[i];
			int grp[kMaxWeld];
			int gc = 0;
			grp[gc++] = i;
			const int ax = *fxr[i], ay = *fyr[i];
			for (int j = i + 1; j < wn; j++)
			{
				if (fxr[j] == nullptr)
					continue;
				const int dx = *fxr[j] - ax, dy = *fyr[j] - ay;
				if ((dx < 0 ? -dx : dx) <= kWeldThresh8 && (dy < 0 ? -dy : dy) <= kWeldThresh8)
				{
					sx += *fxr[j];
					sy += *fyr[j];
					grp[gc++] = j;
				}
			}
			if (gc < 2)
				continue; // lone corner — nothing coincident to weld
			const int16_t cx = static_cast<int16_t>(std::lround(static_cast<double>(sx) / gc));
			const int16_t cy = static_cast<int16_t>(std::lround(static_cast<double>(sy) / gc));
			const int16_t ix = static_cast<int16_t>(std::lround(cx / 8.0));
			const int16_t iy = static_cast<int16_t>(std::lround(cy / 8.0));
			for (int m = 0; m < gc; m++)
			{
				const int idx = grp[m];
				*fxr[idx] = cx;
				*fyr[idx] = cy;
				*xir[idx] = ix;
				*yir[idx] = iy;
				fxr[idx] = nullptr; // mark consumed
			}
		}
		// Welding moved corners up to ~0.5px; the drawer clips the fill to the command
		// bbox, so recompute each gradient command's bbox from its welded int16 corners
		// (else a shared edge that moved outward gets clipped — re-cracking it) and
		// extend the caller's dirty union to cover the welded geometry.
		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand &c = state.commands[ci];
			if (c.type != DisplayCommandType::FillTransformedLinearGradient)
				continue;
			auto &g = c.transformedGradient;
			const int16_t xx[4] = {g.x0, g.x1, g.x2, g.x3};
			const int16_t yy[4] = {g.y0, g.y1, g.y2, g.y3};
			int bx0 = xx[0], by0 = yy[0], bx1 = xx[0], by1 = yy[0];
			for (int k = 1; k < 4; k++)
			{
				if (xx[k] < bx0)
					bx0 = xx[k];
				if (xx[k] > bx1)
					bx1 = xx[k];
				if (yy[k] < by0)
					by0 = yy[k];
				if (yy[k] > by1)
					by1 = yy[k];
			}
			// Only extend the caller's dirty union for a command whose bbox actually
			// CHANGED from welding. A statically-transformed gradient (e.g. the css-3d-cube's
			// perspective stage floor) has no coincident vertices with the moving cube, so it
			// is never welded and its bbox is unchanged — extending the dirty rect to cover its
			// screen-wide perspective trapezoid anyway (the previous behavior) re-inflated the
			// reproject dirty rect to [-263,0,673,730] even after the main loop's movement gate
			// excluded it, which is what kept the dirty-pass reuse path catastrophic. Gating on
			// a real bbox change keeps the reused rect tight (the moving/welded cube only).
			const bool bboxChanged =
					bx0 != c.bx || by0 != c.by ||
					(bx1 - bx0 + 1) != c.bw || (by1 - by0 + 1) != c.bh;
			c.bx = static_cast<int16_t>(bx0);
			c.by = static_cast<int16_t>(by0);
			c.bw = static_cast<int16_t>(bx1 - bx0 + 1);
			c.bh = static_cast<int16_t>(by1 - by0 + 1);
			if (bboxChanged)
			{
				if (dirtyX0 && bx0 < *dirtyX0)
					*dirtyX0 = bx0;
				if (dirtyY0 && by0 < *dirtyY0)
					*dirtyY0 = by0;
				if (dirtyX1 && bx1 > *dirtyX1)
					*dirtyX1 = bx1;
				if (dirtyY1 && by1 > *dirtyY1)
					*dirtyY1 = by1;
			}
		}
	}

	bool DisplayList::tryReprojectTransformed()
	{
		// Only when a prior full record armed this path AND the list hasn't been
		// cleared since (clear() bumps displayListSerial, so a mismatch means the
		// structure may have changed — fall back to a full record).
		if (state.reprojectSerial == 0 || state.reprojectSerial != state.displayListSerial)
		{
#if GEA_RP2350_REPROJECT_DEBUG
			logReprojectReject("serial", static_cast<int>(state.reprojectSerial),
												 static_cast<int>(state.displayListSerial));
#endif
			return false;
		}

		Tree &tree = Tree::instance();
		Node *nodes = tree.nodes();
		const int nodeCount = tree.nodeCount();
		if (!state.ensureNodeScratchCapacity(nodeCount))
		{
#if GEA_RP2350_REPROJECT_DEBUG
			logReprojectReject("scratch", nodeCount);
#endif
			return false;
		}

		// A flat hidden face can remove its entire paint subtree. Rebuild when
		// its visibility changes; an unchanged hidden group needs no commands,
		// and an unchanged visible group can still use the reprojection path.
		// The recorder gives even an empty visible container a valid range.
		for (int i = 0; i < state.drawNodeOrderCount; ++i)
			if (state.drawNodeOrder[i] < 0 || state.drawNodeOrder[i] >= nodeCount) return false;
		for (int id = 0; id < nodeCount; ++id) {
			if (!nodes[id].style.backface_hidden || !ViewRenderer::isTransformableBox(nodes[id]) || preserves3D(nodes[id].style)) continue;
			const bool wasRecorded = state.nodeDrawStart[id] >= 0;
			if (wasRecorded == ViewRenderer::backfaceSubtreeHidden(nodes[id])) return false;
		}

		// Validate first (no partial patching): every recorded command's screen geometry
		// must be recomputable from a stored local rect. Anything else — clips, plain
		// fills/strokes, images, blur, raw text — means the structure isn't a pure
		// transformed-leaf set, so bail to a full rebuild.
		for (int id = 0; id < nodeCount; id++)
		{
			const int start = state.nodeDrawStart[id];
			const int end = state.nodeDrawEnd[id];
			if (start < 0 || end <= start)
				continue;
			for (int ci = start; ci < end && ci < state.commandCount; ci++)
			{
				const DisplayCommandType t = state.commands[ci].type;
				// Arbitrary FillQuad/DrawLine commands store only projected corners, so
				// they cannot be recomputed after a transform change. Opt-in FillQuad
				// commands carry a local rect and can be reprojected below. The other
				// transformed types we can patch (gradient, rounded rect, projected text)
				// and every axis-aligned type are fine.
				if ((t == DisplayCommandType::FillQuad && state.commands[ci].quad.reprojectMode == 0) ||
						t == DisplayCommandType::DrawLine)
				{
#if GEA_RP2350_REPROJECT_DEBUG
					logReprojectReject("command-type", id, ci, static_cast<int>(state.commandCount), t);
#endif
					return false;
				}
			}
		}

		// Patch: re-project each command's corners from its node's CURRENT transform
		// applied to the command's stored local rect. transformedRectCorners + the
		// min/max bbox below are exactly what recordLinearGradientBackground /
		// recordProjectedRasterText do, so the result is identical to a full record.
		// Also accumulate (prev bbox ∪ cur bbox) over the patched (transformed) commands
		// — that union is the subtree's dirty rect, which the dirty pass reuses instead
		// of re-projecting. Only transformed commands count (axis-aligned ones like the
		// root background don't move, and unioning their full-screen bbox would blow the
		// dirty rect up to the whole viewport).
		int rdX0 = 32767, rdY0 = 32767, rdX1 = -32768, rdY1 = -32768;
		for (int id = 0; id < nodeCount; id++)
		{
			const int start = state.nodeDrawStart[id];
			const int end = state.nodeDrawEnd[id];
			if (start < 0 || end <= start)
				continue;
			Node &n = nodes[id];
			// Skip nodes OUTSIDE the dynamic (transform-dirty) subtree entirely. Only the
			// moving cube's transform changed this frame; a statically-transformed node
			// (the css-3d-cube's perspective stage floor/wall/horizon) re-projects to the
			// SAME corners every frame, so patching it is pure waste (~32 of ~45 transformed
			// commands here — the bulk of the reproject's ~3ms 'dlist' cost), and its pixels
			// come from the baked static backdrop, not a display-list replay. transform_dirty
			// is set only on the node whose transform changed (the animated cube container),
			// so "in the dynamic subtree" == has a transform-dirty ancestor-or-self.
			bool nodeInDynamicSubtree = false;
			for (int a = id; a >= 0 && a < nodeCount; a = nodes[a].parent) {
				if (nodes[a].render.transform_dirty) { nodeInDynamicSubtree = true; break; }
			}
			if (!nodeInDynamicSubtree)
				continue;
			for (int ci = start; ci < end && ci < state.commandCount; ci++)
			{
				DisplayCommand &c = state.commands[ci];
				int16_t xs[4], ys[4], fx[4], fy[4];
				if (c.type == DisplayCommandType::FillTransformedLinearGradient)
				{
					ViewRenderer::transformedRectCorners(n, false, c.transformedGradient.lx, c.transformedGradient.ly,
																							 c.transformedGradient.lw, c.transformedGradient.lh, xs, ys, fx, fy);
					c.transformedGradient.x0 = xs[0];
					c.transformedGradient.y0 = ys[0];
					c.transformedGradient.x1 = xs[1];
					c.transformedGradient.y1 = ys[1];
					c.transformedGradient.x2 = xs[2];
					c.transformedGradient.y2 = ys[2];
					c.transformedGradient.x3 = xs[3];
					c.transformedGradient.y3 = ys[3];
					c.transformedGradient.fx0 = fx[0];
					c.transformedGradient.fy0 = fy[0];
					c.transformedGradient.fx1 = fx[1];
					c.transformedGradient.fy1 = fy[1];
					c.transformedGradient.fx2 = fx[2];
					c.transformedGradient.fy2 = fy[2];
					c.transformedGradient.fx3 = fx[3];
					c.transformedGradient.fy3 = fy[3];
				}
				else if (c.type == DisplayCommandType::FillTransformedRoundedRect)
				{
					ViewRenderer::transformedRectCorners(n, false, c.transformedRoundedRect.lx, c.transformedRoundedRect.ly,
																							 c.transformedRoundedRect.lw, c.transformedRoundedRect.lh, xs, ys);
					c.transformedRoundedRect.x0 = xs[0];
					c.transformedRoundedRect.y0 = ys[0];
					c.transformedRoundedRect.x1 = xs[1];
					c.transformedRoundedRect.y1 = ys[1];
					c.transformedRoundedRect.x2 = xs[2];
					c.transformedRoundedRect.y2 = ys[2];
					c.transformedRoundedRect.x3 = xs[3];
					c.transformedRoundedRect.y3 = ys[3];
				}
				else if (c.type == DisplayCommandType::FillQuad && (c.quad.reprojectMode == 1 || c.quad.reprojectMode == 3))
				{
					ViewRenderer::transformedRectCorners(n, false, c.quad.lx, c.quad.ly, c.quad.lw, c.quad.lh, xs, ys);
					if (c.quad.reprojectMode == 3)
					{
						if (ys[2] - ys[1] < 1)
							ys[2] = ys[1] + 1;
						if (ys[3] - ys[0] < 1)
							ys[3] = ys[0] + 1;
						if (xs[1] - xs[0] < 1)
							xs[1] = xs[0] + 1;
						if (xs[2] - xs[3] < 1)
							xs[2] = xs[3] + 1;
					}
					c.quad.x0 = xs[0];
					c.quad.y0 = ys[0];
					c.quad.x1 = xs[1];
					c.quad.y1 = ys[1];
					c.quad.x2 = xs[2];
					c.quad.y2 = ys[2];
					c.quad.x3 = xs[3];
					c.quad.y3 = ys[3];
				}
				else if (c.type == DisplayCommandType::FillQuad && c.quad.reprojectMode == 2)
				{
					int16_t p0x = 0, p0y = 0, p1x = 0, p1y = 0;
					ViewRenderer::transformedPoint(n, false,
																					static_cast<float>(c.quad.lx) * 0.125f,
																					static_cast<float>(c.quad.ly) * 0.125f,
																					0.0f,
																					&p0x,
																					&p0y);
					ViewRenderer::transformedPoint(n, false,
																					static_cast<float>(c.quad.lw) * 0.125f,
																					static_cast<float>(c.quad.lh) * 0.125f,
																					0.0f,
																					&p1x,
																					&p1y);
					strokeSegmentBandCorners(static_cast<float>(p0x),
																	 static_cast<float>(p0y),
																	 static_cast<float>(p1x),
																	 static_cast<float>(p1y),
																	 static_cast<float>(c.quad.aux) * 0.125f,
																	 xs,
																	 ys);
					c.quad.x0 = xs[0];
					c.quad.y0 = ys[0];
					c.quad.x1 = xs[1];
					c.quad.y1 = ys[1];
					c.quad.x2 = xs[2];
					c.quad.y2 = ys[2];
					c.quad.x3 = xs[3];
					c.quad.y3 = ys[3];
				}
				else if (c.type == DisplayCommandType::DrawProjectedText)
				{
					ViewRenderer::transformedRectCorners(n, false, c.projectedText.srcX, c.projectedText.srcY,
																							 c.projectedText.srcW, c.projectedText.srcH, xs, ys);
					c.projectedText.x0 = xs[0];
					c.projectedText.y0 = ys[0];
					c.projectedText.x1 = xs[1];
					c.projectedText.y1 = ys[1];
					c.projectedText.x2 = xs[2];
					c.projectedText.y2 = ys[2];
					c.projectedText.x3 = xs[3];
					c.projectedText.y3 = ys[3];
				}
				else
				{
					continue; // axis-aligned / state command: transform-invariant, leave as-is
				}
				// New (current-frame) projected bbox from the patched corners.
				int bx0 = xs[0], by0 = ys[0], bx1 = xs[0], by1 = ys[0];
				for (int k = 1; k < 4; k++)
				{
					if (xs[k] < bx0)
						bx0 = xs[k];
					if (xs[k] > bx1)
						bx1 = xs[k];
					if (ys[k] < by0)
						by0 = ys[k];
					if (ys[k] > by1)
						by1 = ys[k];
				}
				// Old (previous-frame) bbox = the command's stored bbox, about to be overwritten.
				const int ox1 = c.bx + c.bw - 1, oy1 = c.by + c.bh - 1;
				// Every command that reaches here belongs to the dynamic (transform-dirty)
				// subtree — the whole node was skipped above otherwise — so it always
				// contributes to the reproject dirty rect (prev ∪ cur). Excluding the static
				// decor here is what keeps the reused rect tight (the moving cube only).
				// prev bbox → dirty union.
				if (c.bx < rdX0)
					rdX0 = c.bx;
				if (c.by < rdY0)
					rdY0 = c.by;
				if (ox1 > rdX1)
					rdX1 = ox1;
				if (oy1 > rdY1)
					rdY1 = oy1;
				// cur bbox → dirty union.
				if (bx0 < rdX0)
					rdX0 = bx0;
				if (by0 < rdY0)
					rdY0 = by0;
				if (bx1 > rdX1)
					rdX1 = bx1;
				if (by1 > rdY1)
					rdY1 = by1;
				c.bx = static_cast<int16_t>(bx0);
				c.by = static_cast<int16_t>(by0);
				c.bw = static_cast<int16_t>(bx1 - bx0 + 1);
				c.bh = static_cast<int16_t>(by1 - by0 + 1);
			}
		}
		// Weld coincident face vertices, but do NOT let it extend the reproject dirty rect:
		// its per-command bbox recompute iterates ALL transformed gradients (including the
		// static full-width stage horizon, whose 1px projection jitter would re-inflate the
		// rect to full viewport width). The dynamic-subtree union above already bounds the
		// moving cube, and the ≤1.5px vertex welds are absorbed by the dirty-rect raster
		// guard the tree_render dirty pass adds. (nullptr = skip the weld's dirty extension.)
		weldTransformedFaces(nullptr, nullptr, nullptr, nullptr);

#if GEA_EMBEDDED_REPROJECT_UNIFIED_DIRTY_RECT && GEA_EMBEDDED_REPROJECT_MAX_DIRTY_PERCENT > 0 && GEA_EMBEDDED_REPROJECT_MAX_DIRTY_PERCENT < 100
		if (rdX1 >= rdX0 && rdY1 >= rdY0)
		{
			const int screenW = tree.mountedWidth();
			const int screenH = tree.mountedHeight();
			if (screenW > 0 && screenH > 0)
			{
				const int cx0 = std::max(0, rdX0);
				const int cy0 = std::max(0, rdY0);
				const int cx1 = std::min(screenW - 1, rdX1);
				const int cy1 = std::min(screenH - 1, rdY1);
				if (cx1 >= cx0 && cy1 >= cy0)
				{
					const int64_t dirtyArea = static_cast<int64_t>(cx1 - cx0 + 1) * (cy1 - cy0 + 1);
					const int64_t screenArea = static_cast<int64_t>(screenW) * screenH;
					if (dirtyArea * 100 >= screenArea * GEA_EMBEDDED_REPROJECT_MAX_DIRTY_PERCENT)
					{
#if GEA_RP2350_REPROJECT_DEBUG
						logReprojectReject("dirty-area", static_cast<int>(dirtyArea), static_cast<int>(screenArea),
															 GEA_EMBEDDED_REPROJECT_MAX_DIRTY_PERCENT);
#endif
						return false;
					}
				}
			}
		}
#endif

		state.reprojectDirtyX0 = rdX0;
		state.reprojectDirtyY0 = rdY0;
		state.reprojectDirtyX1 = rdX1;
		state.reprojectDirtyY1 = rdY1;

		// Reproduce the recorder's traversal order after transforms change. Comparing
		// the diverging branches preserves parent/child groups, paint phases,
		// stack levels, projected depth, and order-modified document ties.
		for (int i = 1; i < state.drawNodeOrderCount; ++i) {
			const int node = state.drawNodeOrder[i];
			int j = i - 1;
			while (j >= 0 && PaintOrder::compareNodes(state.drawNodeOrder[j], node) > 0) {
				state.drawNodeOrder[j + 1] = state.drawNodeOrder[j];
				--j;
			}
			state.drawNodeOrder[j + 1] = node;
		}
		return true;
	}

	void DisplayList::recordNode(int id, uint8_t parent_alpha, bool allowSkipStatic)
	{
		Tree &tree = Tree::instance();
		if (id < 0 || id >= tree.nodeCount())
			return;
		if (!state.ensureNodeScratchCapacity(tree.nodeCount()))
			return;
		Node *nodes = tree.nodes();
		int cx1 = tree.mountedWidth() > 0 ? tree.mountedWidth() - 1 : nodes[id].layout.width - 1;
		int cy1 = tree.mountedHeight() > 0 ? tree.mountedHeight() - 1 : nodes[id].layout.height - 1;
		// When the static backdrop is baked AND this frame uses the direct backdrop-blit
		// replay path, mark which subtrees contain a dirty node so recordNode can skip
		// re-recording the fully-static ones (they're blitted, never replayed). Mark each
		// dirty node and all its ancestors; everything else stays 0.
		state.recordSkipStatic = gBackdropCacheValid && allowSkipStatic;
		if (state.recordSkipStatic)
		{
			const int nc = tree.nodeCount();
			auto &treeSt = treeState();
			for (int i = 0; i < nc && i < state.nodeScratchCapacity; i++)
				state.subtreeDirty[i] = 0;
			for (int i = 0; i < nc && i < state.nodeScratchCapacity; i++)
			{
				// Recently-changed leaves (nodeBackdropCooldown) count as dynamic too:
				// the bake excludes them, so their commands must stay recorded or they
				// would vanish from replays after the next full record.
				if (!nodes[i].render.dirty && !(i < kMaxNodes && treeSt.nodeBackdropCooldown[i] > 0))
					continue;
				for (int a = i; a >= 0 && a < nc; a = nodes[a].parent)
				{
					if (a >= state.nodeScratchCapacity)
						break;
					if (state.subtreeDirty[a])
						break;
					state.subtreeDirty[a] = 1;
				}
			}
		}
		if (isDocumentCanvasRoot(nodes[id])) {
			// The UA canvas is below the root's opacity/visibility scope and is
			// independent of its layout box. Replaying it clears removed or
			// translucent propagated backgrounds across the whole viewport.
			if (DisplayCommand *cmd = append()) {
				cmd->type = DisplayCommandType::FillRect;
				cmd->bx = cmd->by = cmd->fill.x = cmd->fill.y = 0;
				cmd->bw = cmd->fill.w = cx1 + 1;
				cmd->bh = cmd->fill.h = cy1 + 1;
				cmd->fill.color = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
			}
		}
		RenderRecorder::recordNode(id, parent_alpha, 0, 0, cx1, cy1, nullptr);
	}

	void DisplayList::recordNodeWithExpandedClip(int id, uint8_t parent_alpha, int clipNode,
																							 int clipX0, int clipY0, int clipX1, int clipY1)
	{
		Tree &tree = Tree::instance();
		if (id < 0 || id >= tree.nodeCount())
			return;
		if (!state.ensureNodeScratchCapacity(tree.nodeCount()))
			return;
		Node *nodes = tree.nodes();
		int cx1 = tree.mountedWidth() > 0 ? tree.mountedWidth() - 1 : nodes[id].layout.width - 1;
		int cy1 = tree.mountedHeight() > 0 ? tree.mountedHeight() - 1 : nodes[id].layout.height - 1;
		state.recordSkipStatic = false;
		state.recordExpandedClipNode = clipNode;
		state.recordExpandedClipX0 = clipX0;
		state.recordExpandedClipY0 = clipY0;
		state.recordExpandedClipX1 = clipX1;
		state.recordExpandedClipY1 = clipY1;
		RenderRecorder::recordNode(id, parent_alpha, 0, 0, cx1, cy1, nullptr);
		state.recordExpandedClipNode = -1;
		state.recordExpandedClipX0 = 0;
		state.recordExpandedClipY0 = 0;
		state.recordExpandedClipX1 = -1;
		state.recordExpandedClipY1 = -1;
	}

	bool DisplayList::canReplayDirectDirtyRegions(int width, int height) const
	{
		if (hasTextClippedBackgrounds()) return false;
		Tree &tree = Tree::instance();
		Node *nodes = tree.nodes();
		if (state.drawNodeOrderCount <= 0)
			return false;

		for (int i = 0; i < tree.nodeCount(); i++)
		{
			if (rstyle(nodes[i].style).filter_blur_radius > 0)
				return false;
			if (nodes[i].style.overflow == 2)
				return false;
		}
		(void)width;
		(void)height;

		return true;
	}

	int DisplayList::commandCount() const { return state.commandCount; }
	bool DisplayList::hasTextClippedBackgrounds() const
	{
		return state.textClippedBackgrounds;
	}

	void DisplayList::clearRetainedBackgroundRecolors()
	{
		state.retainedBackgroundRecolorCount = 0;
	}

	int DisplayList::nodeCommandCount(int node) const
	{
		if (!state.commands || node < 0 || node >= Tree::instance().nodeCount())
			return 0;
		if (!state.hasNodeScratchFor(node))
			return 0;
		const int start = state.nodeDrawStart[node];
		const int end = state.nodeDrawEnd[node];
		if (start < 0 || end <= start || end > state.commandCount)
			return 0;
		return end - start;
	}

	const DisplayCommand *DisplayList::nodeCommandAt(int node, int index) const
	{
		if (!state.commands || node < 0 || node >= Tree::instance().nodeCount() || index < 0)
			return nullptr;
		if (!state.hasNodeScratchFor(node))
			return nullptr;
		const int start = state.nodeDrawStart[node];
		const int end = state.nodeDrawEnd[node];
		if (start < 0 || end <= start || end > state.commandCount)
			return nullptr;
		const int commandIndex = start + index;
		if (commandIndex < start || commandIndex >= end)
			return nullptr;
		return &state.commands[commandIndex];
	}

	bool DisplayList::nodeCommandBounds(int node, int *x0, int *y0, int *x1, int *y1) const
	{
		if (!state.commands || node < 0 || node >= Tree::instance().nodeCount() || !x0 || !y0 || !x1 || !y1)
			return false;
		if (!state.hasNodeScratchFor(node))
			return false;
		const int start = state.nodeDrawStart[node];
		const int end = state.nodeDrawEnd[node];
		if (start < 0 || end <= start || end > state.commandCount)
			return false;
		bool any = false;
		int bx0 = 0;
		int by0 = 0;
		int bx1 = -1;
		int by1 = -1;
		for (int ci = start; ci < end; ci++)
		{
			const DisplayCommand &command = state.commands[ci];
			if (command.bw <= 0 || command.bh <= 0)
				continue;
			const int cx0 = command.bx;
			const int cy0 = command.by;
			const int cx1 = command.bx + command.bw - 1;
			const int cy1 = command.by + command.bh - 1;
			if (!any)
			{
				bx0 = cx0;
				by0 = cy0;
				bx1 = cx1;
				by1 = cy1;
				any = true;
			}
			else
			{
				if (cx0 < bx0)
					bx0 = cx0;
				if (cy0 < by0)
					by0 = cy0;
				if (cx1 > bx1)
					bx1 = cx1;
				if (cy1 > by1)
					by1 = cy1;
			}
		}
		if (!any)
			return false;
		*x0 = bx0;
		*y0 = by0;
		*x1 = bx1;
		*y1 = by1;
		return true;
	}

	void DisplayList::translateNodeCommands(int node, int dx, int dy)
	{
		if (!state.commands || node < 0 || node >= Tree::instance().nodeCount())
			return;
		if (dx == 0 && dy == 0)
			return;
		if (!state.hasNodeScratchFor(node))
			return;
		int start = state.nodeDrawStart[node];
		int end = state.nodeDrawEnd[node];
		if (start >= 0 && end > start && end <= state.commandCount)
		{
			for (int ci = start; ci < end; ci++)
				DisplayCommandTranslator::translate(&state.commands[ci], dx, dy);
		}
		// PushClip/PopClip wrap the node's descendants, so recordNode deliberately
		// emits them outside [nodeDrawStart,nodeDrawEnd]. Move those retained scope
		// commands explicitly; otherwise a translated overflow:hidden subtree is
		// intersected with the clip at its old position.
		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand &command = state.commands[ci];
			if ((command.type == DisplayCommandType::PushClip || command.type == DisplayCommandType::PopClip) &&
					command.clip.nodeId == node)
				DisplayCommandTranslator::translate(&command, dx, dy);
		}
	}

	void DisplayList::scrubBlitPixels(const gea::framework::graphics::pixel::native_t *pixels)
	{
		if (!pixels || !state.commands)
			return;
		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand &c = state.commands[ci];
			if (c.type == DisplayCommandType::BlitImage && c.blit.pixels == pixels)
			{
				// Null pointer makes the draw a no-op (Canvas::drawImage guards it);
				// zero bounds keep dirty-region and coverage passes from treating the
				// dead command as painting its old rect.
				c.blit.pixels = nullptr;
				c.blit.alpha = nullptr;
				c.bw = 0;
				c.bh = 0;
			}
			else if (c.type == DisplayCommandType::BlitImageScaled && c.scaledBlit.pixels == pixels)
			{
				c.scaledBlit.pixels = nullptr;
				c.scaledBlit.alpha = nullptr;
				c.bw = 0;
				c.bh = 0;
			}
		}
	}

	void DisplayList::scrubNodeText(const char *data)
	{
		if (!data || !state.commands)
			return;
		// The drawers guard on an empty string, so pointing the dead command at a
		// static "" makes its replay a no-op; zero bounds keep dirty-region and
		// coverage passes from treating it as painting its old rect.
		static const char kEmpty[] = "";
		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand &c = state.commands[ci];
			if (c.type == DisplayCommandType::DrawText && c.text.text == data)
			{
				c.text.text = kEmpty;
				c.bw = 0;
				c.bh = 0;
			}
			else if (c.type == DisplayCommandType::DrawProjectedText && c.projectedText.text == data)
			{
				c.projectedText.text = kEmpty;
				c.bw = 0;
				c.bh = 0;
			}
		}
	}

	void DisplayList::translateSubtreeCommands(int node, int dx, int dy)
	{
		if (!state.commands || node < 0 || node >= Tree::instance().nodeCount())
			return;
		if (dx == 0 && dy == 0)
			return;
		Tree &tree = Tree::instance();
		Node *nodes = tree.nodes();
		auto belongsToSubtree = [&](int id) {
			for (int cursor = id; cursor >= 0 && cursor < tree.nodeCount(); cursor = nodes[cursor].parent)
				if (cursor == node)
					return true;
			return false;
		};
		for (int id = 0; id < tree.nodeCount(); id++)
		{
			if (!belongsToSubtree(id) || !state.hasNodeScratchFor(id))
				continue;
			const int start = state.nodeDrawStart[id];
			const int end = state.nodeDrawEnd[id];
			if (start >= 0 && end > start && end <= state.commandCount)
			{
				for (int ci = start; ci < end; ci++)
					DisplayCommandTranslator::translate(&state.commands[ci], dx, dy);
			}
		}
		// Clip scopes sit outside node paint ranges. Scan the command buffer once
		// after translating all paint ranges, rather than once per descendant.
		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand &command = state.commands[ci];
			if ((command.type == DisplayCommandType::PushClip || command.type == DisplayCommandType::PopClip) &&
					belongsToSubtree(command.clip.nodeId))
				DisplayCommandTranslator::translate(&command, dx, dy);
		}
	}

	namespace
	{

		// The clip rect a fresh record pass would hand `id` for its OWN paint commands:
		// the mounted viewport, narrowed by every ancestor that pushes an overflow clip.
		// This mirrors RenderRecorder::recordNode (which clamps the child clip to a node
		// whose recordClipBegin succeeded) and ViewRenderer::recordClipBegin's own
		// conditions — view-like, overflow set, has children.
		//
		// Returns false when the record-time clip cannot be reproduced faithfully from
		// layout boxes alone: with any transform live, a clip is a projected quad and
		// recordClipBegin refuses to push one at all. Callers must then fall back to
		// their conservative answer — this predicate only ever LICENSES treating a node
		// as culled, so an unknown must never read as "outside".
		bool recordClipRectForNode(int id, int width, int height, int *cx0, int *cy0, int *cx1, int *cy1)
		{
			Tree &tree = Tree::instance();
			const int nodeCount = tree.nodeCount();
			if (id < 0 || id >= nodeCount || width <= 0 || height <= 0)
				return false;
			if (ViewRenderer::anyTransformActive())
				return false;
			Node *nodes = tree.nodes();
			int x0 = 0;
			int y0 = 0;
			int x1 = width - 1;
			int y1 = height - 1;
			for (int a = LayoutEngine::isViewportFixed(nodes[id]) ? -1 : nodes[id].parent; a >= 0 && a < nodeCount; a = nodes[a].parent)
			{
				const Node &ancestor = nodes[a];
				if (!isViewLikeNodeType(ancestor.type) || ancestor.style.overflow == 0 || ancestor.first_child < 0)
				{
					if (LayoutEngine::isViewportFixed(ancestor)) break;
					continue;
				}
				ClipMath::clampToNode(ancestor, &x0, &y0, &x1, &y1);
				if (LayoutEngine::isViewportFixed(ancestor)) break;
			}
			*cx0 = x0;
			*cy0 = y0;
			*cx1 = x1;
			*cy1 = y1;
			return true;
		}

		// True only when a fresh record pass would PROVABLY emit no commands for this
		// node because it falls outside its ancestors' overflow clips (or the viewport).
		// Answers false whenever that cannot be established.
		bool nodeOutsideRecordClip(int id, int width, int height)
		{
			int cx0 = 0;
			int cy0 = 0;
			int cx1 = -1;
			int cy1 = -1;
			if (!recordClipRectForNode(id, width, height, &cx0, &cy0, &cx1, &cy1))
				return false;
			if (cx0 > cx1 || cy0 > cy1)
				return true;
			const Node &n = Tree::instance().nodes()[id];
			int nx0 = 0;
			int ny0 = 0;
			int nx1 = -1;
			int ny1 = -1;
			ViewRenderer::transformedBounds(n, false, &nx0, &ny0, &nx1, &ny1);
			return !(nx0 <= cx1 && ny0 <= cy1 && nx1 >= cx0 && ny1 >= cy0);
		}

	} // namespace

	bool DisplayList::subtreeRevealsUnrecordedContent(int node, int width, int height) const
	{
		Tree &tree = Tree::instance();
		Node *nodes = tree.nodes();
		if (!state.commands || node < 0 || node >= tree.nodeCount())
			return false;
		if (width <= 0 || height <= 0)
			return false;
		// With a baked static backdrop, recordNode intentionally clears the ranges
		// of fully-static subtrees (they replay via the backdrop blit, not the
		// list), so an empty/cleared range does not mean "culled" here.
		if (staticBackdropActive())
			return false;
		for (int id = 0; id < tree.nodeCount(); id++)
		{
			// Nodes with recorded commands translated correctly. Text/Image/Canvas
			// nodes outside the clip are culled with range -1; a View outside the
			// clip is still descended for its children and keeps an EMPTY range, so
			// an empty range on a view that would paint (visible background) is the
			// same incompleteness. Bare containers (paint nothing anywhere) stay
			// exempt or every translate frame would force a full record.
			if (state.hasNodeScratchFor(id) && state.nodeDrawStart[id] >= 0)
			{
				if (state.nodeDrawEnd[id] > state.nodeDrawStart[id])
					continue;
				const Node &probe = nodes[id];
				const bool paintsBox =
						(probe.style.has_bg && (probe.style.bg_alpha > 0 || styleHasBackgroundImage(probe.style))) ||
						rstyle(probe.style).box_shadow_alpha > 0;
				if (!paintsBox)
					continue;
			}
			const Node &n = nodes[id];
			if (n.layout.width <= 0 || n.layout.height <= 0)
				continue;
			if (n.layout.x + n.layout.width - 1 < 0 || n.layout.x > width - 1 ||
					n.layout.y + n.layout.height - 1 < 0 || n.layout.y > height - 1)
				continue; // still fully outside the viewport — cull stays valid
			// The viewport is only the OUTERMOST clip. Content culled by an ancestor's
			// overflow (a tile parked above its scroll viewport) is on-screen by the
			// test above yet still invisible, and a shift that leaves it outside that
			// same clip reveals nothing: a fresh record would cull it exactly as the
			// retained list already did. Judging the cull against the viewport alone
			// declared every such frame incomplete and forced a full re-record.
			if (nodeOutsideRecordClip(id, width, height))
				continue;
			// Subtree membership + hidden-ancestor filter in one upward walk: a node
			// under a display:none / opacity:0 / blink-hidden ancestor was culled for
			// visibility, not clipping, and stays correctly absent after the shift.
			bool inSubtree = false;
			bool hiddenOrOutside = false;
			for (int cursor = id; cursor >= 0 && cursor < tree.nodeCount(); cursor = nodes[cursor].parent)
			{
				const Node &a = nodes[cursor];
				if (a.style.display == 1 || a.style.opacity == 0 ||
						(a.style.blink_interval_ms > 0 && !a.style.blink_visible))
				{
					hiddenOrOutside = true;
					break;
				}
				if (cursor == node)
				{
					inSubtree = true;
					break;
				}
			}
			if (!inSubtree || hiddenOrOutside)
				continue;
			return true;
		}
		return false;
	}

	bool DisplayList::patchNodeAlpha(int node, uint8_t opacity)
	{
		// The OPEN SetAlpha wrapping a node's subtree lives OUTSIDE the node's
		// [drawStart,drawEnd] range, so scan by the nodeId tag recordNode stamped on
		// it. Recompute from the record-time parent alpha so nested scopes stay
		// correct: alpha = recordParentAlpha * opacity / 255.
		if (!state.commands || node < 0 || node >= Tree::instance().nodeCount())
			return false;
		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand *c = &state.commands[ci];
			if (c->type != DisplayCommandType::SetAlpha || c->alpha.nodeId != node)
				continue;
			c->alpha.alpha = static_cast<uint8_t>((static_cast<int>(c->alpha.recordParentAlpha) * opacity) / 255);
			return true;
		}
		return false;
	}

	namespace
	{

		bool commandOverlapsRect(const DisplayCommand &command, int x0, int y0, int x1, int y1)
		{
			return ClipMath::rectsOverlap(command.bx,
																		command.by,
																		command.bx + command.bw - 1,
																		command.by + command.bh - 1,
																		x0,
																		y0,
																		x1,
																		y1);
		}

		bool commandDoesNotPaintColor(const DisplayCommand &command, uint16_t color)
		{
			uint16_t commandColor = 0;
			switch (command.type)
			{
			case DisplayCommandType::PushClip:
			case DisplayCommandType::PopClip:
			case DisplayCommandType::SetAlpha:
			case DisplayCommandType::BeginFilterBlur:
			case DisplayCommandType::ApplyFilterBlur:
				return true;
			case DisplayCommandType::FillRect:
				commandColor = command.fill.color;
				break;
			case DisplayCommandType::FillCircle:
				commandColor = command.fillCircle.color;
				break;
			case DisplayCommandType::FillRoundedRect:
				commandColor = command.fillRoundedRect.color;
				break;
			case DisplayCommandType::FillQuad:
				commandColor = command.quad.color;
				break;
			case DisplayCommandType::DrawLine:
				commandColor = command.line.color;
				break;
			case DisplayCommandType::StrokeRect:
				commandColor = command.stroke.color;
				break;
			case DisplayCommandType::StrokeRoundedRect:
				commandColor = command.strokeRoundedRect.color;
				break;
			case DisplayCommandType::DrawText:
				commandColor = command.text.color;
				break;
			case DisplayCommandType::DrawProjectedText:
				if (command.projectedText.alpha != 255)
					return false;
				commandColor = command.projectedText.color;
				break;
			case DisplayCommandType::FillTransformedRoundedRect:
				commandColor = command.transformedRoundedRect.color;
				break;
			default:
				return false;
			}
			return commandColor != color;
		}

		bool laterOverlappingCommandsAvoidColor(int node, const DisplayCommand &backgroundCommand, uint16_t color)
		{
			Tree &tree = Tree::instance();
			Node *nodes = tree.nodes();
			const int nodeCount = tree.nodeCount();
			int orderIndex = -1;
			for (int oi = 0; oi < state.drawNodeOrderCount; oi++)
			{
				if (state.drawNodeOrder[oi] == node)
				{
					orderIndex = oi;
					break;
				}
			}
			if (orderIndex < 0)
				return false;
			const int x0 = backgroundCommand.bx;
			const int y0 = backgroundCommand.by;
			const int x1 = backgroundCommand.bx + backgroundCommand.bw - 1;
			const int y1 = backgroundCommand.by + backgroundCommand.bh - 1;
			for (int oi = orderIndex + 1; oi < state.drawNodeOrderCount; oi++)
			{
				const int paintNode = state.drawNodeOrder[oi];
				if (paintNode < 0 || paintNode >= nodeCount)
					continue;
				const int start = state.nodeDrawStart[paintNode];
				const int end = state.nodeDrawEnd[paintNode];
				if (!DisplayCommandReplayer::commandRangeBBoxOverlaps(start, end, x0, y0, x1, y1))
					continue;
				if (DisplayCommandReplayer::effectiveAlphaForNode(paintNode, nodes, nodeCount) != 255)
					return false;
				for (int ci = start; ci < end; ci++)
				{
					const DisplayCommand &command = state.commands[ci];
					if (!commandOverlapsRect(command, x0, y0, x1, y1))
						continue;
					if (!commandDoesNotPaintColor(command, color))
						return false;
				}
			}
			return true;
		}

		bool replayLaterOverlappingCommands(int node, const DisplayReplayRegion &region)
		{
			Tree &tree = Tree::instance();
			Node *nodes = tree.nodes();
			const int nodeCount = tree.nodeCount();
			int orderIndex = -1;
			for (int oi = 0; oi < state.drawNodeOrderCount; oi++)
			{
				if (state.drawNodeOrder[oi] == node)
				{
					orderIndex = oi;
					break;
				}
			}
			if (orderIndex < 0)
				return false;

			UniformRoundedRectBatch roundedRects;
			for (int oi = orderIndex + 1; oi < state.drawNodeOrderCount; oi++)
			{
				const int paintNode = state.drawNodeOrder[oi];
				if (paintNode < 0 || paintNode >= nodeCount)
					continue;
				const Node &paint = nodes[paintNode];
				if (paint.style.display == 1)
					continue;
				if (paint.style.blink_interval_ms > 0 && !paint.style.blink_visible)
					continue;
				DisplayCommandReplayer::replayNodeCommandRangeInRegions(paintNode, &region, 1, roundedRects);
			}
			roundedRects.flush();
			return true;
		}

		bool recolorRetainedFillRect(const DisplayCommand &command,
																 uint16_t oldColor,
																 uint16_t newColor,
																 int *outX0,
																 int *outY0,
																 int *outX1,
																 int *outY1)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return false;
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			int x0 = command.fill.x;
			int y0 = command.fill.y;
			int x1 = command.fill.x + command.fill.w - 1;
			int y1 = command.fill.y + command.fill.h - 1;
			if (x0 < 0)
				x0 = 0;
			if (y0 < 0)
				y0 = 0;
			if (x1 >= canvas->width())
				x1 = canvas->width() - 1;
			if (y1 >= canvas->height())
				y1 = canvas->height() - 1;
			if (x0 > x1 || y0 > y1)
				return false;
			int dirtyX0 = canvas->width();
			int dirtyY0 = canvas->height();
			int dirtyX1 = -1;
			int dirtyY1 = -1;
			for (int y = y0; y <= y1; y++)
			{
				gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
				for (int x = x0; x <= x1; x++)
				{
					if (row[x] != oldColor)
						continue;
					row[x] = newColor;
					if (x < dirtyX0)
						dirtyX0 = x;
					if (x > dirtyX1)
						dirtyX1 = x;
					if (y < dirtyY0)
						dirtyY0 = y;
					if (y > dirtyY1)
						dirtyY1 = y;
				}
			}
			if (dirtyX0 > dirtyX1 || dirtyY0 > dirtyY1)
				return false;
			canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
			if (outX0)
				*outX0 = dirtyX0;
			if (outY0)
				*outY0 = dirtyY0;
			if (outX1)
				*outX1 = dirtyX1;
			if (outY1)
				*outY1 = dirtyY1;
			return true;
		}

		// Fill a native framebuffer span with a native colour. On RGB565 targets this is
		// the original uint32-pair fast path via pixel::fillNative.
		static inline void fillRgb565SpanFast(gea::framework::graphics::pixel::native_t *dst, int count, gea::framework::graphics::pixel::native_t color)
		{
			gea::framework::graphics::pixel::fillNative(dst, count, color);
		}

		// Replace exact-match pixels in a native framebuffer span. On RGB565 targets the
		// uint32-pair scan is preserved byte-for-byte; other targets use a per-pixel scan.
		static inline int replaceRgb565SpanFast(gea::framework::graphics::pixel::native_t *dst, int count, gea::framework::graphics::pixel::native_t oldColor, gea::framework::graphics::pixel::native_t newColor)
		{
			if (count <= 0 || oldColor == newColor)
				return 0;
			int changed = 0;
#if GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
			while (count > 0 && (reinterpret_cast<std::uintptr_t>(dst) & 0x2u) != 0)
			{
				if (*dst == oldColor)
				{
					*dst = newColor;
					++changed;
				}
				++dst;
				--count;
			}
			const std::uint32_t oldPair = static_cast<std::uint32_t>(oldColor) |
																		(static_cast<std::uint32_t>(oldColor) << 16);
			const std::uint32_t newPair = static_cast<std::uint32_t>(newColor) |
																		(static_cast<std::uint32_t>(newColor) << 16);
			const int pairCount = count / 2;
			auto *pairs = reinterpret_cast<std::uint32_t *>(dst);
			for (int i = 0; i < pairCount; ++i)
			{
				const std::uint32_t word = pairs[i];
				if (word == oldPair)
				{
					pairs[i] = newPair;
					changed += 2;
					continue;
				}
				std::uint32_t next = word;
				if ((word & 0xffffu) == oldColor)
				{
					next = (next & 0xffff0000u) | static_cast<std::uint32_t>(newColor);
					++changed;
				}
				if ((word >> 16) == oldColor)
				{
					next = (next & 0x0000ffffu) | (static_cast<std::uint32_t>(newColor) << 16);
					++changed;
				}
				if (next != word)
					pairs[i] = next;
			}
			dst += pairCount * 2;
			count -= pairCount * 2;
			if (count > 0 && *dst == oldColor)
			{
				*dst = newColor;
				++changed;
			}
#else
			for (int i = 0; i < count; ++i)
			{
				if (dst[i] == oldColor)
				{
					dst[i] = newColor;
					++changed;
				}
			}
#endif
			return changed;
		}

		int roundedRectIntegerSqrt(int value)
		{
			if (value <= 0)
				return 0;
			int root = static_cast<int>(std::sqrt(static_cast<double>(value)));
			while ((root + 1) > 0 && (root + 1) <= 46340 && (root + 1) * (root + 1) <= value)
				++root;
			while (root * root > value)
				--root;
			return root;
		}

		bool roundedRectRasterSolidSpan(const decltype(DisplayCommand{}.fillRoundedRect) &r, int sy, int *outX0, int *outX1)
		{
			if (!outX0 || !outX1 || r.w <= 0 || r.h <= 0 || sy < r.y || sy >= r.y + r.h)
				return false;
			const int topR = std::max(static_cast<int>(r.tl), static_cast<int>(r.tr));
			const int bottomR = std::max(static_cast<int>(r.bl), static_cast<int>(r.br));
			int sx0 = 0x7fffffff;
			int sx1 = -0x7fffffff;
			auto addSpan = [&](int left, int right)
			{
				if (left > right)
					return;
				if (left < sx0)
					sx0 = left;
				if (right > sx1)
					sx1 = right;
			};

			if (r.h - topR - bottomR > 0 && sy >= r.y + topR && sy < r.y + r.h - bottomR)
				addSpan(r.x, r.x + r.w - 1);
			if (topR > 0 && sy >= r.y && sy < r.y + topR)
				addSpan(r.x + r.tl, r.x + r.w - r.tr - 1);
			if (bottomR > 0 && sy >= r.y + r.h - bottomR && sy <= r.y + r.h - 1)
				addSpan(r.x + r.bl, r.x + r.w - r.br - 1);
			if (r.tl > 0 && r.tl < topR && sy >= r.y + r.tl && sy < r.y + topR)
				addSpan(r.x, r.x + r.tl - 1);
			if (r.tr > 0 && r.tr < topR && sy >= r.y + r.tr && sy < r.y + topR)
				addSpan(r.x + r.w - r.tr, r.x + r.w - 1);
			if (r.bl > 0 && r.bl < bottomR && sy >= r.y + r.h - bottomR && sy < r.y + r.h - r.bl)
				addSpan(r.x, r.x + r.bl - 1);
			if (r.br > 0 && r.br < bottomR && sy >= r.y + r.h - bottomR && sy < r.y + r.h - r.br)
				addSpan(r.x + r.w - r.br, r.x + r.w - 1);

			if (r.tl > 0 && sy >= r.y && sy <= r.y + r.tl)
			{
				const int dy = r.y + r.tl - sy;
				const int dx = roundedRectIntegerSqrt(r.tl * r.tl - dy * dy);
				addSpan(r.x + r.tl - dx, r.x + r.tl);
			}
			if (r.tr > 0 && sy >= r.y && sy <= r.y + r.tr)
			{
				const int dy = r.y + r.tr - sy;
				const int dx = roundedRectIntegerSqrt(r.tr * r.tr - dy * dy);
				addSpan(r.x + r.w - 1 - r.tr, r.x + r.w - 1 - r.tr + dx);
			}
			if (r.bl > 0 && sy >= r.y + r.h - 1 - r.bl && sy <= r.y + r.h - 1)
			{
				const int dy = sy - (r.y + r.h - 1 - r.bl);
				const int dx = roundedRectIntegerSqrt(r.bl * r.bl - dy * dy);
				addSpan(r.x + r.bl - dx, r.x + r.bl);
			}
			if (r.br > 0 && sy >= r.y + r.h - 1 - r.br && sy <= r.y + r.h - 1)
			{
				const int dy = sy - (r.y + r.h - 1 - r.br);
				const int dx = roundedRectIntegerSqrt(r.br * r.br - dy * dy);
				addSpan(r.x + r.w - 1 - r.br, r.x + r.w - 1 - r.br + dx);
			}

			if (sx0 > sx1)
				return false;
			*outX0 = sx0;
			*outX1 = sx1;
			return true;
		}

		static inline void addUnclippedAxisRoundedRectSpan(int left, int right, int *x0, int *x1)
		{
			if (left > right)
				return;
			if (left < *x0)
				*x0 = left;
			if (right > *x1)
				*x1 = right;
		}

		void axisRoundedRectAnyCoverageSpan(const decltype(DisplayCommand{}.fillRoundedRect) &r,
																				int py,
																				int samples,
																				float kernelWidth,
																				int *spanX0,
																				int *spanX1)
		{
			const int topR = std::max(static_cast<int>(r.tl), static_cast<int>(r.tr));
			const int bottomR = std::max(static_cast<int>(r.bl), static_cast<int>(r.br));
			const float minSample = antialiasOffsetWithKernel(0, samples, kernelWidth);
			const float maxSample = antialiasOffsetWithKernel(samples - 1, samples, kernelWidth);
			const float rowMin = static_cast<float>(py) + minSample;
			const float rowMax = static_cast<float>(py) + maxSample;
			const float left = static_cast<float>(r.x);
			const float top = static_cast<float>(r.y);
			const float right = left + static_cast<float>(r.w);
			const float bottom = top + static_cast<float>(r.h);

			if (rowMax < top || rowMin >= bottom)
				return;

			auto addRectSpan = [&](int sx0, int sx1, float sy0, float sy1)
			{
				if (rowMax < sy0 || rowMin >= sy1)
					return;
				addUnclippedAxisRoundedRectSpan(sx0, sx1, spanX0, spanX1);
			};
			addRectSpan(r.x, r.x + r.w - 1, top + static_cast<float>(topR), bottom - static_cast<float>(bottomR));
			addRectSpan(r.x + r.tl, r.x + r.w - r.tr - 1, top, top + static_cast<float>(topR));
			addRectSpan(r.x + r.bl, r.x + r.w - r.br - 1, bottom - static_cast<float>(bottomR), bottom);
			if (r.tl > 0 && r.tl < topR)
				addRectSpan(r.x, r.x + r.tl - 1, top + static_cast<float>(r.tl), top + static_cast<float>(topR));
			if (r.tr > 0 && r.tr < topR)
				addRectSpan(r.x + r.w - r.tr, r.x + r.w - 1, top + static_cast<float>(r.tr), top + static_cast<float>(topR));
			if (r.bl > 0 && r.bl < bottomR)
				addRectSpan(r.x, r.x + r.bl - 1, bottom - static_cast<float>(bottomR), bottom - static_cast<float>(r.bl));
			if (r.br > 0 && r.br < bottomR)
				addRectSpan(r.x + r.w - r.br, r.x + r.w - 1, bottom - static_cast<float>(bottomR), bottom - static_cast<float>(r.br));

			auto addCornerSpan = [&](int radius, float cx, float cy, bool leftCorner, bool topCorner)
			{
				if (radius <= 0)
					return;
				const float rr = static_cast<float>(radius);
				if (topCorner)
				{
					if (rowMax < cy - rr || rowMin >= cy)
						return;
				}
				else
				{
					if (rowMax < cy || rowMin >= cy + rr)
						return;
				}
				float sampleY = cy;
				if (sampleY < rowMin)
					sampleY = rowMin;
				if (sampleY > rowMax)
					sampleY = rowMax;
				const float dy = sampleY - cy;
				const float dx2 = rr * rr - dy * dy;
				if (dx2 < 0.0f)
					return;
				const float dx = std::sqrt(dx2);
				if (leftCorner)
				{
					const int sx0 = static_cast<int>(std::ceil(cx - dx - maxSample));
					const int sx1 = static_cast<int>(std::floor(cx - minSample));
					addUnclippedAxisRoundedRectSpan(sx0, sx1, spanX0, spanX1);
				}
				else
				{
					const int sx0 = static_cast<int>(std::ceil(cx - maxSample));
					const int sx1 = static_cast<int>(std::floor(cx + dx - minSample));
					addUnclippedAxisRoundedRectSpan(sx0, sx1, spanX0, spanX1);
				}
			};

			addCornerSpan(r.tl, left + static_cast<float>(r.tl), top + static_cast<float>(r.tl), true, true);
			addCornerSpan(r.tr, right - static_cast<float>(r.tr), top + static_cast<float>(r.tr), false, true);
			addCornerSpan(r.bl, left + static_cast<float>(r.bl), bottom - static_cast<float>(r.bl), true, false);
			addCornerSpan(r.br, right - static_cast<float>(r.br), bottom - static_cast<float>(r.br), false, false);
		}

		bool axisRoundedRectIsCircleLike(int w, int h, int tl, int tr, int br, int bl)
		{
			const int minSide = std::min(w, h);
			if (minSide < 16 || std::abs(w - h) > 1)
				return false;
			const int halfMin = minSide / 2;
			const int minRadius = std::max(1, halfMin - 1);
			return tl >= minRadius && tr >= minRadius && br >= minRadius && bl >= minRadius;
		}

		float axisRoundedRectAntialiasKernelWidth(int samples, int w, int h, int tl, int tr, int br, int bl)
		{
			if (samples < 2)
				return 1.0f;
			if (axisRoundedRectIsCircleLike(w, h, tl, tr, br, bl))
			{
				return 1.0f;
			}
			if (samples < 4)
				return 1.0f;
			return std::min(w, h) >= 64 ? 2.0f : 1.0f;
		}

		int axisRoundedRectAntialiasEdgePad(float kernelWidth)
		{
			if (kernelWidth <= 1.0f)
				return 1;
			return static_cast<int>(std::ceil(kernelWidth * 0.5f)) + 1;
		}

		bool axisRoundedRectSampleContains(int x,
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
			if (sx < left || sy < top || sx >= right || sy >= bottom)
				return false;

			auto cornerContains = [](float cx, float cy, float radius, float px, float py)
			{
				if (radius <= 0.0f)
					return true;
				const float dx = px - cx;
				const float dy = py - cy;
				return dx * dx + dy * dy <= radius * radius;
			};

			const float tlRadius = static_cast<float>(tl);
			if (tl > 0 && sx < left + tlRadius && sy < top + tlRadius)
				return cornerContains(left + tlRadius, top + tlRadius, tlRadius, sx, sy);
			const float trRadius = static_cast<float>(tr);
			if (tr > 0 && sx >= right - trRadius && sy < top + trRadius)
				return cornerContains(right - trRadius, top + trRadius, trRadius, sx, sy);
			const float brRadius = static_cast<float>(br);
			if (br > 0 && sx >= right - brRadius && sy >= bottom - brRadius)
				return cornerContains(right - brRadius, bottom - brRadius, brRadius, sx, sy);
			const float blRadius = static_cast<float>(bl);
			if (bl > 0 && sx < left + blRadius && sy >= bottom - blRadius)
				return cornerContains(left + blRadius, bottom - blRadius, blRadius, sx, sy);
			return true;
		}

		bool axisRoundedRectSampleContains(const decltype(DisplayCommand{}.fillRoundedRect) &r, float sx, float sy)
		{
			return axisRoundedRectSampleContains(r.x, r.y, r.w, r.h, r.tl, r.tr, r.br, r.bl, sx, sy);
		}

		int axisRoundedRectCoverage(const decltype(DisplayCommand{}.fillRoundedRect) &r,
																int x,
																int y,
																int samples,
																float kernelWidth)
		{
			int coverage = 0;
			for (int iy = 0; iy < samples; ++iy)
			{
				const float oy = antialiasOffsetWithKernel(iy, samples, kernelWidth);
				for (int ix = 0; ix < samples; ++ix)
				{
					const float ox = antialiasOffsetWithKernel(ix, samples, kernelWidth);
					if (axisRoundedRectSampleContains(r, static_cast<float>(x) + ox, static_cast<float>(y) + oy))
						++coverage;
				}
			}
			return coverage;
		}

		int axisRoundedRectStrokeCoverage(const decltype(DisplayCommand{}.strokeRoundedRect) &r,
																			int x,
																			int y,
																			int samples,
																			float kernelWidth)
		{
			const int lineWidth = std::max(1, static_cast<int>(r.lineWidth));
			const int innerX = static_cast<int>(r.x) + lineWidth;
			const int innerY = static_cast<int>(r.y) + lineWidth;
			const int innerW = static_cast<int>(r.w) - lineWidth * 2;
			const int innerH = static_cast<int>(r.h) - lineWidth * 2;
			const int innerTl = std::max(0, static_cast<int>(r.tl) - lineWidth);
			const int innerTr = std::max(0, static_cast<int>(r.tr) - lineWidth);
			const int innerBr = std::max(0, static_cast<int>(r.br) - lineWidth);
			const int innerBl = std::max(0, static_cast<int>(r.bl) - lineWidth);
			const bool hasInner = innerW > 0 && innerH > 0;
			int coverage = 0;
			for (int iy = 0; iy < samples; ++iy)
			{
				const float oy = antialiasOffsetWithKernel(iy, samples, kernelWidth);
				for (int ix = 0; ix < samples; ++ix)
				{
					const float ox = antialiasOffsetWithKernel(ix, samples, kernelWidth);
					const float sx = static_cast<float>(x) + ox;
					const float sy = static_cast<float>(y) + oy;
					if (!axisRoundedRectSampleContains(r.x, r.y, r.w, r.h, r.tl, r.tr, r.br, r.bl, sx, sy))
						continue;
					if (hasInner &&
							axisRoundedRectSampleContains(innerX, innerY, innerW, innerH,
																						innerTl, innerTr, innerBr, innerBl,
																						sx, sy))
						continue;
					++coverage;
				}
			}
			return coverage;
		}

		int rgb565Distance(std::uint16_t a, std::uint16_t b)
		{
			a = gea::framework::graphics::pixel::toRgb565(a);
			b = gea::framework::graphics::pixel::toRgb565(b);
			const int ar = (a >> 11) & 0x1f;
			const int ag = (a >> 5) & 0x3f;
			const int ab = a & 0x1f;
			const int br = (b >> 11) & 0x1f;
			const int bg = (b >> 5) & 0x3f;
			const int bb = b & 0x1f;
			return std::abs(ar - br) * 8 + std::abs(ag - bg) * 4 + std::abs(ab - bb) * 8;
		}

		std::uint16_t recoverBlendBackdrop(std::uint16_t source, std::uint16_t composite, int alpha)
		{
			const int inverse = 255 - alpha;
			if (inverse <= 0)
				return composite;
			int sr = 0, sg = 0, sb = 0;
			int cr = 0, cg = 0, cb = 0;
			unpackRgb565To888(source, &sr, &sg, &sb);
			unpackRgb565To888(composite, &cr, &cg, &cb);
			const int br = clampByte((cr * 255 - sr * alpha + inverse / 2) / inverse);
			const int bg = clampByte((cg * 255 - sg * alpha + inverse / 2) / inverse);
			const int bb = clampByte((cb * 255 - sb * alpha + inverse / 2) / inverse);
			return gea::framework::graphics::pixel::fromRgb888(br, bg, bb);
		}

		bool recolorRoundedRectCoveragePixel(gea::framework::graphics::pixel::native_t &pixel,
																				 gea::framework::graphics::pixel::native_t oldColor,
																				 gea::framework::graphics::pixel::native_t newColor,
																				 int alpha,
																				 gea::framework::graphics::pixel::native_t fallbackBackdrop)
		{
			if (alpha <= 0)
				return false;
			// Opaque coverage is an exact native-pixel match/replace (correct on every
			// colour space). The sub-coverage AA-edge reconstruction below is a 5/6-bit
			// tuned heuristic, so it runs in RGB565 via the native boundary — identity on
			// RGB565 targets (byte-identical), best-effort on full-colour boards (it bails
			// to a full re-render when it can't confidently match).
			if (alpha >= 255)
			{
				if (pixel != oldColor)
					return false;
				pixel = newColor;
				return true;
			}

			const std::uint16_t pix = gea::framework::graphics::pixel::fromNative(pixel);
			const std::uint16_t oldColor565 = gea::framework::graphics::pixel::fromNative(oldColor);
			const std::uint16_t newColor565 = gea::framework::graphics::pixel::fromNative(newColor);
			std::uint16_t backdrop = gea::framework::graphics::pixel::fromNative(fallbackBackdrop);
			if (pix != oldColor565 && pix != newColor565)
			{
				const std::uint16_t recovered = recoverBlendBackdrop(oldColor565, pix, alpha);
				const std::uint16_t reconstructed = gea::framework::graphics::pixel::blend(oldColor565, backdrop, alpha);
				bool compatible = rgb565Distance(reconstructed, pix) <= 24;
				if (rgb565Distance(reconstructed, pix) > 24)
				{
					const std::uint16_t recoveredReconstructed = gea::framework::graphics::pixel::blend(oldColor565, recovered, alpha);
					if (rgb565Distance(recoveredReconstructed, pix) <= 48)
					{
						backdrop = recovered;
						compatible = true;
					}
				}
				if (!compatible)
					return false;
			}
			const std::uint16_t next = gea::framework::graphics::pixel::blend(newColor565, backdrop, alpha);
			if (pix == next)
				return false;
			pixel = gea::framework::graphics::pixel::toNative(next);
			return true;
		}

		gea::framework::graphics::pixel::native_t roundedRectCoverageFallbackBackdrop(const gea::framework::graphics::Canvas &canvas,
																																									const decltype(DisplayCommand{}.fillRoundedRect) &r,
																																									int x,
																																									int y,
																																									int edgePad)
		{
			if (!canvas.pixels() || canvas.width() <= 0 || canvas.height() <= 0)
				return 0x0000;
			int sampleX = x;
			int sampleY = y;
			const int pad = std::max(1, edgePad + 1);
			if (y < r.y)
			{
				sampleY = y - pad;
			}
			else if (y >= r.y + r.h)
			{
				sampleY = y + pad;
			}
			else if (x < r.x)
			{
				sampleX = x - pad;
			}
			else if (x >= r.x + r.w)
			{
				sampleX = x + pad;
			}
			else
			{
				const int centerX2 = r.x * 2 + r.w;
				const int centerY2 = r.y * 2 + r.h;
				const int dx2 = x * 2 + 1 - centerX2;
				const int dy2 = y * 2 + 1 - centerY2;
				if (std::abs(dy2) >= std::abs(dx2))
					sampleY = dy2 < 0 ? r.y - pad : r.y + r.h + pad - 1;
				else
					sampleX = dx2 < 0 ? r.x - pad : r.x + r.w + pad - 1;
			}
			if (sampleX < 0)
				sampleX = 0;
			if (sampleY < 0)
				sampleY = 0;
			if (sampleX >= canvas.width())
				sampleX = canvas.width() - 1;
			if (sampleY >= canvas.height())
				sampleY = canvas.height() - 1;
			const int stride = canvas.strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			return canvas.pixels()[canvas.rowToPhysical(sampleY) * stride + sampleX];
		}

		struct RecolorProtectedBox
		{
			int x0 = 0;
			int y0 = 0;
			int x1 = -1;
			int y1 = -1;
			DisplayCommand command{};
		};

		struct RecolorProtectedSource
		{
			std::uint16_t color = 0;
			int r = 0;
			int g = 0;
			int b = 0;
			int dr = 0;
			int dg = 0;
			int db = 0;
			int wr = 0;
			int wg = 0;
			int wb = 0;
		};

		struct RecolorPixelCacheEntry
		{
			std::uint16_t input = 0;
			std::uint16_t output = 0;
			std::uint8_t valid = 0;
			std::uint8_t changed = 0;
		};

		struct RecolorPixelCache
		{
			static constexpr int kSize = GEA_EMBEDDED_RECOLOR_PIXEL_CACHE_SIZE;
			static_assert(kSize > 0);
			static_assert((kSize & (kSize - 1)) == 0);
			RecolorPixelCacheEntry entries[kSize]{};

			bool lookup(std::uint16_t input, std::uint16_t *output, bool *changed) const
			{
				const auto &entry = entries[index(input)];
				if (!entry.valid || entry.input != input)
					return false;
				if (output)
					*output = entry.output;
				if (changed)
					*changed = entry.changed != 0;
				return true;
			}

			void store(std::uint16_t input, std::uint16_t output, bool changed)
			{
				auto &entry = entries[index(input)];
				entry.input = input;
				entry.output = output;
				entry.valid = 1;
				entry.changed = changed ? 1 : 0;
			}

			void clear()
			{
				for (auto &entry : entries)
					entry.valid = 0;
			}

			static int index(std::uint16_t input)
			{
				const std::uint32_t hash = static_cast<std::uint32_t>(input) * 2654435761u;
				return static_cast<int>((hash >> 23) & (kSize - 1));
			}
		};

		bool recolorProtectedBoxNeedsForegroundReplay(const RecolorProtectedBox &box)
		{
			const int w = box.x1 - box.x0 + 1;
			const int h = box.y1 - box.y0 + 1;
			if (w <= 0 || h <= 0 || w * h < 512)
				return false;
			switch (box.command.type)
			{
			case DisplayCommandType::FillRoundedRect:
			case DisplayCommandType::StrokeRoundedRect:
				return true;
			default:
				return false;
			}
		}

		void replayRecolorProtectedForeground(const RecolorProtectedBox *boxes,
																					int boxCount,
																					int x0,
																					int y0,
																					int x1,
																					int y1)
		{
			if (!boxes || boxCount <= 0 || x0 > x1 || y0 > y1)
				return;
			gea::platform::display::Display::resetClip();
			gea::platform::display::Display::setAlpha(255);
			gea::platform::display::Display::pushClip(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
			for (int i = 0; i < boxCount; ++i)
			{
				const DisplayCommand &command = boxes[i].command;
				if (!commandOverlapsRect(command, x0, y0, x1, y1))
					continue;
				DisplayCommandDrawer::replay(command);
			}
			gea::platform::display::Display::popClip();
			gea::platform::display::Display::setAlpha(255);
		}

		bool displayCommandHasPaintedBounds(const DisplayCommand &command)
		{
			switch (command.type)
			{
			case DisplayCommandType::FillRect:
			case DisplayCommandType::FillCircle:
			case DisplayCommandType::FillRoundedRect:
			case DisplayCommandType::FillQuad:
			case DisplayCommandType::FillLinearGradient:
			case DisplayCommandType::FillRadialGradient:
			case DisplayCommandType::DrawLine:
			case DisplayCommandType::StrokeRect:
			case DisplayCommandType::StrokeRoundedRect:
			case DisplayCommandType::DrawProjectedText:
			case DisplayCommandType::DrawText:
			case DisplayCommandType::BlitImage:
			case DisplayCommandType::BlitImageScaled:
			case DisplayCommandType::FillTransformedLinearGradient:
			case DisplayCommandType::FillTransformedRoundedRect:
				return command.bw > 0 && command.bh > 0;
			default:
				return false;
			}
		}

		void displayCommandRecolorPaintBounds(const DisplayCommand &command, int *x0, int *y0, int *x1, int *y1)
		{
			if (!x0 || !y0 || !x1 || !y1)
				return;
			*x0 = static_cast<int>(command.bx);
			*y0 = static_cast<int>(command.by);
			*x1 = static_cast<int>(command.bx + command.bw - 1);
			*y1 = static_cast<int>(command.by + command.bh - 1);
			if (command.type == DisplayCommandType::DrawText)
			{
				const int pad = std::max(8, static_cast<int>(command.text.scale * 4.0f + 0.5f));
				*x0 -= pad;
				*y0 -= pad;
				*x1 += pad;
				*y1 += pad;
				return;
			}
			if (command.type != DisplayCommandType::StrokeRoundedRect)
				return;
			const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
			if (aaSamples < 2)
				return;
			const auto &r = command.strokeRoundedRect;
			const float kernelWidth = axisRoundedRectAntialiasKernelWidth(aaSamples, r.w, r.h, r.tl, r.tr, r.br, r.bl);
			const int edgePad = axisRoundedRectAntialiasEdgePad(kernelWidth);
			*x0 -= edgePad;
			*y0 -= edgePad;
			*x1 += edgePad;
			*y1 += edgePad;
		}

		bool displayCommandRecolorSourceColor(const DisplayCommand &command, std::uint16_t *outColor)
		{
			if (!outColor)
				return false;
			switch (command.type)
			{
			case DisplayCommandType::FillRect:
				*outColor = command.fill.color;
				return true;
			case DisplayCommandType::FillCircle:
				*outColor = command.fillCircle.color;
				return true;
			case DisplayCommandType::FillRoundedRect:
				*outColor = command.fillRoundedRect.color;
				return true;
			case DisplayCommandType::FillQuad:
				*outColor = command.quad.color;
				return true;
			case DisplayCommandType::DrawLine:
				*outColor = command.line.color;
				return true;
			case DisplayCommandType::StrokeRect:
				*outColor = command.stroke.color;
				return true;
			case DisplayCommandType::StrokeRoundedRect:
				*outColor = command.strokeRoundedRect.color;
				return true;
			case DisplayCommandType::DrawText:
				*outColor = command.text.color;
				return true;
			case DisplayCommandType::DrawProjectedText:
				if (command.projectedText.alpha != 255)
					return false;
				*outColor = command.projectedText.color;
				return true;
			case DisplayCommandType::FillTransformedRoundedRect:
				*outColor = command.transformedRoundedRect.color;
				return true;
			default:
				return false;
			}
		}

		int collectRecolorProtectedBoxes(int originNode,
																		 int shapeX0,
																		 int shapeY0,
																		 int shapeX1,
																		 int shapeY1,
																		 RecolorProtectedBox *boxes,
																		 int capacity,
																		 std::uint16_t *colors,
																		 int colorCapacity,
																		 int *outColorCount)
		{
			if (!boxes || capacity <= 0)
				return -1;
			if (outColorCount)
				*outColorCount = 0;
			int originOrder = -1;
			for (int oi = 0; oi < state.drawNodeOrderCount; ++oi)
			{
				if (state.drawNodeOrder[oi] == originNode)
				{
					originOrder = oi;
					break;
				}
			}
			if (originOrder < 0)
				return -1;

			Tree &tree = Tree::instance();
			Node *nodes = tree.nodes();
			int count = 0;
			for (int oi = originOrder + 1; oi < state.drawNodeOrderCount; ++oi)
			{
				const int nodeId = state.drawNodeOrder[oi];
				if (nodeId < 0 || nodeId >= tree.nodeCount())
					continue;
				const Node &node = nodes[nodeId];
				if (node.style.display == 1)
					continue;
				if (node.style.blink_interval_ms > 0 && !node.style.blink_visible)
					continue;
				if (!state.hasNodeScratchFor(nodeId))
					continue;
				const int start = state.nodeDrawStart[nodeId];
				const int end = state.nodeDrawEnd[nodeId];
				if (start < 0 || end <= start || end > state.commandCount)
					continue;
				for (int ci = start; ci < end; ++ci)
				{
					const DisplayCommand &command = state.commands[ci];
					if (!displayCommandHasPaintedBounds(command))
						continue;
					std::uint16_t sourceColor = 0;
					const bool collectSourceColor = colors && outColorCount && colorCapacity > 0;
					if (collectSourceColor && !displayCommandRecolorSourceColor(command, &sourceColor))
						return -1;
					int paintX0 = 0;
					int paintY0 = 0;
					int paintX1 = -1;
					int paintY1 = -1;
					displayCommandRecolorPaintBounds(command, &paintX0, &paintY0, &paintX1, &paintY1);
					const int bx0 = std::max(paintX0, shapeX0);
					const int by0 = std::max(paintY0, shapeY0);
					const int bx1 = std::min(paintX1, shapeX1);
					const int by1 = std::min(paintY1, shapeY1);
					if (bx0 > bx1 || by0 > by1)
						continue;
					if (collectSourceColor)
					{
						bool seen = false;
						for (int i = 0; i < *outColorCount; ++i)
						{
							if (colors[i] == sourceColor)
							{
								seen = true;
								break;
							}
						}
						if (!seen)
						{
							if (*outColorCount >= colorCapacity)
								return -1;
							colors[(*outColorCount)++] = sourceColor;
						}
					}
					if (count >= capacity)
						return -1;
					boxes[count++] = RecolorProtectedBox{bx0, by0, bx1, by1, command};
				}
			}
			return count;
		}

		bool protectedCommandBlendAt(const DisplayCommand &command, int x, int y, int aaSamples, std::uint16_t *outColor, int *outAlpha)
		{
			if (!outColor || !outAlpha || !displayCommandHasPaintedBounds(command))
				return false;
			if (x < command.bx || y < command.by || x >= command.bx + command.bw || y >= command.by + command.bh)
				return false;
			const int sampleCount = aaSamples >= 2 ? aaSamples * aaSamples : 1;
			switch (command.type)
			{
			case DisplayCommandType::FillRect:
				*outColor = command.fill.color;
				*outAlpha = 255;
				return true;
			case DisplayCommandType::FillCircle:
			{
				const auto &c = command.fillCircle;
				const float dx = static_cast<float>(x) + 0.5f - static_cast<float>(c.cx);
				const float dy = static_cast<float>(y) + 0.5f - static_cast<float>(c.cy);
				if (dx * dx + dy * dy > static_cast<float>(c.r * c.r))
					return false;
				*outColor = c.color;
				*outAlpha = 255;
				return true;
			}
			case DisplayCommandType::FillRoundedRect:
			{
				auto r = command.fillRoundedRect;
				const int maxRadius = std::min(r.w / 2, r.h / 2);
				r.tl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tl), 0, maxRadius));
				r.tr = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tr), 0, maxRadius));
				r.br = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.br), 0, maxRadius));
				r.bl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.bl), 0, maxRadius));
				const bool useAntialias = aaSamples >= 2 && (r.tl || r.tr || r.br || r.bl);
				const int localSampleCount = useAntialias ? sampleCount : 1;
				int coverage = 0;
				if (useAntialias)
				{
					const float kernelWidth = axisRoundedRectAntialiasKernelWidth(aaSamples, r.w, r.h, r.tl, r.tr, r.br, r.bl);
					coverage = axisRoundedRectCoverage(r, x, y, aaSamples, kernelWidth);
				}
				else
				{
					coverage = axisRoundedRectSampleContains(r, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) ? 1 : 0;
				}
				if (coverage <= 0)
					return false;
				*outColor = r.color;
				*outAlpha = combinedCoverageAlpha(255, coverage, localSampleCount);
				return *outAlpha > 0;
			}
			case DisplayCommandType::StrokeRoundedRect:
			{
				auto r = command.strokeRoundedRect;
				if (r.cssRadii) {
					TransformedRoundedRectCommand outer{}, inner{};
					cssRoundedBorderContours(r, outer, inner);
					const int samples = std::max(1, aaSamples);
					const int coverage = cssRoundedBorderCoverage(outer, inner, x, y, samples);
					*outColor = r.color;
					*outAlpha = combinedCoverageAlpha(255, coverage, samples * samples);
					return *outAlpha > 0;
				}
				const int maxRadius = std::min(r.w / 2, r.h / 2);
				r.tl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tl), 0, maxRadius));
				r.tr = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tr), 0, maxRadius));
				r.br = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.br), 0, maxRadius));
				r.bl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.bl), 0, maxRadius));
				r.lineWidth = static_cast<std::int16_t>(std::max(1, static_cast<int>(r.lineWidth)));
				const bool useAntialias = aaSamples >= 2;
				const int localSampleCount = useAntialias ? sampleCount : 1;
				int coverage = 0;
				if (useAntialias)
				{
					const float kernelWidth = axisRoundedRectAntialiasKernelWidth(aaSamples, r.w, r.h, r.tl, r.tr, r.br, r.bl);
					coverage = axisRoundedRectStrokeCoverage(r, x, y, aaSamples, kernelWidth);
				}
				else
				{
					const bool outer = axisRoundedRectSampleContains(r.x,
																													 r.y,
																													 r.w,
																													 r.h,
																													 r.tl,
																													 r.tr,
																													 r.br,
																													 r.bl,
																													 static_cast<float>(x) + 0.5f,
																													 static_cast<float>(y) + 0.5f);
					const int lineWidth = static_cast<int>(r.lineWidth);
					const int innerW = static_cast<int>(r.w) - lineWidth * 2;
					const int innerH = static_cast<int>(r.h) - lineWidth * 2;
					const bool inner = innerW > 0 && innerH > 0 &&
														 axisRoundedRectSampleContains(static_cast<int>(r.x) + lineWidth,
																													 static_cast<int>(r.y) + lineWidth,
																													 innerW,
																													 innerH,
																													 std::max(0, static_cast<int>(r.tl) - lineWidth),
																													 std::max(0, static_cast<int>(r.tr) - lineWidth),
																													 std::max(0, static_cast<int>(r.br) - lineWidth),
																													 std::max(0, static_cast<int>(r.bl) - lineWidth),
																													 static_cast<float>(x) + 0.5f,
																													 static_cast<float>(y) + 0.5f);
					coverage = outer && !inner ? 1 : 0;
				}
				if (coverage <= 0)
					return false;
				*outColor = r.color;
				*outAlpha = combinedCoverageAlpha(255, coverage, localSampleCount);
				return *outAlpha > 0;
			}
			case DisplayCommandType::FillTransformedRoundedRect:
			{
				const auto &r = command.transformedRoundedRect;
				if (r.lw <= 0 || r.lh <= 0 || !transformedRoundedRectLooksAffine(r))
					return false;
				if (r.backfaceHidden)
				{
					const long area2 = static_cast<long>(r.x0) * r.y1 - static_cast<long>(r.x1) * r.y0 +
														 static_cast<long>(r.x1) * r.y2 - static_cast<long>(r.x2) * r.y1 +
														 static_cast<long>(r.x2) * r.y3 - static_cast<long>(r.x3) * r.y2 +
														 static_cast<long>(r.x3) * r.y0 - static_cast<long>(r.x0) * r.y3;
					if (area2 <= 0)
						return false;
				}
				const float invLocalW = 1.0f / static_cast<float>(r.lw);
				const float invLocalH = 1.0f / static_cast<float>(r.lh);
				const float a = static_cast<float>(r.x1 - r.x0) * invLocalW;
				const float b = static_cast<float>(r.x3 - r.x0) * invLocalH;
				const float c = static_cast<float>(r.y1 - r.y0) * invLocalW;
				const float d = static_cast<float>(r.y3 - r.y0) * invLocalH;
				const float det = a * d - b * c;
				if (std::fabs(det) < 1e-6f)
					return false;
				const float invDet = 1.0f / det;
				const float inv00 = d * invDet;
				const float inv01 = -b * invDet;
				const float inv10 = -c * invDet;
				const float inv11 = a * invDet;
				const float py = static_cast<float>(y) + 0.5f - static_cast<float>(r.y0);
				const float localX = static_cast<float>(r.lx) + inv00 * (static_cast<float>(x) + 0.5f - static_cast<float>(r.x0)) + inv01 * py;
				const float localY = static_cast<float>(r.ly) + inv10 * (static_cast<float>(x) + 0.5f - static_cast<float>(r.x0)) + inv11 * py;
				const int coverage = aaSamples >= 2
																 ? roundedRectCoverageFast(r, localX, localY, inv00, inv10, inv01, inv11, aaSamples)
																 : (roundedRectContainsFast(r, localX, localY) ? 1 : 0);
				if (coverage <= 0)
					return false;
				*outColor = r.color;
				*outAlpha = combinedCoverageAlpha(255, coverage, sampleCount);
				return *outAlpha > 0;
			}
			default:
				return false;
			}
		}

		bool recolorProtectedBackgroundPixel(std::uint16_t &pixel,
																				 int x,
																				 int y,
																				 std::uint16_t oldColor,
																				 std::uint16_t newColor,
																				 const RecolorProtectedBox *boxes,
																				 int boxCount,
																				 int aaSamples)
		{
			if (pixel == oldColor)
			{
				pixel = newColor;
				return true;
			}
			for (int i = boxCount - 1; i >= 0; --i)
			{
				const RecolorProtectedBox &box = boxes[i];
				if (x < box.x0 || x > box.x1 || y < box.y0 || y > box.y1)
					continue;
				std::uint16_t source = 0;
				int alpha = 0;
				if (!protectedCommandBlendAt(box.command, x, y, aaSamples, &source, &alpha))
					continue;
				if (alpha >= 255)
					return false;
				const std::uint16_t oldComposite = gea::framework::graphics::pixel::blend(source, oldColor, alpha);
				if (rgb565Distance(pixel, oldComposite) > 48)
					continue;
				const std::uint16_t next = gea::framework::graphics::pixel::blend(source, newColor, alpha);
				if (pixel == next)
					return false;
				pixel = next;
				return true;
			}
			return false;
		}

		bool recolorProtectedCompositeByColor(std::uint16_t &pixel,
																					std::uint16_t oldColor,
																					std::uint16_t newColor,
																					int oldR,
																					int oldG,
																					int oldB,
																					const RecolorProtectedSource *sources,
																					int sourceCount,
																					RecolorPixelCache *cache)
		{
			if (pixel == oldColor)
			{
				pixel = newColor;
				return true;
			}
			if (!sources || sourceCount <= 0)
				return false;
			const std::uint16_t inputPixel = pixel;
			if (cache)
			{
				std::uint16_t cachedOutput = inputPixel;
				bool cachedChanged = false;
				if (cache->lookup(inputPixel, &cachedOutput, &cachedChanged))
				{
					if (cachedChanged)
						pixel = cachedOutput;
					return cachedChanged;
				}
			}
			for (int i = 0; i < sourceCount; ++i)
			{
				if (pixel == sources[i].color)
				{
					if (cache)
						cache->store(inputPixel, inputPixel, false);
					return false;
				}
			}

			int pxR = 0, pxG = 0, pxB = 0;
			unpackRgb565To888(pixel, &pxR, &pxG, &pxB);
			for (int i = 0; i < sourceCount; ++i)
			{
				const RecolorProtectedSource &source = sources[i];
				int alphaSum = 0;
				int alphaWeight = 0;
				if (source.wr > 0)
				{
					const int alpha = std::clamp(((pxR - oldR) * 255 + source.dr / 2) / source.dr, 0, 255);
					alphaSum += alpha * source.wr;
					alphaWeight += source.wr;
				}
				if (source.wg > 0)
				{
					const int alpha = std::clamp(((pxG - oldG) * 255 + source.dg / 2) / source.dg, 0, 255);
					alphaSum += alpha * source.wg;
					alphaWeight += source.wg;
				}
				if (source.wb > 0)
				{
					const int alpha = std::clamp(((pxB - oldB) * 255 + source.db / 2) / source.db, 0, 255);
					alphaSum += alpha * source.wb;
					alphaWeight += source.wb;
				}
				if (alphaWeight <= 0)
					continue;
				int alpha = (alphaSum + alphaWeight / 2) / alphaWeight;
				if (alpha <= 0 || alpha >= 255)
					continue;

				std::uint16_t oldComposite = gea::framework::graphics::pixel::blend(source.color, oldColor, alpha);
				if (rgb565Distance(pixel, oldComposite) > 56)
				{
					bool matched = false;
					for (int delta : {-16, -8, 8, 16})
					{
						const int adjustedAlpha = std::clamp(alpha + delta, 1, 254);
						oldComposite = gea::framework::graphics::pixel::blend(source.color, oldColor, adjustedAlpha);
						if (rgb565Distance(pixel, oldComposite) <= 56)
						{
							alpha = adjustedAlpha;
							matched = true;
							break;
						}
					}
					if (!matched)
						continue;
				}
				const std::uint16_t next = gea::framework::graphics::pixel::blend(source.color, newColor, alpha);
				if (next == pixel)
				{
					if (cache)
						cache->store(inputPixel, inputPixel, false);
					return false;
				}
				pixel = next;
				if (cache)
					cache->store(inputPixel, next, true);
				return true;
			}
			if (cache)
				cache->store(inputPixel, inputPixel, false);
			return false;
		}

		int recolorSolidSpanWithProtectedBoxes(gea::framework::graphics::pixel::native_t *row,
																					 int y,
																					 int x0,
																					 int x1,
																					 gea::framework::graphics::pixel::native_t oldColor,
																					 gea::framework::graphics::pixel::native_t newColor,
																					 const RecolorProtectedBox *boxes,
																					 int boxCount,
																					 const RecolorProtectedSource *sources,
																					 int sourceCount,
																					 RecolorPixelCache *cache)
		{
			if (x0 > x1)
				return 0;
			if (!boxes || boxCount <= 0)
			{
				fillRgb565SpanFast(row + x0, x1 - x0 + 1, newColor);
				return x1 - x0 + 1;
			}

			// On RGB panels the framebuffer can be the scanout buffer. A destructive
			// "fill around protected boxes, then replay foreground" path exposes the
			// background while the panel is scanning. Replace only exact old background
			// pixels here; large foreground shapes are replayed after the recolor to refresh
			// their AA edge, but they are never blanked in between.
			(void)y;
			(void)sources;
			(void)sourceCount;
			(void)cache;
			return replaceRgb565SpanFast(row + x0, x1 - x0 + 1, oldColor, newColor);
		}

		bool recolorRetainedRoundedRect(int node,
																		const DisplayCommand &command,
																		gea::framework::graphics::pixel::native_t oldColor,
																		gea::framework::graphics::pixel::native_t newColor,
																		int *outX0,
																		int *outY0,
																		int *outX1,
																		int *outY1)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return false;
			auto r = command.fillRoundedRect;
			const int maxRadius = std::min(r.w / 2, r.h / 2);
			r.tl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tl), 0, maxRadius));
			r.tr = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.tr), 0, maxRadius));
			r.br = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.br), 0, maxRadius));
			r.bl = static_cast<std::int16_t>(std::clamp(static_cast<int>(r.bl), 0, maxRadius));
			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			RoundedRectScreenSpan span{
					r.x, r.y, r.w, r.h,
					r.tl * 8, r.tl * 8,
					r.tr * 8, r.tr * 8,
					r.br * 8, r.br * 8,
					r.bl * 8, r.bl * 8};
			const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
			const bool useAntialias = aaSamples >= 2 && (r.tl || r.tr || r.br || r.bl);
			const int sampleCount = useAntialias ? aaSamples * aaSamples : 1;
			const float kernelWidth = useAntialias ? axisRoundedRectAntialiasKernelWidth(aaSamples, r.w, r.h, r.tl, r.tr, r.br, r.bl) : 1.0f;
			const int edgePad = useAntialias ? axisRoundedRectAntialiasEdgePad(kernelWidth) : 0;
			const bool fastInterior = useAntialias &&
																static_cast<int>(r.w) * static_cast<int>(r.h) >= kRetainedRoundedRectFastInteriorArea;
			if (useAntialias && !fastInterior)
			{
				// Small AA circles/pills are cheaper to repaint through the normal dirty-region
				// path than to reverse-blend their retained edge pixels.
				return false;
			}
			const bool protectForegroundDuringFill = fastInterior;
			constexpr int kMaxProtectedBoxes = 128;
			RecolorProtectedBox protectedBoxes[kMaxProtectedBoxes];
			int protectedBoxCount = 0;
			if (protectForegroundDuringFill)
			{
				protectedBoxCount = collectRecolorProtectedBoxes(node,
																												 r.x,
																												 r.y,
																												 r.x + r.w - 1,
																												 r.y + r.h - 1,
																												 protectedBoxes,
																												 kMaxProtectedBoxes,
																												 nullptr,
																												 0,
																												 nullptr);
				if (protectedBoxCount < 0)
					return false;
			}
			const bool replayForegroundAfterFill = false;
			int foregroundReplayX0 = canvas->width();
			int foregroundReplayY0 = canvas->height();
			int foregroundReplayX1 = -1;
			int foregroundReplayY1 = -1;
			if (protectForegroundDuringFill)
			{
				for (int i = 0; i < protectedBoxCount; ++i)
				{
					if (!recolorProtectedBoxNeedsForegroundReplay(protectedBoxes[i]))
						continue;
					if (protectedBoxes[i].x0 < foregroundReplayX0)
						foregroundReplayX0 = protectedBoxes[i].x0;
					if (protectedBoxes[i].y0 < foregroundReplayY0)
						foregroundReplayY0 = protectedBoxes[i].y0;
					if (protectedBoxes[i].x1 > foregroundReplayX1)
						foregroundReplayX1 = protectedBoxes[i].x1;
					if (protectedBoxes[i].y1 > foregroundReplayY1)
						foregroundReplayY1 = protectedBoxes[i].y1;
				}
			}
			int y0 = span.y;
			int y1 = span.y + span.h - 1;
			if (y0 < 0)
				y0 = 0;
			if (y1 >= canvas->height())
				y1 = canvas->height() - 1;
			if (y0 > y1)
				return false;
			const std::int64_t totalStartUs = refreshPerfNowUs();
			FillBandDirty dMain{canvas->width(), canvas->height(), -1, -1};
			FillBandDirty dWork{canvas->width(), canvas->height(), -1, -1};
			static RecolorPixelCache protectedPixelCaches[2];
			if (protectForegroundDuringFill)
			{
				protectedPixelCaches[0].clear();
				protectedPixelCaches[1].clear();
			}
			auto recolorBand = [&](int bandY0, int bandY1, FillBandDirty &dirty)
			{
				RecolorPixelCache *protectedPixelCache = protectForegroundDuringFill
																										 ? (&dirty == &dWork ? &protectedPixelCaches[1] : &protectedPixelCaches[0])
																										 : nullptr;
				auto markDirty = [&](int x0, int y, int x1)
				{
					if (x0 < dirty.x0)
						dirty.x0 = x0;
					if (x1 > dirty.x1)
						dirty.x1 = x1;
					if (y < dirty.y0)
						dirty.y0 = y;
					if (y > dirty.y1)
						dirty.y1 = y;
				};
				for (int y = bandY0; y <= bandY1; y++)
				{
					int rowX0 = 0;
					int rowX1 = -1;
					gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
					const std::int64_t fillStartUs = refreshPerfNowUs();
					const bool hasSolidSpan = roundedRectRasterSolidSpan(r, y, &rowX0, &rowX1);
					if (hasSolidSpan)
					{
						if (rowX0 < 0)
							rowX0 = 0;
						if (rowX1 >= canvas->width())
							rowX1 = canvas->width() - 1;
						if (rowX0 <= rowX1)
						{
							if (fastInterior)
							{
								int fillX0 = rowX0 + edgePad + 1;
								int fillX1 = rowX1 - edgePad - 1;
								if (fillX0 < 0)
									fillX0 = 0;
								if (fillX1 >= canvas->width())
									fillX1 = canvas->width() - 1;
								if (fillX0 <= fillX1)
								{
									int changed = 0;
									if (protectForegroundDuringFill)
									{
										changed = recolorSolidSpanWithProtectedBoxes(row,
																																 y,
																																 fillX0,
																																 fillX1,
																																 oldColor,
																																 newColor,
																																 protectedBoxes,
																																 protectedBoxCount,
																																 nullptr,
																																 0,
																																 protectedPixelCache);
									}
									else if (replayForegroundAfterFill)
									{
										fillRgb565SpanFast(row + fillX0, fillX1 - fillX0 + 1, newColor);
										changed = fillX1 - fillX0 + 1;
									}
									else
									{
										changed = replaceRgb565SpanFast(row + fillX0, fillX1 - fillX0 + 1, oldColor, newColor);
									}
									if (changed > 0)
									{
										dirty.fastPixels += changed;
										markDirty(fillX0, y, fillX1);
									}
								}
							}
							else
							{
								for (int x = rowX0; x <= rowX1; x++)
								{
									if (row[x] != oldColor)
										continue;
									row[x] = newColor;
									markDirty(x, y, x);
								}
							}
						}
					}
					dirty.fillUs += refreshPerfNowUs() - fillStartUs;
					if (!useAntialias)
						continue;

					const std::int64_t edgeStartUs = refreshPerfNowUs();
					int coverageX0 = 0x7fffffff;
					int coverageX1 = -0x7fffffff;
					axisRoundedRectAnyCoverageSpan(r, y, aaSamples, kernelWidth, &coverageX0, &coverageX1);
					if (coverageX0 < static_cast<int>(r.x))
						coverageX0 = static_cast<int>(r.x);
					if (coverageX1 > static_cast<int>(r.x) + static_cast<int>(r.w) - 1)
						coverageX1 = static_cast<int>(r.x) + static_cast<int>(r.w) - 1;
					if (coverageX0 < 0)
						coverageX0 = 0;
					if (coverageX1 >= canvas->width())
						coverageX1 = canvas->width() - 1;
					if (coverageX0 > coverageX1)
						continue;
					auto recolorCoverageRange = [&](int fromX, int toX)
					{
						if (fromX < coverageX0)
							fromX = coverageX0;
						if (toX > coverageX1)
							toX = coverageX1;
						if (fromX > toX)
							return;
						for (int x = fromX; x <= toX; ++x)
						{
							const int coverage = axisRoundedRectCoverage(r, x, y, aaSamples, kernelWidth);
							const int alpha = combinedCoverageAlpha(255, coverage, sampleCount);
							if (alpha <= 0)
								continue;
							const gea::framework::graphics::pixel::native_t fallbackBackdrop = roundedRectCoverageFallbackBackdrop(*canvas, r, x, y, edgePad);
							if (!recolorRoundedRectCoveragePixel(row[x], oldColor, newColor, alpha, fallbackBackdrop))
								continue;
							markDirty(x, y, x);
						}
					};
					if (!hasSolidSpan || rowX0 > rowX1 || rowX0 + edgePad >= rowX1 - edgePad)
					{
						recolorCoverageRange(coverageX0, coverageX1);
					}
					else
					{
						const int leftEdgeEnd = std::min(coverageX1, rowX0 + edgePad);
						const int rightEdgeStart = std::max(coverageX0, rowX1 - edgePad);
						if (leftEdgeEnd >= rightEdgeStart)
						{
							recolorCoverageRange(coverageX0, coverageX1);
						}
						else
						{
							recolorCoverageRange(coverageX0, leftEdgeEnd);
							recolorCoverageRange(rightEdgeStart, coverageX1);
						}
					}
					dirty.edgeUs += refreshPerfNowUs() - edgeStartUs;
				}
			};
			parallelFillRows(y0, y1, dMain, dWork, recolorBand);
			auto mergeDirty = [](FillBandDirty &dst, const FillBandDirty &src)
			{
				if (src.x0 > src.x1 || src.y0 > src.y1)
					return;
				if (src.x0 < dst.x0)
					dst.x0 = src.x0;
				if (src.y0 < dst.y0)
					dst.y0 = src.y0;
				if (src.x1 > dst.x1)
					dst.x1 = src.x1;
				if (src.y1 > dst.y1)
					dst.y1 = src.y1;
				dst.fastPixels += src.fastPixels;
				dst.fillUs += src.fillUs;
				dst.edgeUs += src.edgeUs;
			};
			mergeDirty(dMain, dWork);
			std::int64_t replayUs = 0;
			if (foregroundReplayX0 <= foregroundReplayX1 && foregroundReplayY0 <= foregroundReplayY1)
			{
				if (foregroundReplayX0 < 0)
					foregroundReplayX0 = 0;
				if (foregroundReplayY0 < 0)
					foregroundReplayY0 = 0;
				if (foregroundReplayX1 >= canvas->width())
					foregroundReplayX1 = canvas->width() - 1;
				if (foregroundReplayY1 >= canvas->height())
					foregroundReplayY1 = canvas->height() - 1;
				if (foregroundReplayX0 <= foregroundReplayX1 && foregroundReplayY0 <= foregroundReplayY1)
				{
					gea::platform::display::Display::resetClip();
					gea::platform::display::Display::setAlpha(255);
					// Establish the correct new background under foreground AA edges before
					// replaying. replaceRgb565SpanFast above skips pixels that don't exactly
					// match oldColor (e.g. blended AA pixels of a foreground pill), leaving
					// them stale. The foreground replay then blends against those wrong pixels.
					// Filling with newColor first ensures every AA edge in the foreground
					// replay region blends against the correct new background color.
					gea::platform::display::Display::fillRect(foregroundReplayX0,
																										foregroundReplayY0,
																										foregroundReplayX1 - foregroundReplayX0 + 1,
																										foregroundReplayY1 - foregroundReplayY0 + 1,
																										newColor);
					const DisplayReplayRegion foregroundRegion{
							foregroundReplayX0,
							foregroundReplayY0,
							foregroundReplayX1,
							foregroundReplayY1,
							node};
					const std::int64_t replayStartUs = refreshPerfNowUs();
					replayRecolorProtectedForeground(protectedBoxes,
																					 protectedBoxCount,
																					 foregroundRegion.x0,
																					 foregroundRegion.y0,
																					 foregroundRegion.x1,
																					 foregroundRegion.y1);
					replayUs += refreshPerfNowUs() - replayStartUs;
					if (foregroundReplayX0 < dMain.x0)
						dMain.x0 = foregroundReplayX0;
					if (foregroundReplayY0 < dMain.y0)
						dMain.y0 = foregroundReplayY0;
					if (foregroundReplayX1 > dMain.x1)
						dMain.x1 = foregroundReplayX1;
					if (foregroundReplayY1 > dMain.y1)
						dMain.y1 = foregroundReplayY1;
				}
			}
			const int dirtyX0 = dMain.x0;
			const int dirtyY0 = dMain.y0;
			const int dirtyX1 = dMain.x1;
			const int dirtyY1 = dMain.y1;
			refreshPerfStatsMutable().treeBgRecolorFastPixels += dMain.fastPixels;
			refreshPerfStatsMutable().treeBgRecolorFillUs += dMain.fillUs;
			refreshPerfStatsMutable().treeBgRecolorEdgeUs += dMain.edgeUs;
			if (dirtyX0 > dirtyX1 || dirtyY0 > dirtyY1)
				return false;
			canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
			if (replayForegroundAfterFill)
			{
				gea::platform::display::Display::resetClip();
				gea::platform::display::Display::setAlpha(255);
				const DisplayReplayRegion region{dirtyX0, dirtyY0, dirtyX1, dirtyY1, node};
				const std::int64_t replayStartUs = refreshPerfNowUs();
				if (!replayLaterOverlappingCommands(node, region))
					return false;
				replayUs = refreshPerfNowUs() - replayStartUs;
				refreshPerfStatsMutable().treeBgRecolorReplayUs += replayUs;
			}
			refreshPerfStatsMutable().treeBgRecolorUs += refreshPerfNowUs() - totalStartUs;
			if (outX0)
				*outX0 = dirtyX0;
			if (outY0)
				*outY0 = dirtyY0;
			if (outX1)
				*outX1 = dirtyX1;
			if (outY1)
				*outY1 = dirtyY1;
			return true;
		}

		bool recolorRetainedTransformedRoundedRect(const DisplayCommand &command,
																							 uint16_t oldColor,
																							 uint16_t newColor,
																							 int *outX0,
																							 int *outY0,
																							 int *outX1,
																							 int *outY1)
		{
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !canvas->pixels() || canvas->width() <= 0 || canvas->height() <= 0)
				return false;
			const auto &r = command.transformedRoundedRect;
			if (r.lw <= 0 || r.lh <= 0)
				return false;
			if (!transformedRoundedRectLooksAffine(r))
				return false;
			if (r.backfaceHidden)
			{
				const long area2 = static_cast<long>(r.x0) * r.y1 - static_cast<long>(r.x1) * r.y0 +
													 static_cast<long>(r.x1) * r.y2 - static_cast<long>(r.x2) * r.y1 +
													 static_cast<long>(r.x2) * r.y3 - static_cast<long>(r.x3) * r.y2 +
													 static_cast<long>(r.x3) * r.y0 - static_cast<long>(r.x0) * r.y3;
				if (area2 <= 0)
					return false;
			}

			const int coverageArea = static_cast<int>(command.bw) * static_cast<int>(command.bh);
			const int aaSamples = gea::framework::graphics::Canvas::antialiasSamples();
			const bool useCoverageAntialias = aaSamples >= 2 && coverageArea > 0 && coverageArea <= kAntialiasFullCoverageAreaLimit;
			const int sampleCount = useCoverageAntialias ? aaSamples * aaSamples : 1;
			if (useCoverageAntialias)
				return false;
			RoundedRectScreenSpan span{};
			if (!useCoverageAntialias && transformedRoundedRectToScreenSpan(r, &span))
			{
				const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
				int y0 = span.y;
				int y1 = span.y + span.h - 1;
				if (y0 < 0)
					y0 = 0;
				if (y1 >= canvas->height())
					y1 = canvas->height() - 1;
				if (y0 > y1)
					return false;
				int dirtyX0 = canvas->width();
				int dirtyY0 = canvas->height();
				int dirtyX1 = -1;
				int dirtyY1 = -1;
				for (int y = y0; y <= y1; ++y)
				{
					int rowX0 = 0;
					int rowX1 = -1;
					if (!roundedRectRowSpan(span, y, &rowX0, &rowX1))
						continue;
					if (rowX0 < 0)
						rowX0 = 0;
					if (rowX1 >= canvas->width())
						rowX1 = canvas->width() - 1;
					if (rowX0 > rowX1)
						continue;
					gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
					for (int x = rowX0; x <= rowX1; ++x)
					{
						if (row[x] != oldColor)
							continue;
						row[x] = newColor;
						if (x < dirtyX0)
							dirtyX0 = x;
						if (x > dirtyX1)
							dirtyX1 = x;
						if (y < dirtyY0)
							dirtyY0 = y;
						if (y > dirtyY1)
							dirtyY1 = y;
					}
				}
				if (dirtyX0 > dirtyX1 || dirtyY0 > dirtyY1)
					return false;
				canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
				if (outX0)
					*outX0 = dirtyX0;
				if (outY0)
					*outY0 = dirtyY0;
				if (outX1)
					*outX1 = dirtyX1;
				if (outY1)
					*outY1 = dirtyY1;
				return true;
			}

			const float invLocalW = 1.0f / static_cast<float>(r.lw);
			const float invLocalH = 1.0f / static_cast<float>(r.lh);
			const float a = static_cast<float>(r.x1 - r.x0) * invLocalW;
			const float b = static_cast<float>(r.x3 - r.x0) * invLocalH;
			const float c = static_cast<float>(r.y1 - r.y0) * invLocalW;
			const float d = static_cast<float>(r.y3 - r.y0) * invLocalH;
			const float det = a * d - b * c;
			if (std::fabs(det) < 1e-6f)
				return false;
			const float invDet = 1.0f / det;
			const float inv00 = d * invDet;
			const float inv01 = -b * invDet;
			const float inv10 = -c * invDet;
			const float inv11 = a * invDet;

			int drawX0 = command.bx;
			int drawY0 = command.by;
			int drawX1 = command.bx + command.bw - 1;
			int drawY1 = command.by + command.bh - 1;
			if (drawX0 < 0)
				drawX0 = 0;
			if (drawY0 < 0)
				drawY0 = 0;
			if (drawX1 >= canvas->width())
				drawX1 = canvas->width() - 1;
			if (drawY1 >= canvas->height())
				drawY1 = canvas->height() - 1;
			if (drawX0 > drawX1 || drawY0 > drawY1)
				return false;

			const int stride = canvas->strideBytes() / static_cast<int>(sizeof(gea::framework::graphics::pixel::native_t));
			int dirtyX0 = canvas->width();
			int dirtyY0 = canvas->height();
			int dirtyX1 = -1;
			int dirtyY1 = -1;
			float sampleMarginX = 0.0f;
			float sampleMarginY = 0.0f;
			if (useCoverageAntialias)
			{
				const float kernelWidth = transformedRoundedRectAntialiasKernelWidth(aaSamples, r);
				const float minOffset = antialiasOffsetWithKernel(0, aaSamples, kernelWidth) - 0.5f;
				const float maxOffset = antialiasOffsetWithKernel(aaSamples - 1, aaSamples, kernelWidth) - 0.5f;
				const float offsetX[2] = {minOffset, maxOffset};
				const float offsetY[2] = {minOffset, maxOffset};
				for (float dx : offsetX)
				{
					for (float dy : offsetY)
					{
						sampleMarginX = std::max(sampleMarginX, std::fabs(inv00 * dx + inv01 * dy));
						sampleMarginY = std::max(sampleMarginY, std::fabs(inv10 * dx + inv11 * dy));
					}
				}
			}
			for (int y = drawY0; y <= drawY1; ++y)
			{
				gea::framework::graphics::pixel::native_t *row = canvas->pixels() + canvas->rowToPhysical(y) * stride;
				const float py = static_cast<float>(y) + 0.5f - static_cast<float>(r.y0);
				float localX = static_cast<float>(r.lx) + inv00 * (static_cast<float>(drawX0) + 0.5f - static_cast<float>(r.x0)) + inv01 * py;
				float localY = static_cast<float>(r.ly) + inv10 * (static_cast<float>(drawX0) + 0.5f - static_cast<float>(r.x0)) + inv11 * py;
				for (int x = drawX0; x <= drawX1; ++x)
				{
					const int coverage = useCoverageAntialias
																	 ? (roundedRectSolidCoreContainsFast(r, localX, localY, sampleMarginX, sampleMarginY)
																					? sampleCount
																					: roundedRectCoverageFast(r, localX, localY, inv00, inv10, inv01, inv11, aaSamples))
																	 : (roundedRectContainsFast(r, localX, localY) ? 1 : 0);
					bool changed = false;
					if (coverage > 0)
					{
						if (useCoverageAntialias)
						{
							const int alpha = combinedCoverageAlpha(255, coverage, sampleCount);
							if (alpha >= 255 || row[x] == oldColor)
							{
								if (row[x] == oldColor)
								{
									row[x] = newColor;
									changed = true;
								}
							}
							else
							{
								changed = recolorRoundedRectCoveragePixel(row[x], oldColor, newColor, alpha, 0x0000);
							}
						}
						else if (row[x] == oldColor)
						{
							row[x] = newColor;
							changed = true;
						}
					}
					if (changed)
					{
						if (x < dirtyX0)
							dirtyX0 = x;
						if (x > dirtyX1)
							dirtyX1 = x;
						if (y < dirtyY0)
							dirtyY0 = y;
						if (y > dirtyY1)
							dirtyY1 = y;
					}
					localX += inv00;
					localY += inv10;
				}
			}
			if (dirtyX0 > dirtyX1 || dirtyY0 > dirtyY1)
				return false;
			canvas->markDirty(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
			if (outX0)
				*outX0 = dirtyX0;
			if (outY0)
				*outY0 = dirtyY0;
			if (outX1)
				*outX1 = dirtyX1;
			if (outY1)
				*outY1 = dirtyY1;
			return true;
		}

	} // namespace

	bool DisplayList::recolorRetainedSolidBackground(int node, gea::framework::graphics::pixel::native_t oldColor, gea::framework::graphics::pixel::native_t newColor, int *x0, int *y0, int *x1, int *y1)
	{
		if (hasTextClippedBackgrounds()) return false;
		Tree &tree = Tree::instance();
		if (!state.commands || node < 0 || node >= tree.nodeCount())
			return false;
		if (!state.hasNodeScratchFor(node))
			return false;
		const int start = state.nodeDrawStart[node];
		const int end = state.nodeDrawEnd[node];
		if (start < 0 || end <= start || end > state.commandCount)
			return false;
		for (int ci = start; ci < end; ci++)
		{
			const DisplayCommand &command = state.commands[ci];
			const bool candidate =
					(command.type == DisplayCommandType::FillRect && command.fill.color == newColor) ||
					(command.type == DisplayCommandType::FillRoundedRect && command.fillRoundedRect.color == newColor) ||
					(command.type == DisplayCommandType::FillTransformedRoundedRect && command.transformedRoundedRect.color == newColor);
			if (!candidate)
				continue;
			const bool laterSafe = laterOverlappingCommandsAvoidColor(node, command, oldColor);
			if (!laterSafe)
				return false;
			bool ok = false;
			if (command.type == DisplayCommandType::FillRect)
			{
				ok = recolorRetainedFillRect(command, oldColor, newColor, x0, y0, x1, y1);
			}
			else if (command.type == DisplayCommandType::FillTransformedRoundedRect)
			{
				ok = recolorRetainedTransformedRoundedRect(command, oldColor, newColor, x0, y0, x1, y1);
			}
			else
			{
				ok = recolorRetainedRoundedRect(node, command, oldColor, newColor, x0, y0, x1, y1);
			}
			if (ok && state.retainedBackgroundRecolorCount < DisplayListState::kMaxRetainedBackgroundRecolors &&
					(command.type == DisplayCommandType::FillRect || command.type == DisplayCommandType::FillRoundedRect))
			{
				auto &entry = state.retainedBackgroundRecolors[state.retainedBackgroundRecolorCount++];
				entry.node = static_cast<int16_t>(node);
				entry.command = command;
			}
			return ok;
		}
		return false;
	}

	// See internal.h: a recolor leaves the antialiased edges of everything drawn over
	// the background blended against the OLD colour. Report the later-drawn nodes'
	// paint bounds so the caller can replay them.
	int DisplayList::collectRecolorForegroundRepairRects(int node, int rx0, int ry0, int rx1, int ry1,
																											 int *outRects, int maxRects) const
	{
		if (!outRects || maxRects <= 0 || !state.commands || rx0 > rx1 || ry0 > ry1)
			return 0;
		Tree &tree = Tree::instance();
		if (node < 0 || node >= tree.nodeCount())
			return 0;
		int originOrder = -1;
		for (int oi = 0; oi < state.drawNodeOrderCount; ++oi)
		{
			if (state.drawNodeOrder[oi] == node)
			{
				originOrder = oi;
				break;
			}
		}
		if (originOrder < 0)
			return 0;

		Node *nodes = tree.nodes();
		int count = 0;
		for (int oi = originOrder + 1; oi < state.drawNodeOrderCount; ++oi)
		{
			const int nodeId = state.drawNodeOrder[oi];
			if (nodeId < 0 || nodeId >= tree.nodeCount())
				continue;
			const Node &n = nodes[nodeId];
			if (n.style.display == 1)
				continue;
			if (n.style.blink_interval_ms > 0 && !n.style.blink_visible)
				continue;
			if (!state.hasNodeScratchFor(nodeId))
				continue;
			const int start = state.nodeDrawStart[nodeId];
			const int end = state.nodeDrawEnd[nodeId];
			if (start < 0 || end <= start || end > state.commandCount)
				continue;
			int bx0 = 0x7fffffff;
			int by0 = 0x7fffffff;
			int bx1 = -0x7fffffff;
			int by1 = -0x7fffffff;
			for (int ci = start; ci < end; ++ci)
			{
				const DisplayCommand &command = state.commands[ci];
				if (!displayCommandHasPaintedBounds(command))
					continue;
				int px0 = 0;
				int py0 = 0;
				int px1 = -1;
				int py1 = -1;
				// Same bounds the protected-box collector uses: glyph runs and antialiased
				// strokes are padded so the whole blended edge is covered.
				displayCommandRecolorPaintBounds(command, &px0, &py0, &px1, &py1);
				if (px0 > px1 || py0 > py1)
					continue;
				if (px1 < rx0 || px0 > rx1 || py1 < ry0 || py0 > ry1)
					continue;
				if (px0 < bx0)
					bx0 = px0;
				if (py0 < by0)
					by0 = py0;
				if (px1 > bx1)
					bx1 = px1;
				if (py1 > by1)
					by1 = py1;
			}
			if (bx0 > bx1 || by0 > by1)
				continue;
			if (count < maxRects)
			{
				outRects[count * 4 + 0] = bx0;
				outRects[count * 4 + 1] = by0;
				outRects[count * 4 + 2] = bx1;
				outRects[count * 4 + 3] = by1;
				++count;
				continue;
			}
			// Out of slots: widen the last rect rather than dropping a node — a repair
			// region that is too large still replays correct pixels, a missing one does not.
			int *last = outRects + (maxRects - 1) * 4;
			if (bx0 < last[0])
				last[0] = bx0;
			if (by0 < last[1])
				last[1] = by0;
			if (bx1 > last[2])
				last[2] = bx1;
			if (by1 > last[3])
				last[3] = by1;
		}
		return count;
	}

	bool DisplayList::rerecordNodeCommands(int node)
	{
		Tree &tree = Tree::instance();
		Node *nodes = tree.nodes();
		if (!state.commands || node < 0 || node >= tree.nodeCount())
			return false;
		if (!state.hasNodeScratchFor(node))
			return false;
		int start = state.nodeDrawStart[node];
		int end = state.nodeDrawEnd[node];
		if (start < 0 || end < start || end > state.commandCount)
			return false;

		DisplayCommand tmp[96];
		int tmp_len = 0;
		DisplayCommand *prev_override = state.appendOverride;
		int *prev_override_len = state.appendOverrideCount;
		int prev_override_cap = state.appendOverrideCapacity;
		state.appendOverride = tmp;
		state.appendOverrideCount = &tmp_len;
		state.appendOverrideCapacity = (int)(sizeof(tmp) / sizeof(tmp[0]));

		Node *n = &nodes[node];
		// recordNode gates every paint on `overlaps_clip` — a node outside its
		// ancestors' overflow clips contributes NO commands and keeps an empty range.
		// This in-place re-record is that same per-node emission, so it owes the same
		// gate: without it a clipped-out node re-recorded a full background fill into
		// an empty range, which `old_len == 0 && tmp_len > 0` below (correctly) refuses
		// to splice — and the refusal cost the whole frame its retained display list.
		const bool outsideRecordClip = !isDocumentCanvasRoot(*n) && nodeOutsideRecordClip(node, tree.mountedWidth(), tree.mountedHeight());
		if (!outsideRecordClip && n->style.visibility == 0 && !isCollapsedFlexSubtree(*n) && !ViewRenderer::backfaceSubtreeHidden(*n) &&
				n->style.display != 1 &&
				n->style.opacity != 0 &&
				!(n->style.blink_interval_ms > 0 && !n->style.blink_visible) &&
				!isNativeTextInputView(*n))
		{
			ViewRenderer::recordBox(*n);
			if (n->type == NodeType::Text)
				TextRenderer::record(*n);
			else if (n->type == NodeType::Image)
				ImageRenderer::record(*n);
			else if (n->type == NodeType::Canvas)
				CanvasRenderer::record(*n);
			else if (n->type == NodeType::Camera)
				CameraRenderer::record(*n);
		}

		state.appendOverride = prev_override;
		state.appendOverrideCount = prev_override_len;
		state.appendOverrideCapacity = prev_override_cap;

		int old_len = end - start;
		int delta = tmp_len - old_len;
		if (old_len == 0 && tmp_len > 0)
			return false;
		if (delta > 0 && state.commandCount + delta > DisplayListState::kMaxCommands)
			return false;
		if (delta != 0 && end < state.commandCount)
		{
			std::memmove(&state.commands[end + delta], &state.commands[end],
									 sizeof(state.commands[0]) * (size_t)(state.commandCount - end));
		}
		if (tmp_len > 0)
			std::memcpy(&state.commands[start], tmp, sizeof(tmp[0]) * (size_t)tmp_len);
		state.commandCount += delta;

		if (tmp_len > 0)
		{
			state.nodeDrawStart[node] = start;
			state.nodeDrawEnd[node] = start + tmp_len;
		}
		else
		{
			state.nodeDrawStart[node] = -1;
			state.nodeDrawEnd[node] = -1;
		}
		state.shiftNodeDrawRangesAfter(end, delta, node);
		return true;
	}

	std::uint32_t DisplayList::recordSerial() const
	{
		return state.displayListSerial;
	}

	bool DisplayList::commandOverflow() const
	{
		return state.commandOverflow;
	}

	bool DisplayList::patchScrollbarThumb(int node)
	{
		Tree &tree = Tree::instance();
		Node *nodes = tree.nodes();
		if (!state.commands || node < 0 || node >= tree.nodeCount())
			return false;
		const Node *n = &nodes[node];

		// Mirror of ViewRenderer::recordScrollbar's geometry. Cases where no
		// thumb is ever recorded are a successful no-op, not a failure.
		if (!isViewLikeNodeType(n->type) || (n->type != NodeType::VirtualList && !scrollsOverflowY(n->style)))
			return true;
		if (n->layout.height <= 0 || n->layout.scroll_content_height <= n->layout.height)
			return true;
		const int track_h = n->layout.height - 12;
		if (track_h < 24)
			return true;
		int thumb_h = (n->layout.height * track_h) / n->layout.scroll_content_height;
		if (thumb_h < 24)
			thumb_h = 24;
		if (thumb_h > track_h)
			thumb_h = track_h;
		const int max_scroll = n->layout.scroll_content_height - n->layout.height;
		auto thumbY = [&](int scrollY)
		{
			int y = n->layout.y + 6;
			if (max_scroll > 0)
				y += (scrollY * (track_h - thumb_h)) / max_scroll;
			return y;
		};
		const int oldY = thumbY(n->layout.previous_scroll_y);
		const int newY = thumbY(n->layout.scroll_y);
		const int thumbX = n->layout.x + n->layout.width - 7;
		if (oldY == newY)
			return true;

		for (int ci = 0; ci < state.commandCount; ci++)
		{
			DisplayCommand &c = state.commands[ci];
			if (c.type != DisplayCommandType::FillRoundedRect)
				continue;
			if (c.bx != thumbX || c.bw != 3 || c.by != oldY || c.bh != thumb_h)
				continue;
			c.by = static_cast<std::int16_t>(newY);
			c.fillRoundedRect.y = static_cast<std::int16_t>(newY);
			return true;
		}
		return false;
	}

	void DisplayList::replayDirectDirtyRegion(int x0, int y0, int x1, int y1, int origin)
	{
		// See regionHasOpaqueBase: a region the list does not fully paint must get the
		// mount-time clear back first, or the antialiased edges standing on unpainted
		// background re-blend over themselves and drift outward every replay.
		DisplayCommandReplayer::restoreClearBaseInRegion(x0, y0, x1, y1);
		DisplayCommandReplayer::replayDirectDirtyRegion(x0, y0, x1, y1, origin);
	}

	void DisplayList::replayDirectDirtyRegions(const DisplayReplayRegion *regions, int count)
	{
		for (int i = 0; i < count; ++i)
			DisplayCommandReplayer::restoreClearBaseInRegion(regions[i].x0, regions[i].y0, regions[i].x1, regions[i].y1);
		DisplayCommandReplayer::replayDirectDirtyRegions(regions, count);
	}

	bool DisplayList::canReplaySimpleDirtyRegions(int width, int height) const
	{
		return DisplayCommandReplayer::canReplaySimpleDirtyRegions(width, height);
	}

	void DisplayList::replaySimpleClippedDirtyRegion(int x0, int y0, int x1, int y1, int origin)
	{
		const auto &tree = Tree::instance();
		const int root = tree.mountedRoot();
		if (root >= 0 && root < tree.nodeCount() && isDocumentCanvasRoot(tree.nodes()[root]))
			DisplayCommandReplayer::restoreClearBaseInRegion(x0, y0, x1, y1);
		DisplayCommandReplayer::replaySimpleClippedDirtyRegion(x0, y0, x1, y1, origin);
	}

	void DisplayList::maybeBakeStaticBackdrop(int width, int height, bool stableThisFrame, bool directReplay,
																						bool bakeEligibleThisFrame)
	{
		// The backdrop cache only helps the direct dirty-region replay path; without it
		// we render the full list anyway, so don't bake.
		if (!directReplay)
		{
			gBackdropCacheValid = false;
			gBackdropStableFrames = 0;
			gBackdropFullSyncPending = false;
			return;
		}
		if (DisplayCommandReplayer::canReplaySimpleDirtyRegions(width, height))
		{
			gBackdropCacheValid = false;
			gBackdropStableFrames = 0;
			gBackdropFullSyncPending = false;
			return;
		}
		if (!stableThisFrame)
		{
			// Static content changed (structural rebuild) — drop the cache and re-settle.
			gBackdropCacheValid = false;
			gBackdropStableFrames = 0;
			gBackdropFullSyncPending = false;
			return;
		}
		// Stable-but-quiet frame (keyframe pause, ticker-only tick): keep the cache and
		// the settle counter, but never BAKE here — the bake excludes the dynamic
		// subtree by its render-dirty flags, and a paused cube isn't dirty, so baking
		// now would burn its pixels into the backdrop and ghost them behind the live
		// cube when the animation resumes.
		if (!bakeEligibleThisFrame)
			return;
		if (gBackdropStableFrames < 1000)
			gBackdropStableFrames++;
		// Bake once the static set has been stable for a couple frames (layout settled).
		if (!gBackdropCacheValid && gBackdropStableFrames >= 2 && width > 0 && height > 0)
			DisplayCommandReplayer::bakeStaticBackdrop(width, height);
	}

	void DisplayList::invalidateStaticBackdrop()
	{
		gBackdropCacheValid = false;
		gBackdropStableFrames = 0;
		gBackdropFullSyncPending = false;
	}

	bool DisplayList::staticBackdropActive() const
	{
		return gBackdropCacheValid;
	}

	bool DisplayList::consumeBackdropFullSyncRequest()
	{
		const bool pending = gBackdropFullSyncPending;
		gBackdropFullSyncPending = false;
		return pending;
	}

	void DisplayList::filterBlurCacheStats(int *hits, int *misses) const
	{
		if (hits)
			*hits = state.filterBlurCacheHits;
		if (misses)
			*misses = state.filterBlurCacheMisses;
	}

	void DisplayList::replay()
	{
		DisplayCommandReplayer::replay();
	}

} // namespace gea::embedded::ui
