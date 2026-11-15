#pragma once

#include <cstddef>
#include <cstdint>

// Places a large, cold static buffer in external RAM where the target allows a
// PSRAM .bss (ESP32 with CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY); plain
// .bss elsewhere. Internal SRAM is the scarce resource on PSRAM boards: on the
// CrowPanel 7.0 the RGB bounce buffers leave too little of it for the display's
// SRAM staging strips, and every repaint then rasters straight into PSRAM.
// Use it for caches touched per record or per layout, not per pixel.
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define GEA_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define GEA_EXT_BSS
#endif

namespace gea::framework::memory {

class Allocator {
public:
	static void *allocatePreferSpiram(std::size_t size, std::size_t alignment = alignof(std::max_align_t));
	static void *reallocatePreferSpiram(void *ptr, std::size_t size);
	static void free(void *ptr) noexcept;
};

}  // namespace gea::framework::memory

namespace gea::platform::memory {

class Memory {
public:
	static std::uint32_t internalFree();
	static std::uint32_t internalLargestFreeBlock();
	static std::uint32_t internalMinimumFree();
	static std::uint32_t psramFree();
	static std::uint32_t currentTaskStackHighWaterMark();
	static std::uint32_t geaMainStackBytes();
	static std::uint32_t geaInitStackBytes();
	static std::uint32_t appFrameStackWords();
	static std::uint32_t appFrameStackBytes();
	static std::uint32_t displayFlushConfiguredRows();
	static std::uint32_t displayFlushConfiguredDepth();
	static std::uint32_t displayFlushBufferMaxBytes();
	static std::uint32_t displayFlushRows();
	static std::uint32_t displayFlushDepth();
	static std::uint32_t displayFlushBufferBytes();

	// Holds `bytes` of internal, DMA-capable RAM out of the general heap and hands
	// back an opaque reserve, so a later allocation that MUST be internal -- a
	// display bus, a DMA descriptor ring -- still finds a block after code that
	// runs earlier has taken what it wants. nullptr when the reserve could not be
	// taken; releaseInternalDma() gives it back. A platform that does not
	// distinguish internal memory returns nullptr and the caller carries on.
	//
	// Deliberately a real method on the platform API rather than a weak hook: a
	// weak *undefined* reference does not pull the defining object out of a static
	// archive, so the hook-shaped version of this linked to null on every ESP32
	// image and the reserve silently never happened.
	static void *reserveInternalDma(std::size_t bytes);
	static void releaseInternalDma(void *reserve);
};

}  // namespace gea::platform::memory
