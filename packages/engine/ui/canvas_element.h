// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <algorithm>
#include <cmath>
#include <type_traits>

#include "canvas.h"
#include <image.h>
#include "display.h"
#include "node.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace gea::framework::display_present { struct Frame; }

namespace gea::embedded::ui {

// Borrow numeric containers for the duration of one draw call. Typed arrays
// need no temporary vectors; only the retained triangle command owns a copy.
class CanvasNumericArrayView {
public:
	template <typename Container>
	CanvasNumericArrayView(const Container &values)
	    : source_(&values), size_(values.size()),
	      read_(+[](const void *source, std::size_t index, bool truncate) {
		      const auto &container = *static_cast<const Container *>(source);
		      using Element = std::remove_cvref_t<decltype(container[index])>;
		      static_assert(std::is_arithmetic_v<Element>,
		                    "Canvas arrays must contain numeric values");
		      if constexpr (std::is_floating_point_v<Element>) {
			      const auto value = container[index];
			      if (!std::isfinite(value))
				      return std::int64_t{0};
			      if constexpr (std::is_same_v<Element, float>) {
				      // Keep ordinary Float32 coordinates on the hardware float path.
				      if (value >= -2147483648.0f && value < 2147483520.0f)
					      return static_cast<std::int64_t>(static_cast<std::int32_t>(
					          truncate ? std::trunc(value) : std::round(value)));
			      }
			      return static_cast<std::int64_t>(
			          std::clamp(truncate ? std::trunc(static_cast<double>(value))
			                              : std::round(static_cast<double>(value)),
			                     -2147483648.0, 4294967295.0));
		      } else {
			      return static_cast<std::int64_t>(container[index]);
		      }
	      }) {}

	std::size_t size() const { return size_; }

	std::int64_t operator[](std::size_t index) const { return read_(source_, index, false); }

	// Integer order and packed RGBA values retain integer conversion semantics.
	std::int64_t truncated(std::size_t index) const { return read_(source_, index, true); }

private:
	const void *source_;
	std::size_t size_;
	std::int64_t (*read_)(const void *, std::size_t, bool);
};

struct CanvasPerfStats {
	std::int64_t canvasLookupUs = 0;
	std::int64_t markDirtyUs = 0;
	std::int64_t clearRectUs = 0;
	std::int64_t fillRectUs = 0;
	std::int64_t fillCircleUs = 0;
	std::int64_t fillTriangleUs = 0;
	std::int64_t drawImageUs = 0;
	std::int64_t fillTextUs = 0;
	std::int64_t batchFlushUs = 0;
	int canvasLookupCalls = 0;
	int markDirtyCalls = 0;
	int clearRectCalls = 0;
	int fillRectCalls = 0;
	int fillCircleCalls = 0;
	int fillTriangleCalls = 0;
	int drawImageCalls = 0;
	int fillTextCalls = 0;
	int batchBeginCalls = 0;
	int batchEndCalls = 0;
	int batchFlushCalls = 0;
};

enum class CanvasPresentCommandType : std::uint8_t {
	Clear,
	FillRectRgb565,
	StrokeRectRgb565,
	FillTriangleRgb565,
	FillCircleRgb565,
	StrokeCircleRgb565,
	FillCirclesRgb565,
	DrawImage,
	DrawImageScaled,
	DrawImageRotated90CW,
	DrawImageTiledX,
	FillText,
	FillTrianglesRgb565
};

struct CanvasPresentCommand {
	gea::framework::graphics::ImageReference imageReference;
	CanvasPresentCommandType type = CanvasPresentCommandType::Clear;
	gea::framework::graphics::pixel::native_t clearColor = 0;
	gea::framework::graphics::pixel::native_t color = 0;
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
	// Baked-font routing for FillText: a valid family id draws via the
	// rasterized Inter atlases; -1 falls back to the scaled bitmap font.
	int fontFamilyId = -1;
	int fontSizePx = 16;
	std::uint8_t alpha = 255;
	std::string text;
	std::vector<std::uint16_t> xs;
	std::vector<std::uint16_t> ys;
	std::vector<gea::framework::graphics::pixel::native_t> colors;
	// FillTrianglesRgb565: entries pre-packed in painter's (depth) order.
	std::vector<gea::framework::graphics::TriangleEntry> triangles;
};

void canvasPerfStatsReset();
CanvasPerfStats canvasPerfStatsRead();
void canvasTotalsRead(int *begin, int *end, int *fillRect, int *drawImage, int *nullPixels, int *presentOk);

// Everything a 2D context carries between calls: the open batch and its
// recorded commands, styles, the current path. One per canvas, shared by every
// CanvasRenderingContext2D for it -- `getContext('2d')` hands back the same
// object each time, so a context passed to a helper (a by-value copy in
// generated code) must draw into, and style, the same batch as the original.
struct CanvasContextState {
	std::uint32_t identity_ = 0;
	std::shared_ptr<gea::framework::display_present::Frame> retainedFrame_;
	std::shared_ptr<gea::framework::display_present::Frame> pendingRetainedFrame_;
	int retainedWidth_ = 0;
	int retainedHeight_ = 0;
	int retainedDamageX0_ = 0, retainedDamageY0_ = 0;
	int retainedDamageX1_ = -1, retainedDamageY1_ = -1;
	bool retainedBatchChanged_ = false;
	gea::framework::graphics::Canvas *batchCanvas_ = nullptr;
	std::vector<CanvasPresentCommand> presentCommands_;
	std::size_t presentCommandCount_ = 0;
	gea::framework::graphics::pixel::native_t fillStyle_ = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
	gea::framework::graphics::pixel::native_t strokeStyle_ = gea::framework::graphics::pixel::nativeColor(255, 255, 255);
	std::string fillStyleSource_;
	std::string strokeStyleSource_;
	double arcX_ = 0.0;
	double arcY_ = 0.0;
	double arcRadius_ = 0.0;
	std::vector<double> pathX_;
	std::vector<double> pathY_;
	// Start index in pathX_/pathY_ of each subpath (one per moveTo), so a path
	// can hold MULTIPLE rings/contours — required for concave polygons with
	// holes (even-odd fill) and multi-part strokes.
	std::vector<int> pathStarts_;
	double lineWidth_ = 1.0;
	float fontScale_ = 1.0f;
	int fontFamilyId_ = -1;
	int fontSizePx_ = 16;
	// Cache the last CSS font string so setFont can skip the parse + FontRegistry
	// lookups when an app re-sets the same font every frame (e.g. an FPS readout).
	std::string lastFontStr_;
	std::uint8_t globalAlpha_ = 255;
	int batchDepth_ = 0;
	bool fillStyleCached_ = false;
	bool strokeStyleCached_ = false;
	bool hasArc_ = false;
	bool pathClosed_ = false;
	bool presentRecording_ = false;
	bool batchDirty_ = false;
};

class CanvasRenderingContext2D {
public:
	// A non-explicit default ctor is required so this type can be a by-value
	// field of a geatsc-generated anonymous-record struct: those structs are
	// aggregates that the generated code value-initializes with `Record{}`,
	// which copy-`{}`-initializes each field — and copy-init cannot select an
	// `explicit` constructor. `explicit` is kept on the int form so a bare
	// `int` still can't implicitly convert to a context.
	CanvasRenderingContext2D() : nodeId_(-1), state_(stateFor(-1)) {}
	explicit CanvasRenderingContext2D(int nodeId) : nodeId_(nodeId), state_(stateFor(nodeId)) {}
	// The shared state of canvas `nodeId`; -1 is the direct-canvas display.
	static std::shared_ptr<CanvasContextState> stateFor(int nodeId);
	// Drops canvas `nodeId`'s state, so a node id reused later starts clean.
	static void releaseState(int nodeId);
	static void materializeRetainedCanvas(int nodeId);
	static bool replayRetainedCanvas(int nodeId, std::uint32_t identity, int x, int y, int width, int height);
	static bool hasRetainedCanvas(int nodeId);
	static bool retainedCanvasDamage(int nodeId, int &x0, int &y0, int &x1, int &y1);
	static void clearRetainedCanvasDamage();

	bool valid() const { return nodeId_ >= 0; }
	// The absence state `valid()` already answers, in the spelling a test uses.
	// A default-constructed context refers to no canvas (`nodeId_ == -1`) and
	// `getContext('2d')` on a node that is not a canvas hands one back, so
	// `if (!ctx) return` is the guard an app writes -- and every other gea
	// handle answers it this way (`gea::NativeHandle`, the Apple bridge's
	// wrappers, `gea::host::GeaEmbeddedImage`). Explicit, so it never
	// participates in arithmetic or overload resolution.
	explicit operator bool() const { return valid(); }
	void setFillStyle(const std::string &value);
	void setFillStyleRgb565(gea::framework::graphics::pixel::native_t color);
	void setStrokeStyle(const std::string &value);
	void setStrokeStyleRgb565(gea::framework::graphics::pixel::native_t color);
	void setGlobalAlpha(double alpha);
	void setLineWidth(double width);
	void setFont(const std::string &font);
	void setTextBaseline(const std::string &) {}
	void setTextAlign(const std::string &) {}
	void clear();
	void clearRect(int x, int y, int w, int h);
	void clearRect(double x, double y, double w, double h);
	void fillRect(int x, int y, int w, int h);
	void fillRect(double x, double y, double w, double h);
	void strokeRect(int x, int y, int w, int h);
	void strokeRect(double x, double y, double w, double h);
	void fillCircle(int x, int y, int radius);
	void fillCircle(double x, double y, double radius);
	void strokeCircle(int x, int y, int radius);
	void strokeCircle(double x, double y, double radius);
	void fillCircleRgb565(int x, int y, int radius, gea::framework::graphics::pixel::native_t color);
	void fillCircleRgb565(double x, double y, double radius, gea::framework::graphics::pixel::native_t color);
	void fillTriangleRgb565(int x0, int y0, int x1, int y1, int x2, int y2, gea::framework::graphics::pixel::native_t color);
	void fillTriangleRgb565(double x0, double y0, double x1, double y1, double x2, double y2, gea::framework::graphics::pixel::native_t color);
	void fillCirclesRgb565(const std::vector<std::int32_t> &xs,
	                       const std::vector<std::int32_t> &ys,
	                       int radius,
	                       const std::vector<gea::framework::graphics::pixel::native_t> &colors);
	// count>=0 batches only the first `count` triples (lets callers reuse a fixed-size
	// scratch array without a per-frame slice/copy); count<0 uses the full array length.
	// Pointer + count overloads. Callers whose coordinates live in a typed array
	// (the standard batching idiom: fixed-size scratch Uint16Array reused every
	// frame with a smaller live `count`) bound to the std::vector overloads only
	// through gea_cpp_typed_array's implicit operator std::vector<Element>(),
	// which heap-allocates and copies the FULL array per call and ignores
	// `count`. Measured on bubble-grid: ~150us per call independent of how many
	// circles it carried, ~2.3ms/frame across 15 calls. These take the elements
	// where they already are.
	void fillCirclesRgb565(const std::uint16_t *xs,
	                       const std::uint16_t *ys,
	                       int radius,
	                       const gea::framework::graphics::pixel::native_t *colors,
	                       int count);
	void fillCirclesRgb565(const std::uint16_t *xs,
	                       const std::uint16_t *ys,
	                       int radius,
	                       const gea::framework::graphics::pixel::NativeColor *colors,
	                       int count);
	void fillCirclesRgb565(const std::vector<std::uint16_t> &xs,
	                       const std::vector<std::uint16_t> &ys,
	                       int radius,
	                       const std::vector<gea::framework::graphics::pixel::native_t> &colors,
	                       int count = -1);
	void fillCirclesRgb565(const std::vector<std::uint16_t> &xs,
	                       const std::vector<std::uint16_t> &ys,
	                       int radius,
	                       const std::vector<gea::framework::graphics::pixel::NativeColor> &colors,
	                       int count = -1);
	void fillCirclesRgb565(const std::vector<std::int32_t> &xs,
	                       const std::vector<std::int32_t> &ys,
	                       int radius,
	                       gea::framework::graphics::pixel::native_t color);
	void fillCirclesRgb565(const std::vector<std::uint16_t> &xs,
	                       const std::vector<std::uint16_t> &ys,
	                       int radius,
	                       gea::framework::graphics::pixel::native_t color);
	// Batched triangles, one recorded command per call: `order[0..count)` gives
	// the paint (depth) order into the coordinate/colour arrays. colors hold
	// 0xRRGGBBAA app values (converted to native pixels during packing). This
	// replaces per-triangle fillTriangleRgb565 calls whose per-command record +
	// replay dispatch dominated at a few hundred triangles per frame.
	void fillTrianglesRgb565Sorted(const CanvasNumericArrayView &x0s,
	                               const CanvasNumericArrayView &y0s,
	                               const CanvasNumericArrayView &x1s,
	                               const CanvasNumericArrayView &y1s,
	                               const CanvasNumericArrayView &x2s,
	                               const CanvasNumericArrayView &y2s,
	                               const CanvasNumericArrayView &colors,
	                               const CanvasNumericArrayView &order,
	                               int count);
	void beginPath();
	void arc(double x, double y, double radius, double startAngle, double endAngle);
	void moveTo(double x, double y);
	void lineTo(double x, double y);
	void closePath();
	void fill();
	void stroke();
	void fillText(const std::string &text, int x, int y);
	void fillText(const std::string &text, double x, double y);
	void drawImage(int imageId, int dx, int dy);
	// drawImage at full opacity whatever globalAlpha is: putImageData.
	void putImage(int imageId, int dx, int dy);
	// Draws `height` rows of `width` opaque pixels, row r read from
	// `pixels + r * stride`, at (dx, dy). The rows must stay valid until the
	// frame is presented: they are recorded, not copied. This is a sub-rectangle
	// of a larger buffer -- putImageData's dirty rectangle -- without a copy.
	// Like putImageData, it ignores globalAlpha.
	void drawPixelRows(const gea::framework::graphics::pixel::native_t *pixels, int stride, int width, int height, int dx, int dy);
	void drawImage(int imageId, double dx, double dy);
	void drawImage(int imageId, int dx, int dy, int dw, int dh);
	void drawImage(int imageId, double dx, double dy, double dw, double dh);
	void drawImageRotated90CW(int imageId, int dx, int dy, int dw, int dh);
	void drawImageRotated90CW(int imageId, double dx, double dy, double dw, double dh);
	// Draw an image masked to a circle inscribed in the destination box. Lets an
	// OPAQUE sprite (a JPEG, which has no alpha channel) render as a round tile
	// without carrying an alpha plane: the blit simply skips the pixels outside
	// the shape. Square opaque tiles would otherwise paint their corners over
	// whatever they overlap.
	// Width in pixels of `text` in the context's CURRENT font, so callers can
	// centre or right-align. textAlign is a no-op on this canvas and there was
	// no measureText, which left callers hand-tuning a width factor per string.
	double measureText(const std::string &text);
	// Offset from fillText's y anchor to the vertical centre of the text's ink.
	double measureTextInkCenter(const std::string &text);
	void drawImageCircle(int imageId, int dx, int dy, int dw, int dh);
	void drawImageCircle(int imageId, double dx, double dy, double dw, double dh);
	void drawImageTiledX(int imageId, int dx, int dy, int width);
	void drawImageTiledX(int imageId, double dx, double dy, double width);
	void flush();
	void beginBatch();
	void endBatch();

private:
	gea::framework::graphics::Canvas *canvas();
	gea::framework::graphics::Canvas *drawingCanvas();
	bool recordingPresentBatch() const;
	void prepareBitmapOnlyDraw();
	bool retainNumericBatch();
	std::vector<gea::platform::display::DisplayPresentCommand> displayPresentCommands() const;
	void appendPresentClear(gea::framework::graphics::pixel::native_t color);
	void appendPresentFillRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color);
	void appendPresentStrokeRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color);
	void appendPresentFillTriangle(int x0,
	                               int y0,
	                               int x1,
	                               int y1,
	                               int x2,
	                               int y2,
	                               gea::framework::graphics::pixel::native_t color);
	void appendPresentFillCircle(int x, int y, int radius, gea::framework::graphics::pixel::native_t color);
	void appendPresentStrokeCircle(int x, int y, int radius, gea::framework::graphics::pixel::native_t color);
	void appendPresentCircles(const std::uint16_t *xs,
	                          const std::uint16_t *ys,
	                          int radius,
	                          const gea::framework::graphics::pixel::native_t *colors,
	                          int count);
	void appendPresentCircles(const std::uint16_t *xs,
	                          const std::uint16_t *ys,
	                          int radius,
	                          const gea::framework::graphics::pixel::NativeColor *colors,
	                          int count);
	void appendPresentCircles(const std::vector<std::uint16_t> &xs,
	                          const std::vector<std::uint16_t> &ys,
	                          int radius,
	                          const std::vector<gea::framework::graphics::pixel::native_t> &colors,
	                          int count = -1);
	void appendPresentCircles(const std::vector<std::uint16_t> &xs,
	                          const std::vector<std::uint16_t> &ys,
	                          int radius,
	                          const std::vector<gea::framework::graphics::pixel::NativeColor> &colors,
	                          int count = -1);
	void appendPresentCirclesUniform(const std::vector<std::uint16_t> &xs,
	                                 const std::vector<std::uint16_t> &ys,
	                                 int radius,
	                                 gea::framework::graphics::pixel::native_t color);
	void appendPresentTriangles(const CanvasNumericArrayView &x0s,
	                            const CanvasNumericArrayView &y0s,
	                            const CanvasNumericArrayView &x1s,
	                            const CanvasNumericArrayView &y1s,
	                            const CanvasNumericArrayView &x2s,
	                            const CanvasNumericArrayView &y2s,
	                            const CanvasNumericArrayView &colors,
	                            const CanvasNumericArrayView &order,
	                            int count);
	void appendPresentDrawImage(const gea::framework::graphics::pixel::native_t *pixels,
	                            const std::uint8_t *alphaPixels,
	                            int srcWidth,
	                            int srcHeight,
	                            int x,
	                            int y);
	void appendPresentDrawImageScaled(const gea::framework::graphics::pixel::native_t *pixels,
	                                  const std::uint8_t *alphaPixels,
	                                  int srcWidth,
	                                  int srcHeight,
	                                  int x,
	                                  int y,
	                                  int w,
	                                  int h,
	                                  int radius = 0);
	void appendPresentDrawImageRotated90CW(const gea::framework::graphics::pixel::native_t *pixels,
	                                       const std::uint8_t *alphaPixels,
	                                       int srcWidth,
	                                       int srcHeight,
	                                       int x,
	                                       int y,
	                                       int w,
	                                       int h);
		void appendPresentDrawImageTiledX(const gea::framework::graphics::pixel::native_t *pixels,
		                                  const std::uint8_t *alphaPixels,
		                                  int srcWidth,
		                                  int srcHeight,
		                                  int x,
		                                  int y,
		                                  int w);
		void appendPresentFillText(const std::string &text, int x, int y, gea::framework::graphics::pixel::native_t color);
		CanvasPresentCommand &appendPresentCommand(CanvasPresentCommandType type);
		void resetPresentCommands();
		void replayPresentBatchToCanvas(gea::framework::graphics::Canvas &surface);
		bool presentBatch();
		void applyDrawState(gea::framework::graphics::Canvas &surface) const;
	void markDirty();
	void markDrawDirty();

		int nodeId_;
		std::shared_ptr<CanvasContextState> state_;
};

class CanvasElement : public NodeHandle {
public:
	using NodeHandle::NodeHandle;
	static CanvasElement create();
	CanvasRenderingContext2D getContext2D() const;
};

}  // namespace gea::embedded::ui
