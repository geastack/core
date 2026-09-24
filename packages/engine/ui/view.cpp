// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "state_init.h"
#include "style_values.h"
#include "tree_state.h"
#include <pixel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef quad
#undef quad
#endif

#ifndef GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_EMBEDDED_RENDER_HOT_SRAM 0
#endif

#if GEA_EMBEDDED_RENDER_HOT_SRAM
#define GEA_VIEW_HOT_SRAM_SECTION(name) __attribute__((noinline, noclone, section(".time_critical.gea_view." name)))
#else
#define GEA_VIEW_HOT_SRAM_SECTION(name)
#endif

namespace gea::embedded::ui {

class TextBackgroundClipScope {
	int previous_ = -1;
	bool active_ = false;
public:
	TextBackgroundClipScope(const Node &node, int clip) : active_(clip == 3) {
		if (active_) previous_ = DisplayList::instance().setRecordingTextClipOwner(
		    static_cast<int>(&node - Tree::instance().nodes()));
	}
	~TextBackgroundClipScope() { if (active_) DisplayList::instance().setRecordingTextClipOwner(previous_); }
};

constexpr double kPi = 3.14159265358979323846;
constexpr float kPiF = 3.14159265358979323846f;
// Reciprocal constants so the per-corner transform math uses hardware FPU
// multiplies. The ESP32-S3 (Xtensa LX7) FPU has mul/add/sub but NO divide
// instruction, so `/ 1000.0f` lowered to a software __divsf3 call — measured
// as the dominant per-call cost of applyNodeTransform (12 software divides ×
// ~2300 calls/frame on the spinning css-3d-cube). `* kInv1000f` is a single
// hardware mul; sub-pixel accurate for screen coords (the values feeding this
// are integer permille/pixel quantities <= ~512).
constexpr float kInv1000f = 1.0f / 1000.0f;
constexpr float kInv1800f = 1.0f / 1800.0f;
// Number of constant-color strips a transformed (3D/perspective) linear gradient
// is sliced into (each strip is a FillQuad). Measured cheap on-device (~20ms for
// all of css-3d-cube's face strips combined — the frame cost was the full-screen
// background gradient, not these), so favor smoothness.
constexpr int kTransformedGradientSteps = 72;

uint8_t combineAlpha(uint8_t parentAlpha, uint8_t localAlpha)
{
	return static_cast<uint8_t>((static_cast<int>(parentAlpha) * static_cast<int>(localAlpha) + 127) / 255);
}

bool borderIsSameOpaqueSolidBackground(const Node &node, uint8_t parentAlpha)
{
	return !hasBorderRelief(node.style) && node.style.has_bg &&
	       StyleValues::backgroundClip(node.style, rstyle(node.style).bg_image_layer_count - 1) == 0 &&
	       !styleHasBackgroundImage(node.style) &&
	       node.style.bg_color == borderPaintColor(node.style, 0) &&
	       combineAlpha(parentAlpha, node.style.bg_alpha) == combineAlpha(parentAlpha, borderPaintAlpha(node.style, 0));
}

void appendAlphaCommand(uint8_t alpha, int bx, int by, int bw, int bh)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::SetAlpha;
	cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
	cmd->alpha.alpha = alpha;
}

uint16_t interpolateColor(uint16_t from, uint16_t to, int permille)
{
	if (permille <= 0) return from;
	if (permille >= 1000) return to;
	return gea::framework::graphics::pixel::blend(to, from, (permille * 255 + 500) / 1000);
}

uint8_t interpolateAlpha(uint8_t from, uint8_t to, int permille)
{
	if (permille <= 0) return from;
	if (permille >= 1000) return to;
	return static_cast<uint8_t>((static_cast<int>(from) * (1000 - permille) + static_cast<int>(to) * permille + 500) / 1000);
}

uint16_t interpolatePremultipliedColor(uint16_t fromColor, uint8_t fromAlpha,
                                       uint16_t toColor, uint8_t toAlpha,
                                       int permille)
{
	if (permille <= 0) return fromColor;
	if (permille >= 1000) return toColor;
	int fr = 0, fg = 0, fb = 0, tr = 0, tg = 0, tb = 0;
	gea::framework::graphics::pixel::unpackRgb565(fromColor, &fr, &fg, &fb);
	gea::framework::graphics::pixel::unpackRgb565(toColor, &tr, &tg, &tb);
	fr = (fr * 255 + 15) / 31;
	fg = (fg * 255 + 31) / 63;
	fb = (fb * 255 + 15) / 31;
	tr = (tr * 255 + 15) / 31;
	tg = (tg * 255 + 31) / 63;
	tb = (tb * 255 + 15) / 31;
	const int fromWeight = static_cast<int>(fromAlpha) * (1000 - permille);
	const int toWeight = static_cast<int>(toAlpha) * permille;
	const int weight = fromWeight + toWeight;
	if (weight <= 0) return toColor;
	const int r = (fr * fromWeight + tr * toWeight + weight / 2) / weight;
	const int g = (fg * fromWeight + tg * toWeight + weight / 2) / weight;
	const int b = (fb * fromWeight + tb * toWeight + weight / 2) / weight;
	// Panel-endian-aware: framebuffer pixels are byte-swapped when
	// GEA_EMBEDDED_PIXEL_PANEL_ENDIAN=1 (the endpoints above return the already
	// byte-swapped stop colors). Emitting raw rgb565FromRgb888 here made the
	// interpolated middle of transformed gradient faces land un-swapped → scrambled
	// hues on device. fromRgb888 == rgb565FromRgb888 on host.
	return gea::framework::graphics::pixel::fromRgb888(r, g, b);
}

int clampPermille(int value)
{
	if (value < 0) return 0;
	if (value > 1000) return 1000;
	return value;
}

int gradientToStop(const Node &node)
{
	const int toStop = rstyle(node.style).bg_gradient_to_stop > 0 ? rstyle(node.style).bg_gradient_to_stop : 1000;
	return std::max(1, toStop);
}

int stopRangePermille(int permille, int start, int end)
{
	if (permille <= start) return 0;
	if (permille >= end) return 1000;
	const int span = end - start;
	if (span <= 0) return 1000;
	return ((permille - start) * 1000 + span / 2) / span;
}

uint16_t gradientColorAt(const Node &node, int permille)
{
	permille = clampPermille(permille);
	const int toStop = gradientToStop(node);
	if (rstyle(node.style).bg_gradient_has_mid && rstyle(node.style).bg_gradient_mid_stop > 0 && rstyle(node.style).bg_gradient_mid_stop < toStop) {
		const int stop = rstyle(node.style).bg_gradient_mid_stop;
		if (permille <= stop)
			return interpolatePremultipliedColor(rstyle(node.style).bg_gradient_from_color,
			                                     rstyle(node.style).bg_gradient_from_alpha,
			                                     rstyle(node.style).bg_gradient_mid_color,
			                                     rstyle(node.style).bg_gradient_mid_alpha,
			                                     stopRangePermille(permille, 0, stop));
		return interpolatePremultipliedColor(rstyle(node.style).bg_gradient_mid_color,
		                                     rstyle(node.style).bg_gradient_mid_alpha,
		                                     rstyle(node.style).bg_gradient_to_color,
		                                     rstyle(node.style).bg_gradient_to_alpha,
		                                     stopRangePermille(permille, stop, toStop));
	}
	return interpolatePremultipliedColor(rstyle(node.style).bg_gradient_from_color,
	                                     rstyle(node.style).bg_gradient_from_alpha,
	                                     rstyle(node.style).bg_gradient_to_color,
	                                     rstyle(node.style).bg_gradient_to_alpha,
	                                     stopRangePermille(permille, 0, toStop));
}

uint8_t gradientAlphaAt(const Node &node, int permille)
{
	permille = clampPermille(permille);
	const int toStop = gradientToStop(node);
	if (rstyle(node.style).bg_gradient_has_mid && rstyle(node.style).bg_gradient_mid_stop > 0 && rstyle(node.style).bg_gradient_mid_stop < toStop) {
		const int stop = rstyle(node.style).bg_gradient_mid_stop;
		if (permille <= stop)
			return interpolateAlpha(rstyle(node.style).bg_gradient_from_alpha,
			                        rstyle(node.style).bg_gradient_mid_alpha,
			                        stopRangePermille(permille, 0, stop));
		return interpolateAlpha(rstyle(node.style).bg_gradient_mid_alpha,
		                        rstyle(node.style).bg_gradient_to_alpha,
		                        stopRangePermille(permille, stop, toStop));
	}
	return interpolateAlpha(rstyle(node.style).bg_gradient_from_alpha, rstyle(node.style).bg_gradient_to_alpha, stopRangePermille(permille, 0, toStop));
}

void appendFillRectWithAlpha(int x, int y, int w, int h, uint16_t color, uint8_t alpha, uint8_t parentAlpha, int bx, int by, int bw, int bh)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx, by, bw, bh);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillRect;
		cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
		cmd->fill.x = x; cmd->fill.y = y;
		cmd->fill.w = w; cmd->fill.h = h;
		cmd->fill.color = color;
	}
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx, by, bw, bh);
}

void appendFillRectRaw(int x, int y, int w, int h, uint16_t color, int bx, int by, int bw, int bh)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillRect;
	cmd->bx = bx; cmd->by = by; cmd->bw = bw; cmd->bh = bh;
	cmd->fill.x = x; cmd->fill.y = y;
	cmd->fill.w = w; cmd->fill.h = h;
	cmd->fill.color = color;
}

void appendFillQuadWithAlpha(const int16_t *xs, const int16_t *ys, uint16_t color, uint8_t alpha, uint8_t parentAlpha,
                             int bx, int by, int bw, int bh,
                             int lx = 0, int ly = 0, int lw = 0, int lh = 0, uint8_t reprojectMode = 0)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx, by, bw, bh);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillQuad;
		cmd->bx = bx;
		cmd->by = by;
		cmd->bw = bw;
		cmd->bh = bh;
		cmd->quad.x0 = xs[0]; cmd->quad.y0 = ys[0];
		cmd->quad.x1 = xs[1]; cmd->quad.y1 = ys[1];
		cmd->quad.x2 = xs[2]; cmd->quad.y2 = ys[2];
		cmd->quad.x3 = xs[3]; cmd->quad.y3 = ys[3];
		cmd->quad.lx = static_cast<int16_t>(lx); cmd->quad.ly = static_cast<int16_t>(ly);
		cmd->quad.lw = static_cast<int16_t>(lw); cmd->quad.lh = static_cast<int16_t>(lh);
		cmd->quad.aux = 0;
		cmd->quad.color = color;
		cmd->quad.reprojectMode = reprojectMode;
	}
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx, by, bw, bh);
}

void appendFillQuadRaw(const int16_t *xs, const int16_t *ys, uint16_t color, int bx, int by, int bw, int bh,
                       int lx = 0, int ly = 0, int lw = 0, int lh = 0, uint8_t reprojectMode = 0)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillQuad;
	cmd->bx = bx;
	cmd->by = by;
	cmd->bw = bw;
	cmd->bh = bh;
	cmd->quad.x0 = xs[0]; cmd->quad.y0 = ys[0];
	cmd->quad.x1 = xs[1]; cmd->quad.y1 = ys[1];
	cmd->quad.x2 = xs[2]; cmd->quad.y2 = ys[2];
	cmd->quad.x3 = xs[3]; cmd->quad.y3 = ys[3];
	cmd->quad.lx = static_cast<int16_t>(lx); cmd->quad.ly = static_cast<int16_t>(ly);
	cmd->quad.lw = static_cast<int16_t>(lw); cmd->quad.lh = static_cast<int16_t>(lh);
	cmd->quad.aux = 0;
	cmd->quad.color = color;
	cmd->quad.reprojectMode = reprojectMode;
}

void appendFillQuadStrokeSegmentRaw(const int16_t *xs, const int16_t *ys, uint16_t color,
                                    int bx, int by, int bw, int bh,
                                    float localX0, float localY0, float localX1, float localY1, float strokeWidth)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillQuad;
	cmd->bx = bx;
	cmd->by = by;
	cmd->bw = bw;
	cmd->bh = bh;
	cmd->quad.x0 = xs[0]; cmd->quad.y0 = ys[0];
	cmd->quad.x1 = xs[1]; cmd->quad.y1 = ys[1];
	cmd->quad.x2 = xs[2]; cmd->quad.y2 = ys[2];
	cmd->quad.x3 = xs[3]; cmd->quad.y3 = ys[3];
	cmd->quad.lx = static_cast<int16_t>(std::lroundf(localX0 * 8.0f));
	cmd->quad.ly = static_cast<int16_t>(std::lroundf(localY0 * 8.0f));
	cmd->quad.lw = static_cast<int16_t>(std::lroundf(localX1 * 8.0f));
	cmd->quad.lh = static_cast<int16_t>(std::lroundf(localY1 * 8.0f));
	cmd->quad.aux = static_cast<int16_t>(std::lroundf(strokeWidth * 8.0f));
	cmd->quad.color = color;
	cmd->quad.reprojectMode = 2;
}

void boundsFromCorners(const int16_t *xs, const int16_t *ys, int *x0, int *y0, int *x1, int *y1);

DisplayCommand *appendLinearGradientRectRaw(const Node &node, int x, int y, int w, int h,
                                 uint16_t fromColor, uint16_t midColor, uint16_t toColor,
                                 uint16_t midStop, uint16_t toStop, int16_t angle, uint8_t fromAlpha,
                                 uint8_t midAlpha, uint8_t toAlpha, uint8_t hasMid)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return nullptr;
	cmd->type = DisplayCommandType::FillLinearGradient;
	cmd->bx = x;
	cmd->by = y;
	cmd->bw = w;
	cmd->bh = h;
	cmd->gradient.x = x;
	cmd->gradient.y = y;
	cmd->gradient.w = w;
	cmd->gradient.h = h;
	cmd->gradient.tl = node.style.border_radius[0];
	cmd->gradient.tr = node.style.border_radius[1];
	cmd->gradient.br = node.style.border_radius[2];
	cmd->gradient.bl = node.style.border_radius[3];
	cmd->gradient.fromColor = fromColor;
	cmd->gradient.midColor = midColor;
	cmd->gradient.toColor = toColor;
	cmd->gradient.midStop = midStop;
	cmd->gradient.toStop = toStop > 0 ? toStop : 1000;
	cmd->gradient.angle = angle;
	cmd->gradient.fromAlpha = fromAlpha;
	cmd->gradient.midAlpha = midAlpha;
	cmd->gradient.toAlpha = toAlpha;
	cmd->gradient.hasMid = hasMid;
	return cmd;
}

DisplayCommand *appendLinearGradientRectRaw(const Node &node, int x, int y, int w, int h)
{
	return appendLinearGradientRectRaw(node,
	                            x,
	                            y,
	                            w,
	                            h,
	                            rstyle(node.style).bg_gradient_from_color,
	                            rstyle(node.style).bg_gradient_mid_color,
	                            rstyle(node.style).bg_gradient_to_color,
	                            rstyle(node.style).bg_gradient_mid_stop,
	                            rstyle(node.style).bg_gradient_to_stop,
	                            rstyle(node.style).bg_gradient_angle,
	                            rstyle(node.style).bg_gradient_from_alpha,
	                            rstyle(node.style).bg_gradient_mid_alpha,
	                            rstyle(node.style).bg_gradient_to_alpha,
	                            rstyle(node.style).bg_gradient_has_mid);
}

DisplayCommand *appendRadialGradientRectRaw(const Node &node, int x, int y, int w, int h)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return nullptr;
	cmd->type = DisplayCommandType::FillRadialGradient;
	cmd->bx = x;
	cmd->by = y;
	cmd->bw = w;
	cmd->bh = h;
	cmd->radialGradient.x = x;
	cmd->radialGradient.y = y;
	cmd->radialGradient.w = w;
	cmd->radialGradient.h = h;
	cmd->radialGradient.tl = node.style.border_radius[0];
	cmd->radialGradient.tr = node.style.border_radius[1];
	cmd->radialGradient.br = node.style.border_radius[2];
	cmd->radialGradient.bl = node.style.border_radius[3];
	cmd->radialGradient.cxPermille = rstyle(node.style).bg_radial_gradient_cx;
	cmd->radialGradient.cyPermille = rstyle(node.style).bg_radial_gradient_cy;
	cmd->radialGradient.rxPermille = rstyle(node.style).bg_radial_gradient_rx;
	cmd->radialGradient.ryPermille = rstyle(node.style).bg_radial_gradient_ry;
	cmd->radialGradient.fromColor = rstyle(node.style).bg_radial_gradient_from_color;
	cmd->radialGradient.toColor = rstyle(node.style).bg_radial_gradient_to_color;
	cmd->radialGradient.stopPermille = rstyle(node.style).bg_radial_gradient_stop;
	cmd->radialGradient.fromAlpha = rstyle(node.style).bg_radial_gradient_from_alpha;
	cmd->radialGradient.toAlpha = rstyle(node.style).bg_radial_gradient_to_alpha;
	return cmd;
}

void appendDrawLineRaw(int x0, int y0, int x1, int y1, uint16_t color)
{
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::DrawLine;
	const int bx0 = std::min(x0, x1);
	const int by0 = std::min(y0, y1);
	const int bx1 = std::max(x0, x1);
	const int by1 = std::max(y0, y1);
	cmd->bx = bx0;
	cmd->by = by0;
	cmd->bw = bx1 - bx0 + 1;
	cmd->bh = by1 - by0 + 1;
	cmd->line.x0 = x0;
	cmd->line.y0 = y0;
	cmd->line.x1 = x1;
	cmd->line.y1 = y1;
	cmd->line.color = color;
}

void appendStrokeSegmentBandRaw(float x0, float y0, float x1, float y1, float width, uint16_t color,
                                float localX0 = 0.0f, float localY0 = 0.0f,
                                float localX1 = 0.0f, float localY1 = 0.0f,
                                bool reprojectable = false)
{
	// All-float with a single reciprocal (was double sqrt + 4 double divides per
	// band; called ~144x/frame for the cube's two rings). The Xtensa LX7 has no
	// hardware double and no divide, so this was a heavy chunk of the ring record.
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float len2 = dx * dx + dy * dy;
	if (len2 < 1e-6f) return;
	const float inv = 1.0f / std::sqrt(len2);

	const float half = width * 0.5f;
	const float nx = -dy * half * inv;
	const float ny = dx * half * inv;
	const float ex = dx * 0.75f * inv;
	const float ey = dy * 0.75f * inv;
	int16_t xs[4] = {
	    static_cast<int16_t>(std::lroundf(x0 - ex + nx)),
	    static_cast<int16_t>(std::lroundf(x1 + ex + nx)),
	    static_cast<int16_t>(std::lroundf(x1 + ex - nx)),
	    static_cast<int16_t>(std::lroundf(x0 - ex - nx)),
	};
	int16_t ys[4] = {
	    static_cast<int16_t>(std::lroundf(y0 - ey + ny)),
	    static_cast<int16_t>(std::lroundf(y1 + ey + ny)),
	    static_cast<int16_t>(std::lroundf(y1 + ey - ny)),
	    static_cast<int16_t>(std::lroundf(y0 - ey - ny)),
	};
	int bx0, by0, bx1, by1;
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
	if (reprojectable)
		appendFillQuadStrokeSegmentRaw(xs, ys, color, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
		                               localX0, localY0, localX1, localY1, width);
	else
		appendFillQuadRaw(xs, ys, color, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

bool gradientAlphaNearlyConstant(const Node &node)
{
	uint8_t minAlpha = std::min(rstyle(node.style).bg_gradient_from_alpha, rstyle(node.style).bg_gradient_to_alpha);
	uint8_t maxAlpha = std::max(rstyle(node.style).bg_gradient_from_alpha, rstyle(node.style).bg_gradient_to_alpha);
	if (rstyle(node.style).bg_gradient_has_mid) {
		minAlpha = std::min(minAlpha, rstyle(node.style).bg_gradient_mid_alpha);
		maxAlpha = std::max(maxAlpha, rstyle(node.style).bg_gradient_mid_alpha);
	}
	return maxAlpha - minAlpha <= 8;
}

bool hasAnyRadius(const Node &node)
{
	return node.style.border_radius[0] || node.style.border_radius[1] ||
	       node.style.border_radius[2] || node.style.border_radius[3] ||
	       node.style.border_radius_percent[0] != kUnset ||
	       node.style.border_radius_percent[1] != kUnset ||
	       node.style.border_radius_percent[2] != kUnset ||
	       node.style.border_radius_percent[3] != kUnset;
}

bool hasAnyPercentRadius(const Node &node)
{
	return node.style.border_radius_percent[0] != kUnset ||
	       node.style.border_radius_percent[1] != kUnset ||
	       node.style.border_radius_percent[2] != kUnset ||
	       node.style.border_radius_percent[3] != kUnset;
}

int integerSqrt(int n)
{
	if (n <= 0) return 0;
	int x = n;
	int y = (x + 1) / 2;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2;
	}
	return x;
}

int normalizedRadius(int radius, int maxRadius)
{
	if (radius < 0) return 0;
	return radius > maxRadius ? maxRadius : radius;
}

void roundedNodeRowSpan(const Node &node, int y, int *x0, int *x1)
{
	const int maxRadius = std::min(node.layout.width / 2, node.layout.height / 2);
	const int tl = normalizedRadius(node.style.border_radius[0], maxRadius);
	const int tr = normalizedRadius(node.style.border_radius[1], maxRadius);
	const int br = normalizedRadius(node.style.border_radius[2], maxRadius);
	const int bl = normalizedRadius(node.style.border_radius[3], maxRadius);
	if ((tl | tr | br | bl) == 0) return;

	const int left = node.layout.x;
	const int right = node.layout.x + node.layout.width - 1;
	const int top = node.layout.y;
	const int bottom = node.layout.y + node.layout.height - 1;
	int rowLeft = left;
	int rowRight = right;

	if (tl > 0 && y >= top && y <= top + tl) {
		const int dy = top + tl - y;
		const int dx = integerSqrt(tl * tl - dy * dy);
		rowLeft = std::max(rowLeft, left + tl - dx);
	}
	if (bl > 0 && y >= bottom - bl && y <= bottom) {
		const int dy = y - (bottom - bl);
		const int dx = integerSqrt(bl * bl - dy * dy);
		rowLeft = std::max(rowLeft, left + bl - dx);
	}
	if (tr > 0 && y >= top && y <= top + tr) {
		const int dy = top + tr - y;
		const int dx = integerSqrt(tr * tr - dy * dy);
		rowRight = std::min(rowRight, right - tr + dx);
	}
	if (br > 0 && y >= bottom - br && y <= bottom) {
		const int dy = y - (bottom - br);
		const int dx = integerSqrt(br * br - dy * dy);
		rowRight = std::min(rowRight, right - br + dx);
	}

	*x0 = std::max(*x0, rowLeft);
	*x1 = std::min(*x1, rowRight);
}

bool isFullyRoundedShape(const Node &node)
{
	if (hasAnyPercentRadius(node)) return false;
	if (std::abs(node.layout.width - node.layout.height) > 1) return false;
	const int halfMin = std::min(node.layout.width, node.layout.height) / 2;
	if (halfMin <= 0) return false;
	const int minRadius = std::max(1, halfMin - 1);
	return node.style.border_radius[0] >= minRadius &&
	       node.style.border_radius[1] >= minRadius &&
	       node.style.border_radius[2] >= minRadius &&
	       node.style.border_radius[3] >= minRadius;
}

void resolvedBorderRadii8(const Node &node, int16_t rx8[4], int16_t ry8[4])
{
	const double width = static_cast<double>(std::max(0, static_cast<int>(node.layout.width)));
	const double height = static_cast<double>(std::max(0, static_cast<int>(node.layout.height)));
	double rx[4]{};
	double ry[4]{};
	for (int i = 0; i < 4; i++) {
		if (node.style.border_radius_percent[i] != kUnset) {
			const double p = static_cast<double>(node.style.border_radius_percent[i]) / 1000.0;
			rx[i] = std::max(0.0, width * p);
			ry[i] = std::max(0.0, height * p);
		} else {
			const double r = static_cast<double>(std::max(0, static_cast<int>(node.style.border_radius[i])));
			rx[i] = r;
			ry[i] = r;
		}
	}

	double scale = 1.0;
	const auto constrain = [&](double limit, double sum) {
		if (limit > 0.0 && sum > limit) scale = std::min(scale, limit / sum);
	};
	constrain(width, rx[0] + rx[1]);
	constrain(width, rx[3] + rx[2]);
	constrain(height, ry[0] + ry[3]);
	constrain(height, ry[1] + ry[2]);

	for (int i = 0; i < 4; i++) {
		rx8[i] = static_cast<int16_t>(std::max(0L, std::min(32767L, std::lround(rx[i] * scale * 8.0))));
		ry8[i] = static_cast<int16_t>(std::max(0L, std::min(32767L, std::lround(ry[i] * scale * 8.0))));
	}
}

bool resolvedCircularBorderRadii(const Node &node, int radii[4])
{
	int16_t rx8[4]{};
	int16_t ry8[4]{};
	resolvedBorderRadii8(node, rx8, ry8);
	for (int i = 0; i < 4; i++) {
		if (std::abs(static_cast<int>(rx8[i]) - static_cast<int>(ry8[i])) > 4) return false;
		const int radius8 = (static_cast<int>(rx8[i]) + static_cast<int>(ry8[i])) / 2;
		if (radius8 > std::min(node.layout.width, node.layout.height) * 4) return false;
		radii[i] = std::max(0, std::min(32767, (radius8 + 4) / 8));
	}
	return true;
}

void appendFillRoundedRectWithAlpha(const Node &node, uint8_t parentAlpha, const int *resolvedRadii = nullptr)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.bg_alpha);
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(effectiveAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		const int tl = resolvedRadii ? resolvedRadii[0] : node.style.border_radius[0];
		const int tr = resolvedRadii ? resolvedRadii[1] : node.style.border_radius[1];
		const int br = resolvedRadii ? resolvedRadii[2] : node.style.border_radius[2];
		const int bl = resolvedRadii ? resolvedRadii[3] : node.style.border_radius[3];
		cmd->type = DisplayCommandType::FillRoundedRect;
		cmd->bx = node.layout.x; cmd->by = node.layout.y; cmd->bw = node.layout.width; cmd->bh = node.layout.height;
		cmd->fillRoundedRect.x = node.layout.x; cmd->fillRoundedRect.y = node.layout.y;
		cmd->fillRoundedRect.w = node.layout.width; cmd->fillRoundedRect.h = node.layout.height;
		cmd->fillRoundedRect.tl = tl;
		cmd->fillRoundedRect.tr = tr;
		cmd->fillRoundedRect.br = br;
		cmd->fillRoundedRect.bl = bl;
		cmd->fillRoundedRect.color = node.style.bg_color;
	}
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(parentAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
}

bool appendResolvedCircularRoundedRectWithAlpha(const Node &node, uint8_t parentAlpha)
{
	int radii[4]{};
	if (!resolvedCircularBorderRadii(node, radii)) return false;
	appendFillRoundedRectWithAlpha(node, parentAlpha, radii);
	return true;
}

bool resolvedRadiiAreCircleLike(int w, int h, const int16_t rx8[4], const int16_t ry8[4])
{
	if (w <= 0 || h <= 0 || std::abs(w - h) > 1)
		return false;
	const int targetRx8 = w * 4;
	const int targetRy8 = h * 4;
	for (int i = 0; i < 4; i++) {
		if (std::abs(static_cast<int>(rx8[i]) - targetRx8) > 8)
			return false;
		if (std::abs(static_cast<int>(ry8[i]) - targetRy8) > 8)
			return false;
	}
	return true;
}

bool transformedCornersCircle(const int16_t xs[4], const int16_t ys[4], int *cx, int *cy, int *r)
{
	if (!cx || !cy || !r)
		return false;
	if (std::abs(static_cast<int>(xs[0]) - static_cast<int>(xs[3])) > 1 ||
	    std::abs(static_cast<int>(xs[1]) - static_cast<int>(xs[2])) > 1 ||
	    std::abs(static_cast<int>(ys[0]) - static_cast<int>(ys[1])) > 1 ||
	    std::abs(static_cast<int>(ys[3]) - static_cast<int>(ys[2])) > 1)
		return false;

	const float left = 0.5f * static_cast<float>(static_cast<int>(xs[0]) + static_cast<int>(xs[3]));
	const float right = 0.5f * static_cast<float>(static_cast<int>(xs[1]) + static_cast<int>(xs[2]));
	const float top = 0.5f * static_cast<float>(static_cast<int>(ys[0]) + static_cast<int>(ys[1]));
	const float bottom = 0.5f * static_cast<float>(static_cast<int>(ys[2]) + static_cast<int>(ys[3]));
	const float width = right - left;
	const float height = bottom - top;
	if (width <= 0.0f || height <= 0.0f || std::fabs(width - height) > 1.5f)
		return false;

	const int radius = static_cast<int>(std::lround(std::min(width, height) * 0.5f));
	if (radius <= 0)
		return false;
	*cx = static_cast<int>(std::lround((left + right) * 0.5f));
	*cy = static_cast<int>(std::lround((top + bottom) * 0.5f));
	*r = radius;
	return true;
}

void appendFillCircleWithAlpha(int cx, int cy, int r, gea::framework::graphics::pixel::native_t color, uint8_t effectiveAlpha, uint8_t parentAlpha)
{
	if (r <= 0)
		return;
	const int x = cx - r;
	const int y = cy - r;
	const int d = r * 2 + 1;
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(effectiveAlpha, x, y, d, d);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillCircle;
		cmd->bx = static_cast<int16_t>(x);
		cmd->by = static_cast<int16_t>(y);
		cmd->bw = static_cast<int16_t>(d);
		cmd->bh = static_cast<int16_t>(d);
		cmd->fillCircle.cx = static_cast<int16_t>(cx);
		cmd->fillCircle.cy = static_cast<int16_t>(cy);
		cmd->fillCircle.r = static_cast<int16_t>(r);
		cmd->fillCircle.color = color;
	}
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(parentAlpha, x, y, d, d);
}

void appendStrokeWithAlpha(const Node &node, uint8_t parentAlpha)
{
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, borderPaintAlpha(node.style, 0));
	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(effectiveAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);

	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		// Canvas::strokeRect is deliberately the fast one-pixel primitive. Route
		// wider square CSS borders through the width-aware rounded-rect rasterizer
		// with zero radii instead of silently collapsing border-width to 1px.
		if (hasAnyRadius(node) || node.style.border_width > 1) {
			cmd->type = DisplayCommandType::StrokeRoundedRect;
			cmd->strokeRoundedRect = {};
			cmd->bx = node.layout.x; cmd->by = node.layout.y; cmd->bw = node.layout.width; cmd->bh = node.layout.height;
			cmd->strokeRoundedRect.x = node.layout.x; cmd->strokeRoundedRect.y = node.layout.y;
			cmd->strokeRoundedRect.w = node.layout.width; cmd->strokeRoundedRect.h = node.layout.height;
			cmd->strokeRoundedRect.tl = node.style.border_radius[0];
			cmd->strokeRoundedRect.tr = node.style.border_radius[1];
			cmd->strokeRoundedRect.br = node.style.border_radius[2];
			cmd->strokeRoundedRect.bl = node.style.border_radius[3];
			cmd->strokeRoundedRect.lineWidth = node.style.border_width;
			resolvedBorderRadii8(node, cmd->strokeRoundedRect.rx8, cmd->strokeRoundedRect.ry8);
			cmd->strokeRoundedRect.cssRadii = hasAnyPercentRadius(node);
			for (int i = 0; i < 4; ++i)
				if (cmd->strokeRoundedRect.rx8[i] > std::min(node.layout.width, node.layout.height) * 4 ||
				    cmd->strokeRoundedRect.ry8[i] > std::min(node.layout.width, node.layout.height) * 4)
					cmd->strokeRoundedRect.cssRadii = 1;
			cmd->strokeRoundedRect.color = borderPaintColor(node.style, 0);
		} else {
			cmd->type = DisplayCommandType::StrokeRect;
			cmd->bx = node.layout.x; cmd->by = node.layout.y; cmd->bw = node.layout.width; cmd->bh = node.layout.height;
			cmd->stroke.x = node.layout.x; cmd->stroke.y = node.layout.y;
			cmd->stroke.w = node.layout.width; cmd->stroke.h = node.layout.height;
			cmd->stroke.color = borderPaintColor(node.style, 0);
		}
	}

	if (effectiveAlpha != parentAlpha)
		appendAlphaCommand(parentAlpha, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
}

void boundsFromCorners(const int16_t *xs, const int16_t *ys, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = *x1 = xs[0];
	*y0 = *y1 = ys[0];
	for (int i = 1; i < 4; i++) {
		if (xs[i] < *x0) *x0 = xs[i];
		if (xs[i] > *x1) *x1 = xs[i];
		if (ys[i] < *y0) *y0 = ys[i];
		if (ys[i] > *y1) *y1 = ys[i];
	}
}

class ViewGeometry {
public:
	// float (single-precision) so the per-corner transform math runs on the
	// ESP32-S3 hardware FPU. double here is software-emulated and was ~60% of a
	// spinning css-3d-cube frame (619ms of dlist record). float is sub-pixel
	// accurate for screen coords (<= ~512px) through the short transform chain.
	struct Point3 {
		float x;
		float y;
		float z;
	};

	static int nodeIndex(const Node &node)
	{
		const Node *nodes = Tree::instance().nodes();
		const auto index = &node - nodes;
		return index >= 0 && index < Tree::instance().nodeCount() ? static_cast<int>(index) : -1;
	}

	// CSS Transforms applies to transformable boxes, which excludes non-replaced
	// inline-level boxes. The layout classifier also includes inline replaced
	// elements such as images, so retain those while excluding ordinary inline
	// wrappers and text nodes.
	static bool hasTransformableBox(const Node &node)
	{
		if (node.style.display == kDisplayNone) return false;
		if (node.type == NodeType::Text) return false;
		if (node.type == NodeType::Image) return true;
		return !LayoutEngine::isCssInlineLevelBox(node);
	}

	static bool nodeHasLocalTransform(const Node &node, bool usePrevious)
	{
		if (usePrevious ? !node.render.previous_transformable_box : !hasTransformableBox(node)) return false;
		const RareStyle &rs = rstyle(node.style); // one pool lookup, not 10
		const int rotate = usePrevious ? node.render.previous_transform_rotate : rs.transform_rotate;
		const int rotateX = usePrevious ? node.render.previous_transform_rotate_x : rs.transform_rotate_x;
		const int rotateY = usePrevious ? node.render.previous_transform_rotate_y : rs.transform_rotate_y;
		const int tx = usePrevious ? node.render.previous_transform_translate_x : composedTranslateX(rs);
		const int ty = usePrevious ? node.render.previous_transform_translate_y : composedTranslateY(rs);
		const int tz = usePrevious ? node.render.previous_transform_translate_z : composedTranslateZ(rs);
		const int txPercent = usePrevious ? node.render.previous_transform_translate_x_percent : composedTranslateXPercent(rs);
		const int tyPercent = usePrevious ? node.render.previous_transform_translate_y_percent : composedTranslateYPercent(rs);
		const int sx = usePrevious ? node.render.previous_transform_scale_x : rs.transform_scale_x;
		const int sy = usePrevious ? node.render.previous_transform_scale_y : rs.transform_scale_y;
		const int sz = usePrevious ? node.render.previous_transform_scale_z : rs.transform_scale_z;
		return (usePrevious ? hadIndividualLinearTransform(node.render) : hasIndividualLinearTransform(rs)) ||
		       (rotate % 3600) != 0 ||
		       (rotateX % 3600) != 0 ||
		       (rotateY % 3600) != 0 ||
		       tx != 0 ||
		       ty != 0 ||
		       tz != 0 ||
		       txPercent != 0 ||
		       tyPercent != 0 ||
		       sx != 1000 ||
		       sy != 1000 ||
		       sz != 1000;
	}

	// True iff any node carries a non-identity transform/perspective this frame
	// or last frame. Cached per refresh (keyed on refreshSerial): the DOM doesn't
	// change between refreshes, so one O(n) scan replaces the ancestor-chain walk
	// that hasTransformChain() would otherwise run for every node, every
	// recordBox. When no transform exists anywhere — the overwhelmingly common
	// case (e.g. tilt-breakout: no transforms at all) — the entire keyframe-3D
	// record path is bypassed at O(1).
	static bool GEA_VIEW_HOT_SRAM_SECTION("any_transform_present") anyTransformPresent()
	{
		auto &state = treeState();
		if (state.transformScanSerial == state.refreshSerial) return state.transformPresent;
		// Durable no-transform cache: a transform-free tree stays transform-free until a
		// transform property is set, and both setStyle(transform) sites clear
		// transformScanValid. So a cached `false` holds across mid-refresh refreshSerial
		// bumps with no re-scan — eliminating the scan-per-phase that made the mode gate
		// regress. (A cached `true` is intentionally not held: re-scan per refreshSerial
		// so a transform removal is noticed.)
		if (state.transformScanValid && !state.transformPresent) return false;
		bool present = false;
		for (int i = 0; i < state.nodeCount; i++) {
			const Node &n = state.nodes[i];
			if (nodeHasLocalTransform(n, false) || nodeHasLocalTransform(n, true) ||
			    (hasTransformableBox(n) && rstyle(n.style).perspective > 0) ||
			    (n.render.previous_transformable_box && n.render.previous_perspective > 0)) {
				present = true;
				break;
			}
		}
		state.transformPresent = present;
		state.transformScanSerial = state.refreshSerial;
		state.transformScanValid = true;
		return present;
	}

	static bool GEA_VIEW_HOT_SRAM_SECTION("has_transform_chain") hasTransformChain(const Node &node, bool usePrevious)
	{
		if (!anyTransformPresent()) return false;
		auto &state = treeState();
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (nodeHasLocalTransform(state.nodes[id], usePrevious)) return true;
			if (usePrevious ? !state.nodes[id].render.previous_transformable_box : !hasTransformableBox(state.nodes[id])) continue;
			const int perspective = usePrevious ? state.nodes[id].render.previous_perspective : rstyle(state.nodes[id].style).perspective;
			if (perspective > 0) return true;
		}
		return false;
	}

	// Rotations take precomputed sin/cos (cached per node per frame by
	// nodeRotation) so applyNodeTransform doesn't re-evaluate software sin/cos
	// per corner per gradient strip — the dominant cost of rebuilding a
	// transformed subtree's display list each frame.
	static void rotateX(Point3 &p, float s, float c)
	{
		const float y = p.y * c - p.z * s;
		const float z = p.y * s + p.z * c;
		p.y = y;
		p.z = z;
	}

	static void rotateY(Point3 &p, float s, float c)
	{
		const float x = p.x * c + p.z * s;
		const float z = -p.x * s + p.z * c;
		p.x = x;
		p.z = z;
	}

	static void rotateZ(Point3 &p, float s, float c)
	{
		const float x = p.x * c - p.y * s;
		const float y = p.x * s + p.y * c;
		p.x = x;
		p.y = y;
	}

	// Full per-node affine transform for the current frame, precomputed once and
	// reused across every corner/strip projected through it.
	struct NodeTransformCache {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
		// Not inlined: the -1 node id would make the cache a .data object (see
		// state_init.h).
		__attribute__((noinline)) NodeTransformCache() {}
#endif
		int nodeId = -1;
		int x = 0, y = 0, w = 0, h = 0;
		float ox = 0.0f, oy = 0.0f;
		float transX = 0.0f, transY = 0.0f, transZ = 0.0f;
		float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		bool prevValid = false;
		float pOx = 0.0f, pOy = 0.0f;
		float pTransX = 0.0f, pTransY = 0.0f, pTransZ = 0.0f;
		float previousMatrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	};

	struct AverageDepthCacheEntry {
		int nodeId = -1;
		int depth = 0;
	};

	struct CornerTransformCacheEntry {
		int nodeId = -1;
		int16_t xs[4]{};
		int16_t ys[4]{};
	};

#ifndef GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS
#define GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS 64
#endif
#ifndef GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS
#define GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS 64
#endif
#ifndef GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS
#define GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS 64
#endif
	static constexpr int kActiveTransformCacheSlots = GEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS;
	static constexpr int kActiveDepthCacheSlots = GEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS;
	static constexpr int kActiveCornerCacheSlots = GEA_EMBEDDED_UI_CORNER_CACHE_SLOTS;
	static_assert(kActiveTransformCacheSlots > 0);
	static_assert(kActiveDepthCacheSlots > 0);
	static_assert(kActiveCornerCacheSlots > 0);
	static_assert((kActiveTransformCacheSlots & (kActiveTransformCacheSlots - 1)) == 0);
	static_assert((kActiveDepthCacheSlots & (kActiveDepthCacheSlots - 1)) == 0);
	static_assert((kActiveCornerCacheSlots & (kActiveCornerCacheSlots - 1)) == 0);

	struct ActiveTransformCache {
#if GEA_EMBEDDED_UI_STATE_DYNAMIC_INIT
		// Not inlined: the all-ones serial would make the cache a .data object
		// (see state_init.h).
		__attribute__((noinline)) ActiveTransformCache() {}
#endif
		uint64_t serial = ~0ull;
		NodeTransformCache entries[kActiveTransformCacheSlots]{};
	};

	struct ActiveDepthCache {
		uint64_t serial = ~0ull;
		AverageDepthCacheEntry entries[kActiveDepthCacheSlots]{};
	};

	struct ActiveCornerCache {
		uint64_t serial = ~0ull;
		CornerTransformCacheEntry entries[kActiveCornerCacheSlots]{};
	};

	static void resetCache(NodeTransformCache *entries, int count)
	{
		for (int i = 0; i < count; i++) entries[i].nodeId = -1;
	}

	static void resetCache(AverageDepthCacheEntry *entries, int count)
	{
		for (int i = 0; i < count; i++) entries[i].nodeId = -1;
	}

	static void resetCache(CornerTransformCacheEntry *entries, int count)
	{
		for (int i = 0; i < count; i++) entries[i].nodeId = -1;
	}

	static Point3 applyLinear(const float *m, Point3 p)
	{
		return {m[0] * p.x + m[1] * p.y + m[2] * p.z,
		        m[3] * p.x + m[4] * p.y + m[5] * p.z,
		        m[6] * p.x + m[7] * p.y + m[8] * p.z};
	}

	static void fillLinear(float *m, int rx, int ry, int rz, int sx, int sy, int sz = 1000)
	{
		const float ax = rx * kPiF * kInv1800f, ay = ry * kPiF * kInv1800f, az = rz * kPiF * kInv1800f;
		const float sinX = sinf(ax), cosX = cosf(ax), sinY = sinf(ay), cosY = cosf(ay), sinZ = sinf(az), cosZ = cosf(az);
		for (int column = 0; column < 3; ++column) {
			Point3 basis{column == 0 ? sx * kInv1000f : 0.0f, column == 1 ? sy * kInv1000f : 0.0f, column == 2 ? sz * kInv1000f : 0.0f};
			rotateZ(basis, sinZ, cosZ); rotateY(basis, sinY, cosY); rotateX(basis, sinX, cosX);
			m[column] = basis.x; m[3 + column] = basis.y; m[6 + column] = basis.z;
		}
	}

	static Point3 listTranslation(Point3 shift, unsigned outerAxes, int rx, int ry, int rz)
	{
		Point3 inner{(outerAxes & 1) ? 0.0f : shift.x, (outerAxes & 2) ? 0.0f : shift.y, (outerAxes & 4) ? 0.0f : shift.z};
		float rotation[9]; fillLinear(rotation, rx, ry, rz, 1000, 1000);
		inner = applyLinear(rotation, inner);
		return {inner.x + ((outerAxes & 1) ? shift.x : 0.0f), inner.y + ((outerAxes & 2) ? shift.y : 0.0f), inner.z + ((outerAxes & 4) ? shift.z : 0.0f)};
	}

	// CSS order is individual translate, rotate, scale, then the transform list.
	// Left-multiply the list's linear map and offset once when filling the cache;
	// per-point projection then needs only one affine matrix application.
	static void composeIndividual(float *matrix, Point3 &shift, int angle, int axisX, int axisY, int axisZ,
	                              int sx, int sy, int sz, Point3 translation)
	{
		float x = static_cast<float>(axisX), y = static_cast<float>(axisY), z = static_cast<float>(axisZ);
		const float length = sqrtf(x*x + y*y + z*z);
		float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		if (length > 0 && angle % 3600 != 0) {
			x /= length; y /= length; z /= length;
			const float a = angle * kPiF * kInv1800f, c = cosf(a), sine = sinf(a), t = 1 - c;
			r[0] = c+x*x*t; r[1] = x*y*t-z*sine; r[2] = x*z*t+y*sine;
			r[3] = y*x*t+z*sine; r[4] = c+y*y*t; r[5] = y*z*t-x*sine;
			r[6] = z*x*t-y*sine; r[7] = z*y*t+x*sine; r[8] = c+z*z*t;
		}
		for (int row = 0; row < 3; ++row) { r[row*3] *= sx*kInv1000f; r[row*3+1] *= sy*kInv1000f; r[row*3+2] *= sz*kInv1000f; }
		for (int column = 0; column < 3; ++column) {
			const auto v = applyLinear(r, {matrix[column], matrix[3+column], matrix[6+column]});
			matrix[column] = v.x; matrix[3+column] = v.y; matrix[6+column] = v.z;
		}
		shift = applyLinear(r, shift);
		shift.x += translation.x; shift.y += translation.y; shift.z += translation.z;
	}

	static void fillNodeTransform(NodeTransformCache &e, const Node &node, int id)
	{
		const RareStyle &r = rstyle(node.style);
		e.x = node.layout.x; e.y = node.layout.y; e.w = node.layout.width; e.h = node.layout.height;
		e.ox = e.x + e.w * r.transform_origin_x * kInv1000f;
		e.oy = e.y + e.h * r.transform_origin_y * kInv1000f;
		if (!hasTransformableBox(node)) {
			std::fill(std::begin(e.matrix), std::end(e.matrix), 0.0f);
			e.matrix[0] = e.matrix[4] = e.matrix[8] = 1.0f;
			e.transX = e.transY = e.transZ = 0.0f;
			e.prevValid = false; e.nodeId = id;
			return;
		}
		fillLinear(e.matrix, r.transform_rotate_x, r.transform_rotate_y, r.transform_rotate,
		           r.transform_scale_x, r.transform_scale_y, r.transform_scale_z);
		auto shift = listTranslation({r.transform_translate_x + e.w*r.transform_translate_x_percent*kInv1000f,
		    r.transform_translate_y + e.h*r.transform_translate_y_percent*kInv1000f, static_cast<float>(r.transform_translate_z)},
		    r.transform_translate_outer_axes, r.transform_rotate_x, r.transform_rotate_y, r.transform_rotate);
		composeIndividual(e.matrix, shift, r.rotate_angle, r.rotate_axis_x, r.rotate_axis_y, r.rotate_axis_z,
		    r.scale_x, r.scale_y, r.scale_z,
		    {r.translate_x + e.w*r.translate_x_percent*kInv1000f, r.translate_y + e.h*r.translate_y_percent*kInv1000f, static_cast<float>(r.translate_z)});
		e.transX = shift.x; e.transY = shift.y; e.transZ = shift.z;
		e.prevValid = false; e.nodeId = id;
	}

	static void fillNodeTransformPrev(NodeTransformCache &e, const Node &node)
	{
		const auto &r = node.render;
		if (!r.previous_transformable_box) {
			e.pOx = e.pOy = e.pTransX = e.pTransY = e.pTransZ = 0.0f;
			std::fill(std::begin(e.previousMatrix), std::end(e.previousMatrix), 0.0f);
			e.previousMatrix[0] = e.previousMatrix[4] = e.previousMatrix[8] = 1.0f;
			return;
		}
		const float w = node.layout.previous_width, h = node.layout.previous_height;
		e.pOx = node.layout.previous_x + w*r.previous_transform_origin_x*kInv1000f;
		e.pOy = node.layout.previous_y + h*r.previous_transform_origin_y*kInv1000f;
		fillLinear(e.previousMatrix, r.previous_transform_rotate_x, r.previous_transform_rotate_y, r.previous_transform_rotate,
		    r.previous_transform_scale_x, r.previous_transform_scale_y, r.previous_transform_scale_z);
		auto shift = listTranslation({r.previous_transform_translate_x - r.previous_translate_x + w*(r.previous_transform_translate_x_percent-r.previous_translate_x_percent)*kInv1000f,
		    r.previous_transform_translate_y - r.previous_translate_y + h*(r.previous_transform_translate_y_percent-r.previous_translate_y_percent)*kInv1000f,
		    static_cast<float>(r.previous_transform_translate_z-r.previous_translate_z)}, r.previous_transform_translate_outer_axes,
		    r.previous_transform_rotate_x, r.previous_transform_rotate_y, r.previous_transform_rotate);
		composeIndividual(e.previousMatrix, shift, r.previous_rotate_angle, r.previous_rotate_axis_x, r.previous_rotate_axis_y, r.previous_rotate_axis_z,
		    r.previous_scale_x, r.previous_scale_y, r.previous_scale_z,
		    {r.previous_translate_x + w*r.previous_translate_x_percent*kInv1000f, r.previous_translate_y + h*r.previous_translate_y_percent*kInv1000f, static_cast<float>(r.previous_translate_z)});
		e.pTransX = shift.x; e.pTransY = shift.y; e.pTransZ = shift.z;
	}

	static NodeTransformCache *cachedNodeTransform(const Node &node)
	{
		static ActiveTransformCache cache;
		const int id = nodeIndex(node);
		if (id < 0) return nullptr;
		const uint64_t serial = treeState().refreshSerial;
		if (cache.serial != serial) {
			resetCache(cache.entries, kActiveTransformCacheSlots);
			cache.serial = serial;
		}
		NodeTransformCache &e = cache.entries[id & (kActiveTransformCacheSlots - 1)];
		// The cache was reset above on a serial change, and the STYLE half of a
		// transform is fixed within a refresh serial (the serial bumps whenever the
		// tree re-records / a style mutates). So a matching id lets us skip the
		// per-call nodeTransformMatches style comparison (an rstyle() pool lookup + 16
		// field compares against the scattered RareStyle entry) that the reproject
		// otherwise pays ~72x/frame (12 commands x ~6 ancestors) — the dominant
		// residual reproject cost vs spine's inline-ComputedStyle reads.
		//
		// The LAYOUT half is not covered by that argument: transform-origin and
		// percentage translate are resolved against the node's own box, and layout can
		// change without bumping the refresh serial (layout runs inside a refresh, and
		// callers such as hit-testing read geometry between refreshes). A stale ox/oy/
		// transX/transY silently projects the current rect through the PREVIOUS box's
		// coefficients — e.g. translate(-50%,-50%) of a 480x16 button still shifting by
		// (-240,-8) after the button became 47x47, so hit-testing answered for a rect
		// that is nowhere on screen. Four int compares against fields the caller has
		// already touched (node.layout is hot) is cheap next to the deque probe, and it
		// keeps the fast path honest.
		if (e.nodeId == id) {
			if (e.x == node.layout.x && e.y == node.layout.y &&
			    e.w == node.layout.width && e.h == node.layout.height)
				return &e;
			fillNodeTransform(e, node, id);
			return &e;
		}
		if (e.nodeId != -1) return nullptr;
		fillNodeTransform(e, node, id);
		return &e;
	}

	// Current-frame transform coefficients for a node, computed once per refresh
	// and reused across every corner/strip projected through it.
	// transformRectCorners runs 4 corners x N strips per node (e.g. 4x72 for a
	// gradient face), and EACH applyNodeTransform previously recomputed the origin/
	// scale/translate (6 software __divsf3 — the ESP32-S3 FPU has no divide) plus 3
	// software sin/cos. That per-call arithmetic was the dominant cost of rebuilding
	// a transformed subtree's display list (measured ~38ms / ~2300 calls per spinning
	// css-3d-cube frame). Precomputing it per node collapses the hot path to ~12
	// hardware FPU mul/adds + a cache lookup. Keyed on refreshSerial: the transform
	// is fixed within a refresh, and the serial bumps when the tree re-records (the
	// animating cube each frame). Only the current frame is cached; the colder
	// previous-frame (dirty-bounds) path computes directly.
	static const NodeTransformCache &nodeTransform(const Node &node)
	{
		if (NodeTransformCache *e = cachedNodeTransform(node)) return *e;
		static NodeTransformCache fallback;
		fillNodeTransform(fallback, node, nodeIndex(node));
		return fallback;
	}

	// Pure hardware-FPU application of precomputed transform coefficients to a
	// point — no cache lookup, no divides, no trig. transformRectCorners hoists
	// the per-node cache lookup out of its 4-corner loop and calls this directly.
	static void applyCachedTransform(const NodeTransformCache &t, Point3 &p)
	{
		p.x -= t.ox; p.y -= t.oy;
		p = applyLinear(t.matrix, p);
		p.x += t.transX + t.ox; p.y += t.transY + t.oy; p.z += t.transZ;
	}

	// Facing of a plane within its own CSS 3D rendering context.
	static bool backFacing(const Node &node)
	{
		auto &state = treeState();
		Point3 normal{0, 0, 1};
		float offset = 0;
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount;) {
			const auto &t = nodeTransform(state.nodes[id]);
			const float *m = t.matrix;
			const float cofactors[9] = {
			    m[4]*m[8]-m[5]*m[7], m[5]*m[6]-m[3]*m[8], m[3]*m[7]-m[4]*m[6],
			    m[2]*m[7]-m[1]*m[8], m[0]*m[8]-m[2]*m[6], m[1]*m[6]-m[0]*m[7],
			    m[1]*m[5]-m[2]*m[4], m[2]*m[3]-m[0]*m[5], m[0]*m[4]-m[1]*m[3]};
			const float determinant = m[0]*cofactors[0] + m[1]*cofactors[1] + m[2]*cofactors[2];
			if (determinant == 0) return true;
			offset += normal.x*t.ox + normal.y*t.oy;
			normal = applyLinear(cofactors, normal);
			normal.x /= determinant; normal.y /= determinant; normal.z /= determinant;
			offset -= normal.x*(t.transX+t.ox) + normal.y*(t.transY+t.oy) + normal.z*t.transZ;
			id = state.nodes[id].parent;
			if (id < 0 || id >= state.nodeCount) break;
			const auto &parent = state.nodes[id];
			const auto &rare = rstyle(parent.style);
			if (hasTransformableBox(parent) && rare.perspective > 0) {
				const float ox = parent.layout.x + parent.layout.width * rare.perspective_origin_x * 0.001f;
				const float oy = parent.layout.y + parent.layout.height * rare.perspective_origin_y * 0.001f;
				normal.z += (offset + normal.x * ox + normal.y * oy) / rare.perspective;
			}
			if (!hasTransformableBox(parent) || !preserves3D(parent.style)) break;
		}
		return normal.z < -0.00001f;
	}

	// Previous-frame analogue of applyCachedTransform — pure-FPU application of the
	// lazily-cached previous-frame coefficients. Identical math to applyNodeTransform's
	// usePrevious branch, so the projected corner is bit-identical (no visual change).
	static void applyCachedTransformPrev(const NodeTransformCache &t, Point3 &p)
	{
		p.x -= t.pOx; p.y -= t.pOy;
		p = applyLinear(t.previousMatrix, p);
		p.x += t.pTransX + t.pOx; p.y += t.pTransY + t.pOy; p.z += t.pTransZ;
	}

	// Gather the ancestor chain's cached transform coefficients for the current
	// frame, leaf-first. Returns the chain length, or -1 if it exceeds `cap` (the
	// caller then falls back to the per-ancestor walk). Hoisting this out of a
	// multi-point projection loop turns N points x chain cache-lookups into one
	// chain gather + pure-FPU application per point.
	static constexpr int kMaxChainDepth = 64;  // DOM nesting is shallow; far beyond any real tree
	static int GEA_VIEW_HOT_SRAM_SECTION("gather_transform_chain") gatherTransformChain(const Node &node, const NodeTransformCache **chain)
	{
		auto &state = treeState();
		int depth = 0;
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (depth >= kMaxChainDepth) return -1;
			NodeTransformCache *entry = cachedNodeTransform(state.nodes[id]);
			if (!entry) return -1;
			chain[depth++] = entry;
		}
		return depth;
	}

	// Previous-frame analogue of gatherTransformChain: gathers the same ancestor
	// chain, lazily filling each node's previous-frame coefficients on first access.
	// The dirty-bounds pass projects previous-frame corners through this so the
	// previous-frame projection is O(nodes) cached pure-FPU — matching the current
	// frame — instead of re-walking + recomputing 6 software trig per corner.
	static int gatherTransformChainPrev(const Node &node, const NodeTransformCache **chain)
	{
		auto &state = treeState();
		int depth = 0;
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (depth >= kMaxChainDepth) return -1;
			NodeTransformCache *entry = cachedNodeTransform(state.nodes[id]);
			if (!entry) return -1;
			if (!entry->prevValid) {
				fillNodeTransformPrev(*entry, state.nodes[id]);
				entry->prevValid = true;
			}
			chain[depth++] = entry;
		}
		return depth;
	}

	static void applyNodeTransform(const Node &node, bool usePrevious, Point3 &p)
	{
		// Hot path (current frame): use the per-node precomputed coefficients so
		// the projection is pure hardware-FPU mul/add — no software divides, no
		// int->float conversions, no per-call trig.
		if (!usePrevious) {
			applyCachedTransform(nodeTransform(node), p);
			return;
		}

		NodeTransformCache previous;
		fillNodeTransformPrev(previous, node);
		applyCachedTransformPrev(previous, p);
	}

	static int nearestPerspectiveNode(const Node &node, bool usePrevious)
	{
		if (!anyTransformPresent()) return -1;
		auto &state = treeState();
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
			if (usePrevious ? !state.nodes[id].render.previous_transformable_box : !hasTransformableBox(state.nodes[id])) continue;
			const int perspective = usePrevious ? state.nodes[id].render.previous_perspective : rstyle(state.nodes[id].style).perspective;
			if (perspective > 0) return id;
		}
		return -1;
	}

	static void applyPerspective(int perspectiveNode, bool usePrevious, Point3 &p)
	{
		if (perspectiveNode < 0) return;
		const Node &node = treeState().nodes[perspectiveNode];
		const int x = usePrevious ? node.layout.previous_x : node.layout.x;
		const int y = usePrevious ? node.layout.previous_y : node.layout.y;
		const int w = usePrevious ? node.layout.previous_width : node.layout.width;
		const int h = usePrevious ? node.layout.previous_height : node.layout.height;
		const RareStyle &rs = rstyle(node.style); // one pool lookup, not 3
		const int perspective = usePrevious ? node.render.previous_perspective : rs.perspective;
		const int originX = usePrevious ? node.render.previous_perspective_origin_x : rs.perspective_origin_x;
		const int originY = usePrevious ? node.render.previous_perspective_origin_y : rs.perspective_origin_y;
		if (perspective <= 0) return;
		const float poX = static_cast<float>(x) + static_cast<float>(w) * static_cast<float>(originX) * kInv1000f;
		const float poY = static_cast<float>(y) + static_cast<float>(h) * static_cast<float>(originY) * kInv1000f;
		float scale = static_cast<float>(perspective) / (static_cast<float>(perspective) - p.z);
		if (!std::isfinite(scale)) scale = 1.0f;
		if (scale < 0.05f) scale = 0.05f;
		if (scale > 8.0f) scale = 8.0f;
		p.x = poX + (p.x - poX) * scale;
		p.y = poY + (p.y - poY) * scale;
	}

	// Optional xs8/ys8 receive the same corners in 1/8-px fixed point (sub-pixel),
	// so adjacent faces' shared edges coincide and the rasterizer tiles them
	// watertight instead of cracking at int16 round-to-nearest straddles. Clamped to
	// int16 range (off-screen corners are clipped by the rasterizer regardless).
	static void GEA_VIEW_HOT_SRAM_SECTION("transform_rect_corners") transformRectCorners(const Node &node, bool usePrevious, int x, int y, int w, int h, int16_t *xs, int16_t *ys,
	                                 int16_t *xs8 = nullptr, int16_t *ys8 = nullptr)
	{
		Point3 points[4] = {
		    {(float)x, (float)y, 0.0f},
		    {(float)(x + w), (float)y, 0.0f},
		    {(float)(x + w), (float)(y + h), 0.0f},
		    {(float)x, (float)(y + h), 0.0f},
		};
		auto fx8 = [](float v) -> int16_t {
			long s = std::lroundf(v * 8.0f);
			return (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
		};
		auto &state = treeState();
		const int perspectiveNode = nearestPerspectiveNode(node, usePrevious);
		const NodeTransformCache *chain[kMaxChainDepth];
		// Both frames now gather the chain ONCE and apply pure-FPU cached coefficients
		// to all 4 corners. The previous-frame path (dirty-bounds collection) used to
		// re-walk the ancestor chain and recompute 6 software trig PER CORNER — the
		// dominant cost of the css-3d-cube dirty pass (~280 applyNodeTransform calls/
		// frame). It now matches the current frame: O(nodes) cached, O(1) per corner.
		const int depth = usePrevious ? gatherTransformChainPrev(node, chain) : gatherTransformChain(node, chain);
		if (depth >= 0) {
			// Apply the gathered chain to all 4 corners with pure FPU math — the
			// cache lookup happened once during the gather, not per corner.
			for (int i = 0; i < 4; i++) {
				for (int k = 0; k < depth; k++) {
					if (usePrevious)
						applyCachedTransformPrev(*chain[k], points[i]);
					else
						applyCachedTransform(*chain[k], points[i]);
				}
				applyPerspective(perspectiveNode, usePrevious, points[i]);
				xs[i] = (int16_t)std::lroundf(points[i].x);
				ys[i] = (int16_t)std::lroundf(points[i].y);
				if (xs8) { xs8[i] = fx8(points[i].x); ys8[i] = fx8(points[i].y); }
			}
			return;
		}
		// Pathologically deep chain (gather returned -1): per-corner walk fallback.
		for (int i = 0; i < 4; i++) {
			for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
				applyNodeTransform(state.nodes[id], usePrevious, points[i]);
			applyPerspective(perspectiveNode, usePrevious, points[i]);
			xs[i] = (int16_t)std::lroundf(points[i].x);
			ys[i] = (int16_t)std::lroundf(points[i].y);
			if (xs8) { xs8[i] = fx8(points[i].x); ys8[i] = fx8(points[i].y); }
		}
	}

	static void transformPoint(const Node &node, bool usePrevious, float x, float y, float z, int16_t *outX, int16_t *outY)
	{
		Point3 point{x, y, z};
		auto &state = treeState();
		const int perspectiveNode = nearestPerspectiveNode(node, usePrevious);
		for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
			applyNodeTransform(state.nodes[id], usePrevious, point);
		applyPerspective(perspectiveNode, usePrevious, point);
		*outX = static_cast<int16_t>(std::lroundf(point.x));
		*outY = static_cast<int16_t>(std::lroundf(point.y));
	}

	// Prepared projector for plotting MANY points through one node's transform
	// (e.g. an ellipse-stroke ring's ~60 segment vertices). Gathers the ancestor
	// chain's cached coefficients ONCE; each projectPoint is then pure FPU math
	// instead of re-walking the chain + cache lookup per point (current frame only).
	struct ChainProjector {
		const NodeTransformCache *chain[kMaxChainDepth];
		int depth = -1;          // -1 => chain too deep, fall back to per-point walk
		int perspectiveNode = -1;
		const Node *node = nullptr;
	};
	static ChainProjector GEA_VIEW_HOT_SRAM_SECTION("prepare_projector") prepareProjector(const Node &node)
	{
		ChainProjector pr;
		pr.node = &node;
		pr.perspectiveNode = nearestPerspectiveNode(node, false);
		pr.depth = gatherTransformChain(node, pr.chain);
		return pr;
	}
	static void GEA_VIEW_HOT_SRAM_SECTION("project_point") projectPoint(const ChainProjector &pr, float x, float y, float z, int16_t *outX, int16_t *outY)
	{
		Point3 p{x, y, z};
		if (pr.depth >= 0) {
			for (int k = 0; k < pr.depth; k++) applyCachedTransform(*pr.chain[k], p);
		} else {
			auto &state = treeState();
			for (int id = nodeIndex(*pr.node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
				applyNodeTransform(state.nodes[id], false, p);
		}
		applyPerspective(pr.perspectiveNode, false, p);
		*outX = static_cast<int16_t>(std::lroundf(p.x));
		*outY = static_cast<int16_t>(std::lroundf(p.y));
	}

	static int GEA_VIEW_HOT_SRAM_SECTION("average_depth_compute") averageDepthCompute(const Node &node, bool usePrevious)
	{
		const int x = usePrevious ? node.layout.previous_x : node.layout.x;
		const int y = usePrevious ? node.layout.previous_y : node.layout.y;
		const int w = usePrevious ? node.layout.previous_width : node.layout.width;
		const int h = usePrevious ? node.layout.previous_height : node.layout.height;
		const float fx = static_cast<float>(x), fy = static_cast<float>(y);
		const float fw = static_cast<float>(w), fh = static_cast<float>(h);
		Point3 points[5] = {
		    {fx, fy, 0.0f},
		    {fx + fw, fy, 0.0f},
		    {fx + fw, fy + fh, 0.0f},
		    {fx, fy + fh, 0.0f},
		    {fx + fw * 0.5f, fy + fh * 0.5f, 0.0f},
		};
		// Gather the ancestor chain once and apply to all 5 depth-probe points with
		// pure FPU math (same hoist as transformRectCorners). This is the z-sort's
		// hot path — without it each point re-did a cache lookup per ancestor.
		const NodeTransformCache *chain[kMaxChainDepth];
		const int depth = usePrevious ? gatherTransformChainPrev(node, chain) : gatherTransformChain(node, chain);
		if (depth >= 0) {
			for (int i = 0; i < 5; i++)
				for (int k = 0; k < depth; k++) {
					if (usePrevious)
						applyCachedTransformPrev(*chain[k], points[i]);
					else
						applyCachedTransform(*chain[k], points[i]);
				}
		} else {
			auto &state = treeState();
			for (int i = 0; i < 5; i++)
				for (int id = nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent)
					applyNodeTransform(state.nodes[id], usePrevious, points[i]);
		}
		float sum = 0.0f;
		for (const Point3 &p : points) sum += p.z;
		return static_cast<int>(std::lroundf(sum * 10.0f / 5.0f));
	}

	// Painter's-depth for the current frame, cached per node per refresh. The
	// child z-sort's subtreeDepth() recurses every subtree at every tree level, so
	// without this each node's 5-projection depth was recomputed many times per
	// frame (the dominant ~16ms 'sort' cost on css-3d-cube). The previous-frame
	// path isn't cached (cold, used only for dirty bounds).
	static int GEA_VIEW_HOT_SRAM_SECTION("average_depth") averageDepth(const Node &node, bool usePrevious)
	{
		if (usePrevious) return averageDepthCompute(node, true);
		static ActiveDepthCache cache;
		const int id = nodeIndex(node);
		const uint64_t serial = treeState().refreshSerial;
		if (cache.serial != serial) {
			resetCache(cache.entries, kActiveDepthCacheSlots);
			cache.serial = serial;
		}
		if (id >= 0) {
			AverageDepthCacheEntry &e = cache.entries[id & (kActiveDepthCacheSlots - 1)];
			if (e.nodeId == id) return e.depth;
			if (e.nodeId != -1) return averageDepthCompute(node, false);
			const int d = averageDepthCompute(node, false);
			e.depth = d;
			e.nodeId = id;
			return d;
		}
		return averageDepthCompute(node, false);
	}

	// The node's four projected screen-space corners for the current frame, cached
	// per node per refresh. recordBox projects the same node rect for its bounds
	// AND its fill quad, and nodeOverlapsClip projects it a third time — caching
	// collapses those (and the dirty pass's current-frame half) to one projection.
	static void GEA_VIEW_HOT_SRAM_SECTION("transform_corners") transformCorners(const Node &node, bool usePrevious, int16_t *xs, int16_t *ys)
	{
		if (usePrevious) {
			// Cache previous-frame corners per node per refresh, mirroring the current-frame
			// path below. The dirty-bounds collect projects each node's previous corners
			// REDUNDANTLY — once while walking the dynamic container's whole subtree
			// (transformedSubtreeBoundsRect) and again when the child is visited as its own
			// dirty node — so without this the spinning cube re-projects every face/label's
			// previous corners 2-3x/frame. One cache lookup collapses that to once.
			static ActiveCornerCache prevCache;
			const int pid = nodeIndex(node);
			const uint64_t pserial = treeState().refreshSerial;
			if (prevCache.serial != pserial) {
				resetCache(prevCache.entries, kActiveCornerCacheSlots);
				prevCache.serial = pserial;
			}
			if (pid >= 0) {
				CornerTransformCacheEntry &e = prevCache.entries[pid & (kActiveCornerCacheSlots - 1)];
				if (e.nodeId != pid) {
					if (e.nodeId != -1) {
						// Slot collision with another node this refresh: compute uncached.
						transformRectCorners(node, true, node.layout.previous_x, node.layout.previous_y,
						                     node.layout.previous_width, node.layout.previous_height, xs, ys);
						return;
					}
					transformRectCorners(node, true, node.layout.previous_x, node.layout.previous_y,
					                     node.layout.previous_width, node.layout.previous_height, e.xs, e.ys);
					e.nodeId = pid;
				}
				for (int i = 0; i < 4; i++) { xs[i] = e.xs[i]; ys[i] = e.ys[i]; }
				return;
			}
			transformRectCorners(node, true, node.layout.previous_x, node.layout.previous_y,
			                     node.layout.previous_width, node.layout.previous_height, xs, ys);
			return;
		}
		static ActiveCornerCache cache;
		const int id = nodeIndex(node);
		const uint64_t serial = treeState().refreshSerial;
		if (cache.serial != serial) {
			resetCache(cache.entries, kActiveCornerCacheSlots);
			cache.serial = serial;
		}
		if (id >= 0) {
			CornerTransformCacheEntry &e = cache.entries[id & (kActiveCornerCacheSlots - 1)];
			if (e.nodeId != id) {
				if (e.nodeId != -1) {
					transformRectCorners(node, false, node.layout.x, node.layout.y,
					                     node.layout.width, node.layout.height, xs, ys);
					return;
				}
				transformRectCorners(node, false, node.layout.x, node.layout.y,
				                     node.layout.width, node.layout.height, e.xs, e.ys);
				e.nodeId = id;
			}
			for (int i = 0; i < 4; i++) { xs[i] = e.xs[i]; ys[i] = e.ys[i]; }
			return;
		}
		transformRectCorners(node, false, node.layout.x, node.layout.y,
		                     node.layout.width, node.layout.height, xs, ys);
	}
};

void appendReliefBordersWithAlpha(const Node &node, uint8_t parentAlpha)
{
	using namespace gea::framework::graphics;
	const int w = node.layout.width, h = node.layout.height;
	if (w <= 0 || h <= 0) return;
	int widths[4];
	for (int side = 0; side < 4; ++side)
		widths[side] = std::min(computedBorderWidth(node.style, side), side % 2 ? w / 2 : h / 2);
	const bool transformed = ViewGeometry::hasTransformChain(node, false);
	for (int side = 0; side < 4; ++side) {
		if (widths[side] <= 0) continue;
		const int relief = rstyle(node.style).border_relief[side];
		const int bands = relief == 1 || relief == 2 ? 2 : 1;
		const auto color = borderPaintColor(node.style, side);
		int r, g, b; pixel::unpackRgb565(color, &r, &g, &b);
		r = pixel::expand5To8(r); g = pixel::expand6To8(g); b = pixel::expand5To8(b);
		// CSS permits UA-defined relief shades. Mixing with white keeps even a
		// black currentColor border visibly raised or recessed.
		const auto light = pixel::nativeColor((r * 3 + 510) / 5, (g * 3 + 510) / 5, (b * 3 + 510) / 5);
		const auto dark = pixel::nativeColor(r * 3 / 5, g * 3 / 5, b * 3 / 5);
		for (int band = 0; band < bands; ++band) {
			int16_t ringX[2][4], ringY[2][4];
			for (int ring = 0; ring < 2; ++ring) {
				int inset[4];
				for (int edge = 0; edge < 4; ++edge) inset[edge] = widths[edge] * (band + ring) / bands;
				const int left = node.layout.x + inset[3], right = node.layout.x + w - inset[1];
				const int top = node.layout.y + inset[0], bottom = node.layout.y + h - inset[2];
				const int px[4] = {left, right, right, left}, py[4] = {top, top, bottom, bottom};
				for (int corner = 0; corner < 4; ++corner) {
					if (transformed) ViewGeometry::transformPoint(node, false, px[corner], py[corner], 0, &ringX[ring][corner], &ringY[ring][corner]);
					else { ringX[ring][corner] = px[corner]; ringY[ring][corner] = py[corner]; }
				}
			}
			const int next = (side + 1) % 4;
			const int16_t xs[4] = {ringX[0][side], ringX[0][next], ringX[1][next], ringX[1][side]};
			const int16_t ys[4] = {ringY[0][side], ringY[0][next], ringY[1][next], ringY[1][side]};
			const bool inset = relief == 3 || (relief == 1 && band == 0) || (relief == 2 && band == 1);
			const bool shadedDark = (side == 0 || side == 3) == inset;
			int bx0, by0, bx1, by1; boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
			appendFillQuadWithAlpha(xs, ys, relief == 0 ? color : shadedDark ? dark : light,
			                        borderPaintAlpha(node.style, side), parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
		}
	}
}

void appendSideBordersWithAlpha(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (hasBorderRelief(node.style)) { appendReliefBordersWithAlpha(node, parentAlpha); return; }
	const bool textClippedTransform = StyleValues::hasTextBackgroundClip(node.style) &&
	    ViewGeometry::hasTransformChain(node, false) && !isFullyRoundedShape(node);
	if (w <= 0 || h <= 0 || (!hasSideBorder(node.style) && !borderColorsDiffer(node.style) && !textClippedTransform)) return;

	const bool transformed = ViewGeometry::hasTransformChain(node, false);
	for (int side = 0; side < 4; ++side) {
		const int borderWidth = std::max<int>(node.style.border_width, rstyle(node.style).border_side_width[side]);
		const auto color = borderPaintColor(node.style, side);
		const auto alpha = borderPaintAlpha(node.style, side);
		if (borderWidth <= 0) continue;
		int sx = x;
		int sy = y;
		int sw = w;
		int sh = h;
		if (side == 0) {
			sh = std::min(borderWidth, h);
		} else if (side == 1) {
			sw = std::min(borderWidth, w);
			sx = x + w - sw;
		} else if (side == 2) {
			sh = std::min(borderWidth, h);
			sy = y + h - sh;
		} else {
			sw = std::min(borderWidth, w);
		}
		// Horizontal edges own the corners. Avoid compositing a translucent
		// asymmetric border twice where two side rectangles would overlap.
		if (side == 1 || side == 3) {
			const int top = std::min<int>(std::max<int>(node.style.border_width, rstyle(node.style).border_side_width[0]), h);
			const int bottom = std::min<int>(std::max<int>(node.style.border_width, rstyle(node.style).border_side_width[2]), h - top);
			sy += top;
			sh -= top + bottom;
		}
		if (sw <= 0 || sh <= 0) continue;
		if (transformed) {
			int16_t xs[4], ys[4];
			ViewGeometry::transformRectCorners(node, false, sx, sy, sw, sh, xs, ys);
			int bx0, by0, bx1, by1;
			boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
			appendFillQuadWithAlpha(xs,
			                        ys,
			                        color,
			                        alpha,
			                        parentAlpha,
			                        bx0,
			                        by0,
			                        bx1 - bx0 + 1,
			                        by1 - by0 + 1,
			                        sx,
			                        sy,
			                        sw,
			                        sh,
			                        true);
		} else {
			appendFillRectWithAlpha(sx,
			                        sy,
			                        sw,
			                        sh,
			                        color,
			                        alpha,
			                        parentAlpha,
			                        sx,
			                        sy,
			                        sw,
			                        sh);
		}
	}
}

void recordLinearGradientBackground(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	if (!ViewGeometry::hasTransformChain(node, false)) {
		appendLinearGradientRectRaw(node, x, y, w, h);
		return;
	}

	// Transformed (3D/perspective) face: emit ONE per-pixel-shaded gradient quad
	// instead of ~72 solid-color strips. The drawer inverse-maps each screen pixel
	// to face-local space and samples the gradient directly — exact (no banding),
	// alpha-correct, and 1 command vs 72 (the strips were ~575 quads/frame on the
	// 6-face spinning cube, dominating both record and replay).
	int16_t xs[4], ys[4], fx[4], fy[4];
	ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys, fx, fy);
	int bx0, by0, bx1, by1;
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillTransformedLinearGradient;
	cmd->bx = bx0;
	cmd->by = by0;
	cmd->bw = bx1 - bx0 + 1;
	cmd->bh = by1 - by0 + 1;
	auto &g = cmd->transformedGradient;
	g.x0 = xs[0]; g.y0 = ys[0];
	g.x1 = xs[1]; g.y1 = ys[1];
	g.x2 = xs[2]; g.y2 = ys[2];
	g.x3 = xs[3]; g.y3 = ys[3];
	g.fx0 = fx[0]; g.fy0 = fy[0];
	g.fx1 = fx[1]; g.fy1 = fy[1];
	g.fx2 = fx[2]; g.fy2 = fy[2];
	g.fx3 = fx[3]; g.fy3 = fy[3];
	g.lx = static_cast<int16_t>(x);
	g.ly = static_cast<int16_t>(y);
	g.lw = static_cast<int16_t>(w);
	g.lh = static_cast<int16_t>(h);
	g.fromColor = rstyle(node.style).bg_gradient_from_color;
	g.midColor = rstyle(node.style).bg_gradient_mid_color;
	g.toColor = rstyle(node.style).bg_gradient_to_color;
	g.midStop = static_cast<uint16_t>(rstyle(node.style).bg_gradient_mid_stop);
	g.toStop = static_cast<uint16_t>(gradientToStop(node));
	g.angle = static_cast<int16_t>(rstyle(node.style).bg_gradient_angle);
	g.fromAlpha = rstyle(node.style).bg_gradient_from_alpha;
	g.midAlpha = rstyle(node.style).bg_gradient_mid_alpha;
	g.toAlpha = rstyle(node.style).bg_gradient_to_alpha;
	g.hasMid = rstyle(node.style).bg_gradient_has_mid ? 1 : 0;
	g.backfaceHidden = node.style.backface_hidden ? 1 : 0;
	// Transformed rectangular borders ride this command as an edge frame (the
	// span rasterizer paints the first/last edgeWidth px of every span, tracing
	// the quad outline). Emitting projected stroke quads instead would disarm
	// the transform-reproject fast path every frame.
	if (!StyleValues::hasTextBackgroundClip(node.style) && node.style.border_width > 0 && !hasBorderRelief(node.style) && !hasSideBorder(node.style) && !borderColorsDiffer(node.style) && borderPaintAlpha(node.style, 0) > 0 &&
	    !isFullyRoundedShape(node)) {
		g.edgeColor = borderPaintColor(node.style, 0);
		g.edgeAlpha = borderPaintAlpha(node.style, 0);
		const int ew = node.style.border_width < 1 ? 1 : node.style.border_width;
		g.edgeWidth = static_cast<uint8_t>(ew > 8 ? 8 : ew);
	} else {
		g.edgeColor = 0;
		g.edgeAlpha = 0;
		g.edgeWidth = 0;
	}
}

void recordRadialGradientBackground(const Node &node)
{
	if (!rstyle(node.style).bg_radial_gradient) return;
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) return;
	appendRadialGradientRectRaw(node, x, y, w, h);
}

void recordOverlayLinearGradientBackground(const Node &node)
{
	if (!rstyle(node.style).bg_overlay_gradient) return;
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) return;
	appendLinearGradientRectRaw(node,
	                            x,
	                            y,
	                            w,
	                            h,
	                            rstyle(node.style).bg_overlay_gradient_from_color,
	                            rstyle(node.style).bg_overlay_gradient_mid_color,
	                            rstyle(node.style).bg_overlay_gradient_to_color,
	                            rstyle(node.style).bg_overlay_gradient_mid_stop,
	                            rstyle(node.style).bg_overlay_gradient_to_stop,
	                            rstyle(node.style).bg_overlay_gradient_angle,
	                            rstyle(node.style).bg_overlay_gradient_from_alpha,
	                            rstyle(node.style).bg_overlay_gradient_mid_alpha,
	                            rstyle(node.style).bg_overlay_gradient_to_alpha,
	                            rstyle(node.style).bg_overlay_gradient_has_mid);
}

// Background positioning and painting areas are distinct. In particular, the
// document canvas paints beyond the root box without stretching its images.
bool recordPlacedBackgrounds(const Node &geometry, const Node &source, bool canvas)
{
	const auto &r = rstyle(source.style);
	if (!canvas && r.bg_size_list < 0 && r.bg_position_list < 0 && r.bg_repeat_list < 0 &&
	    r.bg_attachment_list < 0 && r.bg_origin_list < 0) return false;
	if (ViewGeometry::hasTransformChain(geometry, false)) return false;
	const int nodeId = ViewGeometry::nodeIndex(geometry);
	for (int layer = r.bg_image_layer_count-1; layer >= 0; --layer) {
		const bool linear = source.style.bg_fill == 1 && layer == r.bg_gradient_layer;
		const bool overlay = r.bg_overlay_gradient && layer == r.bg_overlay_gradient_layer;
		const bool radial = r.bg_radial_gradient && layer == r.bg_radial_gradient_layer;
		if (!linear && !overlay && !radial) continue;
		const auto p = StyleValues::backgroundPlacement(source.style, nodeId, layer,
		    geometry.layout.x, geometry.layout.y, geometry.layout.width, geometry.layout.height);
		if (p.width <= 0 || p.height <= 0) continue;
		int x = canvas ? 0 : geometry.layout.x, y = canvas ? 0 : geometry.layout.y;
		int w = canvas ? treeState().mountedWidth : geometry.layout.width;
		int h = canvas ? treeState().mountedHeight : geometry.layout.height;
		const int clip = canvas ? 0 : StyleValues::backgroundClip(source.style, layer);
		TextBackgroundClipScope textClip(source, clip);
		if (clip == 1 || clip == 2) {
			int inset[4];
			for (int i = 0; i < 4; ++i) inset[i] = std::max<int>(geometry.style.border_width, rstyle(geometry.style).border_side_width[i]) +
			    (clip == 2 ? std::max<int>(0, geometry.style.padding[i]) : 0);
			x += inset[3]; y += inset[0]; w -= inset[1]+inset[3]; h -= inset[0]+inset[2];
		}
		if (w <= 0 || h <= 0) continue;
		auto *begin = DisplayList::instance().append();
		if (!begin) return true;
		begin->type = DisplayCommandType::PushClip;
		begin->bx = begin->clip.x = x; begin->by = begin->clip.y = y;
		begin->bw = begin->clip.w = w; begin->bh = begin->clip.h = h; begin->clip.nodeId = -1;
		struct TileAxis { double start, step; int count; };
		auto axis = [](int position, int size, int mode, int area, int areaSize, int clipStart, int clipSize) -> TileAxis {
			if (mode == 1) return {static_cast<double>(position), static_cast<double>(size), 1};
			if (mode == 3) {
				const int count = areaSize/size;
				if (count < 2) return {static_cast<double>(position), static_cast<double>(size), 1};
				return {static_cast<double>(area), static_cast<double>(areaSize-size)/(count-1), count};
			}
			const int start = position + static_cast<int>(std::floor(static_cast<double>(clipStart-position)/size))*size;
			return {static_cast<double>(start), static_cast<double>(size), (clipStart+clipSize-start+size-1)/size};
		};
		const auto ax = axis(p.x,p.width,p.repeatX,p.areaX,p.areaWidth,x,w), ay = axis(p.y,p.height,p.repeatY,p.areaY,p.areaHeight,y,h);
		for (int row = 0; row < ay.count; ++row) for (int col = 0; col < ax.count; ++col) {
			const int tx = std::lround(ax.start+col*ax.step), ty = std::lround(ay.start+row*ay.step);
			if (tx >= x+w || ty >= y+h || tx+p.width <= x || ty+p.height <= y) continue;
			DisplayCommand *paint = nullptr;
			if (linear) paint = appendLinearGradientRectRaw(source,tx,ty,p.width,p.height);
			else if (radial) paint = appendRadialGradientRectRaw(source,tx,ty,p.width,p.height);
			else paint = appendLinearGradientRectRaw(source,tx,ty,p.width,p.height,
			    r.bg_overlay_gradient_from_color,r.bg_overlay_gradient_mid_color,r.bg_overlay_gradient_to_color,
			    r.bg_overlay_gradient_mid_stop,r.bg_overlay_gradient_to_stop,r.bg_overlay_gradient_angle,
			    r.bg_overlay_gradient_from_alpha,r.bg_overlay_gradient_mid_alpha,r.bg_overlay_gradient_to_alpha,r.bg_overlay_gradient_has_mid);
			// Radius belongs to the painting box, never to each repeated image.
			if (paint) {
				auto &cmd = *paint;
				if (radial) cmd.radialGradient.tl = cmd.radialGradient.tr = cmd.radialGradient.br = cmd.radialGradient.bl = 0;
				else cmd.gradient.tl = cmd.gradient.tr = cmd.gradient.br = cmd.gradient.bl = 0;
				cmd.bx = std::max(x, tx); cmd.by = std::max(y, ty);
				cmd.bw = std::min(x+w,tx+p.width)-cmd.bx; cmd.bh = std::min(y+h,ty+p.height)-cmd.by;
			}
		}
		auto *end = DisplayList::instance().append();
		if (end) { end->type = DisplayCommandType::PopClip; end->bx = x; end->by = y; end->bw = w; end->bh = h; end->clip.nodeId = -1; }
	}
	return true;
}

void recordTransformedEllipseFill(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	int bx0, by0, bx1, by1;
	ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.bg_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

	const double cx = static_cast<double>(x) + static_cast<double>(w) * 0.5;
	const double cy = static_cast<double>(y) + static_cast<double>(h) * 0.5;
	const double rx = static_cast<double>(w) * 0.5;
	const double ry = static_cast<double>(h) * 0.5;
	int steps = h < 28 ? h : 28;
	if (steps < 6) steps = 6;
	for (int i = 0; i < steps; i++) {
		const int sy = y + (h * i) / steps;
		const int ey = y + (h * (i + 1)) / steps;
		const int sh = ey - sy;
		if (sh <= 0) continue;
		const double midY = static_cast<double>(sy) + static_cast<double>(sh) * 0.5;
		double t = ry > 0.0 ? (midY - cy) / ry : 0.0;
		if (t < -1.0) t = -1.0;
		if (t > 1.0) t = 1.0;
		const double half = rx * std::sqrt(std::max(0.0, 1.0 - t * t));
		const int sx = static_cast<int>(std::lround(cx - half));
		const int sw = static_cast<int>(std::lround(half * 2.0));
		if (sw <= 0) continue;
		int16_t xs[4], ys[4];
		ViewGeometry::transformRectCorners(node, false, sx, sy, sw, sh, xs, ys);
		int qx0, qy0, qx1, qy1;
		boundsFromCorners(xs, ys, &qx0, &qy0, &qx1, &qy1);
		appendFillQuadRaw(xs, ys, node.style.bg_color, qx0, qy0, qx1 - qx0 + 1, qy1 - qy0 + 1,
		                  sx, sy, sw, sh, true);
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

void recordTransformedRoundedRectFill(const Node &node, uint8_t parentAlpha, int backgroundClip = 0)
{
	int inset[4]{};
	if (backgroundClip > 0)
		for (int i = 0; i < 4; ++i)
			inset[i] = std::max<int>(node.style.border_width, rstyle(node.style).border_side_width[i]) +
			    (backgroundClip == 2 ? std::max<int>(0, node.style.padding[i]) : 0);
	const int x = node.layout.x + inset[3];
	const int y = node.layout.y + inset[0];
	const int w = node.layout.width - inset[3] - inset[1];
	const int h = node.layout.height - inset[0] - inset[2];
	if (w <= 0 || h <= 0) return;

	int16_t xs[4], ys[4];
	ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
	int bx0, by0, bx1, by1;
	boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, node.style.bg_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
	int16_t rx8[4]{};
	int16_t ry8[4]{};
	resolvedBorderRadii8(node, rx8, ry8);
	for (int i = 0; i < 4; ++i) {
		rx8[i] = std::max(0, int(rx8[i]) - 8 * inset[i == 0 || i == 3 ? 3 : 1]);
		ry8[i] = std::max(0, int(ry8[i]) - 8 * inset[i < 2 ? 0 : 2]);
	}
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::FillTransformedRoundedRect;
		cmd->bx = bx0;
		cmd->by = by0;
		cmd->bw = bx1 - bx0 + 1;
		cmd->bh = by1 - by0 + 1;
		auto &r = cmd->transformedRoundedRect;
		r.x0 = xs[0]; r.y0 = ys[0];
		r.x1 = xs[1]; r.y1 = ys[1];
		r.x2 = xs[2]; r.y2 = ys[2];
		r.x3 = xs[3]; r.y3 = ys[3];
		r.lx = static_cast<int16_t>(x);
		r.ly = static_cast<int16_t>(y);
		r.lw = static_cast<int16_t>(w);
		r.lh = static_cast<int16_t>(h);
		r.tlRx8 = rx8[0]; r.tlRy8 = ry8[0];
		r.trRx8 = rx8[1]; r.trRy8 = ry8[1];
		r.brRx8 = rx8[2]; r.brRy8 = ry8[2];
		r.blRx8 = rx8[3]; r.blRy8 = ry8[3];
		r.color = node.style.bg_color;
		r.backfaceHidden = node.style.backface_hidden ? 1 : 0;
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

void recordTransformedEllipseStroke(const Node &node, uint8_t parentAlpha)
{
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0 || node.style.border_width <= 0) return;

	int bx0, by0, bx1, by1;
	ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, borderPaintAlpha(node.style, 0));
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

	const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
	const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
	const float stroke = static_cast<float>(node.style.border_width < 1 ? 1 : node.style.border_width);
	const float screenStroke = std::max(1.6f, stroke * 1.2f);
	const float rx = std::max(0.0f, static_cast<float>(w) * 0.5f - stroke * 0.5f);
	const float ry = std::max(0.0f, static_cast<float>(h) * 0.5f - stroke * 0.5f);
	// Segment count adaptive to ring size (~1 segment per 4px of diameter), clamped.
	// Was a flat 256; each segment is a transformed stroke band (record + replay),
	// so this is kept as low as stays visually smooth for the cube's thin rings.
	int segments = (w > h ? w : h) / 4;
	if (segments < 14) segments = 14;
	if (segments > 40) segments = 40;
	const float angStep = (2.0f * kPiF) / static_cast<float>(segments);
	const ViewGeometry::ChainProjector projector = ViewGeometry::prepareProjector(node);
	int16_t prevX = 0;
	int16_t prevY = 0;
	float prevLocalX = 0.0f;
	float prevLocalY = 0.0f;
	for (int i = 0; i <= segments; i++) {
		const float a = static_cast<float>(i) * angStep;
		const float localX = cx + rx * std::cos(a);
		const float localY = cy + ry * std::sin(a);
		int16_t px, py;
		ViewGeometry::projectPoint(projector, localX, localY, 0.0f, &px, &py);
		if (i > 0)
			appendStrokeSegmentBandRaw(prevX, prevY, px, py, screenStroke, borderPaintColor(node.style, 0),
			                           prevLocalX, prevLocalY, localX, localY, true);
		prevX = px;
		prevY = py;
		prevLocalX = localX;
		prevLocalY = localY;
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

void appendBackgroundGridRectRaw(const Node &node, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) {
		int16_t xs[4], ys[4];
		ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
		// Corners are TL,TR,BR,BL. Near the horizon (or a far perspective edge) a grid
		// strip projects to under 1px and its two long edges round to the same row/col —
		// a degenerate, zero-area quad the rasterizer can't fill, so the line vanishes.
		// Round the thin axis up to a minimum of 1px so every line still draws.
		if (ys[2] - ys[1] < 1) ys[2] = ys[1] + 1; // right edge height (BR below TR)
		if (ys[3] - ys[0] < 1) ys[3] = ys[0] + 1; // left edge height (BL below TL)
		if (xs[1] - xs[0] < 1) xs[1] = xs[0] + 1; // top edge width (TR right of TL)
		if (xs[2] - xs[3] < 1) xs[2] = xs[3] + 1; // bottom edge width (BR right of BL)
		int bx0, by0, bx1, by1;
		boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
		appendFillQuadRaw(xs, ys, rstyle(node.style).bg_grid_color, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
		                  x, y, w, h, 3);
	} else {
		if (!hasAnyRadius(node)) {
			appendFillRectRaw(x, y, w, h, rstyle(node.style).bg_grid_color, x, y, w, h);
			return;
		}
		int runX0 = 0;
		int runX1 = -1;
		int runY = 0;
		int runH = 0;
		auto flushRun = [&]() {
			if (runH > 0 && runX0 <= runX1)
				appendFillRectRaw(runX0, runY, runX1 - runX0 + 1, runH, rstyle(node.style).bg_grid_color, runX0, runY, runX1 - runX0 + 1, runH);
			runH = 0;
		};
		for (int py = y; py < y + h; ++py) {
			int rowX0 = x;
			int rowX1 = x + w - 1;
			roundedNodeRowSpan(node, py, &rowX0, &rowX1);
			if (rowX0 > rowX1) {
				flushRun();
				continue;
			}
			if (runH > 0 && rowX0 == runX0 && rowX1 == runX1) {
				++runH;
				continue;
			}
			flushRun();
			runX0 = rowX0;
			runX1 = rowX1;
			runY = py;
			runH = 1;
		}
		flushRun();
	}
}

void recordBackgroundGrid(const Node &node, uint8_t parentAlpha)
{
	if (rstyle(node.style).bg_grid_axes == 0) return;
	const auto placement = StyleValues::backgroundPlacement(node.style, ViewGeometry::nodeIndex(node), 0, node.layout.x, node.layout.y, node.layout.width, node.layout.height);
	const int x0 = node.layout.x;
	const int y0 = node.layout.y;
	const int x1 = node.layout.x + node.layout.width;
	const int y1 = node.layout.y + node.layout.height;
	if (x1 <= x0 || y1 <= y0) return;

	int bx0 = x0, by0 = y0, bx1 = x1 - 1, by1 = y1 - 1;
	if (ViewGeometry::hasTransformChain(node, false))
		ViewRenderer::transformedBounds(node, false, &bx0, &by0, &bx1, &by1);
	const uint8_t effectiveAlpha = combineAlpha(parentAlpha, rstyle(node.style).bg_grid_alpha);
	if (effectiveAlpha != parentAlpha) appendAlphaCommand(effectiveAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);

	if ((rstyle(node.style).bg_grid_axes & 1) != 0 && rstyle(node.style).bg_grid_line_x > 0) {
		const int step = rstyle(node.style).bg_size_list >= 0 ? placement.width : rstyle(node.style).bg_grid_step_x > 0 ? rstyle(node.style).bg_grid_step_x : rstyle(node.style).bg_grid_line_x;
		if (step > 0) {
			for (int x = x0; x < x1; x += step) {
				const int w = std::min<int>(rstyle(node.style).bg_grid_line_x, x1 - x);
				appendBackgroundGridRectRaw(node, x, y0, w, y1 - y0);
			}
		}
	}
	if ((rstyle(node.style).bg_grid_axes & 2) != 0 && rstyle(node.style).bg_grid_line_y > 0) {
		const int step = rstyle(node.style).bg_size_list >= 0 ? placement.height : rstyle(node.style).bg_grid_step_y > 0 ? rstyle(node.style).bg_grid_step_y : rstyle(node.style).bg_grid_line_y;
		if (step > 0) {
			for (int y = y0; y < y1; y += step) {
				const int h = std::min<int>(rstyle(node.style).bg_grid_line_y, y1 - y);
				appendBackgroundGridRectRaw(node, x0, y, x1 - x0, h);
			}
		}
	}

	if (effectiveAlpha != parentAlpha) appendAlphaCommand(parentAlpha, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
}

uint8_t insetShadowAlphaAt(int depth, int solidDepth, int blurRadius, uint8_t baseAlpha)
{
	if (baseAlpha == 0) return 0;
	if (depth < solidDepth) return baseAlpha;
	if (blurRadius <= 0) return 0;
	const int blurDepth = depth - solidDepth;
	if (blurDepth >= blurRadius) return 0;
	double t = 1.0 - (static_cast<double>(blurDepth) + 0.5) / static_cast<double>(blurRadius);
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	const int alpha = static_cast<int>(static_cast<double>(baseAlpha) * t * t + 0.5);
	return static_cast<uint8_t>(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha);
}

void appendInsetShadowBand(const Node &node, uint8_t parentAlpha, int x, int y, int w, int h, uint8_t alpha)
{
	if (alpha == 0 || w <= 0 || h <= 0) return;
	if (ViewGeometry::hasTransformChain(node, false)) {
		int16_t xs[4], ys[4];
		ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
		int bx0, by0, bx1, by1;
		boundsFromCorners(xs, ys, &bx0, &by0, &bx1, &by1);
		appendFillQuadWithAlpha(xs,
		                        ys,
		                        rstyle(node.style).box_shadow_color,
		                        alpha,
		                        parentAlpha,
		                        bx0,
		                        by0,
		                        bx1 - bx0 + 1,
		                        by1 - by0 + 1,
		                        x,
		                        y,
		                        w,
		                        h,
		                        true);
		return;
	}
	if (!hasAnyRadius(node)) {
		appendFillRectWithAlpha(x, y, w, h, rstyle(node.style).box_shadow_color, alpha, parentAlpha, x, y, w, h);
		return;
	}
	int runX0 = 0;
	int runX1 = -1;
	int runY = 0;
	int runH = 0;
	auto flushRun = [&]() {
		if (runH > 0 && runX0 <= runX1)
			appendFillRectWithAlpha(runX0,
			                        runY,
			                        runX1 - runX0 + 1,
			                        runH,
			                        rstyle(node.style).box_shadow_color,
			                        alpha,
			                        parentAlpha,
			                        runX0,
			                        runY,
			                        runX1 - runX0 + 1,
			                        runH);
		runH = 0;
	};
	for (int py = y; py < y + h; ++py) {
		int rowX0 = x;
		int rowX1 = x + w - 1;
		roundedNodeRowSpan(node, py, &rowX0, &rowX1);
		if (rowX0 > rowX1) {
			flushRun();
			continue;
		}
		if (runH > 0 && rowX0 == runX0 && rowX1 == runX1) {
			++runH;
			continue;
		}
		flushRun();
		runX0 = rowX0;
		runX1 = rowX1;
		runY = py;
		runH = 1;
	}
	flushRun();
}

// The inset shadow is the padding contour minus an offset, spread-adjusted
// hole. Represent both contours explicitly: independent edge bands overlap at
// corners and cannot describe rounded holes or negative spread.
struct ShadowContour {
	float x, y, w, h;
	float rx[4]{}, ry[4]{};
};

void constrainShadowRadii(ShadowContour &shape)
{
	float scale = 1.0f;
	auto constrain = [&](float limit, float sum) {
		if (sum > limit && sum > 0) scale = std::min(scale, std::max(0.0f, limit) / sum);
	};
	constrain(shape.w, shape.rx[0] + shape.rx[1]);
	constrain(shape.w, shape.rx[3] + shape.rx[2]);
	constrain(shape.h, shape.ry[0] + shape.ry[3]);
	constrain(shape.h, shape.ry[1] + shape.ry[2]);
	for (int i = 0; i < 4; ++i) { shape.rx[i] *= scale; shape.ry[i] *= scale; }
}

ShadowContour insetShadowClip(const Node &node)
{
	int border[4];
	for (int i = 0; i < 4; ++i) border[i] = std::max<int>(node.style.border_width, rstyle(node.style).border_side_width[i]);
	ShadowContour shape{float(node.layout.x + border[3]), float(node.layout.y + border[0]),
	                    float(std::max(0, node.layout.width - border[3] - border[1])),
	                    float(std::max(0, node.layout.height - border[0] - border[2]))};
	int16_t rx8[4], ry8[4];
	resolvedBorderRadii8(node, rx8, ry8);
	for (int i = 0; i < 4; ++i) {
		shape.rx[i] = std::max(0.0f, rx8[i] * 0.125f - border[(i == 0 || i == 3) ? 3 : 1]);
		shape.ry[i] = std::max(0.0f, ry8[i] * 0.125f - border[i < 2 ? 0 : 2]);
	}
	constrainShadowRadii(shape);
	return shape;
}

ShadowContour insetShadowHole(const ShadowContour &clip, int spread, int ox, int oy)
{
	ShadowContour hole = clip;
	hole.x += spread + ox;
	hole.y += spread + oy;
	hole.w = std::max(0.0f, hole.w - 2 * spread);
	hole.h = std::max(0.0f, hole.h - 2 * spread);
	auto radius = [spread](float r) {
		float adjustment = float(spread);
		if (spread < 0 && r < -spread) {
			const float ratio = r / -spread - 1.0f;
			adjustment *= 1.0f + ratio * ratio * ratio;
		}
		return std::max(0.0f, r - adjustment);
	};
	for (int i = 0; i < 4; ++i) { hole.rx[i] = radius(hole.rx[i]); hole.ry[i] = radius(hole.ry[i]); }
	constrainShadowRadii(hole);
	return hole;
}

bool shadowContourRow(const ShadowContour &shape, int y, int &left, int &right)
{
	const float py = y + 0.5f;
	if (shape.w <= 0 || shape.h <= 0 || py < shape.y || py >= shape.y + shape.h) return false;
	float x0 = shape.x, x1 = shape.x + shape.w;
	for (int i = 0; i < 4; ++i) {
		const bool top = i < 2, isLeft = i == 0 || i == 3;
		const float rx = shape.rx[i], ry = shape.ry[i];
		if (rx <= 0 || ry <= 0) continue;
		const float cy = top ? shape.y + ry : shape.y + shape.h - ry;
		if (top ? py >= cy : py <= cy) continue;
		const float dy = (py - cy) / ry;
		const float inset = rx * (1.0f - std::sqrt(std::max(0.0f, 1.0f - dy * dy)));
		if (isLeft) x0 = std::max(x0, shape.x + inset);
		else x1 = std::min(x1, shape.x + shape.w - inset);
	}
	left = static_cast<int>(std::ceil(x0 - 0.5f));
	right = static_cast<int>(std::ceil(x1 - 0.5f)) - 1;
	return left <= right;
}

void appendShadowRect(const Node &node, uint8_t parentAlpha, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	const auto color = rstyle(node.style).box_shadow_color;
	const auto alpha = rstyle(node.style).box_shadow_alpha;
	if (!ViewGeometry::hasTransformChain(node, false)) {
		appendFillRectWithAlpha(x, y, w, h, color, alpha, parentAlpha, x, y, w, h);
		return;
	}
	int16_t xs[4], ys[4];
	ViewGeometry::transformRectCorners(node, false, x, y, w, h, xs, ys);
	int x0, y0, x1, y1;
	boundsFromCorners(xs, ys, &x0, &y0, &x1, &y1);
	appendFillQuadWithAlpha(xs, ys, color, alpha, parentAlpha, x0, y0, x1 - x0 + 1, y1 - y0 + 1, x, y, w, h, true);
}

void recordSharpInsetShadow(const Node &node, uint8_t parentAlpha, const ShadowContour &clip, int spread, int ox, int oy)
{
	if (clip.w <= 0 || clip.h <= 0) return;
	const ShadowContour hole = insetShadowHole(clip, spread, ox, oy);
	// A centered, opaque circular ring can use the same native stroke primitive
	// as a border. All other contours use disjoint spans, composited exactly once.
	bool circular = true;
	for (int i = 0; i < 4; ++i) circular &= clip.rx[i] == clip.ry[i] && clip.rx[i] == std::floor(clip.rx[i]) && clip.rx[i] <= std::min(clip.w, clip.h) * 0.5f;
	if (ox == 0 && oy == 0 && spread > 0 && hole.w > 0 && hole.h > 0 && circular &&
	    combineAlpha(parentAlpha, rstyle(node.style).box_shadow_alpha) == 255 &&
	    !ViewGeometry::hasTransformChain(node, false)) {
		DisplayCommand *cmd = DisplayList::instance().append();
		if (cmd) {
			cmd->type = DisplayCommandType::StrokeRoundedRect;
			cmd->strokeRoundedRect = {};
			cmd->bx = cmd->strokeRoundedRect.x = static_cast<int16_t>(clip.x);
			cmd->by = cmd->strokeRoundedRect.y = static_cast<int16_t>(clip.y);
			cmd->bw = cmd->strokeRoundedRect.w = static_cast<int16_t>(clip.w);
			cmd->bh = cmd->strokeRoundedRect.h = static_cast<int16_t>(clip.h);
			cmd->strokeRoundedRect.tl = static_cast<int16_t>(clip.rx[0]);
			cmd->strokeRoundedRect.tr = static_cast<int16_t>(clip.rx[1]);
			cmd->strokeRoundedRect.br = static_cast<int16_t>(clip.rx[2]);
			cmd->strokeRoundedRect.bl = static_cast<int16_t>(clip.rx[3]);
			cmd->strokeRoundedRect.lineWidth = spread;
			cmd->strokeRoundedRect.color = rstyle(node.style).box_shadow_color;
		}
		return;
	}
	struct Run { int left = 0, right = -1, y = 0, height = 0; } runs[2];
	auto flush = [&](Run &run) {
		if (run.height > 0) appendShadowRect(node, parentAlpha, run.left, run.y, run.right - run.left + 1, run.height);
		run.height = 0;
	};
	for (int y = static_cast<int>(clip.y); y < clip.y + clip.h; ++y) {
		int left = 0, right = -1, holeLeft = 0, holeRight = -1;
		const bool hasClip = shadowContourRow(clip, y, left, right);
		const bool hasHole = shadowContourRow(hole, y, holeLeft, holeRight);
		int starts[2] = {left, 0}, ends[2] = {right, -1};
		if (!hasClip) ends[0] = starts[0] - 1;
		else if (hasHole && holeLeft <= right && holeRight >= left) {
			ends[0] = std::min(right, holeLeft - 1);
			starts[1] = std::max(left, holeRight + 1);
			ends[1] = right;
		}
		for (int i = 0; i < 2; ++i) {
			auto &run = runs[i];
			if (starts[i] > ends[i]) { flush(run); continue; }
			if (run.height && run.left == starts[i] && run.right == ends[i]) { ++run.height; continue; }
			flush(run);
			run = Run{starts[i], ends[i], y, 1};
		}
	}
	for (auto &run : runs) flush(run);
}

void recordInsetBoxShadow(const Node &node, uint8_t parentAlpha)
{
	if (!rstyle(node.style).box_shadow_inset || rstyle(node.style).box_shadow_alpha == 0) return;
	const int x = node.layout.x;
	const int y = node.layout.y;
	const int w = node.layout.width;
	const int h = node.layout.height;
	if (w <= 0 || h <= 0) return;

	const int blur = std::max<int>(0, rstyle(node.style).box_shadow_blur_radius);
	if (blur == 0) {
		recordSharpInsetShadow(node, parentAlpha, insetShadowClip(node), rstyle(node.style).box_shadow_spread,
		                       rstyle(node.style).box_shadow_offset_x, rstyle(node.style).box_shadow_offset_y);
		return;
	}
	const int spread = std::max<int>(0, rstyle(node.style).box_shadow_spread);
	const int ox = rstyle(node.style).box_shadow_offset_x;
	const int oy = rstyle(node.style).box_shadow_offset_y;
	const uint8_t baseAlpha = rstyle(node.style).box_shadow_alpha;

	const int leftSolid = spread + std::max(0, ox);
	const int rightSolid = spread + std::max(0, -ox);
	const int topSolid = spread + std::max(0, oy);
	const int bottomSolid = spread + std::max(0, -oy);
	const int leftExtent = std::min(w / 2, leftSolid + blur);
	const int rightExtent = std::min(w / 2, rightSolid + blur);
	const int topExtent = std::min(h / 2, topSolid + blur);
	const int bottomExtent = std::min(h / 2, bottomSolid + blur);

	for (int d = 0; d < topExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x, y + d, w, 1, insetShadowAlphaAt(d, topSolid, blur, baseAlpha));
	for (int d = 0; d < bottomExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x, y + h - d - 1, w, 1, insetShadowAlphaAt(d, bottomSolid, blur, baseAlpha));
	for (int d = 0; d < leftExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x + d, y, 1, h, insetShadowAlphaAt(d, leftSolid, blur, baseAlpha));
	for (int d = 0; d < rightExtent; ++d)
		appendInsetShadowBand(node, parentAlpha, x + w - d - 1, y, 1, h, insetShadowAlphaAt(d, rightSolid, blur, baseAlpha));
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_bounds") ViewRenderer::transformedBounds(const Node &node, bool use_prev, int *x0, int *y0, int *x1, int *y1)
{
	const Node *n = &node;
	int x = use_prev ? n->layout.previous_x : n->layout.x;
	int y = use_prev ? n->layout.previous_y : n->layout.y;
	int w = use_prev ? n->layout.previous_width : n->layout.width;
	int h = use_prev ? n->layout.previous_height : n->layout.height;

	if (w <= 0 || h <= 0) {
		*x0 = x; *y0 = y; *x1 = x - 1; *y1 = y - 1;
		return;
	}

	// Hot fast path: a node with no RareStyle entry (rare_style < 0) has no local
	// transform/perspective/filter, so in a transform-free tree its current bounds are
	// exactly the layout box. In a transformed tree we must still walk ancestors:
	// otherwise plain children (for example a face label <span>) ignore their
	// transformed parent plane and paint as axis-aligned overlay text.
	if (!use_prev && n->style.rare_style < 0 && !ViewGeometry::anyTransformPresent()) {
		*x0 = x;
		*y0 = y;
		*x1 = x + w - 1;
		*y1 = y + h - 1;
		return;
	}

	int rotate = use_prev ? n->render.previous_transform_rotate : rstyle(n->style).transform_rotate;

	auto expandForBlur = [&]() {
		const int radius = use_prev ? n->render.previous_filter_blur_radius : rstyle(n->style).filter_blur_radius;
		if (radius <= 0) return;
		const int extentX = std::max(1, radius) * 5;
		const int extentY = std::max(1, radius) * 5;
		*x0 -= extentX;
		*y0 -= extentY;
		*x1 += extentX;
		*y1 += extentY;
	};

	if ((rotate % 3600) == 0 && !ViewGeometry::hasTransformChain(node, use_prev)) {
		*x0 = x;
		*y0 = y;
		*x1 = x + w - 1;
		*y1 = y + h - 1;
		expandForBlur();
		return;
	}

	int16_t xs[4], ys[4];
	ViewGeometry::transformCorners(*n, use_prev, xs, ys);
	*x0 = *x1 = xs[0];
	*y0 = *y1 = ys[0];
	for (int i = 1; i < 4; i++) {
		if (xs[i] < *x0) *x0 = xs[i];
		if (xs[i] > *x1) *x1 = xs[i];
		if (ys[i] < *y0) *y0 = ys[i];
		if (ys[i] > *y1) *y1 = ys[i];
	}
	expandForBlur();
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_corners") ViewRenderer::transformedCorners(const Node &node, bool usePrevious, int16_t *xs, int16_t *ys)
{
	ViewGeometry::transformCorners(node, usePrevious, xs, ys);
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_point") ViewRenderer::transformedPoint(const Node &node, bool usePrevious, float x, float y, float z, int16_t *outX, int16_t *outY)
{
	ViewGeometry::transformPoint(node, usePrevious, x, y, z, outX, outY);
}

bool ViewRenderer::backfaceSubtreeHidden(const Node &node)
{
	const auto &state = treeState();
	for (int id = ViewGeometry::nodeIndex(node); id >= 0 && id < state.nodeCount; id = state.nodes[id].parent) {
		const auto &ancestor = state.nodes[id];
		if (ViewGeometry::hasTransformableBox(ancestor) && ancestor.style.backface_hidden &&
		    !preserves3D(ancestor.style) && ViewGeometry::backFacing(ancestor))
			return true;
	}
	return false;
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_rect_corners") ViewRenderer::transformedRectCorners(const Node &node, bool usePrevious, int x, int y, int w, int h, int16_t *xs, int16_t *ys,
                                          int16_t *xs8, int16_t *ys8)
{
	ViewGeometry::transformRectCorners(node, usePrevious, x, y, w, h, xs, ys, xs8, ys8);
}

int GEA_VIEW_HOT_SRAM_SECTION("view_renderer_transformed_depth") ViewRenderer::transformedDepth(const Node &node, bool usePrevious)
{
	return ViewGeometry::averageDepth(node, usePrevious);
}

bool ViewRenderer::isTransformableBox(const Node &node)
{
	return ViewGeometry::hasTransformableBox(node);
}

bool GEA_VIEW_HOT_SRAM_SECTION("view_renderer_any_transform_active") ViewRenderer::anyTransformActive()
{
	return ViewGeometry::anyTransformPresent();
}

bool ViewRenderer::recordClipBegin(const Node &node)
{
	const Node *n = &node;
	if (ViewGeometry::hasTransformChain(*n, false)) return 0;
	if (!isViewLikeNodeType(n->type) || n->style.overflow == 0) return 0;
	if (n->first_child < 0) return 0;

	int x, y, w, h;
	overflowClipBounds(node, x, y, w, h);
	DisplayCommand *cmd = DisplayList::instance().append();
	if (cmd) {
		cmd->type = DisplayCommandType::PushClip;
		cmd->bx = cmd->clip.x = x;
		cmd->by = cmd->clip.y = y;
		cmd->bw = cmd->clip.w = w;
		cmd->bh = cmd->clip.h = h;
		cmd->clip.nodeId = static_cast<int16_t>(&node - Tree::instance().nodes());
	}
	return 1;
}

int ViewRenderer::scrollMaxX(const Node &node)
{
	if (node.type == NodeType::VirtualList) return 0;
	int max_x = node.layout.scroll_content_width - node.layout.width;
	return max_x > 0 ? max_x : 0;
}

int ViewRenderer::scrollMaxY(const Node &node)
{
	if (node.type == NodeType::VirtualList) {
		const int id = static_cast<int>(&node - Tree::instance().nodes());
		return VirtualListRenderer::scrollMaxY(id);
	}
	int max_y = node.layout.scroll_content_height - node.layout.height;
	return max_y > 0 ? max_y : 0;
}

void ViewRenderer::recordClipEnd(const Node &node)
{
	const Node *n = &node;
	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::PopClip;
	cmd->bx = n->layout.x;
	cmd->by = n->layout.y;
	cmd->bw = n->layout.width;
	cmd->bh = n->layout.height;
	cmd->clip.nodeId = static_cast<int16_t>(&node - Tree::instance().nodes());
}

int ViewRenderer::canvasBackgroundSource()
{
	const auto &state = treeState();
	const int root = state.mountedRoot;
	if (root < 0 || root >= state.nodeCount || !isDocumentCanvasRoot(state.nodes[root])) return -1;
	const Node &html = state.nodes[root];
	// CSS Backgrounds: a transparent root with no images takes the first
	// direct body's background. Its used background is then transparent.
	if (!rstyle(html.style).containment && (!html.style.has_bg || html.style.bg_alpha == 0) && !styleHasBackgroundImage(html.style)) {
		for (int child = html.first_child; child >= 0; child = state.nodes[child].next_sibling)
			if (std::string_view(tagFromId(state.nodes[child].tag_id)) == "body")
				return rstyle(state.nodes[child].style).containment ? root : child;
	}
	return root;
}

void GEA_VIEW_HOT_SRAM_SECTION("view_renderer_record_box") ViewRenderer::recordBox(const Node &node, uint8_t parentAlpha)
{
	const Node *n = &node;
	int x = n->layout.x;
	int y = n->layout.y;
	int w = n->layout.width;
	int h = n->layout.height;
	const int canvasSource = canvasBackgroundSource();
	const bool canvasRoot = isDocumentCanvasRoot(node);
	if (canvasRoot && canvasSource >= 0) {
		const auto &state = treeState();
		const auto &source = state.nodes[canvasSource].style;
		if (source.display != 1 && source.has_bg && source.bg_alpha > 0)
			appendFillRectWithAlpha(0, 0, state.mountedWidth, state.mountedHeight,
			    source.bg_color, source.bg_alpha, parentAlpha, 0, 0, state.mountedWidth, state.mountedHeight);
		if (source.display != 1) {
			const auto &imageSource = state.nodes[canvasSource];
			if (!recordPlacedBackgrounds(node, imageSource, true)) {
				if (source.bg_fill == 1) recordLinearGradientBackground(imageSource, parentAlpha);
				recordRadialGradientBackground(imageSource); recordOverlayLinearGradientBackground(imageSource);
			}
			recordBackgroundGrid(imageSource, parentAlpha);
		}
	}
	if (w <= 0 || h <= 0) return;

	if (n->style.has_bg) {
		// The color is the bottom layer; image longhands never replace it.
		if (n->style.bg_alpha > 0 && !canvasRoot &&
		    (canvasSource < 0 || &treeState().nodes[canvasSource] != n)) {
			const int clip = StyleValues::backgroundClip(n->style, rstyle(n->style).bg_image_layer_count - 1);
			TextBackgroundClipScope textClip(*n, clip);
			if (clip == 1 || clip == 2) {
				recordTransformedRoundedRectFill(*n, parentAlpha, clip);
			} else if (ViewGeometry::hasTransformChain(*n, false)) {
				int16_t xs[4], ys[4];
				int bx0, by0, bx1, by1;
				ViewRenderer::transformedBounds(*n, false, &bx0, &by0, &bx1, &by1);
				if (hasAnyRadius(*n)) {
					recordTransformedRoundedRectFill(*n, parentAlpha);
				} else {
					ViewGeometry::transformCorners(*n, false, xs, ys);
					appendFillQuadWithAlpha(xs, ys, n->style.bg_color, n->style.bg_alpha, parentAlpha,
					                        bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1,
					                        x, y, w, h, true);
				}
			} else {
				if (hasAnyRadius(*n)) {
					if (!appendResolvedCircularRoundedRectWithAlpha(*n, parentAlpha))
						recordTransformedRoundedRectFill(*n, parentAlpha);
				} else {
					appendFillRectWithAlpha(x, y, w, h, n->style.bg_color, n->style.bg_alpha, parentAlpha, x, y, w, h);
				}
			}
		}
		if (!canvasRoot && (canvasSource < 0 || &treeState().nodes[canvasSource] != n)) {
			if (!recordPlacedBackgrounds(*n, *n, false)) {
				if (n->style.bg_fill == 1) {
					TextBackgroundClipScope textClip(*n, StyleValues::backgroundClip(n->style, rstyle(n->style).bg_gradient_layer));
					recordLinearGradientBackground(*n, parentAlpha);
				}
				{
					TextBackgroundClipScope textClip(*n, StyleValues::backgroundClip(n->style, rstyle(n->style).bg_radial_gradient_layer));
					recordRadialGradientBackground(*n);
				}
				{
					TextBackgroundClipScope textClip(*n, StyleValues::backgroundClip(n->style, rstyle(n->style).bg_overlay_gradient_layer));
					recordOverlayLinearGradientBackground(*n);
				}
			}
			recordBackgroundGrid(*n, parentAlpha);
		}
	}

	recordInsetBoxShadow(*n, parentAlpha);

	if (n->style.border_width > 0 && !hasBorderRelief(n->style) && !hasSideBorder(n->style) && !borderColorsDiffer(n->style) && !borderIsSameOpaqueSolidBackground(*n, parentAlpha)) {
		if (ViewGeometry::hasTransformChain(*n, false)) {
			if (isFullyRoundedShape(*n)) recordTransformedEllipseStroke(*n, parentAlpha);
		} else {
			appendStrokeWithAlpha(*n, parentAlpha);
		}
	}
	appendSideBordersWithAlpha(*n, parentAlpha);
}

void ViewRenderer::recordScrollbar(const Node &node)
{
	const Node *n = &node;
	if (!isViewLikeNodeType(n->type) || (n->type != NodeType::VirtualList && !scrollsOverflowY(n->style))) return;
	if (n->layout.height <= 0 || n->layout.scroll_content_height <= n->layout.height) return;

	int track_h = n->layout.height - 12;
	if (track_h < 24) return;

	int thumb_h = (n->layout.height * track_h) / n->layout.scroll_content_height;
	if (thumb_h < 24) thumb_h = 24;
	if (thumb_h > track_h) thumb_h = track_h;

	int max_scroll = n->layout.scroll_content_height - n->layout.height;
	int thumb_y = n->layout.y + 6;
	if (max_scroll > 0)
		thumb_y += (n->layout.scroll_y * (track_h - thumb_h)) / max_scroll;

	DisplayCommand *cmd = DisplayList::instance().append();
	if (!cmd) return;
	cmd->type = DisplayCommandType::FillRoundedRect;
	cmd->bx = n->layout.x + n->layout.width - 7;
	cmd->by = thumb_y;
	cmd->bw = 3;
	cmd->bh = thumb_h;
	cmd->fillRoundedRect.x = cmd->bx;
	cmd->fillRoundedRect.y = cmd->by;
	cmd->fillRoundedRect.w = cmd->bw;
	cmd->fillRoundedRect.h = cmd->bh;
	cmd->fillRoundedRect.tl = 2;
	cmd->fillRoundedRect.tr = 2;
	cmd->fillRoundedRect.br = 2;
	cmd->fillRoundedRect.bl = 2;
	// Command colours are panel-order natives, so this has to go through the one
	// authoring conversion (nativeFromRrggbbaa): it applies the panel byte-swap a
	// bare fromRgb565 would leave off on 16-bit panels AND narrows to the board's
	// native_t on the grayscale panels, where native_t is a byte and the RGB565
	// constant would not fit. 0x8C8E94 is the exact RGB888 expansion of the RGB565
	// 0x8C72 this used to spell, so 16-bit panels emit the identical pixel.
	cmd->fillRoundedRect.color = gea::framework::graphics::pixel::nativeFromRrggbbaa(0x8C8E94FFu);
}

}  // namespace gea::embedded::ui
