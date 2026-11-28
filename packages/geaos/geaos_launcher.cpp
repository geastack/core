// SPDX-License-Identifier: Apache-2.0
#include "geaos.h"

#include <mutex>

namespace gea::geaos {

namespace {

std::string signatureOf(const ScanResult &result);

struct State {
	State() : signature(signatureOf(ScanResult{})) {}
	std::mutex mutex;
	AppHost *host = nullptr;
	Fs *fs = nullptr;
	std::vector<Volume> volumes;
	ScanResult apps;
	std::string signature;  // what the last scan found, to tell a change
	std::string lastError;
	std::uint32_t generation = 0;
	// What launch() last started, and whether its volume has gone since.
	std::optional<InstalledApp> launched;
	bool launchedSeenRunning = false;  // a host may load in the background
	bool launchedVolumeLost = false;
};

State &state()
{
	static State s;
	return s;
}

std::string signatureOf(const ScanResult &result)
{
	std::string out;
	const auto add = [&](const InstalledApp &app) {
		out += app.manifest.id + '\x1f' + app.manifest.version.toString() + '\x1f' + app.manifest.name + '\x1f' +
		       app.manifest.abi + '\x1f' + app.location + '\x1e';
	};
	for (const InstalledApp &app : result.apps) add(app);
	out += '\x1d';
	for (const InstalledApp &app : result.shadowed) add(app);
	out += '\x1d';
	for (const BrokenApp &app : result.broken) out += app.location + '\x1f' + app.reason + '\x1e';
	return out;
}

}  // namespace

void Launcher::setHost(AppHost *host)
{
	std::lock_guard<std::mutex> lock(state().mutex);
	state().host = host;
}

AppHost *Launcher::host()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().host;
}

namespace {

// The launched app, if the host still runs it (an app can exit on its own).
// Called with the lock held.
std::optional<InstalledApp> stillRunning(State &s)
{
	if (!s.launched || !s.host) return std::nullopt;
	const char *running = s.host->runningAppId();
	if (running && s.launched->manifest.id == running) {
		s.launchedSeenRunning = true;
		return s.launched;
	}
	// Still loading (nothing runs yet): keep it. Gone, or replaced: forget it.
	if ((!running || !running[0]) && !s.launchedSeenRunning) return std::nullopt;
	s.launched.reset();
	s.launchedSeenRunning = false;
	s.launchedVolumeLost = false;
	return std::nullopt;
}

}  // namespace

void Launcher::addVolume(const Volume &volume)
{
	std::optional<InstalledApp> restored;
	AppHost *host = nullptr;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		bool replaced = false;
		for (Volume &existing : state().volumes) {
			if (existing.id == volume.id) {
				existing = volume;
				replaced = true;
				break;
			}
		}
		if (!replaced) state().volumes.push_back(volume);
		std::optional<InstalledApp> running = stillRunning(state());
		if (running && state().launchedVolumeLost && running->volumeId == volume.id) {
			state().launchedVolumeLost = false;
			restored = running;
			host = state().host;
		}
	}
	// Outside the lock: a host may call back into the launcher.
	if (restored && host) host->volumeRestored(*restored);
}

void Launcher::removeVolume(std::string_view id)
{
	std::optional<InstalledApp> lost;
	AppHost *host = nullptr;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		auto &volumes = state().volumes;
		bool removed = false;
		for (auto it = volumes.begin(); it != volumes.end(); ++it) {
			if (it->id == id) {
				volumes.erase(it);
				removed = true;
				break;
			}
		}
		if (!removed) return;
		std::optional<InstalledApp> running = stillRunning(state());
		if (running && !state().launchedVolumeLost && running->volumeId == id) {
			state().launchedVolumeLost = true;
			lost = running;
			host = state().host;
		}
	}
	if (lost && host) host->volumeLost(*lost);
}

void Launcher::orderVolumes(const std::vector<std::string> &ids)
{
	std::lock_guard<std::mutex> lock(state().mutex);
	std::vector<Volume> ordered;
	std::vector<Volume> rest = state().volumes;
	for (const std::string &id : ids) {
		for (auto it = rest.begin(); it != rest.end(); ++it) {
			if (it->id != id) continue;
			ordered.push_back(*it);
			rest.erase(it);
			break;
		}
	}
	for (Volume &volume : rest) ordered.push_back(std::move(volume));
	state().volumes = std::move(ordered);
}

std::vector<Volume> Launcher::volumes()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().volumes;
}

std::optional<Volume> Launcher::installVolume()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	for (const Volume &volume : state().volumes)
		if (volume.installTarget) return volume;
	if (state().volumes.empty()) return std::nullopt;
	return state().volumes.front();
}

bool Launcher::rescan()
{
	std::vector<Volume> volumes;
	AppHost *host = nullptr;
	Fs *fs = nullptr;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		volumes = state().volumes;
		host = state().host;
		fs = state().fs ? state().fs : &posixFs();
	}
	// Scan outside the lock: it reads storage and can take a while.
	ScanResult result = scan(volumes, *fs, host ? host->targetKey() : "");
	if (host) {
		for (InstalledApp &app : host->bundledApps()) result.apps.push_back(std::move(app));
	}
	const std::string signature = signatureOf(result);
	std::lock_guard<std::mutex> lock(state().mutex);
	const bool changed = signature != state().signature;
	state().apps = std::move(result);
	state().signature = signature;
	if (changed) state().generation++;
	return changed;
}

std::uint32_t Launcher::generation()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().generation;
}

Fs &Launcher::fs()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().fs ? *state().fs : posixFs();
}

namespace {

std::optional<Volume> volumeFor(std::string_view volumeId, InstallResult &result)
{
	std::optional<Volume> volume;
	if (volumeId.empty()) {
		volume = Launcher::installVolume();
		if (!volume) result.error = "no storage to install to";
	} else {
		for (const Volume &v : Launcher::volumes())
			if (v.id == volumeId) volume = v;
		if (!volume) result.error = "no volume \"" + std::string(volumeId) + "\"";
	}
	return volume;
}

InstallOptions optionsForHost()
{
	InstallOptions options;
	if (AppHost *host = Launcher::host()) {
		options.targetKey = host->targetKey();
		options.abi = host->abi();
		options.maxProgramBytes = host->maxProgramBytes();
	}
	if (const char *running = Launcher::runningAppId()) options.runningId = running;
	options.allowUnverified = !deviceSettings().getBool(settingKeys::kTrustRequireVerified, false);
	return options;
}

}  // namespace

InstallResult Launcher::install(File &source, std::string_view volumeId)
{
	InstallResult result;
	std::optional<Volume> volume = volumeFor(volumeId, result);
	if (!volume) return result;
	result = installPackage(fs(), source, *volume, optionsForHost());
	rescan();
	return result;
}

InstallResult Launcher::installStaged(const std::string &staged, std::string_view volumeId)
{
	InstallResult result;
	std::optional<Volume> volume = volumeFor(volumeId, result);
	if (!volume) return result;
	result = gea::geaos::installStaged(fs(), staged, *volume, optionsForHost());
	rescan();
	return result;
}

bool Launcher::uninstall(std::string_view id)
{
	rescan();
	std::optional<InstalledApp> app = find(id);
	if (!app) {
		setLastError("no installed app \"" + std::string(id) + "\"");
		return false;
	}
	const char *running = runningAppId();
	if (running && app->manifest.id == running) {
		setLastError(app->manifest.name + " is running; return to Home first");
		return false;
	}
	std::string error;
	const bool ok = uninstallApp(fs(), *app, error);
	rescan();
	setLastError(ok ? "" : error);
	// Its permissions and settings go with it (unless another copy of the
	// same id remains on another volume).
	if (ok && !find(app->manifest.id) && !deviceSettings().path().empty()) {
		deviceSettings().forgetApp(app->manifest.id);
		deviceSettings().save(fs());
	}
	return ok;
}

bool Launcher::move(std::string_view id, std::string_view volumeId)
{
	rescan();
	std::optional<InstalledApp> app = find(id);
	if (!app) {
		setLastError("no installed app \"" + std::string(id) + "\"");
		return false;
	}
	const char *running = runningAppId();
	if (running && app->manifest.id == running) {
		setLastError(app->manifest.name + " is running; return to Home first");
		return false;
	}
	std::optional<Volume> target;
	for (const Volume &volume : volumes())
		if (volume.id == volumeId) target = volume;
	if (!target) {
		setLastError("no volume \"" + std::string(volumeId) + "\"");
		return false;
	}
	InstallOptions options;
	if (AppHost *h = host()) {
		options.targetKey = h->targetKey();
		options.abi = h->abi();
		options.maxProgramBytes = h->maxProgramBytes();
	}
	const InstallResult result = moveApp(fs(), *app, *target, options);
	rescan();
	setLastError(result.error);
	return result.ok;
}

std::optional<InstalledApp> Launcher::launchedApp()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return stillRunning(state());
}

bool Launcher::launchedAppVolumeLost()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return stillRunning(state()) && state().launchedVolumeLost;
}

ScanResult Launcher::apps()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().apps;
}

std::optional<InstalledApp> Launcher::find(std::string_view id)
{
	std::lock_guard<std::mutex> lock(state().mutex);
	for (const InstalledApp &app : state().apps.apps)
		if (app.manifest.id == id) return app;
	return std::nullopt;
}

bool Launcher::launch(std::string_view id)
{
	AppHost *host = Launcher::host();
	if (!host) {
		setLastError("this firmware cannot run apps");
		return false;
	}
	// Storage as it is now, not as Home last listed it: an app reinstalled
	// since (a new build over USB, a card swapped) must launch as what it is.
	// Mosaico kept refusing a freshly installed Neon as "built for firmware
	// ABI <old>" from the list it scanned at boot.
	rescan();
	std::optional<InstalledApp> found = find(id);
	if (!found) {
		setLastError("no installed app \"" + std::string(id) + "\"");
		return false;
	}
	const InstalledApp app = *found;
	if (app.revoked) {
		setLastError(app.manifest.name + " is from a revoked developer (" + app.developer + ") and cannot run");
		return false;
	}
	std::string error;
	if (!checkCompatible(app.manifest, host->abi(), error) || !host->launch(app, error)) {
		setLastError(error.empty() ? "cannot launch " + app.manifest.name : error);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		state().launched = app;
		state().launchedSeenRunning = false;
		state().launchedVolumeLost = false;
	}
	setLastError("");
	return true;
}

bool Launcher::home()
{
	AppHost *host = Launcher::host();
	if (!host) return false;
	std::string error;
	if (!host->home(error)) {
		setLastError(error);
		return false;
	}
	return true;
}

const char *Launcher::runningAppId()
{
	AppHost *host = Launcher::host();
	if (!host) return "";
	const char *running = host->runningAppId();
	return running ? running : "";
}

const char *Launcher::currentId()
{
	AppHost *host = Launcher::host();
	if (!host) return "";
	const char *running = host->runningAppId();
	return running && running[0] ? running : host->firmwareId();
}

bool Launcher::appRunning()
{
	const char *running = runningAppId();
	return running && running[0];
}

std::string Launcher::lastError()
{
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().lastError;
}

void Launcher::setLastError(std::string_view error)
{
	std::lock_guard<std::mutex> lock(state().mutex);
	state().lastError = std::string(error);
}

void Launcher::reset(Fs *fs)
{
	std::lock_guard<std::mutex> lock(state().mutex);
	state().host = nullptr;
	state().fs = fs;
	state().volumes.clear();
	state().apps = ScanResult{};
	state().signature = signatureOf(ScanResult{});
	state().lastError.clear();
	state().generation = 0;
	state().launched.reset();
	state().launchedSeenRunning = false;
	state().launchedVolumeLost = false;
}

}  // namespace gea::geaos
