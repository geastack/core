// SPDX-License-Identifier: Apache-2.0
#include "host/timers.h"
#include "host/worker.h"

#include "gea_perf_config.h"  // GEA_EMBEDDED_RAF_PERF (+ GEA_EMBEDDED_PERF master)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <chrono>
#endif
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

namespace gea::host {

namespace {

std::atomic<const char *> callbackStage{"idle"};

std::int64_t nowUs()
{
#ifdef ESP_PLATFORM
	return esp_timer_get_time();
#else
	using Clock = std::chrono::steady_clock;
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
#endif
}

// Perf-only clock for the requestAnimationFrame queue timing below. Returns 0
// (no timer read) when raf perf is compiled out, so the bracketing reads and the
// perfStats_ accumulation fold away. nowUs() above stays the FUNCTIONAL clock
// (timer scheduling / now()); only this gated variant is used for perf timing.
[[maybe_unused]] std::int64_t rafPerfNowUs()
{
#if GEA_EMBEDDED_RAF_PERF
	return nowUs();
#else
	return 0;
#endif
}

double nextTimerId()
{
	static std::int64_t id = 1;
	return static_cast<double>(id++);
}

double normalizeDelayMs(double delayMs)
{
	if (!std::isfinite(delayMs) || delayMs < 0) return 0;
	return delayMs;
}

struct ScheduledTimer {
	double id = 0;
	TimerCallback callback;
	double delayMs = 0;
	double nextFireMs = 0;
	bool interval = false;
	bool active = true;
	bool running = false;
};

class TimerScheduler {
public:
	static TimerScheduler &instance()
	{
		static TimerScheduler scheduler;
		return scheduler;
	}

	double schedule(TimerCallback callback, double delayMs, bool interval)
	{
		if (!callback) return 0;
		auto timer = std::make_shared<ScheduledTimer>();
		timer->id = nextTimerId();
		timer->callback = std::move(callback);
		timer->delayMs = normalizeDelayMs(delayMs);
		timer->nextFireMs = currentTimeMs() + timer->delayMs;
		timer->interval = interval;
		if (timer->interval && timer->delayMs <= 0) timer->delayMs = 1;
		std::lock_guard<std::mutex> lock(mutex_);
		timers_.push_back(timer);
		return timer->id;
	}

	void clear(double id)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &timer : timers_) {
			if (timer && timer->id == id) {
				timer->active = false;
				return;
			}
		}
	}

	void run(double timestampMs)
	{
		if (!std::isfinite(timestampMs)) timestampMs = currentTimeMs();
		lastTimestampMs_ = timestampMs;

		std::vector<std::shared_ptr<ScheduledTimer>> due;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto &timer : timers_) {
				if (!timer || !timer->active || timer->running) continue;
				if (timer->nextFireMs <= timestampMs) {
					timer->running = true;
					due.push_back(timer);
				}
			}
		}

		for (auto &timer : due) {
			if (timer->active && timer->callback) timer->callback();

			std::lock_guard<std::mutex> lock(mutex_);
			timer->running = false;
			if (!timer->active) continue;
			if (timer->interval) {
				timer->nextFireMs = timestampMs + std::max(1.0, timer->delayMs);
			} else {
				timer->active = false;
			}
		}

		compactInactiveTimers();
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &timer : timers_) {
			if (timer) timer->active = false;
		}
		timers_.clear();
	}

private:
	double currentTimeMs() const
	{
		if (std::isfinite(lastTimestampMs_)) return lastTimestampMs_;
		return static_cast<double>(nowUs()) / 1000.0;
	}

	void compactInactiveTimers()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		timers_.erase(
			std::remove_if(timers_.begin(), timers_.end(), [](const auto &timer) { return !timer || (!timer->active && !timer->running); }),
			timers_.end());
	}

	std::mutex mutex_;
	std::vector<std::shared_ptr<ScheduledTimer>> timers_;
	double lastTimestampMs_ = std::numeric_limits<double>::quiet_NaN();
};

class AnimationFrameQueue {
public:
	static AnimationFrameQueue &instance()
	{
		static AnimationFrameQueue queue;
		return queue;
	}

	double request(AnimationFrameCallback callback)
	{
#if GEA_EMBEDDED_RAF_PERF
		perfStats_.requestCount++;
#endif
		if (!callback) {
#if GEA_EMBEDDED_RAF_PERF
			perfStats_.droppedCount++;
#endif
			return 0;
		}
		const double id = nextTimerId();
		pending_.push_back(std::move(callback));
		if (debugLoggingEnabled())
			logDebug("request id=" + std::to_string(static_cast<long long>(id)) +
			         " pending=" + std::to_string(pending_.size()));
		return id;
	}

	void run(AnimationFrameTimestamp timestampMs)
	{
		[[maybe_unused]] const std::int64_t runStartUs = rafPerfNowUs();
		const int count = static_cast<int>(pending_.size());
#if GEA_EMBEDDED_RAF_PERF
		perfStats_.pendingAtRun += count;
#endif
		[[maybe_unused]] const std::int64_t prepareStartUs = rafPerfNowUs();
		std::vector<AnimationFrameCallback> active;
		active.swap(pending_);
		if (debugLoggingEnabled())
			logDebug("run count=" + std::to_string(count) + " ts=" + std::to_string(timestampMs));
#if GEA_EMBEDDED_RAF_PERF
		perfStats_.prepareUs += rafPerfNowUs() - prepareStartUs;
#endif

		for (int i = 0; i < count; i++) {
			if (active[i]) {
				[[maybe_unused]] const std::int64_t callbackStartUs = rafPerfNowUs();
				active[i](timestampMs);
#if GEA_EMBEDDED_RAF_PERF
				const std::int64_t callbackUs = rafPerfNowUs() - callbackStartUs;
				perfStats_.callbackUs += callbackUs;
				if (callbackUs > perfStats_.maxCallbackUs) perfStats_.maxCallbackUs = callbackUs;
				perfStats_.callbackCount++;
#endif
			}
		}
		if (debugLoggingEnabled())
			logDebug("run done nextPending=" + std::to_string(pending_.size()));
#if GEA_EMBEDDED_RAF_PERF
		perfStats_.totalUs += rafPerfNowUs() - runStartUs;
#endif
	}

	void reset()
	{
		pending_.clear();
	}

	void resetPerfStats()
	{
		perfStats_ = {};
	}

	AnimationFramePerfStats perfStats() const
	{
		return perfStats_;
	}

private:
	// Cached once: building the logDebug string argument (std::to_string(double) +
	// concat) ran EVERY frame at 3 call sites even though GEA_RAF_DEBUG_LOG is never
	// set on-device — ~348µs/frame of pure waste (the whole "prepare" phase). Gate the
	// call sites on this so the strings are only built when logging is actually on.
	static bool debugLoggingEnabled()
	{
		static const bool enabled = [] {
			const char *p = std::getenv("GEA_RAF_DEBUG_LOG");
			return p && p[0] != '\0';
		}();
		return enabled;
	}

	void logDebug(const std::string &message)
	{
		const char *path = std::getenv("GEA_RAF_DEBUG_LOG");
		if (!path || path[0] == '\0') return;
		if (++debugLogCount_ > 400) return;
		if (FILE *file = std::fopen(path, "a")) {
			std::fprintf(file, "[raf] %s\n", message.c_str());
			std::fclose(file);
		}
	}

	std::vector<AnimationFrameCallback> pending_{};
	AnimationFramePerfStats perfStats_{};
	int debugLogCount_ = 0;
};

}  // namespace

double setTimeout(TimerCallback callback, double delayMs)
{
	if (workers::Context::isWorkerCurrent()) return workers::Context::current()->schedule(std::move(callback), delayMs, false);
	return TimerScheduler::instance().schedule(std::move(callback), delayMs, false);
}

double setInterval(TimerCallback callback, double delayMs)
{
	if (workers::Context::isWorkerCurrent()) return workers::Context::current()->schedule(std::move(callback), delayMs, true);
	return TimerScheduler::instance().schedule(std::move(callback), delayMs, true);
}

void clearTimeout(double id)
{
	if (workers::Context::isWorkerCurrent()) { workers::Context::current()->cancel(id); return; }
	TimerScheduler::instance().clear(id);
}

void clearInterval(double id)
{
	if (workers::Context::isWorkerCurrent()) { workers::Context::current()->cancel(id); return; }
	TimerScheduler::instance().clear(id);
}

void resetScheduledTimers()
{
	TimerScheduler::instance().reset();
}

double requestAnimationFrame(AnimationFrameCallback callback)
{
	return AnimationFrameQueue::instance().request(std::move(callback));
}

void runAnimationFrameCallbacks(AnimationFrameTimestamp timestampMs)
{
	callbackStage.store("worker_callbacks", std::memory_order_relaxed);
	workers::Context::runMainPending();
	callbackStage.store("timer_callbacks", std::memory_order_relaxed);
	TimerScheduler::instance().run(static_cast<double>(timestampMs));
	callbackStage.store("animation_callbacks", std::memory_order_relaxed);
	AnimationFrameQueue::instance().run(timestampMs);
	callbackStage.store("idle", std::memory_order_relaxed);
}

const char *animationFrameCallbackStage()
{
	return callbackStage.load(std::memory_order_relaxed);
}

void resetAnimationFrameCallbacks()
{
	AnimationFrameQueue::instance().reset();
}

void animationFramePerfStatsReset()
{
	AnimationFrameQueue::instance().resetPerfStats();
}

AnimationFramePerfStats animationFramePerfStatsRead()
{
	return AnimationFrameQueue::instance().perfStats();
}

}  // namespace gea::host
