#pragma once

#include "pixel.h"

#include <cstddef>
#include <cstdint>

namespace gea::framework::graphics {

class RasterizedFont;

struct ClipRect {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
};

struct CanvasDirtyRect {
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
};

// Interleaved circle record used by fillCirclesRgb565WorldYSorted. Field order
// is (y, x, color) so sorting an array of these by ::y is a simple
// std::sort with a single comparator on the first field. The display_present
// pipeline allocates these in INTERNAL RAM, sorts in place, and hands the
// pointer directly to Canvas — no deinterleave step.
struct CircleEntry {
	std::uint16_t y;
	std::uint16_t x;
	pixel::native_t color;
};

// Interleaved opaque-triangle record used by the batched triangle present
// command (one command per frame instead of one per triangle — the per-command
// record/dispatch overhead dominated at a few hundred triangles). Kept in
// PAINTER'S order (depth-sorted by the producer), so the rasterizer may only
// band-reject per entry, never reorder. rowY0/rowY1 cache the vertical extent
// for that reject. int16 coordinates: producers clamp to +/-30000.
struct TriangleEntry {
	std::int16_t x0;
	std::int16_t y0;
	std::int16_t x1;
	std::int16_t y1;
	std::int16_t x2;
	std::int16_t y2;
	pixel::native_t color;
	std::int16_t rowY0;
	std::int16_t rowY1;
};

class Canvas {
public:
	static constexpr int kMaxClipDepth = 32;
	static constexpr int kMaxDirtyRects = 24;
	static void setAntialiasSamples(int samples);
	static int antialiasSamples();
	// Large-static-text sprite cache opt-in (default off). When enabled, text at
	// font.sizePx() >= 40 is rasterized to a coverage sprite once and blitted on
	// subsequent frames instead of re-running the glyph rasterizer. Toggled from
	// TS via Display.setTextRasterCache(boolean).
	static void setTextRasterCacheEnabled(bool enabled);
	static bool textRasterCacheEnabled();
	// Pre-packed glyph stamping opt-in (packed 4bpp targets; no-op elsewhere).
	// The app declares text always draws over ONE solid color (0xRRGGBB);
	// glyphs are then pre-blended+packed once and stamped as byte copies — no
	// per-pixel destination read, no blend. Negative disables. Toggled from TS
	// via Display.setTextSolidBackdrop(number).
	static void setTextSolidBackdrop(int rrggbb);

	// `pixel::native_t` is this target's framebuffer pixel (RGB565 16-bit or
	// RGBA8888 32-bit), fixed at compile time — no runtime format here.
	void bindPixels(pixel::native_t *pixels, int width, int height);
	void bindPixels(pixel::native_t *pixels, int width, int height, int stridePixels);

#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	// Bind a PHYSICAL landscape buffer while presenting a LOGICAL portrait
	// coordinate system. The apps + renderer keep drawing in logical portrait
	// (logicalWidth x logicalHeight); every pixel write is rotated 90° into the
	// physical landscape buffer, whose dimensions are (logicalHeight x
	// logicalWidth). A logical pixel (lx, ly) addresses physical column ly, row
	// (logicalWidth-1)-lx — the same map the papers3/T5 flush-time transpose used,
	// so the Canvas IS the panel buffer and no separate transpose pass is needed.
	// Additive and compile-gated: only built when GEA_EMBEDDED_DISPLAY_ROTATE_
	// LANDSCAPE is defined (papers3), so every other target is byte-for-byte
	// unaffected. Supports both packed grayscale and native RGB framebuffers.
	void bindPixelsRotatedLandscape(pixel::native_t *physPixels, int logicalWidth, int logicalHeight);
#endif

	pixel::native_t *pixels();
	const pixel::native_t *pixels() const;
	int width() const;
	int height() const;
	int strideBytes() const;
	// Read or replace one logical framebuffer pixel in native format. Reads are
	// independent of the current clip; writes obey the clip and mark the logical
	// pixel dirty. Writes replace exactly (they do not apply global alpha).
	pixel::native_t readPixelNative(int x, int y) const;
	void writePixelNativeExact(int x, int y, pixel::native_t value);

	// Software-scroll register: rotates the logical → physical row mapping
	// inside a scroll region without moving any pixels. A pixel write at
	// logical row Y lands at the physical FB row determined by the region:
	//   - outside the region (y < regionY or y >= regionY+regionH): no
	//     translation, physical row = y.
	//   - inside the region: physical row = regionY + ((y − regionY +
	//     scrollOffsetY) mod regionH). Wraps inside the region's row range
	//     so the region acts as a ring buffer along Y.
	// regionH = 0 means "no region active" — every pixel writes through.
	// A scroll becomes a single integer update on scrollOffsetY rather than
	// an H × W memcpy.
	void setScrollRegion(int regionY, int regionH, int scrollOffsetY);
	int scrollOffsetY() const { return scrollOffsetY_; }
	int scrollRegionY() const { return regionY_; }
	int scrollRegionH() const { return regionH_; }
	int rowToPhysical(int y) const {
		if (regionH_ <= 0) return y;
		if (y < regionY_ || y >= regionY_ + regionH_) return y;
		int rel = (y - regionY_) + scrollOffsetY_;
		rel %= regionH_;
		if (rel < 0) rel += regionH_;
		return regionY_ + rel;
	}
#if GEA_PIXEL_STORAGE_PACKED
	// Base of a physical row's packed bytes (sub-byte framebuffers only — GRAY4
	// at 2 px/byte, GRAY2 at 4 px/byte; strideBytes_ carries the depth).
	std::uint8_t *packedRow(int phys) {
		return reinterpret_cast<std::uint8_t *>(pixels_) + static_cast<std::size_t>(phys) * strideBytes_;
	}
	const std::uint8_t *packedRow(int phys) const {
		return reinterpret_cast<const std::uint8_t *>(pixels_) + static_cast<std::size_t>(phys) * strideBytes_;
	}
#endif
#if GEA_EMBEDDED_DISPLAY_ROTATE_LANDSCAPE
	// Landscape-rotation primitives (see bindPixelsRotatedLandscape). A LOGICAL
	// pixel (lx, ly) maps to physical row (width_-1)-lx, physical column ly. These
	// are the SINGLE place the rotation transform lives; every rotated Canvas draw
	// path routes its pixel writes through them, so the transform is verified once.
	bool landscapeRot_ = false;
	int rotPhysRow(int lx) const { return (width_ - 1) - lx; }
	void rotSet(int lx, int ly, pixel::native_t v) {
#if GEA_PIXEL_STORAGE_PACKED
		pixel::packed::set(packedRow(rotPhysRow(lx)), ly, v);
#else
		pixels_[rotPhysRow(lx) * stride_ + ly] = v;
#endif
	}
	void rotBlend(int lx, int ly, pixel::native_t fg, int alpha) {
#if GEA_PIXEL_STORAGE_PACKED
		pixel::packed::blendPixel(packedRow(rotPhysRow(lx)), ly, fg, alpha);
#else
		pixel::native_t &dst = pixels_[rotPhysRow(lx) * stride_ + ly];
		dst = pixel::blendNative(fg, dst, alpha);
#endif
	}
	pixel::native_t rotGet(int lx, int ly) const {
#if GEA_PIXEL_STORAGE_PACKED
		return pixel::packed::get(packedRow(rotPhysRow(lx)), ly);
#else
		return pixels_[rotPhysRow(lx) * stride_ + ly];
#endif
	}
#endif

	void resetDirty();
	void markDirty(int x0, int y0, int x1, int y1);
	bool dirty(int *x0, int *y0, int *x1, int *y1) const;
	int dirtyRects(CanvasDirtyRect *rects, int capacity) const;

	void resetClip();
	void pushClip(int x, int y, int w, int h);
	void popClip();
	void currentClip(int *x0, int *y0, int *x1, int *y1) const;

	void setGlobalAlpha(std::uint8_t alpha);
	std::uint8_t globalAlpha() const;

	void clear(pixel::native_t color);
	void clearRect(int x, int y, int w, int h);
	void scrollRect(int x, int y, int w, int h, int dx, int dy);
	void fillRect(int x, int y, int w, int h, pixel::native_t color);
	void fillRectOpaque(int x, int y, int w, int h, pixel::native_t color);
	void strokeRect(int x, int y, int w, int h, pixel::native_t color);
	void fillCircle(int cx, int cy, int r, pixel::native_t color);
	void fillCirclesRgb565(const std::uint16_t *xs, const std::uint16_t *ys, int count, int r, const pixel::native_t *colors);
	void fillCirclesRgb565Uniform(const std::uint16_t *xs, const std::uint16_t *ys, int count, int r, pixel::native_t color);
	void fillCirclesRgb565WorldYSorted(const CircleEntry *circles, int count,
	                                    int r,
	                                    int yBandMinWorld, int yBandMaxWorld,
	                                    int xOffset, int yOffset);
	void fillRoundedRectBoxesRgb565(const std::int16_t *xs, const std::int16_t *ys, int count,
	                                int w, int h, int tl, int tr, int br, int bl,
	                                const pixel::native_t *colors);
	void strokeCircle(int cx, int cy, int r, pixel::native_t color);
	void drawLine(int x0, int y0, int x1, int y1, pixel::native_t color);
	void drawArc(int cx, int cy, int r, int start_deg, int end_deg, pixel::native_t color);
	void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, pixel::native_t color);
	void fillTriangleOpaque(int x0, int y0, int x1, int y1, int x2, int y2, pixel::native_t color);
	// Opaque triangle BATCH with per-row span occlusion. Entries arrive
	// farthest-first (painter's depth order); this walks them nearest-first and
	// fills only pixels no nearer triangle has already covered — each pixel
	// written once, occluded spans cost a coverage test instead of a fill. Same
	// visible result as drawing the batch back-to-front with fillTriangleOpaque,
	// but without the overdraw. Coords are world; ox/oy shift into this canvas's
	// chunk-local space (like the fillTriangleOpaque caller does). Valid only for
	// OPAQUE, depth-sorted batches. Coverage lives in per-core static scratch, so
	// two Canvases on different cores may run this concurrently.
	void fillTrianglesOpaqueOccluded(const TriangleEntry *tris, int count, int ox, int oy);
	void drawText(const char *text, int x, int y, pixel::native_t color, float scale);
	void drawTextFont(const char *text, int x, int y, pixel::native_t color, int font_id);
	void drawTextFontFamily(const char *text, int x, int y, pixel::native_t color, int family_id, int size_px);
	// Rotated 90deg for panel-native landscape rendering: the anchor and the
	// text advance are in LANDSCAPE coordinates; every glyph pixel writes to
	// the portrait panel address (panel_x = ly, panel_y = height-1-lx). The
	// canvas must be bound to the FULL portrait buffer.
	void drawTextFontFamilyRotated90(const char *text, int lx, int ly, pixel::native_t color, int family_id, int size_px);
	void drawTextFontFamilyOnBackground(const char *text, int x, int y, pixel::native_t color,
	                                    int family_id, int size_px, pixel::native_t background);
	int measureTextFontFamily(const char *text, int family_id, int size_px);
	int measureTextInkCenterFontFamily(const char *text, int family_id, int size_px);
	static void measureTextFont(const char *text, int max_width, int font_id, int *out_w, int *out_h);
	void fillRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, pixel::native_t color);
	void strokeRoundedRect(int x, int y, int w, int h, int tl, int tr, int br, int bl, int lw, pixel::native_t color);
	// Image sources are native_t (same format as the framebuffer for this
	// target), so blits are a native copy/blend with no per-pixel conversion.
	void drawImage(const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h, int dx, int dy);
#if GEA_PIXEL_STORAGE_PACKED
	// Blit a packed source (a grayscale canvas surface, stored in the same
	// sub-byte packing as the framebuffer) straight in — a direct same-depth
	// copy, no unpack/repack. Clipped.
	void blitPacked(const std::uint8_t *src, int src_w, int src_h, int dx, int dy);
#endif
	void drawImage(const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	               int dx, int dy, int dst_w, int dst_h);
	void drawImageRotated90CW(const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	                          int dx, int dy, int dst_w, int dst_h);
	void drawImageRounded(const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	                      int dx, int dy, int dst_w, int dst_h, int tl, int tr, int br, int bl);
	void drawImageTiledX(const pixel::native_t *src, const std::uint8_t *alpha, int src_w, int src_h,
	                     int dx, int dy, int width);

private:
	void writePixel(int x, int y, pixel::native_t fg);
	void writePixelAlpha(int x, int y, pixel::native_t fg, std::uint8_t alpha);
	// Native-pixel writers: the colour is already this target's native pixel
	// (e.g. an image source pixel), so there is no colour conversion — clip +
	// global-alpha + dirty handling only. writePixel/writePixelAlpha are the
	// RGB565-colour entry points and convert once via pixel::toNative().
	void writePixelNative(int x, int y, pixel::native_t c);
	void writePixelNativeAlpha(int x, int y, pixel::native_t c, std::uint8_t alpha);
	// Fill `count` native pixels at physical row `phys` with an RGB565 colour
	// (converted once to native). fillSpanGlobalAlpha honours globalAlpha_.
	void fillSpanColor(int phys, int x0, int count, pixel::native_t color);
	void fillSpanGlobalAlpha(int phys, int x0, int count, pixel::native_t color);
	void markDirtyClipped(int x0, int y0, int x1, int y1);
	void addDirtyRect(int x0, int y0, int x1, int y1);
	bool fillCircleNoDirty(int cx, int cy, int r, pixel::native_t color);
	void fillQuarterCircle(int cx, int cy, int r, int quadrant, pixel::native_t color);
	void fillCircleBox(int x, int y, int size, pixel::native_t color);
	void drawRasterizedText(const char *text, int x, int y, pixel::native_t color, const RasterizedFont &font);
	// Blit a pre-rasterized glyph-coverage sprite (text-relative coords, origin at
	// pen x/y, offset by minLX/minLY) honouring the current clip, global alpha and
	// dirty accounting. Runs of fully-opaque coverage use the fast span fill.
	// Used by the large-static-text sprite cache to avoid re-running the scattered
	// flash-atlas glyph rasterizer every frame. cov is w*h coverage bytes; rowX0/
	// rowX1 are per-row ink extents (rowX1 < rowX0 marks an empty row).
	void blitGlyphSprite(const std::uint8_t *cov, const std::int16_t *rowX0, const std::int16_t *rowX1,
	                     const std::uint8_t *rleVal, const std::uint16_t *rleLen, const std::int32_t *rleRowStart,
	                     int minLX, int minLY, int w, int h, int x, int y, pixel::native_t color);

	int width_ = 0;
	int height_ = 0;
	int stride_ = 0;          // row stride in pixels (>= width_, may be padded)
	int strideBytes_ = 0;
	int regionY_ = 0;         // scroll region origin row (logical)
	int regionH_ = 0;         // scroll region height; 0 = no region active
	int scrollOffsetY_ = 0;   // current rotation within region; see rowToPhysical()
	pixel::native_t *pixels_ = nullptr;

	int dirtyX0_ = 0;
	int dirtyY0_ = 0;
	int dirtyX1_ = -1;
	int dirtyY1_ = -1;
	CanvasDirtyRect dirtyRects_[kMaxDirtyRects] = {};
	int dirtyRectCount_ = 0;
	std::uint8_t globalAlpha_ = 255;

	ClipRect clipStack_[kMaxClipDepth] = {};
	int clipDepth_ = 0;
};

}  // namespace gea::framework::graphics
