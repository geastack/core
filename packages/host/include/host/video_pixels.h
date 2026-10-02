// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <type_traits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace gea::host::media {
class VideoFramePool;

// Allocation failure is an exception where the target has them. An exception-free
// target (the Pebble watch builds with -fno-exceptions) cannot throw, so it aborts.
[[noreturn]] inline void videoAllocationFailed() {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
  throw std::bad_alloc();
#else
  std::abort();
#endif
}

// Packet containers retain the standard allocator contract. Large decoded
// pictures below have an explicit non-throwing allocation boundary instead.
struct VideoOverwriteTag {};
template<class T>
struct VideoAllocator {
  using value_type = T;
  bool valueInitialize = true;
  VideoAllocator() = default;
  explicit VideoAllocator(VideoOverwriteTag) : valueInitialize(false) {}
  template<class U> VideoAllocator(const VideoAllocator<U>& other)
      : valueInitialize(other.valueInitialize) {}
  template<class U, class... Args>
  void construct(U* pointer, Args&&... args) {
    if constexpr (sizeof...(Args) == 0 && std::is_same_v<U, std::uint16_t>) {
      if (!valueInitialize) { ::new (static_cast<void*>(pointer)) U; return; }
    }
    ::new (static_cast<void*>(pointer)) U(std::forward<Args>(args)...);
  }
  T* allocate(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) videoAllocationFailed();
#ifdef ESP_PLATFORM
    constexpr std::size_t alignment = alignof(T) > 16 ? alignof(T) : 16;
    void* result = heap_caps_aligned_alloc(alignment, count * sizeof(T),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void* result = std::malloc(count * sizeof(T));
#endif
    if (!result && count) videoAllocationFailed();
    return static_cast<T*>(result);
  }
  void deallocate(T* pointer, std::size_t) noexcept {
#ifdef ESP_PLATFORM
    heap_caps_free(pointer);
#else
    std::free(pointer);
#endif
  }
  template<class U> bool operator==(const VideoAllocator<U>&) const noexcept { return true; }
};

// Owned, SIMD-aligned pixels. The decoder uses the fallible overwrite operation:
// allocation failure must not itself allocate a C++ exception while under memory
// pressure. A renderer may retain an older immutable frame during replacement.
class VideoPixels {
 public:
  VideoPixels() = default;
  explicit VideoPixels(std::size_t count) { resize(count); }
  VideoPixels(const VideoPixels& other) {
    if (!tryResizeForOverwrite(other.size_)) videoAllocationFailed();
    if (size_) std::memcpy(pixels_, other.pixels_, size_ * sizeof(*pixels_));
  }
  VideoPixels(VideoPixels&& other) noexcept { swap(other); }
  VideoPixels& operator=(const VideoPixels& other) {
    if (this != &other) { VideoPixels copy(other); swap(copy); }
    return *this;
  }
  VideoPixels& operator=(VideoPixels&& other) noexcept {
    if (this != &other) { VideoPixels moved(std::move(other)); swap(moved); }
    return *this;
  }
  ~VideoPixels() { releaseAllocation(); }

  // Every pixel must be written before publication. Failure preserves the
  // previous allocation, size and contents, including on integer overflow.
  bool tryResizeForOverwrite(std::size_t count) noexcept {
    if (count > capacity_) {
      auto* replacement = allocate(count);
      if (!replacement) return false;
      releaseAllocation();
      pixels_ = replacement;
      capacity_ = count;
    }
    size_ = count;
    return true;
  }

  void resize(std::size_t count) {
    const auto previousSize = size_;
    if (count > capacity_) {
      auto* replacement = allocate(count);
      if (!replacement) videoAllocationFailed();
      if (previousSize) std::memcpy(replacement, pixels_, previousSize * sizeof(*pixels_));
      releaseAllocation();
      pixels_ = replacement;
      capacity_ = count;
    }
    if (count > previousSize) std::fill_n(pixels_ + previousSize, count - previousSize, 0);
    size_ = count;
  }

  void assign(std::size_t count, std::uint16_t value) {
    if (!tryResizeForOverwrite(count)) videoAllocationFailed();
    if (count) std::fill_n(pixels_, count, value);
  }
  void pop_back() { if (size_) --size_; }
  void swap(VideoPixels& other) noexcept {
    std::swap(pixels_, other.pixels_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
    pooled_.swap(other.pooled_);
  }
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return !size_; }
  std::uint16_t* data() noexcept { return pixels_; }
  const std::uint16_t* data() const noexcept { return pixels_; }
  std::uint16_t* begin() noexcept { return pixels_; }
  const std::uint16_t* begin() const noexcept { return pixels_; }
  std::uint16_t* end() noexcept { return size_ ? pixels_ + size_ : pixels_; }
  const std::uint16_t* end() const noexcept { return size_ ? pixels_ + size_ : pixels_; }
  std::uint16_t& operator[](std::size_t index) { return pixels_[index]; }
  const std::uint16_t& operator[](std::size_t index) const { return pixels_[index]; }
  std::uint16_t& front() { return pixels_[0]; }
  const std::uint16_t& front() const { return pixels_[0]; }
  bool operator==(const VideoPixels& other) const {
    return size_ == other.size_ && (!size_ || std::equal(begin(), end(), other.begin()));
  }

 private:
  friend class VideoFramePool;
  struct Storage {
    std::uint16_t* pixels = nullptr;
    std::size_t capacity = 0;
    ~Storage() { release(pixels); }
  };
  void releaseAllocation() noexcept {
    if (pooled_) pooled_.reset();
    else release(pixels_);
  }
  static std::uint16_t* allocate(std::size_t count) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t)) return nullptr;
    const auto bytes = count * sizeof(std::uint16_t);
#ifdef ESP_PLATFORM
    return static_cast<std::uint16_t*>(heap_caps_aligned_alloc(
        16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    // Round up without overflowing: aligned_alloc requires a multiple of 16.
    if (bytes > std::numeric_limits<std::size_t>::max() - 15) return nullptr;
#if defined(__ANDROID__)
    // Bionic declares aligned_alloc only from API 28; posix_memalign exists at every level.
    void* result = nullptr;
    if (posix_memalign(&result, 16, (bytes + 15) & ~std::size_t(15)) != 0) return nullptr;
    return static_cast<std::uint16_t*>(result);
#elif defined(_WIN32)
    // The MSVC CRT has no aligned_alloc: its aligned blocks come from
    // _aligned_malloc and must go back through _aligned_free (see release).
    return static_cast<std::uint16_t*>(_aligned_malloc((bytes + 15) & ~std::size_t(15), 16));
#else
    return static_cast<std::uint16_t*>(::aligned_alloc(16, (bytes + 15) & ~std::size_t(15)));
#endif
#endif
  }
  static void release(std::uint16_t* pointer) noexcept {
#ifdef ESP_PLATFORM
    heap_caps_free(pointer);
#elif defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
  }
  std::uint16_t* pixels_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  std::shared_ptr<Storage> pooled_;
};

inline VideoPixels videoPixelsForOverwrite(std::size_t count) {
  VideoPixels pixels;
  if (!pixels.tryResizeForOverwrite(count)) videoAllocationFailed();
  return pixels;
}
} // namespace gea::host::media
