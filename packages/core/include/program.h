// SPDX-License-Identifier: Apache-2.0
// The program the runtime is running, when it is not the firmware's own.
//
// A firmware links one program (the generated app) and the runtime reaches it
// through link-time symbols: __gea_top_level, the stylesheet prelude,
// generated::drainMicrotasks, the asset and font registries. A firmware that
// hosts installable programs (.geapp) can replace that program at run time
// with one it loaded; the loaded program reaches the runtime through the same
// entry points, collected here in a table. While no program is loaded every
// caller uses the link-time symbols exactly as before.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::framework::graphics {
struct RasterizedFontData;
}

namespace gea::framework::program {

struct Hooks {
	void (*topLevel)() = nullptr;
	void (*registerPrelude)() = nullptr;
	void (*drainMicrotasks)() = nullptr;
	// Resets every module cell, so what the program held can be collected
	// before its static destructors run (geatsc --release-globals-symbol).
	void (*releaseGlobals)() = nullptr;
	bool (*assetLookup)(const char *path, const unsigned char **data, unsigned long *length) = nullptr;
	bool (*assetsPrepare)() = nullptr;
	void (*fontsEnsureLinked)() = nullptr;
	const graphics::RasterizedFontData *(*lookupFont)(int fontId) = nullptr;
	const graphics::RasterizedFontData *(*lookupFontForFamily)(int familyId, int sizePx) = nullptr;
	int (*lookupFontFamily)(const char *family) = nullptr;
	const char *(*lookupFontFamilyName)(int familyId) = nullptr;
	const std::uint8_t *(*lookupRuntimeTtfFontForFamily)(int familyId, unsigned long *length) = nullptr;
	const int *(*runtimeTtfDeclaredCodepoints)(int *count) = nullptr;
};

// The loaded program's hooks, or nullptr while the firmware's own program runs.
const Hooks *loaded();

// What loads and unloads programs: the platform's .geapp loader.
class Host {
public:
	virtual ~Host() = default;
	// Loads the program `id` names and returns its hooks, or nullptr with the
	// reason in `error`.
	virtual const Hooks *load(const char *id, char *error, std::size_t errorSize) = 0;
	// Runs the program's static destructors. Its code stays mapped.
	virtual void finalize(const Hooks *hooks) = 0;
	// Frees the program's image. Nothing may point into it afterwards.
	virtual void release(const Hooks *hooks) = 0;
	// Applies the program's per-app settings (present mode, ...) before it mounts.
	virtual void configure(const char *id) { (void)id; }
	// Optional: load() split so the program that is running keeps drawing (a
	// launcher's loading animation) while the next one is read from storage.
	// beginLoad starts the part that needs no runtime state on a task of its
	// own: false with `error` empty means the host cannot, and load() is used;
	// false with a reason is a failed launch. loadDone() polls it from the
	// runtime's loop. finishLoad runs on the runtime's task, after the running
	// program is unmounted, and returns what load() would have.
	virtual bool beginLoad(const char *id, char *error, std::size_t errorSize)
	{
		(void)id;
		if (errorSize) error[0] = '\0';
		return false;
	}
	virtual bool loadDone() { return true; }
	virtual const Hooks *finishLoad(char *error, std::size_t errorSize)
	{
		if (errorSize) error[0] = '\0';
		return nullptr;
	}
};

void setHost(Host *host);
Host *host();

// Asks the runtime to replace the running program with `id`, or with the
// firmware's own program when `id` is null or empty. The switch runs on the
// runtime's loop between frames, never inside a callback of the program being
// replaced, so a program may request it from its own event handlers.
void requestLaunch(const char *id);
bool launchPending();
// Takes the pending request: copies its id (empty = the firmware's program).
bool takeLaunchRequest(char *id, std::size_t idSize);

// The id of the running loaded program ("" for the firmware's own).
const char *runningId();
// Why the last launch failed ("" if it did not).
const char *lastError();

// Called by the runtime's switch only.
void setLoaded(const Hooks *hooks, const char *id);
void setLastError(const char *error);

}  // namespace gea::framework::program
