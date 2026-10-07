#pragma once

#include "canvas.h"
#include "display.h"
#include "image.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include <string>
#include <utility>
#include <vector>

#if __has_include("esp_heap_caps.h")
#include "esp_heap_caps.h"
#else
#define GEA_DISPLAY_PRESENT_HAS_ESP_HEAP_CAPS 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
inline void *heap_caps_malloc(std::size_t size, int)
{
	return std::malloc(size);
}
inline void heap_caps_free(void *ptr)
{
	std::free(ptr);
}
#endif

namespace gea::framework::display_present {

struct Rect {
	int x0 = 0;
	int y0 = 0;
	int x1 = -1;
	int y1 = -1;
};

inline bool valid(const Rect &rect)
{
	return rect.x0 <= rect.x1 && rect.y0 <= rect.y1;
}

inline int area(const Rect &rect)
{
	if (!valid(rect)) return 0;
	return (rect.x1 - rect.x0 + 1) * (rect.y1 - rect.y0 + 1);
}

inline Rect unite(const Rect &a, const Rect &b)
{
	if (!valid(a)) return b;
	if (!valid(b)) return a;
	Rect out = a;
	if (b.x0 < out.x0) out.x0 = b.x0;
	if (b.y0 < out.y0) out.y0 = b.y0;
	if (b.x1 > out.x1) out.x1 = b.x1;
	if (b.y1 > out.y1) out.y1 = b.y1;
	return out;
}

inline Rect fullScreen(int width, int height)
{
	return {0, 0, width - 1, height - 1};
}

inline Rect clampAndAlign(Rect rect, int width, int height)
{
	rect.x0 &= ~1;
	rect.y0 &= ~1;
	rect.x1 |= 1;
	rect.y1 |= 1;
	if (rect.x0 < 0) rect.x0 = 0;
	if (rect.y0 < 0) rect.y0 = 0;
	if (rect.x1 >= width) rect.x1 = width - 1;
	if (rect.y1 >= height) rect.y1 = height - 1;
	return rect;
}

// unique_ptr deleter that frees buffers allocated via heap_caps_malloc.
// We must NOT use the global operator delete (or std::default_delete) here:
// the project's overridden operator new (targets/esp32/services/heap.cpp)
// routes through EspHeapAllocator and prefers PSRAM; freeing a
// heap_caps_malloc(INTERNAL) buffer through that path corrupts the heap.
struct HeapCapsFree {
	void operator()(void *p) const noexcept {
		heap_caps_free(p);
	}
};

static constexpr int kInlineCircleEntryCapacity = 32;

struct Command {
	gea::framework::graphics::ImageReference imageReference;
	using CircleEntry = gea::framework::graphics::CircleEntry;

	gea::platform::display::DisplayPresentCommandType type =
		gea::platform::display::DisplayPresentCommandType::Clear;
	gea::framework::graphics::pixel::native_t color = 0;
	std::uint8_t alpha = 255;
	const gea::framework::graphics::pixel::native_t *pixels = nullptr;
	const std::uint8_t *alphaPixels = nullptr;
	int srcWidth = 0;
	int srcHeight = 0;
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	int x2 = 0;
	int y2 = 0;
	int radius = 0;
	float scale = 1.0f;
	int fontFamilyId = -1;
	int fontSizePx = 16;
	std::string text;
	// FillCirclesRgb565: small batches stay inline; larger batches own an
	// INTERNAL-RAM buffer of CircleEntry triples (sorted by y), allocated once
	// in extractFrame. No PSRAM deinterleave copy — the canvas rasterizer reads
	// directly from this storage.
	std::array<CircleEntry, kInlineCircleEntryCapacity> inlineCircles{};
	std::unique_ptr<CircleEntry[], HeapCapsFree> circlesBuffer;
	int circlesCount = 0;
	bool circlesInline = false;
	// FillTrianglesRgb565: same ownership scheme as the circle batch. Entries
	// stay in the producer's painter's (depth) order; the batch's overall row
	// extent is cached in the Command's y0/y1 fields for whole-chunk skips.
	std::unique_ptr<gea::framework::graphics::TriangleEntry[], HeapCapsFree> trianglesBuffer;
	int trianglesCount = 0;

	const CircleEntry *circleEntries() const
	{
		return circlesInline ? inlineCircles.data() : circlesBuffer.get();
	}

	bool hasCircleEntries() const
	{
		return circlesCount <= 0 || circleEntries() != nullptr;
	}
};

struct Frame {
	std::vector<Command> commands;

	void clear()
	{
		commands.clear();
	}
};

inline Rect rectFromBox(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return {};
	return {x, y, x + w - 1, y + h - 1};
}

inline Rect circleBounds(int x, int y, int radius)
{
	return {x - radius, y - radius, x + radius, y + radius};
}

inline Rect triangleBounds(const Command &command)
{
	return {
		std::min({command.x0, command.x1, command.x2}),
		std::min({command.y0, command.y1, command.y2}),
		std::max({command.x0, command.x1, command.x2}),
		std::max({command.y0, command.y1, command.y2}),
	};
}

inline Rect textBounds(const Command &command)
{
	const int glyphWidth = std::max(1, static_cast<int>(8.0f * command.scale + 0.5f));
	const int glyphHeight = std::max(1, static_cast<int>(16.0f * command.scale + 0.5f));
	return rectFromBox(command.x, command.y, static_cast<int>(command.text.size()) * glyphWidth, glyphHeight);
}

inline Rect commandBounds(const Command &command, int width, int height)
{
	using Type = gea::platform::display::DisplayPresentCommandType;
	switch (command.type) {
	case Type::Clear:
		return fullScreen(width, height);
	case Type::FillRectRgb565:
	case Type::StrokeRectRgb565:
		return rectFromBox(command.x, command.y, command.w, command.h);
	case Type::FillTriangleRgb565:
		return triangleBounds(command);
	case Type::FillCircleRgb565:
	case Type::StrokeCircleRgb565:
		return circleBounds(command.x, command.y, command.radius);
	case Type::FillCirclesRgb565: {
		Rect bounds{};
		const auto *circles = command.circleEntries();
		const int limit = command.circlesCount;
		if (!circles) return bounds;
		for (int i = 0; i < limit; i++) {
			bounds = unite(bounds, circleBounds(circles[i].x, circles[i].y, command.radius));
		}
		return bounds;
	}
	case Type::FillTrianglesRgb565: {
		Rect bounds{};
		const gea::framework::graphics::TriangleEntry *tris = command.trianglesBuffer.get();
		const int limit = command.trianglesCount;
		if (!tris) return bounds;
		for (int i = 0; i < limit; i++) {
			const auto &t = tris[i];
			const Rect triRect{
				std::min({static_cast<int>(t.x0), static_cast<int>(t.x1), static_cast<int>(t.x2)}),
				static_cast<int>(t.rowY0),
				std::max({static_cast<int>(t.x0), static_cast<int>(t.x1), static_cast<int>(t.x2)}),
				static_cast<int>(t.rowY1),
			};
			bounds = unite(bounds, triRect);
		}
		return bounds;
	}
	case Type::DrawImage:
		return rectFromBox(command.x, command.y, command.srcWidth, command.srcHeight);
	case Type::DrawImageScaled:
	case Type::DrawImageRotated90CW:
		return rectFromBox(command.x, command.y, command.w, command.h);
	case Type::DrawImageTiledX:
		return rectFromBox(command.x, command.y, command.w, command.srcHeight);
	case Type::FillText:
		return textBounds(command);
	}
	return {};
}

inline void addRect(Rect *rects, int *count, int capacity, Rect rect, int width, int height)
{
	if (!rects || !count || capacity <= 0) return;
	rect = clampAndAlign(rect, width, height);
	if (!valid(rect)) return;

	for (int i = 0; i < *count; i++) {
		if (rects[i].x0 <= rect.x1 + 1 && rects[i].x1 + 1 >= rect.x0 &&
		    rects[i].y0 <= rect.y1 + 1 && rects[i].y1 + 1 >= rect.y0) {
			rects[i] = unite(rects[i], rect);
			return;
		}
	}

	if (*count < capacity) {
		rects[(*count)++] = rect;
		return;
	}

	int best = 0;
	int bestCost = 0x7fffffff;
	for (int i = 0; i < *count; i++) {
		const Rect merged = unite(rects[i], rect);
		const int cost = area(merged) - area(rects[i]);
		if (cost < bestCost) {
			bestCost = cost;
			best = i;
		}
	}
	rects[best] = unite(rects[best], rect);
}

inline void addCommandBounds(const Command &command, Rect *rects, int *count, int capacity, int width, int height)
{
	using Type = gea::platform::display::DisplayPresentCommandType;
	if (command.type == Type::FillCirclesRgb565) {
		const auto *circles = command.circleEntries();
		const int limit = command.circlesCount;
		if (!circles) return;
		for (int i = 0; i < limit; i++) {
			addRect(rects, count, capacity, circleBounds(circles[i].x, circles[i].y, command.radius), width, height);
		}
		return;
	}
	addRect(rects, count, capacity, commandBounds(command, width, height), width, height);
}

// An app-registered buffer (native surface, ImageData565, video frame) is
// rewritten in place, so equal pointers say nothing about equal pixels.
inline bool imagePixelsStable(const Command &command)
{
	return !gea::framework::graphics::ImageStore::instance().pixelsMayChange(command.pixels);
}

inline bool commandsEqual(const Command &a, const Command &b)
{
	if (a.type != b.type || a.alpha != b.alpha) return false;
	using Type = gea::platform::display::DisplayPresentCommandType;
	switch (a.type) {
	case Type::Clear:
		return a.color == b.color;
	case Type::FillRectRgb565:
	case Type::StrokeRectRgb565:
		return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && a.color == b.color;
	case Type::FillTriangleRgb565:
		return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1 &&
		       a.x2 == b.x2 && a.y2 == b.y2 && a.color == b.color;
	case Type::FillCircleRgb565:
	case Type::StrokeCircleRgb565:
		return a.x == b.x && a.y == b.y && a.radius == b.radius && a.color == b.color;
	case Type::FillCirclesRgb565: {
		if (a.radius != b.radius || a.circlesCount != b.circlesCount) return false;
		if (a.circlesCount == 0) return true;
		const auto *ap = a.circleEntries();
		const auto *bp = b.circleEntries();
		if (!ap || !bp) return ap == bp;
		return std::memcmp(ap, bp,
		                   static_cast<std::size_t>(a.circlesCount) *
		                       sizeof(gea::framework::graphics::CircleEntry)) == 0;
	}
	case Type::FillTrianglesRgb565: {
		if (a.trianglesCount != b.trianglesCount) return false;
		if (a.trianglesCount == 0) return true;
		const auto *ap = a.trianglesBuffer.get();
		const auto *bp = b.trianglesBuffer.get();
		if (!ap || !bp) return ap == bp;
		return std::memcmp(ap, bp,
		                   static_cast<std::size_t>(a.trianglesCount) *
		                       sizeof(gea::framework::graphics::TriangleEntry)) == 0;
	}
	case Type::DrawImage:
		return a.pixels == b.pixels && a.alphaPixels == b.alphaPixels && imagePixelsStable(a) &&
		       a.srcWidth == b.srcWidth && a.srcHeight == b.srcHeight && a.x == b.x && a.y == b.y;
	case Type::DrawImageScaled:
	case Type::DrawImageRotated90CW:
		return a.pixels == b.pixels && a.alphaPixels == b.alphaPixels && imagePixelsStable(a) &&
		       a.srcWidth == b.srcWidth && a.srcHeight == b.srcHeight &&
		       a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
	case Type::DrawImageTiledX:
		return a.pixels == b.pixels && a.alphaPixels == b.alphaPixels && imagePixelsStable(a) &&
		       a.srcWidth == b.srcWidth && a.srcHeight == b.srcHeight &&
		       a.x == b.x && a.y == b.y && a.w == b.w;
	case Type::FillText:
		return a.x == b.x && a.y == b.y && a.color == b.color && a.scale == b.scale &&
		       a.fontFamilyId == b.fontFamilyId && a.fontSizePx == b.fontSizePx && a.text == b.text;
	}
	return false;
}

inline bool extractFrame(const gea::platform::display::DisplayPresentCommand *commands, int commandCount, Frame &frame)
{
	frame.clear();
	if (!commands || commandCount <= 0) return false;
	frame.commands.reserve(static_cast<std::size_t>(commandCount));
	for (int i = 0; i < commandCount; i++) {
		const auto &command = commands[i];
		Command out;
		out.type = command.type;
		switch (command.type) {
		case gea::platform::display::DisplayPresentCommandType::Clear:
			out.color = command.clear.color;
			break;
		case gea::platform::display::DisplayPresentCommandType::FillRectRgb565:
			out.x = command.fillRectRgb565.x;
			out.y = command.fillRectRgb565.y;
			out.w = command.fillRectRgb565.w;
			out.h = command.fillRectRgb565.h;
			out.color = command.fillRectRgb565.color;
			out.alpha = command.fillRectRgb565.alpha;
			break;
		case gea::platform::display::DisplayPresentCommandType::StrokeRectRgb565:
			out.x = command.strokeRectRgb565.x;
			out.y = command.strokeRectRgb565.y;
			out.w = command.strokeRectRgb565.w;
			out.h = command.strokeRectRgb565.h;
			out.color = command.strokeRectRgb565.color;
			out.alpha = command.strokeRectRgb565.alpha;
			break;
		case gea::platform::display::DisplayPresentCommandType::FillTriangleRgb565:
			out.x0 = command.fillTriangleRgb565.x0;
			out.y0 = command.fillTriangleRgb565.y0;
			out.x1 = command.fillTriangleRgb565.x1;
			out.y1 = command.fillTriangleRgb565.y1;
			out.x2 = command.fillTriangleRgb565.x2;
			out.y2 = command.fillTriangleRgb565.y2;
			out.color = command.fillTriangleRgb565.color;
			out.alpha = command.fillTriangleRgb565.alpha;
			break;
			case gea::platform::display::DisplayPresentCommandType::FillCircleRgb565:
				out.x = command.fillCircleRgb565.x;
				out.y = command.fillCircleRgb565.y;
				out.radius = command.fillCircleRgb565.radius;
				out.color = command.fillCircleRgb565.color;
				out.alpha = command.fillCircleRgb565.alpha;
				break;
			case gea::platform::display::DisplayPresentCommandType::StrokeCircleRgb565:
				out.x = command.strokeCircleRgb565.x;
				out.y = command.strokeCircleRgb565.y;
				out.radius = command.strokeCircleRgb565.radius;
				out.color = command.strokeCircleRgb565.color;
				out.alpha = command.strokeCircleRgb565.alpha;
				break;
			case gea::platform::display::DisplayPresentCommandType::FillCirclesRgb565: {
				const auto &src = command.fillCirclesRgb565;
				if (!src.xs || !src.ys || !src.colors || src.count < 0 || src.radius <= 0) return false;
				out.radius = src.radius;
				out.alpha = src.alpha;
				// Opt 4: small inline storage, otherwise one owned INTERNAL-RAM
				// buffer, sorted in place.
				//
				// The previous design did TWO things this loop now collapses into
				// one: (a) build an interleaved scratch array in INTERNAL RAM and
				// sort it, then (b) scatter-copy the sorted triples into three
				// PSRAM std::vector<uint16_t>'s on the Command. Both the second
				// alloc and the PSRAM stores are gone — extractFrame allocates
				// either an inline Command array or ONE CircleEntry[] in INTERNAL
				// RAM, sorts it, and the canvas rasterizer reads directly from it.
				//
				// MALLOC_CAP_INTERNAL is required: the project's overridden
				// operator new (targets/esp32/services/heap.cpp) prefers PSRAM,
				// and empirically a new PSRAM allocation in this hot path blanks
				// the CO5300 panel — even a 4-byte alloc.
				const int count = src.count;
				using gea::framework::graphics::CircleEntry;
				if (count > 0) {
					CircleEntry *buffer = nullptr;
					if (count <= kInlineCircleEntryCapacity) {
						out.circlesInline = true;
						buffer = out.inlineCircles.data();
					} else {
						buffer = static_cast<CircleEntry *>(
							heap_caps_malloc(sizeof(CircleEntry) * static_cast<std::size_t>(count),
							                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
						if (!buffer) return false;
					}
					bool sortedByBucket = false;
					constexpr int kCircleBucketSortMaxYRange = 2048;
					if (count <= 65535) {
						int minY = static_cast<int>(src.ys[0]);
						int maxY = minY;
						for (int i = 1; i < count; i++) {
							const int y = static_cast<int>(src.ys[i]);
							if (y < minY) minY = y;
							if (y > maxY) maxY = y;
						}
						const int yRange = maxY - minY + 1;
						if (yRange > 0 && yRange <= kCircleBucketSortMaxYRange) {
							std::uint16_t offsets[kCircleBucketSortMaxYRange] = {};
							for (int i = 0; i < count; i++) {
								offsets[static_cast<int>(src.ys[i]) - minY]++;
							}
							std::uint16_t running = 0;
							for (int y = 0; y < yRange; y++) {
								const std::uint16_t n = offsets[y];
								offsets[y] = running;
								running = static_cast<std::uint16_t>(running + n);
							}
							for (int i = 0; i < count; i++) {
								const int bucket = static_cast<int>(src.ys[i]) - minY;
								buffer[offsets[bucket]++] = CircleEntry{src.ys[i], src.xs[i], src.colors[i]};
							}
							sortedByBucket = true;
						}
					}
					if (!sortedByBucket) {
						for (int i = 0; i < count; i++) {
							buffer[i] = CircleEntry{src.ys[i], src.xs[i], src.colors[i]};
						}
						std::sort(buffer, buffer + count,
							[](const CircleEntry &a, const CircleEntry &b) { return a.y < b.y; });
					}
					if (!out.circlesInline) out.circlesBuffer.reset(buffer);
				}
				out.circlesCount = count;
				break;
			}
		case gea::platform::display::DisplayPresentCommandType::FillTrianglesRgb565: {
			// Same INTERNAL-RAM ownership rationale as the circle batch above.
			// Entries arrive pre-packed and depth-ordered from the canvas layer;
			// the copy also caches the batch-wide row extent in the Command's
			// y0/y1 fields for whole-chunk skips in commandRowExtent.
			const auto &src = command.fillTrianglesRgb565;
			if (!src.entries || src.count < 0) return false;
			out.alpha = src.alpha;
			using gea::framework::graphics::TriangleEntry;
			const int count = src.count;
			int batchY0 = 0;
			int batchY1 = -1;
			if (count > 0) {
				TriangleEntry *buffer = static_cast<TriangleEntry *>(
					heap_caps_malloc(sizeof(TriangleEntry) * static_cast<std::size_t>(count),
					                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
				if (!buffer) return false;
				std::memcpy(buffer, src.entries, sizeof(TriangleEntry) * static_cast<std::size_t>(count));
				batchY0 = buffer[0].rowY0;
				batchY1 = buffer[0].rowY1;
				for (int i = 1; i < count; i++) {
					if (buffer[i].rowY0 < batchY0) batchY0 = buffer[i].rowY0;
					if (buffer[i].rowY1 > batchY1) batchY1 = buffer[i].rowY1;
				}
				out.trianglesBuffer.reset(buffer);
			}
			out.trianglesCount = count;
			out.y0 = batchY0;
			out.y1 = batchY1;
			break;
		}
		case gea::platform::display::DisplayPresentCommandType::DrawImage:
			if (!command.drawImage.pixels || command.drawImage.srcWidth <= 0 || command.drawImage.srcHeight <= 0) return false;
			out.pixels = command.drawImage.pixels;
			out.alphaPixels = command.drawImage.alphaPixels;
			out.srcWidth = command.drawImage.srcWidth;
			out.srcHeight = command.drawImage.srcHeight;
			out.x = command.drawImage.x;
			out.y = command.drawImage.y;
			out.alpha = command.drawImage.alpha;
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImageScaled:
			if (!command.drawImageScaled.pixels || command.drawImageScaled.srcWidth <= 0 ||
			    command.drawImageScaled.srcHeight <= 0 || command.drawImageScaled.w <= 0 ||
			    command.drawImageScaled.h <= 0)
				return false;
			out.pixels = command.drawImageScaled.pixels;
			out.alphaPixels = command.drawImageScaled.alphaPixels;
			out.srcWidth = command.drawImageScaled.srcWidth;
			out.srcHeight = command.drawImageScaled.srcHeight;
			out.x = command.drawImageScaled.x;
			out.y = command.drawImageScaled.y;
			out.w = command.drawImageScaled.w;
			out.h = command.drawImageScaled.h;
			out.alpha = command.drawImageScaled.alpha;
			out.radius = command.drawImageScaled.radius;
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImageRotated90CW:
			if (!command.drawImageRotated90CW.pixels || command.drawImageRotated90CW.srcWidth <= 0 ||
			    command.drawImageRotated90CW.srcHeight <= 0 || command.drawImageRotated90CW.w <= 0 ||
			    command.drawImageRotated90CW.h <= 0)
				return false;
			out.pixels = command.drawImageRotated90CW.pixels;
			out.alphaPixels = command.drawImageRotated90CW.alphaPixels;
			out.srcWidth = command.drawImageRotated90CW.srcWidth;
			out.srcHeight = command.drawImageRotated90CW.srcHeight;
			out.x = command.drawImageRotated90CW.x;
			out.y = command.drawImageRotated90CW.y;
			out.w = command.drawImageRotated90CW.w;
			out.h = command.drawImageRotated90CW.h;
			out.alpha = command.drawImageRotated90CW.alpha;
			break;
		case gea::platform::display::DisplayPresentCommandType::DrawImageTiledX:
			if (!command.drawImageTiledX.pixels || command.drawImageTiledX.srcWidth <= 0 ||
			    command.drawImageTiledX.srcHeight <= 0 || command.drawImageTiledX.w <= 0)
				return false;
			out.pixels = command.drawImageTiledX.pixels;
			out.alphaPixels = command.drawImageTiledX.alphaPixels;
			out.srcWidth = command.drawImageTiledX.srcWidth;
			out.srcHeight = command.drawImageTiledX.srcHeight;
			out.x = command.drawImageTiledX.x;
			out.y = command.drawImageTiledX.y;
			out.w = command.drawImageTiledX.w;
			out.alpha = command.drawImageTiledX.alpha;
			break;
		case gea::platform::display::DisplayPresentCommandType::FillText:
			if (!command.fillText.text) return false;
			out.text = command.fillText.text;
			out.x = command.fillText.x;
			out.y = command.fillText.y;
			out.color = command.fillText.color;
			out.scale = command.fillText.scale;
			out.fontFamilyId = command.fillText.fontFamilyId;
			out.fontSizePx = command.fillText.fontSizePx;
			out.alpha = command.fillText.alpha;
			break;
		}
		out.imageReference = gea::framework::graphics::ImageReference(
			gea::framework::graphics::ImageStore::instance().idForPixels(out.pixels));
		frame.commands.push_back(std::move(out));
	}
	return true;
}

inline bool frameHasOpaqueBase(const Frame &frame, int width, int height)
{
	if (frame.commands.empty()) return false;
	const Command &first = frame.commands.front();
	using Type = gea::platform::display::DisplayPresentCommandType;
	if (first.type == Type::Clear) return true;
	if (first.alpha != 255) return false;
	Rect bounds;
	if (first.type == Type::FillRectRgb565) {
		bounds = rectFromBox(first.x, first.y, first.w, first.h);
	} else if (first.type == Type::DrawImage && first.alphaPixels == nullptr) {
		// A full-panel opaque image (putImageData of a whole-screen buffer)
		// paints every pixel, the same as a full-panel fill.
		bounds = rectFromBox(first.x, first.y, first.srcWidth, first.srcHeight);
	} else {
		return false;
	}
	return bounds.x0 <= 0 && bounds.y0 <= 0 && bounds.x1 >= width - 1 && bounds.y1 >= height - 1;
}

inline int dirtyRects(const Frame *previous,
                      const Frame &current,
                      Rect *rects,
                      int capacity,
                      int width,
                      int height)
{
	if (!rects || capacity <= 0 || width <= 0 || height <= 0) return 0;
	if (!frameHasOpaqueBase(current, width, height)) {
		rects[0] = fullScreen(width, height);
		return 1;
	}
	if (!previous || !frameHasOpaqueBase(*previous, width, height)) {
		rects[0] = fullScreen(width, height);
		return 1;
	}

	int count = 0;
	const std::size_t commandCount = std::max(previous->commands.size(), current.commands.size());
	for (std::size_t i = 0; i < commandCount; i++) {
		const Command *oldCommand = i < previous->commands.size() ? &previous->commands[i] : nullptr;
		const Command *newCommand = i < current.commands.size() ? &current.commands[i] : nullptr;
		if (oldCommand && newCommand && commandsEqual(*oldCommand, *newCommand)) continue;
		if (oldCommand) addCommandBounds(*oldCommand, rects, &count, capacity, width, height);
		if (newCommand) addCommandBounds(*newCommand, rects, &count, capacity, width, height);
	}

	int totalArea = 0;
	for (int i = 0; i < count; i++) totalArea += area(rects[i]);
	if (totalArea >= width * height) {
		rects[0] = fullScreen(width, height);
		return 1;
	}
	return count;
}

// Cheap vertical extent of a command, for skipping whole flush chunks.
// Returns false for types without a cheap extent (Clear, FillText, the
// pre-banded circle batch) — those must be visited by every chunk, and
// outY0/outY1 are set to the full display range.
inline bool commandRowExtent(const Command &command, int displayHeight, int &outY0, int &outY1)
{
	using Type = gea::platform::display::DisplayPresentCommandType;
	switch (command.type) {
	case Type::FillRectRgb565:
	case Type::StrokeRectRgb565:
		outY0 = command.y;
		outY1 = command.y + command.h - 1;
		return true;
	case Type::FillTriangleRgb565: {
		const int lo01 = command.y0 < command.y1 ? command.y0 : command.y1;
		const int hi01 = command.y0 > command.y1 ? command.y0 : command.y1;
		outY0 = lo01 < command.y2 ? lo01 : command.y2;
		outY1 = hi01 > command.y2 ? hi01 : command.y2;
		return true;
	}
	case Type::FillTrianglesRgb565:
		// Batch-wide extent cached at extract time; per-entry rejects happen
		// inside the raster loop.
		outY0 = command.y0;
		outY1 = command.y1;
		return true;
	case Type::FillCircleRgb565:
	case Type::StrokeCircleRgb565:
		outY0 = command.y - command.radius;
		outY1 = command.y + command.radius;
		return true;
	case Type::DrawImage:
	case Type::DrawImageTiledX:
		outY0 = command.y;
		outY1 = command.y + command.srcHeight - 1;
		return true;
	case Type::DrawImageScaled:
	case Type::DrawImageRotated90CW:
		outY0 = command.y;
		outY1 = command.y + command.h - 1;
		return true;
	case Type::FillText:
		if (command.fontFamilyId < 0) {
			const Rect bounds = textBounds(command);
			outY0 = bounds.y0;
			outY1 = bounds.y1;
			return true;
		}
		break;
	default:
		break;
	}
	outY0 = 0;
	outY1 = displayHeight - 1;
	return false;
}

inline void rasterCommandRows(gea::framework::graphics::Canvas &canvas,
                              gea::framework::graphics::pixel::native_t *buffer,
                              int regionX0,
                              int regionWidth,
                              int row,
                              int chunkRows,
                              int displayHeight,
                              const Command &command)
{
	if (!buffer) return;
	using Type = gea::platform::display::DisplayPresentCommandType;
	// Row-extent reject: the frame's command list is replayed once per flush
	// chunk, so a command whose vertical extent misses [row, row+chunkRows)
	// must cost O(1) here. Without this, every 16-row chunk dispatched EVERY
	// command into Canvas (bind + primitive-level clipping) — ~6.3k dispatches
	// per frame for a 360-triangle scene, ~30ms/frame on esp32-s3. With it, a
	// triangle spanning ~40 rows is visited by the ~3 chunks it touches.
	// (Callers that replay many chunks should hoist commandRowExtent() out of
	// their chunk loop and skip before calling — this in-function reject is
	// the safety net for one-shot callers.)
	{
		const int chunkY1 = row + chunkRows - 1;
		int cmdY0 = 0;
		int cmdY1 = displayHeight - 1;
		const bool bounded = commandRowExtent(command, displayHeight, cmdY0, cmdY1);
		if (bounded && (cmdY1 < row || cmdY0 > chunkY1)) return;
	}
	if (command.type == Type::Clear) {
		const int pixelCount = regionWidth * chunkRows;
		if (command.color == 0) std::memset(buffer, 0, static_cast<std::size_t>(pixelCount) * sizeof(std::uint16_t));
		else std::fill_n(buffer, pixelCount, command.color);
		return;
	}

	canvas.setGlobalAlpha(command.alpha);
	const int ox = regionX0;
	const int oy = row;
	switch (command.type) {
	case Type::Clear:
		break;
	case Type::FillRectRgb565:
		// Present replay fills a fresh flush chunk from scratch, so Canvas::fillRect's
		// persistent-surface optimizations (per-pixel dst-compare to skip unchanged pixels +
		// run-based markDirty) are pure overhead here — every pixel is new and the present
		// already owns the dirty region. Opaque rects (the full-screen background + floor
		// band that cover most of every chunk) take the flat span fill; only translucent
		// rects need the per-pixel blend.
		if (command.alpha == 255)
			canvas.fillRectOpaque(command.x - ox, command.y - oy, command.w, command.h, command.color);
		else
			canvas.fillRect(command.x - ox, command.y - oy, command.w, command.h, command.color);
		break;
	case Type::StrokeRectRgb565:
		canvas.strokeRect(command.x - ox, command.y - oy, command.w, command.h, command.color);
		break;
	case Type::FillTriangleRgb565:
		// Same rationale as the opaque-rect branch above: replay chunks are
		// fresh, so skip the per-pixel dst-compare + markDirty bookkeeping.
		if (command.alpha == 255)
			canvas.fillTriangleOpaque(command.x0 - ox, command.y0 - oy,
			                          command.x1 - ox, command.y1 - oy,
			                          command.x2 - ox, command.y2 - oy,
			                          command.color);
		else
			canvas.fillTriangle(command.x0 - ox, command.y0 - oy,
			                    command.x1 - ox, command.y1 - oy,
			                    command.x2 - ox, command.y2 - oy,
			                    command.color);
		break;
	case Type::FillCircleRgb565:
		canvas.fillCircle(command.x - ox, command.y - oy, command.radius, command.color);
		break;
	case Type::StrokeCircleRgb565:
		canvas.strokeCircle(command.x - ox, command.y - oy, command.radius, command.color);
		break;
	case Type::FillCirclesRgb565: {
		// Hand the whole sorted batch to Canvas's optimized batch entrypoint.
		// It inlines fillCircleNoDirty's hot path and uses [chunkY0, chunkY1]
		// as a world-Y band to skip leading + break trailing in one loop.
		const int chunkY0 = row;
		const int chunkY1 = row + chunkRows - 1;
		canvas.fillCirclesRgb565WorldYSorted(command.circleEntries(),
		                                     command.circlesCount,
		                                     command.radius,
		                                     chunkY0, chunkY1, ox, oy);
		break;
	}
	case Type::FillTrianglesRgb565: {
		// Depth-ordered batch (farthest-first). OPAQUE batches take the span-
		// occlusion path (front-to-back, each pixel written once); it band-rejects
		// per chunk internally. TRANSLUCENT must stay painter's back-to-front.
		const gea::framework::graphics::TriangleEntry *tris = command.trianglesBuffer.get();
		const int count = command.trianglesCount;
		if (!tris) break;
		if (command.alpha == 255) {
			canvas.fillTrianglesOpaqueOccluded(tris, count, ox, oy);
		} else {
			const int chunkY1 = row + chunkRows - 1;
			for (int i = 0; i < count; i++) {
				const auto &t = tris[i];
				if (t.rowY1 < row || t.rowY0 > chunkY1) continue;
				canvas.fillTriangle(t.x0 - ox, t.y0 - oy, t.x1 - ox, t.y1 - oy,
				                    t.x2 - ox, t.y2 - oy, t.color);
			}
		}
		break;
	}
	case Type::DrawImage:
		canvas.drawImage(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
		                 command.x - ox, command.y - oy);
		break;
	case Type::DrawImageScaled:
		// A corner radius masks the destination box. All four equal to
		// min(w,h)/2 is a circle -- how an OPAQUE sprite gets a round mask with
		// no alpha plane at all, which is both smaller in flash and cheaper to
		// blit than carrying a mask channel.
		if (command.radius > 0) {
			canvas.drawImageRounded(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                        command.x - ox, command.y - oy, command.w, command.h,
			                        command.radius, command.radius, command.radius, command.radius);
			break;
		}
		// 1:1 "scaled" draws (integer-zoom map tiles) take the unscaled blit:
		// row memcpys instead of per-pixel nearest sampling (~2.5x faster).
		if (command.w == command.srcWidth && command.h == command.srcHeight)
			canvas.drawImage(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                 command.x - ox, command.y - oy);
		else
			canvas.drawImage(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                 command.x - ox, command.y - oy, command.w, command.h);
		break;
	case Type::DrawImageRotated90CW:
		canvas.drawImageRotated90CW(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
		                            command.x - ox, command.y - oy, command.w, command.h);
		break;
	case Type::DrawImageTiledX:
		canvas.drawImageTiledX(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
		                       command.x - ox, command.y - oy, command.w);
		break;
	case Type::FillText:
		if (command.fontFamilyId >= 0)
			canvas.drawTextFontFamily(command.text.c_str(), command.x - ox, command.y - oy, command.color,
			                          command.fontFamilyId, command.fontSizePx);
		else
			canvas.drawText(command.text.c_str(), command.x - ox, command.y - oy, command.color, command.scale);
		break;
	}
	(void)displayHeight;
}

inline void rasterFrameRows(gea::framework::graphics::pixel::native_t *buffer,
                            int regionX0,
                            int regionWidth,
                            int row,
                            int chunkRows,
                            int displayHeight,
                            const Frame &frame)
{
	if (!buffer || regionWidth <= 0 || chunkRows <= 0) return;
	gea::framework::graphics::Canvas canvas;
	canvas.bindPixels(buffer, regionWidth, chunkRows);
	for (const Command &command : frame.commands) {
		rasterCommandRows(canvas, buffer, regionX0, regionWidth, row, chunkRows, displayHeight, command);
	}
}

inline void rasterFrameRowsStrided(gea::framework::graphics::pixel::native_t *buffer,
                                   int stridePixels,
                                   int regionX0,
                                   int regionWidth,
                                   int row,
                                   int chunkRows,
                                   int displayHeight,
                                   const Frame &frame)
{
	if (!buffer || stridePixels < regionWidth || regionWidth <= 0 || chunkRows <= 0) return;
	gea::framework::graphics::Canvas canvas;
	canvas.bindPixels(buffer, regionWidth, chunkRows, stridePixels);
	for (const Command &command : frame.commands) {
		if (command.type == gea::platform::display::DisplayPresentCommandType::Clear) {
			for (int y = 0; y < chunkRows; ++y) {
				gea::framework::graphics::pixel::native_t *dst = buffer + static_cast<std::size_t>(y) * stridePixels;
				if (command.color == 0) {
					std::memset(dst, 0, static_cast<std::size_t>(regionWidth) * sizeof(gea::framework::graphics::pixel::native_t));
				} else {
					std::fill_n(dst, regionWidth, command.color);
				}
			}
			continue;
		}
		rasterCommandRows(canvas, buffer, regionX0, regionWidth, row, chunkRows, displayHeight, command);
	}
}

}  // namespace gea::framework::display_present
