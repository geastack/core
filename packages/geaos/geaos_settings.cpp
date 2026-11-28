// SPDX-License-Identifier: Apache-2.0
// GeaOS per-device settings and the system services they apply to.
#include "geaos.h"
#include "geaos_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gea::geaos {

namespace {

constexpr std::size_t kMaxSettingsBytes = 64 * 1024;

std::string permissionsKey(std::string_view appId)
{
	std::string key = "apps.";
	key.append(appId);
	key += ".permissions";
	return key;
}

std::vector<std::string> splitList(const std::string &text)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t comma = text.find(',', start);
		const std::size_t end = comma == std::string::npos ? text.size() : comma;
		if (end > start) out.emplace_back(text.substr(start, end - start));
		if (comma == std::string::npos) break;
		start = comma + 1;
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::string joinList(const std::vector<std::string> &items)
{
	std::string out;
	for (const auto &item : items) {
		if (!out.empty()) out += ',';
		out += item;
	}
	return out;
}

std::string formatNumber(double value)
{
	char buffer[40];
	if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e15)
		std::snprintf(buffer, sizeof(buffer), "%.0f", value);
	else if (std::isfinite(value))
		std::snprintf(buffer, sizeof(buffer), "%.17g", value);
	else
		std::snprintf(buffer, sizeof(buffer), "0");
	return buffer;
}

class NoServices final : public SystemServices {};

NoServices g_noServices;
SystemServices *g_services = &g_noServices;
Settings g_deviceSettings;

}  // namespace

// ---- Settings --------------------------------------------------------------

Settings::Entry *Settings::find(std::string_view key)
{
	auto it = std::lower_bound(values_.begin(), values_.end(), key, [](const auto &entry, std::string_view k) { return entry.first < k; });
	return it != values_.end() && it->first == key ? &it->second : nullptr;
}

const Settings::Entry *Settings::find(std::string_view key) const
{
	auto it = std::lower_bound(values_.begin(), values_.end(), key, [](const auto &entry, std::string_view k) { return entry.first < k; });
	return it != values_.end() && it->first == key ? &it->second : nullptr;
}

Settings::Entry &Settings::slot(std::string_view key)
{
	auto it = std::lower_bound(values_.begin(), values_.end(), key, [](const auto &entry, std::string_view k) { return entry.first < k; });
	if (it != values_.end() && it->first == key) return it->second;
	return values_.insert(it, {std::string(key), Entry{}})->second;
}

bool Settings::load(Fs &fs, const std::string &path, std::string *error)
{
	path_ = path;
	values_.clear();
	if (!fs.exists(path)) return true;
	std::string text;
	auto fail = [&](const std::string &why) {
		values_.clear();
		if (error) *error = why;
		return false;
	};
	if (!fs.readFile(path, text, kMaxSettingsBytes)) return fail("settings file unreadable or too large");
	json::Value root;
	std::string parseError;
	if (!json::parse(text, root, parseError)) return fail("settings file damaged: " + parseError);
	if (root.kind != json::Value::Kind::Object) return fail("settings file is not a JSON object");
	for (const auto &[key, value] : root.members) {
		if (key.empty()) continue;
		Entry entry;
		switch (value.kind) {
		case json::Value::Kind::Bool:
			entry.kind = Entry::Kind::Bool;
			entry.boolean = value.boolean;
			break;
		case json::Value::Kind::Number:
			entry.kind = Entry::Kind::Number;
			entry.number = value.number;
			break;
		case json::Value::Kind::String:
			entry.kind = Entry::Kind::String;
			entry.string = value.string;
			break;
		default:
			continue;  // a value this version does not keep
		}
		slot(key) = std::move(entry);
	}
	return true;
}

bool Settings::save(Fs &fs) const
{
	if (path_.empty()) return false;
	std::string text = "{\n";
	bool first = true;
	for (const auto &[key, entry] : values_) {
		if (!first) text += ",\n";
		first = false;
		text += "  ";
		json::appendQuoted(text, key);
		text += ": ";
		switch (entry.kind) {
		case Entry::Kind::Bool: text += entry.boolean ? "true" : "false"; break;
		case Entry::Kind::Number: text += formatNumber(entry.number); break;
		case Entry::Kind::String: json::appendQuoted(text, entry.string); break;
		}
	}
	text += first ? "}\n" : "\n}\n";

	const std::size_t slash = path_.rfind('/');
	if (slash != std::string::npos && slash > 0) fs.makeDir(path_.substr(0, slash));
	const std::string temporary = path_ + ".tmp";
	auto file = fs.create(temporary);
	if (!file) return false;
	if (!file->write(text.data(), text.size()) || !file->finish()) {
		file.reset();
		fs.remove(temporary);
		return false;
	}
	file.reset();
	// FAT will not rename over a file: the old one goes first. A cut between
	// the two leaves only .tmp, which load() does not read -- the settings
	// fall back to defaults, never to a half-written file.
	if (fs.exists(path_) && !fs.remove(path_)) {
		fs.remove(temporary);
		return false;
	}
	return fs.rename(temporary, path_);
}

bool Settings::has(std::string_view key) const { return find(key) != nullptr; }

bool Settings::getBool(std::string_view key, bool fallback) const
{
	const Entry *entry = find(key);
	return entry && entry->kind == Entry::Kind::Bool ? entry->boolean : fallback;
}

double Settings::getNumber(std::string_view key, double fallback) const
{
	const Entry *entry = find(key);
	return entry && entry->kind == Entry::Kind::Number ? entry->number : fallback;
}

std::string Settings::getString(std::string_view key, std::string_view fallback) const
{
	const Entry *entry = find(key);
	return entry && entry->kind == Entry::Kind::String ? entry->string : std::string(fallback);
}

void Settings::setBool(std::string_view key, bool value)
{
	Entry &entry = slot(key);
	entry = Entry{};
	entry.kind = Entry::Kind::Bool;
	entry.boolean = value;
}

void Settings::setNumber(std::string_view key, double value)
{
	Entry &entry = slot(key);
	entry = Entry{};
	entry.kind = Entry::Kind::Number;
	entry.number = std::isfinite(value) ? value : 0;
}

void Settings::setString(std::string_view key, std::string_view value)
{
	Entry &entry = slot(key);
	entry = Entry{};
	entry.kind = Entry::Kind::String;
	entry.string = std::string(value);
}

void Settings::remove(std::string_view key)
{
	auto it = std::lower_bound(values_.begin(), values_.end(), key, [](const auto &entry, std::string_view k) { return entry.first < k; });
	if (it != values_.end() && it->first == key) values_.erase(it);
}

std::vector<std::string> Settings::keys() const
{
	std::vector<std::string> out;
	out.reserve(values_.size());
	for (const auto &entry : values_) out.push_back(entry.first);
	return out;
}

std::string Settings::valueText(std::string_view key) const
{
	const Entry *entry = find(key);
	if (!entry) return {};
	if (key == settingKeys::kWifiPassword) return entry->kind == Entry::Kind::String && !entry->string.empty() ? "(set)" : "";
	switch (entry->kind) {
	case Entry::Kind::Bool: return entry->boolean ? "true" : "false";
	case Entry::Kind::Number: return formatNumber(entry->number);
	case Entry::Kind::String: return entry->string;
	}
	return {};
}

std::vector<std::string> Settings::granted(std::string_view appId) const
{
	return splitList(getString(permissionsKey(appId)));
}

bool Settings::isGranted(std::string_view appId, std::string_view permission) const
{
	const auto list = granted(appId);
	return std::binary_search(list.begin(), list.end(), std::string(permission));
}

void Settings::setGranted(std::string_view appId, std::string_view permission, bool grant)
{
	if (permission.empty() || permission.find(',') != std::string_view::npos) return;
	auto list = granted(appId);
	const std::string name(permission);
	auto it = std::lower_bound(list.begin(), list.end(), name);
	const bool present = it != list.end() && *it == name;
	if (grant && !present) list.insert(it, name);
	if (!grant && present) list.erase(it);
	if (list.empty()) remove(permissionsKey(appId));
	else setString(permissionsKey(appId), joinList(list));
}

void Settings::forgetApp(std::string_view appId)
{
	std::string prefix = "apps.";
	prefix.append(appId);
	prefix += '.';
	values_.erase(std::remove_if(values_.begin(), values_.end(), [&](const auto &entry) { return entry.first.compare(0, prefix.size(), prefix) == 0; }),
	              values_.end());
}

// ---- Services ----------------------------------------------------------------

std::string settingsPath(const Volume &volume)
{
	std::string root = volume.root;
	while (root.size() > 1 && root.back() == '/') root.pop_back();
	return root + "/system/settings.json";
}

void setSystemServices(SystemServices *services) { g_services = services ? services : &g_noServices; }

SystemServices &systemServices() { return *g_services; }

int applySettings(const Settings &settings, SystemServices &services)
{
	int applied = 0;
	auto percent = [](double value) { return static_cast<int>(std::lround(std::clamp(value, 0.0, 100.0))); };
	if (services.hasBrightness() && settings.has(settingKeys::kBrightness))
		applied += services.setBrightness(percent(settings.getNumber(settingKeys::kBrightness, 100))) ? 1 : 0;
	if (services.hasVolume() && settings.has(settingKeys::kVolume))
		applied += services.setVolume(percent(settings.getNumber(settingKeys::kVolume, 50))) ? 1 : 0;
	if (services.hasWifi() && settings.has(settingKeys::kWifiEnabled)) {
		const bool on = settings.getBool(settingKeys::kWifiEnabled, false);
		const std::string ssid = settings.getString(settingKeys::kWifiSsid);
		// A network the user chose is joined; without one, Wi-Fi only turns
		// on or off (with whatever the firmware was built to join).
		if (on && !ssid.empty())
			applied += services.connectWifi(ssid, settings.getString(settingKeys::kWifiPassword)) ? 1 : 0;
		else
			applied += services.setWifiEnabled(on) ? 1 : 0;
	}
	if (services.hasBle() && settings.has(settingKeys::kBleEnabled))
		applied += services.setBleEnabled(settings.getBool(settingKeys::kBleEnabled, false)) ? 1 : 0;
	if (services.hasClock() && settings.has(settingKeys::kTimeZone))
		applied += services.setTimeZone(settings.getString(settingKeys::kTimeZone)) ? 1 : 0;
	return applied;
}

Settings &deviceSettings() { return g_deviceSettings; }

bool loadDeviceSettings(Fs &fs)
{
	auto volume = Launcher::installVolume();
	if (!volume) return false;
	g_deviceSettings.load(fs, settingsPath(*volume));
	// A volume the user chose for new installs goes to the front.
	const std::string preferred = g_deviceSettings.getString(settingKeys::kInstallVolume);
	if (!preferred.empty()) Launcher::orderVolumes({preferred});
	applySettings(g_deviceSettings, systemServices());
	applyTrustSettings(g_deviceSettings);
	return true;
}

void applyTrustSettings(const Settings &settings)
{
	TrustPolicy policy = trustPolicy();
	policy.revoked = splitList(settings.getString(settingKeys::kTrustRevoked));
	setTrustPolicy(std::move(policy));
}

}  // namespace gea::geaos
