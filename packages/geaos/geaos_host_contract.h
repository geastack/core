// SPDX-License-Identifier: Apache-2.0
// The contract every GeaOS host keeps, as a check a host's own test runs
// against a live instance. Header-only so a target's test can use it without
// linking anything beyond the app model.
//
// It checks only what holds on every host, in any state the host can be
// constructed in: it never launches anything, and it calls home() only when
// Home is already showing (where it must refuse without side effects).
#pragma once

#include "geaos.h"

#include <set>
#include <string>

namespace gea::geaos {

// Appends one line per broken rule to `failures`; true if there were none.
inline bool checkHostContract(AppHost &host, std::string &failures)
{
	const std::size_t before = failures.size();
	auto fail = [&](const std::string &rule) { failures += rule + "\n"; };

	const char *key = host.targetKey();
	if (!key || !key[0]) fail("targetKey() must name the program entry this host runs");
	else if (!isSafeEntryName(programEntryName(key))) fail("targetKey() must be usable as a package entry name");
	const char *abi = host.abi();
	if (!abi || !abi[0]) fail("abi() must not be empty: apps are refused without one");
	if (!host.firmwareId()) fail("firmwareId() must not be null");
	const char *running = host.runningAppId();
	if (!running) fail("runningAppId() must not be null (\"\" while Home runs)");
	else if (running[0] && !isValidAppId(running)) fail(std::string("runningAppId() is not a valid app id: ") + running);

	std::set<std::string> ids;
	for (const InstalledApp &app : host.bundledApps()) {
		const std::string &id = app.manifest.id;
		if (!isValidAppId(id)) fail("bundled app with an invalid id: " + id);
		if (!ids.insert(id).second) fail("bundled app listed twice: " + id);
		if (abi && app.manifest.abi != abi) fail("bundled app " + id + " must carry the host's ABI, so Launcher::launch accepts it");
		if (app.manifest.name.empty()) fail("bundled app " + id + " has no name");
		std::string reason;
		if (abi && !checkCompatible(app.manifest, abi, reason)) fail("bundled app " + id + " is refused: " + reason);
	}

	// With Home showing, home() has nowhere to go: it refuses, says why, and
	// leaves the host where it was.
	if (running && !running[0]) {
		std::string error;
		if (host.home(error)) fail("home() must refuse while Home is already running");
		else if (error.empty()) fail("home() must say why it refused");
		const char *after = host.runningAppId();
		if (!after || after[0]) fail("a refused home() must not change runningAppId()");
	}
	return failures.size() == before;
}

}  // namespace gea::geaos
