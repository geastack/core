#pragma once

// A layer painted UNDER the retained UI tree, in the same flush.
//
// A native renderer that owns the whole screen (gea3d) cannot stream its frame
// to the panel on its own while a UI tree is mounted: the UI's next dirty-rect
// flush copies the framebuffer, which never held that frame, over it. Instead
// it registers a painter here and marks the canvas dirty; the tree's refresh
// then calls the painter for each dirty rect (clipped to it, drawing into the
// bound canvas) before it replays the nodes, so the HUD lands on top and the
// panel sees one composited image.
//
// A target that records draw commands instead of pixels asks the underlay to
// describe itself as geometry instead (`describe`), when its painter can.
#include <cstdint>

namespace gea::framework::display
{
	class Underlay
	{
	public:
		// Fills [x0, x1] x [y0, y1] (inclusive, screen coordinates) of
		// Display::canvas(), honouring its current clip.
		using Paint = void (*)(int x0, int y0, int x1, int y1, void *user);

		// A triangle, or a line `width` pixels across its minor axis from
		// corner 0 to corner 1. Corners are screen coordinates in 28.4 fixed
		// point, colours 0xRRGGBB, interpolated linearly in screen space.
		struct Primitive {
			enum Kind : std::uint8_t { kTriangle = 0, kLine = 1 };
			enum Blend : std::uint8_t { kSourceOver = 0, kAdd = 1 };
			std::int32_t x[3];
			std::int32_t y[3];
			std::uint32_t color[3];
			Kind kind;
			Blend blend;
			std::uint8_t alpha;  // multiplies the colour; 255 for opaque primitives
			std::uint8_t width;
		};
		class Sink {
		public:
			// The registered area (inclusive), before anything else.
			virtual void area(int x0, int y0, int x1, int y1) = 0;
			// Fills the area; row r of n gets top + (bottom - top) * r / (n - 1).
			virtual void background(std::uint32_t top, std::uint32_t bottom) = 0;
			virtual void primitive(const Primitive &primitive) = 0;
		};
		// Hands the content Paint would draw to `sink`, in painter's order.
		using Describe = void (*)(Sink &sink, void *user);

		// True when a retained UI tree is mounted and refreshes on its own, so a
		// registered underlay will be painted. False for direct-canvas apps,
		// which present their frames themselves.
		static bool composited();
		// Registers the painter for the screen area it covers; replaces any
		// previous one. The caller marks the area dirty to have it repainted.
		static void set(Paint paint, void *user, int x0, int y0, int x1, int y1, Describe describe = nullptr);
		// Declares that the painter writes every pixel of its area (a 3D frame
		// with a background colour). `set` resets it to false.
		static void setOpaque(bool opaque);
		// True when an opaque underlay paints all of [x0, x1] x [y0, y1]: a
		// refresh that paints it there repaints that rect completely, so the
		// display need not restore the rect's old pixels first.
		static bool covers(int x0, int y0, int x1, int y1);
		static void clear();
		static bool active();
		// Paints the part of [x0, x1] x [y0, y1] the underlay covers.
		static void paint(int x0, int y0, int x1, int y1);
		// Describes the underlay to `sink`; false when none is registered or
		// it was registered without a Describe.
		static bool describe(Sink &sink);
	};
}  // namespace gea::framework::display
