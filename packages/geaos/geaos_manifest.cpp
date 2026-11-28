// SPDX-License-Identifier: Apache-2.0
#include "geaos.h"
#include "geaos_json.h"

namespace gea::geaos {

namespace {

struct PermissionName {
	const char *name;
	std::uint32_t bit;
};

constexpr PermissionName kPermissions[] = {
    {"network", kPermissionNetwork}, {"bluetooth", kPermissionBluetooth}, {"storage", kPermissionStorage},
    {"audio", kPermissionAudio},     {"camera", kPermissionCamera},       {"location", kPermissionLocation},
};

bool hasControlCharacters(std::string_view text)
{
	for (const char c : text)
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return true;
	return false;
}

// Reads an optional string member. False (with the error set) if present
// but not a string.
bool optionalString(const json::Value &root, const char *key, std::string &out, std::string &error)
{
	const json::Value *value = root.get(key);
	if (!value || value->kind == json::Value::Kind::Null) return true;
	if (value->kind != json::Value::Kind::String) {
		error = std::string("\"") + key + "\" must be a string";
		return false;
	}
	out = value->string;
	return true;
}

bool parsePart(std::string_view text, std::uint32_t &out)
{
	if (text.empty() || text.size() > 9) return false;
	if (text.size() > 1 && text[0] == '0') return false;
	out = 0;
	for (const char c : text) {
		if (c < '0' || c > '9') return false;
		out = out * 10 + static_cast<std::uint32_t>(c - '0');
	}
	return true;
}

}  // namespace

bool Version::parse(std::string_view text, Version &out)
{
	Version version;
	std::uint32_t *parts[] = {&version.major, &version.minor, &version.patch};
	std::size_t index = 0;
	while (true) {
		if (index >= 3) return false;
		const std::size_t dot = text.find('.');
		if (!parsePart(text.substr(0, dot), *parts[index])) return false;
		index++;
		if (dot == std::string_view::npos) break;
		text.remove_prefix(dot + 1);
	}
	out = version;
	return true;
}

int Version::compare(const Version &other) const
{
	if (major != other.major) return major < other.major ? -1 : 1;
	if (minor != other.minor) return minor < other.minor ? -1 : 1;
	if (patch != other.patch) return patch < other.patch ? -1 : 1;
	return 0;
}

std::string Version::toString() const
{
	return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

bool isValidAppId(std::string_view id)
{
	if (id.empty() || id.size() > kMaxIdLength) return false;
	const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
	if (!alnum(id.front()) || id.back() == '.') return false;
	for (const char c : id)
		if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
	return true;
}

bool parseManifest(std::string_view text, Manifest &out, std::string &error, std::string_view fallbackId)
{
	error.clear();
	if (text.size() > kMaxManifestBytes) {
		error = "manifest is larger than " + std::to_string(kMaxManifestBytes) + " bytes";
		return false;
	}
	json::Value root;
	std::string parseError;
	if (!json::parse(text, root, parseError)) {
		error = "manifest is not valid JSON: " + parseError;
		return false;
	}
	if (root.kind != json::Value::Kind::Object) {
		error = "manifest must be a JSON object";
		return false;
	}

	Manifest manifest;
	if (!optionalString(root, "id", manifest.id, error)) return false;
	if (manifest.id.empty()) manifest.id = std::string(fallbackId);
	if (manifest.id.empty()) {
		error = "manifest has no \"id\"";
		return false;
	}
	if (!isValidAppId(manifest.id)) {
		error = "invalid app id \"" + manifest.id + "\"";
		return false;
	}
	if (!fallbackId.empty() && manifest.id != fallbackId) {
		error = "manifest id \"" + manifest.id + "\" does not match \"" + std::string(fallbackId) + "\"";
		return false;
	}

	if (!optionalString(root, "name", manifest.name, error)) return false;
	if (manifest.name.empty()) manifest.name = manifest.id;
	if (manifest.name.size() > kMaxNameBytes) {
		error = "\"name\" is longer than " + std::to_string(kMaxNameBytes) + " bytes";
		return false;
	}
	if (hasControlCharacters(manifest.name)) {
		error = "\"name\" contains control characters";
		return false;
	}

	std::string version;
	if (!optionalString(root, "version", version, error)) return false;
	if (!version.empty() && !Version::parse(version, manifest.version)) {
		error = "invalid \"version\" \"" + version + "\"";
		return false;
	}

	if (!optionalString(root, "abi", manifest.abi, error)) return false;
	if (manifest.abi.size() > 64 || hasControlCharacters(manifest.abi)) {
		error = "invalid \"abi\"";
		return false;
	}

	if (!optionalString(root, "icon", manifest.icon, error)) return false;
	if (!manifest.icon.empty() && !isSafeEntryName(manifest.icon)) {
		error = "invalid \"icon\" path \"" + manifest.icon + "\"";
		return false;
	}

	std::string present;
	if (!optionalString(root, "present", present, error)) return false;
	if (present.empty() || present == "sync") {
		manifest.present = PresentMode::Sync;
	} else if (present == "async") {
		manifest.present = PresentMode::Async;
	} else {
		error = "\"present\" must be \"sync\" or \"async\"";
		return false;
	}

	if (const json::Value *permissions = root.get("permissions")) {
		if (permissions->kind != json::Value::Kind::Array) {
			error = "\"permissions\" must be an array";
			return false;
		}
		for (const json::Value &item : permissions->items) {
			if (item.kind != json::Value::Kind::String) {
				error = "\"permissions\" entries must be strings";
				return false;
			}
			std::uint32_t bit = 0;
			for (const PermissionName &known : kPermissions)
				if (item.string == known.name) bit = known.bit;
			if (!bit) {
				// An unknown permission is refused, not dropped: an app must
				// not run without something it says it needs.
				error = "unknown permission \"" + item.string + "\"";
				return false;
			}
			manifest.permissions |= bit;
		}
	}

	if (const json::Value *pin = root.get("pinAssets")) {
		if (pin->kind != json::Value::Kind::Bool) {
			error = "\"pinAssets\" must be true or false";
			return false;
		}
		manifest.pinAssets = pin->boolean;
	}

	if (const json::Value *sizes = root.get("programBytes")) {
		if (sizes->kind != json::Value::Kind::Object) {
			error = "\"programBytes\" must be an object of target: bytes";
			return false;
		}
		for (const auto &[target, value] : sizes->members) {
			if (value.kind != json::Value::Kind::Number || value.number < 0 || value.number > 4294967295.0 ||
			    value.number != static_cast<double>(static_cast<std::uint64_t>(value.number))) {
				error = "\"programBytes\" for " + target + " must be a whole number of bytes";
				return false;
			}
			manifest.programBytes.emplace_back(target, static_cast<std::uint64_t>(value.number));
		}
	}

	if (!optionalString(root, "developer", manifest.developer, error)) return false;
	if (!optionalString(root, "signature", manifest.signature, error)) return false;

	out = std::move(manifest);
	return true;
}

std::string writeManifest(const Manifest &manifest)
{
	std::string out = "{\"id\":";
	json::appendQuoted(out, manifest.id);
	out += ",\"name\":";
	json::appendQuoted(out, manifest.name);
	out += ",\"version\":";
	json::appendQuoted(out, manifest.version.toString());
	out += ",\"abi\":";
	json::appendQuoted(out, manifest.abi);
	if (!manifest.icon.empty()) {
		out += ",\"icon\":";
		json::appendQuoted(out, manifest.icon);
	}
	out += ",\"present\":";
	out += manifest.present == PresentMode::Async ? "\"async\"" : "\"sync\"";
	out += ",\"permissions\":[";
	bool first = true;
	for (const PermissionName &known : kPermissions) {
		if (!(manifest.permissions & known.bit)) continue;
		if (!first) out += ',';
		first = false;
		json::appendQuoted(out, known.name);
	}
	out += ']';
	if (manifest.pinAssets) out += ",\"pinAssets\":true";
	if (!manifest.programBytes.empty()) {
		out += ",\"programBytes\":{";
		for (std::size_t i = 0; i < manifest.programBytes.size(); i++) {
			if (i) out += ',';
			json::appendQuoted(out, manifest.programBytes[i].first);
			out += ':';
			out += std::to_string(manifest.programBytes[i].second);
		}
		out += '}';
	}
	if (!manifest.developer.empty()) {
		out += ",\"developer\":";
		json::appendQuoted(out, manifest.developer);
	}
	if (!manifest.signature.empty()) {
		out += ",\"signature\":";
		json::appendQuoted(out, manifest.signature);
	}
	out += '}';
	return out;
}

}  // namespace gea::geaos
