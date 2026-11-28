// SPDX-License-Identifier: Apache-2.0
#include "geaos.h"

#include <algorithm>
#include <map>

namespace gea::geaos {

namespace {

constexpr std::string_view kPackageSuffix = ".geapp";
constexpr const char *kLegacyProgram = "program.so";

bool endsWith(std::string_view text, std::string_view suffix)
{
	return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

constexpr std::size_t kMaxSignatureBytes = 4096;

// The label a scan can give without re-hashing the app (its install did):
// who signed it, whether a root vouches for them, whether they are revoked.
void labelTrust(InstalledApp &app, const std::string &signatureJson)
{
	SignatureCheck check;
	std::string error;
	const bool ok = checkSignature(signatureJson, {}, trustPolicy(), check, error);
	app.developer = check.developer;
	app.revoked = check.revoked;
	app.trust = ok ? check.trust : Trust::Unverified;
}

void scanFolder(Fs &fs, const Volume &volume, const std::string &folder, const std::string &name, std::string_view targetKey,
                std::vector<InstalledApp> &found, std::vector<BrokenApp> &broken)
{
	const auto fail = [&](std::string reason) { broken.push_back({volume.id, folder, std::move(reason)}); };
	if (!isValidAppId(name)) return fail("folder name is not a valid app id");
	std::string text;
	if (!fs.exists(folder + "/manifest.json")) return fail("no manifest.json");
	if (!fs.readFile(folder + "/manifest.json", text, kMaxManifestBytes))
		return fail("manifest.json is unreadable or larger than " + std::to_string(kMaxManifestBytes) + " bytes");
	InstalledApp app;
	std::string error;
	if (!parseManifest(text, app.manifest, error, name)) return fail(error);
	if (folderProgramPath(fs, folder, targetKey).empty()) return fail("no program for " + std::string(targetKey));
	app.volumeId = volume.id;
	app.location = folder;
	app.layout = AppLayout::Folder;
	const std::string icon = app.manifest.icon.empty() ? "icon.png" : app.manifest.icon;
	if (fs.exists(folder + "/" + icon)) app.iconPath = folder + "/" + icon;
	std::string signature;
	if (fs.exists(folder + "/" + kSignatureEntry) && fs.readFile(folder + "/" + kSignatureEntry, signature, kMaxSignatureBytes))
		labelTrust(app, signature);
	found.push_back(std::move(app));
}

void scanPackage(Fs &fs, const Volume &volume, const std::string &path, const std::string &name, std::string_view targetKey,
                 std::vector<InstalledApp> &found, std::vector<BrokenApp> &broken)
{
	const auto fail = [&](std::string reason) { broken.push_back({volume.id, path, std::move(reason)}); };
	const std::string stem = name.substr(0, name.size() - kPackageSuffix.size());
	if (!isValidAppId(stem)) return fail("file name is not a valid app id");
	PackageReader reader;
	std::string error;
	if (!reader.open(fs.open(path), error)) return fail(error);
	const PackageEntry *manifestEntry = reader.find("manifest.json");
	if (!manifestEntry) return fail("package has no manifest.json");
	if (manifestEntry->size > kMaxManifestBytes) return fail("manifest.json is larger than " + std::to_string(kMaxManifestBytes) + " bytes");
	std::string text;
	if (!reader.read(*manifestEntry, text, error)) return fail(error);
	InstalledApp app;
	if (!parseManifest(text, app.manifest, error, stem)) return fail(error);
	if (!reader.find(programEntryName(targetKey))) return fail("no program for " + std::string(targetKey));
	app.volumeId = volume.id;
	app.location = path;
	app.layout = AppLayout::Package;
	if (const PackageEntry *entry = reader.find(kSignatureEntry)) {
		std::string signature;
		if (entry->size <= kMaxSignatureBytes && reader.read(*entry, signature, error)) labelTrust(app, signature);
		else app.trust = Trust::Unverified;
	}
	found.push_back(std::move(app));
}

}  // namespace

std::string appsDirectory(const Volume &volume)
{
	if (volume.root.empty() || volume.root == "/") return "/apps";
	if (volume.root.back() == '/') return volume.root + "apps";
	return volume.root + "/apps";
}

std::string folderProgramPath(Fs &fs, const std::string &folder, std::string_view targetKey)
{
	const std::string specific = folder + "/" + programEntryName(targetKey);
	if (!targetKey.empty() && fs.exists(specific)) return specific;
	const std::string legacy = folder + "/" + kLegacyProgram;
	if (fs.exists(legacy)) return legacy;
	return {};
}

ScanResult scan(const std::vector<Volume> &volumes, Fs &fs, std::string_view targetKey)
{
	std::vector<InstalledApp> found;
	ScanResult result;
	std::vector<std::string> seenRoots;
	for (const Volume &volume : volumes) {
		const std::string dir = appsDirectory(volume);
		// Two volumes at one root (a board that declares a path twice) would
		// list every app twice; scan it once.
		if (std::find(seenRoots.begin(), seenRoots.end(), dir) != seenRoots.end()) continue;
		seenRoots.push_back(dir);
		std::vector<DirEntry> entries;
		if (!fs.listDir(dir, entries)) continue;
		std::sort(entries.begin(), entries.end(), [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
		for (const DirEntry &entry : entries) {
			// Hidden: the installer's staging files (.<id>.geapp.partial /
			// .ready), the icon cache, macOS ._ forks.
			if (entry.name.empty() || entry.name[0] == '.') continue;
			const std::string path = dir + "/" + entry.name;
			if (entry.directory) {
				scanFolder(fs, volume, path, entry.name, targetKey, found, result.broken);
			} else if (endsWith(entry.name, kPackageSuffix)) {
				scanPackage(fs, volume, path, entry.name, targetKey, found, result.broken);
			}
			// Any other file in apps/ is not an app and is ignored.
		}
	}

	// One winner per id: the highest version, then the earliest volume, then
	// a folder over a package on the same volume (found order breaks ties).
	std::map<std::string, std::size_t> winner;
	for (std::size_t i = 0; i < found.size(); i++) {
		auto it = winner.find(found[i].manifest.id);
		if (it == winner.end()) {
			winner.emplace(found[i].manifest.id, i);
			continue;
		}
		if (found[i].manifest.version.compare(found[it->second].manifest.version) > 0) it->second = i;
	}
	for (std::size_t i = 0; i < found.size(); i++) {
		if (winner[found[i].manifest.id] == i) result.apps.push_back(found[i]);
		else result.shadowed.push_back(found[i]);
	}
	std::stable_sort(result.apps.begin(), result.apps.end(), [](const InstalledApp &a, const InstalledApp &b) {
		if (a.manifest.name != b.manifest.name) return a.manifest.name < b.manifest.name;
		return a.manifest.id < b.manifest.id;
	});
	return result;
}

bool checkCompatible(const Manifest &manifest, std::string_view firmwareAbi, std::string &error)
{
	if (manifest.abi.empty()) {
		error = manifest.name + " does not say which firmware it was built for; rebuild it";
		return false;
	}
	if (manifest.abi != firmwareAbi) {
		error = manifest.name + " was built for firmware ABI " + manifest.abi + ", this firmware is " + std::string(firmwareAbi) +
		        "; rebuild it";
		return false;
	}
	return true;
}

}  // namespace gea::geaos
