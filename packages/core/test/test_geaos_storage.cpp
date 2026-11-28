// SPDX-License-Identifier: Apache-2.0
// GeaOS storage: installing, uninstalling and recovering apps on volumes that
// fill up, go read-only, or are pulled out at any moment; volume arrival and
// removal; what the running app hears when its card goes. Built with
// ASan/UBSan and TSan by run-geaos-storage.sh.
#include "geaos.h"
#include "geaos_crypto.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace gea::geaos;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
	do {                                                                               \
		g_checks++;                                                                    \
		if (!(cond)) {                                                                 \
			g_failures++;                                                              \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                              \
	} while (0)

#define CHECK_CONTAINS(text, needle)                                                                      \
	do {                                                                                                  \
		g_checks++;                                                                                       \
		if (std::string(text).find(needle) == std::string::npos) {                                        \
			g_failures++;                                                                                 \
			std::fprintf(stderr, "%s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, __LINE__,             \
			             std::string(text).c_str(), needle);                                              \
		}                                                                                                 \
	} while (0)

// ---- A card: an in-memory filesystem that can fill, lock or vanish -------

class Card;

class CardFile final : public File {
public:
	CardFile(Card &card, std::string data) : card_(card), data_(std::move(data)) {}
	std::uint64_t size() const override { return data_.size(); }
	bool readAt(std::uint64_t offset, void *buffer, std::size_t length) override;

private:
	Card &card_;
	std::string data_;
};

class Card final : public Fs {
public:
	std::map<std::string, std::string> files;
	std::set<std::string> dirs;
	bool readOnly = false;  // write-protect switch
	bool gone = false;      // pulled out: every operation fails
	// Bytes the card can still take (-1: unlimited). A write past it fails.
	long long capacity = -1;
	// Report free space (a FAT volume can; some cannot).
	bool reportsFree = true;
	// Pull the card after this many more mutating operations (-1: never).
	int pullAfter = -1;
	int mutations = 0;

	void put(const std::string &path, const std::string &data)
	{
		files[path] = data;
		for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
			dirs.insert(path.substr(0, slash));
	}

	// Counts a mutating operation; false once the card is (or just got) pulled.
	bool step()
	{
		if (gone) return false;
		mutations++;
		if (pullAfter >= 0 && pullAfter-- == 0) {
			gone = true;
			return false;
		}
		return true;
	}
	long long used() const
	{
		long long total = 0;
		for (const auto &f : files) total += static_cast<long long>(f.second.size());
		return total;
	}

	bool listDir(const std::string &path, std::vector<DirEntry> &out) override
	{
		out.clear();
		if (gone || !dirs.count(path)) return false;
		std::set<std::string> seen;
		const std::string prefix = path + "/";
		const auto consider = [&](const std::string &full, bool isFile, std::uint64_t size) {
			if (full.compare(0, prefix.size(), prefix) != 0) return;
			const std::string rest = full.substr(prefix.size());
			const std::size_t slash = rest.find('/');
			const std::string name = rest.substr(0, slash);
			if (name.empty() || !seen.insert(name).second) return;
			out.push_back({name, slash != std::string::npos || !isFile, isFile && slash == std::string::npos ? size : 0});
		};
		for (const auto &f : files) consider(f.first, true, f.second.size());
		for (const auto &d : dirs) consider(d, false, 0);
		return true;
	}
	bool exists(const std::string &path) override { return !gone && (files.count(path) || dirs.count(path)); }
	std::unique_ptr<File> open(const std::string &path) override
	{
		if (gone) return nullptr;
		auto it = files.find(path);
		if (it == files.end()) return nullptr;
		return std::make_unique<CardFile>(*this, it->second);
	}

	class Writer final : public WritableFile {
	public:
		Writer(Card &card, std::string path) : card_(card), path_(std::move(path)) {}
		bool write(const void *data, std::size_t length) override
		{
			if (failed_ || !card_.step() || !card_.files.count(path_)) return failed_ = true, false;
			if (card_.capacity >= 0 && card_.used() + static_cast<long long>(length) > card_.capacity) {
				// A full FAT volume takes what fits, then refuses.
				const long long room = std::max(0LL, card_.capacity - card_.used());
				card_.files[path_].append(static_cast<const char *>(data), static_cast<std::size_t>(room));
				return failed_ = true, false;
			}
			card_.files[path_].append(static_cast<const char *>(data), length);
			return true;
		}
		bool finish() override { return !failed_ && card_.step() && card_.files.count(path_); }

	private:
		Card &card_;
		std::string path_;
		bool failed_ = false;
	};

	std::unique_ptr<WritableFile> create(const std::string &path) override
	{
		if (readOnly || !step()) return nullptr;
		const std::string parent = path.substr(0, path.rfind('/'));
		if (!dirs.count(parent)) return nullptr;
		files[path].clear();
		return std::make_unique<Writer>(*this, path);
	}
	bool makeDir(const std::string &path) override
	{
		if (dirs.count(path) && !gone) return true;
		if (readOnly || !step()) return false;
		dirs.insert(path);
		return true;
	}
	bool rename(const std::string &from, const std::string &to) override
	{
		if (readOnly || !step()) return false;
		auto it = files.find(from);
		if (it == files.end() || files.count(to) || dirs.count(to)) return false;  // FAT: no replacing rename
		files[to] = std::move(it->second);
		files.erase(from);
		return true;
	}
	bool remove(const std::string &path) override
	{
		if (readOnly || !step()) return false;
		if (files.erase(path)) return true;
		if (!dirs.count(path)) return false;
		const std::string prefix = path + "/";
		for (const auto &f : files)
			if (f.first.compare(0, prefix.size(), prefix) == 0) return false;  // not empty
		for (const auto &d : dirs)
			if (d.compare(0, prefix.size(), prefix) == 0) return false;
		dirs.erase(path);
		return true;
	}
	std::optional<std::uint64_t> freeBytes(const std::string &) override
	{
		if (gone || !reportsFree || capacity < 0) return std::nullopt;
		return static_cast<std::uint64_t>(std::max(0LL, capacity - used()));
	}

	// Paths under `dir` whose name starts with '.'.
	std::vector<std::string> hiddenUnder(const std::string &dir) const
	{
		std::vector<std::string> out;
		const std::string prefix = dir + "/.";
		for (const auto &f : files)
			if (f.first.compare(0, prefix.size(), prefix) == 0) out.push_back(f.first);
		return out;
	}
};

bool CardFile::readAt(std::uint64_t offset, void *buffer, std::size_t length)
{
	if (card_.gone || offset + length > data_.size()) return false;
	std::memcpy(buffer, data_.data() + offset, length);
	return true;
}

class Bytes final : public File {
public:
	explicit Bytes(std::string data) : data_(std::move(data)) {}
	std::uint64_t size() const override { return data_.size(); }
	bool readAt(std::uint64_t offset, void *buffer, std::size_t length) override
	{
		if (offset + length > data_.size()) return false;
		std::memcpy(buffer, data_.data() + offset, length);
		return true;
	}

private:
	std::string data_;
};

constexpr const char *kTarget = "esp32-riscv32";
constexpr const char *kAbi = "abi1";

std::string manifestJson(const std::string &id, const std::string &version = "1.0.0", const std::string &abi = kAbi,
                         const std::string &extra = "")
{
	return "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"version\":\"" + version + "\",\"abi\":\"" + abi + "\"" + extra + "}";
}

std::string package(const std::string &id, const std::string &version = "1.0.0", const std::string &abi = kAbi,
                    const std::string &target = kTarget, std::size_t programBytes = 40000)
{
	PackageWriter writer;
	writer.add("manifest.json", manifestJson(id, version, abi));
	std::string program(programBytes, '\0');
	for (std::size_t i = 0; i < program.size(); i++) program[i] = static_cast<char>((i * 31 + id.size() + version.size()) & 0xFF);
	writer.add(programEntryName(target), program);
	writer.add("icon.png", "PNG" + id);
	return writer.finish();
}

InstallOptions options()
{
	InstallOptions o;
	o.targetKey = kTarget;
	o.abi = kAbi;
	o.reserveBytes = 0;
	return o;
}

Volume card(const std::string &id = "sdcard", const std::string &root = "/sdcard")
{
	Volume v;
	v.id = id;
	v.root = root;
	v.removable = true;
	v.installTarget = true;
	v.label = id == "sdcard" ? "SD card" : id;
	return v;
}

void putFolderApp(Card &fs, const std::string &root, const std::string &id, const std::string &version = "1.0.0")
{
	fs.put(root + "/apps/" + id + "/manifest.json", manifestJson(id, version));
	fs.put(root + "/apps/" + id + "/program/" + kTarget, "ELF");
}

// The one app `id` the registry sees on `volume`, if it is whole.
std::optional<InstalledApp> scanned(Card &fs, const Volume &volume, const std::string &id, ScanResult *all = nullptr)
{
	ScanResult result = scan({volume}, fs, kTarget);
	if (all) *all = result;
	for (const InstalledApp &app : result.apps)
		if (app.manifest.id == id) return app;
	return std::nullopt;
}

// ---- Install ---------------------------------------------------------------

void testInstall()
{
	const Volume sd = card();
	{
		Card fs;
		fs.dirs.insert("/sdcard");
		Bytes source(package("ember"));
		InstallResult r = installPackage(fs, source, sd, options());
		CHECK(r.ok);
		CHECK(r.error.empty());
		CHECK(r.id == "ember");
		CHECK(r.location == "/sdcard/apps/ember.geapp");
		CHECK(fs.files["/sdcard/apps/ember.geapp"] == package("ember"));
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
		std::optional<InstalledApp> app = scanned(fs, sd, "ember");
		CHECK(app && app->layout == AppLayout::Package);
	}
	// An update replaces the old package, and a folder copy of the same app
	// on the same volume (installed over USB before) goes with it.
	{
		Card fs;
		putFolderApp(fs, "/sdcard", "ember");
		fs.put("/sdcard/apps/ember/.hidden", "x");
		fs.put("/sdcard/apps/ember/assets/a/b.bin", "deep");
		Bytes v2(package("ember", "2.0.0"));
		CHECK(installPackage(fs, v2, sd, options()).ok);
		CHECK(!fs.exists("/sdcard/apps/ember"));
		CHECK(!fs.exists("/sdcard/apps/ember/assets/a/b.bin"));
		Bytes v3(package("ember", "3.0.0"));
		CHECK(installPackage(fs, v3, sd, options()).ok);
		std::optional<InstalledApp> app = scanned(fs, sd, "ember");
		CHECK(app && app->manifest.version.toString() == "3.0.0");
		// A downgrade is an explicit choice: allowed.
		Bytes v1(package("ember", "1.0.0"));
		CHECK(installPackage(fs, v1, sd, options()).ok);
		app = scanned(fs, sd, "ember");
		CHECK(app && app->manifest.version.toString() == "1.0.0");
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
	}
	// The apps folder is made on a blank card.
	{
		Card fs;
		fs.dirs.insert("/sdcard");
		Bytes source(package("neon"));
		CHECK(installPackage(fs, source, sd, options()).ok);
		CHECK(fs.dirs.count("/sdcard/apps"));
	}
}

// The device's program-size limit: a program exactly at it installs, one
// byte over is refused with both sizes; the manifest's measured size (the
// loaded image, larger than the file) counts when it is the larger.
void testProgramSizeLimit()
{
	const Volume sd = card();
	InstallOptions limited = options();
	limited.maxProgramBytes = 40000;
	{
		Card fs;
		Bytes source(package("ember", "1.0.0", kAbi, kTarget, 40000));
		CHECK(installPackage(fs, source, sd, limited).ok);
	}
	{
		Card fs;
		Bytes source(package("ember", "1.0.0", kAbi, kTarget, 40001));
		InstallResult r = installPackage(fs, source, sd, limited);
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "ember needs 40 KB of memory; this device runs apps up to 40 KB");
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
	}
	{
		// A 30000-byte file whose image needs 50000 once loaded (zeroed data).
		Card fs;
		PackageWriter writer;
		writer.add("manifest.json", manifestJson("ember", "1.0.0", kAbi, ",\"programBytes\":{\"" + std::string(kTarget) + "\":50000,\"other\":1}"));
		writer.add(programEntryName(kTarget), std::string(30000, 'x'));
		const std::string bytes = writer.finish();
		Bytes source(bytes);
		InstallResult r = installPackage(fs, source, sd, limited);
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "needs 49 KB");
		InstallOptions roomy = limited;
		roomy.maxProgramBytes = 50000;
		Bytes again(bytes);
		CHECK(installPackage(fs, again, sd, roomy).ok);
	}
	{
		// No limit: anything goes.
		Card fs;
		Bytes source(package("ember", "1.0.0", kAbi, kTarget, 90000));
		CHECK(installPackage(fs, source, sd, options()).ok);
	}
	// The manifest round-trips its sizes; a bad one is refused.
	Manifest m;
	std::string error;
	CHECK(parseManifest(manifestJson("a", "1.0.0", kAbi, ",\"programBytes\":{\"esp32-riscv32\":1234}"), m, error));
	CHECK(m.programBytes.size() == 1 && m.programBytes[0].first == "esp32-riscv32" && m.programBytes[0].second == 1234);
	Manifest back;
	CHECK(parseManifest(writeManifest(m), back, error) && back.programBytes == m.programBytes);
	CHECK(!parseManifest(manifestJson("a", "1.0.0", kAbi, ",\"programBytes\":{\"x\":1.5}"), m, error));
	CHECK_CONTAINS(error, "whole number of bytes");
	CHECK(!parseManifest(manifestJson("a", "1.0.0", kAbi, ",\"programBytes\":[1]"), m, error));
	CHECK(!parseManifest(manifestJson("a", "1.0.0", kAbi, ",\"programBytes\":{\"x\":-1}"), m, error));
}

void testInstallRefusals()
{
	const Volume sd = card();
	const auto fresh = [] {
		Card fs;
		putFolderApp(fs, "/sdcard", "ember");
		return fs;
	};
	const auto untouched = [&](Card &fs, const char *why) {
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
		CHECK(scanned(fs, sd, "ember").has_value());
		if (!fs.hiddenUnder("/sdcard/apps").empty()) std::fprintf(stderr, "  leftovers after: %s\n", why);
	};
	{
		Card fs = fresh();
		Bytes source(package("ember", "2.0.0", kAbi, "xtensa-esp32s3"));
		InstallResult r = installPackage(fs, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "not built for this device");
		CHECK(fs.mutations == 0);  // refused before writing anything
		untouched(fs, "wrong target");
	}
	{
		Card fs = fresh();
		Bytes source(package("ember", "2.0.0", "abi2"));
		InstallResult r = installPackage(fs, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "firmware ABI abi2");
		CHECK(fs.mutations == 0);
		untouched(fs, "wrong abi");
	}
	// Corrupt program bytes: the index is fine, the entry CRC is not. Found
	// on the written copy, which is then removed.
	{
		Card fs = fresh();
		std::string bytes = package("ember", "2.0.0");
		bytes[bytes.size() - 100] ^= 0x5a;
		Bytes source(bytes);
		InstallResult r = installPackage(fs, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "corrupt");
		untouched(fs, "corrupt entry");
		CHECK(scanned(fs, sd, "ember")->manifest.version.toString() == "1.0.0");
	}
	// Not a package at all; a truncated one.
	{
		Card fs = fresh();
		Bytes junk("this is not a package");
		CHECK(!installPackage(fs, junk, sd, options()).ok);
		const std::string whole = package("ember", "2.0.0");
		Bytes truncated(whole.substr(0, whole.size() / 2));
		CHECK(!installPackage(fs, truncated, sd, options()).ok);
		CHECK(fs.mutations == 0);
		untouched(fs, "junk");
	}
	// Full card, known in advance: refused with the sizes, nothing written.
	{
		Card fs = fresh();
		fs.capacity = fs.used() + 1000;
		Bytes source(package("ember", "2.0.0"));
		InstallResult r = installPackage(fs, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "not enough space on SD card");
		CHECK_CONTAINS(r.error, "KB free");
		CHECK(fs.mutations == 0);
		untouched(fs, "full, reported");
	}
	// Full card the filesystem does not report: the write fails part way and
	// the partial copy is removed.
	{
		Card fs = fresh();
		fs.capacity = fs.used() + 20000;
		fs.reportsFree = false;
		Bytes source(package("ember", "2.0.0"));
		InstallResult r = installPackage(fs, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "failed (full or removed)");
		untouched(fs, "full, unreported");
	}
	// The reserve is honoured.
	{
		Card fs = fresh();
		const std::string bytes = package("ember", "2.0.0");
		fs.capacity = fs.used() + static_cast<long long>(bytes.size()) + 100;
		InstallOptions o = options();
		o.reserveBytes = 4096;
		Bytes source(bytes);
		CHECK(!installPackage(fs, source, sd, o).ok);
		o.reserveBytes = 0;
		CHECK(installPackage(fs, source, sd, o).ok);
	}
	// Write-protected card (the volume says so) and a card that refuses
	// writes without saying (the filesystem does).
	{
		Card fs = fresh();
		Volume locked = sd;
		locked.readOnly = true;
		Bytes source(package("ember", "2.0.0"));
		InstallResult r = installPackage(fs, source, locked, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "SD card is read-only");
		Card ro = fresh();
		ro.readOnly = true;
		r = installPackage(ro, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "cannot write to SD card");
		untouched(ro, "read-only");
		Card blank;
		blank.dirs.insert("/sdcard");
		blank.readOnly = true;
		r = installPackage(blank, source, sd, options());
		CHECK(!r.ok);
		CHECK_CONTAINS(r.error, "cannot write to SD card");
	}
}

// The card is pulled (or the power cut) after every possible number of
// operations. Whatever step that hits, once the card is back and recovered
// the volume holds the old app or the new one -- whole, never broken -- and
// no staging file.
void testPulledMidInstall()
{
	const Volume sd = card();
	const std::string v2 = package("ember", "2.0.0");
	int sawOld = 0, sawNew = 0;
	for (int k = 0; k < 200; k++) {
		Card fs;
		putFolderApp(fs, "/sdcard", "ember");
		fs.put("/sdcard/apps/ember/assets/big.bin", std::string(5000, 'a'));
		fs.pullAfter = k;
		Bytes source(v2);
		InstallResult r = installPackage(fs, source, sd, options());
		const bool pulled = fs.gone;
		if (!pulled) {
			CHECK(r.ok);
			CHECK(sawOld > 0);  // the loop did cut installs short before
			break;
		}
		CHECK(!r.ok);
		fs.gone = false;
		fs.pullAfter = -1;
		recoverInstalls(fs, sd);
		ScanResult all;
		std::optional<InstalledApp> app = scanned(fs, sd, "ember", &all);
		CHECK(app.has_value());
		CHECK(all.broken.empty());
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
		if (app) (app->manifest.version.toString() == "2.0.0" ? sawNew : sawOld)++;
		// A .ready was committed: the new one is whole.
		if (app && app->manifest.version.toString() == "2.0.0") CHECK(fs.files["/sdcard/apps/ember.geapp"] == v2);
	}
	CHECK(sawOld > 0);
	CHECK(sawNew > 0);  // some cuts fell after the commit point
}

// What an interrupted install leaves, found on the next mount.
void testRecover()
{
	const Volume sd = card();
	{
		Card fs;
		putFolderApp(fs, "/sdcard", "ember");
		fs.put("/sdcard/apps/.ember.geapp.partial", "half of a package");
		fs.put("/sdcard/apps/.neon.geapp.ready", package("neon"));
		fs.put("/sdcard/apps/neon/manifest.json", manifestJson("neon", "0.9.0"));
		fs.put("/sdcard/apps/.icons/neon-1.0.0.png", "PNG");  // the host's icon cache: not ours
		fs.put("/sdcard/apps/.Bad Name.geapp.ready", "x");      // not an app id: left alone
		CHECK(recoverInstalls(fs, sd) == 2);
		CHECK(!fs.exists("/sdcard/apps/.ember.geapp.partial"));
		CHECK(!fs.exists("/sdcard/apps/.neon.geapp.ready"));
		CHECK(!fs.exists("/sdcard/apps/neon"));
		CHECK(fs.files["/sdcard/apps/neon.geapp"] == package("neon"));
		CHECK(fs.exists("/sdcard/apps/.icons/neon-1.0.0.png"));
		CHECK(fs.exists("/sdcard/apps/.Bad Name.geapp.ready"));
		CHECK(recoverInstalls(fs, sd) == 0);  // idempotent
		ScanResult all;
		CHECK(scanned(fs, sd, "neon", &all).has_value());
		CHECK(scanned(fs, sd, "ember").has_value());
		CHECK(all.broken.empty());
	}
	// Read-only, missing apps folder: nothing to do, nothing touched.
	{
		Card fs;
		fs.put("/sdcard/apps/.ember.geapp.partial", "x");
		Volume locked = sd;
		locked.readOnly = true;
		CHECK(recoverInstalls(fs, locked) == 0);
		CHECK(fs.exists("/sdcard/apps/.ember.geapp.partial"));
		Card empty;
		CHECK(recoverInstalls(empty, sd) == 0);
	}
	// A half-copied app the installer did not write (dragged onto the card
	// on a computer, the copy cut short) shows as broken, with the reason,
	// and never as an app.
	{
		Card fs;
		const std::string whole = package("wander");
		fs.put("/sdcard/apps/wander.geapp", whole.substr(0, whole.size() - 5000));
		fs.put("/sdcard/apps/sonora/manifest.json", manifestJson("sonora"));  // program not copied yet
		fs.put("/sdcard/apps/nimbus/program/esp32-riscv32", "ELF");            // manifest not copied yet
		ScanResult all;
		CHECK(!scanned(fs, sd, "wander", &all));
		CHECK(all.apps.empty());
		CHECK(all.broken.size() == 3);
	}
}

void testInstallStaged()
{
	const Volume sd = card();
	{
		Card fs;
		putFolderApp(fs, "/sdcard", "ember");
		fs.put("/sdcard/apps/.ember.upload", package("ember", "2.0.0"));
		InstallResult r = installStaged(fs, "/sdcard/apps/.ember.upload", sd, options());
		CHECK(r.ok);
		CHECK(!fs.exists("/sdcard/apps/.ember.upload"));
		CHECK(scanned(fs, sd, "ember")->manifest.version.toString() == "2.0.0");
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
	}
	// A corrupt upload is refused and removed; the installed app stays.
	{
		Card fs;
		putFolderApp(fs, "/sdcard", "ember");
		std::string bytes = package("ember", "2.0.0");
		bytes[bytes.size() - 10] ^= 1;
		fs.put("/sdcard/apps/.ember.upload", bytes);
		InstallResult r = installStaged(fs, "/sdcard/apps/.ember.upload", sd, options());
		CHECK(!r.ok);
		CHECK(!fs.exists("/sdcard/apps/.ember.upload"));
		CHECK(scanned(fs, sd, "ember")->manifest.version.toString() == "1.0.0");
	}
	// Only from the volume's own apps folder (a rename cannot cross volumes,
	// and nothing elsewhere is the installer's to consume).
	{
		Card fs;
		fs.put("/sdcard/apps/.x", package("ember"));
		fs.put("/sdcard/other/ember.geapp", package("ember"));
		fs.put("/nand/apps/.x", package("ember"));
		CHECK_CONTAINS(installStaged(fs, "/sdcard/other/ember.geapp", sd, options()).error, "must be in /sdcard/apps");
		CHECK_CONTAINS(installStaged(fs, "/nand/apps/.x", sd, options()).error, "must be in /sdcard/apps");
		CHECK_CONTAINS(installStaged(fs, "/sdcard/apps/sub/.x", sd, options()).error, "must be in /sdcard/apps");
		CHECK_CONTAINS(installStaged(fs, "/sdcard/apps/.missing", sd, options()).error, "no staged package");
		CHECK(fs.exists("/sdcard/other/ember.geapp"));
	}
}

void testUninstall()
{
	const Volume sd = card();
	Card fs;
	putFolderApp(fs, "/sdcard", "ember");
	fs.put("/sdcard/apps/ember/.DS_Store", "x");
	fs.put("/sdcard/apps/neon.geapp", package("neon"));
	std::string error;
	std::optional<InstalledApp> ember = scanned(fs, sd, "ember");
	std::optional<InstalledApp> neon = scanned(fs, sd, "neon");
	CHECK(ember && neon);
	fs.readOnly = true;
	CHECK(!uninstallApp(fs, *ember, error));
	CHECK_CONTAINS(error, "cannot remove ember");
	fs.readOnly = false;
	CHECK(uninstallApp(fs, *ember, error));
	CHECK(!fs.exists("/sdcard/apps/ember"));
	CHECK(uninstallApp(fs, *neon, error));
	CHECK(!fs.exists("/sdcard/apps/neon.geapp"));
	CHECK(uninstallApp(fs, *neon, error));  // already gone: done
	InstalledApp bundled;
	bundled.manifest.name = "Settings";
	CHECK(!uninstallApp(fs, bundled, error));
	CHECK_CONTAINS(error, "part of the system");
}

// ---- The launcher and the monitor --------------------------------------------

class Host final : public AppHost {
public:
	std::string running;
	bool backgroundLoad = false;  // launch() returns before the app runs
	std::string pending;
	std::vector<std::string> events;
	const char *targetKey() const override { return kTarget; }
	const char *abi() const override { return kAbi; }
	bool launch(const InstalledApp &app, std::string &) override
	{
		if (backgroundLoad) pending = app.manifest.id;
		else running = app.manifest.id;
		return true;
	}
	bool home(std::string &error) override
	{
		if (running.empty()) {
			error = "already home";
			return false;
		}
		running.clear();
		return true;
	}
	const char *runningAppId() const override { return running.c_str(); }
	void volumeLost(const InstalledApp &app) override { events.push_back("lost " + app.manifest.id + "@" + app.volumeId); }
	void volumeRestored(const InstalledApp &app) override { events.push_back("restored " + app.manifest.id + "@" + app.volumeId); }
};

class Slot final : public VolumeSource {
public:
	Slot(Card &fs, Volume volume) : fs_(fs), volume_(std::move(volume)) {}
	bool inserted = false;
	bool formatted = true;
	int probes = 0;
	int losses = 0;
	Volume volume() const override { return volume_; }
	bool probe() override
	{
		probes++;
		// An unformatted card is in the slot but never mounts.
		return inserted && formatted && !fs_.gone;
	}
	void lost() override { losses++; }

private:
	Card &fs_;
	Volume volume_;
};

void testMonitor()
{
	Card fs;
	Launcher::reset(&fs);
	Host host;
	Launcher::setHost(&host);
	Volume nandVolume;
	nandVolume.id = "nand";
	nandVolume.root = "/nand";
	nandVolume.installTarget = true;
	putFolderApp(fs, "/nand", "ember");
	putFolderApp(fs, "/nand", "neon", "1.0.0");
	FixedVolumeSource nand(nandVolume, fs);
	Slot slot(fs, card());
	VolumeMonitor monitor;
	monitor.add(&nand);
	monitor.add(&slot);
	monitor.add(&slot);  // added once

	CHECK(monitor.poll());
	CHECK(Launcher::volumes().size() == 1);
	CHECK(Launcher::apps().apps.size() == 2);
	const std::uint32_t g1 = Launcher::generation();
	// Nothing changed: no rescan reported, no generation step (no UI churn).
	CHECK(!monitor.poll());
	CHECK(!monitor.poll());
	CHECK(Launcher::generation() == g1);

	// An unformatted card: in the slot, never a volume.
	slot.inserted = true;
	slot.formatted = false;
	CHECK(!monitor.poll());
	CHECK(Launcher::volumes().size() == 1);

	// A card with neon 2.0 and its own app, plus an interrupted install.
	putFolderApp(fs, "/sdcard", "neon", "2.0.0");
	fs.put("/sdcard/apps/wander.geapp", package("wander"));
	fs.put("/sdcard/apps/.sonora.geapp.partial", "half");
	slot.formatted = true;
	CHECK(monitor.poll());
	CHECK(Launcher::volumes().size() == 2);
	CHECK(!fs.exists("/sdcard/apps/.sonora.geapp.partial"));  // recovered on mount
	ScanResult apps = Launcher::apps();
	CHECK(apps.apps.size() == 3);
	CHECK(Launcher::find("neon")->volumeId == "sdcard");  // higher version wins
	CHECK(apps.shadowed.size() == 1);
	CHECK(Launcher::generation() == g1 + 1);

	// A file copied onto a mounted volume shows up on the next poll -- not
	// on a poll that only watches for volumes (an app is running).
	putFolderApp(fs, "/nand", "nimbus");
	CHECK(!monitor.poll(false));
	CHECK(!Launcher::find("nimbus"));
	CHECK(monitor.poll());
	CHECK(Launcher::find("nimbus").has_value());

	// Launch the card's app, then pull the card: the host hears it once,
	// the app keeps running, and its volume is gone from the list.
	CHECK(Launcher::launch("wander"));
	CHECK(host.running == "wander");
	CHECK(Launcher::launchedApp() && Launcher::launchedApp()->volumeId == "sdcard");
	slot.inserted = false;
	CHECK(monitor.poll(false));  // a volume moved: rescanned even so
	CHECK(slot.losses == 1);
	CHECK(host.events.size() == 1 && host.events[0] == "lost wander@sdcard");
	CHECK(Launcher::launchedAppVolumeLost());
	CHECK(!Launcher::find("wander"));
	CHECK(Launcher::find("neon")->volumeId == "nand");  // the shadowed copy surfaces
	CHECK(!monitor.poll());
	CHECK(host.events.size() == 1);

	// Back in: restored once, and volume order is source order again even
	// though the card arrived last -- a version tie goes to internal.
	slot.inserted = true;
	putFolderApp(fs, "/sdcard", "nimbus");  // same version as internal's
	CHECK(monitor.poll());
	CHECK(host.events.size() == 2 && host.events[1] == "restored wander@sdcard");
	CHECK(!Launcher::launchedAppVolumeLost());
	CHECK(Launcher::find("nimbus")->volumeId == "nand");

	// Losing a volume the running app is not on tells the host nothing.
	Volume extra;
	extra.id = "usb";
	extra.root = "/usb";
	Launcher::addVolume(extra);
	Launcher::removeVolume("usb");
	CHECK(host.events.size() == 2);

	// The app exits on its own: it is no longer "launched", and a card
	// pulled now is nobody's business.
	host.running.clear();
	CHECK(!Launcher::launchedApp());
	slot.inserted = false;
	monitor.poll();
	CHECK(host.events.size() == 2);
	slot.inserted = true;
	monitor.poll();
	CHECK(host.events.size() == 2);

	// A host that loads in the background: launched, not yet running, is
	// kept; once it runs, a pull is reported.
	host.backgroundLoad = true;
	CHECK(Launcher::launch("wander"));
	CHECK(!Launcher::launchedApp());  // nothing runs yet
	host.running = host.pending;
	CHECK(Launcher::launchedApp().has_value());
	slot.inserted = false;
	monitor.poll();
	CHECK(host.events.size() == 3 && host.events[2] == "lost wander@sdcard");
	host.running.clear();
	host.backgroundLoad = false;

	// The card is pulled while the registry is scanning it: the scan gets
	// what it could read, and the next poll notices the card is gone.
	slot.inserted = true;
	monitor.poll();
	fs.gone = true;
	monitor.poll();
	CHECK(slot.losses >= 3);
	// (One fake backs both volumes here, so internal vanished with it.)
	CHECK(Launcher::volumes().empty());
	fs.gone = false;
	monitor.poll();
	CHECK(Launcher::volumes().size() == 2);
	CHECK(Launcher::volumes()[0].id == "nand");

	// Removing a source drops its volume.
	monitor.remove(&slot);
	CHECK(Launcher::volumes().size() == 1);
	monitor.poll();
	CHECK(!Launcher::find("wander"));
	Launcher::reset();
}

void testLauncherInstall()
{
	Card fs;
	Launcher::reset(&fs);
	Host host;
	Launcher::setHost(&host);
	Bytes ember(package("ember"));
	CHECK(!Launcher::install(ember).ok);  // no volume yet
	CHECK_CONTAINS(Launcher::install(ember).error, "no storage");
	Volume nandVolume;
	nandVolume.id = "nand";
	nandVolume.root = "/nand";
	fs.dirs.insert("/nand");
	fs.dirs.insert("/sdcard");
	Launcher::addVolume(nandVolume);
	Launcher::addVolume(card());  // the install target
	const std::uint32_t g0 = Launcher::generation();
	InstallResult r = Launcher::install(ember);
	CHECK(r.ok);
	CHECK(r.location == "/sdcard/apps/ember.geapp");
	CHECK(Launcher::generation() == g0 + 1);  // a UI redraws without being told
	CHECK(Launcher::find("ember").has_value());
	CHECK(Launcher::install(ember, "nand").ok);
	CHECK(fs.exists("/nand/apps/ember.geapp"));
	CHECK_CONTAINS(Launcher::install(ember, "usb").error, "no volume \"usb\"");
	// The host's target and ABI are checked.
	Bytes other(package("neon", "1.0.0", "abi9"));
	CHECK_CONTAINS(Launcher::install(other).error, "abi9");
	// Uninstall: refused while running, then done.
	CHECK(Launcher::launch("ember"));
	CHECK(!Launcher::uninstall("ember"));
	CHECK_CONTAINS(Launcher::lastError(), "is running");
	host.running.clear();
	CHECK(Launcher::uninstall("ember"));  // the winning copy: nand, the earlier volume
	CHECK(Launcher::find("ember").has_value());  // the other copy surfaces
	CHECK(Launcher::uninstall("ember"));
	CHECK(!Launcher::find("ember"));
	CHECK(!Launcher::uninstall("ember"));
	CHECK_CONTAINS(Launcher::lastError(), "no installed app");
	// Staged: a package pushed onto the volume, committed in place.
	fs.put("/sdcard/apps/.upload", package("sonora"));
	CHECK(Launcher::installStaged("/sdcard/apps/.upload").ok);
	CHECK(Launcher::find("sonora").has_value());
	Launcher::reset();
}

void testPinAssetsManifest()
{
	Manifest m;
	std::string error;
	CHECK(parseManifest(manifestJson("a", "1.0.0", kAbi, ",\"pinAssets\":true"), m, error));
	CHECK(m.pinAssets);
	CHECK(writeManifest(m).find("\"pinAssets\":true") != std::string::npos);
	Manifest back;
	CHECK(parseManifest(writeManifest(m), back, error) && back.pinAssets);
	CHECK(parseManifest(manifestJson("a"), m, error) && !m.pinAssets);
	CHECK(writeManifest(m).find("pinAssets") == std::string::npos);
	CHECK(!parseManifest(manifestJson("a", "1.0.0", kAbi, ",\"pinAssets\":\"yes\""), m, error));
	CHECK_CONTAINS(error, "pinAssets");
}

// Installs, scans and pulls from several tasks at once (the runtime task,
// a USB console task and the volume monitor's timer), for TSan.
void testConcurrent()
{
	Card fs;  // only touched under the launcher's operations below
	(void)fs;
	std::string root = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : ".") + "/geaos-storage-concurrent";
	posixFs().removeTree(root);
	posixFs().makeDir(root);
	Launcher::reset(&posixFs());
	Host host;
	Launcher::setHost(&host);
	Volume v;
	v.id = "disk";
	v.root = root;
	v.installTarget = true;
	FixedVolumeSource source(v, posixFs());
	VolumeMonitor monitor;
	monitor.add(&source);
	monitor.poll();
	std::atomic<bool> stop{false};
	std::thread poller([&] {
		while (!stop) monitor.poll();
	});
	std::thread reader([&] {
		while (!stop) {
			(void)Launcher::apps();
			(void)Launcher::generation();
			(void)Launcher::launchedAppVolumeLost();
		}
	});
	for (int i = 0; i < 20; i++) {
		Bytes p(package("app" + std::to_string(i % 4), std::to_string(1 + i / 4) + ".0.0"));
		CHECK(Launcher::install(p).ok);
	}
	stop = true;
	poller.join();
	reader.join();
	Launcher::rescan();
	CHECK(Launcher::apps().apps.size() == 4);
	CHECK(Launcher::find("app0")->manifest.version.toString() == "5.0.0");
	Launcher::reset();
	posixFs().removeTree(root);
}

// The real filesystem: the write side every POSIX/VFS target uses.
void testPosixFs(const std::string &scratch)
{
	Fs &fs = posixFs();
	const std::string root = scratch + "/geaos-storage-posix";
	fs.removeTree(root);
	CHECK(!fs.exists(root));
	CHECK(fs.makeDir(root));
	CHECK(fs.makeDir(root));  // exists already: fine
	{
		std::unique_ptr<WritableFile> out = fs.create(root + "/a.bin");
		CHECK(out && out->write("hello", 5) && out->write("", 0) && out->finish());
	}
	std::string text;
	CHECK(fs.readFile(root + "/a.bin", text, 100) && text == "hello");
	CHECK(fs.rename(root + "/a.bin", root + "/b.bin"));
	CHECK(!fs.exists(root + "/a.bin"));
	CHECK(fs.freeBytes(root).has_value() && *fs.freeBytes(root) > 0);
	CHECK(!fs.create(root + "/missing/dir/x"));
	// removeTree empties hidden files too (a macOS ._ fork, .DS_Store).
	CHECK(fs.makeDir(root + "/apps"));
	CHECK(fs.makeDir(root + "/apps/x"));
	CHECK(fs.makeDir(root + "/apps/x/deep"));
	for (const char *name : {"/apps/x/.DS_Store", "/apps/x/._icon.png", "/apps/x/deep/f"}) {
		std::unique_ptr<WritableFile> out = fs.create(root + name);
		CHECK(out && out->write("1", 1) && out->finish());
	}
	std::vector<DirEntry> entries;
	CHECK(fs.listDir(root + "/apps/x", entries) && entries.size() == 3);  // dot-names listed
	CHECK(fs.removeTree(root + "/apps/x"));
	CHECK(!fs.exists(root + "/apps/x"));
	CHECK(fs.removeTree(root + "/apps/never-was"));
	// Install through the real filesystem, including the hidden staging the
	// registry must not list.
	Volume v;
	v.id = "disk";
	v.root = root;
	Bytes source(package("ember"));
	InstallResult r = installPackage(fs, source, v, options());
	CHECK(r.ok);
	{
		std::unique_ptr<WritableFile> out = fs.create(root + "/apps/.neon.geapp.partial");
		CHECK(out && out->write("x", 1) && out->finish());
	}
	ScanResult all = scan({v}, fs, kTarget);
	CHECK(all.apps.size() == 1 && all.broken.empty());
	CHECK(recoverInstalls(fs, v) == 1);
	CHECK(fs.removeTree(root));
	CHECK(!fs.exists(root));
}

}  // namespace


// ---- Move between volumes ------------------------------------------------------

void testMove()
{
	const Volume nand{"nand", "/nand", false, true, "Internal storage"};
	const Volume sd = card();
	{
		// A package app moves whole; the original goes once the copy committed.
		Card fs;
		Bytes source(package("ember"));
		CHECK(installPackage(fs, source, nand, options()).ok);
		auto app = scanned(fs, nand, "ember");
		CHECK(app.has_value());
		const std::uint64_t bytes = installedBytes(fs, *app);
		CHECK(bytes == package("ember").size());
		const InstallResult r = moveApp(fs, *app, sd, options());
		CHECK(r.ok);
		CHECK(r.location == "/sdcard/apps/ember.geapp");
		CHECK(!fs.exists("/nand/apps/ember.geapp"));
		CHECK(fs.files["/sdcard/apps/ember.geapp"] == package("ember"));
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
	}
	{
		// A folder app is packed on the way: every file comes along.
		Card fs;
		putFolderApp(fs, "/nand", "neon");
		fs.put("/nand/apps/neon/assets/a/b.bin", "BIN");
		fs.put("/nand/apps/neon/icon.png", "PNG");
		auto app = scanned(fs, nand, "neon");
		CHECK(app && app->layout == AppLayout::Folder);
		CHECK(installedBytes(fs, *app) == manifestJson("neon").size() + 3 + 3 + 3);
		const InstallResult r = moveApp(fs, *app, sd, options());
		CHECK(r.ok);
		CHECK(!fs.exists("/nand/apps/neon"));
		auto moved = scanned(fs, sd, "neon");
		CHECK(moved && moved->layout == AppLayout::Package);
		std::string error, data;
		PackageReader reader;
		CHECK(reader.open(fs.open("/sdcard/apps/neon.geapp"), error));
		CHECK(reader.read("assets/a/b.bin", data, error) && data == "BIN");
		CHECK(reader.read("icon.png", data, error) && data == "PNG");
		CHECK(reader.read(programEntryName(kTarget), data, error) && data == "ELF");
	}
	{
		// The card fills mid-copy: the app stays on the device, nothing is left on the card.
		Card fs;
		Bytes source(package("ember"));
		CHECK(installPackage(fs, source, nand, options()).ok);
		fs.capacity = fs.used() + 1000;
		fs.reportsFree = false;  // so the copy starts and runs out partway
		auto app = scanned(fs, nand, "ember");
		const InstallResult r = moveApp(fs, *app, sd, options());
		CHECK(!r.ok);
		CHECK(fs.exists("/nand/apps/ember.geapp"));
		CHECK(fs.hiddenUnder("/sdcard/apps").empty());
		CHECK(!fs.exists("/sdcard/apps/ember.geapp"));
	}
	{
		// Refusals: same volume, a read-only card, a bundled app.
		Card fs;
		Bytes source(package("ember"));
		CHECK(installPackage(fs, source, nand, options()).ok);
		auto app = scanned(fs, nand, "ember");
		CHECK_CONTAINS(moveApp(fs, *app, nand, options()).error, "already on");
		Volume locked = sd;
		locked.readOnly = true;
		CHECK_CONTAINS(moveApp(fs, *app, locked, options()).error, "read-only");
		InstalledApp bundled = *app;
		bundled.location.clear();
		CHECK_CONTAINS(moveApp(fs, bundled, sd, options()).error, "part of the system");
		CHECK(fs.exists("/nand/apps/ember.geapp"));
	}
	{
		// Through the Launcher: refused while running, else moved and rescanned.
		Card fs;
		Launcher::reset(&fs);
		Host host;
		Launcher::setHost(&host);
		fs.dirs.insert("/nand");
		fs.dirs.insert("/sdcard");
		Launcher::addVolume(nand);
		Launcher::addVolume(sd);
		Bytes source(package("ember"));
		CHECK(Launcher::install(source, "nand").ok);
		// Refused while it runs.
		CHECK(Launcher::launch("ember"));
		CHECK(!Launcher::move("ember", "sdcard"));
		CHECK_CONTAINS(Launcher::lastError(), "running");
		CHECK(Launcher::home());
		CHECK(!Launcher::move("ember", "nowhere"));
		CHECK_CONTAINS(Launcher::lastError(), "no volume");
		CHECK(Launcher::move("ember", "sdcard"));
		auto app = Launcher::find("ember");
		CHECK(app && app->volumeId == "sdcard");
		CHECK(!Launcher::move("missing", "nand"));
		Launcher::reset();
	}
}

// ---- Settings ---------------------------------------------------------------------

class FakeServices final : public SystemServices {
public:
	int brightnessValue = -1, volumeValue = -1;
	bool wifi = false, ble = false;
	std::string tz;
	bool hasBrightness() const override { return true; }
	bool setBrightness(int percent) override { return brightnessValue = percent, true; }
	int brightness() const override { return brightnessValue; }
	bool hasVolume() const override { return true; }
	bool setVolume(int percent) override { return volumeValue = percent, true; }
	bool hasBle() const override { return true; }
	bool setBleEnabled(bool on) override { return ble = on, true; }
	bool hasClock() const override { return true; }
	bool setTimeZone(const std::string &zone) override { return tz = zone, true; }
	// No Wi-Fi on this device.
};

class WifiServices final : public SystemServices {
public:
	int enables = 0, disables = 0;
	std::string joinedSsid, joinedPassword;
	bool hasWifi() const override { return true; }
	bool setWifiEnabled(bool on) override { return (on ? enables : disables)++, true; }
	bool connectWifi(const std::string &ssid, const std::string &password) override
	{
		joinedSsid = ssid;
		joinedPassword = password;
		return true;
	}
};

void testWifiSettings()
{
	{
		// A chosen network is joined with its password.
		Settings settings;
		settings.setBool(settingKeys::kWifiEnabled, true);
		settings.setString(settingKeys::kWifiSsid, "home-net");
		settings.setString(settingKeys::kWifiPassword, "secret pass");
		WifiServices services;
		CHECK(applySettings(settings, services) == 1);
		CHECK(services.joinedSsid == "home-net");
		CHECK(services.joinedPassword == "secret pass");
		CHECK(services.enables == 0);
		// The password is never printed back.
		CHECK(settings.valueText(settingKeys::kWifiPassword) == "(set)");
		CHECK(settings.getString(settingKeys::kWifiPassword) == "secret pass");
	}
	{
		// No network chosen: Wi-Fi just turns on; off is off, whatever is stored.
		Settings on;
		on.setBool(settingKeys::kWifiEnabled, true);
		WifiServices a;
		CHECK(applySettings(on, a) == 1 && a.enables == 1 && a.joinedSsid.empty());
		Settings off;
		off.setBool(settingKeys::kWifiEnabled, false);
		off.setString(settingKeys::kWifiSsid, "home-net");
		WifiServices b;
		CHECK(applySettings(off, b) == 1 && b.disables == 1 && b.joinedSsid.empty());
	}
}

void testSettings()
{
	const Volume nand{"nand", "/nand", false, true, "Internal storage"};
	const std::string path = settingsPath(nand);
	CHECK(path == "/nand/system/settings.json");
	{
		// Round trip through the file, every kind of value.
		Card fs;
		fs.dirs.insert("/nand");
		Settings settings;
		CHECK(settings.load(fs, path));  // absent: empty, clean
		CHECK(settings.keys().empty());
		settings.setNumber(settingKeys::kBrightness, 70);
		settings.setNumber("x.ratio", 0.25);
		settings.setBool(settingKeys::kBleEnabled, true);
		settings.setString(settingKeys::kTimeZone, "CET-1CEST,M3.5.0,M10.5.0/3");
		settings.setString("quote", "a \"b\" \\ c\n");
		settings.setGranted("ember", "wifi", true);
		settings.setGranted("ember", "audio", true);
		settings.setGranted("ember", "wifi", true);  // no duplicate
		settings.setGranted("neon", "ble", true);
		CHECK(settings.save(fs));
		CHECK(fs.files.count(path));
		CHECK(!fs.files.count(path + ".tmp"));
		Settings back;
		CHECK(back.load(fs, path));
		CHECK(back.getNumber(settingKeys::kBrightness, 0) == 70);
		CHECK(back.getNumber("x.ratio", 0) == 0.25);
		CHECK(back.getBool(settingKeys::kBleEnabled, false));
		CHECK(back.getString(settingKeys::kTimeZone) == "CET-1CEST,M3.5.0,M10.5.0/3");
		CHECK(back.getString("quote") == "a \"b\" \\ c\n");
		CHECK(back.valueText(settingKeys::kBrightness) == "70");
		CHECK(back.valueText("x.ratio") == "0.25");
		CHECK(back.valueText(settingKeys::kBleEnabled) == "true");
		CHECK(back.valueText("absent").empty());
		CHECK((back.granted("ember") == std::vector<std::string>{"audio", "wifi"}));
		CHECK(back.isGranted("neon", "ble") && !back.isGranted("neon", "wifi"));
		// Wrong-typed reads fall back.
		CHECK(back.getBool(settingKeys::kBrightness, true));
		CHECK(back.getNumber(settingKeys::kTimeZone, 5) == 5);
		back.setGranted("ember", "wifi", false);
		CHECK((back.granted("ember") == std::vector<std::string>{"audio"}));
		back.setGranted("ember", "audio", false);
		CHECK(!back.has("apps.ember.permissions"));
		back.setGranted("ember", "bad,name", true);
		CHECK(back.granted("ember").empty());
		back.setString("apps.neon.theme", "dark");
		back.forgetApp("neon");
		CHECK(!back.has("apps.neon.permissions") && !back.has("apps.neon.theme"));
		CHECK(back.has(settingKeys::kBrightness));
		// Saving again replaces the old file (FAT: no replacing rename).
		CHECK(back.save(fs));
		Settings again;
		CHECK(again.load(fs, path) && !again.has("apps.neon.theme"));
	}
	{
		// A damaged file reads as empty, says why, and is replaced on save.
		Card fs;
		fs.put(path, "{\"display.brightness\": 40,");
		Settings settings;
		std::string error;
		CHECK(!settings.load(fs, path, &error));
		CHECK_CONTAINS(error, "damaged");
		CHECK(settings.keys().empty());
		fs.put(path, "[1,2]");
		CHECK(!settings.load(fs, path, &error));
		CHECK_CONTAINS(error, "not a JSON object");
		settings.setNumber(settingKeys::kVolume, 30);
		CHECK(settings.save(fs));
		Settings back;
		CHECK(back.load(fs, path) && back.getNumber(settingKeys::kVolume, 0) == 30);
		// Values this version does not keep (objects, arrays, null) are skipped.
		fs.put(path, "{\"a\":{\"b\":1},\"c\":[1],\"d\":null,\"e\":true}");
		CHECK(back.load(fs, path));
		CHECK((back.keys() == std::vector<std::string>{"e"}));
	}
	{
		// A cut at every step of a save leaves the old settings or the new, never a mix.
		for (int cut = 0; cut < 8; cut++) {
			Card fs;
			fs.dirs.insert("/nand");
			Settings old;
			old.load(fs, path);
			old.setNumber(settingKeys::kBrightness, 10);
			CHECK(old.save(fs));
			Settings next;
			next.load(fs, path);
			next.setNumber(settingKeys::kBrightness, 90);
			fs.pullAfter = cut;
			next.save(fs);
			fs.gone = false;
			fs.pullAfter = -1;
			Settings read;
			read.load(fs, path);
			const double value = read.getNumber(settingKeys::kBrightness, -1);
			CHECK(value == 10 || value == 90 || value == -1);
			CHECK(!fs.files.count(path) || value != -1);
		}
	}
	{
		// A read-only volume: save fails, the store is unchanged in memory.
		Card fs;
		fs.dirs.insert("/nand");
		fs.readOnly = true;
		Settings settings;
		settings.load(fs, path);
		settings.setBool("x", true);
		CHECK(!settings.save(fs));
		CHECK(settings.getBool("x", false));
	}
	{
		// Applying at boot: present keys, capabilities the device has; values clamped.
		Settings settings;
		settings.setNumber(settingKeys::kBrightness, 140);
		settings.setNumber(settingKeys::kVolume, 33.6);
		settings.setBool(settingKeys::kWifiEnabled, true);  // no Wi-Fi: skipped
		settings.setBool(settingKeys::kBleEnabled, true);
		settings.setString(settingKeys::kTimeZone, "UTC0");
		FakeServices services;
		CHECK(applySettings(settings, services) == 4);
		CHECK(services.brightnessValue == 100);
		CHECK(services.volumeValue == 34);
		CHECK(services.ble && services.tz == "UTC0");
		SystemServices none;
		CHECK(applySettings(settings, none) == 0);
		Settings empty;
		FakeServices untouched;
		CHECK(applySettings(empty, untouched) == 0 && untouched.brightnessValue == -1);
	}
	{
		// The device store: loaded from the install volume, applied, and
		// forgetting an uninstalled app's grants.
		Card fs;
		Launcher::reset(&fs);
		Host host;
		Launcher::setHost(&host);
		CHECK(!loadDeviceSettings(fs));  // no volume yet
		Launcher::addVolume(nand);
		fs.dirs.insert("/nand");
		fs.put(path, "{\"display.brightness\": 55, \"apps.ember.permissions\": \"wifi\"}");
		FakeServices services;
		setSystemServices(&services);
		CHECK(&systemServices() == &services);
		CHECK(loadDeviceSettings(fs));
		CHECK(services.brightnessValue == 55);
		CHECK(deviceSettings().isGranted("ember", "wifi"));
		Bytes source(package("ember"));
		CHECK(Launcher::install(source).ok);
		CHECK(Launcher::uninstall("ember"));
		CHECK(!deviceSettings().isGranted("ember", "wifi"));
		Settings reread;
		CHECK(reread.load(fs, path) && !reread.isGranted("ember", "wifi") && reread.getNumber(settingKeys::kBrightness, 0) == 55);
		setSystemServices(nullptr);
		CHECK(!systemServices().hasBrightness());
		deviceSettings() = Settings{};
		Launcher::reset();
	}
}

void testInstallOverRunning()
{
	const Volume nand{"nand", "/nand", false, true, "Internal storage"};
	Card fs;
	Bytes first(package("ember"));
	CHECK(installPackage(fs, first, nand, options()).ok);
	const std::string before = fs.files["/nand/apps/ember.geapp"];
	// The same app while it runs: refused, the installed copy untouched,
	// nothing staged left behind.
	InstallOptions running = options();
	running.runningId = "ember";
	Bytes again(package("ember"));
	const InstallResult r = installPackage(fs, again, nand, running);
	CHECK(!r.ok);
	CHECK(r.error.find("is running") != std::string::npos);
	CHECK(fs.files["/nand/apps/ember.geapp"] == before);
	CHECK(fs.hiddenUnder("/nand/apps").empty());
	// A staged copy is refused the same way and removed.
	fs.put("/nand/apps/.push.geapp.partial", package("ember"));
	const InstallResult staged = installStaged(fs, "/nand/apps/.push.geapp.partial", nand, running);
	CHECK(!staged.ok);
	CHECK(fs.hiddenUnder("/nand/apps").empty());
	// Another app installs while ember runs.
	Bytes other(package("neon"));
	CHECK(installPackage(fs, other, nand, running).ok);
}

// ---- Signing ---------------------------------------------------------------

namespace sig {

using namespace gea::geaos::crypto;

std::string fromHex(const std::string &hex)
{
	std::string out;
	for (std::size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
	return out;
}

struct Key {
	std::uint8_t seed[32];
	std::uint8_t pub[32];
	explicit Key(std::uint8_t fill)
	{
		for (int i = 0; i < 32; i++) seed[i] = static_cast<std::uint8_t>(fill + i * 7);
		ed25519PublicKey(seed, pub);
	}
	std::string publicText() const { return toBase64(pub, 32); }
	std::string sign(const std::string &message) const
	{
		std::uint8_t out[64];
		ed25519Sign(out, message.data(), message.size(), seed);
		return toBase64(out, 64);
	}
};

using Entries = std::vector<std::pair<std::string, std::string>>;

Entries appEntries(const std::string &id, const std::string &target = kTarget)
{
	std::string program(5000, '\0');
	for (std::size_t i = 0; i < program.size(); i++) program[i] = static_cast<char>((i * 13 + id.size()) & 0xFF);
	return {{"manifest.json", manifestJson(id)}, {programEntryName(target), program}, {"icon.png", "PNG" + id}};
}

std::string message(const Entries &entries)
{
	std::vector<std::tuple<std::string, std::uint64_t, std::string>> digests;
	for (const auto &[name, data] : entries) {
		std::uint8_t d[64];
		sha512(data.data(), data.size(), d);
		digests.emplace_back(name, data.size(), toHex(d, 64));
	}
	return signingMessage(digests);
}

// signature.json for `entries`, signed by `developer`, with a certificate
// from `root` (none when null).
std::string signatureJson(const Entries &signedEntries, const std::string &id, const Key &developer, const Key *root,
                          std::int64_t expires = 4102444800)
{
	std::string json = "{\"developer\":\"" + id + "\",\"key\":\"" + developer.publicText() + "\",\"signature\":\"" +
	                   developer.sign(message(signedEntries)) + "\"";
	if (root)
		json += ",\"certificate\":{\"expires\":" + std::to_string(expires) + ",\"signature\":\"" +
		        root->sign(certificateMessage(id, developer.publicText(), expires)) + "\"}";
	return json + "}";
}

std::string build(const Entries &entries, const std::string &signature)
{
	PackageWriter writer;
	for (const auto &[name, data] : entries) writer.add(name, data);
	if (!signature.empty()) writer.add(kSignatureEntry, signature);
	return writer.finish();
}

}  // namespace sig

void testCrypto()
{
	using namespace gea::geaos::crypto;
	std::uint8_t d[64];
	sha512("abc", 3, d);
	CHECK(toHex(d, 64) ==
	      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
	const std::string thousand(1000, 'a');
	Sha512 streamed;
	for (std::size_t i = 0; i < thousand.size(); i += 37) streamed.update(thousand.data() + i, std::min<std::size_t>(37, thousand.size() - i));
	streamed.finish(d);
	CHECK(toHex(d, 64) ==
	      "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97");

	// RFC 8032 test 1, and signatures Node's crypto made with the same key.
	const std::string seed = sig::fromHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
	const std::string pub = sig::fromHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
	std::uint8_t derived[32];
	ed25519PublicKey(reinterpret_cast<const std::uint8_t *>(seed.data()), derived);
	CHECK(std::memcmp(derived, pub.data(), 32) == 0);
	const struct {
		const char *message, *signature;
	} vectors[] = {
		{"", "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
		{"abc", "80d724b01e7ca260f4cc7f8de7c95f73cfac615bab1f762b6435b6ec26c8cf6d2c758dae2f87399a8eeda1cbcd2835ac5ba66d6ecaa3aba5e567a751053dc207"},
		{"gea-package-v1\nmanifest.json\t12\tdeadbeef\n",
		 "982fc9aec0949eacec37ee0f1f4b807f375e6c6f20cc64a129a0b97fa35b5fb15036052f31c2b2e65ecce5558be83cb5a683b2e84b3be5ac875df468e39a3200"},
	};
	const auto *pk = reinterpret_cast<const std::uint8_t *>(pub.data());
	for (const auto &v : vectors) {
		const std::string signature = sig::fromHex(v.signature);
		const auto *sg = reinterpret_cast<const std::uint8_t *>(signature.data());
		CHECK(ed25519Verify(sg, v.message, std::strlen(v.message), pk));
		std::uint8_t made[64];
		ed25519Sign(made, v.message, std::strlen(v.message), reinterpret_cast<const std::uint8_t *>(seed.data()));
		CHECK(std::memcmp(made, sg, 64) == 0);
		// One flipped bit anywhere fails it.
		std::string bad = signature;
		bad[5] ^= 1;
		CHECK(!ed25519Verify(reinterpret_cast<const std::uint8_t *>(bad.data()), v.message, std::strlen(v.message), pk));
		bad = signature;
		bad[40] ^= 1;
		CHECK(!ed25519Verify(reinterpret_cast<const std::uint8_t *>(bad.data()), v.message, std::strlen(v.message), pk));
		std::string other = std::string(v.message) + "x";
		CHECK(!ed25519Verify(sg, other.data(), other.size(), pk));
	}
	// s >= L is a malleated signature.
	std::string malleated = sig::fromHex(vectors[0].signature);
	malleated[63] = static_cast<char>(0xff);
	CHECK(!ed25519Verify(reinterpret_cast<const std::uint8_t *>(malleated.data()), "", 0, pk));
	std::string raw;
	CHECK(fromBase64(toBase64(pub.data(), 32), raw) && raw == pub);
	CHECK(fromBase64("YWJj", raw) && raw == "abc");
	CHECK(!fromBase64("YW*j", raw));
}

void testTrust()
{
	const Volume nand{"nand", "/nand", false, true, "Internal storage"};
	const sig::Key root(1), developer(50), stranger(90);
	TrustPolicy policy;
	policy.roots = {root.publicText()};
	policy.now = 1800000000;
	setTrustPolicy(policy);
	const auto entries = sig::appEntries("ember");
	auto install = [&](const std::string &bytes, InstallOptions o = options()) {
		Card fs;
		Bytes source(bytes);
		InstallResult r = installPackage(fs, source, nand, o);
		CHECK(fs.hiddenUnder("/nand/apps").empty());  // nothing staged left behind, ever
		if (r.ok) {
			auto app = scanned(fs, nand, r.id);
			CHECK(app.has_value());
			if (app) CHECK(app->trust == r.trust);
		}
		return r;
	};
	{
		// Signed by a developer the root vouches for.
		InstallResult r = install(sig::build(entries, sig::signatureJson(entries, "acme", developer, &root)));
		CHECK(r.ok);
		CHECK(r.trust == Trust::Verified);
		CHECK(r.developer == "acme");
	}
	{
		// Self-signed and unsigned: installed, labelled; refused when only verified apps may install.
		InstallResult self = install(sig::build(entries, sig::signatureJson(entries, "acme", developer, nullptr)));
		CHECK(self.ok && self.trust == Trust::Unverified);
		InstallResult foreign = install(sig::build(entries, sig::signatureJson(entries, "acme", developer, &stranger)));
		CHECK(foreign.ok && foreign.trust == Trust::Unverified);
		InstallResult none = install(sig::build(entries, ""));
		CHECK(none.ok && none.trust == Trust::Unsigned);
		InstallOptions strict = options();
		strict.allowUnverified = false;
		InstallResult refused = install(sig::build(entries, ""), strict);
		CHECK(!refused.ok && refused.error.find("unverified developer") != std::string::npos);
		CHECK(install(sig::build(entries, sig::signatureJson(entries, "acme", developer, &root)), strict).ok);
	}
	{
		// Tampered: a program byte changed after signing.
		auto changed = entries;
		changed[1].second[100] ^= 1;
		InstallResult r = install(sig::build(changed, sig::signatureJson(entries, "acme", developer, &root)));
		CHECK(!r.ok && r.error.find("does not match") != std::string::npos);
		// An entry added after signing.
		auto added = entries;
		added.emplace_back("assets/extra.bin", "X");
		CHECK(!install(sig::build(added, sig::signatureJson(entries, "acme", developer, &root))).ok);
		// A signature for another target: the program entry is named for that target.
		const auto other = sig::appEntries("ember", "esp32-xtensa-s3");
		auto swapped = other;
		swapped[1].first = programEntryName(kTarget);
		CHECK(!install(sig::build(swapped, sig::signatureJson(other, "acme", developer, &root))).ok);
		// A certificate issued to "acme" with the name changed to "evil": the root never vouched for that.
		std::string forged = sig::signatureJson(entries, "acme", developer, &root);
		forged.replace(forged.find("\"acme\""), 6, "\"evil\"");
		InstallResult renamed = install(sig::build(entries, forged));
		CHECK(renamed.ok && renamed.trust == Trust::Unverified && renamed.developer == "evil");
	}
	{
		// Expired certificate: refused while the clock is known, labelled once it is not.
		const std::string expired = sig::build(entries, sig::signatureJson(entries, "acme", developer, &root, 1700000000));
		InstallResult r = install(expired);
		CHECK(!r.ok && r.error.find("expired") != std::string::npos);
		TrustPolicy noClock = policy;
		noClock.now = 0;
		setTrustPolicy(noClock);
		CHECK(install(expired).ok);
		setTrustPolicy(policy);
	}
	{
		// Revoked developer: refused at install; an installed copy is flagged and Home will not launch it.
		Card fs;
		Bytes source(sig::build(entries, sig::signatureJson(entries, "acme", developer, &root)));
		CHECK(installPackage(fs, source, nand, options()).ok);
		TrustPolicy revoked = policy;
		revoked.revoked = {"acme"};
		setTrustPolicy(revoked);
		auto app = scanned(fs, nand, "ember");
		CHECK(app && app->revoked && app->developer == "acme");
		Bytes again(sig::build(entries, sig::signatureJson(entries, "acme", developer, &root)));
		InstallResult r = installPackage(fs, again, nand, options());
		CHECK(!r.ok && r.error.find("revoked") != std::string::npos);
		revoked.revoked = {developer.publicText()};  // by key works the same
		setTrustPolicy(revoked);
		Bytes third(sig::build(entries, sig::signatureJson(entries, "acme", developer, &root)));
		CHECK(!installPackage(fs, third, nand, options()).ok);
		setTrustPolicy(policy);
	}
	{
		// A damaged signature.json is refused, never ignored.
		CHECK(!install(sig::build(entries, "{not json")).ok);
		CHECK(!install(sig::build(entries, "{\"developer\":\"acme\",\"key\":\"short\"}")).ok);
	}
	{
		// A package the CLI signed (cli/src/geapp-sign.mjs, Node's Ed25519): the
		// device side agrees on the message, the hashes and both signatures.
		const std::string signedByCli = sig::fromHex(
	    "474541504b471a0001000300200000005a0000004ca628bf00000000000000000d006d616e69666573742e6a736f6e7a0000003a00000067f0d83b15"
	    "0070726f6772616d2f65737033322d72697363763332b40000000b00000085b821b70e007369676e61747572652e6a736f6ebf0000003c01000003a5"
	    "ae807b226964223a2274696e79222c226e616d65223a2254696e79222c2276657273696f6e223a22312e302e30222c22616269223a2261626931227d"
	    "454c462d50524f4752414d7b22646576656c6f706572223a2261636d65222c226b6579223a222f52636b4f4671677831746b2b336a4e59432b68325a"
	    "4839362f64724538574f31774c71794458703968673d222c227369676e6174757265223a22364555645a37727566622f54794f426b382f5a432f6837"
	    "7664773854656f74694c4c527873334e625461675673783543645346776777466c58514d41786e536945587875753976394834526f43514437334c78"
	    "7741673d3d222c226365727469666963617465223a7b2265787069726573223a343130323434343830302c227369676e6174757265223a2244312f65"
	    "686d536535616e655752353852756d733452724951456465462b2b6c4e336b704756394250756159653448577859352f73484748463038534f596546"
	    "5a2f42522f6873682b3333623132744332524c6844413d3d227d7d");
		TrustPolicy cli;
		cli.roots = {"6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw="};
		setTrustPolicy(cli);
		Card fs;
		Bytes source(signedByCli);
		InstallResult r = installPackage(fs, source, nand, options());
		CHECK(r.ok);
		CHECK(r.trust == Trust::Verified);
		CHECK(r.developer == "acme");
		std::string tampered = signedByCli;
		tampered[tampered.find("ELF-PROGRAM")] = 'X';
		// (the CRC catches this first; the signature would too)
		Card fs2;
		Bytes bad(tampered);
		CHECK(!installPackage(fs2, bad, nand, options()).ok);
	}
	setTrustPolicy({});
}

int main(int argc, char **argv)
{
	const std::string scratch = argc > 1 ? argv[1] : ".";
	testInstall();
	testInstallRefusals();
	testProgramSizeLimit();
	testPulledMidInstall();
	testRecover();
	testInstallStaged();
	testUninstall();
	testMonitor();
	testLauncherInstall();
	testPinAssetsManifest();
	testConcurrent();
	testMove();
	testInstallOverRunning();
	testSettings();
	testWifiSettings();
	testCrypto();
	testTrust();
	testPosixFs(scratch);
	if (g_failures) {
		std::fprintf(stderr, "FAIL geaos storage: %d of %d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("PASS geaos storage: %d checks\n", g_checks);
	return 0;
}
