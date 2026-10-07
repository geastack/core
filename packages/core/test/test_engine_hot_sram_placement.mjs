// The RP2350 renderer keeps its hot path in time-critical SRAM. The macros and
// the annotated functions live here, in the engine; a board opts in with
// GEA_EMBEDDED_RENDER_HOT_SRAM=1.
//
// These assertions came from geastack/targets, which was reading this
// repository's sources through a sibling-checkout path. Each repo now guards
// its own files; the boards that switch these features ON assert that in their
// own target tests.
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const source = (rel) => readFileSync(new URL(`../../engine/ui/${rel}`, import.meta.url), 'utf8')

const render = source('render.cpp')
const view = source('view.cpp')
const text = source('text.cpp')
const nodeModel = source('node_model.h')

assert.match(render, /#define GEA_RENDER_HOT_SRAM __attribute__\(\(section\("\.time_critical\.gea_render"\)\)\)/, 'renderer should map hot functions to Pico time-critical SRAM when enabled')
assert.match(view, /#define GEA_VIEW_HOT_SRAM_SECTION\(name\) __attribute__\(\(noinline, noclone, section\("\.time_critical\.gea_view\." name\)\)\)/, 'view geometry should support Pico time-critical SRAM placement when enabled')
assert.match(view, /GEA_VIEW_HOT_SRAM_SECTION\("transform_rect_corners"\) transformRectCorners/, 'view geometry should annotate transformed corner projection for optional SRAM placement')
assert.match(view, /GEA_VIEW_HOT_SRAM_SECTION\("average_depth_compute"\) averageDepthCompute/, 'view geometry should annotate 3D depth sort projection for optional SRAM placement')
assert.match(view, /GEA_VIEW_HOT_SRAM_SECTION\("view_renderer_transformed_bounds"\) ViewRenderer::transformedBounds/, 'view renderer should annotate transformed bounds for optional SRAM placement')
assert.match(view, /GEA_VIEW_HOT_SRAM_SECTION\("view_renderer_record_box"\) ViewRenderer::recordBox/, 'view renderer should annotate 3D display-list recording for optional SRAM placement')
assert.match(text, /#define GEA_TEXT_HOT_SRAM __attribute__\(\(noinline, noclone, section\("\.time_critical\.gea_text\.record"\)\)\)/, 'text renderer should support Pico time-critical SRAM placement when enabled')
assert.match(text, /void GEA_TEXT_HOT_SRAM TextRenderer::record/, 'text renderer should annotate cube label recording for optional SRAM placement')
assert.match(render, /gDisplayNodeDrawStart\[kMaxNodes\][\s\S]*?section\("\.uninitialized_data"\)/, 'renderer should provide target-gated SRAM node draw-start storage')
assert.match(render, /gDisplayDrawNodeBBox\[kMaxNodes \* 4\][\s\S]*?section\("\.uninitialized_data"\)/, 'renderer should provide target-gated SRAM node bbox storage')
assert.match(render, /gDisplayReplayClipPushed\[kMaxNodes\][\s\S]*?section\("\.uninitialized_data"\)/, 'renderer should provide target-gated SRAM replay clip storage')
assert.match(render, /gDisplayRecordChildrenScratch\[kScratchDepth \* kMaxChildren\][\s\S]*?section\("\.uninitialized_data"\)/, 'renderer should provide target-gated SRAM record children storage')
assert.match(render, /nodeScratchExternal = true/, 'renderer should track external SRAM node scratch ownership')
assert.match(render, /replayClipExternal = true/, 'renderer should track external SRAM replay clip ownership')
assert.match(render, /recordChildrenExternal = true/, 'renderer should track external SRAM record children ownership')
assert.match(render, /if \(!nodeScratchExternal\)[\s\S]*?Allocator::free\(drawNodeBBox\)/, 'renderer should not free static SRAM node scratch and should free heap bboxes')
assert.match(render, /if \(!replayClipExternal\)[\s\S]*?Allocator::free\(replayClipPushed\)/, 'renderer should not free static SRAM replay clip storage')
assert.match(render, /if \(!recordChildrenExternal\)[\s\S]*?Allocator::free\(recordChildrenScratch\)/, 'renderer should not free static SRAM record children storage')
assert.match(render, /GEA_RENDER_HOT_SRAM void drawTransformedLinearGradient/, 'translucent cube face rasterizer should be eligible for SRAM execution')
assert.match(render, /buildScanEdges\(vx, vy, vp, outerEdges\)/, 'transformed-gradient rasterizer should support target-gated cached scan edges')
assert.match(render, /GEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_LOCK/, 'transformed-gradient rasterizer should support a target-gated LUT lock for core1 replay')
// The bank storage moved out of .bss into a lazy heap allocation, so an app that
// never draws a transformed gradient reserves nothing. Still one bank per core.
assert.match(render, /new FaceLutSlot\[kTransformedGradientLutBanks\]\[kTransformedGradientLutSlots\]\(\)/, 'transformed-gradient rasterizer should support per-core LUT banks')
assert.match(render, /lutBank %= kTransformedGradientLutBanks/, 'transformed-gradient rasterizer should select a LUT bank from the render core id')
assert.match(render, /GEA_EMBEDDED_RENDER_PARALLEL_DIRTY_REPLAY && GEA_RENDER_SPLIT_ALLOWED && allowSplit/, 'renderer should combine target and offscreen safety gates before coarse dirty-region replay')
assert.match(render, /#define GEA_RENDER_SPLIT_ALLOWED \(!gea::embedded::ui::gSnapshotRasterActive && !gea::embedded::ui::gBandedRasterActive\)/, 'banded snapshots must keep workers away from the temporarily rebound canvas')
assert.match(render, /#define GEA_RENDER_SPLIT_ALLOWED \(!gea::embedded::ui::gSnapshotRasterActive\)/, 'ordinary offscreen snapshots must also disable framebuffer worker replay')
assert.match(render, /PaintOrder::compareNodes\(state\.drawNodeOrder\[j\], node\) > 0/, 'reproject draw-order sorting should reuse the recorder paint-order comparison (stack levels, depth, z-index) instead of a private key')
assert.match(render, /GEA_RENDER_HOT_SRAM void drawProjectedText/, 'Tufty should keep visible projected cube labels in SRAM')
assert.match(render, /sramCoveragePool\[kProjectedTextCacheBanks\]\[GEA_EMBEDDED_PROJECTED_TEXT_SRAM_CACHE_BYTES\]/, 'projected text should support target-gated SRAM coverage buffers')
assert.match(render, /static void GEA_RENDER_HOT_SRAM replay\(const DisplayCommand &c\)/, 'Tufty should keep command replay dispatch in SRAM')
assert.match(render, /static void GEA_RENDER_HOT_SRAM replayDirectDirtyRegions/, 'Tufty should keep dirty-region replay in SRAM')
assert.match(render, /GEA_RENDER_HOT_SRAM bool retainedBackgroundCommandCanBeSkippedInRegion/, 'Tufty should keep retained-background replay checks in SRAM')
assert.match(render, /auto blendTextRgb565/, 'projected text should hoist constant RGB565 foreground expansion')
assert.match(nodeModel, /RareStyle sram_\[GEA_EMBEDDED_RARE_STYLE_SRAM_POOL_ENTRIES\]/, 'RareStyle pool should keep a target-sized first tier in SRAM')
assert.match(nodeModel, /std::deque<RareStyle> spill_/, 'RareStyle pool should spill beyond the SRAM tier instead of hard-failing larger apps')
