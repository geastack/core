#pragma once

#include "platform/file_cache.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "File-backed assets require external BSS storage on ESP32"
#endif
#define GEA_FILE_ASSET_BUFFER EXT_RAM_BSS_ATTR
#else
#define GEA_FILE_ASSET_BUFFER
#endif

namespace gea::platform::storage {
struct AssetSegment {
  unsigned char *data;
  std::size_t size;
};

// The generator names immutable bundles by content. Validate the complete
// payload before exposing any font/image, then retain it in RAM for rendering.
inline bool loadAssetBundle(const char *path, const AssetSegment *segments,
                            std::size_t count, std::size_t bytes,
                            std::uint32_t expectedCrc) {
  if (!ensureMounted()) {
    std::fprintf(stderr, "Gea asset bundle storage not mounted: %s\n", path);
    return false;
  }
  auto *file = std::fopen(path, "rb");
  if (!file) {
    std::fprintf(stderr, "Gea asset bundle missing: %s\n", path);
    return false;
  }
  std::uint32_t crc = 0xffffffffU;
  constexpr std::uint32_t table[] = {
      0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
      0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
      0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
      0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU};
  std::size_t read = 0;
  bool ok = true;
  for (std::size_t index = 0; index < count; ++index) {
    const auto &part = segments[index];
    if (part.size > bytes - read ||
        std::fread(part.data, 1, part.size, file) != part.size) {
      ok = false;
      break;
    }
    read += part.size;
    for (std::size_t i = 0; i < part.size; ++i) {
      crc ^= part.data[i];
      crc = (crc >> 4) ^ table[crc & 15];
      crc = (crc >> 4) ^ table[crc & 15];
    }
  }
  ok = ok && read == bytes && (crc ^ 0xffffffffU) == expectedCrc &&
       std::fgetc(file) == EOF && !std::ferror(file);
  std::fclose(file);
  if (!ok) {
    for (std::size_t index = 0; index < count; ++index)
      std::memset(segments[index].data, 0, segments[index].size);
    std::fprintf(stderr, "Gea asset bundle failed validation: %s\n", path);
  } else
    std::fprintf(stderr, "Gea asset bundle loaded: %s (%lu bytes)\n", path,
                 static_cast<unsigned long>(bytes));
  return ok;
}
} // namespace gea::platform::storage
