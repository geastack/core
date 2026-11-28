// SPDX-License-Identifier: Apache-2.0
// GeaOS app model: manifest, package, registry and launcher, including the
// malformed and adversarial inputs an app volume (an SD card anyone can write)
// can hold. Built with ASan/UBSan by run-geaos-app-model.sh.
#include "geaos.h"
#include "geaos_host_contract.h"
#include "geaos_json.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

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

// ---- An in-memory filesystem ---------------------------------------------

class MemFile final : public File {
public:
	explicit MemFile(std::string data) : data_(std::move(data)) {}
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

class MemFs final : public Fs {
public:
	std::map<std::string, std::string> files;
	std::set<std::string> dirs;
	std::atomic<int> listCalls{0};  // scanned from several threads in the launcher stress test

	void put(const std::string &path, const std::string &data)
	{
		files[path] = data;
		for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
			dirs.insert(path.substr(0, slash));
	}
	void mkdir(const std::string &path) { put(path + "/.keep", ""), files.erase(path + "/.keep"); }

	bool listDir(const std::string &path, std::vector<DirEntry> &out) override
	{
		listCalls++;
		out.clear();
		if (!dirs.count(path)) return false;
		std::set<std::string> seen;
		const std::string prefix = path + "/";
		const auto consider = [&](const std::string &full, bool isFile, std::uint64_t size) {
			if (full.compare(0, prefix.size(), prefix) != 0) return;
			const std::string rest = full.substr(prefix.size());
			const std::size_t slash = rest.find('/');
			const std::string name = rest.substr(0, slash);
			if (name.empty() || name[0] == '.' || !seen.insert(name).second) return;
			out.push_back({name, slash != std::string::npos || !isFile, isFile && slash == std::string::npos ? size : 0});
		};
		for (const auto &f : files) consider(f.first, true, f.second.size());
		for (const auto &d : dirs) consider(d, false, 0);
		return true;
	}
	bool exists(const std::string &path) override { return files.count(path) || dirs.count(path); }
	std::unique_ptr<File> open(const std::string &path) override
	{
		auto it = files.find(path);
		if (it == files.end()) return nullptr;
		return std::make_unique<MemFile>(it->second);
	}
};

std::unique_ptr<File> memFile(const std::string &data)
{
	return std::make_unique<MemFile>(data);
}

std::string manifestJson(const std::string &id, const std::string &extra = "", const std::string &abi = "abi1")
{
	return "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"abi\":\"" + abi + "\"" + extra + "}";
}

std::string packageWith(const std::vector<std::pair<std::string, std::string>> &entries)
{
	PackageWriter writer;
	for (const auto &entry : entries) writer.add(entry.first, entry.second);
	return writer.finish();
}

// Patches a little-endian u32 / u16 into `data`.
void poke32(std::string &data, std::size_t at, std::uint32_t v)
{
	for (int i = 0; i < 4; i++) data[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
void poke16(std::string &data, std::size_t at, std::uint16_t v)
{
	data[at] = static_cast<char>(v & 0xFF);
	data[at + 1] = static_cast<char>(v >> 8);
}
// Recomputes the index CRC after hand-editing an index.
std::uint32_t peek32(const std::string &data, std::size_t at)
{
	std::uint32_t v = 0;
	for (int i = 3; i >= 0; i--) v = (v << 8) | static_cast<unsigned char>(data[at + i]);
	return v;
}
void fixIndexCrc(std::string &data)
{
	const std::uint64_t offset = peek32(data, 12), size = peek32(data, 16);
	if (offset + size > data.size()) return;  // a fuzzed index outside the file stays broken
	poke32(data, 20, crc32(data.data() + offset, static_cast<std::size_t>(size)));
}

bool openPackage(const std::string &data, std::string &error)
{
	PackageReader reader;
	return reader.open(memFile(data), error);
}

// ---- JSON ------------------------------------------------------------------

void testJson()
{
	json::Value v;
	std::string e;
	CHECK(json::parse("{\"a\":[1,2,{\"b\":null}],\"c\":true,\"d\":-1.5e3}", v, e));
	CHECK(v.get("c") && v.get("c")->boolean);
	CHECK(v.get("d") && v.get("d")->number == -1500);
	CHECK(v.get("missing") == nullptr);
	CHECK(json::parse("  \"x\"  ", v, e) && v.string == "x");
	CHECK(json::parse("\xEF\xBB\xBF{}", v, e));  // BOM tolerated

	const char *bad[] = {
	    "",          " ",          "{",          "}",          "{\"a\":1,}", "[1,]",        "{\"a\" 1}",
	    "{a:1}",     "{\"a\":1}x", "[1 2]",      "01",         "1.",         ".5",          "-",
	    "1e",        "tru",        "nul",        "\"abc",      "\"\\x\"",    "\"\\u12\"",   "\"\\ud800\"",
	    "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"\\u0000\"", "\"a\nb\"", "{\"a\":1,\"a\":2}", "'x'",
	};
	for (const char *text : bad) {
		e.clear();
		const bool ok = json::parse(text, v, e);
		if (ok) std::fprintf(stderr, "json accepted: %s\n", text);
		CHECK(!ok);
		CHECK(!e.empty());
	}

	CHECK(json::parse("\"\\ud83d\\ude00\"", v, e) && v.string == "\xF0\x9F\x98\x80");  // surrogate pair
	CHECK(json::parse("\"\\u00e9\\n\\t\\\"\\\\\\/\"", v, e) && v.string == "\xC3\xA9\n\t\"\\/");

	std::string deep(33, '[');
	deep += std::string(33, ']');
	CHECK(!json::parse(deep, v, e));
	std::string ok32(32, '[');
	ok32 += std::string(32, ']');
	CHECK(json::parse(ok32, v, e));

	// Raw UTF-8 validation inside strings.
	CHECK(!json::parse("\"\xC0\x80\"", v, e));          // overlong NUL
	CHECK(!json::parse("\"\xE0\x80\xAF\"", v, e));      // overlong '/'
	CHECK(!json::parse("\"\xED\xA0\x80\"", v, e));      // encoded surrogate
	CHECK(!json::parse("\"\xF5\x80\x80\x80\"", v, e));  // above U+10FFFF
	CHECK(!json::parse("\"\xE2\x82\"", v, e));          // truncated sequence
	CHECK(!json::parse("\"\x80\"", v, e));              // lone continuation
	CHECK(json::parse("\"\xE2\x82\xAC\xF0\x9F\x98\x80\"", v, e));  // euro + emoji

	CHECK(json::isValidUtf8(""));
	CHECK(!json::isValidUtf8("\xC3"));
	CHECK(!json::isValidUtf8("\xF4\x90\x80\x80"));

	std::string quoted;
	json::appendQuoted(quoted, std::string("a\"b\\c\n\x01", 7));
	CHECK(quoted == "\"a\\\"b\\\\c\\n\\u0001\"");
}

// ---- Versions and ids --------------------------------------------------------

void testVersionAndId()
{
	Version v;
	CHECK(Version::parse("1", v) && v.major == 1 && v.minor == 0 && v.patch == 0);
	CHECK(Version::parse("1.2", v) && v.minor == 2);
	CHECK(Version::parse("10.20.30", v) && v.toString() == "10.20.30");
	CHECK(Version::parse("0.0.0", v));
	CHECK(Version::parse("999999999.0.1", v));
	for (const char *bad : {"", "1.2.3.4", "01", "1.02", "1..2", "a", "1.", ".1", "1.2.", "-1", "1.2.3-beta", " 1",
	                        "1000000000", "1.2.3 "}) {
		const bool ok = Version::parse(bad, v);
		if (ok) std::fprintf(stderr, "version accepted: %s\n", bad);
		CHECK(!ok);
	}
	Version a, b;
	Version::parse("1.2.3", a);
	Version::parse("1.10.0", b);
	CHECK(a.compare(b) < 0 && b.compare(a) > 0 && a.compare(a) == 0);

	for (const char *good : {"a", "neon", "neon-drift", "com.example.app", "a_b", "9lives", "x.y-z_0"})
		CHECK(isValidAppId(good));
	for (const char *bad : {"", "-a", ".a", "_a", "a.", "Neon", "a b", "a/b", "a\\b", "..", ".", "caf\xC3\xA9", "a\n"})
		CHECK(!isValidAppId(bad));
	CHECK(isValidAppId(std::string(63, 'a')));
	CHECK(!isValidAppId(std::string(64, 'a')));
}

// ---- Manifests ---------------------------------------------------------------

void testManifest()
{
	Manifest m;
	std::string e;
	CHECK(parseManifest("{\"id\":\"neon\",\"abi\":\"x\"}", m, e));
	CHECK(m.id == "neon" && m.name == "neon" && m.abi == "x" && m.present == PresentMode::Sync && m.permissions == 0);
	CHECK(m.version.compare(Version{}) == 0);

	CHECK(parseManifest("{\"name\":\"Neon\"}", m, e, "neon") && m.id == "neon" && m.name == "Neon");
	CHECK(!parseManifest("{\"id\":\"other\"}", m, e, "neon"));
	CHECK_CONTAINS(e, "does not match");
	CHECK(!parseManifest("{\"name\":\"x\"}", m, e));
	CHECK_CONTAINS(e, "no \"id\"");
	CHECK(!parseManifest("{\"id\":\"Bad Id\"}", m, e));
	CHECK_CONTAINS(e, "invalid app id");

	CHECK(!parseManifest("[]", m, e));
	CHECK_CONTAINS(e, "JSON object");
	CHECK(!parseManifest("not json", m, e));
	CHECK_CONTAINS(e, "not valid JSON");
	CHECK(!parseManifest("{\"id\":5}", m, e));
	CHECK_CONTAINS(e, "must be a string");
	CHECK(!parseManifest("{\"id\":\"a\",\"name\":[]}", m, e));
	CHECK(parseManifest("{\"id\":\"a\",\"name\":null}", m, e) && m.name == "a");  // null = absent

	CHECK(parseManifest("{\"id\":\"a\",\"name\":\"" + std::string(64, 'x') + "\"}", m, e));
	CHECK(!parseManifest("{\"id\":\"a\",\"name\":\"" + std::string(65, 'x') + "\"}", m, e));
	CHECK(!parseManifest("{\"id\":\"a\",\"name\":\"two\\nlines\"}", m, e));
	CHECK_CONTAINS(e, "control characters");
	CHECK(parseManifest("{\"id\":\"a\",\"name\":\"Caf\\u00e9 \\\"Neon\\\"\"}", m, e) && m.name == "Caf\xC3\xA9 \"Neon\"");

	CHECK(parseManifest("{\"id\":\"a\",\"version\":\"2.1.0\",\"future\":{\"x\":[1]}}", m, e));
	CHECK(m.version.major == 2 && m.version.minor == 1);
	CHECK(!parseManifest("{\"id\":\"a\",\"version\":\"2.x\"}", m, e));
	CHECK_CONTAINS(e, "version");
	CHECK(!parseManifest("{\"id\":\"a\",\"version\":2}", m, e));

	CHECK(parseManifest("{\"id\":\"a\",\"present\":\"async\"}", m, e) && m.present == PresentMode::Async);
	CHECK(!parseManifest("{\"id\":\"a\",\"present\":\"fast\"}", m, e));

	CHECK(parseManifest("{\"id\":\"a\",\"permissions\":[\"network\",\"audio\",\"network\"]}", m, e));
	CHECK(m.permissions == (kPermissionNetwork | kPermissionAudio));
	CHECK(!parseManifest("{\"id\":\"a\",\"permissions\":[\"root\"]}", m, e));
	CHECK_CONTAINS(e, "unknown permission");
	CHECK(!parseManifest("{\"id\":\"a\",\"permissions\":\"network\"}", m, e));
	CHECK(!parseManifest("{\"id\":\"a\",\"permissions\":[1]}", m, e));

	CHECK(parseManifest("{\"id\":\"a\",\"icon\":\"assets/icon.png\"}", m, e) && m.icon == "assets/icon.png");
	for (const char *icon : {"../icon.png", "/etc/x", "a//b", "a/./b", "a\\\\b", ""}) {
		const std::string text = std::string("{\"id\":\"a\",\"icon\":\"") + icon + "\"}";
		const bool ok = parseManifest(text, m, e);
		if (*icon) CHECK(!ok);
		else CHECK(ok && m.icon.empty());
	}

	CHECK(!parseManifest("{\"id\":\"a\",\"abi\":\"" + std::string(65, 'f') + "\"}", m, e));
	std::string huge = "{\"id\":\"a\",\"pad\":\"" + std::string(kMaxManifestBytes, ' ') + "\"}";
	CHECK(!parseManifest(huge, m, e));
	CHECK_CONTAINS(e, "larger than");

	Manifest out;
	out.id = "com.example.neon";
	out.name = "Neon \"Drift\" \xE2\x9C\xA8";
	Version::parse("3.4.5", out.version);
	out.abi = "dfa9533191c543cb";
	out.icon = "icon.png";
	out.present = PresentMode::Async;
	out.permissions = kPermissionBluetooth | kPermissionCamera;
	out.developer = "dev-123";
	out.signature = "sig==";
	Manifest back;
	CHECK(parseManifest(writeManifest(out), back, e));
	CHECK(back.id == out.id && back.name == out.name && back.version.compare(out.version) == 0 && back.abi == out.abi &&
	      back.icon == out.icon && back.present == out.present && back.permissions == out.permissions &&
	      back.developer == out.developer && back.signature == out.signature);
}

// ---- Packages ------------------------------------------------------------

void testPackage()
{
	const std::string program(5000, '\x7f');
	const std::string data = packageWith({{"manifest.json", manifestJson("neon")},
	                                      {"program/esp32-riscv32", program},
	                                      {"icon.png", "PNG"},
	                                      {"assets/empty", ""}});
	PackageReader reader;
	std::string e;
	CHECK(reader.open(memFile(data), e));
	CHECK(reader.entries().size() == 4);
	std::string out;
	CHECK(reader.read("program/esp32-riscv32", out, e) && out == program);
	CHECK(reader.read("assets/empty", out, e) && out.empty());
	CHECK(!reader.read("missing", out, e));
	CHECK(reader.find("icon.png") && !reader.find("ICON.PNG"));

	// An empty package is well-formed (the registry then refuses it).
	CHECK(openPackage(packageWith({}), e));

	// Golden package: the CLI's JS writer (cli/src/geapp-package.mjs, tested in
	// cli/test/geapp-package.test.mjs) must produce these exact bytes too.
	{
		static const char kGoldenHex[] =
		    "474541504b471a0001000200200000003e00000099ebeade00000000000000000d006d616e69666573742e6a736f6e5e000000170000001"
		    "8f7d2d7150070726f6772616d2f65737033322d726973637633327500000007000000c50f58517b226964223a2264656d6f222c22616269223a2278227d7f454c46010203";
		const std::string golden =
		    packageWith({{"manifest.json", "{\"id\":\"demo\",\"abi\":\"x\"}"}, {"program/esp32-riscv32", std::string("\x7f" "ELF\x01\x02\x03", 7)}});
		static const char kHex[] = "0123456789abcdef";
		std::string hex;
		for (const char c : golden) {
			hex += kHex[static_cast<unsigned char>(c) >> 4];
			hex += kHex[static_cast<unsigned char>(c) & 0xF];
		}
		CHECK(hex == kGoldenHex);
		CHECK(openPackage(golden, e));
	}

	// Corrupt entry data: the index is fine, the read is refused.
	{
		std::string corrupt = data;
		corrupt[corrupt.size() - 1] ^= 0x01;  // inside the last non-empty entry
		PackageReader r;
		CHECK(r.open(memFile(corrupt), e));
		std::string icon;
		CHECK(!r.read("icon.png", icon, e));
		CHECK_CONTAINS(e, "corrupt");
	}

	// Every truncation fails cleanly (and ASan sees no overread).
	for (std::size_t length = 0; length < data.size(); length++) {
		PackageReader r;
		std::string err;
		if (r.open(memFile(data.substr(0, length)), err)) {
			// Only possible once header and index fit; then a cut entry must fail its read.
			std::string body;
			bool allRead = true;
			for (const PackageEntry &entry : r.entries()) allRead = allRead && r.read(entry, body, err);
			CHECK(!allRead);
		}
	}

	{
		std::string bad = data;
		bad[0] = 'X';
		CHECK(!openPackage(bad, e));
		CHECK_CONTAINS(e, "not a .geapp");
	}
	{
		std::string bad = data;
		poke16(bad, 8, 2);
		CHECK(!openPackage(bad, e));
		CHECK_CONTAINS(e, "unsupported package format");
	}
	{
		std::string bad = data;
		bad[30] = 1;
		CHECK(!openPackage(bad, e));
		CHECK_CONTAINS(e, "reserved");
	}
	{
		std::string bad = data;
		bad[33] ^= 0x40;  // inside the index: the CRC catches it
		CHECK(!openPackage(bad, e));
		CHECK_CONTAINS(e, "corrupt");
	}
	{
		std::string bad = data;
		poke16(bad, 10, static_cast<std::uint16_t>(kMaxPackageEntries + 1));
		CHECK(!openPackage(bad, e));
	}
	{
		std::string bad = data;
		poke32(bad, 16, 0xFFFFFF00u);  // index size beyond the file
		CHECK(!openPackage(bad, e));
		CHECK_CONTAINS(e, "out of bounds");
	}
	{
		std::string bad = data;
		poke32(bad, 12, 4);  // index inside the header
		CHECK(!openPackage(bad, e));
	}

	// Hand-built indexes the writer would never produce.
	const auto build = [](const std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>> &entries, std::size_t payload) {
		std::string index;
		for (const auto &[name, offset, size] : entries) {
			index += static_cast<char>(name.size() & 0xFF);
			index += static_cast<char>(name.size() >> 8);
			index += name;
			for (std::uint32_t v : {offset, size, 0u})
				for (int i = 0; i < 4; i++) index += static_cast<char>((v >> (8 * i)) & 0xFF);
		}
		std::string out("GEAPKG\x1a", 7);
		out += '\0';
		out += '\x01';
		out += '\0';
		out += static_cast<char>(entries.size());
		out += '\0';
		std::string tail(20, '\0');
		out += tail;
		poke32(out, 12, 32);
		poke32(out, 16, static_cast<std::uint32_t>(index.size()));
		out += index;
		out += std::string(payload, 'p');
		fixIndexCrc(out);
		return out;
	};
	{
		const std::string dup = build({{"a", 200, 0}, {"a", 200, 0}}, 300);
		CHECK(!openPackage(dup, e));
		CHECK_CONTAINS(e, "duplicate");
	}
	for (const char *name : {"../evil", "/abs", "a//b", "a/../b", "", "a\\b"}) {
		const std::string unsafe = build({{name, 100, 0}}, 200);
		CHECK(!openPackage(unsafe, e));
	}
	{
		const std::string oob = build({{"a", 100, 100000}}, 50);
		CHECK(!openPackage(oob, e));
		CHECK_CONTAINS(e, "out of bounds");
	}
	{
		const std::string overlapIndex = build({{"a", 32, 10}}, 100);
		CHECK(!openPackage(overlapIndex, e));
		CHECK_CONTAINS(e, "overlaps");
	}
	{
		const std::string overlap = build({{"a", 100, 50}, {"b", 120, 50}}, 300);
		CHECK(!openPackage(overlap, e));
		CHECK_CONTAINS(e, "overlap");
	}
	{
		const std::string wrapAround = build({{"a", 0xFFFFFFF0u, 0x20}}, 100);
		CHECK(!openPackage(wrapAround, e));
	}

	// Deterministic fuzz: random byte flips never crash, and whatever opens
	// reads only within bounds.
	std::uint32_t seed = 12345;
	const auto next = [&seed] {
		seed = seed * 1103515245u + 12345u;
		return seed >> 8;
	};
	for (int round = 0; round < 3000; round++) {
		std::string mutated = data;
		const int flips = 1 + static_cast<int>(next() % 4);
		for (int f = 0; f < flips; f++) mutated[next() % mutated.size()] = static_cast<char>(next());
		if (next() % 2) fixIndexCrc(mutated);  // half the time, get past the CRC
		PackageReader r;
		std::string err;
		if (r.open(memFile(mutated), err)) {
			std::string body;
			for (const PackageEntry &entry : r.entries()) r.read(entry, body, err);
		}
	}

	PackageWriter writer;
	CHECK(writer.add("a", "1"));
	CHECK(!writer.add("a", "2"));
	CHECK(!writer.add("../a", "x"));
	CHECK(!writer.add("", "x"));
	CHECK(programEntryName("esp32-riscv32") == "program/esp32-riscv32");

	for (const char *safe : {"a", "a/b", "program/esp32-riscv32", "assets/img/x.png", "a.b", "...x"}) CHECK(isSafeEntryName(safe));
	for (const char *unsafe : {"", "/a", "a/", "a//b", "./a", "a/.", "..", "a/../b", "a\\b"}) CHECK(!isSafeEntryName(unsafe));
	CHECK(!isSafeEntryName(std::string("a\0b", 3)));
	CHECK(isSafeEntryName(std::string(kMaxPathLength, 'a')));
	CHECK(!isSafeEntryName(std::string(kMaxPathLength + 1, 'a')));

	CHECK(crc32("123456789", 9) == 0xCBF43926u);  // the standard check value
	CHECK(crc32("", 0) == 0);
	CHECK(crc32("56789", 5, crc32("1234", 4)) == 0xCBF43926u);  // streaming
}

// ---- Registry --------------------------------------------------------------

constexpr const char *kTarget = "esp32-riscv32";

void putFolderApp(MemFs &fs, const std::string &root, const std::string &id, const std::string &manifest, bool legacyProgram = false)
{
	fs.put(root + "/apps/" + id + "/manifest.json", manifest);
	fs.put(root + "/apps/" + id + (legacyProgram ? "/program.so" : std::string("/program/") + kTarget), "ELF");
}

const InstalledApp *findApp(const ScanResult &r, const std::string &id)
{
	for (const InstalledApp &app : r.apps)
		if (app.manifest.id == id) return &app;
	return nullptr;
}

bool brokenWith(const ScanResult &r, const std::string &locationSuffix, const std::string &reason)
{
	for (const BrokenApp &b : r.broken)
		if (b.location.size() >= locationSuffix.size() &&
		    b.location.compare(b.location.size() - locationSuffix.size(), locationSuffix.size(), locationSuffix) == 0 &&
		    b.reason.find(reason) != std::string::npos)
			return true;
	return false;
}

void testRegistry()
{
	MemFs fs;
	const Volume nand{"nand", "/nand", false, true};
	const Volume sd{"sdcard", "/sdcard", true, false};

	putFolderApp(fs, "/nand", "neon", manifestJson("neon", ",\"version\":\"1.0.0\""));
	putFolderApp(fs, "/nand", "ember", manifestJson("ember"), true);  // legacy program.so
	fs.put("/nand/apps/ember/icon.png", "PNG");
	putFolderApp(fs, "/nand", "nimbus", manifestJson("nimbus", ",\"icon\":\"art/i.png\""));
	fs.put("/nand/apps/nimbus/art/i.png", "PNG");
	fs.put("/nand/apps/noprog/manifest.json", manifestJson("noprog"));
	fs.put("/nand/apps/noman/program.so", "ELF");
	putFolderApp(fs, "/nand", "badjson", "{oops");
	putFolderApp(fs, "/nand", "mismatch", manifestJson("other"));
	putFolderApp(fs, "/nand", "Bad_Name", manifestJson("bad_name"));
	fs.put("/nand/apps/notes.txt", "not an app");
	fs.put("/nand/apps/.hidden/manifest.json", manifestJson("hidden"));
	fs.put("/nand/apps/huge/manifest.json", "{\"id\":\"huge\",\"pad\":\"" + std::string(kMaxManifestBytes + 10, ' ') + "\"}");
	fs.put("/nand/apps/huge/program.so", "ELF");

	// Packages on the card.
	fs.put("/sdcard/apps/wander.geapp",
	       packageWith({{"manifest.json", "{\"id\":\"wander\",\"name\":\"Wander\",\"abi\":\"abi1\"}"}, {std::string("program/") + kTarget, "ELF"}}));
	fs.put("/sdcard/apps/otheronly.geapp", packageWith({{"manifest.json", manifestJson("otheronly")}, {"program/macos-arm64", "MACHO"}}));
	fs.put("/sdcard/apps/nomanifest.geapp", packageWith({{std::string("program/") + kTarget, "ELF"}}));
	fs.put("/sdcard/apps/garbage.geapp", "this is not a package");
	fs.put("/sdcard/apps/Bad.geapp", packageWith({{"manifest.json", manifestJson("bad")}}));
	// Same id on both volumes: the card has a newer neon.
	putFolderApp(fs, "/sdcard", "neon", manifestJson("neon", ",\"version\":\"1.1.0\""));
	// Same version on both: the earlier volume wins.
	putFolderApp(fs, "/sdcard", "ember", manifestJson("ember"));

	ScanResult r = scan({nand, sd}, fs, kTarget);

	CHECK(findApp(r, "neon") && findApp(r, "neon")->volumeId == "sdcard");     // newer wins
	CHECK(findApp(r, "ember") && findApp(r, "ember")->volumeId == "nand");     // tie: first volume
	CHECK(findApp(r, "ember") && findApp(r, "ember")->iconPath == "/nand/apps/ember/icon.png");
	CHECK(findApp(r, "nimbus") && findApp(r, "nimbus")->iconPath == "/nand/apps/nimbus/art/i.png");
	CHECK(findApp(r, "neon") && findApp(r, "neon")->iconPath.empty());
	CHECK(findApp(r, "wander") && findApp(r, "wander")->layout == AppLayout::Package);
	CHECK(findApp(r, "wander") && findApp(r, "wander")->manifest.name == "Wander");
	CHECK(!findApp(r, "hidden") && !findApp(r, "otheronly") && !findApp(r, "noprog"));
	CHECK(r.shadowed.size() == 2);

	CHECK(brokenWith(r, "/noprog", "no program for esp32-riscv32"));
	CHECK(brokenWith(r, "/noman", "no manifest.json"));
	CHECK(brokenWith(r, "/badjson", "not valid JSON"));
	CHECK(brokenWith(r, "/mismatch", "does not match"));
	CHECK(brokenWith(r, "/Bad_Name", "not a valid app id"));
	CHECK(brokenWith(r, "/huge", "larger than"));
	CHECK(brokenWith(r, "/otheronly.geapp", "no program for esp32-riscv32"));
	CHECK(brokenWith(r, "/nomanifest.geapp", "no manifest.json"));
	CHECK(brokenWith(r, "/garbage.geapp", "not a .geapp"));
	CHECK(brokenWith(r, "/Bad.geapp", "not a valid app id"));
	CHECK(r.broken.size() == 10);

	// Sorted by name, then id.
	for (std::size_t i = 1; i < r.apps.size(); i++) {
		const Manifest &a = r.apps[i - 1].manifest, &b = r.apps[i].manifest;
		CHECK(a.name < b.name || (a.name == b.name && a.id < b.id));
	}

	// The program for this target is preferred over the legacy name.
	fs.put("/nand/apps/neon/program.so", "OLD");
	CHECK(folderProgramPath(fs, "/nand/apps/neon", kTarget) == std::string("/nand/apps/neon/program/") + kTarget);
	CHECK(folderProgramPath(fs, "/nand/apps/ember", kTarget) == "/nand/apps/ember/program.so");
	CHECK(folderProgramPath(fs, "/nand/apps/noman", "").empty() == false);

	// A missing apps folder or a missing volume is no apps, not an error.
	MemFs empty;
	ScanResult none = scan({nand, sd}, empty, kTarget);
	CHECK(none.apps.empty() && none.broken.empty() && none.shadowed.empty());
	CHECK(scan({}, fs, kTarget).apps.empty());

	// A root declared twice is scanned once.
	MemFs once;
	putFolderApp(once, "/nand", "solo", manifestJson("solo"));
	const int before = once.listCalls;
	ScanResult twice = scan({nand, Volume{"alias", "/nand"}}, once, kTarget);
	CHECK(twice.apps.size() == 1 && twice.shadowed.empty());
	CHECK(once.listCalls - before == 1);

	// A folder and a package of the same id and version on one volume: the folder.
	MemFs both;
	putFolderApp(both, "/v", "dual", manifestJson("dual"));
	both.put("/v/apps/dual.geapp", packageWith({{"manifest.json", manifestJson("dual")}, {std::string("program/") + kTarget, "E"}}));
	ScanResult d = scan({Volume{"v", "/v"}}, both, kTarget);
	CHECK(d.apps.size() == 1 && d.apps[0].layout == AppLayout::Folder && d.shadowed.size() == 1);

	CHECK(appsDirectory(Volume{"x", "/"}) == "/apps");
	CHECK(appsDirectory(Volume{"x", "/sd/"}) == "/sd/apps");
	CHECK(appsDirectory(Volume{"x", "/sd"}) == "/sd/apps");

	std::string e;
	Manifest m;
	m.name = "Neon";
	CHECK(!checkCompatible(m, "abc", e));
	CHECK_CONTAINS(e, "does not say");
	m.abi = "def";
	CHECK(!checkCompatible(m, "abc", e));
	CHECK_CONTAINS(e, "built for firmware ABI def");
	m.abi = "abc";
	CHECK(checkCompatible(m, "abc", e));
}

// ---- Launcher --------------------------------------------------------------

class FakeHost final : public AppHost {
public:
	std::string abiValue = "abi1";
	std::string running;
	std::string failWith;
	int launches = 0;
	std::vector<InstalledApp> bundled;
	const char *targetKey() const override { return kTarget; }
	const char *abi() const override { return abiValue.c_str(); }
	bool launch(const InstalledApp &app, std::string &error) override
	{
		launches++;
		if (!failWith.empty()) {
			error = failWith;
			return false;
		}
		running = app.manifest.id;
		return true;
	}
	bool home(std::string &error) override
	{
		if (!failWith.empty()) {
			error = failWith;
			return false;
		}
		running.clear();
		return true;
	}
	const char *runningAppId() const override { return running.c_str(); }
	std::vector<InstalledApp> bundledApps() const override { return bundled; }
};

// The host contract check passes a well-behaved host and names each rule a
// broken one breaks.
class ContractHost final : public AppHost {
public:
	std::string key = "esp32-riscv32";
	std::string abiValue = "abi1";
	std::string running;
	bool homeSucceeds = false;
	std::string homeError = "no app is running";
	std::vector<InstalledApp> bundled;
	const char *targetKey() const override { return key.c_str(); }
	const char *abi() const override { return abiValue.c_str(); }
	bool launch(const InstalledApp &, std::string &) override { return false; }
	bool home(std::string &error) override
	{
		if (homeSucceeds) return true;
		error = homeError;
		return false;
	}
	const char *runningAppId() const override { return running.c_str(); }
	std::vector<InstalledApp> bundledApps() const override { return bundled; }
};

InstalledApp bundledApp(const std::string &id, const std::string &abi, const std::string &name = "App")
{
	InstalledApp app;
	app.manifest.id = id;
	app.manifest.name = name;
	app.manifest.abi = abi;
	return app;
}

void testHostContract()
{
	std::string failures;
	ContractHost good;
	good.bundled = {bundledApp("neon", "abi1"), bundledApp("ember", "abi1")};
	CHECK(checkHostContract(good, failures) && failures.empty());
	ContractHost inApp;
	inApp.running = "neon";
	inApp.homeSucceeds = true;  // home() is not probed while an app runs
	CHECK(checkHostContract(inApp, failures) && failures.empty());

	auto broken = [&](ContractHost &host, const char *rule) {
		std::string out;
		CHECK(!checkHostContract(host, out));
		CHECK_CONTAINS(out, rule);
	};
	ContractHost noKey;
	noKey.key = "";
	broken(noKey, "targetKey()");
	ContractHost badKey;
	badKey.key = "../x";
	broken(badKey, "package entry name");
	ContractHost noAbi;
	noAbi.abiValue = "";
	broken(noAbi, "abi() must not be empty");
	ContractHost badRunning;
	badRunning.running = "Not An Id";
	broken(badRunning, "not a valid app id");
	ContractHost dupes;
	dupes.bundled = {bundledApp("neon", "abi1"), bundledApp("neon", "abi1")};
	broken(dupes, "listed twice");
	ContractHost badId;
	badId.bundled = {bundledApp("Neon!", "abi1")};
	broken(badId, "invalid id");
	ContractHost wrongAbi;
	wrongAbi.bundled = {bundledApp("neon", "abi2")};
	broken(wrongAbi, "host's ABI");
	ContractHost noName;
	noName.bundled = {bundledApp("neon", "abi1", "")};
	broken(noName, "has no name");
	ContractHost homeFromHome;
	homeFromHome.homeSucceeds = true;
	broken(homeFromHome, "must refuse while Home");
	ContractHost silent;
	silent.homeError = "";
	broken(silent, "say why");

	// The launcher's own test host keeps the contract too.
	FakeHost fake;
	fake.failWith = "no app is running";
	CHECK(checkHostContract(fake, failures) && failures.empty());
}

void testLauncher()
{
	MemFs fs;
	Launcher::reset(&fs);
	CHECK(!Launcher::launch("neon"));
	CHECK_CONTAINS(Launcher::lastError(), "cannot run apps");
	CHECK(!Launcher::home());
	CHECK(std::string(Launcher::runningAppId()).empty());
	CHECK(!Launcher::installVolume());

	FakeHost host;
	Launcher::setHost(&host);
	Launcher::addVolume({"nand", "/nand", false, false});
	Launcher::addVolume({"sdcard", "/sdcard", true, true});
	CHECK(Launcher::installVolume() && Launcher::installVolume()->id == "sdcard");
	Launcher::addVolume({"nand", "/nand2", false, false});  // same id replaces
	CHECK(Launcher::volumes().size() == 2 && Launcher::volumes()[0].root == "/nand2");
	Launcher::addVolume({"nand", "/nand", false, false});

	CHECK(!Launcher::rescan());  // nothing yet, nothing changed
	putFolderApp(fs, "/nand", "neon", manifestJson("neon"));
	putFolderApp(fs, "/nand", "old", manifestJson("old", "", "abi0"));
	putFolderApp(fs, "/nand", "noabi", "{\"id\":\"noabi\"}");
	CHECK(Launcher::rescan());
	CHECK(!Launcher::rescan());  // unchanged
	CHECK(Launcher::apps().apps.size() == 3);
	CHECK(Launcher::find("neon").has_value() && !Launcher::find("nope"));

	CHECK(!Launcher::launch("nope"));
	CHECK_CONTAINS(Launcher::lastError(), "no installed app");
	CHECK(!Launcher::launch("old"));
	CHECK_CONTAINS(Launcher::lastError(), "built for firmware ABI abi0");
	CHECK(!Launcher::launch("noabi"));
	CHECK_CONTAINS(Launcher::lastError(), "does not say");
	CHECK(host.launches == 0);  // refused before the host was asked

	CHECK(Launcher::launch("neon"));
	CHECK(Launcher::lastError().empty());
	CHECK(std::string(Launcher::runningAppId()) == "neon");
	CHECK(Launcher::home());
	CHECK(std::string(Launcher::runningAppId()).empty());

	// Rebuilt for this firmware after the last scan: launch reads storage
	// again rather than refusing it from the stale list.
	putFolderApp(fs, "/nand", "old", manifestJson("old"));
	CHECK(Launcher::launch("old"));
	CHECK(std::string(Launcher::runningAppId()) == "old");
	CHECK(Launcher::home());
	// Removed after the last scan: refused as not installed.
	fs.files.erase("/nand/apps/old/manifest.json");
	CHECK(!Launcher::launch("old"));
	CHECK_CONTAINS(Launcher::lastError(), "no installed app");

	host.failWith = "out of PSRAM";
	CHECK(!Launcher::launch("neon"));
	CHECK(Launcher::lastError() == "out of PSRAM");
	CHECK(!Launcher::home());
	host.failWith.clear();

	// A card with a newer app arrives, then is taken out.
	putFolderApp(fs, "/sdcard", "card", manifestJson("card"));
	CHECK(Launcher::rescan());
	CHECK(Launcher::find("card").has_value());
	Launcher::removeVolume("sdcard");
	CHECK(Launcher::rescan());
	CHECK(!Launcher::find("card"));
	Launcher::removeVolume("missing");  // no-op

	// A broken app appearing is a change too (Settings lists it).
	fs.put("/nand/apps/broken/manifest.json", "{");
	CHECK(Launcher::rescan());

	// Bundled apps (Apple) join the list.
	InstalledApp bundled;
	bundled.manifest.id = "bundled";
	bundled.manifest.name = "Bundled";
	bundled.manifest.abi = "abi1";
	host.bundled.push_back(bundled);
	CHECK(Launcher::rescan());
	CHECK(Launcher::launch("bundled"));

	// Concurrent scans, launches and reads: no data race crashes (ASan), and the
	// list is always whole.
	std::atomic<bool> stop{false};
	std::thread scanner([&] {
		while (!stop) Launcher::rescan();
	});
	std::thread reader([&] {
		while (!stop) {
			ScanResult snapshot = Launcher::apps();
			for (const InstalledApp &app : snapshot.apps) CHECK(!app.manifest.id.empty());
			Launcher::find("neon");
			Launcher::lastError();
		}
	});
	for (int i = 0; i < 2000; i++) {
		Launcher::launch(i % 2 ? "neon" : "nope");
		if (i % 100 == 0) Launcher::addVolume({"sdcard", "/sdcard", true, true});
		if (i % 100 == 50) Launcher::removeVolume("sdcard");
	}
	stop = true;
	scanner.join();
	reader.join();

	Launcher::reset();
}

// ---- The POSIX filesystem ----------------------------------------------------

void writeFile(const std::string &path, const std::string &data)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	std::fwrite(data.data(), 1, data.size(), f);
	std::fclose(f);
}

void testPosixFs(const std::string &scratch)
{
	const std::string root = scratch + "/geaos-posix-test";
	std::system(("rm -rf '" + root + "'").c_str());
	::mkdir(root.c_str(), 0755);
	::mkdir((root + "/apps").c_str(), 0755);
	::mkdir((root + "/apps/neon").c_str(), 0755);
	::mkdir((root + "/apps/neon/program").c_str(), 0755);
	writeFile(root + "/apps/neon/manifest.json", manifestJson("neon"));
	writeFile(root + "/apps/neon/program/" + kTarget, "ELF");
	writeFile(root + "/apps/._neon", "macOS resource fork");
	writeFile(root + "/apps/wander.geapp", packageWith({{"manifest.json", manifestJson("wander")}, {std::string("program/") + kTarget, "E"}}));

	ScanResult r = scan({Volume{"disk", root}}, posixFs(), kTarget);
	CHECK(r.apps.size() == 2 && r.broken.empty());
	CHECK(findApp(r, "neon") && findApp(r, "wander"));

	Fs &fs = posixFs();
	std::string text;
	CHECK(fs.readFile(root + "/apps/neon/manifest.json", text, 1024) && text == manifestJson("neon"));
	CHECK(!fs.readFile(root + "/apps/neon/manifest.json", text, 4));  // over the limit
	CHECK(!fs.open(root + "/apps/neon"));                               // a folder is not a file
	CHECK(!fs.open(root + "/missing"));
	auto file = fs.open(root + "/apps/neon/manifest.json");
	char buffer[8];
	CHECK(file && !file->readAt(file->size() - 2, buffer, 4));  // past the end
	std::vector<DirEntry> entries;
	CHECK(!fs.listDir(root + "/missing", entries));

	std::system(("rm -rf '" + root + "'").c_str());
}

}  // namespace

int main(int argc, char **argv)
{
	const std::string scratch = argc > 1 ? argv[1] : ".";
	testJson();
	testVersionAndId();
	testManifest();
	testPackage();
	testRegistry();
	testHostContract();
	testLauncher();
	testPosixFs(scratch);
	if (g_failures) {
		std::fprintf(stderr, "geaos app model: %d of %d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("geaos app model: %d checks passed\n", g_checks);
	return 0;
}
