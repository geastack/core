// SPDX-License-Identifier: Apache-2.0
// GeaOS app model: what an installed app is, where apps live, and how a
// target runs one. Portable C++ with no platform dependencies; every target
// (ESP32, RP2350, desktop, Apple, web) shares it and supplies an AppHost.
//
// An app lives on a volume -- a storage root a board declares (internal NAND
// or flash, an SD card, an app-data folder) -- under `<root>/apps/`, in one
// of two layouts:
//
//   <root>/apps/<id>/manifest.json (+ program, icon, assets)   a folder
//   <root>/apps/<id>.geapp                                      a package file
//
// The registry scans every mounted volume, keeps one app per id (highest
// version, then volume order), and lists what it could not use with the
// reason, so Settings can show a broken app instead of hiding it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <string_view>
#include <vector>

namespace gea::geaos {

// ---- Limits --------------------------------------------------------------

constexpr std::size_t kMaxIdLength = 63;
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::size_t kMaxManifestBytes = 16 * 1024;
constexpr std::size_t kMaxPathLength = 255;
constexpr std::size_t kMaxPackageEntries = 4096;

// ---- Manifest ------------------------------------------------------------

struct Version {
	std::uint32_t major = 0;
	std::uint32_t minor = 0;
	std::uint32_t patch = 0;

	// "1", "1.2" or "1.2.3"; each part a decimal without leading zeros.
	static bool parse(std::string_view text, Version &out);
	int compare(const Version &other) const;
	std::string toString() const;
};

enum class PresentMode : std::uint8_t { Sync, Async };

enum Permission : std::uint32_t {
	kPermissionNetwork = 1u << 0,
	kPermissionBluetooth = 1u << 1,
	kPermissionStorage = 1u << 2,
	kPermissionAudio = 1u << 3,
	kPermissionCamera = 1u << 4,
	kPermissionLocation = 1u << 5,
};

struct Manifest {
	std::string id;
	std::string name;
	Version version;
	// The firmware ABI the app was built for. Required: an app that does not
	// say which firmware it targets is refused, never guessed at.
	std::string abi;
	// Path of the icon inside the app ("" = none).
	std::string icon;
	PresentMode present = PresentMode::Sync;
	std::uint32_t permissions = 0;
	// Keep the app's assets in memory from launch, so it keeps running when
	// the volume it came from goes away (a card pulled). Without it the host
	// pauses the app until the volume comes back.
	bool pinAssets = false;
	// The memory each target's program needs once loaded (its image: code,
	// data and zeroed data), as the build measured it, by target key. A
	// device with a size limit checks it at install; a target missing here
	// is checked by its program entry's size instead.
	std::vector<std::pair<std::string, std::uint64_t>> programBytes;
	// Reserved for verification: the developer identity and the signature
	// over the package. Empty for unverified apps.
	std::string developer;
	std::string signature;
};

// [a-z0-9] followed by [a-z0-9._-], at most kMaxIdLength bytes, not ending
// in '.', and never "." / "..".
bool isValidAppId(std::string_view id);

// Parses manifest.json. Unknown fields are ignored (newer tools may add
// them); a field of the wrong type is an error. `fallbackId` names an app
// whose manifest carries no id (a folder app named by its folder).
bool parseManifest(std::string_view json, Manifest &out, std::string &error, std::string_view fallbackId = {});
std::string writeManifest(const Manifest &manifest);

// ---- Files ---------------------------------------------------------------

struct DirEntry {
	std::string name;
	bool directory = false;
	std::uint64_t size = 0;
};

class File {
public:
	virtual ~File() = default;
	virtual std::uint64_t size() const = 0;
	// Reads exactly `length` bytes at `offset`; false on a short read.
	virtual bool readAt(std::uint64_t offset, void *buffer, std::size_t length) = 0;
};

// A file being written. Nothing is promised about its contents until
// finish() returns true; a writer destroyed without finish() leaves a
// partial file the caller must remove.
class WritableFile {
public:
	virtual ~WritableFile() = default;
	virtual bool write(const void *data, std::size_t length) = 0;
	// Flushes and closes; false if any write, the flush or the close failed
	// (a full or pulled card).
	virtual bool finish() = 0;
};

// The filesystem the registry reads and the installer writes. posixFs()
// serves every target with a POSIX/VFS layer; tests substitute their own.
// The write side defaults to "not writable", so a read-only Fs needs only
// the first three.
class Fs {
public:
	virtual ~Fs() = default;
	virtual bool listDir(const std::string &path, std::vector<DirEntry> &out) = 0;
	virtual bool exists(const std::string &path) = 0;
	virtual std::unique_ptr<File> open(const std::string &path) = 0;
	// Creates or truncates a file; nullptr if the volume is read-only, full
	// or gone.
	virtual std::unique_ptr<WritableFile> create(const std::string &) { return nullptr; }
	// Creates one directory; true if it exists afterwards.
	virtual bool makeDir(const std::string &) { return false; }
	// Renames within a volume. FAT refuses to rename over an existing file,
	// so callers remove the target first and never rely on replacement.
	virtual bool rename(const std::string &, const std::string &) { return false; }
	// Removes a file or an empty directory.
	virtual bool remove(const std::string &) { return false; }
	// Free bytes on the volume holding `path`, if the filesystem can say.
	virtual std::optional<std::uint64_t> freeBytes(const std::string &) { return std::nullopt; }
	// Reads a whole file of at most `maxBytes`; false if larger or unreadable.
	bool readFile(const std::string &path, std::string &out, std::size_t maxBytes);
	// Removes a file, or a directory and everything under it. True if
	// nothing is left at `path`.
	bool removeTree(const std::string &path);
};

Fs &posixFs();

// ---- Packages (.geapp) ---------------------------------------------------
//
// Little-endian. A 32-byte header, an index, then entry data:
//
//   header  magic "GEAPKG\x1a\0" | u16 format=1 | u16 entryCount |
//           u32 indexOffset | u32 indexSize | u32 indexCrc32 | 8 zero bytes
//   index   per entry: u16 nameLength | name | u32 offset | u32 size | u32 crc32
//
// Entry names are relative paths ("manifest.json", "icon.png",
// "program/<target>", "assets/..."). One package can carry the program for
// several targets; each host reads the entry for its own target key.

constexpr std::uint16_t kPackageFormat = 1;
constexpr std::size_t kPackageHeaderSize = 32;

struct PackageEntry {
	std::string name;
	std::uint32_t offset = 0;
	std::uint32_t size = 0;
	std::uint32_t crc32 = 0;
};

class PackageReader {
public:
	// Validates the header and index against the file's size. Entry data is
	// checked (CRC) when read.
	bool open(std::unique_ptr<File> file, std::string &error);
	const std::vector<PackageEntry> &entries() const { return entries_; }
	const PackageEntry *find(std::string_view name) const;
	bool read(const PackageEntry &entry, std::string &out, std::string &error);
	bool read(std::string_view name, std::string &out, std::string &error);

private:
	std::unique_ptr<File> file_;
	std::vector<PackageEntry> entries_;
};

class PackageWriter {
public:
	// False if the name is unsafe or already added.
	bool add(std::string_view name, std::string_view data);
	std::string finish() const;

private:
	std::vector<std::pair<std::string, std::string>> entries_;
};

// A relative path with no empty, "." or ".." parts, no backslash, no NUL,
// not absolute, at most kMaxPathLength bytes.
bool isSafeEntryName(std::string_view name);

std::uint32_t crc32(const void *data, std::size_t length, std::uint32_t crc = 0);

// The program entry name for a target key ("program/<key>").
std::string programEntryName(std::string_view targetKey);

// ---- Volumes -------------------------------------------------------------

struct Volume {
	Volume() = default;
	// Positional, so a board writes {"nand", "/nand", false, true}.
	Volume(std::string id, std::string root, bool removable = false, bool installTarget = false, std::string label = {},
	       bool readOnly = false)
	    : id(std::move(id)), root(std::move(root)), removable(removable), installTarget(installTarget), label(std::move(label)),
	      readOnly(readOnly)
	{
	}
	std::string id;    // "nand", "sdcard", "internal", "appdata" ...
	std::string root;  // apps live in <root>/apps
	bool removable = false;
	// Where new apps install by default. A board marks exactly one.
	bool installTarget = false;
	// Shown in Settings and in errors ("SD card", "Internal storage").
	std::string label;
	// Mounted read-only (a write-protected card): apps run, nothing installs.
	bool readOnly = false;
};

std::string appsDirectory(const Volume &volume);

// ---- Trust ---------------------------------------------------------------
//
// A package may carry signature.json: the developer's Ed25519 signature over
// every other entry (name, size and SHA-512, sorted by name -- so the
// manifest, each target's program and every asset are covered), and a
// certificate in which a root of the developer program vouches for that
// developer's key until a date.
//
//   {"developer":"acme","key":"<base64 Ed25519 public key>",
//    "signature":"<base64 signature over signingMessage()>",
//    "certificate":{"expires":<unix seconds>,"signature":"<base64 root signature over certificateMessage()>"}}
//
// Verified: the signature matches and a trusted root signed the certificate.
// Unverified: signed, but by a key no trusted root vouches for. Unsigned: no
// signature.json. A signature that does not match the package, a revoked
// developer or key, and an expired certificate are refused outright.

inline constexpr char kSignatureEntry[] = "signature.json";

enum class Trust : std::uint8_t { Unsigned, Unverified, Verified };
const char *trustName(Trust trust);

struct TrustPolicy {
	std::vector<std::string> roots;    // root public keys, base64
	std::vector<std::string> revoked;  // revoked developer ids and keys (base64)
	// Unix seconds; 0 while the clock is unknown (expiry unchecked); negative:
	// read the system clock at each check (unknown until it is past 2023).
	std::int64_t now = 0;
};

// The process-wide policy installs and scans check against.
void setTrustPolicy(TrustPolicy policy);
TrustPolicy trustPolicy();

struct SignatureCheck {
	Trust trust = Trust::Unsigned;
	std::string developer;
	bool revoked = false;
};

// One line per entry: "<name>\t<size>\t<sha512 hex>\n", sorted by name,
// signature.json left out, after the header "gea-package-v1\n".
std::string signingMessage(std::vector<std::tuple<std::string, std::uint64_t, std::string>> entries);
std::string certificateMessage(std::string_view developer, std::string_view key, std::int64_t expires);
// Checks signature.json against `message` (from signingMessage). An empty
// message skips the package signature (a scan, after the install verified
// it) and checks only the certificate and revocation. False with the reason
// for a package that must not install.
bool checkSignature(std::string_view signatureJson, const std::string &message, const TrustPolicy &policy, SignatureCheck &out,
                    std::string &error);

// ---- Registry ------------------------------------------------------------

enum class AppLayout : std::uint8_t { Folder, Package };

struct InstalledApp {
	Manifest manifest;
	std::string volumeId;
	// The app's folder, or its .geapp file.
	std::string location;
	AppLayout layout = AppLayout::Folder;
	// Path of the icon file for a folder app ("" for none or a package app,
	// whose icon is the manifest's icon entry).
	std::string iconPath;
	// Who signed it, as far as the trust policy can tell at scan time.
	Trust trust = Trust::Unsigned;
	std::string developer;
	bool revoked = false;  // a revoked developer or key: Home refuses to launch it
};

struct BrokenApp {
	std::string volumeId;
	std::string location;
	std::string reason;
};

struct ScanResult {
	// One app per id, sorted by name, then id.
	std::vector<InstalledApp> apps;
	// Apps with the same id that lost to a higher version or earlier volume.
	std::vector<InstalledApp> shadowed;
	std::vector<BrokenApp> broken;
};

// Scans `volumes` in order (earlier wins a version tie). A folder app needs
// a manifest and a program for `targetKey`: "program/<targetKey>" or the
// legacy single "program.so". A package app needs a manifest entry and the
// same program entry. A volume whose apps folder is missing has no apps.
ScanResult scan(const std::vector<Volume> &volumes, Fs &fs, std::string_view targetKey);

// The location of the program file for a folder app ("" if none).
std::string folderProgramPath(Fs &fs, const std::string &folder, std::string_view targetKey);

// Refuses an app built for another firmware ABI, or for none.
bool checkCompatible(const Manifest &manifest, std::string_view firmwareAbi, std::string &error);

// ---- Install, uninstall, move -------------------------------------------
//
// An install never leaves a half-written app where the registry looks. The
// package is written to <apps>/.<id>.geapp.partial (the registry ignores
// dot-names), every entry's CRC is checked by reading it back, and only then
// is it renamed to .<id>.geapp.ready -- the commit point. The old copy of the
// app on that volume (package or folder) is removed and .ready renamed to
// <id>.geapp. A power cut or pulled card at any step leaves either the old
// app or, once .ready exists, the new one: recoverInstalls() on the next
// mount deletes a stale .partial and finishes a .ready.

struct InstallOptions {
	// The program entry the device runs; a package without it is refused
	// ("not built for this device"). Empty skips the check.
	std::string targetKey;
	// The firmware ABI; a package built for another is refused. Empty skips.
	std::string abi;
	// Bytes kept free on the volume beyond the package itself.
	std::uint64_t reserveBytes = 64 * 1024;
	// The largest program the device runs (0: no limit). A package whose
	// program for targetKey needs more is refused.
	std::uint64_t maxProgramBytes = 0;
	// The app running now: a package with this id is refused (its program
	// and assets are in use). Empty skips.
	std::string runningId;
	// Checked against the process-wide trustPolicy(). False refuses a
	// package that is unsigned or not vouched for by a trusted root.
	bool allowUnverified = true;
};

struct InstallResult {
	bool ok = false;
	std::string error;
	Trust trust = Trust::Unsigned;
	std::string developer;
	std::string id;
	std::string location;  // the installed <apps>/<id>.geapp
};

// Copies the package read from `source` onto `volume` and commits it.
InstallResult installPackage(Fs &fs, File &source, const Volume &volume, const InstallOptions &options);
// Commits a package already written onto `volume` (pushed over USB or
// downloaded straight to the card): verifies it in place, then the same
// commit as installPackage. `staged` must be inside the volume's apps folder
// and is consumed (renamed or, on a refusal, removed).
InstallResult installStaged(Fs &fs, const std::string &staged, const Volume &volume, const InstallOptions &options);
// Finishes or discards what an interrupted install left on `volume`.
// Returns how many leftovers it handled.
int recoverInstalls(Fs &fs, const Volume &volume);
// Removes an installed app (its package or its folder) from its volume.
bool uninstallApp(Fs &fs, const InstalledApp &app, std::string &error);
// Validates a package without installing it: header, index, every entry's
// CRC, the manifest, and the options' target and ABI. Fills `manifest`.
bool verifyPackage(Fs &fs, const std::string &path, const InstallOptions &options, Manifest &manifest, std::string &error);
// Moves an installed app to another volume (device to card and back): the
// same verified, committed install there, then the original removed. A
// folder app is packed into a .geapp on the way. The original stays if the
// copy fails.
InstallResult moveApp(Fs &fs, const InstalledApp &app, const Volume &to, const InstallOptions &options);
// Bytes an installed app takes on its volume (its package, or its folder).
std::uint64_t installedBytes(Fs &fs, const InstalledApp &app);

// ---- Volume monitoring ----------------------------------------------------
//
// A source is one place apps can live that may come and go: an SD slot, a
// USB drive, a folder on a desktop. The monitor polls each source (a target
// calls poll() from a timer, or right away when a card-detect pin changes),
// adds or removes its volume, and rescans when anything moved. The same
// rescan is the desktop's folder watcher: an unchanged folder costs one
// directory listing and changes nothing.

class VolumeSource {
public:
	virtual ~VolumeSource() = default;
	// The volume this source provides when present.
	virtual Volume volume() const = 0;
	// Called on every poll: mount the volume if it is there and not mounted
	// yet, notice it is gone if it was. True while it is mounted and usable.
	virtual bool probe() = 0;
	// Called once after probe() first returns false: release what the mount
	// held (unmount, free the card handle). The next probe() may mount again.
	virtual void lost() {}
};

// A volume that is always there once mounted (internal NAND, an app-data
// folder): probe() is `exists(root)`.
class FixedVolumeSource final : public VolumeSource {
public:
	FixedVolumeSource(Volume volume, Fs &fs) : volume_(std::move(volume)), fs_(fs) {}
	Volume volume() const override { return volume_; }
	bool probe() override { return fs_.exists(volume_.root); }

private:
	Volume volume_;
	Fs &fs_;
};

class VolumeMonitor {
public:
	// Sources are polled in the order added, which is also volume order (an
	// earlier volume wins a version tie). The monitor does not own them.
	void add(VolumeSource *source);
	void remove(VolumeSource *source);
	// Probes every source, applies arrivals and removals to the Launcher,
	// recovers interrupted installs on a newly mounted volume, and rescans.
	// True if the list of apps changed. With `rescanUnchanged` false the
	// rescan runs only when a volume came or went: a device polling while an
	// app runs spares that app the storage traffic of a full listing.
	bool poll(bool rescanUnchanged = true);
	// Test support.
	void clear();

private:
	struct Entry {
		VolumeSource *source;
		bool mounted;
	};
	std::vector<Entry> sources_;
};

// ---- Hosts and the launcher ----------------------------------------------

// What a target supplies: how it runs an app. An in-process host (ESP32,
// RP2350) loads the program into its own address space; a process host
// (desktop, Linux, Android) starts a child process; a bundled host (Apple)
// switches to an app compiled into the shell.
class AppHost {
public:
	virtual ~AppHost() = default;
	// Which program entry this host runs ("esp32-riscv32", "macos-arm64" ...).
	virtual const char *targetKey() const = 0;
	// The firmware ABI apps must match.
	virtual const char *abi() const = 0;
	// The largest program this device runs, in bytes once loaded (0: no
	// limit). Installs check it; the loader checks it again.
	virtual std::uint64_t maxProgramBytes() const { return 0; }
	// Starts switching to `app`. May finish later (a background load); errors
	// found later are reported through Launcher::setLastError.
	virtual bool launch(const InstalledApp &app, std::string &error) = 0;
	// Returns to Home.
	virtual bool home(std::string &error) = 0;
	// The id of the app now running ("" while Home runs).
	virtual const char *runningAppId() const = 0;
	// The id of the firmware or shell itself ("home"), shown while no app runs.
	virtual const char *firmwareId() const { return ""; }
	// The apps a bundled host carries; empty for hosts that scan volumes.
	virtual std::vector<InstalledApp> bundledApps() const { return {}; }
	// The volume the running app came from went away (`app` is what was
	// launched). Its code is already in memory; a host whose app reads
	// assets from the volume pauses it unless manifest.pinAssets.
	virtual void volumeLost(const InstalledApp &) {}
	// That volume is back: resume what volumeLost paused.
	virtual void volumeRestored(const InstalledApp &) {}
};

class Launcher {
public:
	static void setHost(AppHost *host);
	static AppHost *host();

	static void addVolume(const Volume &volume);
	// Removes a volume (a card taken out); its apps disappear on the next
	// scan. If the running app came from it, the host hears volumeLost();
	// when the same volume is added again, volumeRestored().
	static void removeVolume(std::string_view id);
	static std::vector<Volume> volumes();
	// Puts the named volumes first, in this order (scan order: an earlier
	// volume wins a version tie). Unnamed ones keep their order after them.
	static void orderVolumes(const std::vector<std::string> &ids);
	// The volume new apps install to: the one marked installTarget, else the
	// first; nothing if no volume is mounted.
	static std::optional<Volume> installVolume();

	// Rescans every volume. True if the set of apps changed (ids, versions,
	// locations or broken entries) since the last scan.
	static bool rescan();
	// Counts the rescans that changed the list. A UI keeps the value it last
	// drew and redraws when it moves, without being called from another task.
	static std::uint32_t generation();

	// Installs onto the install volume (or `volumeId`), then rescans. The
	// host's target key and ABI are checked when a host is set.
	static InstallResult install(File &source, std::string_view volumeId = {});
	static InstallResult installStaged(const std::string &staged, std::string_view volumeId = {});
	// Removes the app that `id` resolves to. Refused while it runs.
	static bool uninstall(std::string_view id);
	// Moves the app that `id` resolves to onto volume `volumeId`, then
	// rescans. Refused while it runs; false with the reason in lastError().
	static bool move(std::string_view id, std::string_view volumeId);
	// The app most recently launched, while it still runs.
	static std::optional<InstalledApp> launchedApp();
	// True while the running app's volume is gone (volumeLost was sent).
	static bool launchedAppVolumeLost();
	// Copies: a scan on another task may replace the list at any time.
	static ScanResult apps();
	static std::optional<InstalledApp> find(std::string_view id);

	// Resolves `id` (rescanning once if it is not known yet: a card may have
	// arrived), checks it against the host's ABI, and asks the host to run it.
	// False with the reason in lastError().
	static bool launch(std::string_view id);
	static bool home();
	// The running app's id, "" while Home (or a firmware without a host) runs.
	static const char *runningAppId();
	// What is on screen: the running app, else the firmware's own id.
	static const char *currentId();
	// True while an app (not Home) runs: a home button has somewhere to go.
	static bool appRunning();

	static std::string lastError();
	static void setLastError(std::string_view error);

	// The filesystem the launcher scans and installs through.
	static Fs &fs();

	// Test support: forgets volumes, apps, host and errors.
	static void reset(Fs *fs = nullptr);
};

// ---- Settings and system services ---------------------------------------
//
// Per-device settings, kept as one JSON object in <volume>/system/settings.json
// on the install volume and applied at boot. Keys are dotted names; values are
// booleans, numbers or strings. Per-app permission grants live under
// "apps.<id>.permissions" as a comma-separated list.

class Settings {
public:
	// Reads `path`. A missing file is an empty store; a damaged one is
	// treated as empty too (and replaced on the next save), with the reason
	// in `error`. True if the file was read cleanly or was absent.
	bool load(Fs &fs, const std::string &path, std::string *error = nullptr);
	// Writes the store to the path it was loaded from: to <path>.tmp first,
	// then over the old file, so a cut leaves the old settings or the new.
	bool save(Fs &fs) const;
	const std::string &path() const { return path_; }

	bool has(std::string_view key) const;
	bool getBool(std::string_view key, bool fallback) const;
	double getNumber(std::string_view key, double fallback) const;
	std::string getString(std::string_view key, std::string_view fallback = {}) const;
	void setBool(std::string_view key, bool value);
	void setNumber(std::string_view key, double value);
	void setString(std::string_view key, std::string_view value);
	void remove(std::string_view key);
	// Every key, sorted.
	std::vector<std::string> keys() const;
	// A value as text: "true"/"false", a number, or the string itself; "" if absent.
	std::string valueText(std::string_view key) const;

	// Permission grants an app holds (sorted, no duplicates).
	std::vector<std::string> granted(std::string_view appId) const;
	bool isGranted(std::string_view appId, std::string_view permission) const;
	void setGranted(std::string_view appId, std::string_view permission, bool granted);
	// Forgets everything stored for an app (on uninstall).
	void forgetApp(std::string_view appId);

	// Test support.
	void clear() { values_.clear(); }

private:
	struct Entry {
		enum class Kind : std::uint8_t { Bool, Number, String } kind = Kind::String;
		bool boolean = false;
		double number = 0;
		std::string string;
	};
	std::vector<std::pair<std::string, Entry>> values_;  // sorted by key
	std::string path_;
	Entry *find(std::string_view key);
	const Entry *find(std::string_view key) const;
	Entry &slot(std::string_view key);
};

// The keys the system itself reads.
namespace settingKeys {
inline constexpr char kBrightness[] = "display.brightness";  // 0..100
inline constexpr char kVolume[] = "audio.volume";            // 0..100
inline constexpr char kWifiEnabled[] = "wifi.enabled";
inline constexpr char kWifiSsid[] = "wifi.ssid";
inline constexpr char kTrustRevoked[] = "trust.revoked";            // comma-separated developer ids and keys
inline constexpr char kTrustRequireVerified[] = "trust.requireVerified";  // refuse apps no root vouches for
inline constexpr char kWifiPassword[] = "wifi.password";  // never printed back (Settings::valueText redacts it)
inline constexpr char kBleEnabled[] = "ble.enabled";
inline constexpr char kTimeZone[] = "time.zone";             // POSIX TZ string
inline constexpr char kInstallVolume[] = "storage.installVolume";
}  // namespace settingKeys

// Where a volume keeps the device's settings.
std::string settingsPath(const Volume &volume);

struct PowerState {
	int batteryPercent = -1;  // -1: no battery, or unknown
	bool charging = false;
	bool external = true;     // running from external power
};

struct About {
	std::string firmware;  // "home 0.1.0"
	std::string abi;
	std::string board;
	std::string targetKey;
};

// What a target can do for Settings. Every capability defaults to absent: a
// target overrides what its hardware has, and Settings shows only those.
class SystemServices {
public:
	virtual ~SystemServices() = default;
	virtual bool hasBrightness() const { return false; }
	virtual bool setBrightness(int /*percent*/) { return false; }
	virtual int brightness() const { return -1; }

	virtual bool hasVolume() const { return false; }
	virtual bool setVolume(int /*percent*/) { return false; }
	virtual int volume() const { return -1; }

	virtual bool hasWifi() const { return false; }
	virtual bool setWifiEnabled(bool /*on*/) { return false; }
	virtual bool connectWifi(const std::string & /*ssid*/, const std::string & /*password*/) { return false; }
	virtual std::string wifiStatus() const { return {}; }  // "connected to <ssid>", "off" ...

	virtual bool hasBle() const { return false; }
	virtual bool setBleEnabled(bool /*on*/) { return false; }

	virtual bool hasClock() const { return false; }
	virtual bool setTimeZone(const std::string & /*tz*/) { return false; }

	virtual PowerState power() const { return {}; }
	virtual About about() const { return {}; }
};

// The target's services (a default with nothing, until a target sets its own).
void setSystemServices(SystemServices *services);
SystemServices &systemServices();

// Applies stored settings to the services: brightness, volume, radios, time
// zone. Keys that are absent, or capabilities the target lacks, are skipped.
// Returns how many settings were applied.
int applySettings(const Settings &settings, SystemServices &services);

// The device's settings, loaded from the install volume by loadDeviceSettings.
Settings &deviceSettings();
// Loads deviceSettings() from the install volume (or the first volume) and
// applies them; a target calls it at boot once its volumes are added. False
// if there is no volume to keep settings on.
bool loadDeviceSettings(Fs &fs);
// Takes the revocation list from settings (trust.revoked) into the process
// trust policy, keeping its roots. loadDeviceSettings calls it.
void applyTrustSettings(const Settings &settings);

// Posts the system Settings toggle (a long press of a board's home button)
// to the runtime's event queue. Defined in geaos_system.cpp, which links the
// runtime's scheduler; false if the queue is full.
bool queueSettingsToggle();

}  // namespace gea::geaos
