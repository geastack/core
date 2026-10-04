// SPDX-License-Identifier: Apache-2.0
#include "canvas_element.h"

#include "display.h"
#include "internal.h"
#include "pixel.h"
#include "tree_internal.h"

#include <graphics/font.h>
#include <image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gea_perf_config.h"  // GEA_EMBEDDED_CANVAS_PERF_DETAIL (+ GEA_EMBEDDED_PERF master)

namespace gea::embedded::ui {

namespace {

// One state per canvas node: every context for a node is a handle onto it.
// Contexts are made and used on the frame task only.
std::unordered_map<int, std::shared_ptr<CanvasContextState>> &contextStates()
{
	static std::unordered_map<int, std::shared_ptr<CanvasContextState>> states;
	return states;
}

}  // namespace

std::shared_ptr<CanvasContextState> CanvasRenderingContext2D::stateFor(int nodeId)
{
	// -1 is the direct-canvas display context (and an unattached default one).
	auto &slot = contextStates()[nodeId < 0 ? -1 : nodeId];
	if (!slot) slot = std::make_shared<CanvasContextState>();
	return slot;
}

void CanvasRenderingContext2D::releaseState(int nodeId)
{
	contextStates().erase(nodeId);
}

namespace {

CanvasPerfStats gCanvasPerfStats;

// NEVER-RESET totals (the window stats above reset per perf interval).
int gTotalBeginBatch = 0;
int gTotalEndBatch = 0;
int gTotalFillRect = 0;
int gTotalDrawImage = 0;
int gTotalDrawImageNullPixels = 0;
int gTotalPresentBatchOk = 0;

// Canvas-2D imperative colours arrive already in this board's native pixel
// (geatsc folds literals via nativeFromRgba8888 at build time; runtime values go
// through geaCanvasValueNative / parseCanvasColor). So these are identity
// pass-throughs now, kept as named seams for the geatsc-emitted call sites.
gea::framework::graphics::pixel::native_t canvasRgb565(gea::framework::graphics::pixel::native_t color)
{
	return color;
}

std::int64_t nowUs()
{
#if GEA_EMBEDDED_CANVAS_PERF_DETAIL
	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
#else
	return 0;
#endif
}

int clampColorChannel(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return value;
}

gea::framework::graphics::pixel::native_t parseCanvasColor(const std::string &value)
{
	const char *cursor = value.c_str();
	auto skipSpace = [&]() {
		while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') cursor++;
	};
	auto readCssNumber = [&](int &out) -> bool {
		skipSpace();
		char *end = nullptr;
		long parsed = std::strtol(cursor, &end, 10);
		if (end == cursor) return false;
		cursor = end;
		if (*cursor == '.') {
			cursor++;
			while (*cursor >= '0' && *cursor <= '9') cursor++;
		}
		skipSpace();
		out = static_cast<int>(parsed);
		return true;
	};
	auto consume = [&](char expected) -> bool {
		skipSpace();
		if (*cursor != expected) return false;
		cursor++;
		return true;
	};

	int r = 0, g = 0, b = 0;
	if (value.rfind("rgb(", 0) == 0) {
		cursor += 4;
		if (readCssNumber(r) && consume(',') && readCssNumber(g) && consume(',') &&
		    readCssNumber(b) && consume(')')) {
			return gea::framework::graphics::pixel::nativeColor(
			    clampColorChannel(r),
			    clampColorChannel(g),
			    clampColorChannel(b));
		}
	}
	if (std::sscanf(value.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3 ||
	    std::sscanf(value.c_str(), "rgb(%d, %d, %d)", &r, &g, &b) == 3) {
		return gea::framework::graphics::pixel::nativeColor(
		    clampColorChannel(r),
		    clampColorChannel(g),
		    clampColorChannel(b));
	}
	if (value.size() == 7 && value[0] == '#') {
		char *end = nullptr;
		const long rgb = std::strtol(value.c_str() + 1, &end, 16);
		if (end && *end == '\0') {
			return gea::framework::graphics::pixel::nativeColor(
			    static_cast<int>((rgb >> 16) & 0xff),
			    static_cast<int>((rgb >> 8) & 0xff),
			    static_cast<int>(rgb & 0xff));
		}
	}
	return gea::framework::graphics::pixel::nativeColor(255, 255, 255);
}

int rounded(double value)
{
	if (!std::isfinite(value)) return 0;
	return static_cast<int>(value + (value >= 0 ? 0.5 : -0.5));
}

std::uint8_t alphaFromUnit(double value)
{
	if (!std::isfinite(value)) return 255;
	if (value < 0.0) value = 0.0;
	if (value > 1.0) value = 1.0;
	return static_cast<std::uint8_t>(value * 255.0 + 0.5);
}

// Pixel size from a CSS font shorthand ("16px Inter") — 0 when absent.
int fontPxFromCss(const std::string &font)
{
	const char *cursor = font.c_str();
	while (*cursor) {
		char *end = nullptr;
		const double parsed = std::strtod(cursor, &end);
		if (end != cursor && parsed > 0.0) {
			while (*end == ' ' || *end == '\t') end++;
			if (end[0] == 'p' && end[1] == 'x') return static_cast<int>(parsed + 0.5);
			cursor = end;
			continue;
		}
		cursor++;
	}
	return 0;
}

// First family name after the size in a CSS font shorthand; empty when none.
std::string fontFamilyFromCss(const std::string &font)
{
	const std::size_t px = font.find("px");
	std::size_t start = px == std::string::npos ? 0 : px + 2;
	while (start < font.size() && (font[start] == ' ' || font[start] == '"' || font[start] == '\'')) start++;
	std::size_t end = start;
	while (end < font.size() && font[end] != ',' && font[end] != '"' && font[end] != '\'') end++;
	while (end > start && font[end - 1] == ' ') end--;
	return font.substr(start, end - start);
}

float fontScaleFromCss(const std::string &font)
{
	const char *cursor = font.c_str();
	while (*cursor) {
		char *end = nullptr;
		const double parsed = std::strtod(cursor, &end);
		if (end != cursor && parsed > 0.0) {
			while (*end == ' ' || *end == '\t') end++;
			if (end[0] == 'p' && end[1] == 'x') return static_cast<float>(parsed / 16.0);
			cursor = end;
			continue;
		}
		cursor++;
	}
	return 1.0f;
}

}  // namespace

void canvasPerfStatsReset()
{
	gCanvasPerfStats = {};
}

CanvasPerfStats canvasPerfStatsRead()
{
#if GEA_EMBEDDED_CANVAS_PERF_DETAIL
	return gCanvasPerfStats;
#else
	// Perf off: nothing observes gCanvasPerfStats, so the per-op `…Calls++` writes
	// in the hot draw path (fillCircle ×1000/frame, etc.) become dead stores to a
	// never-read, internal-linkage static and are dead-store-eliminated — no
	// counter machinery survives. (The timing reads were already compiled out:
	// the file-local nowUs() returns 0 when GEA_EMBEDDED_CANVAS_PERF_DETAIL is 0.)
	return {};
#endif
}

void canvasTotalsRead(int *begin, int *end, int *fillRect, int *drawImage, int *nullPixels, int *presentOk)
{
	*begin = gTotalBeginBatch;
	*end = gTotalEndBatch;
	*fillRect = gTotalFillRect;
	*drawImage = gTotalDrawImage;
	*nullPixels = gTotalDrawImageNullPixels;
	*presentOk = gTotalPresentBatchOk;
}

CanvasPresentCommand &CanvasRenderingContext2D::appendPresentCommand(CanvasPresentCommandType type)
{
	if (state_->presentCommandCount_ < state_->presentCommands_.size()) {
		CanvasPresentCommand &command = state_->presentCommands_[state_->presentCommandCount_++];
		command.type = type;
		return command;
	}
	state_->presentCommands_.emplace_back();
	state_->presentCommandCount_ = state_->presentCommands_.size();
	CanvasPresentCommand &command = state_->presentCommands_.back();
	command.type = type;
	return command;
}

void CanvasRenderingContext2D::resetPresentCommands()
{
	state_->presentCommandCount_ = 0;
}

#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT

CanvasElement CanvasElement::create()
{
	return CanvasElement(-1);
}

CanvasRenderingContext2D CanvasElement::getContext2D() const
{
	return CanvasRenderingContext2D(-1);
}

gea::framework::graphics::Canvas *CanvasRenderingContext2D::canvas()
{
	return gea::platform::display::Display::canvas();
}

gea::framework::graphics::Canvas *CanvasRenderingContext2D::drawingCanvas()
{
	return canvas();
}

bool CanvasRenderingContext2D::recordingPresentBatch() const
{
	return state_->presentRecording_ && state_->batchDepth_ > 0;
}

void CanvasRenderingContext2D::appendPresentClear(gea::framework::graphics::pixel::native_t color)
{
	resetPresentCommands();
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::Clear);
	command.clearColor = color;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color)
{
	if (w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillRectRgb565);
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentStrokeRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color)
{
	if (w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::StrokeRectRgb565);
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillTriangle(int x0,
                                                         int y0,
                                                         int x1,
                                                         int y1,
                                                         int x2,
                                                         int y2,
                                                         gea::framework::graphics::pixel::native_t color)
{
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillTriangleRgb565);
	command.x0 = x0;
	command.y0 = y0;
	command.x1 = x1;
	command.y1 = y1;
	command.x2 = x2;
	command.y2 = y2;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillCircle(int x, int y, int radius, gea::framework::graphics::pixel::native_t color)
{
	if (radius <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCircleRgb565);
	command.x = x;
	command.y = y;
	command.radius = radius;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentStrokeCircle(int x, int y, int radius, gea::framework::graphics::pixel::native_t color)
{
	if (radius <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::StrokeCircleRgb565);
	command.x = x;
	command.y = y;
	command.radius = radius;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

// Pointer + count forms: no temporary vector, and only the live `count`
// elements are copied into the recorded command (which reuses its capacity
// across frames, so this is a memcpy into an already-sized buffer).
void CanvasRenderingContext2D::appendPresentCircles(const std::uint16_t *xs,
                                                    const std::uint16_t *ys,
                                                    int radius,
                                                    const gea::framework::graphics::pixel::native_t *colors,
                                                    int count)
{
	if (radius <= 0 || count <= 0 || !xs || !ys || !colors) return;
	const std::size_t limit = static_cast<std::size_t>(count);
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	command.xs.assign(xs, xs + limit);
	command.ys.assign(ys, ys + limit);
	command.colors.assign(colors, colors + limit);
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCircles(const std::uint16_t *xs,
                                                    const std::uint16_t *ys,
                                                    int radius,
                                                    const gea::framework::graphics::pixel::NativeColor *colors,
                                                    int count)
{
	if (radius <= 0 || count <= 0 || !xs || !ys || !colors) return;
	const std::size_t limit = static_cast<std::size_t>(count);
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	command.xs.assign(xs, xs + limit);
	command.ys.assign(ys, ys + limit);
	command.colors.resize(limit);
	for (std::size_t i = 0; i < limit; ++i) command.colors[i] = colors[i].value;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCircles(const std::vector<std::uint16_t> &xs,
                                                    const std::vector<std::uint16_t> &ys,
                                                    int radius,
                                                    const std::vector<gea::framework::graphics::pixel::native_t> &colors,
                                                    int count)
{
	if (radius <= 0) return;
	std::size_t limit = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < limit) limit = colors.size();
	if (count >= 0 && static_cast<std::size_t>(count) < limit) limit = static_cast<std::size_t>(count);
	if (limit == 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	command.xs.assign(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(limit));
	command.ys.assign(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(limit));
	command.colors.assign(colors.begin(), colors.begin() + static_cast<std::ptrdiff_t>(limit));
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCircles(const std::vector<std::uint16_t> &xs,
                                                    const std::vector<std::uint16_t> &ys,
                                                    int radius,
                                                    const std::vector<gea::framework::graphics::pixel::NativeColor> &colors,
                                                    int count)
{
	if (radius <= 0) return;
	std::size_t limit = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < limit) limit = colors.size();
	if (count >= 0 && static_cast<std::size_t>(count) < limit) limit = static_cast<std::size_t>(count);
	if (limit == 0) return;

	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.xs.assign(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(limit));
	command.ys.assign(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(limit));
	command.colors.resize(limit);
	for (std::size_t i = 0; i < limit; ++i) command.colors[i] = colors[i].value;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCirclesUniform(const std::vector<std::uint16_t> &xs,
                                                           const std::vector<std::uint16_t> &ys,
                                                           int radius,
                                                           gea::framework::graphics::pixel::native_t color)
{
	const int capped = static_cast<int>(std::min(xs.size(), ys.size()));
	if (capped <= 0 || radius <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	command.xs.assign(xs.begin(), xs.begin() + capped);
	command.ys.assign(ys.begin(), ys.begin() + capped);
	command.colors.assign(static_cast<std::size_t>(capped), canvasRgb565(color));
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentTriangles(const std::vector<std::int32_t> &x0s,
                                                      const std::vector<std::int32_t> &y0s,
                                                      const std::vector<std::int32_t> &x1s,
                                                      const std::vector<std::int32_t> &y1s,
                                                      const std::vector<std::int32_t> &x2s,
                                                      const std::vector<std::int32_t> &y2s,
                                                      const std::vector<std::uint32_t> &colors,
                                                      const std::vector<std::int32_t> &order,
                                                      int count)
{
	std::size_t limit = std::min({x0s.size(), y0s.size(), x1s.size(), y1s.size(), x2s.size(), y2s.size(), colors.size(), order.size()});
	if (count >= 0) limit = std::min(limit, static_cast<std::size_t>(count));
	if (limit == 0) return;
	using gea::framework::graphics::TriangleEntry;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillTrianglesRgb565);
	command.alpha = state_->globalAlpha_;
	command.triangles.resize(limit);
	const std::int32_t *px0 = x0s.data();
	const std::int32_t *py0 = y0s.data();
	const std::int32_t *px1 = x1s.data();
	const std::int32_t *py1 = y1s.data();
	const std::int32_t *px2 = x2s.data();
	const std::int32_t *py2 = y2s.data();
	const std::uint32_t *pcolors = colors.data();
	const std::int32_t *porder = order.data();
	TriangleEntry *dst = command.triangles.data();
	const std::size_t sourceSize = x0s.size();
	for (std::size_t i = 0; i < limit; i++) {
		const std::int32_t rawIndex = porder[i];
		const std::size_t o = rawIndex >= 0 && static_cast<std::size_t>(rawIndex) < sourceSize
			? static_cast<std::size_t>(rawIndex)
			: 0;
		TriangleEntry &t = dst[i];
		t.x0 = static_cast<std::int16_t>(px0[o]);
		t.y0 = static_cast<std::int16_t>(py0[o]);
		t.x1 = static_cast<std::int16_t>(px1[o]);
		t.y1 = static_cast<std::int16_t>(py1[o]);
		t.x2 = static_cast<std::int16_t>(px2[o]);
		t.y2 = static_cast<std::int16_t>(py2[o]);
		t.color = gea::framework::graphics::pixel::nativeFromRrggbbaa(pcolors[o]);
		const std::int16_t lo01 = t.y0 < t.y1 ? t.y0 : t.y1;
		const std::int16_t hi01 = t.y0 > t.y1 ? t.y0 : t.y1;
		t.rowY0 = lo01 < t.y2 ? lo01 : t.y2;
		t.rowY1 = hi01 > t.y2 ? hi01 : t.y2;
	}
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentDrawImage(const gea::framework::graphics::pixel::native_t *pixels,
                                                      const std::uint8_t *alphaPixels,
                                                      int srcWidth,
                                                      int srcHeight,
                                                      int x,
                                                      int y)
{
	if (!pixels || srcWidth <= 0 || srcHeight <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::DrawImage);
	command.pixels = pixels;
	command.alphaPixels = alphaPixels;
	command.srcWidth = srcWidth;
	command.srcHeight = srcHeight;
	command.x = x;
	command.y = y;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentDrawImageScaled(const gea::framework::graphics::pixel::native_t *pixels,
                                                            const std::uint8_t *alphaPixels,
                                                            int srcWidth,
                                                            int srcHeight,
                                                            int x,
                                                            int y,
                                                            int w,
                                                            int h,
                                                            int radius)
{
	if (!pixels || srcWidth <= 0 || srcHeight <= 0 || w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::DrawImageScaled);
	command.pixels = pixels;
	command.alphaPixels = alphaPixels;
	command.srcWidth = srcWidth;
	command.srcHeight = srcHeight;
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}
void CanvasRenderingContext2D::appendPresentDrawImageRotated90CW(const gea::framework::graphics::pixel::native_t *, const std::uint8_t *, int, int, int, int, int, int) {}
void CanvasRenderingContext2D::appendPresentDrawImageTiledX(const gea::framework::graphics::pixel::native_t *, const std::uint8_t *, int, int, int, int, int) {}
void CanvasRenderingContext2D::appendPresentFillText(const std::string &, int, int, gea::framework::graphics::pixel::native_t) {}
void CanvasRenderingContext2D::replayPresentBatchToCanvas(gea::framework::graphics::Canvas &) {}

bool CanvasRenderingContext2D::presentBatch()
{
	if (state_->presentCommandCount_ == 0) return false;
	gTotalPresentBatchOk++;
	std::vector<gea::platform::display::DisplayPresentCommand> commands;
	commands.reserve(state_->presentCommandCount_);
	for (std::size_t commandIndex = 0; commandIndex < state_->presentCommandCount_; ++commandIndex) {
		const CanvasPresentCommand &command = state_->presentCommands_[commandIndex];
		gea::platform::display::DisplayPresentCommand displayCommand{};
		switch (command.type) {
		case CanvasPresentCommandType::Clear:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::Clear;
			displayCommand.clear.color = command.clearColor;
			break;
		case CanvasPresentCommandType::FillRectRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillRectRgb565;
			displayCommand.fillRectRgb565 = {command.x, command.y, command.w, command.h, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::StrokeRectRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::StrokeRectRgb565;
			displayCommand.strokeRectRgb565 = {command.x, command.y, command.w, command.h, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::FillTriangleRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillTriangleRgb565;
			displayCommand.fillTriangleRgb565 = {command.x0, command.y0, command.x1, command.y1, command.x2, command.y2, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::FillCircleRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillCircleRgb565;
			displayCommand.fillCircleRgb565 = {command.x, command.y, command.radius, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::StrokeCircleRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::StrokeCircleRgb565;
			displayCommand.strokeCircleRgb565 = {command.x, command.y, command.radius, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::FillCirclesRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillCirclesRgb565;
			displayCommand.fillCirclesRgb565.xs = command.xs.data();
			displayCommand.fillCirclesRgb565.ys = command.ys.data();
			displayCommand.fillCirclesRgb565.colors = command.colors.data();
			displayCommand.fillCirclesRgb565.count = static_cast<int>(command.xs.size());
			displayCommand.fillCirclesRgb565.radius = command.radius;
			displayCommand.fillCirclesRgb565.alpha = command.alpha;
			break;
		case CanvasPresentCommandType::FillTrianglesRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillTrianglesRgb565;
			displayCommand.fillTrianglesRgb565.entries = command.triangles.data();
			displayCommand.fillTrianglesRgb565.count = static_cast<int>(command.triangles.size());
			displayCommand.fillTrianglesRgb565.alpha = command.alpha;
			break;
		case CanvasPresentCommandType::DrawImage:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::DrawImage;
			displayCommand.drawImage = {command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                            command.x,      command.y,          command.alpha};
			break;
		case CanvasPresentCommandType::DrawImageScaled:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::DrawImageScaled;
			displayCommand.drawImageScaled = {command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                                  command.x,      command.y,           command.w,        command.h,
			                                  command.alpha,  command.radius};
			break;
		default:
			continue;
		}
		commands.push_back(displayCommand);
	}
	return gea::platform::display::Display::present(commands.data(), static_cast<int>(commands.size()));
}

void CanvasRenderingContext2D::applyDrawState(gea::framework::graphics::Canvas &surface) const
{
	surface.setGlobalAlpha(state_->globalAlpha_);
}

void CanvasRenderingContext2D::markDirty()
{
	if (state_->batchDepth_ > 0) state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::markDrawDirty()
{
	markDirty();
}

void CanvasRenderingContext2D::setFillStyle(const std::string &value)
{
	if (state_->fillStyleCached_ && value == state_->fillStyleSource_) return;
	state_->fillStyle_ = parseCanvasColor(value);
	state_->fillStyleSource_ = value;
	state_->fillStyleCached_ = true;
}

void CanvasRenderingContext2D::setFillStyleRgb565(gea::framework::graphics::pixel::native_t color)
{
	state_->fillStyle_ = canvasRgb565(color);
	state_->fillStyleCached_ = false;
}

void CanvasRenderingContext2D::setStrokeStyle(const std::string &value)
{
	if (state_->strokeStyleCached_ && value == state_->strokeStyleSource_) return;
	state_->strokeStyle_ = parseCanvasColor(value);
	state_->strokeStyleSource_ = value;
	state_->strokeStyleCached_ = true;
}

void CanvasRenderingContext2D::setStrokeStyleRgb565(gea::framework::graphics::pixel::native_t color)
{
	state_->strokeStyle_ = canvasRgb565(color);
	state_->strokeStyleCached_ = false;
}

void CanvasRenderingContext2D::setGlobalAlpha(double alpha) { state_->globalAlpha_ = alphaFromUnit(alpha); }
void CanvasRenderingContext2D::setLineWidth(double width) { if (std::isfinite(width) && width > 0.0) state_->lineWidth_ = width; }
void CanvasRenderingContext2D::setFont(const std::string &font) { state_->lastFontStr_ = font; state_->fontScale_ = fontScaleFromCss(font); state_->fontSizePx_ = fontPxFromCss(font); }

void CanvasRenderingContext2D::clear()
{
	clearRect(0, 0, gea::platform::display::kWidth, gea::platform::display::kHeight);
}

void CanvasRenderingContext2D::clearRect(int x, int y, int w, int h)
{
	const std::uint8_t previousAlpha = state_->globalAlpha_;
	state_->globalAlpha_ = 255;
	fillRect(x, y, w, h);
	state_->globalAlpha_ = previousAlpha;
}

void CanvasRenderingContext2D::clearRect(double x, double y, double w, double h) { clearRect(rounded(x), rounded(y), rounded(w), rounded(h)); }

void CanvasRenderingContext2D::fillRect(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	gTotalFillRect++;
	if (recordingPresentBatch()) {
		appendPresentFillRect(x, y, w, h, state_->fillStyle_);
		return;
	}
	gea::platform::display::Display::fillRect(x, y, w, h, state_->fillStyle_);
}

void CanvasRenderingContext2D::fillRect(double x, double y, double w, double h) { fillRect(rounded(x), rounded(y), rounded(w), rounded(h)); }

void CanvasRenderingContext2D::strokeRect(int x, int y, int w, int h)
{
	if (recordingPresentBatch()) {
		appendPresentStrokeRect(x, y, w, h, state_->strokeStyle_);
		return;
	}
	gea::platform::display::Display::strokeRect(x, y, w, h, state_->strokeStyle_);
}

void CanvasRenderingContext2D::strokeRect(double x, double y, double w, double h) { strokeRect(rounded(x), rounded(y), rounded(w), rounded(h)); }

void CanvasRenderingContext2D::fillCircle(int x, int y, int radius) { fillCircleRgb565(x, y, radius, state_->fillStyle_); }
void CanvasRenderingContext2D::fillCircle(double x, double y, double radius) { fillCircle(rounded(x), rounded(y), rounded(radius)); }

void CanvasRenderingContext2D::strokeCircle(int x, int y, int radius)
{
	if (recordingPresentBatch()) {
		appendPresentStrokeCircle(x, y, radius, state_->strokeStyle_);
		return;
	}
	gea::platform::display::Display::strokeCircle(x, y, radius, state_->strokeStyle_);
}

void CanvasRenderingContext2D::strokeCircle(double x, double y, double radius) { strokeCircle(rounded(x), rounded(y), rounded(radius)); }

void CanvasRenderingContext2D::fillCircleRgb565(int x, int y, int radius, gea::framework::graphics::pixel::native_t color)
{
	if (recordingPresentBatch()) {
		appendPresentFillCircle(x, y, radius, canvasRgb565(color));
		return;
	}
	gea::platform::display::Display::fillCircle(x, y, radius, canvasRgb565(color));
}

void CanvasRenderingContext2D::fillCircleRgb565(double x, double y, double radius, gea::framework::graphics::pixel::native_t color)
{
	fillCircleRgb565(rounded(x), rounded(y), rounded(radius), color);
}

void CanvasRenderingContext2D::fillTriangleRgb565(int x0, int y0, int x1, int y1, int x2, int y2, gea::framework::graphics::pixel::native_t color)
{
	if (recordingPresentBatch()) {
		appendPresentFillTriangle(x0, y0, x1, y1, x2, y2, canvasRgb565(color));
		return;
	}
	gea::platform::display::Display::fillTriangle(x0, y0, x1, y1, x2, y2, canvasRgb565(color));
}

void CanvasRenderingContext2D::fillTriangleRgb565(double x0, double y0, double x1, double y1, double x2, double y2, gea::framework::graphics::pixel::native_t color)
{
	fillTriangleRgb565(rounded(x0), rounded(y0), rounded(x1), rounded(y1), rounded(x2), rounded(y2), color);
}

void CanvasRenderingContext2D::fillTrianglesRgb565Sorted(const std::vector<std::int32_t> &x0s,
                                                         const std::vector<std::int32_t> &y0s,
                                                         const std::vector<std::int32_t> &x1s,
                                                         const std::vector<std::int32_t> &y1s,
                                                         const std::vector<std::int32_t> &x2s,
                                                         const std::vector<std::int32_t> &y2s,
                                                         const std::vector<std::uint32_t> &colors,
                                                         const std::vector<std::int32_t> &order,
                                                         int count)
{
	if (recordingPresentBatch()) appendPresentTriangles(x0s, y0s, x1s, y1s, x2s, y2s, colors, order, count);
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::uint16_t *xs, const std::uint16_t *ys, int radius, const gea::framework::graphics::pixel::native_t *colors, int count) { if (recordingPresentBatch()) appendPresentCircles(xs, ys, radius, colors, count); }
void CanvasRenderingContext2D::fillCirclesRgb565(const std::uint16_t *xs, const std::uint16_t *ys, int radius, const gea::framework::graphics::pixel::NativeColor *colors, int count) { if (recordingPresentBatch()) appendPresentCircles(xs, ys, radius, colors, count); }
void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::int32_t> &, const std::vector<std::int32_t> &, int, const std::vector<gea::framework::graphics::pixel::native_t> &) {}
void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::uint16_t> &xs, const std::vector<std::uint16_t> &ys, int radius, const std::vector<gea::framework::graphics::pixel::native_t> &colors, int count) { if (recordingPresentBatch()) appendPresentCircles(xs, ys, radius, colors, count); }
void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::uint16_t> &xs, const std::vector<std::uint16_t> &ys, int radius, const std::vector<gea::framework::graphics::pixel::NativeColor> &colors, int count) { if (recordingPresentBatch()) appendPresentCircles(xs, ys, radius, colors, count); }
void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::int32_t> &, const std::vector<std::int32_t> &, int, gea::framework::graphics::pixel::native_t) {}
void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::uint16_t> &xs, const std::vector<std::uint16_t> &ys, int radius, gea::framework::graphics::pixel::native_t color) { if (recordingPresentBatch()) appendPresentCirclesUniform(xs, ys, radius, color); }

void CanvasRenderingContext2D::beginPath() { state_->pathX_.clear(); state_->pathY_.clear(); state_->pathStarts_.clear(); state_->hasArc_ = false; state_->pathClosed_ = false; }
void CanvasRenderingContext2D::arc(double x, double y, double radius, double, double) { state_->arcX_ = x; state_->arcY_ = y; state_->arcRadius_ = radius; state_->hasArc_ = true; }
void CanvasRenderingContext2D::moveTo(double x, double y) { state_->pathStarts_.push_back(static_cast<int>(state_->pathX_.size())); state_->pathX_.push_back(x); state_->pathY_.push_back(y); }
void CanvasRenderingContext2D::lineTo(double x, double y) { state_->pathX_.push_back(x); state_->pathY_.push_back(y); }
void CanvasRenderingContext2D::closePath() { state_->pathClosed_ = true; }
void CanvasRenderingContext2D::fill() { if (state_->hasArc_) fillCircle(state_->arcX_, state_->arcY_, state_->arcRadius_); }
void CanvasRenderingContext2D::stroke() { if (state_->hasArc_) strokeCircle(state_->arcX_, state_->arcY_, state_->arcRadius_); }
void CanvasRenderingContext2D::fillText(const std::string &, int, int) {}
void CanvasRenderingContext2D::fillText(const std::string &text, double x, double y) { fillText(text, rounded(x), rounded(y)); }
// Images on the direct path record a present command like every other
// primitive here. These used to be stubs, so ctx.drawImage() silently drew
// NOTHING on a direct-canvas board -- the call was counted and then dropped,
// which reads as a black screen rather than as an error. The display layer has
// always had DrawImage/DrawImageScaled commands; only this side was missing.
void CanvasRenderingContext2D::drawImage(int imageId, int dx, int dy)
{
	gTotalDrawImage++;
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0) { gTotalDrawImageNullPixels++; return; }
	if (!recordingPresentBatch()) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	appendPresentDrawImage(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
}
void CanvasRenderingContext2D::drawImage(int imageId, double x, double y) { drawImage(imageId, rounded(x), rounded(y)); }
void CanvasRenderingContext2D::drawPixelRows(const gea::framework::graphics::pixel::native_t *pixels, int stride, int width, int height, int dx, int dy)
{
	if (!pixels || width <= 0 || height <= 0 || stride < width) return;
	if (!recordingPresentBatch()) return;
	const std::uint8_t previousAlpha = state_->globalAlpha_;
	state_->globalAlpha_ = 255;
	for (int row = 0; row < height; ++row)
		appendPresentDrawImage(pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride), nullptr, width, 1, dx, dy + row);
	state_->globalAlpha_ = previousAlpha;
}
void CanvasRenderingContext2D::putImage(int imageId, int dx, int dy)
{
	// putImageData replaces the destination pixels: globalAlpha does not apply.
	const std::uint8_t previousAlpha = state_->globalAlpha_;
	state_->globalAlpha_ = 255;
	drawImage(imageId, dx, dy);
	state_->globalAlpha_ = previousAlpha;
}
void CanvasRenderingContext2D::drawImage(int imageId, int dx, int dy, int dw, int dh)
{
	gTotalDrawImage++;
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0) { gTotalDrawImageNullPixels++; return; }
	if (!recordingPresentBatch()) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	// A 1:1 destination takes the unscaled command, whose executor is a row
	// memcpy (or a per-pixel alpha blend) instead of the resampler.
	if (dw == srcW && dh == srcH) appendPresentDrawImage(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy);
	else appendPresentDrawImageScaled(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
}
void CanvasRenderingContext2D::drawImage(int imageId, double x, double y, double w, double h) { drawImage(imageId, rounded(x), rounded(y), rounded(w), rounded(h)); }
double CanvasRenderingContext2D::measureTextInkCenter(const std::string &text)
{
	auto *surface = drawingCanvas();
	if (!surface) surface = canvas();
	if (!surface) return 0.0;
	return static_cast<double>(surface->measureTextInkCenterFontFamily(text.c_str(), state_->fontFamilyId_, state_->fontSizePx_));
}

double CanvasRenderingContext2D::measureText(const std::string &text)
{
	auto *surface = drawingCanvas();
	if (!surface) surface = canvas();
	if (!surface) return 0.0;
	return static_cast<double>(surface->measureTextFontFamily(text.c_str(), state_->fontFamilyId_, state_->fontSizePx_));
}

void CanvasRenderingContext2D::drawImageCircle(int imageId, int dx, int dy, int dw, int dh)
{
	gTotalDrawImage++;
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0 || dw <= 0 || dh <= 0) { gTotalDrawImageNullPixels++; return; }
	if (!recordingPresentBatch()) return;
	gCanvasPerfStats.drawImageCalls++;
	const int radius = (dw < dh ? dw : dh) / 2;
	appendPresentDrawImageScaled(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh, radius);
}
void CanvasRenderingContext2D::drawImageCircle(int imageId, double x, double y, double w, double h) { drawImageCircle(imageId, rounded(x), rounded(y), rounded(w), rounded(h)); }
void CanvasRenderingContext2D::drawImageRotated90CW(int, int, int, int, int) {}
void CanvasRenderingContext2D::drawImageRotated90CW(int imageId, double x, double y, double w, double h) { drawImageRotated90CW(imageId, rounded(x), rounded(y), rounded(w), rounded(h)); }
void CanvasRenderingContext2D::drawImageTiledX(int, int, int, int) {}
void CanvasRenderingContext2D::drawImageTiledX(int imageId, double x, double y, double w) { drawImageTiledX(imageId, rounded(x), rounded(y), rounded(w)); }
void CanvasRenderingContext2D::flush() { gea::platform::display::Display::flush(); }

void CanvasRenderingContext2D::beginBatch()
{
	gTotalBeginBatch++;
	gCanvasPerfStats.batchBeginCalls++;
	state_->batchDepth_++;
	if (state_->batchDepth_ != 1) return;
	state_->batchDirty_ = false;
	resetPresentCommands();
	state_->presentRecording_ = true;
}

void CanvasRenderingContext2D::endBatch()
{
	gTotalEndBatch++;
	gCanvasPerfStats.batchEndCalls++;
	if (state_->batchDepth_ <= 0) return;
	state_->batchDepth_--;
	if (state_->batchDepth_ > 0) return;
	if (state_->batchDirty_) {
		gCanvasPerfStats.batchFlushCalls++;
		(void)presentBatch();
	}
	resetPresentCommands();
	state_->presentRecording_ = false;
	state_->batchDirty_ = false;
}

void CanvasRenderer::record(const Node &) {}

#else

CanvasElement CanvasElement::create()
{
	const int id = Tree::instance().createCanvas();
	// A recycled node id must not inherit the previous canvas's styles or batch.
	CanvasRenderingContext2D::releaseState(id);
	return CanvasElement(id);
}

CanvasRenderingContext2D CanvasElement::getContext2D() const
{
	return CanvasRenderingContext2D(id_);
}

gea::framework::graphics::Canvas *CanvasRenderingContext2D::canvas()
{
	const std::int64_t started = nowUs();
	gCanvasPerfStats.canvasLookupCalls++;
	gea::framework::graphics::Canvas *result = nullptr;
	if (state_->batchCanvas_) {
		result = state_->batchCanvas_;
	} else if (nodeId_ >= 0) {
		result = Tree::instance().ensureCanvas(nodeId_);
	}
	gCanvasPerfStats.canvasLookupUs += nowUs() - started;
	return result;
}

gea::framework::graphics::Canvas *CanvasRenderingContext2D::drawingCanvas()
{
	if (state_->presentRecording_) {
			auto *surface = canvas();
			if (surface) {
				replayPresentBatchToCanvas(*surface);
				resetPresentCommands();
				state_->presentRecording_ = false;
				state_->batchCanvas_ = surface;
				state_->batchDirty_ = true;
		}
		return surface;
	}
	if (state_->batchCanvas_) return state_->batchCanvas_;
	return canvas();
}

bool CanvasRenderingContext2D::recordingPresentBatch() const
{
	return state_->presentRecording_ && state_->batchDepth_ > 0 && nodeId_ >= 0;
}

void CanvasRenderingContext2D::appendPresentClear(gea::framework::graphics::pixel::native_t color)
{
	resetPresentCommands();
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::Clear);
	command.clearColor = color;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color)
{
	if (w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillRectRgb565);
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentStrokeRect(int x, int y, int w, int h, gea::framework::graphics::pixel::native_t color)
{
	if (w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::StrokeRectRgb565);
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillTriangle(int x0,
                                                         int y0,
                                                         int x1,
                                                         int y1,
                                                         int x2,
                                                         int y2,
                                                         gea::framework::graphics::pixel::native_t color)
{
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillTriangleRgb565);
	command.x0 = x0;
	command.y0 = y0;
	command.x1 = x1;
	command.y1 = y1;
	command.x2 = x2;
	command.y2 = y2;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillCircle(int x, int y, int radius, gea::framework::graphics::pixel::native_t color)
{
	if (radius <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCircleRgb565);
	command.x = x;
	command.y = y;
	command.radius = radius;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentStrokeCircle(int x, int y, int radius, gea::framework::graphics::pixel::native_t color)
{
	if (radius <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::StrokeCircleRgb565);
	command.x = x;
	command.y = y;
	command.radius = radius;
	command.color = color;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

// Pointer + count forms (standard, non-direct-canvas variant). No temporary
// vector, and only the live `count` elements are copied into the recorded
// command, whose capacity is reused across frames.
void CanvasRenderingContext2D::appendPresentCircles(const std::uint16_t *xs,
                                                    const std::uint16_t *ys,
                                                    int radius,
                                                    const gea::framework::graphics::pixel::native_t *colors,
                                                    int count)
{
	if (radius <= 0 || count <= 0 || !xs || !ys || !colors) return;
	const std::size_t limit = static_cast<std::size_t>(count);
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	command.xs.assign(xs, xs + limit);
	command.ys.assign(ys, ys + limit);
	command.colors.assign(colors, colors + limit);
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCircles(const std::uint16_t *xs,
                                                    const std::uint16_t *ys,
                                                    int radius,
                                                    const gea::framework::graphics::pixel::NativeColor *colors,
                                                    int count)
{
	if (radius <= 0 || count <= 0 || !xs || !ys || !colors) return;
	const std::size_t limit = static_cast<std::size_t>(count);
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	command.xs.assign(xs, xs + limit);
	command.ys.assign(ys, ys + limit);
	command.colors.resize(limit);
	for (std::size_t i = 0; i < limit; ++i) command.colors[i] = colors[i].value;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::uint16_t *xs,
                                                 const std::uint16_t *ys,
                                                 int radius,
                                                 const gea::framework::graphics::pixel::native_t *colors,
                                                 int count)
{
	if (recordingPresentBatch()) {
		gCanvasPerfStats.fillCircleCalls += count > 0 ? count : 0;
		appendPresentCircles(xs, ys, radius, colors, count);
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface || radius <= 0 || count <= 0) return;
	gCanvasPerfStats.fillCircleCalls += count;
	applyDrawState(*surface);
	surface->fillCirclesRgb565(xs, ys, count, radius, colors);
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::uint16_t *xs,
                                                 const std::uint16_t *ys,
                                                 int radius,
                                                 const gea::framework::graphics::pixel::NativeColor *colors,
                                                 int count)
{
	if (recordingPresentBatch()) {
		gCanvasPerfStats.fillCircleCalls += count > 0 ? count : 0;
		appendPresentCircles(xs, ys, radius, colors, count);
		return;
	}
	if (count <= 0 || !colors) return;
	std::vector<gea::framework::graphics::pixel::native_t> native(static_cast<std::size_t>(count));
	for (int i = 0; i < count; ++i) native[static_cast<std::size_t>(i)] = colors[i].value;
	fillCirclesRgb565(xs, ys, radius, native.data(), count);
}


void CanvasRenderingContext2D::appendPresentCircles(const std::vector<std::uint16_t> &xs,
                                                    const std::vector<std::uint16_t> &ys,
                                                    int radius,
                                                    const std::vector<gea::framework::graphics::pixel::native_t> &colors,
                                                    int count)
{
	if (radius <= 0) return;
	std::size_t limit = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < limit) limit = colors.size();
	if (count >= 0 && static_cast<std::size_t>(count) < limit) limit = static_cast<std::size_t>(count);
	const std::size_t count_ = limit;
	if (count_ == 0) return;

	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.xs.assign(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count_));
	command.ys.assign(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(count_));
	command.colors.assign(colors.begin(), colors.begin() + static_cast<std::ptrdiff_t>(count_));
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCircles(const std::vector<std::uint16_t> &xs,
                                                    const std::vector<std::uint16_t> &ys,
                                                    int radius,
                                                    const std::vector<gea::framework::graphics::pixel::NativeColor> &colors,
                                                    int count)
{
	if (radius <= 0) return;
	std::size_t limit = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < limit) limit = colors.size();
	if (count >= 0 && static_cast<std::size_t>(count) < limit) limit = static_cast<std::size_t>(count);
	if (limit == 0) return;

	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.xs.assign(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(limit));
	command.ys.assign(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(limit));
	command.colors.resize(limit);
	for (std::size_t i = 0; i < limit; ++i) command.colors[i] = colors[i].value;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentCirclesUniform(const std::vector<std::uint16_t> &xs,
                                                           const std::vector<std::uint16_t> &ys,
                                                           int radius,
                                                           gea::framework::graphics::pixel::native_t color)
{
	if (radius <= 0) return;
	const std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	if (count == 0) return;

	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillCirclesRgb565);
	command.radius = radius;
	command.xs.assign(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count));
	command.ys.assign(ys.begin(), ys.begin() + static_cast<std::ptrdiff_t>(count));
	command.colors.assign(count, color);
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentTriangles(const std::vector<std::int32_t> &x0s,
                                                      const std::vector<std::int32_t> &y0s,
                                                      const std::vector<std::int32_t> &x1s,
                                                      const std::vector<std::int32_t> &y1s,
                                                      const std::vector<std::int32_t> &x2s,
                                                      const std::vector<std::int32_t> &y2s,
                                                      const std::vector<std::uint32_t> &colors,
                                                      const std::vector<std::int32_t> &order,
                                                      int count)
{
	std::size_t limit = std::min({x0s.size(), y0s.size(), x1s.size(), y1s.size(), x2s.size(), y2s.size(), colors.size(), order.size()});
	if (count >= 0) limit = std::min(limit, static_cast<std::size_t>(count));
	if (limit == 0) return;
	using gea::framework::graphics::TriangleEntry;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillTrianglesRgb565);
	command.alpha = state_->globalAlpha_;
	command.triangles.resize(limit);
	const std::int32_t *px0 = x0s.data();
	const std::int32_t *py0 = y0s.data();
	const std::int32_t *px1 = x1s.data();
	const std::int32_t *py1 = y1s.data();
	const std::int32_t *px2 = x2s.data();
	const std::int32_t *py2 = y2s.data();
	const std::uint32_t *pcolors = colors.data();
	const std::int32_t *porder = order.data();
	TriangleEntry *dst = command.triangles.data();
	const std::size_t sourceSize = x0s.size();
	for (std::size_t i = 0; i < limit; i++) {
		const std::int32_t rawIndex = porder[i];
		const std::size_t o = rawIndex >= 0 && static_cast<std::size_t>(rawIndex) < sourceSize
			? static_cast<std::size_t>(rawIndex)
			: 0;
		TriangleEntry &t = dst[i];
		t.x0 = static_cast<std::int16_t>(px0[o]);
		t.y0 = static_cast<std::int16_t>(py0[o]);
		t.x1 = static_cast<std::int16_t>(px1[o]);
		t.y1 = static_cast<std::int16_t>(py1[o]);
		t.x2 = static_cast<std::int16_t>(px2[o]);
		t.y2 = static_cast<std::int16_t>(py2[o]);
		t.color = gea::framework::graphics::pixel::nativeFromRrggbbaa(pcolors[o]);
		const std::int16_t lo01 = t.y0 < t.y1 ? t.y0 : t.y1;
		const std::int16_t hi01 = t.y0 > t.y1 ? t.y0 : t.y1;
		t.rowY0 = lo01 < t.y2 ? lo01 : t.y2;
		t.rowY1 = hi01 > t.y2 ? hi01 : t.y2;
	}
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentDrawImage(const gea::framework::graphics::pixel::native_t *pixels,
                                                      const std::uint8_t *alphaPixels,
                                                      int srcWidth,
                                                      int srcHeight,
                                                      int x,
                                                      int y)
{
	if (!pixels || srcWidth <= 0 || srcHeight <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::DrawImage);
	command.pixels = pixels;
	command.alphaPixels = alphaPixels;
	command.srcWidth = srcWidth;
	command.srcHeight = srcHeight;
	command.x = x;
	command.y = y;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentDrawImageScaled(const gea::framework::graphics::pixel::native_t *pixels,
                                                            const std::uint8_t *alphaPixels,
                                                            int srcWidth,
                                                            int srcHeight,
                                                            int x,
                                                            int y,
                                                            int w,
                                                            int h,
                                                            int radius)
{
	if (!pixels || srcWidth <= 0 || srcHeight <= 0 || w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::DrawImageScaled);
	command.pixels = pixels;
	command.alphaPixels = alphaPixels;
	command.srcWidth = srcWidth;
	command.srcHeight = srcHeight;
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.radius = radius;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentDrawImageRotated90CW(const gea::framework::graphics::pixel::native_t *pixels,
                                                                 const std::uint8_t *alphaPixels,
                                                                 int srcWidth,
                                                                 int srcHeight,
                                                                 int x,
                                                                 int y,
                                                                 int w,
                                                                 int h)
{
	if (!pixels || srcWidth <= 0 || srcHeight <= 0 || w <= 0 || h <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::DrawImageRotated90CW);
	command.pixels = pixels;
	command.alphaPixels = alphaPixels;
	command.srcWidth = srcWidth;
	command.srcHeight = srcHeight;
	command.x = x;
	command.y = y;
	command.w = w;
	command.h = h;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentDrawImageTiledX(const gea::framework::graphics::pixel::native_t *pixels,
                                                            const std::uint8_t *alphaPixels,
                                                            int srcWidth,
                                                            int srcHeight,
                                                            int x,
                                                            int y,
                                                            int w)
{
	if (!pixels || srcWidth <= 0 || srcHeight <= 0 || w <= 0) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::DrawImageTiledX);
	command.pixels = pixels;
	command.alphaPixels = alphaPixels;
	command.srcWidth = srcWidth;
	command.srcHeight = srcHeight;
	command.x = x;
	command.y = y;
	command.w = w;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::appendPresentFillText(const std::string &text, int x, int y, gea::framework::graphics::pixel::native_t color)
{
	if (text.empty()) return;
	CanvasPresentCommand &command = appendPresentCommand(CanvasPresentCommandType::FillText);
	command.text = text;
	command.x = x;
	command.y = y;
	command.color = color;
	command.scale = state_->fontScale_;
	command.fontFamilyId = state_->fontFamilyId_;
	command.fontSizePx = state_->fontSizePx_;
	command.alpha = state_->globalAlpha_;
	state_->batchDirty_ = true;
}

void CanvasRenderingContext2D::replayPresentBatchToCanvas(gea::framework::graphics::Canvas &surface)
{
	for (std::size_t commandIndex = 0; commandIndex < state_->presentCommandCount_; ++commandIndex) {
		const CanvasPresentCommand &command = state_->presentCommands_[commandIndex];
		surface.setGlobalAlpha(command.alpha);
		switch (command.type) {
		case CanvasPresentCommandType::Clear:
			surface.clear(command.clearColor);
			break;
		case CanvasPresentCommandType::FillRectRgb565:
			surface.fillRect(command.x, command.y, command.w, command.h, command.color);
			break;
		case CanvasPresentCommandType::StrokeRectRgb565:
			surface.strokeRect(command.x, command.y, command.w, command.h, command.color);
			break;
		case CanvasPresentCommandType::FillTriangleRgb565:
			surface.fillTriangle(command.x0, command.y0, command.x1, command.y1, command.x2, command.y2, command.color);
			break;
		case CanvasPresentCommandType::FillCircleRgb565:
			surface.fillCircle(command.x, command.y, command.radius, command.color);
			break;
		case CanvasPresentCommandType::StrokeCircleRgb565:
			surface.strokeCircle(command.x, command.y, command.radius, command.color);
			break;
			case CanvasPresentCommandType::FillCirclesRgb565:
				surface.fillCirclesRgb565(command.xs.data(),
				                          command.ys.data(),
				                          static_cast<int>(command.xs.size()),
				                          command.radius,
				                          command.colors.data());
				break;
			case CanvasPresentCommandType::FillTrianglesRgb565:
				// Entries are depth-ordered; draw in order.
				for (const auto &t : command.triangles) {
				surface.fillTriangle(t.x0, t.y0, t.x1, t.y1, t.x2, t.y2, t.color);
			}
			break;
		case CanvasPresentCommandType::DrawImage:
			surface.drawImage(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight, command.x, command.y);
			break;
		case CanvasPresentCommandType::DrawImageScaled:
			// Honour the corner radius, exactly like the chunk-raster path
			// (display_present.h rasterCommandRows). Without this the replay
			// fallback drew every image as a FULL SQUARE, so a JPEG icon's
			// opaque black corners chipped into its neighbours. This path is
			// taken whenever presentBatch is rejected -- e.g. every frame a
			// popup appears/disappears -- which is exactly when the app showed
			// the square-corner artifact.
			if (command.radius > 0)
				surface.drawImageRounded(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
				                         command.x, command.y, command.w, command.h, command.radius, command.radius,
				                         command.radius, command.radius);
			else
				surface.drawImage(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
				                  command.x, command.y, command.w, command.h);
			break;
		case CanvasPresentCommandType::DrawImageRotated90CW:
			surface.drawImageRotated90CW(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                             command.x, command.y, command.w, command.h);
			break;
		case CanvasPresentCommandType::DrawImageTiledX:
			surface.drawImageTiledX(command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
			                        command.x, command.y, command.w);
			break;
		case CanvasPresentCommandType::FillText:
			if (command.fontFamilyId >= 0)
				surface.drawTextFontFamily(command.text.c_str(), command.x, command.y, command.color, command.fontFamilyId, command.fontSizePx);
			else
				surface.drawText(command.text.c_str(), command.x, command.y, command.color, command.scale);
			break;
		}
	}
	surface.setGlobalAlpha(state_->globalAlpha_);
}

bool CanvasRenderingContext2D::presentBatch()
{
	if (state_->presentCommandCount_ == 0) return false;
	gTotalPresentBatchOk++;

	std::vector<gea::platform::display::DisplayPresentCommand> commands;
	commands.reserve(state_->presentCommandCount_);
	for (std::size_t commandIndex = 0; commandIndex < state_->presentCommandCount_; ++commandIndex) {
		const CanvasPresentCommand &command = state_->presentCommands_[commandIndex];
		gea::platform::display::DisplayPresentCommand displayCommand{};
		switch (command.type) {
		case CanvasPresentCommandType::Clear:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::Clear;
			displayCommand.clear.color = command.clearColor;
			break;
		case CanvasPresentCommandType::FillRectRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillRectRgb565;
			displayCommand.fillRectRgb565 = {command.x, command.y, command.w, command.h, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::StrokeRectRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::StrokeRectRgb565;
			displayCommand.strokeRectRgb565 = {command.x, command.y, command.w, command.h, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::FillTriangleRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillTriangleRgb565;
			displayCommand.fillTriangleRgb565 = {
				command.x0, command.y0, command.x1, command.y1, command.x2, command.y2, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::FillCircleRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillCircleRgb565;
			displayCommand.fillCircleRgb565 = {command.x, command.y, command.radius, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::StrokeCircleRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::StrokeCircleRgb565;
			displayCommand.strokeCircleRgb565 = {command.x, command.y, command.radius, command.color, command.alpha};
			break;
		case CanvasPresentCommandType::FillCirclesRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillCirclesRgb565;
			displayCommand.fillCirclesRgb565.xs = command.xs.data();
			displayCommand.fillCirclesRgb565.ys = command.ys.data();
			displayCommand.fillCirclesRgb565.colors = command.colors.data();
			displayCommand.fillCirclesRgb565.count = static_cast<int>(command.xs.size());
			displayCommand.fillCirclesRgb565.radius = command.radius;
			displayCommand.fillCirclesRgb565.alpha = command.alpha;
			break;
		case CanvasPresentCommandType::FillTrianglesRgb565:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillTrianglesRgb565;
			displayCommand.fillTrianglesRgb565.entries = command.triangles.data();
			displayCommand.fillTrianglesRgb565.count = static_cast<int>(command.triangles.size());
			displayCommand.fillTrianglesRgb565.alpha = command.alpha;
			break;
		case CanvasPresentCommandType::DrawImage:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::DrawImage;
			displayCommand.drawImage = {
				command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight, command.x, command.y, command.alpha};
			break;
		case CanvasPresentCommandType::DrawImageScaled:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::DrawImageScaled;
			displayCommand.drawImageScaled = {
				command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
				command.x, command.y, command.w, command.h, command.alpha, command.radius};
			break;
		case CanvasPresentCommandType::DrawImageRotated90CW:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::DrawImageRotated90CW;
			displayCommand.drawImageRotated90CW = {
				command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight,
				command.x, command.y, command.w, command.h, command.alpha};
			break;
		case CanvasPresentCommandType::DrawImageTiledX:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::DrawImageTiledX;
			displayCommand.drawImageTiledX = {
				command.pixels, command.alphaPixels, command.srcWidth, command.srcHeight, command.x, command.y, command.w, command.alpha};
			break;
		case CanvasPresentCommandType::FillText:
			displayCommand.type = gea::platform::display::DisplayPresentCommandType::FillText;
			displayCommand.fillText = {command.text.c_str(), command.x, command.y, command.color, command.scale,
			                           command.fontFamilyId, command.fontSizePx, command.alpha};
			break;
		}
		commands.push_back(displayCommand);
	}
	return gea::platform::display::Display::present(commands.data(), static_cast<int>(commands.size()));
}

void CanvasRenderingContext2D::applyDrawState(gea::framework::graphics::Canvas &surface) const
{
	surface.setGlobalAlpha(state_->globalAlpha_);
}

void CanvasRenderingContext2D::markDirty()
{
	const std::int64_t started = nowUs();
	gCanvasPerfStats.markDirtyCalls++;
	if (state_->batchDepth_ > 0) {
		state_->batchDirty_ = true;
		gCanvasPerfStats.markDirtyUs += nowUs() - started;
		return;
	}
	if (nodeId_ >= 0) Tree::instance().markCanvasDirty(nodeId_);
	gCanvasPerfStats.markDirtyUs += nowUs() - started;
}

void CanvasRenderingContext2D::markDrawDirty()
{
	if (state_->batchDepth_ > 0) {
		state_->batchDirty_ = true;
		return;
	}
	markDirty();
}

void CanvasRenderingContext2D::setFillStyle(const std::string &value)
{
	if (state_->fillStyleCached_ && value == state_->fillStyleSource_) return;
	state_->fillStyle_ = parseCanvasColor(value);
	state_->fillStyleSource_ = value;
	state_->fillStyleCached_ = true;
}

void CanvasRenderingContext2D::setFillStyleRgb565(gea::framework::graphics::pixel::native_t color)
{
	state_->fillStyle_ = canvasRgb565(color);
	state_->fillStyleCached_ = false;
}

void CanvasRenderingContext2D::setStrokeStyle(const std::string &value)
{
	if (state_->strokeStyleCached_ && value == state_->strokeStyleSource_) return;
	state_->strokeStyle_ = parseCanvasColor(value);
	state_->strokeStyleSource_ = value;
	state_->strokeStyleCached_ = true;
}

void CanvasRenderingContext2D::setStrokeStyleRgb565(gea::framework::graphics::pixel::native_t color)
{
	state_->strokeStyle_ = canvasRgb565(color);
	state_->strokeStyleCached_ = false;
}

void CanvasRenderingContext2D::setGlobalAlpha(double alpha)
{
	state_->globalAlpha_ = alphaFromUnit(alpha);
}

void CanvasRenderingContext2D::setLineWidth(double width)
{
	if (!std::isfinite(width) || width <= 0.0) return;
	state_->lineWidth_ = width;
}

void CanvasRenderingContext2D::setFont(const std::string &font)
{
	// Re-setting the same font (common: a per-frame FPS/HUD readout) is a no-op —
	// skip the CSS parse + FontRegistry family/atlas lookups entirely.
	if (font == state_->lastFontStr_) return;
	state_->lastFontStr_ = font;
	state_->fontScale_ = fontScaleFromCss(font);
	const int px = fontPxFromCss(font);
	if (px > 0) state_->fontSizePx_ = px;
	const std::string family = fontFamilyFromCss(font);
	state_->fontFamilyId_ = family.empty() ? -1 : gea::framework::graphics::FontRegistry::familyId(family.c_str());
	// Only commit to the baked path when an atlas actually exists near this
	// size; otherwise stay on the scaled bitmap font.
	if (state_->fontFamilyId_ >= 0 &&
	    !gea::framework::graphics::FontRegistry::rasterizedFamily(state_->fontFamilyId_, state_->fontSizePx_).valid()) {
		state_->fontFamilyId_ = -1;
	}
}

void CanvasRenderingContext2D::clear()
{
	if (recordingPresentBatch()) {
		appendPresentClear(0);
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	clearRect(0, 0, surface->width(), surface->height());
}

void CanvasRenderingContext2D::clearRect(int x, int y, int w, int h)
{
	if (recordingPresentBatch()) {
		const std::uint8_t previousAlpha = state_->globalAlpha_;
		state_->globalAlpha_ = 255;
		appendPresentFillRect(x, y, w, h, 0);
		state_->globalAlpha_ = previousAlpha;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.clearRectCalls++;
	surface->clearRect(x, y, w, h);
	gCanvasPerfStats.clearRectUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::clearRect(double x, double y, double w, double h)
{
	clearRect(rounded(x), rounded(y), rounded(w), rounded(h));
}

void CanvasRenderingContext2D::fillRect(int x, int y, int w, int h)
{
	gTotalFillRect++;
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		gCanvasPerfStats.fillRectCalls++;
		appendPresentFillRect(x, y, w, h, state_->fillStyle_);
		gCanvasPerfStats.fillRectUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillRectCalls++;
	applyDrawState(*surface);
	surface->fillRect(x, y, w, h, state_->fillStyle_);
	gCanvasPerfStats.fillRectUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillRect(double x, double y, double w, double h)
{
	fillRect(rounded(x), rounded(y), rounded(w), rounded(h));
}

void CanvasRenderingContext2D::strokeRect(int x, int y, int w, int h)
{
	if (recordingPresentBatch()) {
		appendPresentStrokeRect(x, y, w, h, state_->strokeStyle_);
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->strokeRect(x, y, w, h, state_->strokeStyle_);
	markDrawDirty();
}

void CanvasRenderingContext2D::strokeRect(double x, double y, double w, double h)
{
	strokeRect(rounded(x), rounded(y), rounded(w), rounded(h));
}

void CanvasRenderingContext2D::fillCircle(int x, int y, int radius)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		gCanvasPerfStats.fillCircleCalls++;
		appendPresentFillCircle(x, y, radius, state_->fillStyle_);
		gCanvasPerfStats.fillCircleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillCircleCalls++;
	applyDrawState(*surface);
	surface->fillCircle(x, y, radius, state_->fillStyle_);
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCircle(double x, double y, double radius)
{
	fillCircle(rounded(x), rounded(y), rounded(radius));
}

void CanvasRenderingContext2D::strokeCircle(int x, int y, int radius)
{
	if (recordingPresentBatch()) {
		appendPresentStrokeCircle(x, y, radius, state_->strokeStyle_);
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->strokeCircle(x, y, radius, state_->strokeStyle_);
	markDrawDirty();
}

void CanvasRenderingContext2D::strokeCircle(double x, double y, double radius)
{
	strokeCircle(rounded(x), rounded(y), rounded(radius));
}

void CanvasRenderingContext2D::fillCircleRgb565(int x, int y, int radius, gea::framework::graphics::pixel::native_t color)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		gCanvasPerfStats.fillCircleCalls++;
		appendPresentFillCircle(x, y, radius, canvasRgb565(color));
		gCanvasPerfStats.fillCircleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillCircleCalls++;
	applyDrawState(*surface);
	surface->fillCircle(x, y, radius, canvasRgb565(color));
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCircleRgb565(double x, double y, double radius, gea::framework::graphics::pixel::native_t color)
{
	fillCircleRgb565(rounded(x), rounded(y), rounded(radius), color);
}

void CanvasRenderingContext2D::fillTriangleRgb565(int x0,
                                                  int y0,
                                                  int x1,
                                                  int y1,
                                                  int x2,
                                                  int y2,
                                                  gea::framework::graphics::pixel::native_t color)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		gCanvasPerfStats.fillTriangleCalls++;
		appendPresentFillTriangle(x0, y0, x1, y1, x2, y2, canvasRgb565(color));
		gCanvasPerfStats.fillTriangleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillTriangleCalls++;
	applyDrawState(*surface);
	surface->fillTriangle(x0, y0, x1, y1, x2, y2, canvasRgb565(color));
	gCanvasPerfStats.fillTriangleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillTriangleRgb565(double x0,
                                                  double y0,
                                                  double x1,
                                                  double y1,
                                                  double x2,
                                                  double y2,
                                                  gea::framework::graphics::pixel::native_t color)
{
	fillTriangleRgb565(rounded(x0), rounded(y0), rounded(x1), rounded(y1), rounded(x2), rounded(y2), color);
}

void CanvasRenderingContext2D::fillTrianglesRgb565Sorted(const std::vector<std::int32_t> &x0s,
                                                         const std::vector<std::int32_t> &y0s,
                                                         const std::vector<std::int32_t> &x1s,
                                                         const std::vector<std::int32_t> &y1s,
                                                         const std::vector<std::int32_t> &x2s,
                                                         const std::vector<std::int32_t> &y2s,
                                                         const std::vector<std::uint32_t> &colors,
                                                         const std::vector<std::int32_t> &order,
                                                         int count)
{
	std::size_t limit = std::min({x0s.size(), y0s.size(), x1s.size(), y1s.size(), x2s.size(), y2s.size(), colors.size(), order.size()});
	if (count >= 0) limit = std::min(limit, static_cast<std::size_t>(count));
	if (limit == 0) return;
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		gCanvasPerfStats.fillTriangleCalls += static_cast<int>(limit);
		appendPresentTriangles(x0s, y0s, x1s, y1s, x2s, y2s, colors, order, count);
		gCanvasPerfStats.fillTriangleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillTriangleCalls += static_cast<int>(limit);
	applyDrawState(*surface);
	const std::size_t sourceSize = x0s.size();
	for (std::size_t i = 0; i < limit; i++) {
		const std::int32_t rawIndex = order[i];
		const std::size_t o = rawIndex >= 0 && static_cast<std::size_t>(rawIndex) < sourceSize
			? static_cast<std::size_t>(rawIndex)
			: 0;
		surface->fillTriangle(x0s[o], y0s[o], x1s[o], y1s[o], x2s[o], y2s[o],
		                      gea::framework::graphics::pixel::nativeFromRrggbbaa(colors[o]));
	}
	gCanvasPerfStats.fillTriangleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::int32_t> &xs,
                                                 const std::vector<std::int32_t> &ys,
                                                 int radius,
                                                 const std::vector<gea::framework::graphics::pixel::native_t> &colors)
{
	auto *surface = drawingCanvas();
	if (!surface || radius <= 0) return;
	const std::int64_t started = nowUs();
	std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < count) count = colors.size();
	gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
	applyDrawState(*surface);
	for (std::size_t i = 0; i < count; i++) {
		surface->fillCircle(xs[i], ys[i], radius, canvasRgb565(colors[i]));
	}
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::uint16_t> &xs,
                                                 const std::vector<std::uint16_t> &ys,
                                                 int radius,
                                                 const std::vector<gea::framework::graphics::pixel::native_t> &colors,
                                                 int countArg)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
		if (colors.size() < count) count = colors.size();
		if (countArg >= 0 && static_cast<std::size_t>(countArg) < count) count = static_cast<std::size_t>(countArg);
		gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
		appendPresentCircles(xs, ys, radius, colors, static_cast<int>(count));
		gCanvasPerfStats.fillCircleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface || radius <= 0) return;
	std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < count) count = colors.size();
	if (countArg >= 0 && static_cast<std::size_t>(countArg) < count) count = static_cast<std::size_t>(countArg);
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
	applyDrawState(*surface);
	surface->fillCirclesRgb565(xs.data(), ys.data(), static_cast<int>(count), radius, colors.data());
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::uint16_t> &xs,
                                                 const std::vector<std::uint16_t> &ys,
                                                 int radius,
                                                 const std::vector<gea::framework::graphics::pixel::NativeColor> &colors,
                                                 int countArg)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
		if (colors.size() < count) count = colors.size();
		if (countArg >= 0 && static_cast<std::size_t>(countArg) < count) count = static_cast<std::size_t>(countArg);
		gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
		appendPresentCircles(xs, ys, radius, colors, static_cast<int>(count));
		gCanvasPerfStats.fillCircleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface || radius <= 0) return;
	std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	if (colors.size() < count) count = colors.size();
	if (countArg >= 0 && static_cast<std::size_t>(countArg) < count) count = static_cast<std::size_t>(countArg);
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
	std::vector<gea::framework::graphics::pixel::native_t> nativeColors;
	nativeColors.reserve(count);
	for (std::size_t i = 0; i < count; ++i) nativeColors.push_back(colors[i].value);
	applyDrawState(*surface);
	surface->fillCirclesRgb565(xs.data(), ys.data(), static_cast<int>(count), radius, nativeColors.data());
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::int32_t> &xs,
                                                 const std::vector<std::int32_t> &ys,
                                                 int radius,
                                                 gea::framework::graphics::pixel::native_t color)
{
	auto *surface = drawingCanvas();
	if (!surface || radius <= 0) return;
	const std::int64_t started = nowUs();
	const std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
	applyDrawState(*surface);
	for (std::size_t i = 0; i < count; i++) {
		surface->fillCircle(xs[i], ys[i], radius, canvasRgb565(color));
	}
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillCirclesRgb565(const std::vector<std::uint16_t> &xs,
                                                 const std::vector<std::uint16_t> &ys,
                                                 int radius,
                                                 gea::framework::graphics::pixel::native_t color)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		const std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
		gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
		appendPresentCirclesUniform(xs, ys, radius, canvasRgb565(color));
		gCanvasPerfStats.fillCircleUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface || radius <= 0) return;
	const std::size_t count = xs.size() < ys.size() ? xs.size() : ys.size();
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillCircleCalls += static_cast<int>(count);
	applyDrawState(*surface);
	surface->fillCirclesRgb565Uniform(xs.data(), ys.data(), static_cast<int>(count), radius, canvasRgb565(color));
	gCanvasPerfStats.fillCircleUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::beginPath()
{
	state_->hasArc_ = false;
	state_->pathX_.clear();
	state_->pathY_.clear();
	state_->pathStarts_.clear();
	state_->pathClosed_ = false;
}

void CanvasRenderingContext2D::arc(double x, double y, double radius, double, double)
{
	state_->arcX_ = x;
	state_->arcY_ = y;
	state_->arcRadius_ = radius;
	state_->hasArc_ = true;
}

void CanvasRenderingContext2D::moveTo(double x, double y)
{
	// Standard canvas semantics: moveTo starts a NEW subpath (it does not clear
	// the path — beginPath does). This lets one path hold many rings.
	state_->pathStarts_.push_back(static_cast<int>(state_->pathX_.size()));
	state_->pathX_.push_back(x);
	state_->pathY_.push_back(y);
	state_->pathClosed_ = false;
}

void CanvasRenderingContext2D::lineTo(double x, double y)
{
	if (state_->pathX_.empty()) moveTo(x, y);
	else {
		state_->pathX_.push_back(x);
		state_->pathY_.push_back(y);
	}
}

void CanvasRenderingContext2D::closePath()
{
	state_->pathClosed_ = true;
}

void CanvasRenderingContext2D::fill()
{
	// Batch-aware (mirrors fillRect): when recording a present batch the spans
	// are appended as present commands so they actually reach the display; only
	// outside a batch do we draw straight to the surface. (The app composites
	// the map inside beginBatch/endBatch, so direct draws would never present.)
	const bool rec = recordingPresentBatch();
	auto *surface = rec ? canvas() : drawingCanvas();
	if (!surface) return;
	if (!rec) applyDrawState(*surface);
	if (state_->hasArc_) {
		const int cx = rounded(state_->arcX_);
		const int cy = rounded(state_->arcY_);
		const int r = rounded(state_->arcRadius_);
		if (rec) appendPresentFillCircle(cx, cy, r, state_->fillStyle_);
		else {
			surface->fillCircle(cx, cy, r, state_->fillStyle_);
			markDrawDirty();
		}
		return;
	}
	const std::size_t n = state_->pathX_.size();
	if (n < 3 || state_->pathStarts_.empty()) return;

	// Even-odd scanline fill over all subpaths (each implicitly closed). Unlike
	// a triangle fan (convex only), this fills arbitrary concave shapes AND
	// holes — coastlines, lakes-with-islands, building courtyards. Inside spans
	// emit as 1px fillRects.
	double minYf = state_->pathY_[0];
	double maxYf = state_->pathY_[0];
	for (std::size_t i = 1; i < n; i++) {
		if (state_->pathY_[i] < minYf) minYf = state_->pathY_[i];
		if (state_->pathY_[i] > maxYf) maxYf = state_->pathY_[i];
	}
	const int yTop = std::max(0, static_cast<int>(std::floor(minYf)));
	const int yBot = std::min(surface->height() - 1, static_cast<int>(std::ceil(maxYf)));
	const std::size_t nStarts = state_->pathStarts_.size();
	static std::vector<double> nodes;
	for (int y = yTop; y <= yBot; y++) {
		const double yc = y + 0.5;
		nodes.clear();
		for (std::size_t s = 0; s < nStarts; s++) {
			const std::size_t a = static_cast<std::size_t>(state_->pathStarts_[s]);
			const std::size_t b = (s + 1 < nStarts) ? static_cast<std::size_t>(state_->pathStarts_[s + 1]) : n;
			if (b - a < 2) continue;
			for (std::size_t i = a; i < b; i++) {
				const std::size_t j = (i + 1 < b) ? i + 1 : a;  // close the ring
				const double yi = state_->pathY_[i];
				const double yj = state_->pathY_[j];
				if ((yi < yc && yj >= yc) || (yj < yc && yi >= yc)) {
					const double t = (yc - yi) / (yj - yi);
					nodes.push_back(state_->pathX_[i] + t * (state_->pathX_[j] - state_->pathX_[i]));
				}
			}
		}
		if (nodes.size() < 2) continue;
		std::sort(nodes.begin(), nodes.end());
		for (std::size_t k = 0; k + 1 < nodes.size(); k += 2) {
			const int xa = static_cast<int>(std::lround(nodes[k]));
			const int xb = static_cast<int>(std::lround(nodes[k + 1]));
			if (xb > xa) {
				if (rec) appendPresentFillRect(xa, y, xb - xa, 1, state_->fillStyle_);
				else surface->fillRect(xa, y, xb - xa, 1, state_->fillStyle_);
			}
		}
	}
	if (!rec) markDrawDirty();
}

void CanvasRenderingContext2D::stroke()
{
	const bool rec = recordingPresentBatch();
	auto *surface = rec ? canvas() : drawingCanvas();
	if (!surface) return;
	if (!rec) applyDrawState(*surface);
	if (state_->hasArc_) {
		const int cx = rounded(state_->arcX_);
		const int cy = rounded(state_->arcY_);
		const int r = rounded(state_->arcRadius_);
		if (rec) appendPresentStrokeCircle(cx, cy, r, state_->strokeStyle_);
		else {
			surface->strokeCircle(cx, cy, r, state_->strokeStyle_);
			markDrawDirty();
		}
		return;
	}
	const std::size_t n = state_->pathX_.size();
	if (n < 2 || state_->pathStarts_.empty()) return;
	// Honor lineWidth: every segment is an oriented quad (two triangles), with a
	// round disc at each joint/cap for widths ≥ ~2px — roads, casings, rivers.
	// Always quad (not Bresenham) so it has a present-command representation.
	const double half = std::max(0.5, state_->lineWidth_ * 0.5);
	const std::size_t nStarts = state_->pathStarts_.size();
	for (std::size_t s = 0; s < nStarts; s++) {
		const std::size_t a = static_cast<std::size_t>(state_->pathStarts_[s]);
		const std::size_t b = (s + 1 < nStarts) ? static_cast<std::size_t>(state_->pathStarts_[s + 1]) : n;
		if (b - a < 2) continue;
		const std::size_t lastSeg = state_->pathClosed_ ? b : b - 1;
		for (std::size_t i = a; i < lastSeg; i++) {
			const std::size_t j = (i + 1 < b) ? i + 1 : a;  // wrap when closed
			const double dx = state_->pathX_[j] - state_->pathX_[i];
			const double dy = state_->pathY_[j] - state_->pathY_[i];
			const double len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-6) continue;
			const double px = -dy / len * half;  // perpendicular offset
			const double py = dx / len * half;
			const int ax0 = rounded(state_->pathX_[i] + px), ay0 = rounded(state_->pathY_[i] + py);
			const int ax1 = rounded(state_->pathX_[i] - px), ay1 = rounded(state_->pathY_[i] - py);
			const int bx0 = rounded(state_->pathX_[j] + px), by0 = rounded(state_->pathY_[j] + py);
			const int bx1 = rounded(state_->pathX_[j] - px), by1 = rounded(state_->pathY_[j] - py);
			if (rec) {
				appendPresentFillTriangle(ax0, ay0, ax1, ay1, bx1, by1, state_->strokeStyle_);
				appendPresentFillTriangle(ax0, ay0, bx1, by1, bx0, by0, state_->strokeStyle_);
			} else {
				surface->fillTriangle(ax0, ay0, ax1, ay1, bx1, by1, state_->strokeStyle_);
				surface->fillTriangle(ax0, ay0, bx1, by1, bx0, by0, state_->strokeStyle_);
			}
		}
		if (half >= 1.0) {
			const int r = rounded(half);
			for (std::size_t i = a; i < b; i++) {
				const int cx = rounded(state_->pathX_[i]);
				const int cy = rounded(state_->pathY_[i]);
				if (rec) appendPresentFillCircle(cx, cy, r, state_->strokeStyle_);
				else surface->fillCircle(cx, cy, r, state_->strokeStyle_);
			}
		}
	}
	if (!rec) markDrawDirty();
}

void CanvasRenderingContext2D::fillText(const std::string &text, int x, int y)
{
	if (recordingPresentBatch()) {
		const std::int64_t started = nowUs();
		gCanvasPerfStats.fillTextCalls++;
		appendPresentFillText(text, x, y, state_->fillStyle_);
		gCanvasPerfStats.fillTextUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface || text.empty()) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.fillTextCalls++;
	applyDrawState(*surface);
	if (state_->fontFamilyId_ >= 0) surface->drawTextFontFamily(text.c_str(), x, y, state_->fillStyle_, state_->fontFamilyId_, state_->fontSizePx_);
	else surface->drawText(text.c_str(), x, y, state_->fillStyle_, state_->fontScale_);
	gCanvasPerfStats.fillTextUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::fillText(const std::string &text, double x, double y)
{
	fillText(text, rounded(x), rounded(y));
}

void CanvasRenderingContext2D::drawImage(int imageId, int dx, int dy)
{
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	if (recordingPresentBatch()) {
		appendPresentDrawImage(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy);
		gCanvasPerfStats.drawImageUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->drawImage(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::drawImage(int imageId, double dx, double dy)
{
	drawImage(imageId, rounded(dx), rounded(dy));
}

void CanvasRenderingContext2D::drawPixelRows(const gea::framework::graphics::pixel::native_t *pixels, int stride, int width, int height, int dx, int dy)
{
	if (!pixels || width <= 0 || height <= 0 || stride < width) return;
	const std::uint8_t previousAlpha = state_->globalAlpha_;
	state_->globalAlpha_ = 255;
	if (recordingPresentBatch()) {
		for (int row = 0; row < height; ++row)
			appendPresentDrawImage(pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride), nullptr, width, 1, dx, dy + row);
	} else if (auto *surface = drawingCanvas()) {
		applyDrawState(*surface);
		for (int row = 0; row < height; ++row)
			surface->drawImage(pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride), nullptr, width, 1, dx, dy + row);
		markDrawDirty();
	}
	state_->globalAlpha_ = previousAlpha;
}

void CanvasRenderingContext2D::putImage(int imageId, int dx, int dy)
{
	// putImageData replaces the destination pixels: globalAlpha does not apply.
	const std::uint8_t previousAlpha = state_->globalAlpha_;
	state_->globalAlpha_ = 255;
	drawImage(imageId, dx, dy);
	state_->globalAlpha_ = previousAlpha;
}

void CanvasRenderingContext2D::drawImage(int imageId, int dx, int dy, int dw, int dh)
{
	gTotalDrawImage++;
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0) { gTotalDrawImageNullPixels++; return; }
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	if (recordingPresentBatch()) {
		appendPresentDrawImageScaled(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh);
		gCanvasPerfStats.drawImageUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->drawImage(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::drawImage(int imageId, double dx, double dy, double dw, double dh)
{
	drawImage(imageId, rounded(dx), rounded(dy), rounded(dw), rounded(dh));
}

double CanvasRenderingContext2D::measureTextInkCenter(const std::string &text)
{
	auto *surface = drawingCanvas();
	if (!surface) surface = canvas();
	if (!surface) return 0.0;
	return static_cast<double>(surface->measureTextInkCenterFontFamily(text.c_str(), state_->fontFamilyId_, state_->fontSizePx_));
}

double CanvasRenderingContext2D::measureText(const std::string &text)
{
	auto *surface = drawingCanvas();
	if (!surface) surface = canvas();
	if (!surface) return 0.0;
	return static_cast<double>(surface->measureTextFontFamily(text.c_str(), state_->fontFamilyId_, state_->fontSizePx_));
}

void CanvasRenderingContext2D::drawImageCircle(int imageId, int dx, int dy, int dw, int dh)
{
	gTotalDrawImage++;
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0 || dw <= 0 || dh <= 0) { gTotalDrawImageNullPixels++; return; }
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	const int radius = (dw < dh ? dw : dh) / 2;
	if (recordingPresentBatch()) {
		appendPresentDrawImageScaled(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh, radius);
		gCanvasPerfStats.drawImageUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->drawImageRounded(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh, radius, radius,
	                          radius, radius);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::drawImageCircle(int imageId, double dx, double dy, double dw, double dh)
{
	drawImageCircle(imageId, rounded(dx), rounded(dy), rounded(dw), rounded(dh));
}

void CanvasRenderingContext2D::drawImageRotated90CW(int imageId, int dx, int dy, int dw, int dh)
{
	gTotalDrawImage++;
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0 || dw <= 0 || dh <= 0) { gTotalDrawImageNullPixels++; return; }
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	if (recordingPresentBatch()) {
		appendPresentDrawImageRotated90CW(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh);
		gCanvasPerfStats.drawImageUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->drawImageRotated90CW(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, dw, dh);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::drawImageRotated90CW(int imageId, double dx, double dy, double dw, double dh)
{
	drawImageRotated90CW(imageId, rounded(dx), rounded(dy), rounded(dw), rounded(dh));
}

void CanvasRenderingContext2D::drawImageTiledX(int imageId, int dx, int dy, int width)
{
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int srcW = images.width(imageId);
	const int srcH = images.height(imageId);
	const gea::framework::graphics::pixel::native_t *pixels = images.currentPixels(imageId);
	if (!pixels || srcW <= 0 || srcH <= 0 || width <= 0) return;
	const std::int64_t started = nowUs();
	gCanvasPerfStats.drawImageCalls++;
	if (recordingPresentBatch()) {
		appendPresentDrawImageTiledX(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, width);
		gCanvasPerfStats.drawImageUs += nowUs() - started;
		return;
	}
	auto *surface = drawingCanvas();
	if (!surface) return;
	applyDrawState(*surface);
	surface->drawImageTiledX(pixels, images.currentAlpha(imageId), srcW, srcH, dx, dy, width);
	gCanvasPerfStats.drawImageUs += nowUs() - started;
	markDrawDirty();
}

void CanvasRenderingContext2D::drawImageTiledX(int imageId, double dx, double dy, double width)
{
	drawImageTiledX(imageId, rounded(dx), rounded(dy), rounded(width));
}

void CanvasRenderingContext2D::flush()
{
	if (nodeId_ < 0) return;
	if (!Tree::instance().isDisplayBackedCanvas(nodeId_)) {
		markDirty();
		return;
	}
	gea::platform::display::Display::flush();
}

void CanvasRenderingContext2D::beginBatch()
{
	gTotalBeginBatch++;
	gCanvasPerfStats.batchBeginCalls++;
	state_->batchDepth_++;
	if (state_->batchDepth_ != 1) return;
	state_->batchDirty_ = false;
	resetPresentCommands();
	// Re-evaluate display-backed eligibility every frame without binding the
	// display canvas: command-recorded batches do not need a framebuffer until
	// they fall back to replay. This avoids expensive target-side preservation
	// on double-buffered panels.
	if (nodeId_ >= 0 && Tree::instance().markDisplayBackedCanvas(nodeId_)) {
		state_->presentRecording_ = true;
		state_->batchCanvas_ = nullptr;
		return;
	}
	if (nodeId_ >= 0) Tree::instance().ensureCanvas(nodeId_);
	if (nodeId_ >= 0 && Tree::instance().isDisplayBackedCanvas(nodeId_)) {
		state_->presentRecording_ = true;
		state_->batchCanvas_ = nullptr;
		return;
	}
	state_->presentRecording_ = false;
	state_->batchCanvas_ = canvas();
}

void CanvasRenderingContext2D::endBatch()
{
	gTotalEndBatch++;
	gCanvasPerfStats.batchEndCalls++;
	if (state_->batchDepth_ <= 0) return;
	state_->batchDepth_--;
	if (state_->batchDepth_ > 0) return;
	state_->batchCanvas_ = nullptr;
	if (state_->batchDirty_ && nodeId_ >= 0) {
		if (Tree::instance().isDisplayBackedCanvas(nodeId_)) {
			const std::int64_t started = nowUs();
			gCanvasPerfStats.batchFlushCalls++;
			bool presented = false;
			if (state_->presentRecording_) presented = presentBatch();
			if (presented) {
				Tree::instance().clearNodeDisplayCommandDirty(nodeId_);
			}
			if (!presented) {
				// Replay+flush paints the OFF-SCREEN composed buffer over the
				// scanout — when this interleaves with a double-buffered present
				// path the screen alternates fresh/stale frames (flicker).
				// Loud while diagnosing that exact symptom.
				static int replayFalls = 0;
				replayFalls++;
				if ((replayFalls & 7) == 1) std::printf("[canvas] presentBatch REJECTED -> replay+flush (#%d)\n", replayFalls);
				state_->presentRecording_ = false;
				auto *surface = canvas();
				if (surface) replayPresentBatchToCanvas(*surface);
				gea::platform::display::Display::flush();
			}
			gCanvasPerfStats.batchFlushUs += nowUs() - started;
		} else {
			Tree::instance().markCanvasDirty(nodeId_);
		}
	}
	resetPresentCommands();
	state_->presentRecording_ = false;
	state_->batchDirty_ = false;
}

void CanvasRenderer::record(const Node &node)
{
	const Node *nodes = Tree::instance().nodes();
	const int id = static_cast<int>(&node - nodes);
	if (Tree::instance().isDisplayBackedCanvas(id)) return;

	const auto *surface = Tree::instance().canvas(id);
	if (!surface || !surface->pixels() || surface->width() <= 0 || surface->height() <= 0) return;

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::BlitImage;
	cmd->bx = node.layout.x;
	cmd->by = node.layout.y;
	cmd->bw = node.layout.width;
	cmd->bh = node.layout.height;
	cmd->blit.pixels = surface->pixels();
	cmd->blit.alpha = nullptr;
	cmd->blit.sourceWidth = surface->width();
	cmd->blit.sourceHeight = surface->height();
	cmd->blit.dx = node.layout.x;
	cmd->blit.dy = node.layout.y;
	// The canvas surface is stored in the framebuffer's native packing; on the
	// grayscale targets that is sub-byte packed (2 px/byte on GRAY4, 4 px/byte on
	// GRAY2), so flag it for the blit to unpack.
	cmd->blit.sourcePacked = GEA_PIXEL_STORAGE_PACKED ? 1 : 0;
}

#endif

}  // namespace gea::embedded::ui
