// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <cstdint>

namespace gea::host {

#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
using AnimationFrameTimestamp = float;
#else
using AnimationFrameTimestamp = double;
#endif

using AnimationFrameCallback = std::function<void(AnimationFrameTimestamp)>;
using TimerCallback = std::function<void()>;

struct AnimationFramePerfStats {
	std::int64_t totalUs = 0;
	std::int64_t prepareUs = 0;
	std::int64_t callbackUs = 0;
	std::int64_t maxCallbackUs = 0;
	int pendingAtRun = 0;
	int callbackCount = 0;
	int requestCount = 0;
	int droppedCount = 0;
};

double setTimeout(TimerCallback callback, double delayMs);
double setInterval(TimerCallback callback, double delayMs);
void clearTimeout(double id = 0);
void clearInterval(double id = 0);
void resetScheduledTimers();

double requestAnimationFrame(AnimationFrameCallback callback);
void runAnimationFrameCallbacks(AnimationFrameTimestamp timestampMs);
const char *animationFrameCallbackStage();
void resetAnimationFrameCallbacks();
void animationFramePerfStatsReset();
AnimationFramePerfStats animationFramePerfStatsRead();

}  // namespace gea::host
