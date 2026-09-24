// SPDX-License-Identifier: Apache-2.0
#include "absolute_leaf_refresh.h"
#include "dirty_regions.h"
#include "display.h"
#include "document.h"
#include "internal.h"
#include "layout_snapshot.h"
#include "refresh_perf.h"
#include "root_scroll_refresh.h"
#include "tree_state.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef GEA_EMBEDDED_FLUSH_ANCHOR_LEFT_PARTIAL_RECTS
#define GEA_EMBEDDED_FLUSH_ANCHOR_LEFT_PARTIAL_RECTS 1
#endif

// Fused replay+flush: rasterize each dirty chunk directly into the DMA buffer during
// the flush, instead of replaying into the PSRAM framebuffer and then copying. Off by
// default; opt-in per board. Only valid for simpleUnifiedReplay apps whose display list
// fully repaints each dirty region from scratch (opaque background + draws — e.g.
// bouncing-balls), because the chunk buffer is not seeded from the framebuffer.
#ifndef GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH
#define GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH 0
#endif

#ifndef GEA_EMBEDDED_DISPLAY_FUSE_INTERLEAVED_REPLAY_FLUSH
#define GEA_EMBEDDED_DISPLAY_FUSE_INTERLEAVED_REPLAY_FLUSH 0
#endif

#ifndef GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_CLEAR_RGB565
#define GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_CLEAR_RGB565 -1
#endif

#ifndef GEA_EMBEDDED_REPROJECT_UNIFIED_DIRTY_RECT
#define GEA_EMBEDDED_REPROJECT_UNIFIED_DIRTY_RECT 0
#endif

#ifndef GEA_EMBEDDED_BROAD_DIRTY_BBOX
#define GEA_EMBEDDED_BROAD_DIRTY_BBOX 0
#endif

#ifndef GEA_EMBEDDED_FLUSH_COALESCE_DIRTY_REGIONS
#define GEA_EMBEDDED_FLUSH_COALESCE_DIRTY_REGIONS 1
#endif

#ifndef GEA_EMBEDDED_REPLAY_COALESCE_DIRTY_REGIONS
#define GEA_EMBEDDED_REPLAY_COALESCE_DIRTY_REGIONS 0
#endif

#ifndef GEA_EMBEDDED_RETAINED_MOVE_DIRTY_GUARD_PX
#define GEA_EMBEDDED_RETAINED_MOVE_DIRTY_GUARD_PX 2
#endif

#ifndef GEA_EMBEDDED_TEXT_DIRTY_GUARD_PX
#define GEA_EMBEDDED_TEXT_DIRTY_GUARD_PX 2
#endif

// subtreeRevealsUnrecordedContent (render.cpp) is an O(nodeCount) whole-tree scan; the
// per-node keep-list guard below calls it once per content-dirty / translated node, so a
// scene with many independently-moving nodes (bouncing-balls: 64 balls translate every
// frame) pays O(N^2) reveal-checks. It exists to reveal content that was culled outside
// the record-time clip and later slid into view (sky-hop pan). Apps whose moving content
// stays within the recorded viewport can't reveal anything, so the check is pure cost.
#ifndef GEA_EMBEDDED_SUBTREE_REVEAL_CHECK
#define GEA_EMBEDDED_SUBTREE_REVEAL_CHECK 1
#endif

namespace gea::embedded::ui
{
	namespace
	{

#if GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH
		struct FusedReplayRasterCtx
		{
			int screenW;
			int screenH;
		};

		// Display::flushRectsRasterized callback. For each DMA chunk [ox,oy]..[ox+w-1,oy+h-1]
		// it rebinds the draw canvas ONTO the chunk buffer — an offset base pointer with the
		// chunk stride, so absolute screen (x,y) writes land at pixels[(y-oy)*w + (x-ox)] —
		// then replays the dirty display-list clipped to the chunk straight into it. The
		// caller (Display) DMAs the chunk and restores the canvas via rebindCanvasToFramebuffer.
		void fusedReplayRaster(gea::framework::graphics::pixel::native_t *pixels, int width, int height, int ox, int oy, void *user)
		{
			auto *ctx = static_cast<FusedReplayRasterCtx *>(user);
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !ctx)
				return;
#if GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_CLEAR_RGB565 >= 0
			std::fill_n(pixels,
			            static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
			            static_cast<gea::framework::graphics::pixel::native_t>(GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_CLEAR_RGB565));
#endif
			using PixT = gea::framework::graphics::pixel::native_t;
			PixT *base = reinterpret_cast<PixT *>(pixels) -
									 (static_cast<std::ptrdiff_t>(oy) * width + ox);
			canvas->bindPixels(base, ctx->screenW, ctx->screenH, width);
			gea::platform::display::Display::resetClip();
			gea::platform::display::Display::setAlpha(255);
			gea::platform::display::Display::pushClip(ox, oy, width, height);
			DisplayList::instance().replaySimpleClippedDirtyRegion(ox, oy, ox + width - 1, oy + height - 1, -1);
			gea::platform::display::Display::popClip();
		}

		void fusedDirectReplayRaster(gea::framework::graphics::pixel::native_t *pixels, int width, int height, int ox, int oy, void *user)
		{
			auto *ctx = static_cast<FusedReplayRasterCtx *>(user);
			auto *canvas = gea::platform::display::Display::canvas();
			if (!canvas || !ctx)
				return;
#if GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_CLEAR_RGB565 >= 0
			std::fill_n(pixels,
			            static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
			            static_cast<gea::framework::graphics::pixel::native_t>(GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_CLEAR_RGB565));
#endif
			using PixT = gea::framework::graphics::pixel::native_t;
			PixT *base = reinterpret_cast<PixT *>(pixels) -
									 (static_cast<std::ptrdiff_t>(oy) * width + ox);
			canvas->bindPixels(base, ctx->screenW, ctx->screenH, width);
			gea::platform::display::Display::resetClip();
			gea::platform::display::Display::setAlpha(255);
			gea::platform::display::Display::pushClip(ox, oy, width, height);
			const DisplayReplayRegion region{ox, oy, ox + width - 1, oy + height - 1, -1};
			DisplayList::instance().replayDirectDirtyRegions(&region, 1);
			gea::platform::display::Display::popClip();
		}
#endif

		struct PreservedScrollOffset
		{
			int32_t x = 0;
			int32_t y = 0;
		};

		void preserveScrollOffsets(PreservedScrollOffset *offsets, int count)
		{
			if (!offsets || count <= 0)
				return;
			auto &state = treeState();
			const int n = state.nodeCount < count ? state.nodeCount : count;
			for (int i = 0; i < n; ++i)
			{
				offsets[i].x = state.nodes[i].layout.scroll_x;
				offsets[i].y = state.nodes[i].layout.scroll_y;
			}
		}

		void restoreScrollOffsetsAfterLayout(const PreservedScrollOffset *offsets, int count)
		{
			if (!offsets || count <= 0)
				return;
			auto &state = treeState();
			const int n = state.nodeCount < count ? state.nodeCount : count;
			for (int i = 0; i < n; ++i)
			{
				Node &node = state.nodes[i];
				if (node.type == NodeType::VirtualList)
				{
					node.layout.scroll_x = 0;
					int y = offsets[i].y;
					const int maxY = VirtualListRenderer::scrollMaxY(i);
					if (y < 0)
						y = 0;
					if (y > maxY)
						y = maxY;
					node.layout.scroll_y = static_cast<int32_t>(y);
					continue;
				}
				if (node.style.overflow != 2)
				{
					node.layout.scroll_x = 0;
					node.layout.scroll_y = 0;
					continue;
				}
				if (scrollsOverflowX(node.style))
				{
					int x = offsets[i].x;
					const int maxX = ViewRenderer::scrollMaxX(node);
					if (x < 0)
						x = 0;
					if (x > maxX)
						x = maxX;
					node.layout.scroll_x = static_cast<int32_t>(x);
				}
				else
				{
					node.layout.scroll_x = 0;
				}

				if (scrollsOverflowY(node.style))
				{
					int y = offsets[i].y;
					const int maxY = ViewRenderer::scrollMaxY(node);
					if (y < 0)
						y = 0;
					if (y > maxY)
						y = maxY;
					node.layout.scroll_y = static_cast<int32_t>(y);
				}
				else
				{
					node.layout.scroll_y = 0;
				}
			}
		}

		int dirtyRectArea(const DirtyRegions::Rect &r)
		{
			if (r.x0 > r.x1 || r.y0 > r.y1)
				return 0;
			return (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
		}

		bool dirtyRectContains(const DirtyRegions::Rect &outer, const DirtyRegions::Rect &inner)
		{
			return outer.x0 <= inner.x0 &&
						 outer.y0 <= inner.y0 &&
						 outer.x1 >= inner.x1 &&
						 outer.y1 >= inner.y1;
		}

		bool dirtyRectCoveredByAny(const DirtyRegions::Rect *rects, int count, const DirtyRegions::Rect &inner)
		{
			if (!rects || count <= 0)
				return false;
			for (int i = 0; i < count; ++i)
			{
				if (dirtyRectContains(rects[i], inner))
					return true;
			}
			return false;
		}

		bool styleHasRoundedRasterEdge(const ComputedStyle &style)
		{
			for (int i = 0; i < 4; ++i)
			{
				if (style.border_radius[i] > 0)
					return true;
				if (style.border_radius_percent[i] != kUnset && style.border_radius_percent[i] > 0)
					return true;
			}
			return false;
		}

		bool dirtyRectNeedsRasterGuard(const Node &node)
		{
			if (hasAnyBorder(node.style))
				return true;
			if (rstyle(node.style).filter_blur_radius > 0 || node.render.previous_filter_blur_radius > 0)
				return true;
			if (node.style.mask_right_fade_width > 0)
				return true;
			if (rstyle(node.style).box_shadow_alpha > 0)
				return true;
			return node.style.has_bg && (styleHasRoundedRasterEdge(node.style) || node.style.bg_alpha < 255);
		}

		constexpr int kRetainedMoveDirtyGuardPx = GEA_EMBEDDED_RETAINED_MOVE_DIRTY_GUARD_PX;

		DirtyRegions::Rect expandDirtyRect(DirtyRegions::Rect rect, int pad)
		{
			if (pad <= 0)
				return rect;
			rect.x0 -= pad;
			rect.y0 -= pad;
			rect.x1 += pad;
			rect.y1 += pad;
			return rect;
		}

		DirtyRegions::Rect dirtyRectWithRasterGuard(const Node &node, DirtyRegions::Rect rect)
		{
			if (!dirtyRectNeedsRasterGuard(node))
				return rect;
			return expandDirtyRect(rect, 1);
		}

		DirtyRegions::Rect dirtyRectWithRetainedMoveGuard(DirtyRegions::Rect rect)
		{
			// Retained absolute movement translates existing display commands and then
			// streams only the collected dirty bands to the panel. Exact prev-box ∪
			// cur-box math is too brittle here: a one-row miss leaves visible residue
			// because no later full-frame copy cleans it up. Keep the replay/flush
			// window slightly conservative for every retained move.
			return expandDirtyRect(rect, kRetainedMoveDirtyGuardPx);
		}

		DirtyRegions::Rect uniteDirtyRects(const DirtyRegions::Rect &a, const DirtyRegions::Rect &b)
		{
			DirtyRegions::Rect out = a;
			if (b.x0 < out.x0)
				out.x0 = b.x0;
			if (b.y0 < out.y0)
				out.y0 = b.y0;
			if (b.x1 > out.x1)
				out.x1 = b.x1;
			if (b.y1 > out.y1)
				out.y1 = b.y1;
			if (a.origin != b.origin)
				out.origin = -1;
			return out;
		}

		void coalesceHighCoverageDirtyRegions(DirtyRegions::Rect *rects, int *rect_count, int width, int height)
		{
			if (!rects || !rect_count || *rect_count < 2 || width <= 0 || height <= 0)
				return;

			DirtyRegions::Rect bounds = rects[0];
			int total_area = 0;
			for (int i = 0; i < *rect_count; i++)
			{
				total_area += dirtyRectArea(rects[i]);
				bounds = uniteDirtyRects(bounds, rects[i]);
			}

			const int screen_area = width * height;
			const bool many_rects = (*rect_count * *rect_count) >= DirtyRegions::kMaxRects;
			const bool broad_paint = total_area >= screen_area - total_area;
			if (!many_rects || !broad_paint)
				return;

			bounds.origin = -1;
			rects[0] = bounds;
			*rect_count = 1;
		}

// Gap-merge tightness for coalesceLowCostDirtyRegions. The greedy pass merges a pair when
// the gap it adds is <= (smaller rect area >> SHIFT). SHIFT=0 = original behavior (allow a
// gap as large as the smaller rect), which chains across scattered rects into wide windows
// full of unchanged background — fine when per-window flush setup is expensive, but on a
// board whose flush pays per *pixel* (PSRAM copy) and ~0 per window, those gap pixels are
// pure waste that spike the copy and miss the TE. Raise SHIFT on such boards to keep merges
// to near-touching rects only. flush_rects stays a superset of the dirty rects either way,
// so tighter windows never leave stale pixels.
#ifndef GEA_EMBEDDED_DIRTY_COALESCE_GAP_SHIFT
#define GEA_EMBEDDED_DIRTY_COALESCE_GAP_SHIFT 0
#endif

		void coalesceLowCostDirtyRegions(DirtyRegions::Rect *rects, int *rect_count)
		{
			if (!rects || !rect_count || *rect_count < 2)
				return;

			while (*rect_count > 1)
			{
				int bestI = -1;
				int bestJ = -1;
				int bestExtra = 0x7fffffff;
				int bestAllowance = 0;

				for (int i = 0; i < *rect_count; i++)
				{
					const int areaI = dirtyRectArea(rects[i]);
					if (areaI <= 0)
						continue;
					for (int j = i + 1; j < *rect_count; j++)
					{
						const int areaJ = dirtyRectArea(rects[j]);
						if (areaJ <= 0)
							continue;
						const DirtyRegions::Rect merged = uniteDirtyRects(rects[i], rects[j]);
						const int extra = dirtyRectArea(merged) - areaI - areaJ;
						const int allowance = (areaI < areaJ ? areaI : areaJ) >> GEA_EMBEDDED_DIRTY_COALESCE_GAP_SHIFT;
						if (extra < bestExtra)
						{
							bestExtra = extra;
							bestAllowance = allowance;
							bestI = i;
							bestJ = j;
						}
					}
				}

				if (bestI < 0 || bestJ < 0 || bestExtra > bestAllowance)
					return;
				rects[bestI] = uniteDirtyRects(rects[bestI], rects[bestJ]);
				rects[bestJ] = rects[*rect_count - 1];
				(*rect_count)--;
			}
		}

		int nodeBoxWidth(const Node &node, bool usePrevious)
		{
			return usePrevious ? node.layout.previous_width : node.layout.width;
		}

		int nodeBoxHeight(const Node &node, bool usePrevious)
		{
			return usePrevious ? node.layout.previous_height : node.layout.height;
		}

		int nodeBoxRotation(const Node &node, bool usePrevious)
		{
			return usePrevious ? node.render.previous_transform_rotate : rstyle(node.style).transform_rotate;
		}

		bool hasTransformState(const Node &node)
		{
			const RareStyle &rs = rstyle(node.style); // one pool lookup, not 11
			return hasIndividualLinearTransform(rs) || hadIndividualLinearTransform(node.render) ||
	       rs.transform_rotate != 0 ||
						 node.render.previous_transform_rotate != 0 ||
						 rs.transform_rotate_x != 0 ||
						 node.render.previous_transform_rotate_x != 0 ||
						 rs.transform_rotate_y != 0 ||
						 node.render.previous_transform_rotate_y != 0 ||
						 composedTranslateX(rs) != 0 ||
						 node.render.previous_transform_translate_x != 0 ||
						 composedTranslateY(rs) != 0 ||
						 node.render.previous_transform_translate_y != 0 ||
						 composedTranslateZ(rs) != 0 ||
						 node.render.previous_transform_translate_z != 0 ||
						 composedTranslateXPercent(rs) != 0 ||
						 node.render.previous_transform_translate_x_percent != 0 ||
						 composedTranslateYPercent(rs) != 0 ||
						 node.render.previous_transform_translate_y_percent != 0 ||
						 rs.transform_scale_x != 1000 ||
						 node.render.previous_transform_scale_x != 1000 ||
						 rs.transform_scale_y != 1000 ||
						 rs.transform_scale_z != 1000 ||
						 node.render.previous_transform_scale_y != 1000 ||
						 node.render.previous_transform_scale_z != 1000 ||
						 rs.perspective > 0 ||
						 node.render.previous_perspective > 0;
		}

		bool hasThreeDimensionalTransformState(const Node &node)
		{
			const auto &r = rstyle(node.style);
			return ((r.rotate_angle % 3600) && (r.rotate_axis_x || r.rotate_axis_y)) ||
			    ((node.render.previous_rotate_angle % 3600) && (node.render.previous_rotate_axis_x || node.render.previous_rotate_axis_y)) ||
			    r.scale_z != 1000 || node.render.previous_scale_z != 1000 ||
			    rstyle(node.style).transform_rotate_x != 0 ||
						 node.render.previous_transform_rotate_x != 0 ||
						 rstyle(node.style).transform_rotate_y != 0 ||
						 node.render.previous_transform_rotate_y != 0 ||
						 composedTranslateZ(rstyle(node.style)) != 0 ||
						 node.render.previous_transform_translate_z != 0 ||
						 rstyle(node.style).perspective > 0 ||
						 node.render.previous_perspective > 0;
		}

		bool clipDirtyRect(DirtyRegions::Rect *rect, int width, int height)
		{
			if (!rect || rect->x0 > rect->x1 || rect->y0 > rect->y1)
				return false;
			if (rect->x0 < 0)
				rect->x0 = 0;
			if (rect->y0 < 0)
				rect->y0 = 0;
			if (rect->x1 >= width)
				rect->x1 = width - 1;
			if (rect->y1 >= height)
				rect->y1 = height - 1;
			return rect->x0 <= rect->x1 && rect->y0 <= rect->y1;
		}

		// Set for a frame whose dirty set is "broad" — many scattered movers (e.g. 64
		// bouncing balls) whose union the flush will coalesce to its bounding box
		// anyway (FLUSH_ADAPTIVE_COALESCE), and where the wide bbox flush is faster
		// than precise windows on this QSPI board. In that case the precise
		// O(n*kMaxRects) DirtyRegions::add merge (+ the later coalesce passes) is pure
		// wasted CPU: it produces precise rects that are immediately discarded for the
		// bbox. So accumulate ONE bbox per array (O(1) per add) instead. Single-
		// threaded refresh, so a file-static flag is safe; reset after the collect.
		bool g_broadDirtyBbox = false;

		void addDirtyRegion(DirtyRegions::Rect *rects, int *rect_count, DirtyRegions::Rect rect, int width, int height)
		{
			if (!clipDirtyRect(&rect, width, height))
				return;
			if (g_broadDirtyBbox)
			{
				if (*rect_count == 0)
					rects[(*rect_count)++] = rect;
				else
					rects[0] = uniteDirtyRects(rects[0], rect);
				return;
			}
			DirtyRegions::add(rects, rect_count, DirtyRegions::kMaxRects, rect);
		}

		void appendDirtyRegion(DirtyRegions::Rect *rects, int *rect_count, DirtyRegions::Rect rect, int width, int height)
		{
			if (!clipDirtyRect(&rect, width, height))
				return;
			if (!rects || !rect_count || *rect_count >= DirtyRegions::kMaxRects)
				return;
			rects[(*rect_count)++] = rect;
		}

		bool resolveNodeCircularRadii(const Node &node, int radii[4])
		{
			if (!radii || node.layout.width <= 0 || node.layout.height <= 0)
				return false;
			const double width = static_cast<double>(node.layout.width);
			const double height = static_cast<double>(node.layout.height);
			double rx[4]{};
			double ry[4]{};
			for (int i = 0; i < 4; ++i)
			{
				if (node.style.border_radius_percent[i] != kUnset)
				{
					const double p = static_cast<double>(node.style.border_radius_percent[i]) / 1000.0;
					rx[i] = std::max(0.0, width * p);
					ry[i] = std::max(0.0, height * p);
				}
				else
				{
					const double r = static_cast<double>(std::max(0, static_cast<int>(node.style.border_radius[i])));
					rx[i] = r;
					ry[i] = r;
				}
			}

			double scale = 1.0;
			const auto constrain = [&](double limit, double sum)
			{
				if (limit > 0.0 && sum > limit)
					scale = std::min(scale, limit / sum);
			};
			constrain(width, rx[0] + rx[1]);
			constrain(width, rx[3] + rx[2]);
			constrain(height, ry[0] + ry[3]);
			constrain(height, ry[1] + ry[2]);

			for (int i = 0; i < 4; ++i)
			{
				const double sx = rx[i] * scale;
				const double sy = ry[i] * scale;
				if (std::abs(sx - sy) > 1.0)
					return false;
				radii[i] = static_cast<int>(std::lround((sx + sy) * 0.5));
			}
			return true;
		}

		bool nodeIsCircleLikeRoundedBackground(const Node &node)
		{
			const int w = static_cast<int>(node.layout.width);
			const int h = static_cast<int>(node.layout.height);
			const int minSide = std::min(w, h);
			if (minSide < 96 || std::abs(w - h) > 2)
				return false;
			int radii[4]{};
			if (!resolveNodeCircularRadii(node, radii))
				return false;
			const int minRadius = std::max(1, minSide / 2 - 2);
			return radii[0] >= minRadius &&
						 radii[1] >= minRadius &&
						 radii[2] >= minRadius &&
						 radii[3] >= minRadius;
		}

		int appendCircleLikeRecolorFlushRegions(DirtyRegions::Rect *rects,
																						int *rect_count,
																						const Node &node,
																						const DirtyRegions::Rect &recolorRect,
																						int width,
																						int height)
		{
			if (!nodeIsCircleLikeRoundedBackground(node))
				return 0;
			if (!rects || !rect_count || *rect_count >= DirtyRegions::kMaxRects)
				return 0;
			const int available = DirtyRegions::kMaxRects - *rect_count;
			const int bandCount = std::min(DirtyRegions::kMaxRects, available);
			if (bandCount < 4)
				return 0;

			const double cx = static_cast<double>(node.layout.x) + static_cast<double>(node.layout.width) * 0.5;
			const double cy = static_cast<double>(node.layout.y) + static_cast<double>(node.layout.height) * 0.5;
			const double rx = static_cast<double>(node.layout.width) * 0.5;
			const double ry = static_cast<double>(node.layout.height) * 0.5;
			if (rx <= 0.0 || ry <= 0.0)
				return 0;

			const int y0 = recolorRect.y0;
			const int y1 = recolorRect.y1;
			if (y0 > y1)
				return 0;
			const int rows = y1 - y0 + 1;
			const int pad = 4;
			int pixels = 0;
			for (int band = 0; band < bandCount; ++band)
			{
				const int by0 = y0 + (rows * band) / bandCount;
				const int by1 = y0 + (rows * (band + 1)) / bandCount - 1;
				if (by0 > by1)
					continue;
				const double upper = static_cast<double>(by0) + 0.5;
				const double lower = static_cast<double>(by1) + 0.5;
				const double sampleY = cy < upper ? upper : (cy > lower ? lower : cy);
				const double dy = (sampleY - cy) / ry;
				const double dx = dy * dy >= 1.0 ? 0.0 : rx * std::sqrt(1.0 - dy * dy);
				int bx0 = static_cast<int>(std::floor(cx - dx)) - pad;
				int bx1 = static_cast<int>(std::ceil(cx + dx)) + pad;
				if (bx0 < recolorRect.x0)
					bx0 = recolorRect.x0;
				if (bx1 > recolorRect.x1)
					bx1 = recolorRect.x1;
				DirtyRegions::Rect bandRect{bx0, by0, bx1, by1, recolorRect.origin};
				const int before = *rect_count;
				appendDirtyRegion(rects, rect_count, bandRect, width, height);
				if (*rect_count != before)
					pixels += dirtyRectArea(rects[*rect_count - 1]);
			}
			return pixels;
		}

		bool canUseStripDirtyForTransformedLeaf(const Node &node)
		{
			return isViewLikeNodeType(node.type) &&
						 node.first_child < 0 &&
						 node.style.display != 1 &&
						 node.style.has_bg &&
						 !hasAnyBorder(node.style) &&
						 hasTransformState(node);
		}

		bool canUseStaticBackdropStripDirtyForTransformedLeaf(const Node &node)
		{
			return canUseStripDirtyForTransformedLeaf(node) &&
						 !hasThreeDimensionalTransformState(node);
		}

		bool layoutSizeChangeNeedsCommandRerecord(const Node &node)
		{
			if (node.layout.width == node.layout.previous_width &&
					node.layout.height == node.layout.previous_height)
				return false;
			if (node.type == NodeType::Text ||
					node.type == NodeType::Image ||
					node.type == NodeType::Canvas)
				return true;
			if (node.style.has_bg || hasAnyBorder(node.style))
				return true;
			if (rstyle(node.style).box_shadow_alpha > 0 || rstyle(node.style).filter_blur_radius > 0)
				return true;
			if (node.style.mask_right_fade_width > 0)
				return true;
			return false;
		}

		int transformedLeafDirtyShapeCount()
		{
			auto &state = treeState();
			int count = 0;
			for (int i = 0; i < state.nodeCount; i++)
			{
				const Node &node = state.nodes[i];
				if (!node.render.dirty || !canUseStripDirtyForTransformedLeaf(node))
					continue;
				if (node.layout.previous_width > 0 && node.layout.previous_height > 0)
					count++;
				if (node.layout.width > 0 && node.layout.height > 0)
					count++;
			}
			return count;
		}

		bool transformedLeafDirtyShapesAreFlat2D()
		{
			auto &state = treeState();
			bool any = false;
			for (int i = 0; i < state.nodeCount; i++)
			{
				const Node &node = state.nodes[i];
				if (!node.render.dirty || !canUseStripDirtyForTransformedLeaf(node))
					continue;
				any = true;
				if (hasThreeDimensionalTransformState(node))
					return false;
			}
			return any;
		}

		// A node counts as dirty for the settle verdict when either its render state or
		// its recorded commands changed this frame.
		bool staticBackdropNodeIsDirty(int node)
		{
			auto &state = treeState();
			if (node < 0 || node >= state.nodeCount)
				return false;
			return state.nodes[node].render.dirty || state.nodeCommandDirty[node];
		}

		// True when some PROPER ancestor of `node` is itself dirty — i.e. this node sits
		// inside another dirty subtree and is therefore not a root of the change.
		bool staticBackdropHasDirtyAncestor(int node)
		{
			auto &state = treeState();
			if (node < 0 || node >= state.nodeCount)
				return false;
			for (int p = state.nodes[node].parent; p >= 0 && p < state.nodeCount; p = state.nodes[p].parent)
			{
				if (staticBackdropNodeIsDirty(p))
					return true;
			}
			return false;
		}

		bool commandsAreSameOpaqueFillShape(DisplayCommand oldCommand, DisplayCommand newCommand)
		{
			if (oldCommand.type != newCommand.type)
				return false;
			if (oldCommand.bx != newCommand.bx ||
					oldCommand.by != newCommand.by ||
					oldCommand.bw != newCommand.bw ||
					oldCommand.bh != newCommand.bh)
				return false;
			switch (oldCommand.type)
			{
			case DisplayCommandType::FillRect:
				oldCommand.fill.color = newCommand.fill.color;
				return std::memcmp(&oldCommand.fill, &newCommand.fill, sizeof(oldCommand.fill)) == 0;
			case DisplayCommandType::FillCircle:
				oldCommand.fillCircle.color = newCommand.fillCircle.color;
				return std::memcmp(&oldCommand.fillCircle,
													 &newCommand.fillCircle,
													 sizeof(oldCommand.fillCircle)) == 0;
			case DisplayCommandType::FillRoundedRect:
				if (gea::platform::display::Display::aa() >= 2 &&
						(oldCommand.fillRoundedRect.tl || oldCommand.fillRoundedRect.tr ||
						 oldCommand.fillRoundedRect.br || oldCommand.fillRoundedRect.bl))
					return false;
				oldCommand.fillRoundedRect.color = newCommand.fillRoundedRect.color;
				return std::memcmp(&oldCommand.fillRoundedRect,
													 &newCommand.fillRoundedRect,
													 sizeof(oldCommand.fillRoundedRect)) == 0;
			case DisplayCommandType::FillTransformedRoundedRect:
				if (gea::platform::display::Display::aa() >= 2)
					return false;
				oldCommand.transformedRoundedRect.color = newCommand.transformedRoundedRect.color;
				return std::memcmp(&oldCommand.transformedRoundedRect,
													 &newCommand.transformedRoundedRect,
													 sizeof(oldCommand.transformedRoundedRect)) == 0;
			default:
				return false;
			}
		}

		// Frame-stability verdict for the static-backdrop cache.
		// kDynamic — exactly one dynamic (transformed) subtree is moving: the shape the
		//            bake was designed for; eligible to settle/bake and to arm reproject.
		// kQuiet   — nothing dynamic this frame (eased animations at a keyframe pause,
		//            or only tolerated ticker leaves changed): the scene is MORE static
		//            than ever — a live cache must be KEPT, not dropped. Treating quiet
		//            as unstable made every fps-badge tick that landed inside the
		//            cube-spin's eased pauses drop the cache, and the resume re-bake +
		//            full-viewport sync was the ~90ms frame behind the loop-wrap fps dip.
		// kFailed  — a non-bakeable change happened (recolor in flight, a static node's
		//            content changed, two independent dynamic roots): the baked pixels
		//            can be stale, drop the cache.
		enum class BackdropSettle
		{
			kFailed,
			kQuiet,
			kDynamic
		};

		BackdropSettle staticBackdropFrameSettle(bool debugPrint = false)
		{
#ifndef ESP_PLATFORM
			static const bool debugEnv = std::getenv("GEA_DEBUG_BACKDROP") != nullptr;
			const bool debugSettle = debugPrint || debugEnv;
#else
			const bool debugSettle = debugPrint;
#endif
			auto &state = treeState();
			// The verdict is a question about the TREE, not about array order: does ONE
			// dirty subtree contain every other dirty node? This used to be answered by a
			// single forward scan that anchored on the first dirty node and skipped later
			// nodes that were its descendants — correct only while node indices arrived in
			// PRE-order, where the first dirty node is necessarily the topmost one.
			// The JSX runtime now builds children before their parent (gea::jsx::create for
			// the child, then gea::jsx::child on the parent), so indices arrive in
			// POST-order and the first dirty node is the DEEPEST. Anchoring on it made a
			// node's own PARENT look like a second, independent dynamic root: css-3d-cube's
			// spinning .cube (id 22) anchored, then its floating .cube-wrap (id 23) was
			// rejected, so the live backdrop cache was dropped and re-baked EVERY frame —
			// 72fps -> 25fps. Find the topmost dirty node explicitly instead, so the answer
			// holds under either ordering.
			int dynamicRoot = -1;
			int secondRoot = -1;
			for (int i = 0; i < state.nodeCount; i++)
			{
				const Node &node = state.nodes[i];
				if (!node.render.dirty && !state.nodeCommandDirty[i])
					continue;
				if (node.render.bg_recolor_pending)
				{
					if (debugSettle)
						std::printf("[settle] FAIL node=%d bg_recolor_pending\n", i);
					return BackdropSettle::kFailed;
				}
				// Recently-changed dynamic leaves (ticking text badges) are tolerated: the
				// bake excludes them and their updates flow through the normal dirty-region
				// replay, so their dirtiness doesn't make the static scene unstable.
				if (node.first_child < 0 && !node.render.transform_dirty &&
						state.nodeBackdropCooldown[i] > 0)
					continue;
				// Inside another dirty subtree: carried by that subtree's root, not a root.
				if (staticBackdropHasDirtyAncestor(i))
					continue;
				if (dynamicRoot < 0)
					dynamicRoot = i;
				else if (secondRoot < 0)
					secondRoot = i;
			}
			if (secondRoot >= 0)
			{
				if (debugSettle)
					std::printf("[settle] FAIL second dynamic root node=%d (first=%d)\n", secondRoot, dynamicRoot);
				return BackdropSettle::kFailed;
			}
			if (dynamicRoot < 0)
			{
				if (debugSettle)
					std::printf("[settle] QUIET no dynamic root this frame\n");
				return BackdropSettle::kQuiet;
			}
			// The one root must be the bake's shape: a transformed subtree, not a leaf and
			// not a content change on a static node.
			const Node &rootNode = state.nodes[dynamicRoot];
			if (!state.nodeCommandDirty[dynamicRoot] || !rootNode.render.transform_dirty ||
					rootNode.first_child < 0)
			{
				if (debugSettle)
					std::printf("[settle] FAIL node=%d cmdDirty=%d transformDirty=%d leaf=%d cooldown=%d\n",
											dynamicRoot, state.nodeCommandDirty[dynamicRoot] ? 1 : 0,
											rootNode.render.transform_dirty ? 1 : 0,
											rootNode.first_child < 0 ? 1 : 0, state.nodeBackdropCooldown[dynamicRoot]);
				return BackdropSettle::kFailed;
			}
			return BackdropSettle::kDynamic;
		}

		DirtyRegions::Rect transformedBoundsRect(const Node &node, bool usePrevious, int origin)
		{
			int x0, y0, x1, y1;
			ViewRenderer::transformedBounds(node, usePrevious, &x0, &y0, &x1, &y1);
			return DirtyRegions::Rect{x0, y0, x1, y1, origin};
		}

		void unionTransformedSubtreeBounds(int node, bool usePrevious, int *x0, int *y0, int *x1, int *y1, bool *any)
		{
			auto &state = treeState();
			if (node < 0 || node >= state.nodeCount)
				return;
			Node &n = state.nodes[node];
			if (n.style.display != 1 && n.layout.width > 0 && n.layout.height > 0)
			{
				int nx0, ny0, nx1, ny1;
				ViewRenderer::transformedBounds(n, usePrevious, &nx0, &ny0, &nx1, &ny1);
				if (!*any)
				{
					*x0 = nx0;
					*y0 = ny0;
					*x1 = nx1;
					*y1 = ny1;
					*any = true;
				}
				else
				{
					if (nx0 < *x0)
						*x0 = nx0;
					if (ny0 < *y0)
						*y0 = ny0;
					if (nx1 > *x1)
						*x1 = nx1;
					if (ny1 > *y1)
						*y1 = ny1;
				}
			}
			for (int child = n.first_child; child >= 0; child = state.nodes[child].next_sibling)
				unionTransformedSubtreeBounds(child, usePrevious, x0, y0, x1, y1, any);
		}

		DirtyRegions::Rect transformedSubtreeBoundsRect(int node, bool usePrevious, int origin)
		{
			int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
			bool any = false;
			unionTransformedSubtreeBounds(node, usePrevious, &x0, &y0, &x1, &y1, &any);
			return any ? DirtyRegions::Rect{x0, y0, x1, y1, origin}
								 : transformedBoundsRect(treeState().nodes[node], usePrevious, origin);
		}

		bool dirtyRectsIntersect(const DirtyRegions::Rect &a, const DirtyRegions::Rect &b)
		{
			return a.x0 <= b.x1 && a.x1 >= b.x0 &&
						 a.y0 <= b.y1 && a.y1 >= b.y0;
		}

		int filterBlurRadiusForNode(const Node &node, bool usePrevious)
		{
			return usePrevious ? node.render.previous_filter_blur_radius : rstyle(node.style).filter_blur_radius;
		}

		DirtyRegions::Rect filterBlurBoundsRect(const Node &node, bool usePrevious, int origin)
		{
			return transformedBoundsRect(node, usePrevious, origin);
		}

		void expandDirtyRegionsForFilterBlur(DirtyRegions::Rect *rects,
																				 int *rect_count,
																				 DirtyRegions::Rect *flush_rects,
																				 int *flush_rect_count,
																				 bool unifiedRectPath,
																				 int width,
																				 int height)
		{
			if (!rects || !rect_count || *rect_count <= 0 || width <= 0 || height <= 0)
				return;
			auto &state = treeState();
			const int initialRectCount = *rect_count;
			for (int i = 0; i < state.nodeCount; i++)
			{
				const Node &node = state.nodes[i];
				if (node.style.display == 1)
					continue;
				for (int pass = 0; pass < 2; pass++)
				{
					const bool usePrevious = pass == 1;
					if (filterBlurRadiusForNode(node, usePrevious) <= 0)
						continue;
					DirtyRegions::Rect blurRect = filterBlurBoundsRect(node, usePrevious, i);
					if (!clipDirtyRect(&blurRect, width, height))
						continue;
					bool intersects = false;
					for (int r = 0; r < initialRectCount; r++)
					{
						if (dirtyRectsIntersect(rects[r], blurRect))
						{
							intersects = true;
							break;
						}
					}
					if (!intersects)
						continue;
					addDirtyRegion(rects, rect_count, blurRect, width, height);
					if (!unifiedRectPath)
						addDirtyRegion(flush_rects, flush_rect_count, blurRect, width, height);
				}
			}
		}

		void appendTransformedLeafShape(DirtyRegions::Rect *rects,
																		int *rect_count,
																		const Node &node,
																		bool usePrevious,
																		int origin,
																		int width,
																		int height,
																		int segmentLimit,
																		bool mergeSegments)
		{
			const int boxWidth = nodeBoxWidth(node, usePrevious);
			const int boxHeight = nodeBoxHeight(node, usePrevious);
			if (boxWidth <= 0 || boxHeight <= 0)
				return;

			const int rotate = nodeBoxRotation(node, usePrevious);
			if ((rotate % 3600) == 0 || segmentLimit <= 1)
			{
				if (mergeSegments)
					addDirtyRegion(rects, rect_count, transformedBoundsRect(node, usePrevious, origin), width, height);
				else
					appendDirtyRegion(rects, rect_count, transformedBoundsRect(node, usePrevious, origin), width, height);
				return;
			}

			int longSide = boxWidth >= boxHeight ? boxWidth : boxHeight;
			int shortSide = boxWidth < boxHeight ? boxWidth : boxHeight;
			if (shortSide < 1)
				shortSide = 1;
			int segments = (longSide + shortSide - 1) / shortSide;
			if (segments > segmentLimit)
				segments = segmentLimit;
			if (segments < 1)
				segments = 1;
			if (segments <= 1)
			{
				if (mergeSegments)
					addDirtyRegion(rects, rect_count, transformedBoundsRect(node, usePrevious, origin), width, height);
				else
					appendDirtyRegion(rects, rect_count, transformedBoundsRect(node, usePrevious, origin), width, height);
				return;
			}

			int16_t xs[4], ys[4];
			ViewRenderer::transformedCorners(node, usePrevious, xs, ys);
			struct Point
			{
				double x;
				double y;
			};
			Point p[4] = {
					{static_cast<double>(xs[0]), static_cast<double>(ys[0])},
					{static_cast<double>(xs[1]), static_cast<double>(ys[1])},
					{static_cast<double>(xs[2]), static_cast<double>(ys[2])},
					{static_cast<double>(xs[3]), static_cast<double>(ys[3])},
			};

			Point start;
			Point end;
			Point offset;
			if (boxWidth >= boxHeight)
			{
				start = {(p[0].x + p[3].x) * 0.5, (p[0].y + p[3].y) * 0.5};
				end = {(p[1].x + p[2].x) * 0.5, (p[1].y + p[2].y) * 0.5};
				offset = {(p[0].x - p[3].x) * 0.5, (p[0].y - p[3].y) * 0.5};
			}
			else
			{
				start = {(p[0].x + p[1].x) * 0.5, (p[0].y + p[1].y) * 0.5};
				end = {(p[3].x + p[2].x) * 0.5, (p[3].y + p[2].y) * 0.5};
				offset = {(p[1].x - p[0].x) * 0.5, (p[1].y - p[0].y) * 0.5};
			}

			const double dx = end.x - start.x;
			const double dy = end.y - start.y;
			for (int i = 0; i < segments; i++)
			{
				const double t0 = static_cast<double>(i) / static_cast<double>(segments);
				const double t1 = static_cast<double>(i + 1) / static_cast<double>(segments);
				Point a{start.x + dx * t0, start.y + dy * t0};
				Point b{start.x + dx * t1, start.y + dy * t1};
				Point q[4] = {
						{a.x + offset.x, a.y + offset.y},
						{b.x + offset.x, b.y + offset.y},
						{b.x - offset.x, b.y - offset.y},
						{a.x - offset.x, a.y - offset.y},
				};

				double minX = q[0].x, maxX = q[0].x;
				double minY = q[0].y, maxY = q[0].y;
				for (int qi = 1; qi < 4; qi++)
				{
					if (q[qi].x < minX)
						minX = q[qi].x;
					if (q[qi].x > maxX)
						maxX = q[qi].x;
					if (q[qi].y < minY)
						minY = q[qi].y;
					if (q[qi].y > maxY)
						maxY = q[qi].y;
				}

				DirtyRegions::Rect strip{
						static_cast<int>(std::floor(minX)) - 1,
						static_cast<int>(std::floor(minY)) - 1,
						static_cast<int>(std::ceil(maxX)) + 1,
						static_cast<int>(std::ceil(maxY)) + 1,
						origin};
				if (mergeSegments)
					addDirtyRegion(rects, rect_count, strip, width, height);
				else
					appendDirtyRegion(rects, rect_count, strip, width, height);
			}
		}

		bool appendTransformedLeafDirtyRegions(DirtyRegions::Rect *rects,
																					 int *rect_count,
																					 DirtyRegions::Rect *flush_rects,
																					 int *flush_rect_count,
																					 const Node &node,
																					 int origin,
																					 int width,
																					 int height,
																					 int segmentLimit,
																					 bool segmentReplayRegions)
		{
			if (!canUseStripDirtyForTransformedLeaf(node))
				return false;
			if (node.layout.previous_width <= 0 && node.layout.previous_height <= 0 &&
					node.layout.width <= 0 && node.layout.height <= 0)
				return true;

			const int previousRotate = nodeBoxRotation(node, true);
			const int currentRotate = nodeBoxRotation(node, false);
			if ((previousRotate % 3600) == 0 && (currentRotate % 3600) == 0)
			{
				bool have = false;
				DirtyRegions::Rect combined{0, 0, -1, -1, origin};
				if (node.layout.previous_width > 0 && node.layout.previous_height > 0)
				{
					combined = transformedBoundsRect(node, true, origin);
					have = true;
				}
				if (node.layout.width > 0 && node.layout.height > 0)
				{
					const DirtyRegions::Rect current = transformedBoundsRect(node, false, origin);
					combined = have ? uniteDirtyRects(combined, current) : current;
					have = true;
				}
				if (have)
				{
					appendDirtyRegion(rects, rect_count, combined, width, height);
					appendDirtyRegion(flush_rects, flush_rect_count, combined, width, height);
				}
				return true;
			}

			appendTransformedLeafShape(rects, rect_count, node, true, origin, width, height, segmentReplayRegions ? segmentLimit : 1, !segmentReplayRegions);
			appendTransformedLeafShape(rects, rect_count, node, false, origin, width, height, segmentReplayRegions ? segmentLimit : 1, !segmentReplayRegions);
			appendTransformedLeafShape(flush_rects, flush_rect_count, node, true, origin, width, height, segmentLimit, false);
			appendTransformedLeafShape(flush_rects, flush_rect_count, node, false, origin, width, height, segmentLimit, false);
			return true;
		}

		// ---------------------------------------------------------------------------
		// Horizontal-pan fast path.
		//
		// A side-scroller camera (sky-hop's `<div style="left: -cameraX">` inside an
		// overflow:hidden viewport) translates a wide world subtree by dx every frame.
		// The normal refresh marks the whole panned subtree's bbox dirty (~the entire
		// viewport) and re-replays every visible tile — ~73% of the screen rastered per
		// frame. This path recreates the reverted-and-never-ported `memcpy-shift
		// overflow viewport` optimization (commit 24d88b79) on the C++ runtime: shift
		// the viewport's pixels by dx in place, then repaint only:
		//   - the newly-revealed edge strip,
		//   - non-uniform static backdrop the shift smeared (e.g. the hill image; a
		//     solid water fill is horizontally lossless under a shift, so it's skipped),
		//   - independently-moving sprites (player/enemies/coins), including the ghost
		//     the shift left at their previous position + dx.
		// It only engages on translate-only frames; structural/record frames fall back
		// to the normal path (which is also what brings newly-recorded tiles on-screen).

		bool panClipToViewport(int width, int height, int *x, int *y, int *w, int *h)
		{
			if (*w <= 0 || *h <= 0 || width <= 0 || height <= 0)
				return false;
			int x0 = *x;
			int y0 = *y;
			int x1 = *x + *w - 1;
			int y1 = *y + *h - 1;
			if (x0 < 0)
				x0 = 0;
			if (y0 < 0)
				y0 = 0;
			if (x1 >= width)
				x1 = width - 1;
			if (y1 >= height)
				y1 = height - 1;
			if (x0 > x1 || y0 > y1)
				return false;
			*x = x0;
			*y = y0;
			*w = x1 - x0 + 1;
			*h = y1 - y0 + 1;
			return true;
		}

		bool panNodeHasTransform(const Node &n)
		{
			return hasIndividualLinearTransform(rstyle(n.style)) || hadIndividualLinearTransform(n.render) ||
	       rstyle(n.style).transform_rotate != 0 || n.render.previous_transform_rotate != 0 ||
						 rstyle(n.style).transform_rotate_x != 0 || n.render.previous_transform_rotate_x != 0 ||
						 rstyle(n.style).transform_rotate_y != 0 || n.render.previous_transform_rotate_y != 0 ||
						 composedTranslateX(rstyle(n.style)) != 0 || n.render.previous_transform_translate_x != 0 ||
						 composedTranslateY(rstyle(n.style)) != 0 || n.render.previous_transform_translate_y != 0 ||
						 composedTranslateZ(rstyle(n.style)) != 0 || n.render.previous_transform_translate_z != 0 ||
						 composedTranslateXPercent(rstyle(n.style)) != 0 || n.render.previous_transform_translate_x_percent != 0 ||
						 composedTranslateYPercent(rstyle(n.style)) != 0 || n.render.previous_transform_translate_y_percent != 0 ||
						 rstyle(n.style).transform_scale_x != 1000 || n.render.previous_transform_scale_x != 1000 ||
						 rstyle(n.style).transform_scale_y != 1000 || rstyle(n.style).transform_scale_z != 1000 || n.render.previous_transform_scale_y != 1000 || n.render.previous_transform_scale_z != 1000 ||
						 rstyle(n.style).perspective > 0 || n.render.previous_perspective > 0;
		}

		bool panIsDescendantOf(int node, int ancestor)
		{
			auto &state = treeState();
			for (int p = node; p >= 0 && p < state.nodeCount; p = state.nodes[p].parent)
			{
				if (p == ancestor)
					return true;
			}
			return false;
		}

		// True when shifting this node's painted pixels horizontally is lossless (so the
		// memcpy already placed them correctly and no repaint is needed): a plain View
		// that paints nothing or a single solid background color, with no gradient/grid,
		// border, radius, shadow, blur, mask, or partial opacity. Images/text/canvas and
		// anything non-uniform return false → they get repainted after the shift.
		bool panNodeIsHorizontallyScrollSafe(const Node &n, int viewportX, int viewportWidth)
		{
			if (n.type != NodeType::View)
				return false;
			if (n.style.opacity != 255)
				return false;
			if (hasAnyBorder(n.style))
				return false;
			for (int r = 0; r < 4; r++)
			{
				if (n.style.border_radius[r] != 0)
					return false;
				if (n.style.border_radius_percent[r] != kUnset && n.style.border_radius_percent[r] != 0)
					return false;
			}
			if (rstyle(n.style).filter_blur_radius > 0)
				return false;
			if (n.style.mask_right_fade_width > 0)
				return false;
			if (rstyle(n.style).box_shadow_alpha > 0 &&
					(rstyle(n.style).box_shadow_offset_x != 0 || rstyle(n.style).box_shadow_offset_y != 0 ||
					 rstyle(n.style).box_shadow_blur_radius != 0 || rstyle(n.style).box_shadow_spread != 0))
				return false;
			if (n.style.has_bg)
			{
				// A narrow solid rectangle still has horizontal edges: scrolling
				// it creates a ghost just like scrolling text or a rounded control.
				if (n.style.bg_alpha != 255 || panNodeHasTransform(n) ||
				    n.layout.x > viewportX || n.layout.x + n.layout.width < viewportX + viewportWidth)
					return false;
				for (int parent = n.parent; parent >= 0; parent = treeState().nodes[parent].parent) {
					const Node &clip = treeState().nodes[parent];
					if (panNodeHasTransform(clip) || (clip.style.overflow &&
					    (clip.layout.x > viewportX || clip.layout.x + clip.layout.width < viewportX + viewportWidth)))
						return false;
				}
				if (styleHasBackgroundImage(n.style))
					return false; // gradient fill (linear/radial/overlay)
				if (rstyle(n.style).bg_grid_axes != 0)
					return false; // grid overlay
			}
			return true;
		}

		// Returns whether the region survived viewport clipping and was actually
		// replayed, so the caller can report the frame as a direct retained replay.
		bool panReplayRegion(int width, int height, int rx, int ry, int rw, int rh, int origin = -1)
		{
			if (!panClipToViewport(width, height, &rx, &ry, &rw, &rh))
				return false;
			gea::platform::display::Display::resetClip();
			gea::platform::display::Display::setAlpha(255);
			DisplayList::instance().replayDirectDirtyRegion(rx, ry, rx + rw - 1, ry + rh - 1, origin);
			refreshPerfStatsMutable().treePanRepaintArea += rw * rh;
			return true;
		}

		bool tryHorizontalPanRefresh(int root, int width, int height, bool framebufferWasDirty,
																 bool displayListWasDirty, bool displayListWasStructural,
																 bool displayListFullRecord)
		{
			static int panCoverageRoot = -1;
			static int panCoverageWrapper = -1;
			static int panCoverageClipParent = -1;
			static int panCoverageWrapperX = 0;
			static int panCoveragePadding = 0;
			// Serial of the display list the padded record produced. The coverage window
			// describes THAT list; any clear() elsewhere bumps the serial and invalidates it.
			static std::uint32_t panCoverageSerial = 0;
			auto resetPanCoverage = []()
			{
				panCoverageRoot = -1;
				panCoverageWrapper = -1;
				panCoverageClipParent = -1;
				panCoverageWrapperX = 0;
				panCoveragePadding = 0;
				panCoverageSerial = 0;
			};
			if (width <= 0 || height <= 0)
			{
				resetPanCoverage();
				return false;
			}
			if (gHorizontalPanFastPathDisabled || LayoutEngine::containsViewportFixed(root))
			{
				resetPanCoverage();
				return false;
			}
			// Need a translate-only frame: the display list must reflect the new positions
			// via in-place command translation, not a structural rebuild (which can add/
			// remove content the memcpy can't account for) or an external framebuffer dirty.
			if (framebufferWasDirty || displayListWasDirty || displayListWasStructural || displayListFullRecord)
			{
				resetPanCoverage();
				return false;
			}
			if (transformedLeafDirtyShapeCount() != 0)
			{
				resetPanCoverage();
				return false;
			}
			if (DisplayList::instance().staticBackdropActive())
			{
				resetPanCoverage();
				return false;
			}

			auto &state = treeState();

			// 1. Find the single pan wrapper among dirty nodes.
			int wrapper = -1;
			int panDx = 0;
			for (int i = 0; i < state.nodeCount; i++)
			{
				Node *n = &state.nodes[i];
				if (!n->render.dirty)
					continue;
				if (n->style.display == kDisplayNone)
					continue;
				if (n->first_child < 0)
					continue;
				if (n->parent < 0)
					continue;
				Node *p = &state.nodes[n->parent];
				if (p->style.overflow != 1)
					continue; // parent must clip (overflow:hidden)
				const int dx = n->layout.x - n->layout.previous_x;
				const int dy = n->layout.y - n->layout.previous_y;
				if (dx == 0 || dy != 0)
					continue;
				if (n->layout.width != n->layout.previous_width)
					continue;
				if (n->layout.height != n->layout.previous_height)
					continue;
				if (n->layout.previous_width <= 0 || n->layout.previous_height <= 0)
					continue;
				if (n->style.opacity != 255)
					continue;
				if (panNodeHasTransform(*n))
					continue;
				// The clipping parent must itself be stationary so the viewport rect is fixed.
				if (p->layout.x != p->layout.previous_x || p->layout.y != p->layout.previous_y)
					continue;
				if (p->layout.width != p->layout.previous_width || p->layout.height != p->layout.previous_height)
					continue;
				if (wrapper >= 0)
					return false; // ambiguous — more than one pan candidate
				wrapper = i;
				panDx = dx;
			}
			if (wrapper < 0)
				return false;

			const int absDx = panDx < 0 ? -panDx : panDx;

			// 2. The clip viewport is the wrapper's overflow:hidden parent.
			Node *W = &state.nodes[wrapper];
			Node *P = &state.nodes[W->parent];
			int vx = P->layout.x, vy = P->layout.y, vw = P->layout.width, vh = P->layout.height;
			if (!panClipToViewport(width, height, &vx, &vy, &vw, &vh))
				return false;
			if (absDx >= vw)
				return false; // teleport — a full repaint is simpler and safer

			// Shifting the viewport is useful for a camera/world that spans it,
			// not a small floating subtree (drag previews, tooltips). For those,
			// the shift corrupts every stationary sibling and step 5 repaints
			// nearly the entire screen instead of just the old/new card bounds.
			if (W->layout.width < vw || W->layout.height < vh)
				return false;

			// 3. Decide what the memcpy can't reproduce and must be repainted explicitly:
			//    every wrapper descendant that did NOT translate uniformly with the camera.
			//    A descendant needs repainting when it is dirty (its appearance/size/visibility
			//    changed this frame — pulsing coins, the hurt blink toggling display) OR its
			//    screen delta != (dx,0) (it does not move 1:1 with the world: the camera-locked
			//    player, independently-walking enemies). Uniformly-shifted descendants (the
			//    static level tiles) are correctly carried by the memcpy and skipped. Hidden
			//    (display:none) descendants are NOT skipped here: a camera-locked sprite that
			//    just blinked off still has a stale ghost the memcpy dragged along, and its
			//    repaint (which now draws no commands) is what erases it. A DIRTY node OUTSIDE
			//    the wrapper (a pressed control, a HUD digit) means non-world content changed
			//    under/over the shift, which a world memcpy can't honour → fall back entirely.
			constexpr int kMaxPanExtras = 48;
			int extras[kMaxPanExtras];
			int extraCount = 0;
			for (int i = 0; i < state.nodeCount; i++)
			{
				if (i == wrapper)
					continue;
				Node *n = &state.nodes[i];
				if (!panIsDescendantOf(i, wrapper))
				{
					if (n->render.dirty)
						return false; // non-world content changed — handled only by full replay
					continue;				// static non-world content → step 5
				}
				const bool uniformShift = (n->layout.x - n->layout.previous_x == panDx) &&
																	(n->layout.y - n->layout.previous_y == 0);
				if (!n->render.dirty && uniformShift)
					continue; // carried correctly by the memcpy
				if (extraCount >= kMaxPanExtras)
					return false;
				extras[extraCount++] = i;
			}

			// Every reason to decline this frame has now been checked, so it is finally
			// safe to touch the retained display list. Nothing above this line may mutate
			// it: `refresh()` has already done its own display-list maintenance for this
			// frame (in-place command translation / rerecordNodeCommands) and recorded the
			// resulting per-node command bounds in state.nodeCommandDirty*, and it uses
			// those bounds to build the dirty rects it replays if this function declines.
			// Rebuilding the list and THEN declining swaps the commands out from under
			// those bounds, so the fallback replays rects that no longer describe what
			// changed and leaves dx-wide slivers of the previous frame unrepainted.
			//
			// The retained display list is clipped at record time, so commands for
			// off-viewport tiles do not exist until they scroll into view. Record a
			// padded child clip for the pan viewport, then reuse that covered command
			// window across frames until camera motion exhausts the padding.
			constexpr int kPanRecordPadding = 512;
			const bool coverageValid =
					panCoverageRoot == root &&
					panCoverageWrapper == wrapper &&
					panCoverageClipParent == W->parent &&
					panCoveragePadding == kPanRecordPadding &&
					// The coverage window describes the list this function recorded. Any clear()
					// elsewhere (refresh()'s full record, root_scroll_refresh) bumps the record
					// serial and leaves a normally-clipped list behind, so the padded-window
					// claim above would be false. Completes the identity check; it does not
					// change this app's frames, which never hit that interleaving.
					panCoverageSerial == DisplayList::instance().recordSerial() &&
					((W->layout.x - panCoverageWrapperX) < 0
							 ? -(W->layout.x - panCoverageWrapperX)
							 : (W->layout.x - panCoverageWrapperX)) + absDx < kPanRecordPadding;
			if (!coverageValid)
			{
				const std::int64_t rebuildStartUs = refreshPerfNowUs();
				DisplayList::instance().clear();
				DisplayList::instance().recordNodeWithExpandedClip(
						root,
						255,
						W->parent,
						vx - kPanRecordPadding,
						vy,
						vx + vw - 1 + kPanRecordPadding,
						vy + vh - 1);
				DisplayList::instance().weldTransformedFaces();
				state.displayListDirty = false;
				panCoverageRoot = root;
				panCoverageWrapper = wrapper;
				panCoverageClipParent = W->parent;
				panCoverageWrapperX = W->layout.x;
				panCoveragePadding = kPanRecordPadding;
				panCoverageSerial = DisplayList::instance().recordSerial();
				refreshPerfStatsMutable().treeRecordNodeUs += refreshPerfNowUs() - rebuildStartUs;
			}

			// ---- Commit: shift the framebuffer and repaint only what the shift can't. ----
			gea::platform::display::Display::resetClip();
			gea::platform::display::Display::setAlpha(255);
			gea::platform::display::Display::scrollRect(vx, vy, vw, vh, panDx, 0);

			// Regions this frame served by replaying the retained display list straight
			// into a scoped rect — the same replayDirectDirtyRegion the non-pan path
			// counts as a direct replay. Reported below so a pan frame is not
			// indistinguishable from a conservative full repaint in the stats.
			int directRegions = 0;

			// 4. Newly-revealed edge strip.
			if (panDx < 0)
				directRegions += panReplayRegion(width, height, vx + vw - absDx, vy, absDx, vh, wrapper) ? 1 : 0;
			else
				directRegions += panReplayRegion(width, height, vx, vy, absDx, vh, wrapper) ? 1 : 0;

			// 5. Non-uniform static content the shift corrupted: the hill image, but also any
			//    fixed UI that overlaps the scrolled viewport (the controls, RESET button —
			//    siblings drawn over the world). The memcpy moved their pixels by dx, so the
			//    element's true position now shows shifted neighbours AND a ghost sits at
			//    bounds+dx. Repaint the union of both so the ghost is erased and the element is
			//    redrawn at its fixed position. Solid fills (the water) are horizontally
			//    lossless under a shift and stay correct from the memcpy alone.
			for (int i = 0; i < state.nodeCount; i++)
			{
				Node *n = &state.nodes[i];
				if (n->style.display == kDisplayNone)
					continue;
				if (i == wrapper || panIsDescendantOf(i, wrapper))
					continue;
				if (n->render.dirty)
					continue;
				if (panNodeIsHorizontallyScrollSafe(*n, vx, vw))
					continue;
				const DirtyRegions::Rect b = transformedBoundsRect(*n, false, i);
				if (b.x0 > b.x1 || b.y0 > b.y1)
					continue;
				int ux0 = b.x0 < b.x0 + panDx ? b.x0 : b.x0 + panDx;
				int ux1 = b.x1 > b.x1 + panDx ? b.x1 : b.x1 + panDx;
				// Clip to the scrolled viewport — only that region was corrupted by the shift.
				if (ux0 < vx)
					ux0 = vx;
				if (ux1 > vx + vw - 1)
					ux1 = vx + vw - 1;
				int uy0 = b.y0 < vy ? vy : b.y0;
				int uy1 = b.y1 > vy + vh - 1 ? vy + vh - 1 : b.y1;
				if (ux0 > ux1 || uy0 > uy1)
					continue;
				directRegions += panReplayRegion(width, height, ux0, uy0, ux1 - ux0 + 1, uy1 - uy0 + 1, i) ? 1 : 0;
			}

			// 6. Independently-moving sprites: redraw the union of the new bounds and the
			//    ghost (previous bounds shifted by the pan) so the smeared old copy is erased.
			for (int e = 0; e < extraCount; e++)
			{
				const int id = extras[e];
				Node *n = &state.nodes[id];
				const DirtyRegions::Rect cur = transformedSubtreeBoundsRect(id, false, id);
				int x0 = cur.x0, y0 = cur.y0, x1 = cur.x1, y1 = cur.y1;
				if (n->layout.previous_width > 0 && n->layout.previous_height > 0)
				{
					const DirtyRegions::Rect prev = transformedSubtreeBoundsRect(id, true, id);
					const int gx0 = prev.x0 + panDx, gx1 = prev.x1 + panDx;
					if (gx0 < x0)
						x0 = gx0;
					if (prev.y0 < y0)
						y0 = prev.y0;
					if (gx1 > x1)
						x1 = gx1;
					if (prev.y1 > y1)
						y1 = prev.y1;
				}
				if (x0 > x1 || y0 > y1)
					continue;
				directRegions += panReplayRegion(width, height, x0, y0, x1 - x0 + 1, y1 - y0 + 1, id) ? 1 : 0;
			}

			gea::platform::display::Display::resetClip();
			gea::platform::display::Display::setAlpha(255);
			gea::platform::display::Display::flush();

			// Keep the static-backdrop cache reset while panning, then mirror refresh()'s tail.
			DisplayList::instance().maybeBakeStaticBackdrop(width, height, false, false);
			LayoutSnapshot::capture();
			state.canvases.forEach([](CanvasSurfaceState &surface)
														 {
		if (auto *canvas = surface.canvas()) canvas->resetDirty(); });
			refreshPerfStatsMutable().treePanReplayCalls++;
			if (directRegions > 0)
				refreshPerfStatsMutable().treeDirectReplayCalls++;
			return true;
		}

	} // namespace

	// Render the CURRENT retained display list full-screen into `dst` (rgb565) for a
	// screenshot. The fused flush (FUSE_REPLAY_FLUSH) rasterizes into the DMA staging
	// buffer and never writes the PSRAM framebuffer, so Display::copySnapshotRgb565 returns
	// the first-ever rendered frame for a fused retained app (a frozen "frame 0"). Replaying
	// the live display list into the snapshot buffer here yields the real on-screen frame.
	// The caller must hold AppState::lock() so the frame task isn't using the shared canvas
	// or mutating the list. Returns false for non-retained (canvas/present) apps, whose
	// pixels aren't in the retained command list — the caller falls back to the present path.
	bool renderRetainedSnapshotRgb565(std::uint16_t *dst, int width, int height)
	{
		if (!dst || width <= 0 || height <= 0)
			return false;
		if (DisplayList::instance().commandCount() < 2)
			return false; // canvas/present app: nothing retained to replay
		auto *canvas = gea::platform::display::Display::canvas();
		if (!canvas)
			return false;
		using PixT = gea::framework::graphics::pixel::native_t;
		PixT *base = reinterpret_cast<PixT *>(dst);
		canvas->bindPixels(base, width, height, width);
		gea::platform::display::Display::resetClip();
		gea::platform::display::Display::setAlpha(255);
		gea::platform::display::Display::pushClip(0, 0, width, height);
		// The direct-region replay applies each node's ancestor overflow clips
		// (pushNodeReplayClips); the simple clipped walk used before SKIPS
		// PushClip/PopClip and is only valid when canReplaySimpleDirtyRegions
		// holds — which the scroll fast path's padded coverage record breaks
		// (rows recorded beyond the scroll viewport printed above it in every
		// snapshot, reading as a phantom on-screen ghost during debugging).
		{
			const DisplayReplayRegion region{0, 0, width - 1, height - 1, -1};
			DisplayList::instance().replayDirectDirtyRegions(&region, 1);
		}
		gea::platform::display::Display::popClip();
		// Restore the shared canvas onto the real framebuffer before the frame task resumes.
		gea::platform::display::Display::rebindCanvasToFramebuffer();
		const int pixels = width * height;
		for (int i = 0; i < pixels; i++)
			dst[i] = gea::framework::graphics::pixel::toRgb565(base[i]);
		return true;
	}

	namespace
	{
		// The (root, box) the last layout in this tree was performed for.
		//
		// `computeLayout` needs it because the dirty flags alone cannot answer
		// its question -- see the comment there. Recorded rather than derived
		// from the root's own box, because a root that sizes to its content
		// never equals the box it was offered, and treating that as "not laid
		// out" would take the local-refresh fast path away from every such tree.
		//
		// -1 is not a node, so a fresh process starts out having been laid out
		// for nothing, which is the safe direction: the first call does the full
		// pass. An app switch needs no reset for the same reason from the other
		// end -- `Tree::clear` sets `nodeCount` to 0, so every call in the gap
		// before the next `mount` leaves at the root-range check above, and
		// `mount` always lays out and re-records.
		int gLayoutViewportRoot = -1;
		int gLayoutViewportWidth = -1;
		int gLayoutViewportHeight = -1;

		void noteLaidOutViewport(int root, int width, int height)
		{
			gLayoutViewportRoot = root;
			gLayoutViewportWidth = width;
			gLayoutViewportHeight = height;
		}
	} // namespace

	void Tree::mount(int root, int width, int height)
	{
		auto &state = treeState();
		if (root < 0 || root >= state.nodeCount)
			return;
		state.refreshSerial++;

		state.mountedRoot = root;
		state.mountedWidth = width;
		state.mountedHeight = height;

		// While the initial style batch is open, per-node class styles haven't been
		// applied yet, so laying out now produces UNSTYLED, content-sized boxes. That
		// garbage layout is not just unpresentable — it POISONS percent resolution:
		// `percentBasisForNode` prefers a parent's `layout.height` when it's > 0, so
		// endStyleMountBatch would resolve e.g. `max-height: 100%` against the stale
		// content height instead of the declared style chain (observed: a square board
		// clamped to its content height, collapsing flex children). Skip layout/paint
		// entirely here — the guaranteed styled re-mount after endStyleMountBatch (see
		// gea_app_entry.cpp Application::init) does the real, correct layout and present.
		if (styleMountBatchActive())
			return;

		gea::platform::display::Display::clearNoFlush();
		gea::platform::display::Display::resetClip();
		gea::platform::display::Display::setAlpha(255);

		LayoutEngine::instance().beginLayoutPass();
		LayoutEngine::instance().layoutNode(root, width, height);
		applyPendingScrollIntoView();
		LayoutEngine::instance().resolveAbsoluteCoords(root, 0, 0);
		noteLaidOutViewport(root, width, height);
		state.refreshSerial++;
		if (state.refreshSerial == 0)
			state.refreshSerial = 1;
		LayoutSnapshot::markChangesDirty();

		DisplayList::instance().clear();
		DisplayList::instance().recordNode(root, 255);
		DisplayList::instance().weldTransformedFaces();
		DisplayList::instance().replay();
		state.displayListDirty = false;
		state.displayListRebuildStructural = false; // this path already repainted everything

		// Don't present the very first mount while the initial style batch is still
		// open: per-node class styles haven't been applied yet, so this frame is
		// unstyled (block layout at the origin, no colors). endStyleMountBatch()
		// applies styles and the follow-up styled mount/refresh presents instead —
		// avoiding a flash of unstyled content (text + images, no CSS) at boot.
		if (!styleMountBatchActive())
			gea::platform::display::Display::flush();
		state.canvases.forEach([](CanvasSurfaceState &surface)
													 {
		if (auto *canvas = surface.canvas()) canvas->resetDirty(); });
		LayoutSnapshot::capture();
	}

	void Tree::computeLayout(int root, int width, int height)
	{
		auto &state = treeState();
		if (root < 0 || root >= state.nodeCount)
			return;
		// Whether the tree was last laid out for THIS viewport. Both cheap paths
		// below are keyed on the dirty flags, which answer "has the tree
		// changed" -- and that is only half of this entry point's question. A
		// caller also supplies a root and a box, and the flags say nothing about
		// whether the geometry in the tree was computed for them.
		//
		// They come apart whenever something else consumes the dirty flags
		// first, for a DIFFERENT viewport. `Document::refreshMountedIfDirty`
		// does exactly that, at the MOUNTED size, and both callers of this
		// function are native shells that run after it with a PANE root and size
		// (`macos_native_shell.mm` sets the pane node's width/height and calls
		// this; the test harness's `refresh()` then `computeLayout` is the same
		// order). Every reactive tree mutation therefore ended with the pane
		// holding the mounted viewport's geometry and this call returning
		// without touching it -- measured on gea-companion, whose detail pane
		// came back 632x261 from a `computeLayout(node, 632, 548)`, shrinking a
		// page that fits into one that does not until its rows overlapped.
		//
		// Asked as "was the last layout for this request" rather than "does the
		// root's box equal the one asked for", because those are not the same
		// question: a root that legitimately sizes to its own content never
		// equals the available box, and reading it that way takes the local
		// refresh path away from every such tree permanently (measured:
		// `run-gea-style-viewport-metrics` and
		// `run-gea-temperature-dial-local-refresh` both broke on it).
		const bool laidOutForRequest = gLayoutViewportRoot == root && gLayoutViewportWidth == width && gLayoutViewportHeight == height;
		int layout_mode = AbsoluteLeafRefresh::mode();
		if (layout_mode < 0 && laidOutForRequest)
			return;
		if (layout_mode > 0 && laidOutForRequest)
		{
			AbsoluteLeafRefresh::refreshPositions();
		}
		else
		{
			PreservedScrollOffset scrollOffsets[kMaxNodes];
			preserveScrollOffsets(scrollOffsets, kMaxNodes);
			LayoutEngine::instance().beginLayoutPass();
		LayoutEngine::instance().layoutNode(root, width, height);
			restoreScrollOffsetsAfterLayout(scrollOffsets, kMaxNodes);
			applyPendingScrollIntoView();
			LayoutEngine::instance().resolveAbsoluteCoords(root, 0, 0);
			noteLaidOutViewport(root, width, height);
		}
		state.refreshSerial++;
		if (state.refreshSerial == 0)
			state.refreshSerial = 1;
	}

	namespace
	{
		// Scoped-relayout root for the current dirty set: the lowest ancestor
		// containing every dirty node whose own box was stable last frame — its
		// subtree can be re-laid in isolation (LayoutEngine::layoutNodeScoped)
		// instead of relayouting the whole tree. Returns -1 when no such node
		// exists below the root (full relayout).
		// A parent walk that cannot outlive the tree.
		//
		// `parent` chains are supposed to end at -1, and every walk here trusted
		// that. A tree whose parent links form a CYCLE turns each of those walks
		// into an infinite loop inside the frame -- not a crash, not a wrong
		// picture, a board that stops: the e-reader wedged for 74 seconds in
		// `Document::refreshMountedIfDirty` with the panel idle and nothing
		// queued, until the task watchdog fired. A walk bounded by the node count
		// cannot do that, and no honest chain is longer than the tree is deep, so
		// the bound costs a correct walk nothing.
		//
		// Tripping it is a DEFECT upstream -- something published a cycle -- so it
		// says so once rather than failing silently, and answers the way the
		// caller's own "no scope found" answer already means: relayout whole.
		bool walkParents(TreeState &state, int from, int &steps)
		{
			steps++;
			if (steps <= state.nodeCount + 1)
				return true;
			// Repeat rather than latch: the walk is re-entered every frame, and a
			// one-shot report is gone before a serial reader can be attached.
			static unsigned hits = 0;
			if ((hits++ & 0xffu) == 0)
				std::printf("[tree] parent chain from node=%d parent=%d exceeds nodeCount=%d -- cycle in the tree\n", from,
				            from >= 0 && from < state.nodeCount ? state.nodes[from].parent : -2, state.nodeCount);
			return false;
		}

		int scopedRelayoutRoot(TreeState &state, int root)
		{
			auto isAncestorOrSelf = [&](int node, int ancestor) {
				int steps = 0;
				for (int c = node; c >= 0 && c < state.nodeCount; c = state.nodes[c].parent) {
					if (c == ancestor)
						return true;
					if (!walkParents(state, node, steps))
						return false;
				}
				return false;
			};
			int lca = -1;
			for (int i = 0; i < state.nodeCount; i++)
			{
				// Only geometry-relevant dirt widens the relayout scope; paint-only
				// dirt (layout_dirty clear) repaints in place via the record path.
				if (!state.nodes[i].render.dirty || !state.nodes[i].render.layout_dirty)
					continue;
				if (lca < 0)
				{
					lca = i;
					continue;
				}
				int steps = 0;
				while (lca >= 0 && !isAncestorOrSelf(i, lca))
				{
					if (!walkParents(state, lca, steps))
						return -1;
					lca = state.nodes[lca].parent;
				}
				if (lca < 0)
					return -1;
			}
			if (lca < 0)
				return -1;
			// A dirty node's own box may change; the scope must be a stable strict
			// ancestor.
			return state.nodes[lca].render.dirty ? state.nodes[lca].parent : lca;
		}

		// First ancestor-or-self of `from` (strictly below root) whose box was
		// stable last frame — a scoped-relayout candidate. Stability is only a
		// heuristic (an auto-sized box still resizes with new content):
		// layoutNodeScoped's post-layout dims check rejects those and the
		// caller retries from the candidate's parent.
		int stableScopeAt(TreeState &state, int from, int root)
		{
			int steps = 0;
			for (int scope = from; scope >= 0 && scope != root; scope = state.nodes[scope].parent)
			{
				if (!walkParents(state, from, steps))
					return -1;
				Node &a = state.nodes[scope];
				const bool dimsStable = a.layout.width == a.layout.previous_width &&
																a.layout.height == a.layout.previous_height &&
																a.layout.width > 0 && a.layout.height > 0;
				if (dimsStable && a.style.display != kDisplayNone)
					return scope;
			}
			return -1;
		}
	} // namespace

	void Tree::refresh(int root, int width, int height)
	{
		auto &state = treeState();
		if (root < 0 || root >= state.nodeCount)
			return;
		const bool viewportChanged = gLayoutViewportRoot != root ||
		    gLayoutViewportWidth != width || gLayoutViewportHeight != height;
		if (viewportChanged) {
			state.displayListDirty = true;
			state.displayListRebuildStructural = true;
			state.nodes[root].render.dirty = true;
			state.nodes[root].render.layout_dirty = true;
		}
		state.mountedWidth = width;
		state.mountedHeight = height;
		// Bump before the scroll-only fast path returns: a scroll-only refresh still
		// moves content, so native renderers must re-sync.
		state.refreshSerial++;
		auto &perf = refreshPerfStatsMutable();
		perf.treeRefreshCalls++;
		DisplayList::instance().clearRetainedBackgroundRecolors();

		const bool textClipDependencies = DisplayList::instance().hasTextClippedBackgrounds();
		if (textClipDependencies) {
			bool changed = state.displayListDirty;
			for (int i = 0; !changed && i < state.nodeCount; ++i)
				changed = state.nodes[i].render.dirty || state.nodes[i].render.layout_dirty || state.nodes[i].render.transform_dirty;
			if (changed) {
				// Background ink depends on descendants, including those whose own
				// foreground is transparent. Rebuild and repaint these dependencies
				// together before considering retained translation/recolor shortcuts.
				state.displayListDirty = true;
				state.displayListRebuildStructural = true;
				state.nodes[root].render.dirty = true;
			}
		}
		if (!textClipDependencies && state.pendingScrollIntoViewNode < 0 && RootScrollOnlyRefresh::refresh(root, width, height))
			return;

		// This frame is NOT a pure scroll, so the software scroll register's
		// circular row rotation (engaged by Display::scrollRect during the fast
		// path above) must not survive into it: content drawn now would mix
		// rotated and unrotated rows (observed: a stale header band + the whole
		// screen duplicated ~70px lower after the weather keyboard raised, and
		// the previous screen's rows surfacing through later repaints). Reset
		// the region to identity and dirty its rows so this refresh repaints
		// them in their unrotated placement.
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) || !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
		if (!Document::directCanvasContextUsed()) {
			if (auto *rotCanvas = gea::platform::display::Display::canvas()) {
				const int regionH = rotCanvas->scrollRegionH();
				if (regionH > 0) {
					const int regionY = rotCanvas->scrollRegionY();
					gea::platform::display::Display::resetScrollRegion();
					rotCanvas->markDirty(0, regionY, width - 1, regionY + regionH - 1);
					state.displayListDirty = true;
				}
			}
		}
#endif

		const std::int64_t layoutModeStartUs = refreshPerfNowUs();
		int framebufferDirtyX0 = 0;
		int framebufferDirtyY0 = 0;
		int framebufferDirtyX1 = -1;
		int framebufferDirtyY1 = -1;
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) || !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
		auto *displayCanvas = Document::directCanvasContextUsed() ? nullptr : gea::platform::display::Display::canvas();
		const bool framebuffer_was_dirty = displayCanvas &&
																			 displayCanvas->dirty(&framebufferDirtyX0,
																														&framebufferDirtyY0,
																														&framebufferDirtyX1,
																														&framebufferDirtyY1);
#else
		const bool framebuffer_was_dirty = false;
#endif
		int layout_mode = AbsoluteLeafRefresh::mode();
		perf.treeLayoutModeUs += refreshPerfNowUs() - layoutModeStartUs;
		if (layout_mode < 0)
		{
			if (!framebuffer_was_dirty && !state.displayListDirty && state.pendingScrollIntoViewNode < 0)
				return;
			layout_mode = state.displayListDirty ? 0 : 1;
		}
		if (state.pendingScrollIntoViewNode >= 0)
			layout_mode = 0;

		const std::int64_t layoutStartUs = refreshPerfNowUs();
		if (layout_mode > 0)
		{
			AbsoluteLeafRefresh::refreshPositions();
		}
		else
		{
			PreservedScrollOffset scrollOffsets[kMaxNodes];
			preserveScrollOffsets(scrollOffsets, kMaxNodes);
			// Scoped relayout: when every dirty node sits under a box-stable
			// ancestor (e.g. a city-name text growing inside a fixed-size header),
			// re-lay only that subtree — the memoized available box makes the
			// scoped pass reproducible, and layoutNodeScoped falls back (returns
			// false) if the scope's own dims end up changing after all.
			bool scoped = false;
			// Paint-only frame: no node carries geometry-relevant dirt and nothing
			// structural is pending — skip the relayout entirely; the record/replay
			// flow below repaints the dirty nodes in place.
			bool anyLayoutDirty = state.displayListRebuildStructural ||
														state.pendingScrollIntoViewNode >= 0;
			if (!anyLayoutDirty)
			{
				for (int i = 0; i < state.nodeCount; i++)
				{
					if (state.nodes[i].render.dirty && state.nodes[i].render.layout_dirty)
					{
						anyLayoutDirty = true;
						break;
					}
				}
			}
			if (anyLayoutDirty)
			{
			if (!viewportChanged && state.pendingScrollIntoViewNode < 0)
			{
				int scope = scopedRelayoutRoot(state, root);
				if (scope >= 0)
					scope = stableScopeAt(state, scope, root);
				if (scope < 0)
					perf.treeScopedRejectReason = 1;
				for (int attempt = 0; scope >= 0 && attempt < 2; attempt++)
				{
					scoped = LayoutEngine::instance().layoutNodeScoped(scope, root);
					if (scoped)
					{
						perf.treeScopedLayouts++;
						break;
					}
					// Most rejects mean the scope autosized to the new content —
					// retry from the next stable ancestor up (each failed attempt's
					// partial layout is fully redone by the retry / full fallback).
					scope = stableScopeAt(state, state.nodes[scope].parent, root);
				}
			}
			if (!scoped)
			{
				LayoutEngine::instance().beginLayoutPass();
				LayoutEngine::instance().layoutNode(root, width, height);
			}
			restoreScrollOffsetsAfterLayout(scrollOffsets, kMaxNodes);
			applyPendingScrollIntoView();
			if (!scoped)
				LayoutEngine::instance().resolveAbsoluteCoords(root, 0, 0);
			noteLaidOutViewport(root, width, height);
			// Reflow ghost guard: a full relayout can MOVE nodes that carry no dirty
			// mark of their own — flex siblings shifted because a text node's box grew
			// (a city-rail chip going '--' → '20°' pushes its neighbors right). The
			// dirty pass below only visits render.dirty nodes, so an unmarked moved
			// node neither repaints its vacated box nor translates its retained
			// display commands: its old glyphs ghost at the previous position. Mark
			// every node whose box changed beyond what its ancestors' scroll deltas
			// explain (pure scroll frames stay unmarked, so scroll perf is untouched).
			for (int i = 0; i < state.nodeCount; i++)
			{
				Node &n = state.nodes[i];
				if (n.render.dirty)
					continue;
				if (n.parent < 0 && i != root)
					continue;
				if (n.style.display == kDisplayNone)
					continue;
				if (n.layout.previous_width <= 0 && n.layout.previous_height <= 0 &&
						n.layout.width <= 0 && n.layout.height <= 0)
					continue;
				if (n.layout.width == n.layout.previous_width && n.layout.height == n.layout.previous_height)
				{
					const int dx = n.layout.x - n.layout.previous_x;
					const int dy = n.layout.y - n.layout.previous_y;
					if (dx == 0 && dy == 0)
						continue;
					int scrollDx = 0;
					int scrollDy = 0;
					for (int a = n.parent; a >= 0 && a < state.nodeCount; a = state.nodes[a].parent)
					{
						scrollDx -= state.nodes[a].layout.scroll_x - state.nodes[a].layout.previous_scroll_x;
						scrollDy -= state.nodes[a].layout.scroll_y - state.nodes[a].layout.previous_scroll_y;
					}
					if (dx == scrollDx && dy == scrollDy)
						continue;
				}
				n.render.dirty = 1;
				n.render.layout_dirty = 1;
				n.render.non_scroll_dirty = 1;
			}
			}  // anyLayoutDirty
		}
		// Transform geometry caches can be touched before layout settles during a
		// refresh; once absolute coords are final, invalidate that pre-layout basis.
		state.refreshSerial++;
		if (state.refreshSerial == 0)
			state.refreshSerial = 1;
		perf.treeLayoutUs += refreshPerfNowUs() - layoutStartUs;

		const std::int64_t displayListStartUs = refreshPerfNowUs();
		const bool display_list_was_dirty = state.displayListDirty;
		// Consume the structural flag for this frame's rebuild decision. A
		// content-only rebuild (appearance change on existing nodes — partial
		// opacity, absolute-leaf resize) needs no full-viewport repaint; the
		// per-node dirty rects below cover everything that changed. Reset now; the
		// next frame's mutations re-raise it via markDisplayListDirty().
		const bool display_list_was_structural = state.displayListRebuildStructural;
		state.displayListRebuildStructural = false;
		// Record side: keep the display list and re-record ONLY dirty nodes whenever
		// there's no structural change. This is INDEPENDENT of
		// canReplayDirectDirtyRegions — that guard only restricts the incremental
		// dirty-region REPLAY (which can't reproduce a partial-opacity stack), NOT
		// whether we can avoid re-recording unchanged nodes. Decoupling the two keeps
		// partial-opacity/size animations and content-only relayouts off the full-
		// rebuild path.
		const bool keep_display_list = !state.displayListDirty;
		// Replay side additionally needs the compositing guard. When it bails (any
		// partial opacity), the list is still kept incrementally above and replayed
		// via the safe full-per-rect path below.
		int direct_replay = keep_display_list &&
												DisplayList::instance().canReplayDirectDirtyRegions(width, height);
#if GEA_EMBEDDED_SIMPLE_REPLAY_DEBUG
		{
			// Log only the COPY frames (the ones that fell off the fused path) — those are
			// the drops. Throttled so a steady copy stream doesn't itself perturb timing.
			static int g = 0;
			if (!direct_replay && (g++ % 8) == 0)
				std::printf("[gate-COPY] dlDirty=%d structural=%d keep=%d simpleOK=%d\n",
					(int)state.displayListDirty, (int)state.displayListRebuildStructural,
					(int)keep_display_list,
					(int)DisplayList::instance().canReplaySimpleDirtyRegions(width, height));
		}
#endif
		BackdropSettle backdropSettle = staticBackdropFrameSettle();
#ifndef ESP_PLATFORM
		// A/B kill switch for the quiet-keeps-cache behavior (host debugging only).
		{
			static const bool legacyDrop = std::getenv("GEA_BACKDROP_LEGACY_DROP") != nullptr;
			if (legacyDrop && backdropSettle == BackdropSettle::kQuiet)
				backdropSettle = BackdropSettle::kFailed;
		}
#endif
		// Eligible = a dynamic root is actively moving (settle/bake/reproject shape).
		const bool staticBackdropEligible =
				direct_replay != 0 && backdropSettle == BackdropSettle::kDynamic;
		// Stable = nothing happened that could stale the baked pixels. Quiet frames
		// (keyframe pauses, ticker-only frames) are stable: keep a live cache across
		// them instead of forcing the drop → re-bake → full-sync cycle on resume.
		const bool staticBackdropStable =
				direct_replay != 0 && backdropSettle != BackdropSettle::kFailed;
#ifndef ESP_PLATFORM
		{
			static const bool debugBackdrop = std::getenv("GEA_DEBUG_BACKDROP") != nullptr;
			if (debugBackdrop)
				std::printf("[gate] dlDirty=%d keep=%d directReplay=%d settle=%s backdropActive=%d\n",
										display_list_was_dirty ? 1 : 0, keep_display_list ? 1 : 0, direct_replay,
										backdropSettle == BackdropSettle::kDynamic	 ? "dynamic"
										: backdropSettle == BackdropSettle::kQuiet ? "quiet"
																															 : "FAILED",
										DisplayList::instance().staticBackdropActive() ? 1 : 0);
		}
#endif
		if (!staticBackdropStable && DisplayList::instance().staticBackdropActive())
		{
			// Dropping a live backdrop cache forces a re-bake + full-viewport sync later —
			// the expensive event behind animation-wrap fps dips. Log why it failed.
			std::printf("[bdrop] drop directReplay=%d — settle reasons:\n", direct_replay);
			if (direct_replay != 0)
				staticBackdropFrameSettle(/*debugPrint=*/true);
			DisplayList::instance().invalidateStaticBackdrop();
		}
		// Set when this frame took the transform-only reproject fast path — lets the
		// dirty pass reuse the reproject's (prev ∪ cur) bbox instead of re-projecting.
		bool reprojected = false;
		// Set when the display list was (re)built by a full clear()+recordNode() rather
		// than kept via in-place command translation — the horizontal-pan fast path only
		// engages on pure translate frames (see tryHorizontalPanRefresh).
		bool displayListFullRecord = false;
		if (keep_display_list)
		{
			int can_keep_display_list = 1;
			int rebuild_for_transform = 0;
			// Content-dirty nodes without render.dirty are skipped by the main loop
			// below (their commands are patched in place), but a node that was fully
			// outside the record-time clip has NO commands to patch — if it has since
			// slid into the viewport (a pulsing coin at the pan edge), the retained
			// list is incomplete and only a full record can reveal it.
#if GEA_EMBEDDED_SUBTREE_REVEAL_CHECK
			for (int i = 0; i < state.nodeCount && can_keep_display_list; i++)
			{
				if (!state.nodeCommandDirty[i] || state.nodes[i].render.dirty)
					continue;
				if (DisplayList::instance().subtreeRevealsUnrecordedContent(i, width, height))
					can_keep_display_list = 0;
			}
#endif
			// Set when any node's commands were re-recorded in place: a re-recorded
			// transformed face comes back with unwelded corners next to its still-welded
			// neighbors, so the weld must run again after the loop.
			bool rerecorded_any = false;
			for (int i = 0; can_keep_display_list && i < state.nodeCount; i++)
			{
				Node *n = &state.nodes[i];
				if (!n->render.dirty)
					continue;
				// Shift this node's commands still owe AFTER its ancestors translate theirs.
				// Every dirty ancestor with children translates its WHOLE subtree by its own
				// delta minus what its own ancestors already applied, so those applied deltas
				// telescope: the shift a descendant actually receives from above is exactly
				// the NEAREST such ancestor's delta — not the sum over the chain.
				//
				// Summing (what this did) is correct only while at most one ancestor is dirty.
				// The moment two nested containers move together — a camera wrapper and the
				// full-size world layers positioned at 0 inside it all report the same delta —
				// the inner ones compute applied == 0 and translate NOTHING, while every leaf
				// compensates for all of them. The leaf then cancels a shift no one applied and
				// its commands stay behind by that much, silently: the tree is right, the
				// display list is stale, and every later frame replays the world at the old
				// position until something forces a full record.
				int inherited_dx = 0;
				int inherited_dy = 0;
				for (int ancestor = n->parent; ancestor >= 0 && ancestor < state.nodeCount; ancestor = state.nodes[ancestor].parent)
				{
					Node *a = &state.nodes[ancestor];
					if (!a->render.dirty || a->first_child < 0)
						continue;
					inherited_dx = a->layout.x - a->layout.previous_x;
					inherited_dy = a->layout.y - a->layout.previous_y;
					break;
				}
				int dx = n->layout.x - n->layout.previous_x - inherited_dx;
				int dy = n->layout.y - n->layout.previous_y - inherited_dy;
				if (dx || dy)
				{
					if (n->first_child >= 0)
						DisplayList::instance().translateSubtreeCommands(i, dx, dy);
					else
						DisplayList::instance().translateNodeCommands(i, dx, dy);
					// The move may have slid content that was fully outside the
					// record-time clip (culled: no commands) into the viewport — a
					// camera/world wrapper panning the next level tile on screen, or an
					// independently-walking sprite re-entering from off-screen. The
					// retained list is then incomplete and replay would leave stale
					// background where that content entered, so force a full record.
#if GEA_EMBEDDED_SUBTREE_REVEAL_CHECK
					if (DisplayList::instance().subtreeRevealsUnrecordedContent(i, width, height))
					{
						can_keep_display_list = 0;
						break;
					}
#endif
				}
				if (layoutSizeChangeNeedsCommandRerecord(*n))
					state.nodeCommandDirty[i] = 1;
				if (state.nodeCommandDirty[i] && n->render.transform_dirty && n->first_child >= 0)
				{
					rebuild_for_transform = 1;
					continue;
				}
				int oldCmdX0 = 0;
				int oldCmdY0 = 0;
				int oldCmdX1 = -1;
				int oldCmdY1 = -1;
				const bool hadOldCommandBounds =
						state.nodeCommandDirty[i] &&
						DisplayList::instance().nodeCommandBounds(i, &oldCmdX0, &oldCmdY0, &oldCmdX1, &oldCmdY1);
				DisplayCommand oldSingleCommand{};
				bool hadOldSingleCommand = false;
				if (state.nodeCommandDirty[i] && DisplayList::instance().nodeCommandCount(i) == 1)
				{
					if (const DisplayCommand *oldCommand = DisplayList::instance().nodeCommandAt(i, 0))
					{
						oldSingleCommand = *oldCommand;
						hadOldSingleCommand = true;
					}
				}
				if (state.nodeCommandDirty[i] &&
						!DisplayList::instance().rerecordNodeCommands(i))
				{
					can_keep_display_list = 0;
					break;
				}
				if (state.nodeCommandDirty[i])
					rerecorded_any = true;
				if (state.nodeCommandDirty[i])
				{
					state.nodeCommandDirtyCanOverpaint[i] = 0;
					if (hadOldSingleCommand && DisplayList::instance().nodeCommandCount(i) == 1)
					{
						if (const DisplayCommand *newCommand = DisplayList::instance().nodeCommandAt(i, 0))
							state.nodeCommandDirtyCanOverpaint[i] =
									commandsAreSameOpaqueFillShape(oldSingleCommand, *newCommand) ? 1 : 0;
					}
					int newCmdX0 = 0;
					int newCmdY0 = 0;
					int newCmdX1 = -1;
					int newCmdY1 = -1;
					const bool hasNewCommandBounds =
							DisplayList::instance().nodeCommandBounds(i, &newCmdX0, &newCmdY0, &newCmdX1, &newCmdY1);
					if (hadOldCommandBounds || hasNewCommandBounds)
					{
						int bx0 = hadOldCommandBounds ? oldCmdX0 : newCmdX0;
						int by0 = hadOldCommandBounds ? oldCmdY0 : newCmdY0;
						int bx1 = hadOldCommandBounds ? oldCmdX1 : newCmdX1;
						int by1 = hadOldCommandBounds ? oldCmdY1 : newCmdY1;
						if (hasNewCommandBounds)
						{
							if (newCmdX0 < bx0)
								bx0 = newCmdX0;
							if (newCmdY0 < by0)
								by0 = newCmdY0;
							if (newCmdX1 > bx1)
								bx1 = newCmdX1;
							if (newCmdY1 > by1)
								by1 = newCmdY1;
						}
						state.nodeCommandDirtyBoundsValid[i] = 1;
						state.nodeCommandDirtyX0[i] = static_cast<int16_t>(bx0);
						state.nodeCommandDirtyY0[i] = static_cast<int16_t>(by0);
						state.nodeCommandDirtyX1[i] = static_cast<int16_t>(bx1);
						state.nodeCommandDirtyY1[i] = static_cast<int16_t>(by1);
					}
				}
			}
			if (can_keep_display_list && rerecorded_any && !rebuild_for_transform)
			{
				// Re-recorded faces carry fresh (unwelded) corners; weld them back against
				// their neighbors or the shared edges crack until the next reproject — and
				// permanently if the scene goes static.
				DisplayList::instance().weldTransformedFaces();
			}
			if (can_keep_display_list && rebuild_for_transform)
			{
				const std::int64_t recordNodeStartUs = refreshPerfNowUs();
					// Transform-only frame: if a prior full record armed it, just re-project
					// the existing commands' corners in place — skipping the full
					// clear()+recordNode() walk (the spinning cube's dominant record cost: the
					// geometry is static CSS, only the transform changes per frame). Falls back
					// to a full record on any structural/clip/command-type change.
				const bool allowSkip = direct_replay != 0 && !framebuffer_was_dirty;
				// Reproject fast path gated on the static backdrop being baked (matches spine):
				// otherwise reproject stays armed across the bake frame and re-projects the
				// stale full-scene list forever instead of re-recording lean (see backdrop notes).
				reprojected = allowSkip && DisplayList::instance().staticBackdropActive() &&
											DisplayList::instance().tryReprojectTransformed();
				if (!reprojected)
				{
					DisplayList::instance().clear();
					// This frame keeps the direct backdrop-blit replay path (unless the
					// framebuffer was externally dirtied), so let recordNode skip re-recording
					// the baked static subtrees — they're blitted, not replayed.
					DisplayList::instance().recordNode(root, 255, allowSkip);
					DisplayList::instance().weldTransformedFaces();
					DisplayList::instance().armTransformReproject(allowSkip && DisplayList::instance().staticBackdropActive());
				}
				state.displayListDirty = false;
				displayListFullRecord = !reprojected;
				perf.treeRecordNodeUs += refreshPerfNowUs() - recordNodeStartUs;
				perf.treeRecordCalls++;
				perf.treeRecordedNodes += state.nodeCount;
				perf.treeRecordedCommands += DisplayList::instance().commandCount();
			}
			else if (!can_keep_display_list)
			{
				direct_replay = 0;
				displayListFullRecord = true;
				DisplayList::instance().clear();
				DisplayList::instance().recordNode(root, 255);
				DisplayList::instance().weldTransformedFaces();
				state.displayListDirty = false;
			}
		}
		else
		{
			const std::int64_t recordNodeStartUs = refreshPerfNowUs();
			displayListFullRecord = true;
			DisplayList::instance().clear();
			DisplayList::instance().recordNode(root, 255);
			DisplayList::instance().weldTransformedFaces();
			state.displayListDirty = false;
			perf.treeRecordNodeUs += refreshPerfNowUs() - recordNodeStartUs;
			perf.treeRecordCalls++;
			perf.treeRecordedNodes += state.nodeCount;
			perf.treeRecordedCommands += DisplayList::instance().commandCount();
		}
		if (displayListFullRecord)
			perf.treeFullRecords++;
		if (reprojected)
			perf.treeReprojects++;
		perf.treeDisplayListUs += refreshPerfNowUs() - displayListStartUs;

		// Horizontal-pan fast path: when a camera/world wrapper translated the viewport
		// this frame (and nothing structural changed), memcpy-shift the viewport instead
		// of re-replaying the whole panned scene. Handles the frame fully when it engages.
		if (tryHorizontalPanRefresh(root, width, height, framebuffer_was_dirty,
																display_list_was_dirty, display_list_was_structural,
																displayListFullRecord))
		{
			return;
		}

		DirtyRegions::Rect rects[DirtyRegions::kMaxRects];
		DirtyRegions::Rect flush_rects[DirtyRegions::kMaxRects];
		int rect_count = 0;
		int flush_rect_count = 0;
		bool preserveFlushOnlyRecolorRegions = false;
		const std::int64_t dirtyCollectStartUs = refreshPerfNowUs();
		if (framebuffer_was_dirty)
			direct_replay = 0;
		const int transformedShapeCount = transformedLeafDirtyShapeCount();
		const bool preserveFlatTransformedReplayRects = direct_replay && transformedLeafDirtyShapesAreFlat2D();
		int transformedSegmentLimit = DirtyRegions::kMaxRects;
		if (transformedShapeCount > 0)
		{
			transformedSegmentLimit = DirtyRegions::kMaxRects / transformedShapeCount;
			if (transformedSegmentLimit < 1)
				transformedSegmentLimit = 1;
		}
		// Fast path for apps with zero rotated dirty leaves (e.g. bouncing-balls-jsx):
		// the draw-rect array and flush-rect array end up identical because the only
		// divergence point is appendTransformedLeafDirtyRegions, which is gated on
		// (direct_replay && rotated-leaf detected). Collect once, coalesce once, then
		// copy the result into flush_rects so normal JSX absolute-leaf motion keeps the
		// small dirty windows while replaying the command list only once.
		const bool unifiedRectPath = (transformedShapeCount == 0);
		// Only broad geometry animation should collapse to one bounding box. A
		// weather update can dirty many fixed text/image leaves without changing
		// their boxes; merging those paint-only leaves into one near-fullscreen rect
		// defeats retained painting.
		{
			int geometryDirtyLeafEstimate = 0;
			for (int i = 0; i < state.nodeCount; i++)
				if (state.nodes[i].render.dirty &&
						(state.nodes[i].render.layout_dirty || state.nodes[i].render.transform_dirty))
					geometryDirtyLeafEstimate++;
			g_broadDirtyBbox = GEA_EMBEDDED_BROAD_DIRTY_BBOX && unifiedRectPath &&
					geometryDirtyLeafEstimate >= DirtyRegions::kMaxRects;
		}
		for (int i = 0; i < state.nodeCount; i++)
		{
			if (!state.nodes[i].render.dirty)
				continue;
			Node *n = &state.nodes[i];

			int dr_x0 = width, dr_y0 = height, dr_x1 = -1, dr_y1 = -1;

			int canvas_x0 = 0, canvas_y0 = 0, canvas_x1 = -1, canvas_y1 = -1;
			const auto *canvas_surface = n->type == NodeType::Canvas ? state.canvases.find(i) : nullptr;
			const auto *canvas = canvas_surface ? canvas_surface->canvas() : nullptr;
			const bool canvas_dirty = canvas_surface &&
																canvas &&
																canvas->dirty(&canvas_x0, &canvas_y0, &canvas_x1, &canvas_y1);

			// Glyph-incremental text dirty: setText found that only a middle run of glyphs
			// changed within an unchanged box. Flush just that run across the node's height
			// instead of the whole (possibly near-fullscreen-wide) text box — a 26vmin
			// ticker then re-copies only its changed digits, fitting a TE-locked frame.
			if (n->type == NodeType::Text && n->render.text_partial_dirty && !canvas_dirty &&
					!n->render.transform_dirty && n->layout.width > 0 && n->layout.height > 0)
			{
				// Y extent: prefer the recorded glyph command bounds (cap-height..baseline,
				// no line-height leading — ~0.7x the layout box, and a union of old+new so
				// both glyphs are covered) over the full box, so the changed-digit re-flush
				// copies only the glyph rows. Only when the node didn't move/resize
				// vertically (else the old rows at the prior position must be covered too).
				int py0 = n->layout.y;
				int py1 = static_cast<int>(n->layout.y + n->layout.height - 1);
				if (state.nodeCommandDirtyBoundsValid[i] &&
						n->layout.y == n->layout.previous_y &&
						n->layout.height == n->layout.previous_height)
				{
					py0 = state.nodeCommandDirtyY0[i];
					py1 = state.nodeCommandDirtyY1[i];
				}
				DirtyRegions::Rect partial{n->render.text_dirty_x0, py0, n->render.text_dirty_x1, py1, i};
				partial = expandDirtyRect(partial, GEA_EMBEDDED_TEXT_DIRTY_GUARD_PX);
				partial = dirtyRectWithRasterGuard(*n, partial);
				addDirtyRegion(rects, &rect_count, partial, width, height);
				if (!unifiedRectPath)
					addDirtyRegion(flush_rects, &flush_rect_count, partial, width, height);
				continue;
			}

			if (!canvas_dirty &&
					direct_replay &&
					n->render.bg_recolor_pending &&
					n->first_child < 0 &&
					!hasAnyBorder(n->style) &&
					n->style.opacity == 255 &&
					rstyle(n->style).filter_blur_radius <= 0 &&
					rstyle(n->style).box_shadow_blur_radius <= 0 &&
					n->layout.x == n->layout.previous_x &&
					n->layout.y == n->layout.previous_y &&
					n->layout.width == n->layout.previous_width &&
					n->layout.height == n->layout.previous_height &&
					!n->render.transform_dirty)
			{
				int recolorX0 = 0;
				int recolorY0 = 0;
				int recolorX1 = -1;
				int recolorY1 = -1;
				if (DisplayList::instance().recolorRetainedSolidBackground(i,
																																	 n->render.bg_recolor_from,
																																	 n->render.bg_recolor_to,
																																	 &recolorX0,
																																	 &recolorY0,
																																	 &recolorX1,
																																	 &recolorY1))
				{
					const DirtyRegions::Rect recolorRect{recolorX0, recolorY0, recolorX1, recolorY1, i};
					int recolorPixels = appendCircleLikeRecolorFlushRegions(flush_rects,
																																	&flush_rect_count,
																																	*n,
																																	recolorRect,
																																	width,
																																	height);
					if (recolorPixels > 0)
					{
						preserveFlushOnlyRecolorRegions = true;
					}
					else
					{
						addDirtyRegion(flush_rects, &flush_rect_count, recolorRect, width, height);
						recolorPixels = dirtyRectArea(recolorRect);
					}
					// The recolor only replaced pixels that matched the OLD background colour
					// exactly. Every antialiased edge of a node drawn OVER this background is a
					// blend of the old colour with that node's colour, so it matched nothing and
					// is now stale (the dial's tick marks and labels keep a halo of the previous
					// background). Dirty those nodes: a scoped direct replay of their paint
					// bounds redraws background + foreground there exactly as a full replay
					// would, which is the only repair that also covers glyph runs (they have no
					// coverage function to re-blend against).
					{
						int repairRects[DirtyRegions::kMaxRects * 4];
						const int repairCount = DisplayList::instance().collectRecolorForegroundRepairRects(
								i, recolorX0, recolorY0, recolorX1, recolorY1, repairRects, DirtyRegions::kMaxRects);
						for (int ri = 0; ri < repairCount; ri++)
						{
							// origin -1: the region's pixels come from the background node and the
							// foreground node both, so the replay must walk the whole draw order.
							const DirtyRegions::Rect repair{
									repairRects[ri * 4 + 0], repairRects[ri * 4 + 1],
									repairRects[ri * 4 + 2], repairRects[ri * 4 + 3], -1};
							addDirtyRegion(rects, &rect_count, repair, width, height);
							if (!unifiedRectPath)
								addDirtyRegion(flush_rects, &flush_rect_count, repair, width, height);
						}
					}
					perf.treeBgRecolorCalls++;
					perf.treeBgRecolorPixels += recolorPixels;
					n->render.dirty = 0;
					n->render.layout_dirty = 0;
					n->render.non_scroll_dirty = 0;
					n->render.bg_recolor_pending = 0;
					continue;
				}
			}
			n->render.bg_recolor_pending = 0;

			// Moved transform-free leaf fast path (the per-frame hot path for the 64
			// bouncing balls): its dirty rect is prev-box ∪ cur-box plus the retained
			// movement guard. For a transform-free node, transformedBoundsRect()
			// returns precisely the layout
			// box — so the slow path below burns the canUseCommandDirtyBounds probe plus
			// two transformedBoundsRect() calls (with their pooled transform reads + the
			// hasTransformChain walk) only to recompute that same union. Build it directly.
			// Gated on a layout change so paint-only nodes still get the tighter
			// canUseCommandDirtyBounds rect. anyTransformActive() is cached + the dirty
			// pass runs after all mid-refresh refreshSerial bumps, so no extra scan.
			if (n->type != NodeType::Canvas && !n->render.transform_dirty &&
					n->first_child < 0 &&
					n->layout.width > 0 && n->layout.height > 0 &&
					n->layout.previous_width > 0 && n->layout.previous_height > 0 &&
					(n->layout.x != n->layout.previous_x || n->layout.y != n->layout.previous_y ||
					 n->layout.width != n->layout.previous_width ||
					 n->layout.height != n->layout.previous_height) &&
					!ViewRenderer::anyTransformActive())
			{
				const int cx0 = n->layout.x;
				const int cy0 = n->layout.y;
				const int cx1 = cx0 + n->layout.width - 1;
				const int cy1 = cy0 + n->layout.height - 1;
				const int px0 = n->layout.previous_x;
				const int py0 = n->layout.previous_y;
				const int px1 = px0 + n->layout.previous_width - 1;
				const int py1 = py0 + n->layout.previous_height - 1;
				DirtyRegions::Rect leafRect{cx0 < px0 ? cx0 : px0,
																		cy0 < py0 ? cy0 : py0,
																		cx1 > px1 ? cx1 : px1,
																		cy1 > py1 ? cy1 : py1,
																		i};
				leafRect = dirtyRectWithRetainedMoveGuard(leafRect);
				leafRect = dirtyRectWithRasterGuard(*n, leafRect);
				addDirtyRegion(rects, &rect_count, leafRect, width, height);
				if (!unifiedRectPath)
					addDirtyRegion(flush_rects, &flush_rect_count, leafRect, width, height);
				continue;
			}

			bool usedReprojectRect = false;
			const bool canUseCommandDirtyBounds =
					state.nodeCommandDirtyBoundsValid[i] &&
					(!n->render.transform_dirty || state.nodeCommandDirtyCanOverpaint[i]);
			if (state.nodeCommandDirtyBoundsValid[i] &&
					canUseCommandDirtyBounds &&
					!canvas_dirty &&
					n->layout.x == n->layout.previous_x &&
					n->layout.y == n->layout.previous_y &&
					n->layout.width == n->layout.previous_width &&
					n->layout.height == n->layout.previous_height)
			{
				dr_x0 = state.nodeCommandDirtyX0[i];
				dr_y0 = state.nodeCommandDirtyY0[i];
				dr_x1 = state.nodeCommandDirtyX1[i];
				dr_y1 = state.nodeCommandDirtyY1[i];
				usedReprojectRect = true;
			}
			else if (canvas_dirty)
			{
				dr_x0 = n->layout.x + canvas_x0;
				dr_y0 = n->layout.y + canvas_y0;
				dr_x1 = n->layout.x + canvas_x1;
				dr_y1 = n->layout.y + canvas_y1;
			}
				else if (reprojected && n->render.transform_dirty && n->first_child >= 0 &&
								 DisplayList::instance().reprojectDirtyRect(&dr_x0, &dr_y0, &dr_x1, &dr_y1))
				{
					// The reproject already projected every corner of this transformed subtree
					// and recorded prev∪cur as its dirty rect — reuse it instead of calling
					// transformedSubtreeBoundsRect twice (the dominant dirty-collect cost, ~1680us
					// on the css-3d-cube). UNCONDITIONAL, matching spine. This was previously
					// net-negative ONLY because reprojectDirtyRect was ballooned by the static
					// perspective stage floor (a non-moving rotateX command unioned into the rect →
					// screen-wide [-263,0,673,730]); render.cpp now gates that union on real
					// movement, so the reused rect is the tight moving-cube bbox and the reuse path
					// is the cheap win (dirty ~1990us → ~150us) with replay/flush unchanged.
					usedReprojectRect = true;
				}
				else if (direct_replay &&
								 (!DisplayList::instance().staticBackdropActive() ||
								canUseStaticBackdropStripDirtyForTransformedLeaf(*n)) &&
							 appendTransformedLeafDirtyRegions(rects,
																								 &rect_count,
																								 flush_rects,
																								 &flush_rect_count,
																								 *n,
																								 i,
																								 width,
																								 height,
																								 transformedSegmentLimit,
																								 !DisplayList::instance().staticBackdropActive()))
			{
				// Keep perspective/3D static-backdrop frames on the single-bbox path: per-face
				// segmented regions can create blit-edge seams. Flat 2D rotated leaves (clock
				// hands, needles) replay one coalesced bbox into the framebuffer but flush
				// segmented dirty strips, avoiding repeated clipped replay work.
				continue;
			}
			else if (n->layout.previous_width > 0 && n->layout.previous_height > 0)
			{
				DirtyRegions::Rect previousBounds = n->render.transform_dirty && n->first_child >= 0
																								? transformedSubtreeBoundsRect(i, true, i)
																								: transformedBoundsRect(*n, true, i);
				dr_x0 = previousBounds.x0;
				dr_y0 = previousBounds.y0;
				dr_x1 = previousBounds.x1;
				dr_y1 = previousBounds.y1;
			}

			if (!usedReprojectRect && !canvas_dirty && n->layout.width > 0 && n->layout.height > 0)
			{
				DirtyRegions::Rect currentBounds = n->render.transform_dirty && n->first_child >= 0
																							 ? transformedSubtreeBoundsRect(i, false, i)
																							 : transformedBoundsRect(*n, false, i);
				int nx0 = currentBounds.x0;
				int ny0 = currentBounds.y0;
				int nx1 = currentBounds.x1;
				int ny1 = currentBounds.y1;
				if (nx0 < dr_x0)
					dr_x0 = nx0;
				if (ny0 < dr_y0)
					dr_y0 = ny0;
				if (nx1 > dr_x1)
					dr_x1 = nx1;
				if (ny1 > dr_y1)
					dr_y1 = ny1;
			}

			if (!usedReprojectRect && !canvas_dirty && !n->render.transform_dirty &&
					n->layout.previous_width > 0 && n->layout.previous_height > 0 &&
					(n->layout.x != n->layout.previous_x || n->layout.y != n->layout.previous_y ||
					 n->layout.width != n->layout.previous_width ||
					 n->layout.height != n->layout.previous_height))
			{
				DirtyRegions::Rect guarded = dirtyRectWithRetainedMoveGuard(
						DirtyRegions::Rect{dr_x0, dr_y0, dr_x1, dr_y1, i});
				dr_x0 = guarded.x0;
				dr_y0 = guarded.y0;
				dr_x1 = guarded.x1;
				dr_y1 = guarded.y1;
			}

			if (dr_x0 > dr_x1 || dr_y0 > dr_y1)
				continue;

			// Transformed subtrees (the spinning cube) derive their dirty bbox from the
			// min/max of int16_t-rounded projected corners. The rasterized silhouette can
			// fall up to ~1px outside that bbox, so as the cube rotates its retreating edge
			// is left 1px un-repainted — a thin trailing seam against the backdrop. Pad the
			// rect a couple px so the previous frame's edge is always covered (clipped to the
			// viewport by addDirtyRegion; after the full sync this padding blits identical
			// backdrop, so it introduces no new seam).
			// Tag the rect with the originating dirty node id. The replay
			// path uses it for a static-dispatch fast path while still
			// replaying overlapping siblings, such as overlay labels, in the
			// original draw order.
			const DirtyRegions::Rect dirtyRect =
					dirtyRectWithRasterGuard(*n, DirtyRegions::Rect{dr_x0, dr_y0, dr_x1, dr_y1, i});
			addDirtyRegion(rects, &rect_count, dirtyRect, width, height);
			if (!unifiedRectPath)
				addDirtyRegion(flush_rects, &flush_rect_count, dirtyRect, width, height);
		}
		if (display_list_was_dirty && display_list_was_structural && width > 0 && height > 0)
		{
			// A STRUCTURAL command-list rebuild can change draw order, alpha scopes,
			// or which commands exist. Repaint the viewport once so the framebuffer
			// matches the rebuilt scene instead of relying on stale incremental
			// pixels. Content-only rebuilds (appearance changes on existing nodes —
			// see markDisplayListContentDirty) skip this: the per-node dirty rects
			// above already cover every pixel that changed.
			const DirtyRegions::Rect fullViewport{0, 0, width - 1, height - 1, root};
			addDirtyRegion(rects, &rect_count, fullViewport, width, height);
			if (!unifiedRectPath)
				addDirtyRegion(flush_rects, &flush_rect_count, fullViewport, width, height);
		}
		if (width > 0 && height > 0 && DisplayList::instance().consumeBackdropFullSyncRequest())
		{
			// The static-backdrop cache was just baked. Repaint the full viewport from it
			// once (blit the whole baked backdrop + replay the cube) so the framebuffer and
			// panel become exactly the baked backdrop everywhere — eliminating the seam at
			// later per-region blit boundaries.
			const DirtyRegions::Rect fullViewport{0, 0, width - 1, height - 1, root};
			addDirtyRegion(rects, &rect_count, fullViewport, width, height);
			if (!unifiedRectPath)
				addDirtyRegion(flush_rects, &flush_rect_count, fullViewport, width, height);
		}
		if (framebuffer_was_dirty)
		{
			// Some platforms can clear or dirty the retained framebuffer outside
			// node mutation tracking. Replay that canvas area so static nodes are
			// restored even when no corresponding DOM-ish node changed this frame.
			const DirtyRegions::Rect framebufferRect{
					framebufferDirtyX0,
					framebufferDirtyY0,
					framebufferDirtyX1,
					framebufferDirtyY1,
					root};
			addDirtyRegion(rects, &rect_count, framebufferRect, width, height);
			if (!unifiedRectPath)
				addDirtyRegion(flush_rects, &flush_rect_count, framebufferRect, width, height);
		}
		// filter: blur() is rebuilt by diffing the fully rendered filtered source
		// against its saved background. If a dirty rect touches a filtered node,
		// replay the full filtered source/output bounds so the cache never mixes
		// retained pixels with the current frame.
		expandDirtyRegionsForFilterBlur(rects, &rect_count, flush_rects, &flush_rect_count, unifiedRectPath, width, height);
		perf.treeDirtyCollectUs += refreshPerfNowUs() - dirtyCollectStartUs;
			// Done collecting (and the structural/framebuffer/filter adds above); clear the
		// broad-bbox flag so it can't leak into any later addDirtyRegion path.
		g_broadDirtyBbox = false;

		const std::int64_t dirtyCoalesceStartUs = refreshPerfNowUs();
		if (!unifiedRectPath || GEA_EMBEDDED_REPLAY_COALESCE_DIRTY_REGIONS)
			coalesceLowCostDirtyRegions(rects, &rect_count);
		if (!preserveFlatTransformedReplayRects)
			coalesceHighCoverageDirtyRegions(rects, &rect_count, width, height);
		if (unifiedRectPath)
		{
			if (flush_rect_count > 0)
			{
				if (preserveFlushOnlyRecolorRegions)
				{
					for (int i = 0; i < rect_count; i++)
					{
						if (dirtyRectCoveredByAny(flush_rects, flush_rect_count, rects[i]))
							continue;
						appendDirtyRegion(flush_rects, &flush_rect_count, rects[i], width, height);
					}
				}
				else
				{
					for (int i = 0; i < rect_count; i++)
						addDirtyRegion(flush_rects, &flush_rect_count, rects[i], width, height);
#if GEA_EMBEDDED_FLUSH_COALESCE_DIRTY_REGIONS
					coalesceLowCostDirtyRegions(flush_rects, &flush_rect_count);
					if (!preserveFlatTransformedReplayRects)
						coalesceHighCoverageDirtyRegions(flush_rects, &flush_rect_count, width, height);
#endif
				}
			}
			else
			{
				std::memcpy(flush_rects, rects, sizeof(rects[0]) * static_cast<std::size_t>(rect_count));
				flush_rect_count = rect_count;
#if GEA_EMBEDDED_FLUSH_COALESCE_DIRTY_REGIONS
				// Replay uses the tight per-region rects[] (HighCoverage only) for minimal
				// rasterize, but the FLUSH set must be LowCost-coalesced: the CO5300 pays a
				// large per-window setup penalty, so merging nearby windows (extra-area <=
				// smaller-rect, ~no real pixel cost) collapses ~24 scattered ball boxes into a
				// handful of wider windows — ~5ms flush vs ~20ms. At 52c15558 (58fps) LowCost
				// ran unconditionally on the shared rects/flush set; the unified path later
				// split them and dropped LowCost from the flush set, which is the regression.
				coalesceLowCostDirtyRegions(flush_rects, &flush_rect_count);
				if (!preserveFlatTransformedReplayRects)
					coalesceHighCoverageDirtyRegions(flush_rects, &flush_rect_count, width, height);
#endif
			}
		}
		else
		{
#if GEA_EMBEDDED_FLUSH_COALESCE_DIRTY_REGIONS
			coalesceLowCostDirtyRegions(flush_rects, &flush_rect_count);
			if (!preserveFlatTransformedReplayRects)
				coalesceHighCoverageDirtyRegions(flush_rects, &flush_rect_count, width, height);
#endif
		}
		perf.treeDirtyCoalesceUs += refreshPerfNowUs() - dirtyCoalesceStartUs;

		gea::platform::display::DisplayFlushRect flushRects[DirtyRegions::kMaxRects];
		int flushRectCount = 0;
		if (rect_count > 0)
		{
			perf.treeReplayRegions += rect_count;
			for (int i = 0; i < rect_count; i++)
			{
				if (rects[i].origin >= 0)
					perf.treeReplayOriginRegions++;
				if (perf.treeReplayRegionSampleCount < gea::embedded::ui::RefreshPerfStats::kMaxTreeReplayRegionSamples)
				{
					const int sample = perf.treeReplayRegionSampleCount++;
					perf.treeReplayRegionX0[sample] = static_cast<std::int16_t>(rects[i].x0);
					perf.treeReplayRegionY0[sample] = static_cast<std::int16_t>(rects[i].y0);
					perf.treeReplayRegionX1[sample] = static_cast<std::int16_t>(rects[i].x1);
					perf.treeReplayRegionY1[sample] = static_cast<std::int16_t>(rects[i].y1);
					perf.treeReplayRegionOrigin[sample] = static_cast<std::int16_t>(rects[i].origin);
				}
			}
		}
		const bool hasFlushOnlyRecolorRegions = preserveFlushOnlyRecolorRegions && flush_rect_count > 0;
		const bool simpleDirtyReplayAvailable =
				direct_replay && unifiedRectPath && DisplayList::instance().canReplaySimpleDirtyRegions(width, height);
		const bool simpleUnifiedReplay = simpleDirtyReplayAvailable && rect_count > 0 && !hasFlushOnlyRecolorRegions;
		const bool interleavedUnifiedFlush =
				direct_replay && unifiedRectPath && rect_count > 0 && !simpleDirtyReplayAvailable && !hasFlushOnlyRecolorRegions;
#if GEA_EMBEDDED_SIMPLE_REPLAY_DEBUG
		{
			static int b = 0;
			if ((b++ % 600) == 0)
				std::printf("[branch] txShapes=%d unifiedRect=%d simpleAvail=%d simpleUnified=%d interleaved=%d recolorOnly=%d FUSE=%d\n",
					transformedShapeCount, (int)unifiedRectPath, (int)simpleDirtyReplayAvailable,
					(int)simpleUnifiedReplay, (int)interleavedUnifiedFlush, (int)hasFlushOnlyRecolorRegions,
					(int)GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH);
		}
#endif
		if (simpleUnifiedReplay)
		{
			perf.treeDirectReplayCalls++;
#if GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH
			// Fused: skip the separate replay-into-framebuffer pass entirely. Hand the
			// coalesced dirty windows to the flush, which chunks them and calls
			// fusedReplayRaster to replay the display-list straight into each DMA buffer.
			{
				FusedReplayRasterCtx fctx{width, height};
				gea::platform::display::DisplayFlushRect sparseRects[DirtyRegions::kMaxRects];
				int sparseCount = 0;
				for (int i = 0; i < flush_rect_count && sparseCount < DirtyRegions::kMaxRects; i++)
				{
					DirtyRegions::Rect *r = &flush_rects[i];
					if (r->x0 > r->x1 || r->y0 > r->y1)
						continue;
					sparseRects[sparseCount++] =
							gea::platform::display::DisplayFlushRect{r->x0, r->y0, r->x1, r->y1};
				}
				const std::int64_t flushStartUs = refreshPerfNowUs();
				if (sparseCount > 0)
				{
					// Deferred per-chunk drain: chunks pipeline (rasterize N+1 under DMA N via
					// the depth-2 slots). The cross-rect entry-drain now spin-waits, so this no
					// longer pays the blocking cross-core wakeup that capped it at 35fps.
					gea::platform::display::Display::flushRectsRasterized(sparseRects, sparseCount, fusedReplayRaster, &fctx, /*allowPerChunkDrain=*/false);
					gea::platform::display::Display::rebindCanvasToFramebuffer();
				}
				perf.treeFlushRectsUs += refreshPerfNowUs() - flushStartUs;
			}
#else
			// A single wide flush of the dirty union avoids the per-window overhead and
			// the narrow-flush QSPI penalty (~4.5 vs ~45 px/us on the SH8601) that many
			// small per-rect flushes pay — but ONLY when the rects are clustered enough
			// that their bounding box is a tight cover. For scattered rects (e.g.
			// bouncing-balls spread across the panel) the union bbox approaches the full
			// surface while real coverage is a few percent, so one union flush
			// copies+TXes ~10x the pixels of sparse per-rect flushes — that flipped this
			// app from 62fps to 38fps. Gate the union flush on measured coverage, not
			// just rect count: take it only when many rects AND the union is a tight
			// cover (break-even on the SH8601 lands near union <= ~4.5x summed area;
			// 3x keeps a safe margin and still serves genuinely clustered scenes).
			long long sumArea = 0;
			int unionX0 = rects[0].x0;
			int unionY0 = rects[0].y0;
			int unionX1 = rects[0].x1;
			int unionY1 = rects[0].y1;
			for (int i = 0; i < rect_count; i++)
			{
				const DirtyRegions::Rect *r = &rects[i];
				sumArea += static_cast<long long>(r->x1 - r->x0 + 1) * static_cast<long long>(r->y1 - r->y0 + 1);
				if (r->x0 < unionX0)
					unionX0 = r->x0;
				if (r->y0 < unionY0)
					unionY0 = r->y0;
				if (r->x1 > unionX1)
					unionX1 = r->x1;
				if (r->y1 > unionY1)
					unionY1 = r->y1;
			}
			const long long unionArea =
					static_cast<long long>(unionX1 - unionX0 + 1) * static_cast<long long>(unionY1 - unionY0 + 1);
			const bool useUnionFlush =
					rect_count >= DirtyRegions::kMaxRects / 2 && unionArea <= sumArea * 3;
			// Replay every region hot, back-to-back, with NO flush interleaved — an
			// interleaved per-rect flush contends the replay working set and inflates
			// replay (11.8 vs 3.5ms). This is the 52c15558 (58fps) path.
				if (rect_count > 1)
				{
					bool usePerOriginReplay = rect_count <= 4;
					for (int i = 0; i < rect_count && usePerOriginReplay; i++)
						usePerOriginReplay = rects[i].origin >= 0;
					if (usePerOriginReplay)
					{
						for (int i = 0; i < rect_count; i++)
						{
							DirtyRegions::Rect *r = &rects[i];
							gea::platform::display::Display::resetClip();
							gea::platform::display::Display::setAlpha(255);
							gea::platform::display::Display::pushClip(r->x0, r->y0, r->x1 - r->x0 + 1, r->y1 - r->y0 + 1);
							const std::int64_t replayStartUs = refreshPerfNowUs();
							DisplayList::instance().replaySimpleClippedDirtyRegion(r->x0, r->y0, r->x1, r->y1, r->origin);
							perf.treeReplayUs += refreshPerfNowUs() - replayStartUs;
							gea::platform::display::Display::popClip();
						}
					}
					else
					{
						gea::platform::display::Display::resetClip();
						gea::platform::display::Display::setAlpha(255);
						const std::int64_t replayStartUs = refreshPerfNowUs();
						DisplayReplayRegion regions[DirtyRegions::kMaxRects];
						for (int i = 0; i < rect_count; i++)
						{
							DirtyRegions::Rect *r = &rects[i];
							regions[i] = DisplayReplayRegion{r->x0, r->y0, r->x1, r->y1, r->origin};
						}
						DisplayList::instance().replayDirectDirtyRegions(regions, rect_count);
						perf.treeReplayUs += refreshPerfNowUs() - replayStartUs;
					}
				}
				else
				{
					for (int i = 0; i < rect_count; i++)
					{
						DirtyRegions::Rect *r = &rects[i];
						gea::platform::display::Display::resetClip();
						gea::platform::display::Display::setAlpha(255);
						gea::platform::display::Display::pushClip(r->x0, r->y0, r->x1 - r->x0 + 1, r->y1 - r->y0 + 1);
						const std::int64_t replayStartUs = refreshPerfNowUs();
						DisplayList::instance().replaySimpleClippedDirtyRegion(r->x0, r->y0, r->x1, r->y1, r->origin);
						perf.treeReplayUs += refreshPerfNowUs() - replayStartUs;
						gea::platform::display::Display::popClip();
					}
				}
			const std::int64_t flushStartUs = refreshPerfNowUs();
			if (useUnionFlush)
			{
				// Clustered dirty: one wide bbox flush.
				gea::platform::display::Display::flush();
			}
			else
			{
				// Scattered dirty (bouncing-balls): flush the COALESCED flush_rects[], NOT
				// the raw replay rects[]. Replay above renders from the tight per-ball rects[]
				// (minimal rasterize), but the CO5300 pays a large per-window setup penalty
				// (~0.8ms/window), so flushing ~24 tight ball boxes serializes to ~20ms of
				// panel wait. flush_rects[] has already been merged by coalesceLowCostDirtyRegions
				// (extra-area <= smaller-rect) into a handful of wider windows covering the same
				// pixels — measured ~5ms flush vs ~20ms. This is the 52c15558 (58fps) path,
				// which flushed the coalesced flush_rects. flush_rects is a superset of rects
				// (every rect was added before coalescing), so each flushed window's framebuffer
				// content is already correct.
				gea::platform::display::DisplayFlushRect sparseRects[DirtyRegions::kMaxRects];
				int sparseCount = 0;
				for (int i = 0; i < flush_rect_count && sparseCount < DirtyRegions::kMaxRects; i++)
				{
					DirtyRegions::Rect *r = &flush_rects[i];
					if (r->x0 > r->x1 || r->y0 > r->y1)
						continue;
					sparseRects[sparseCount++] =
							gea::platform::display::DisplayFlushRect{r->x0, r->y0, r->x1, r->y1};
				}
				if (sparseCount > 0)
					gea::platform::display::Display::flushRects(sparseRects, sparseCount);
			}
			perf.treeFlushRectsUs += refreshPerfNowUs() - flushStartUs;
#endif
		}
		else if (interleavedUnifiedFlush)
		{
			perf.treeDirectReplayCalls++;
#if GEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH && GEA_EMBEDDED_DISPLAY_FUSE_INTERLEAVED_REPLAY_FLUSH
			FusedReplayRasterCtx fctx{width, height};
			gea::platform::display::DisplayFlushRect sparseRects[DirtyRegions::kMaxRects];
			int sparseCount = 0;
			for (int i = 0; i < flush_rect_count && sparseCount < DirtyRegions::kMaxRects; i++)
			{
				DirtyRegions::Rect *r = &flush_rects[i];
				if (r->x0 > r->x1 || r->y0 > r->y1)
					continue;
				sparseRects[sparseCount++] =
						gea::platform::display::DisplayFlushRect{r->x0, r->y0, r->x1, r->y1};
			}
			const std::int64_t flushStartUs = refreshPerfNowUs();
			if (sparseCount > 0)
			{
				gea::platform::display::Display::flushRectsRasterized(sparseRects, sparseCount, fusedDirectReplayRaster, &fctx, /*allowPerChunkDrain=*/false);
				gea::platform::display::Display::rebindCanvasToFramebuffer();
			}
			perf.treeFlushRectsUs += refreshPerfNowUs() - flushStartUs;
#else
			for (int i = 0; i < rect_count; i++)
			{
				DirtyRegions::Rect *r = &rects[i];
				gea::platform::display::Display::resetClip();
				gea::platform::display::Display::setAlpha(255);
				gea::platform::display::Display::pushClip(r->x0, r->y0, r->x1 - r->x0 + 1, r->y1 - r->y0 + 1);
				const std::int64_t replayStartUs = refreshPerfNowUs();
				const DisplayReplayRegion region{r->x0, r->y0, r->x1, r->y1, r->origin};
				DisplayList::instance().replayDirectDirtyRegions(&region, 1);
				perf.treeReplayUs += refreshPerfNowUs() - replayStartUs;
				gea::platform::display::Display::popClip();
				const std::int64_t flushStartUs = refreshPerfNowUs();
				gea::platform::display::Display::flush();
				perf.treeFlushRectsUs += refreshPerfNowUs() - flushStartUs;
			}
#endif
		}
		else
		{
			const std::int64_t replayStartUs = refreshPerfNowUs();
			if (direct_replay && rect_count > 0)
			{
				perf.treeDirectReplayCalls++;
				gea::platform::display::Display::resetClip();
				gea::platform::display::Display::setAlpha(255);
				DisplayReplayRegion regions[DirtyRegions::kMaxRects];
				for (int i = 0; i < rect_count; i++)
				{
					DirtyRegions::Rect *r = &rects[i];
					regions[i] = DisplayReplayRegion{r->x0, r->y0, r->x1, r->y1, r->origin};
				}
				DisplayList::instance().replayDirectDirtyRegions(regions, rect_count);
			}
			else
			{
				// Conservative paint path for full display-list replay.
				for (int i = 0; i < rect_count; i++)
				{
					DirtyRegions::Rect *r = &rects[i];
					gea::platform::display::Display::resetClip();
					gea::platform::display::Display::setAlpha(255);
					gea::platform::display::Display::pushClip(r->x0, r->y0, r->x1 - r->x0 + 1, r->y1 - r->y0 + 1);
					DisplayList::instance().replay();
					gea::platform::display::Display::popClip();
				}
			}
			perf.treeReplayUs += refreshPerfNowUs() - replayStartUs;
		}

		// Static-backdrop cache: once the static scene (bg + stage) has been stable for a
		// couple frames, bake it into the backdrop buffer so subsequent dirty-region
		// replays blit it and replay only the dynamic subtree (the cube) on top. Must run
		// HERE — before LayoutSnapshot::capture() clears render.dirty — so the bake can
		// tell the dynamic cube (still dirty) from the static scene and exclude it; the
		// bake rebinds the canvas to its own buffer and restores it before the flush
		// reads the framebuffer. A structural rebuild this frame (!stable) drops the
		// cache so it re-settles.
#ifndef ESP_PLATFORM
		{
			static const bool debugBackdrop = std::getenv("GEA_DEBUG_BACKDROP") != nullptr;
			if (debugBackdrop)
				std::printf("[bake-gate] simpleAvail=%d unified=%d canSimple=%d structural=%d rects=%d flushRects=%d\n",
										simpleDirtyReplayAvailable ? 1 : 0, unifiedRectPath ? 1 : 0,
										DisplayList::instance().canReplaySimpleDirtyRegions(width, height) ? 1 : 0,
										display_list_was_structural ? 1 : 0, rect_count, flush_rect_count);
		}
#endif
		DisplayList::instance().maybeBakeStaticBackdrop(
				width,
				height,
				!display_list_was_structural && staticBackdropStable,
				direct_replay != 0 && !simpleDirtyReplayAvailable && staticBackdropStable,
				staticBackdropEligible);

		const std::int64_t flushRectsStartUs = refreshPerfNowUs();
		if (!simpleUnifiedReplay && !interleavedUnifiedFlush)
		{
			for (int i = 0; i < flush_rect_count; i++)
			{
				DirtyRegions::Rect *r = &flush_rects[i];
				if (flushRectCount < DirtyRegions::kMaxRects)
				{
					if (unifiedRectPath)
					{
						flushRects[flushRectCount++] = gea::platform::display::DisplayFlushRect{r->x0, r->y0, r->x1, r->y1};
						continue;
					}
					// A partial-width CO5300 flush window leaves a 1px stale/black column at its
					// LEFT edge on the panel (not in the framebuffer). CO5300 targets anchor the
					// window at x=0; DSI targets without that artifact can keep the actual dirty
					// x0 so narrow transformed-leaf strips stay narrow on the wire.
#if GEA_EMBEDDED_FLUSH_ANCHOR_LEFT_PARTIAL_RECTS
					const int flushX0 = 0;
#else
					const int flushX0 = r->x0;
#endif
					flushRects[flushRectCount++] = gea::platform::display::DisplayFlushRect{flushX0, r->y0, r->x1, r->y1};
				}
			}
			if (flushRectCount > 0)
				gea::platform::display::Display::flushRects(flushRects, flushRectCount);
			perf.treeFlushRectsUs += refreshPerfNowUs() - flushRectsStartUs;
		}
		const std::int64_t snapshotStartUs = refreshPerfNowUs();
		LayoutSnapshot::capture();
		state.canvases.forEach([](CanvasSurfaceState &surface)
													 {
		if (auto *canvas = surface.canvas()) canvas->resetDirty(); });
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) || !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
		// displayCanvas is only declared when the direct-canvas context is off (see the
		// guard at its declaration above); with direct canvas it is always null, so the
		// resetDirty is a no-op. Guard the use to match — a global define (web build)
		// otherwise leaves this referencing an undeclared name.
		if (displayCanvas)
			displayCanvas->resetDirty();
#endif
		perf.treeSnapshotUs += refreshPerfNowUs() - snapshotStartUs;
	}

	void Tree::frame(int timestamp_ms)
	{
		auto &state = treeState();
		if (state.mountedRoot < 0)
			return;
		if (timestamp_ms < 0)
			timestamp_ms = 0;
		state.lastFrameMs = timestamp_ms;

		if (state.inputTickRequired)
			tickInput(timestamp_ms);

		// Caret blink for `<input>` focus on the pixel target. The macOS path
		// owns its own caret via NSResponder; on ESP32 InputRenderer::record
		// reads activeInputCaretVisible() each frame, and this tick is what
		// flips the bool every ~500ms.
		tickInputCaret(timestamp_ms);

		int changed = 0;
		for (int i = 0; i < state.nodeCount; i++)
		{
			Node *n = &state.nodes[i];
			if (n->style.blink_interval_ms <= 0)
			{
				if (!n->style.blink_visible)
				{
					n->style.blink_visible = 1;
					n->render.dirty = 1;
					n->render.layout_dirty = 1;
					n->render.non_scroll_dirty = 1;
					changed = 1;
				}
				continue;
			}

			int elapsed = timestamp_ms - n->style.blink_started_ms;
			if (elapsed < 0)
				elapsed = 0;
			int visible = ((elapsed / n->style.blink_interval_ms) % 2) == 0;
			if (n->style.blink_visible == visible)
				continue;
			n->style.blink_visible = visible;
			n->render.dirty = 1;
			n->render.layout_dirty = 1;
			n->render.non_scroll_dirty = 1;
			markDisplayListDirty();
			changed = 1;
		}

		// Live <camera> preview: while a camera is streaming, repaint each camera
		// leaf every frame so fresh frames reach the panel — the same idea as the
		// caret/blink ticks above, but driven by the platform preview provider
		// rather than a timer.
		CameraSurfaceProvider *cameraProvider = CameraSurface::provider();
		if (cameraProvider && cameraProvider->isStreaming())
		{
			const bool nativeOverlay = cameraProvider->previewMode() == CameraPreviewMode::NativeOverlay;
			bool hasVisibleCamera = false;
			for (int i = 0; i < state.nodeCount; i++)
			{
				if (!state.nodeActive[i])
					continue;
				Node *n = &state.nodes[i];
				if (n->type != NodeType::Camera)
					continue;
				if (n->style.display == kDisplayNone)
					continue;
				hasVisibleCamera = true;
				if (nativeOverlay)
				{
					// Feed the live visual rect to the provider every frame. (record()
					// also calls positionPreviewLayer, but it only runs while the node is
					// dirty — which we deliberately avoid here — so the camera's rect
					// would otherwise never update.)
					CameraRenderer::positionNativeOverlay(*n, cameraProvider);
					continue; // camera owns its rect; presented directly below
				}
				n->render.dirty = 1;
				n->render.layout_dirty = 1;
				n->render.non_scroll_dirty = 1;
				// Content-only: the camera's blit command is unchanged frame to frame —
				// only the surface pixels it points at are refreshed — so there's no
				// draw-order change. Using the structural markDisplayListDirty() forced a
				// full-viewport rebuild + flush every frame (the whole 1280x720 panel);
				// the content variant replays and flushes just this node's rect.
				markDisplayListContentDirty();
				changed = 1;
			}
			// NativeOverlay: the platform paints the live frame straight into the
			// framebuffer at the node's rect and flushes only that region — no
			// display-list blit, no replay. Runs every frame, independent of `changed`.
			if (nativeOverlay && hasVisibleCamera)
				cameraProvider->presentNativeOverlay();
		}

		if (changed)
			refresh(state.mountedRoot, state.mountedWidth, state.mountedHeight);
	}

} // namespace gea::embedded::ui
