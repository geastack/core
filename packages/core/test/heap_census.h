// SPDX-License-Identifier: Apache-2.0
//
// Whole-process heap census for the memory-census tests, on both hosts.
//
// macOS reports bytes and blocks in use across every allocator zone. glibc has
// no in-use block count, so on Linux the bytes come from mallinfo2() and the
// blocks are the live allocations made through operator new -- the census
// fixtures allocate through C++ containers, so that is what they measure.
// Include from exactly one translation unit: on Linux it replaces the global
// operator new/delete.
#pragma once

#include <cstddef>

#if defined(__APPLE__)
#include <malloc/malloc.h>

struct HeapCensus {
	std::size_t size_in_use = 0;
	unsigned blocks_in_use = 0;
};

inline void captureHeapCensus(HeapCensus *census)
{
	malloc_statistics_t heap{};
	malloc_zone_statistics(nullptr, &heap);  // all allocator zones
	census->size_in_use = heap.size_in_use;
	census->blocks_in_use = heap.blocks_in_use;
}

#elif defined(__GLIBC__)
#include <cstdlib>
#include <malloc.h>
#include <new>

struct HeapCensus {
	std::size_t size_in_use = 0;
	unsigned blocks_in_use = 0;
};

inline unsigned gHeapCensusLiveBlocks = 0;

inline void captureHeapCensus(HeapCensus *census)
{
	const struct mallinfo2 info = mallinfo2();
	census->size_in_use = info.uordblks + info.hblkhd;
	census->blocks_in_use = gHeapCensusLiveBlocks;
}

static void *heapCensusAllocate(std::size_t size)
{
	void *block = std::malloc(size ? size : 1);
	if (!block) throw std::bad_alloc();
	++gHeapCensusLiveBlocks;
	return block;
}

static void heapCensusRelease(void *block) noexcept
{
	if (!block) return;
	--gHeapCensusLiveBlocks;
	std::free(block);
}

void *operator new(std::size_t size) { return heapCensusAllocate(size); }
void *operator new[](std::size_t size) { return heapCensusAllocate(size); }
void operator delete(void *block) noexcept { heapCensusRelease(block); }
void operator delete[](void *block) noexcept { heapCensusRelease(block); }
void operator delete(void *block, std::size_t) noexcept { heapCensusRelease(block); }
void operator delete[](void *block, std::size_t) noexcept { heapCensusRelease(block); }

#else
#error "heap_census.h: no heap statistics for this host"
#endif
