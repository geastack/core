// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "canvas.h"

#include <cstdint>

namespace gea::embedded::ui {

enum class CanvasSurfaceMode : std::uint8_t {
	OwnedBitmap,
	DisplayFramebuffer
};

struct CanvasSurfaceState {
	int nodeId = -1;
	CanvasSurfaceMode mode = CanvasSurfaceMode::OwnedBitmap;
	gea::framework::graphics::Canvas localCanvas;
	gea::framework::graphics::Canvas *activeCanvas = nullptr;
	gea::framework::graphics::pixel::native_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	std::uint32_t redraws = 0;
	CanvasSurfaceState *next = nullptr;

	bool ownsPixels() const;
	bool displayBacked() const;
	gea::framework::graphics::Canvas *canvas();
	const gea::framework::graphics::Canvas *canvas() const;
};

class CanvasStore {
public:
	CanvasSurfaceState *find(int nodeId);
	const CanvasSurfaceState *find(int nodeId) const;
	CanvasSurfaceState *ensure(int nodeId);
	gea::framework::graphics::Canvas *bindOwned(CanvasSurfaceState &surface, int width, int height);
	void markDisplayFramebuffer(CanvasSurfaceState &surface, int width, int height);
	gea::framework::graphics::Canvas *bindDisplayFramebuffer(CanvasSurfaceState &surface,
	                                                         gea::framework::graphics::Canvas &displayCanvas);
	void remove(int nodeId);
	void clear();

	template <typename Fn>
	void forEach(Fn fn)
	{
		for (auto *surface = head_; surface; surface = surface->next) fn(*surface);
	}

	template <typename Fn>
	void forEach(Fn fn) const
	{
		for (const auto *surface = head_; surface; surface = surface->next) fn(*surface);
	}

private:
	CanvasSurfaceState *head_ = nullptr;
};

}  // namespace gea::embedded::ui
