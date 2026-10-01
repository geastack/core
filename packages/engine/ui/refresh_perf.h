// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "gea_perf_config.h"  // GEA_EMBEDDED_UI_REFRESH_PERF (+ GEA_EMBEDDED_PERF master)
#include "state_init.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#elif defined(GEA_PICO_SDK) && GEA_PICO_SDK
#include "pico/time.h"
#else
#include <chrono>
#endif

namespace gea::embedded::ui {

struct RefreshPerfStats {
	std::int64_t rootScrollScanUs = 0;
	std::int64_t rootScrollRebuildUs = 0;
	std::int64_t rootScrollScrollRectUs = 0;
	std::int64_t rootScrollStripReplayUs = 0;
	std::int64_t rootScrollScrollbarReplayUs = 0;
	std::int64_t rootScrollFullReplayUs = 0;
	std::int64_t rootScrollStreamUs = 0;
	std::int64_t rootScrollExtraReplayUs = 0;
	std::int64_t rootScrollFlushUs = 0;
	std::int64_t rootScrollSnapshotUs = 0;
	std::int64_t virtualListRegionFillUs = 0;
	std::int64_t virtualListRegionTextUs = 0;
	std::int64_t virtualListRegionThumbUs = 0;
	std::int64_t treeLayoutModeUs = 0;
	std::int64_t treeLayoutUs = 0;
	std::int64_t treeDisplayListUs = 0;
	std::int64_t treeRecordNodeUs = 0;
	std::int64_t treeSetStyleUs = 0;
	std::int64_t treeSetTextUs = 0;
	std::int64_t treeAbsModeUs = 0;
	std::int64_t treeDirtyCollectUs = 0;
	std::int64_t treeDirtyCoalesceUs = 0;
	std::int64_t treeReplayUs = 0;
	// TEMP replay-split instrumentation (simple clipped dirty replay): attribute
	// the per-region replay total to (1) the ancestor-chain background restore,
	// (2) the dynamic nodes-after-origin replay, and (3) the batched rounded-rect
	// raster flush. Lets us see whether the bouncing-balls replay cost is bg
	// re-fill, overlapping-ball overdraw, or the circle rasterizer.
	std::int64_t treeReplaySplitBgUs = 0;
	std::int64_t treeReplaySplitDynUs = 0;
	std::int64_t treeReplaySplitRasterUs = 0;
	std::int64_t treeReplayBgRestoreUs = 0;
	std::int64_t treeReplayNodeWalkUs = 0;
	std::int64_t treeReplayCommandFilterUs = 0;
	std::int64_t treeReplayCommandClipUs = 0;
	std::int64_t treeFlushRectsUs = 0;
	std::int64_t treeSnapshotUs = 0;
	std::int64_t treeBgRecolorUs = 0;
	std::int64_t treeBgRecolorFillUs = 0;
	std::int64_t treeBgRecolorEdgeUs = 0;
	std::int64_t treeBgRecolorReplayUs = 0;
	static constexpr int kMaxTreeReplayRegionSamples = 8;
	int treeReplayRegions = 0;
	int treeReplayOriginRegions = 0;
	int treeReplayCommandChecks = 0;
	int treeReplayDirectRegionCalls = 0;
	int treeReplayDirectRegionsCalls = 0;
	int treeReplaySimpleRegionCalls = 0;
	int treeReplayParallelAttempts = 0;
	int treeReplayParallelSuccesses = 0;
	int treeReplayCommandFilterCalls = 0;
	int treeReplayCommandClipCalls = 0;
	int treeReplayRegionSampleCount = 0;
	std::int16_t treeReplayRegionX0[kMaxTreeReplayRegionSamples]{};
	std::int16_t treeReplayRegionY0[kMaxTreeReplayRegionSamples]{};
	std::int16_t treeReplayRegionX1[kMaxTreeReplayRegionSamples]{};
	std::int16_t treeReplayRegionY1[kMaxTreeReplayRegionSamples]{};
	std::int16_t treeReplayRegionOrigin[kMaxTreeReplayRegionSamples]{};
	int treeReplayFillRectCommands = 0;
	int treeReplayCircleCommands = 0;
	int treeReplayRoundedRectCommands = 0;
	int treeReplayTransformedRoundedRectCommands = 0;
	int treeReplayTextCommands = 0;
	int treeReplayOtherCommands = 0;
	// Per-command-type microseconds spent in DisplayCommandDrawer::replay, so the
	// SLOW-frame log can attribute the replay total to gradients vs text vs images
	// vs fills vs everything else (rounded rects, lines, clips, alpha, ...).
	std::int64_t treeReplayFillUs = 0;
	std::int64_t treeReplayCircleUs = 0;
	std::int64_t treeReplayTextUs = 0;
	std::int64_t treeReplayGradientUs = 0;
	std::int64_t treeReplayImageUs = 0;
	std::int64_t treeReplayOtherUs = 0;
	int projectedTextCacheCalls = 0;
	int projectedTextCacheHits = 0;
	int projectedTextCacheMisses = 0;
	int projectedTextCacheFallbacks = 0;
	int projectedTextCacheSramUses = 0;
	int projectedTextCachePsramUses = 0;
	int projectedTextCacheBytes = 0;
	int projectedTextCacheSramBytes = 0;
	int projectedTextCacheMaxEntryBytes = 0;
	// Layout instrumentation: layoutNode/repositionChildren call counts (>> node
	// count signals redundant re-layout / O(n^2)) and time spent measuring text
	// (font metrics) during layout — the prime suspect for a slow full re-layout.
	int treeLayoutNodeCalls = 0;
	int treeLayoutMemoHits = 0;
	int treeScopedLayouts = 0;
	// Why the scoped-relayout attempt fell back (last occurrence):
	// 1=no stable scope found, 2=avail unknown, 3=abs containing block escapes,
	// 4=scope dims changed after layout. 0 = no fallback recorded.
	int treeScopedRejectReason = 0;
	int treeScopedRejectNode = -1;
	int treeLayoutRepositionCalls = 0;
	std::int64_t treeLayoutTextUs = 0;
#if GEA_EMBEDDED_UI_REFRESH_PERF
	// Deterministic work budgets for animation regressions; absent with perf off.
	int treeIntegerPositionFastCalls = 0;
	int treeTranslateClipChecks = 0;
#endif
	int treeSetStyleCalls = 0;
	int treeSetStyleChanged = 0;
	int treeSetStyleNoop = 0;
	int treeSetStyleLayoutChanged = 0;
	int treeSetStyleTransformChanged = 0;
	int treeSetStylePaintChanged = 0;
	int treeSetTextCalls = 0;
	int treeSetTextChanged = 0;
	int treeSetTextStable = 0;
	int treeSetTextHidden = 0;
	int treeMarkDisplayListDirtyCalls = 0;
	int treeMarkDisplayListContentDirtyCalls = 0;
	int treeMarkNodeCommandDirtyCalls = 0;
	int treeAbsContainingAreaCalls = 0;
	int treeAbsContainingAreaHits = 0;
	int treeAbsModeCalls = 0;
	int treeAbsModeFast = 0;
	int treeAbsModeFull = 0;
	int treeAbsModeNoop = 0;
	int treeAbsModeDirtyNodes = 0;
	int treeAbsModeRejectNode = -1;
	int treeAbsModeRejectReason = 0;
	int rootScrollCalls = 0;
	int rootScrollAccepted = 0;
	int rootScrollRejected = 0;
	int rootScrollMovePx = 0;
	int rootScrollViewportPx = 0;
	int rootScrollStripPx = 0;
	int rootScrollExtraDirtyNodes = 0;
	int rootScrollStreamFrames = 0;
	int rootScrollSyncFrames = 0;
	int virtualListRegionCalls = 0;
	int virtualListRegionRows = 0;
	int virtualListRegionTextCalls = 0;
	int treeRefreshCalls = 0;
	int treeDirectReplayCalls = 0;
	int treeBgRecolorCalls = 0;
	int treeBgRecolorPixels = 0;
	int treeBgRecolorFastPixels = 0;
	int treeRecordCalls = 0;
	int treeRecordedNodes = 0;
	int treeRecordedCommands = 0;
	// Display-list rebuild path taken this frame: a full clear()+recordNode()
	// (treeFullRecords) vs the transform-only corner reproject (treeReprojects).
	// A spinning-transform scene should reproject ~every frame; full records
	// showing up mid-animation mark the frames that fell off the fast path.
	int treeFullRecords = 0;
	int treeReprojects = 0;
	// Horizontal-pan fast path: a camera/world wrapper inside an overflow:hidden
	// parent shifted its `left` by dx, so the viewport is memcpy-scrolled and only
	// the revealed strip + independently-moving sprites + smeared non-uniform
	// backdrop are repainted (instead of re-replaying the whole panned viewport).
	int treePanReplayCalls = 0;
	// Total pixel area actually re-replayed by the pan fast path (sum of clipped
	// repaint regions). Far below viewport-area/frame proves the memcpy-shift is
	// avoiding the whole-viewport repaint.
	int treePanRepaintArea = 0;
};

inline RefreshPerfStats gRefreshPerfStats;

// Gate for the horizontal-pan fast path. Enabled by default. The fast path is
// verified pixel-identical to the full-replay reference across full gameplay
// (walk/fall/collide/collect/die/respawn/hurt-blink) by the per-frame check in
// test_gea_sky_hop_jsx_main.cpp. The host test flips this to compare on-vs-off.
inline bool gHorizontalPanFastPathDisabled = false;

// Per-operation refresh profiling (the ScopedRefreshStat timers wrapping setStyle,
// record-node, layout, replay, etc.) reads a hardware timer twice per timed region.
// On a hot frame (e.g. 128 setStyle calls + dozens of render-op regions) those reads
// add up, and they are NOT removed by the production log gate — only the log *output*
// is. Set GEA_EMBEDDED_UI_REFRESH_PERF=0 to compile the clock reads out (the timers
// then accumulate 0 and optimize away); the frame-level fps/phase timing is separate
// (esp_timer in the scheduler) and unaffected.
// GEA_EMBEDDED_UI_REFRESH_PERF (and the GEA_EMBEDDED_PERF master that forces it
// off) come from gea_perf_config.h, included above.

inline std::int64_t refreshPerfNowUs()
{
#if !GEA_EMBEDDED_UI_REFRESH_PERF
	return 0;
#elif defined(ESP_PLATFORM)
	return esp_timer_get_time();
#elif defined(GEA_PICO_SDK) && GEA_PICO_SDK
	return static_cast<std::int64_t>(time_us_64());
#else
	using Clock = std::chrono::steady_clock;
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
#endif
}

// Diagnostic writes must be compiled out, not redirected to a throwaway object:
// the latter still performs loads/stores in every style update and replay scan.
// A statement macro preserves surrounding if/else binding and does not evaluate
// its arguments when profiling is off.
#define GEA_REFRESH_PERF(...) \
	do { if constexpr (GEA_EMBEDDED_UI_REFRESH_PERF) { __VA_ARGS__; } } while (false)

inline RefreshPerfStats &refreshPerfStatsMutable()
{
	return gRefreshPerfStats;
}

inline void refreshPerfStatsReset()
{
	GEA_REFRESH_PERF(gRefreshPerfStats = {});
}

inline RefreshPerfStats refreshPerfStatsRead()
{
#if GEA_EMBEDDED_UI_REFRESH_PERF
	return gRefreshPerfStats;
#else
	return {};
#endif
}

}  // namespace gea::embedded::ui
