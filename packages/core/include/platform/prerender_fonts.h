// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "graphics/font.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace gea::platform::prerender {

namespace detail {

struct DocumentFont {
	std::string family;
	std::vector<std::uint8_t> bytes;
};

inline std::vector<DocumentFont> pageFonts;
// Keep per-document IDs above generated font IDs and within the engine's int16_t storage.
inline constexpr int firstPageFontId = 16384;

inline std::string fontKey(const char *family)
{
	std::string key(family ? family : "");
	std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
	return key;
}

inline const DocumentFont *documentFont(int id)
{
	const int index = id - firstPageFontId;
	return index >= 0 && static_cast<std::size_t>(index) < pageFonts.size() ? &pageFonts[index] : nullptr;
}

inline const framework::graphics::RuntimeFontProvider documentFontProvider{
	+[](const char *family) {
		const std::string key = fontKey(family);
		for (std::size_t i = 0; i < pageFonts.size(); ++i) {
			if (pageFonts[i].family == key) return firstPageFontId + static_cast<int>(i);
		}
		return -1;
	},
	+[](int id) -> const char * {
		const auto *font = documentFont(id);
		return font ? font->family.c_str() : nullptr;
	},
	+[](int id, unsigned long *length) -> const std::uint8_t * {
		const auto *font = documentFont(id);
		if (font) *length = static_cast<unsigned long>(font->bytes.size());
		return font ? font->bytes.data() : nullptr;
	},
};

} // namespace detail

inline void installFontProvider()
{
	framework::graphics::FontRegistry::setRuntimeFontProvider(&detail::documentFontProvider);
}

inline void beginRuntimeFonts()
{
	framework::graphics::FontRegistry::invalidateRuntimeFonts();
	detail::pageFonts.clear();
}

inline bool registerRuntimeFont(const char *path, const char *family)
{
	const auto key = detail::fontKey(family);
	if (key.empty() || detail::pageFonts.size() >= 128) return false;
	if (detail::documentFontProvider.lookupFamily(family) >= 0) return true;
	constexpr std::uintmax_t maxFontBytes = 16 * 1024 * 1024;
	const std::string_view filename(path);
	const std::filesystem::path file(std::u8string(filename.begin(), filename.end()));
	std::error_code error;
	const auto size = std::filesystem::file_size(file, error);
	if (error || size < 12 || size > maxFontBytes) return false;

	std::ifstream input(file, std::ios::binary);
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
	detail::pageFonts.push_back({key, std::move(bytes)});
	const int id = detail::firstPageFontId + static_cast<int>(detail::pageFonts.size()) - 1;
	if (framework::graphics::FontRegistry::rasterizedFamily(id, 20).valid()) return true;

	framework::graphics::FontRegistry::invalidateRuntimeFonts();
	detail::pageFonts.pop_back();
	return false;
}

} // namespace gea::platform::prerender
