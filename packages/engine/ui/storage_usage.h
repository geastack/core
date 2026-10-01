// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace gea::embedded::ui {
// Read-only census, requested after the frame sample. No allocation counters
// or writes are added to style/text/render hot paths. Heap excludes allocator
// headers; callers report those separately. Shared control blocks are opaque
// to standard C++, so an owning shared_ptr explicitly makes a census partial.
struct StorageUsage {
    std::size_t payload = 0, heap = 0, allocations = 0, staticBytes = 0;
    std::size_t untrackedOwners = 0;
    void addAllocation(const void *pointer, std::size_t requested) {
        if (!pointer) return;
        payload += requested;
#if defined(ESP_PLATFORM)
        heap += heap_caps_get_allocated_size(const_cast<void *>(pointer));
#else
        heap += requested;
#endif
        ++allocations;
    }
    template <class T, class A> void addVector(const std::vector<T, A> &values) {
        if (values.capacity()) addAllocation(values.data(), values.capacity() * sizeof(T));
    }
    void addString(const std::string &value) {
        const auto data = reinterpret_cast<std::uintptr_t>(value.data());
        const auto object = reinterpret_cast<std::uintptr_t>(&value);
        // Small-string buffers belong to their already-counted string object.
        if (value.capacity() && (data < object || data >= object + sizeof(value)))
            addAllocation(value.data(), value.capacity() + 1);
    }
    StorageUsage &operator+=(const StorageUsage &other) {
        payload += other.payload; heap += other.heap; allocations += other.allocations;
        staticBytes += other.staticBytes; untrackedOwners += other.untrackedOwners;
        return *this;
    }
};
}
