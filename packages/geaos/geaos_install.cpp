// SPDX-License-Identifier: Apache-2.0
// Installing, verifying and removing apps on a volume (see geaos.h for the
// staging and commit protocol).
#include "geaos.h"
#include "geaos_crypto.h"

#include <cstring>

#include <algorithm>
#include <tuple>
#include <vector>

namespace gea::geaos {

namespace {

constexpr std::size_t kChunk = 16 * 1024;
constexpr std::string_view kPartial = ".geapp.partial";
constexpr std::string_view kReady = ".geapp.ready";

bool endsWith(std::string_view text, std::string_view suffix)
{
	return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string volumeName(const Volume &volume)
{
	return volume.label.empty() ? volume.id : volume.label;
}

std::string partialPath(const std::string &apps, const std::string &id)
{
	return apps + "/." + id + std::string(kPartial);
}

std::string readyPath(const std::string &apps, const std::string &id)
{
	return apps + "/." + id + std::string(kReady);
}

// Streams every entry and checks its CRC, without holding a whole program in
// memory.
// With `digests`, also hashes each entry (SHA-512) in the same pass, for the
// signature check.
using Digests = std::vector<std::tuple<std::string, std::uint64_t, std::string>>;

bool checkEntries(File &file, const std::vector<PackageEntry> &entries, std::string &error, Digests *digests = nullptr)
{
	std::vector<char> buffer(kChunk);
	for (const PackageEntry &entry : entries) {
		std::uint32_t crc = 0;
		std::uint64_t done = 0;
		crypto::Sha512 hash;
		while (done < entry.size) {
			const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, entry.size - done));
			if (!file.readAt(entry.offset + done, buffer.data(), n)) {
				error = "cannot read \"" + entry.name + "\" from the package";
				return false;
			}
			crc = crc32(buffer.data(), n, crc);
			if (digests) hash.update(buffer.data(), n);
			done += n;
		}
		if (crc != entry.crc32) {
			error = "package entry \"" + entry.name + "\" is corrupt (CRC mismatch)";
			return false;
		}
		if (digests) {
			std::uint8_t digest[64];
			hash.finish(digest);
			digests->emplace_back(entry.name, entry.size, crypto::toHex(digest, sizeof(digest)));
		}
	}
	return true;
}

constexpr std::size_t kMaxSignatureBytes = 4096;

// The package's signature against the trust policy; refuses what must not
// install, and what is not verified when the options do not allow it.
bool checkTrust(PackageReader &reader, const Digests &digests, const Manifest &manifest, const InstallOptions &options,
                SignatureCheck &trust, std::string &error)
{
	trust = SignatureCheck{};
	const TrustPolicy policy = trustPolicy();
	if (const PackageEntry *entry = reader.find(kSignatureEntry)) {
		if (entry->size > kMaxSignatureBytes) {
			error = "signature.json is larger than " + std::to_string(kMaxSignatureBytes) + " bytes";
			return false;
		}
		std::string text;
		if (!reader.read(*entry, text, error)) return false;
		if (!checkSignature(text, signingMessage(digests), policy, trust, error)) {
			error = manifest.name + ": " + error;
			return false;
		}
	}
	if (trust.trust != Trust::Verified && !options.allowUnverified) {
		error = manifest.name + " is from an unverified developer; confirm to install it anyway";
		return false;
	}
	return true;
}

std::string kilobytes(std::uint64_t bytes) { return std::to_string((bytes + 1023) / 1024) + " KB"; }

// Is this package for this device: its program, its ABI, its size.
bool checkForDevice(const PackageReader &reader, const Manifest &manifest, const InstallOptions &options, std::string &error)
{
	if (!options.runningId.empty() && manifest.id == options.runningId) {
		error = manifest.name + " is running; return to Home first";
		return false;
	}
	const PackageEntry *program = options.targetKey.empty() ? nullptr : reader.find(programEntryName(options.targetKey));
	if (!options.targetKey.empty() && !program) {
		error = manifest.name + " is not built for this device (no program for " + options.targetKey + ")";
		return false;
	}
	if (!options.abi.empty() && !checkCompatible(manifest, options.abi, error)) return false;
	if (options.maxProgramBytes && program) {
		std::uint64_t needs = program->size;
		for (const auto &[target, bytes] : manifest.programBytes)
			if (target == options.targetKey && bytes > needs) needs = bytes;
		if (needs > options.maxProgramBytes) {
			error = manifest.name + " needs " + kilobytes(needs) + " of memory; this device runs apps up to " +
			        kilobytes(options.maxProgramBytes);
			return false;
		}
	}
	return true;
}

bool verifyOpen(Fs &fs, const std::string &path, const InstallOptions &options, Manifest &manifest, std::string &error,
                SignatureCheck *trustOut = nullptr)
{
	PackageReader reader;
	if (!reader.open(fs.open(path), error)) {
		if (error.empty()) error = "cannot open the package";
		return false;
	}
	const PackageEntry *entry = reader.find("manifest.json");
	if (!entry) {
		error = "package has no manifest.json";
		return false;
	}
	if (entry->size > kMaxManifestBytes) {
		error = "manifest.json is larger than " + std::to_string(kMaxManifestBytes) + " bytes";
		return false;
	}
	std::string text;
	if (!reader.read(*entry, text, error)) return false;
	if (!parseManifest(text, manifest, error)) return false;
	if (!checkForDevice(reader, manifest, options, error)) return false;
	std::unique_ptr<File> file = fs.open(path);
	if (!file) {
		error = "cannot open the package";
		return false;
	}
	const bool signedPackage = reader.find(kSignatureEntry) != nullptr;
	Digests digests;
	if (!checkEntries(*file, reader.entries(), error, signedPackage ? &digests : nullptr)) return false;
	SignatureCheck trust;
	if (!checkTrust(reader, digests, manifest, options, trust, error)) return false;
	if (trustOut) *trustOut = trust;
	return true;
}

// Makes <root>/apps; false if the volume cannot hold it.
bool ensureAppsDirectory(Fs &fs, const Volume &volume, std::string &error)
{
	if (volume.readOnly) {
		error = volumeName(volume) + " is read-only";
		return false;
	}
	const std::string apps = appsDirectory(volume);
	if (fs.exists(apps) || fs.makeDir(apps)) return true;
	error = "cannot write to " + volumeName(volume) + " (read-only, full or removed)";
	return false;
}

// The commit: .ready is the new app; the old copies on this volume go, then
// .ready takes the final name. Each step is one a recovery can redo.
bool commitReady(Fs &fs, const std::string &apps, const std::string &id, InstallResult &result)
{
	const std::string ready = readyPath(apps, id);
	const std::string target = apps + "/" + id + ".geapp";
	const std::string folder = apps + "/" + id;
	if (fs.exists(folder) && !fs.removeTree(folder)) {
		result.error = "cannot remove the old copy of " + id;
		return false;
	}
	if (fs.exists(target) && !fs.remove(target)) {
		result.error = "cannot replace the old copy of " + id;
		return false;
	}
	if (!fs.rename(ready, target)) {
		result.error = "cannot move " + id + " into place";
		return false;
	}
	result.ok = true;
	result.id = id;
	result.location = target;
	return true;
}

// Verifies the file at `partial`, renames it to .ready and commits. Removes
// `partial` on a refusal.
InstallResult finishStaged(Fs &fs, const std::string &partial, const Volume &volume, const InstallOptions &options)
{
	InstallResult result;
	Manifest manifest;
	SignatureCheck trust;
	if (!verifyOpen(fs, partial, options, manifest, result.error, &trust)) {
		fs.remove(partial);
		return result;
	}
	const std::string apps = appsDirectory(volume);
	const std::string ready = readyPath(apps, manifest.id);
	if (fs.exists(ready) && !fs.remove(ready)) {
		fs.remove(partial);
		result.error = "cannot replace a stale staged copy of " + manifest.id;
		return result;
	}
	if (!fs.rename(partial, ready)) {
		fs.remove(partial);
		result.error = "cannot stage " + manifest.id + " on " + volumeName(volume);
		return result;
	}
	result.trust = trust.trust;
	result.developer = trust.developer;
	commitReady(fs, apps, manifest.id, result);
	return result;
}

}  // namespace

bool verifyPackage(Fs &fs, const std::string &path, const InstallOptions &options, Manifest &manifest, std::string &error)
{
	return verifyOpen(fs, path, options, manifest, error);
}

InstallResult installPackage(Fs &fs, File &source, const Volume &volume, const InstallOptions &options)
{
	InstallResult result;
	// Read the manifest first: the staging name carries the id, and a
	// package for another device or firmware is refused before a byte is
	// written. Entry CRCs are checked once, on the written copy.
	PackageReader probe;
	struct Borrowed final : File {
		explicit Borrowed(File &f) : f(f) {}
		std::uint64_t size() const override { return f.size(); }
		bool readAt(std::uint64_t offset, void *buffer, std::size_t length) override { return f.readAt(offset, buffer, length); }
		File &f;
	};
	if (!probe.open(std::make_unique<Borrowed>(source), result.error)) return result;
	std::string text;
	Manifest manifest;
	const PackageEntry *entry = probe.find("manifest.json");
	if (!entry) {
		result.error = "package has no manifest.json";
		return result;
	}
	if (entry->size > kMaxManifestBytes) {
		result.error = "manifest.json is larger than " + std::to_string(kMaxManifestBytes) + " bytes";
		return result;
	}
	if (!probe.read(*entry, text, result.error) || !parseManifest(text, manifest, result.error)) return result;
	if (!checkForDevice(probe, manifest, options, result.error)) return result;
	if (!ensureAppsDirectory(fs, volume, result.error)) return result;

	const std::string apps = appsDirectory(volume);
	const std::uint64_t size = source.size();
	if (std::optional<std::uint64_t> free = fs.freeBytes(apps)) {
		if (*free < size + options.reserveBytes) {
			result.error = "not enough space on " + volumeName(volume) + ": " + manifest.name + " needs " +
			               std::to_string((size + 1023) / 1024) + " KB, " + std::to_string(*free / 1024) + " KB free";
			return result;
		}
	}

	const std::string partial = partialPath(apps, manifest.id);
	std::unique_ptr<WritableFile> out = fs.create(partial);
	if (!out) {
		result.error = "cannot write to " + volumeName(volume) + " (read-only, full or removed)";
		return result;
	}
	std::vector<char> buffer(kChunk);
	bool ok = true;
	for (std::uint64_t done = 0; ok && done < size;) {
		const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, size - done));
		if (!source.readAt(done, buffer.data(), n)) {
			result.error = "cannot read the package";
			ok = false;
		} else if (!out->write(buffer.data(), n)) {
			result.error = "writing to " + volumeName(volume) + " failed (full or removed)";
			ok = false;
		}
		done += n;
	}
	if (ok && !out->finish()) {
		result.error = "writing to " + volumeName(volume) + " failed (full or removed)";
		ok = false;
	}
	out.reset();
	if (!ok) {
		fs.remove(partial);
		return result;
	}
	return finishStaged(fs, partial, volume, options);
}

InstallResult installStaged(Fs &fs, const std::string &staged, const Volume &volume, const InstallOptions &options)
{
	InstallResult result;
	const std::string apps = appsDirectory(volume);
	const std::string prefix = apps + "/";
	if (staged.compare(0, prefix.size(), prefix) != 0 || staged.find('/', prefix.size()) != std::string::npos) {
		result.error = "a staged package must be in " + apps;
		return result;
	}
	if (volume.readOnly) {
		result.error = volumeName(volume) + " is read-only";
		return result;
	}
	if (!fs.exists(staged)) {
		result.error = "no staged package at " + staged;
		return result;
	}
	return finishStaged(fs, staged, volume, options);
}

int recoverInstalls(Fs &fs, const Volume &volume)
{
	if (volume.readOnly) return 0;
	const std::string apps = appsDirectory(volume);
	std::vector<DirEntry> entries;
	if (!fs.listDir(apps, entries)) return 0;
	int handled = 0;
	for (const DirEntry &entry : entries) {
		if (entry.directory || entry.name.size() < 2 || entry.name[0] != '.') continue;
		const std::string path = apps + "/" + entry.name;
		if (endsWith(entry.name, kPartial)) {
			// Never verified: whatever it holds is not an app yet.
			fs.remove(path);
			handled++;
		} else if (endsWith(entry.name, kReady)) {
			const std::string id = entry.name.substr(1, entry.name.size() - 1 - kReady.size());
			// A .ready was verified before the rename that made it: finish
			// the commit. A name that is not an app id was not ours.
			if (!isValidAppId(id)) continue;
			InstallResult result;
			if (!commitReady(fs, apps, id, result)) continue;
			handled++;
		}
	}
	return handled;
}

namespace {

// A folder app's files as one package, for moveApp.
bool packFolder(Fs &fs, const std::string &root, const std::string &relative, PackageWriter &writer, std::uint64_t &budget, std::string &error)
{
	std::vector<DirEntry> entries;
	const std::string dir = relative.empty() ? root : root + "/" + relative;
	if (!fs.listDir(dir, entries)) {
		error = "cannot read " + dir;
		return false;
	}
	for (const DirEntry &entry : entries) {
		if (entry.name.empty() || entry.name[0] == '.') continue;
		const std::string name = relative.empty() ? entry.name : relative + "/" + entry.name;
		if (entry.directory) {
			if (!packFolder(fs, root, name, writer, budget, error)) return false;
			continue;
		}
		std::string data;
		if (entry.size > budget || !fs.readFile(dir + "/" + entry.name, data, static_cast<std::size_t>(budget))) {
			error = "cannot read " + name + " (unreadable or too large)";
			return false;
		}
		budget -= data.size();
		if (!writer.add(name, data)) {
			error = "cannot pack " + name;
			return false;
		}
	}
	return true;
}

struct StringFile final : File {
	explicit StringFile(std::string data) : data(std::move(data)) {}
	std::uint64_t size() const override { return data.size(); }
	bool readAt(std::uint64_t offset, void *buffer, std::size_t length) override
	{
		if (offset > data.size() || length > data.size() - offset) return false;
		std::memcpy(buffer, data.data() + offset, length);
		return true;
	}
	std::string data;
};

std::uint64_t treeBytes(Fs &fs, const std::string &dir)
{
	std::vector<DirEntry> entries;
	if (!fs.listDir(dir, entries)) return 0;
	std::uint64_t total = 0;
	for (const DirEntry &entry : entries) total += entry.directory ? treeBytes(fs, dir + "/" + entry.name) : entry.size;
	return total;
}

}  // namespace

std::uint64_t installedBytes(Fs &fs, const InstalledApp &app)
{
	if (app.location.empty()) return 0;
	if (app.layout == AppLayout::Folder) return treeBytes(fs, app.location);
	auto file = fs.open(app.location);
	return file ? file->size() : 0;
}

InstallResult moveApp(Fs &fs, const InstalledApp &app, const Volume &to, const InstallOptions &options)
{
	InstallResult result;
	if (app.location.empty()) {
		result.error = app.manifest.name + " is part of the system and cannot be moved";
		return result;
	}
	if (app.volumeId == to.id) {
		result.error = app.manifest.name + " is already on " + volumeName(to);
		return result;
	}
	if (to.readOnly) {
		result.error = volumeName(to) + " is read-only";
		return result;
	}
	std::unique_ptr<File> source;
	if (app.layout == AppLayout::Package) {
		source = fs.open(app.location);
		if (!source) {
			result.error = "cannot read " + app.manifest.name + " (its volume may have been removed)";
			return result;
		}
	} else {
		PackageWriter writer;
		std::uint64_t budget = 256ull * 1024 * 1024;
		if (!packFolder(fs, app.location, "", writer, budget, result.error)) return result;
		source = std::make_unique<StringFile>(writer.finish());
	}
	// The copy is committed (and verified) on the new volume before the
	// original goes: a failure anywhere leaves the app where it was.
	result = installPackage(fs, *source, to, options);
	source.reset();
	if (!result.ok) return result;
	std::string error;
	if (!uninstallApp(fs, app, error)) {
		// Both copies exist; the scan picks one by volume order. Say so.
		result.error = "moved, but the old copy stayed: " + error;
	}
	return result;
}

bool uninstallApp(Fs &fs, const InstalledApp &app, std::string &error)
{
	if (app.location.empty()) {
		error = app.manifest.name + " is part of the system and cannot be removed";
		return false;
	}
	const bool ok = app.layout == AppLayout::Folder ? fs.removeTree(app.location) : (fs.remove(app.location) || !fs.exists(app.location));
	if (!ok) error = "cannot remove " + app.manifest.name + " (read-only or removed volume)";
	return ok;
}

}  // namespace gea::geaos
