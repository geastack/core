// SPDX-License-Identifier: Apache-2.0
#include "services/diagnostics.h"

#include "diagnostics_internal.h"
#include "memory.h"
#include "services/diagnostics_platform.h"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#ifdef ESP_PLATFORM
#include <atomic>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#endif

namespace gea::framework::services {

namespace {

// SPIRAM-backed ring that buffers log bytes between the producer (any task
// calling ESP_LOG) and the diagnostics network client, which drains it in
// 96-byte chunks per poll (see DiagnosticsConnection). It must comfortably hold
// the largest single burst — the ~1.5 KB per-frame perf line, written under one
// lock — plus slack for concurrent logs, or the client sees only the line's
// tail. 8 KB holds several perf lines of backlog at negligible SPIRAM cost.
constexpr std::size_t kLogRingSize = 8192;
constexpr int kDiagnosticsPort = 8081;

class DiagnosticsMemory {
public:
	static void *allocate(std::size_t size)
	{
		return gea::framework::memory::Allocator::allocatePreferSpiram(size);
	}
};

class LogRing {
public:
	bool begin()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		if (data_) return true;
		if (!data_) {
			data_ = static_cast<char *>(DiagnosticsMemory::allocate(kLogRingSize));
			if (!data_) {
				std::fputs("Diagnostics log ring allocation failed\n", stdout);
				return false;
			}
		}
		head_ = 0;
		used_ = 0;
		total_ = 0;
		return true;
	}

	void write(const char *data, std::size_t len)
	{
		if (!data || len == 0) return;
		std::lock_guard<std::mutex> guard(mutex_);
		if (!data_) return;
		// Retain the latest ringful, preserving absolute byte positions even
		// when one message itself is larger than the ring.
		const std::size_t retained = std::min(len, kLogRingSize);
		const std::size_t begin = (head_ + len - retained) % kLogRingSize;
		const std::size_t first = std::min(retained, kLogRingSize - begin);
		std::memcpy(data_ + begin, data + len - retained, first);
		std::memcpy(data_, data + len - retained + first, retained - first);
		head_ = (head_ + len) % kLogRingSize;
		used_ = std::min(kLogRingSize, used_ + len);
		total_ += len;
	}

	std::size_t oldestTotal()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		return oldestTotalLocked();
	}

	std::size_t latestTotal()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		return total_;
	}

	int copySince(std::size_t *cursor, char *dst, int cap)
	{
		if (!cursor || !dst || cap <= 0) return 0;
		std::lock_guard<std::mutex> guard(mutex_);
		if (!data_) return 0;
		std::size_t oldest = oldestTotalLocked();
		if (*cursor < oldest) *cursor = oldest;
		std::size_t available = (total_ > *cursor) ? (total_ - *cursor) : 0;
		if (available == 0) return 0;
		std::size_t count = available > static_cast<std::size_t>(cap)
			? static_cast<std::size_t>(cap)
			: available;
		const std::size_t begin = *cursor % kLogRingSize;
		const std::size_t first = std::min(count, kLogRingSize - begin);
		std::memcpy(dst, data_ + begin, first);
		std::memcpy(dst + first, data_, count - first);
		*cursor += count;
		return static_cast<int>(count);
	}

private:
	std::size_t oldestTotalLocked() const
	{
		return (total_ > used_) ? (total_ - used_) : 0;
	}

	std::mutex mutex_;
	char *data_ = nullptr;
	std::size_t head_ = 0;
	std::size_t used_ = 0;
	std::size_t total_ = 0;
};

LogRing &logRing()
{
	static LogRing ring;
	return ring;
}

#ifdef ESP_PLATFORM
std::atomic<TaskHandle_t> consoleTask{nullptr};

void consoleLogTask(void *)
{
	std::size_t cursor = logRing().oldestTotal();
	char chunk[2048];
	std::size_t pending = 0;
	for (;;) {
		const std::size_t oldest = logRing().oldestTotal();
		if (cursor < oldest) {
			std::fprintf(stdout, "[gea_log] dropped_bytes=%u\n",
				static_cast<unsigned>(oldest - cursor));
			cursor = oldest;
			pending = 0;
		}
		const int count = logRing().copySince(&cursor, chunk + pending, sizeof(chunk) - pending);
		if (count) {
			pending += static_cast<std::size_t>(count);
			std::size_t complete = pending;
			while (complete && chunk[complete - 1] != '\n') --complete;
			// UART VFS can spin while its FIFO drains. Only this background
			// task may pay that cost; no ring mutex is held during the write.
			// Emit complete lines together so GEADEV replies cannot appear
			// inside a partially emitted diagnostic line.
			if (complete) {
				std::fwrite(chunk, 1, complete, stdout);
				pending -= complete;
				std::memmove(chunk, chunk + complete, pending);
			} else if (pending == sizeof(chunk)) {
				// Bound arbitrarily long lines without pinning the console lock
				// across task sleeps. The network ring keeps the original bytes.
				flockfile(stdout);
				std::fwrite(chunk, 1, pending, stdout);
				std::fputs("\n[gea_log] continued_line=1\n", stdout);
				funlockfile(stdout);
				pending = 0;
			}
			vTaskDelay(1);
		} else {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		}
	}
}

bool beginConsoleLogTask()
{
	static std::mutex startupMutex;
	std::lock_guard<std::mutex> guard(startupMutex);
	if (consoleTask.load(std::memory_order_acquire)) return true;
	TaskHandle_t task = nullptr;
	BaseType_t created = pdFAIL;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
	// The 2 KB line staging buffer plus newlib/VFS write frames exceeds 4 KB.
	// Keep ample console stack in PSRAM; audio never uses it.
	created = xTaskCreateWithCaps(consoleLogTask, "gea_log", 64 * 1024, nullptr, 1,
		&task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
	if (created != pdPASS) {
		created = xTaskCreate(consoleLogTask, "gea_log", 8192, nullptr, 1, &task);
	}
	if (created != pdPASS) {
		std::fputs("Diagnostics console task allocation failed\n", stdout);
		return false;
	}
	consoleTask.store(task, std::memory_order_release);
	return true;
}
#endif

void emitLog(const char *data, std::size_t len)
{
	logRing().write(data, len);
#ifdef ESP_PLATFORM
	if (auto task = consoleTask.load(std::memory_order_acquire)) {
		xTaskNotifyGive(task);
	} else {
		// Preserve early boot output before the diagnostics service starts.
		std::fwrite(data, 1, len, stdout);
	}
#else
	std::fwrite(data, 1, len, stdout);
#endif
}

}  // namespace

namespace diagnostics_internal {

bool beginLogRing()
{
	return logRing().begin();
}

void writeLog(const char *data, std::size_t len)
{
	logRing().write(data, len);
}

std::size_t oldestLogTotal()
{
	return logRing().oldestTotal();
}

std::size_t latestLogTotal()
{
	return logRing().latestTotal();
}

int copyLogSince(std::size_t *cursor, char *dst, int cap)
{
	return logRing().copySince(cursor, dst, cap);
}

}  // namespace diagnostics_internal

bool DiagnosticsServer::begin()
{
	if (!diagnostics_internal::beginLogRing()) return false;
#ifdef ESP_PLATFORM
	return beginConsoleLogTask();
#else
	return true;
#endif
}

int DiagnosticsServer::port()
{
	return kDiagnosticsPort;
}

namespace {

// Format `fmt`/`args` and tee the result to the USB console (stdout) and the
// diagnostics log ring (streamed to a network client). The common short line
// formats into a stack buffer; anything longer — notably the ~1.5 KB per-frame
// perf line — re-formats into a heap buffer so the whole line, including its
// trailing newline, is emitted instead of being clipped at 255 bytes (which
// dropped the newline and ran consecutive log lines together). Returns the
// untruncated formatted length, per the esp_log_set_vprintf contract.
int formatAndEmitLog(const char *fmt, std::va_list args)
{
	char stackBuf[256];
	std::va_list argsCopy;
	va_copy(argsCopy, args);
	const int len = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
	if (len <= 0) {
		va_end(argsCopy);
		return len;
	}
	if (len < static_cast<int>(sizeof(stackBuf))) {
		emitLog(stackBuf, static_cast<std::size_t>(len));
		va_end(argsCopy);
		return len;
	}
	const std::size_t size = static_cast<std::size_t>(len) + 1;
	char *heapBuf = static_cast<char *>(gea::framework::memory::Allocator::allocatePreferSpiram(size));
	if (heapBuf) {
		std::vsnprintf(heapBuf, size, fmt, argsCopy);
		emitLog(heapBuf, static_cast<std::size_t>(len));
		gea::framework::memory::Allocator::free(heapBuf);
	} else {
		// Allocation failed: emit the clipped stack buffer rather than nothing.
		emitLog(stackBuf, sizeof(stackBuf) - 1);
	}
	va_end(argsCopy);
	return len;
}

}  // namespace

int DiagnosticsServer::vprint(const char *fmt, std::va_list args)
{
	return formatAndEmitLog(fmt, args);
}

void DiagnosticsServer::print(const char *fmt, ...)
{
	std::va_list args;
	va_start(args, fmt);
	formatAndEmitLog(fmt, args);
	va_end(args);
}

void DiagnosticsServer::start()
{
	diagnostics_internal::startServer();
}

void HeapProbe::log(const char *stage)
{
	diagnosticsPlatform().logHeapProbe(stage);
}

void StackProbe::logCurrentTask(const char *stage)
{
	diagnosticsPlatform().logCurrentTaskStackProbe(stage);
}

void HeapTaskSummary::log(const char *stage)
{
	diagnosticsPlatform().logHeapTaskSummary(stage);
}

}  // namespace gea::framework::services
