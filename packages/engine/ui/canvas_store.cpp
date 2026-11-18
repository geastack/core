// SPDX-License-Identifier: Apache-2.0
#include "canvas_store.h"

#include "internal.h"
#include "memory.h"

#include <cstddef>
#include <new>

namespace gea::embedded::ui {

namespace {

void releaseOwnedPixels(CanvasSurfaceState *surface)
{
	if (!surface || !surface->ownsPixels()) return;
	// The retained display list can still hold a BlitImage recorded against this
	// buffer (the node's re-record only happens later in the frame). On hosts
	// whose allocator munmaps large frees (geaos) a replay in that gap reads
	// unmapped memory — SIGSEGV. Kill the dangling references before freeing.
	DisplayList::instance().scrubBlitPixels(surface->pixels);
	gea::framework::memory::Allocator::free(surface->pixels);
	surface->pixels = nullptr;
}

void releaseSurface(CanvasSurfaceState *surface)
{
	if (!surface) return;
	releaseOwnedPixels(surface);
	delete surface;
}

}  // namespace

bool CanvasSurfaceState::ownsPixels() const
{
	return mode == CanvasSurfaceMode::OwnedBitmap && pixels != nullptr;
}

bool CanvasSurfaceState::displayBacked() const
{
	return mode == CanvasSurfaceMode::DisplayFramebuffer;
}

gea::framework::graphics::Canvas *CanvasSurfaceState::canvas()
{
	return activeCanvas;
}

const gea::framework::graphics::Canvas *CanvasSurfaceState::canvas() const
{
	return activeCanvas;
}

CanvasSurfaceState *CanvasStore::find(int nodeId)
{
	for (auto *surface = head_; surface; surface = surface->next) {
		if (surface->nodeId == nodeId) return surface;
	}
	return nullptr;
}

const CanvasSurfaceState *CanvasStore::find(int nodeId) const
{
	for (const auto *surface = head_; surface; surface = surface->next) {
		if (surface->nodeId == nodeId) return surface;
	}
	return nullptr;
}

CanvasSurfaceState *CanvasStore::ensure(int nodeId)
{
	if (auto *surface = find(nodeId)) return surface;
	auto *surface = new (std::nothrow) CanvasSurfaceState();
	if (!surface) return nullptr;
	surface->nodeId = nodeId;
	surface->next = head_;
	head_ = surface;
	return surface;
}

gea::framework::graphics::Canvas *CanvasStore::bindOwned(CanvasSurfaceState &surface, int width, int height)
{
	if (width <= 0 || height <= 0) return nullptr;

	const bool copyFromDisplay = surface.displayBacked() && surface.canvas() &&
	                             surface.canvas()->pixels() &&
	                             surface.canvas()->width() == width &&
	                             surface.canvas()->height() == height;
	const gea::framework::graphics::pixel::native_t *displayPixels = copyFromDisplay ? surface.canvas()->pixels() : nullptr;

	if (!surface.displayBacked() && surface.pixels && surface.width == width && surface.height == height) {
		surface.activeCanvas = &surface.localCanvas;
		return surface.canvas();
	}

	releaseOwnedPixels(&surface);
	surface.mode = CanvasSurfaceMode::OwnedBitmap;
	surface.activeCanvas = &surface.localCanvas;
	surface.pixels = nullptr;
	surface.width = 0;
	surface.height = 0;

	// Cache-line aligned (size rounded up to the alignment) so DMA engines — the
	// ESP32-P4 PPA in particular — can scale/rotate straight into this buffer with
	// no bounce copy. A few padding bytes per canvas is a negligible cost.
	// Apple hosts align to the page instead, so the GPU can read the buffer in
	// place (a Metal buffer wraps whole, page-aligned pages) and presenting a
	// canvas copies nothing.
#ifdef __APPLE__
	constexpr std::size_t kCanvasAlign = 16384;
#else
	constexpr std::size_t kCanvasAlign = 128;
#endif
	const auto pixelBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(gea::framework::graphics::pixel::native_t);
	const auto byteCount = (pixelBytes + (kCanvasAlign - 1)) & ~(kCanvasAlign - 1);
	surface.pixels = static_cast<gea::framework::graphics::pixel::native_t *>(
	    gea::framework::memory::Allocator::allocatePreferSpiram(byteCount, kCanvasAlign));
	if (!surface.pixels) return nullptr;

	surface.width = width;
	surface.height = height;
	surface.localCanvas.bindPixels(surface.pixels, width, height);
	if (displayPixels)
		surface.localCanvas.drawImage(displayPixels, nullptr, width, height, 0, 0);
	else
		surface.localCanvas.clear(0);
	return surface.canvas();
}

void CanvasStore::markDisplayFramebuffer(CanvasSurfaceState &surface, int width, int height)
{
	if (width <= 0 || height <= 0) return;
	releaseOwnedPixels(&surface);
	surface.mode = CanvasSurfaceMode::DisplayFramebuffer;
	surface.activeCanvas = nullptr;
	surface.pixels = nullptr;
	surface.width = width;
	surface.height = height;
}

gea::framework::graphics::Canvas *CanvasStore::bindDisplayFramebuffer(CanvasSurfaceState &surface,
                                                                      gea::framework::graphics::Canvas &displayCanvas)
{
	if (!displayCanvas.pixels() || displayCanvas.width() <= 0 || displayCanvas.height() <= 0) return nullptr;
	if (surface.displayBacked() &&
	    surface.canvas() == &displayCanvas &&
	    surface.pixels == displayCanvas.pixels() &&
	    surface.width == displayCanvas.width() &&
	    surface.height == displayCanvas.height())
		return surface.canvas();

	const bool copyFromOwned = surface.ownsPixels() &&
	                           surface.width == displayCanvas.width() &&
	                           surface.height == displayCanvas.height();
	gea::framework::graphics::pixel::native_t *ownedPixels = copyFromOwned ? surface.pixels : nullptr;

	if (ownedPixels) displayCanvas.drawImage(ownedPixels, nullptr, surface.width, surface.height, 0, 0);
	releaseOwnedPixels(&surface);

	surface.mode = CanvasSurfaceMode::DisplayFramebuffer;
	surface.activeCanvas = &displayCanvas;
	surface.pixels = displayCanvas.pixels();
	surface.width = displayCanvas.width();
	surface.height = displayCanvas.height();
	return surface.canvas();
}

void CanvasStore::remove(int nodeId)
{
	CanvasSurfaceState **link = &head_;
	while (*link) {
		auto *surface = *link;
		if (surface->nodeId != nodeId) {
			link = &surface->next;
			continue;
		}
		*link = surface->next;
		releaseSurface(surface);
		return;
	}
}

void CanvasStore::clear()
{
	auto *surface = head_;
	while (surface) {
		auto *next = surface->next;
		releaseSurface(surface);
		surface = next;
	}
	head_ = nullptr;
}

}  // namespace gea::embedded::ui
