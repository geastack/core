// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <cstdint>
#include <type_traits>

namespace gea {
template <typename Signature> struct CallableObject;
template <typename T> class Optional;
struct Undefined;
}

namespace gea::host {

// Application::frame supplies integer milliseconds on every native target.
using AnimationFrameTimestamp = int;

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

double setTimeout(TimerCallback callback, double delayMs = 0.0);
template <typename Result, typename Argument, typename Missing = gea::Undefined,
          typename = decltype(static_cast<Argument>(Missing{}))>
double setTimeout(const gea::CallableObject<Result(Argument)> &callback, double delayMs = 0.0)
{
	return setTimeout(TimerCallback([callback] { callback.call(static_cast<Argument>(Missing{})); }), delayMs);
}
double setInterval(TimerCallback callback, double delayMs = 0.0);
void clearTimeout(double id = 0);
inline void clearTimeout(const gea::Undefined &) {}
template <typename Number, std::enable_if_t<std::is_arithmetic_v<Number>, int> = 0>
void clearTimeout(const gea::Optional<Number> &id)
{
	if (id.has_value()) clearTimeout(static_cast<double>(*id));
}
void clearInterval(double id = 0);
void resetScheduledTimers();

double requestAnimationFrame(AnimationFrameCallback callback);
void runAnimationFrameCallbacks(AnimationFrameTimestamp timestampMs);
const char *animationFrameCallbackStage();
void resetAnimationFrameCallbacks();
// Process-terminal, idempotent, called on the application thread before native
// realm/static teardown. Permanently rejects new timers/RAF (returning 0),
// including reentrant capture destructors; never use for an app remount.
void retireScheduledCallbacks();
void animationFramePerfStatsReset();
AnimationFramePerfStats animationFramePerfStatsRead();

}  // namespace gea::host
