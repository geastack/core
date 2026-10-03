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
namespace gea::framework::display
{
	class Underlay
	{
	public:
		// Fills [x0, x1] x [y0, y1] (inclusive, screen coordinates) of
		// Display::canvas(), honouring its current clip.
		using Paint = void (*)(int x0, int y0, int x1, int y1, void *user);

		// True when a retained UI tree is mounted and refreshes on its own, so a
		// registered underlay will be painted. False for direct-canvas apps,
		// which present their frames themselves.
		static bool composited();
		// Registers the painter for the screen area it covers; replaces any
		// previous one. The caller marks the area dirty to have it repainted.
		static void set(Paint paint, void *user, int x0, int y0, int x1, int y1);
		static void clear();
		static bool active();
		// Paints the part of [x0, x1] x [y0, y1] the underlay covers.
		static void paint(int x0, int y0, int x1, int y1);
	};
}  // namespace gea::framework::display
