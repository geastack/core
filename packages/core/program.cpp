// SPDX-License-Identifier: Apache-2.0
#include "program.h"

#include <atomic>
#include <cstring>

namespace gea::framework::program {

namespace {

const Hooks *g_loaded = nullptr;
Host *g_host = nullptr;
char g_runningId[64] = "";
char g_lastError[160] = "";

// One pending request: an id written by whoever asks, read by the runtime loop.
// The flag is published last and consumed first, so the loop never reads a
// half-written id; a second request before the loop runs replaces the first.
std::atomic<bool> g_pending{false};
char g_pendingId[64] = "";

void copy(char *out, std::size_t size, const char *text)
{
	if (size == 0) return;
	std::strncpy(out, text ? text : "", size - 1);
	out[size - 1] = '\0';
}

}  // namespace

const Hooks *loaded()
{
	return g_loaded;
}

void setHost(Host *host)
{
	g_host = host;
}

Host *host()
{
	return g_host;
}

void requestLaunch(const char *id)
{
	g_pending.store(false, std::memory_order_relaxed);
	copy(g_pendingId, sizeof(g_pendingId), id);
	g_pending.store(true, std::memory_order_release);
}

bool launchPending()
{
	return g_pending.load(std::memory_order_acquire);
}

bool takeLaunchRequest(char *id, std::size_t idSize)
{
	if (!g_pending.exchange(false, std::memory_order_acq_rel)) return false;
	copy(id, idSize, g_pendingId);
	return true;
}

const char *runningId()
{
	return g_runningId;
}

const char *lastError()
{
	return g_lastError;
}

void setLoaded(const Hooks *hooks, const char *id)
{
	g_loaded = hooks;
	copy(g_runningId, sizeof(g_runningId), hooks ? id : "");
}

void setLastError(const char *error)
{
	copy(g_lastError, sizeof(g_lastError), error);
}

}  // namespace gea::framework::program
