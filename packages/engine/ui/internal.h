// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "tree_internal.h"

#include <cstdint>
#include <vector>

#ifdef quad
#undef quad
#endif

namespace gea::embedded::ui
{

// True while the root-scroll fast path repaints its exposed strips / moved
// slots. On packed e-paper targets the image blits skip while this is set —
// a fling shows paper where covers would raster (the moved pixels themselves
// scrollRect along and stay visible). Cuts the per-frame strip cost to
// text+fills; rootScrollImageHoldTick() repaints once with images on settle.
extern bool gScrollStripReplayActive;
// Set by the root-scroll fast path on every frame it actually scrolls.
extern bool gScrollRanThisFrame;
// Frame counter (ticked once per refresh pass) + the frame of the last scroll:
// the image hold applies only to CONSECUTIVE scroll frames (a live drag or
// fling), never to a one-shot jump like a row-snap commit — that single frame
// paints its images inline and needs no deferred repaint.
extern int gUiFrameCounter;
extern int gLastScrollUiFrame;
// Set when a blit was skipped under the hold; consumed by the settle tick.
extern bool gScrollImageHoldPending;
// Frame-entry tick: the first frame WITHOUT scroll activity after skipped
// blits marks the tree dirty for one normal full refresh (images included).
void rootScrollImageHoldTick();

	bool isCollapsedFlexItem(const Node &node);
	bool isCollapsedFlexSubtree(const Node &node);

	class CameraSurfaceProvider;

	// Max children collected per node in one recordNode level. The record scratch
	// is kScratchDepth * kMaxChildren ints; at 256 that is a 16 KB per-record heap
	// allocation, which FAILS on a PSRAM-less, heap-fragmented part (ESP32-C3
	// Xteink X3, largest free block ~16 KB) — childrenForDepth() then returns
	// nullptr and recordNode() never recurses past the root, so nothing renders.
	// Overridable so such boards can shrink it to a value whose scratch fits.
#ifndef GEA_EMBEDDED_MAX_CHILDREN
#define GEA_EMBEDDED_MAX_CHILDREN 256
#endif
	inline constexpr int kMaxChildren = GEA_EMBEDDED_MAX_CHILDREN;
	inline constexpr int kMaxFlexLines = 32;

	enum class DisplayCommandType : uint8_t
	{
		PushClip,
		PopClip,
		SetAlpha,
		FillRect,
		FillCircle,
		FillRoundedRect,
		FillQuad,
		FillLinearGradient,
		FillRadialGradient,
		DrawLine,
		StrokeRect,
		StrokeRoundedRect,
		DrawProjectedText,
		DrawText,
		BlitImage,
		BlitImageScaled,
		BeginFilterBlur,
		ApplyFilterBlur,
		FillTransformedLinearGradient,
		FillTransformedRoundedRect
	};

	struct DisplayCommand
	{
		DisplayCommandType type;
		bool textDecorationInk = false;
		int16_t bx, by, bw, bh;
		// Background paint can use descendant glyph coverage without changing
		// foreground text color or allocating a viewport-sized mask.
		int16_t textClipOwner = -1;
		union
		{
			struct
			{
				int16_t x, y, w, h;
				// Clip scopes are recorded outside the owning node's paint-command
				// range. Keep the owner so retained subtree translations can move the
				// scope with the node (and undo the scroll viewport's own movement).
				int16_t nodeId;
			} clip;
			struct
			{
				uint8_t alpha;
				// OPEN SetAlpha of an opacity scope is tagged with its node so a
				// partial→partial fade can patch the value in place
				// (DisplayList::patchNodeAlpha): nodeId = owning node (-1 on the CLOSE
				// command), recordParentAlpha = parent alpha at record time so the
				// patch recomputes alpha = recordParentAlpha * opacity / 255.
				uint8_t recordParentAlpha;
				int16_t nodeId;
			} alpha;
			struct
			{
				int16_t x, y, w, h;
				gea::framework::graphics::pixel::native_t color;
			} fill;
			struct
			{
				int16_t cx, cy, r;
				gea::framework::graphics::pixel::native_t color;
			} fillCircle;
			struct
			{
				int16_t x, y, w, h;
				int16_t tl, tr, br, bl;
				gea::framework::graphics::pixel::native_t color;
			} fillRoundedRect;
			struct
			{
				int16_t x0, y0, x1, y1, x2, y2, x3, y3;
				// Transform-reproject payload. mode 1: lx/ly/lw/lh are a local rect.
				// mode 2: lx/ly/lw/lh are local segment endpoints in 1/8 px; aux is
				// stroke width in 1/8 screen px.
				int16_t lx, ly, lw, lh, aux;
				gea::framework::graphics::pixel::native_t color;
				uint8_t reprojectMode;
			} quad;
			struct
			{
				int16_t x, y, w, h;
				int16_t tl, tr, br, bl;
				gea::framework::graphics::pixel::native_t fromColor;
				gea::framework::graphics::pixel::native_t midColor;
				gea::framework::graphics::pixel::native_t toColor;
				uint16_t midStop;
				int16_t angle;
				uint8_t fromAlpha;
				uint8_t midAlpha;
				uint8_t toAlpha;
				uint16_t toStop;
				uint8_t hasMid;
			} gradient;
			struct
			{
				int16_t x, y, w, h;
				int16_t tl, tr, br, bl;
				int16_t cxPermille, cyPermille;
				int16_t rxPermille, ryPermille;
				gea::framework::graphics::pixel::native_t fromColor;
				gea::framework::graphics::pixel::native_t toColor;
				uint16_t stopPermille;
				uint8_t fromAlpha;
				uint8_t toAlpha;
			} radialGradient;
			struct
			{
				int16_t x0, y0, x1, y1;
				gea::framework::graphics::pixel::native_t color;
			} line;
			struct
			{
				int16_t x, y, w, h;
				gea::framework::graphics::pixel::native_t color;
			} stroke;
			struct
			{
				int16_t x, y, w, h;
				int16_t tl, tr, br, bl;
				int16_t lineWidth;
				gea::framework::graphics::pixel::native_t color;
				// CSS radii are already normalized on the outer border box. Inner
				// curves may exceed half the clipped box; do not clamp them again.
				int16_t rx8[4], ry8[4];
				uint8_t cssRadii;
			} strokeRoundedRect;
			struct
			{
				const char *text;
				int16_t x, y, maxWidth;
				gea::framework::graphics::pixel::native_t color;
				float scale;
				int8_t align;
				int8_t textTransform;
				int16_t lineHeight;
				int16_t containerWidth;
				int16_t fontId;
				// CSS white-space / text-overflow, carried so the deferred draw
				// can reproduce single-line truncation. whiteSpace==1 => nowrap
				// (no width-based wrapping); textOverflow==1 => ellipsis ("...").
				int8_t whiteSpace;
				int8_t textOverflow;
				// Content-box height. With ellipsis on WRAPPED text this caps the
				// line budget (height / line advance) and the last line ellipsizes
				// — the multi-line clamp. 0 = unlimited (draw every line).
				int16_t maxHeight;
				// Inline continuation offset for the run's FIRST line only (see
				// LayoutBox::inline_indent). 0 for every run that starts its own line.
				int16_t firstLineIndent;
			} text;
			struct
			{
				const char *text;
				int16_t srcX, srcY, srcW, srcH;
				int16_t x0, y0, x1, y1, x2, y2, x3, y3;
				int16_t fontId, fontSize;
				gea::framework::graphics::pixel::native_t color;
				uint8_t alpha;
				int8_t textTransform;
				int8_t whiteSpace;
				// CSS backface-visibility:hidden — drop this projected label when its winding
				// points away from the viewer, matching its (culled) back-facing parent face.
				uint8_t backfaceHidden;
			} projectedText;
			// A 3D/perspective-transformed linear-gradient face, shaded per pixel via
			// inverse homography (screen->local) + gradient LUT — replaces the old
			// ~72-strips-per-face approximation (one quad command, exact gradient).
			struct
			{
				int16_t x0, y0, x1, y1, x2, y2, x3, y3; // screen corners (TL,TR,BR,BL)
				// Same corners in 1/8-px fixed point. The int16 corners above round each
				// face's projected vertices independently, so two faces meeting at a shared
				// 3D edge can land ~1px apart at round-to-nearest straddles, leaving a
				// shimmering backdrop crack. The rasterizer uses these sub-pixel corners so
				// adjacent faces' shared edges coincide and tile watertight (no bloat, no AA).
				int16_t fx0, fy0, fx1, fy1, fx2, fy2, fx3, fy3;
				int16_t lx, ly, lw, lh;																									// local (untransformed) rect
				gea::framework::graphics::pixel::native_t fromColor, midColor, toColor; // native pixel stops
				uint16_t midStop, toStop;																								// permille
				int16_t angle;																													// gradient angle, tenths of degree
				uint8_t fromAlpha, midAlpha, toAlpha, hasMid;
				// CSS backface-visibility:hidden — drop this face when its projected winding
				// points away from the viewer (a closed opaque solid occludes its back faces
				// anyway, so culling them removes the wasted overdraw). 0 => always drawn.
				uint8_t backfaceHidden;
				// CSS border on the transformed face, drawn by the span rasterizer as an
				// edge frame (the first/last edgeWidth px of every span + the quad's
				// top/bottom rows trace the full outline). Folded into this command so
				// borders don't emit projected stroke quads — those would disarm the
				// transform-reproject fast path every frame. edgeWidth 0 => no border.
				gea::framework::graphics::pixel::native_t edgeColor;
				uint8_t edgeAlpha;
				uint8_t edgeWidth;
			} transformedGradient;
			struct
			{
				int16_t x0, y0, x1, y1, x2, y2, x3, y3; // screen corners (TL,TR,BR,BL)
				int16_t lx, ly, lw, lh;									// local (untransformed) rect
				int16_t tlRx8, tlRy8;
				int16_t trRx8, trRy8;
				int16_t brRx8, brRy8;
				int16_t blRx8, blRy8;
				gea::framework::graphics::pixel::native_t color;
				uint8_t backfaceHidden;
			} transformedRoundedRect;
			struct
			{
				const gea::framework::graphics::pixel::native_t *pixels;
				const uint8_t *alpha;
				int16_t sourceWidth, sourceHeight, dx, dy;
				// Packed grayscale targets only: the source is a sub-byte-packed canvas
				// surface (2 px/byte on GRAY4, 4 px/byte on GRAY2) rather than an
				// unpacked decoded image, so the blit must unpack it.
				uint8_t sourcePacked;
			} blit;
			struct
			{
				const gea::framework::graphics::pixel::native_t *pixels;
				const uint8_t *alpha;
				int16_t sourceWidth, sourceHeight, dx, dy, dw, dh;
				int16_t tl, tr, br, bl;
			} scaledBlit;
			struct
			{
				int16_t nodeId;
				int16_t radius;
				int16_t radiusX;
				int16_t radiusY;
				int16_t sourceAlphaCap;
			} filterBlur;
		};
	};

	struct DisplayReplayRegion
	{
		int x0;
		int y0;
		int x1;
		int y1;
		int origin;
	};

	class DisplayList
	{
	public:
		static DisplayList &instance();

		DisplayCommand *append();
		int setRecordingTextClipOwner(int owner);
		void clear();
		void resetStorage();
		// allowSkipStatic: caller guarantees this frame will use the direct
		// (backdrop-blit) replay path, so recordNode may skip re-recording fully-static
		// subtrees when the backdrop is baked. Must be false on the full-replay path.
		void recordNode(int id, uint8_t parentAlpha, bool allowSkipStatic = false);
		void recordNodeWithExpandedClip(int id, uint8_t parentAlpha, int clipNode,
																		int clipX0, int clipY0, int clipX1, int clipY1);
		void replay();
		bool canReplayDirectDirtyRegions(int width, int height) const;
		void translateNodeCommands(int node, int dx, int dy);
		void translateSubtreeCommands(int node, int dx, int dy);
		// After translating a subtree in place, content that was fully outside the
		// record-time clip (culled: no commands recorded) may now overlap the
		// viewport. The retained list is then incomplete — replay would paint stale
		// background where that content entered — so the caller must fall back to a
		// full record. Returns true when such a reveal exists.
		bool subtreeRevealsUnrecordedContent(int node, int width, int height) const;
		bool rerecordNodeCommands(int node);
		// A pixel buffer referenced by retained blit commands is about to be freed
		// (canvas surface mode flip / resize / removal). The owning node is only
		// *marked* dirty — its re-record happens later — so any replay in the gap
		// would read freed memory. Neutralize every retained command still pointing
		// at the buffer.
		void scrubBlitPixels(const gea::framework::graphics::pixel::native_t *pixels);
		// A node's text buffer is about to be freed (node destruction) or has just
		// been reallocated (setText growing past capacity). Retained DrawText /
		// DrawProjectedText commands hold a raw c_str() pointer into that buffer,
		// and a stale-list replay (partial refresh, backdrop restore) in the gap
		// before the next record would read freed memory — on device this crashed
		// (LoadStoreError) replaying the reader after the TOC unmounted its rows.
		// Neutralize every retained text command still pointing at the buffer.
		void scrubNodeText(const char *data);
		// Patch a node's opacity-scope alpha in place (partial→partial fade) without a
		// rebuild. Returns false if the node has no SetAlpha scope in the current list.
		bool patchNodeAlpha(int node, uint8_t opacity);
		// Bumped by clear(): lets a caller detect that its recorded coverage (e.g. the
		// root-scroll translate window) was replaced by someone else's record.
		std::uint32_t recordSerial() const;
		// True when an append() was dropped since the last clear() — the recorded
		// list is truncated and must not be treated as complete coverage.
		bool commandOverflow() const;
		// Move a scroll node's recorded scrollbar thumb (the single FillRoundedRect
		// recordScrollbar emits, which lives OUTSIDE the node's draw range) from its
		// previous_scroll_y position to the current scroll_y position, in place.
		// Returns false when the thumb command isn't found — caller must full-record.
		bool patchScrollbarThumb(int node);
		bool recolorRetainedSolidBackground(int node, gea::framework::graphics::pixel::native_t oldColor, gea::framework::graphics::pixel::native_t newColor, int *x0, int *y0, int *x1, int *y1);
		// A retained-background recolor rewrites the background pixels in place; the
		// span replace only touches pixels that match the OLD colour exactly. Every
		// antialiased edge of a node drawn OVER that background is a blend of the old
		// colour with the foreground colour, so it matches nothing and is left stale —
		// the tick marks and labels on a recoloured dial keep a halo of the previous
		// background. Report the paint bounds of the later-drawn nodes that overlap the
		// recoloured area so the caller can add them as ordinary dirty replay regions:
		// a scoped direct replay of those rects redraws background + foreground exactly
		// as a full replay would, with no per-command-type special casing (glyph runs
		// have no coverage function to re-blend against).
		// Writes x0,y0,x1,y1 quadruples; returns the rect count (never more than
		// maxRects — the overflow is unioned into the last slot).
		int collectRecolorForegroundRepairRects(int node, int x0, int y0, int x1, int y1,
																							int *outRects, int maxRects) const;
		void replayDirectDirtyRegion(int x0, int y0, int x1, int y1, int origin = -1);
		void replayDirectDirtyRegions(const DisplayReplayRegion *regions, int count);
		bool canReplaySimpleDirtyRegions(int width, int height) const;
		void replaySimpleClippedDirtyRegion(int x0, int y0, int x1, int y1, int origin = -1);
		// Static-backdrop cache: bake every non-dirty (static) node into the bg cache
		// buffer once it's stable, so the dirty-region replay can blit that backdrop and
		// replay only the dynamic subtree (the spinning cube) on top — making the static
		// stage free per frame. invalidate clears it on a structural change.
		// stableThisFrame: nothing happened that could stale baked pixels (quiet
		// pause/ticker frames included) — false drops the cache. bakeEligibleThisFrame:
		// a dynamic root is actively dirty, so a bake taken now can exclude it — quiet
		// frames keep the cache but must not bake (the paused subtree would be baked in).
		void maybeBakeStaticBackdrop(int width, int height, bool stableThisFrame, bool directReplay,
																 bool bakeEligibleThisFrame = true);
		void invalidateStaticBackdrop();
		bool staticBackdropActive() const;
		// True once after each bake: caller must repaint the full viewport from the
		// cache that frame so the framebuffer/panel become exactly the baked backdrop.
		bool consumeBackdropFullSyncRequest();
		// Transform-only fast path: re-project every recorded command's corners in place
		// (from its stored local rect through the node's current transform) instead of
		// clear()+recordNode(). Returns false — caller must full-record — when not armed
		// (structure may have changed) or a command type isn't re-projectable. Produces
		// byte-identical geometry to a full record.
		bool tryReprojectTransformed();
		// Watertight vertex weld over the recorded transformed-gradient faces: snaps
		// tight clusters of projected corners (shared 3D vertices projected through
		// independent transform chains) to their centroid so adjacent faces tile
		// without backdrop cracks. Must run after ANY path that (re)creates face
		// commands — tryReprojectTransformed calls it internally; every full record
		// needs an explicit call or shared edges crack for that one frame. Non-null
		// dirty pointers are extended to cover the welded geometry.
		void weldTransformedFaces(int *dirtyX0 = nullptr, int *dirtyY0 = nullptr, int *dirtyX1 = nullptr, int *dirtyY1 = nullptr);
		// Arm/disarm the reproject path after a record. Eligible records keep enough
		// local geometry in their transformed commands for later transform-only frames
		// to patch projected corners in place; if a backdrop cache is active, static
		// subtrees may also have been skipped from the recorded list.
		void armTransformReproject(bool eligible);
		// The (prev ∪ cur) screen bbox the last tryReprojectTransformed() patched — the
		// spinning subtree's dirty rect, free to reuse instead of re-projecting it in the
		// dirty pass. Returns false if empty. Only meaningful on a reprojected frame.
		bool reprojectDirtyRect(int *x0, int *y0, int *x1, int *y1) const;
		void filterBlurCacheStats(int *hits, int *misses) const;
		int commandCount() const;
		bool hasTextClippedBackgrounds() const;
		int nodeCommandCount(int node) const;
		const DisplayCommand *nodeCommandAt(int node, int index) const;
		bool nodeCommandBounds(int node, int *x0, int *y0, int *x1, int *y1) const;
		void clearRetainedBackgroundRecolors();
	};

	class LayoutEngine
	{
	public:
		static LayoutEngine &instance();
		static int alignedAbsoluteOffset(const Node &parent, const Node &child, bool horizontal,
		                                 const Node *containing = nullptr, int containingStart = 0, int containingSize = 0);
		static int fixedContainingBlock(const Node &node);
		static bool isViewportFixed(const Node &node);
		static bool containsViewportFixed(int node);
		static bool absoluteGridArea(const Node &parent, const Node &child, int &x, int &y, int &width, int &height);
		static void absoluteContainingArea(const Node &parent, const Node &child, int &x, int &y, int &width, int &height);

		int clampSize(int size, int minSize, int maxSize) const;
		int collectChildren(int parent, int *out, int max, bool skipAbsolute) const;
		// Starts a new layout pass: invalidates the intra-pass layoutNode memo
		// (LayoutBox::memo_pass). Must be called before each root layoutNode()
		// so memo hits never leak across passes/frames.
		void beginLayoutPass();
		void layoutNode(int id, int availableWidth, int availableHeight, bool intrinsicBoxEdges = false);
		// Scoped relayout: re-lays ONLY `scope`'s subtree using the available box
		// remembered from its last layout, then resolves the subtree's absolute
		// coords from the scope's (unchanged) position. Returns false — caller
		// must full-relayout — when the scope's remembered avail is unknown, an
		// absolute descendant's containing block escapes the scope, or the
		// scope's own dims changed as a result (its content no longer fits the
		// same box, so ancestors need reflow after all).
		bool layoutNodeScoped(int scope, int treeRoot);
		void repositionChildren(int id);
		void resolveAbsoluteCoords(int id, int parentX, int parentY);
		// True when `n` is an inline-level box (a span/text/image without an
		// explicit block-level display) — the inline-formatting input that makes a
		// plain block flow its children as a row (LayoutNodePass::
		// resolveRowDirection). Exposed for refresh fast paths that mirror that
		// flow decision without running a layout pass.
		static bool isInlineLevelNode(const Node &n);
		// CSS display classification without layout-only margin heuristics.
		// Inline-level boxes blockified by float, absolute/fixed positioning, or
		// flex/grid-item status return false.
		static bool isCssInlineLevelBox(const Node &n, bool hypothetical = false);
	};

	// Shared order for recording, retained transform replay, and hit testing.
	class PaintOrder
	{
	public:
		static void sortChildren(int *children, int count, int contextRoot);
		static int compareNodes(int first, int second);
		static bool isContext(int node);
		static bool isGroup(int node);
		static std::vector<int> collectChildren(int node, bool groupRoot = true, bool includePositioned = true);
	};

	class ViewRenderer
	{
	public:
		static bool recordClipBegin(const Node &node);
		static void recordClipEnd(const Node &node);
		static void recordBox(const Node &node, uint8_t parentAlpha = 255);
		static int canvasBackgroundSource();
		static void recordScrollbar(const Node &node);
		static int scrollMaxX(const Node &node);
		static int scrollMaxY(const Node &node);
		static void transformedBounds(const Node &node, bool usePrevious, int *x0, int *y0, int *x1, int *y1);
		static void transformedPoint(const Node &node, bool usePrevious, float x, float y, float z, int16_t *outX, int16_t *outY);
		static void transformedCorners(const Node &node, bool usePrevious, int16_t *xs, int16_t *ys);
		static void transformedRectCorners(const Node &node, bool usePrevious, int x, int y, int w, int h, int16_t *xs, int16_t *ys,
																			 int16_t *xs8 = nullptr, int16_t *ys8 = nullptr);
		static bool backfaceSubtreeHidden(const Node &node);
		static bool isTransformableBox(const Node &node);
		static int transformedDepth(const Node &node, bool usePrevious);
		// True iff any node in the tree carries a transform/perspective this frame
		// or last (cached once per refresh). Lets paint-order sorting skip the 3D
		// depth walk entirely for all-2D trees.
		static bool anyTransformActive();
	};

	// How one text run fragments across the line boxes of an inline formatting
	// context, given that its first line starts part-way along a line box.
	// `lineCount == 0` means the run could not be fragmented (see
	// TextRenderer::canFragmentInlineRuns) and must be flowed as one atomic box.
	struct InlineFlowMeasure {
		int lineCount = 0;
		int firstLineWidth = 0;
		int firstLineTrailingSpace = 0;
		int lastLineWidth = 0;
		int maxLineWidth = 0;
		int lineAdvance = 0;
		// Shift applied to the run's FIRST line, negative by exactly the width of
		// the leading collapsible whitespace CSS drops at the start of a line box.
		// Spaces carry no ink, so drawing the line that much further left renders
		// identically to deleting them — and keeps the run a single unmodified
		// string for the display list.
		int firstLineIndentAdjust = 0;

	};

	class TextRenderer
	{
	public:
		// Shared preparation for measuring and replaying authored text. Storage
		// owns the result only when collapsing or case transformation is needed.
		static const char *prepareText(const char *text, int textTransform, int whiteSpace, std::string &storage);
		static void layout(int id, int availableWidth);
		static int baselineOffset(const Node &node, bool last);
		// True when a run's line breaking is reproducible from per-glyph advances,
		// i.e. the same wrapper the draw path uses. A host that measures whole
		// strings for us (CoreText on Apple targets) does its own breaking, which
		// this engine cannot subdivide — such targets keep flowing runs whole.
		static bool canFragmentInlineRuns(const Node &node);
		// Wraps `node`'s text with `firstAvail` px available on its first line and
		// `contentWidth` on every line after it — the measurement an inline
		// formatting context needs to continue a run after the box that precedes
		// it on the same line box. Leading collapsible whitespace is dropped when
		// `atLineStart`, as CSS does when a line box begins.
		static InlineFlowMeasure measureInlineFlow(const Node &node, int firstAvail, int contentWidth, bool atLineStart);
		static void record(const Node &node, uint8_t parentAlpha = 255);
		static void unionCoverageRow(const DisplayCommand &command, int screenY, int screenX,
		                             int width, uint8_t *outCoverage);
		static void drawWrapped(const char *text, int x, int y, int maxWidth, gea::framework::graphics::pixel::native_t color, float scale, int textAlign,
														int containerWidth, int fontId, int textTransform = 0, int lineHeight = 0, int whiteSpace = 0, int textOverflow = 0, int maxHeight = 0,
														int firstLineIndent = 0);
		// Single-line width measure for places that don't go through layout()
		// — used by InputRenderer to position the caret at the end of the value.
		static int measureWidth(const char *text, int fontId, int fontSize, int textTransform = 0);
		static int firstUnbreakableWidth(const Node &node);
		static int minContentWidth(const Node &node, int *pendingWord = nullptr);
		static int measureHeight(const char *text, int fontId, int fontSize, int textTransform = 0, int lineHeight = 0);
		// In-place box re-measure for a rebound text node on refresh paths that
		// skip the layout pass (root-scroll fast path): record() wraps and clips
		// by layout.width/height, so a longer string in a stale box truncates.
		// Returns true when the box changed. Anchoring is left to the caller.
		// keepBoxWidth re-wraps within the current layout.width (a cross-
		// stretched block's box) and only updates the height.
		static bool remeasureContentBox(int id, bool keepBoxWidth = false);
	};

	// Materializes `<input>` JSX elements. They emit View nodes (tag_name=="input")
	// with no Text child — the user-visible text lives in the `value` attribute
	// (or `placeholder` when `value` is empty). This renderer paints that text on
	// top of the View box recorded by ViewRenderer. Pixel-only targets can pair
	// this with the framework-owned on-screen keyboard; native text targets use
	// platform controls instead.
	class InputRenderer
	{
	public:
		static void layout(int id, int availableWidth);
		static void record(int id);
	};

	class ImageRenderer
	{
	public:
		static void layout(int id);
		static void record(const Node &node);
	};

	class CanvasRenderer
	{
	public:
		static void record(const Node &node);
	};

	// Presents a <camera> leaf's live preview each frame: framebuffer-mode
	// backends fill an owned RGB565 buffer that's blitted into the node's computed
	// rect (CSS clip / z-order honoured); native-overlay backends (iOS) get their
	// preview layer repositioned over the rect. Implemented in camera_element.cpp.
	class CameraRenderer
	{
	public:
		static void record(const Node &node);
		static void positionNativeOverlay(const Node &node, CameraSurfaceProvider *provider);
	};

	// A <virtual-list> windows a small pool of real child slot nodes over a
	// virtual content height of itemCount * rowHeight. It carries no app-specific
	// row content — rows are ordinary child nodes drawn by the generic render
	// path. These helpers only track the item count and proxy the element's scroll
	// position onto the standard layout.scroll_y so the generic overflow:scroll
	// machinery (scrollbar, scrollRect fast path, clipping) drives the list.
	class VirtualListRenderer
	{
	public:
		static void init(int node);
		static void configureAttribute(int node, const char *name, const char *value);
		static int itemCount(int node);
		static int virtualContentHeight(int node, int rowHeight);
		// Last measured row height (first slot child's rendered CSS layout height),
		// cached by virtualContentHeight(). Surfaced to the app as the element's
		// `rowHeight` property so it positions slots from one source of truth.
		static int rowHeight(int node);
		static int scrollTop(int node);
		static int previousScrollTop(int node);
		static int scrollMaxY(int node);
		static bool setScrollTop(int node, int scrollTop);
		static void captureSnapshot(int node);
	};

} // namespace gea::embedded::ui
