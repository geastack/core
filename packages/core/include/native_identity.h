#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#endif

// Node creation and removal are cold; keeping them out of line also keeps
// their growth path from widening callers' value ranges.
#if defined(__GNUC__)
#define GEA_IDENTITY_COLD __attribute__((noinline, cold))
#else
#define GEA_IDENTITY_COLD
#endif

namespace gea::framework::identity {
struct Token {};
struct NodeToken : Token {
  // Node expandos belong to the live allocation. Retiring a node clears
  // this attachment even if one of its properties retains a handle back
  // to the same node. Only the node registry lock accesses it.
  mutable std::shared_ptr<void> propertyStorage;
};
using Owner = std::shared_ptr<const Token>;
using NodeOwner = std::shared_ptr<const NodeToken>;
// A copied carrier retains the same occurrence, including copies made before
// the first identity observation. Fresh event carriers have distinct tokens.
class Occurrence {
public:
  Occurrence() = default;
  Occurrence(const Occurrence &other) : owner_(other.owner()) {}
  Occurrence &operator=(const Occurrence &other) {
    if (this != &other) owner_ = other.owner();
    return *this;
  }
  Owner owner() const {
    if (!owner_) owner_ = std::make_shared<Token>();
    return owner_;
  }
  const void *identity() const { return owner().get(); }
private:
  mutable Owner owner_;
};

// Tree ids are reused. Registry entries cover only the current allocation;
// the tree retains live-node tokens until removal/clear. Retained handles
// own retired tokens independently; weak collections never extend that life.
struct NodeRegistry {
#if defined(ESP_PLATFORM)
  // Every node handle reads this. A FreeRTOS mutex costs a take and a give
  // with priority inheritance, so ESP-IDF guards the few instructions with a
  // spinlock critical section. Nothing allocates or frees inside it.
  portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
  void enter() { portENTER_CRITICAL(&lock); }
  void exit() { portEXIT_CRITICAL(&lock); }
#else
  std::mutex mutex;
  void enter() { mutex.lock(); }
  void exit() { mutex.unlock(); }
#endif
  // Indexed by tree id; ids are small and dense. Changed under the lock.
  std::vector<NodeOwner> nodes;
  // The address of each live token, published for lock-free `current`
  // checks. A handle's own owner keeps its token alive, so an equal address
  // is the same allocation. Pages are never freed, so readers never race a
  // reallocation; ids beyond the directory fall back to the locked path.
  static constexpr std::size_t kPageSize = 64;
  static constexpr std::size_t kPages = 64;
  using Page = std::atomic<const Token *>;
  std::atomic<Page *> pages[kPages] = {};
  Page *page(std::size_t index) const {
    return index / kPageSize < kPages ? pages[index / kPageSize].load(std::memory_order_acquire) : nullptr;
  }
  // Allocates outside any lock; a racing writer's spare page is discarded.
  void ensurePage(std::size_t index) {
    if (index / kPageSize >= kPages || page(index)) return;
    Page *fresh = new Page[kPageSize];
    for (std::size_t i = 0; i < kPageSize; ++i) fresh[i].store(nullptr, std::memory_order_relaxed);
    Page *expected = nullptr;
    if (!pages[index / kPageSize].compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
      delete[] fresh;
  }
  // Under the lock.
  void publish(std::size_t index, const Token *token) {
    if (Page *slots = page(index)) slots[index % kPageSize].store(token, std::memory_order_release);
  }
};
class RegistryLock {
public:
  explicit RegistryLock(NodeRegistry &registry) : registry_(registry) { registry_.enter(); }
  ~RegistryLock() { registry_.exit(); }
  RegistryLock(const RegistryLock &) = delete;
  RegistryLock &operator=(const RegistryLock &) = delete;
private:
  NodeRegistry &registry_;
};
inline NodeRegistry &registry() {
  // Without hardware atomics (ESP32-S3) a guarded function-local static takes
  // an RTOS mutex on every access; later calls take this plain load.
  static std::atomic<NodeRegistry *> ready{nullptr};
  if (auto *value = ready.load(std::memory_order_acquire)) return *value;
  static NodeRegistry value;
  ready.store(&value, std::memory_order_release);
  return value;
}
inline Owner node(int id) {
  if (id < 0) return {};
  const auto index = static_cast<std::size_t>(id);
  auto &r = registry();
  RegistryLock lock(r);
  return index < r.nodes.size() ? r.nodes[index] : Owner{};
}
// `token` must be kept alive by the caller (a handle's owner), so an equal
// address is the same allocation.
inline bool current(int id, const Token *token) {
  if (id < 0 || !token) return false;
  const auto index = static_cast<std::size_t>(id);
  auto &r = registry();
  // The slot is only compared, never dereferenced, so it needs no barrier;
  // the page itself is acquired because its slots are read.
  if (const auto *slots = r.page(index))
    return slots[index % NodeRegistry::kPageSize].load(std::memory_order_relaxed) == token;
  RegistryLock lock(r);
  return index < r.nodes.size() && r.nodes[index].get() == token;
}
inline bool current(int id, const Owner &owner) { return current(id, owner.get()); }
inline std::shared_ptr<void> nodePropertyStorage(int id, const Owner &owner, const std::shared_ptr<void> &initial = {}) {
  if (id < 0 || !owner) return {};
  auto &r = registry();
  RegistryLock lock(r);
  const auto index = static_cast<std::size_t>(id);
  if (index >= r.nodes.size() || r.nodes[index] != owner) return {};
  auto &storage = r.nodes[index]->propertyStorage;
  if (!storage) storage = initial;
  return storage;
}
GEA_IDENTITY_COLD inline void activate(int id) {
  if (id < 0) return;
  const auto index = static_cast<std::size_t>(id);
  // Declared before the lock: the replaced token and any outgrown storage
  // are released after it.
  NodeOwner token = std::make_shared<NodeToken>();
  std::shared_ptr<void> releasedProperties;
  const Token *published = token.get();
  std::vector<NodeOwner> spare;
  auto &r = registry();
  r.ensurePage(index);
  for (;;) {
    std::size_t size;
    {
      RegistryLock lock(r);
      if (index < r.nodes.size()) {
        std::swap(r.nodes[index], token);
        if (token) releasedProperties = std::move(token->propertyStorage);
        r.publish(index, published);
        return;
      }
      if (index < spare.size() && r.nodes.size() <= spare.size()) {
        std::swap_ranges(r.nodes.begin(), r.nodes.end(), spare.begin());
        r.nodes.swap(spare);
        std::swap(r.nodes[index], token);
        r.publish(index, published);
        return;
      }
      size = r.nodes.size();
    }
    spare.assign(std::max(index + 1, size * 2), NodeOwner{});
  }
}
GEA_IDENTITY_COLD inline void retire(int id) {
  if (id < 0) return;
  const auto index = static_cast<std::size_t>(id);
  NodeOwner released;
  std::shared_ptr<void> releasedProperties;
  auto &r = registry();
  RegistryLock lock(r);
  if (index < r.nodes.size()) std::swap(r.nodes[index], released);
  if (released) releasedProperties = std::move(released->propertyStorage);
  r.publish(index, nullptr);
}
GEA_IDENTITY_COLD inline void clear() {
  std::vector<NodeOwner> released;
  auto &r = registry();
  {
    RegistryLock lock(r);
    for (std::size_t index = 0; index < r.nodes.size(); ++index) r.publish(index, nullptr);
    released.swap(r.nodes);
  }
  // These tokens can no longer pass the registry check, so no lookup can
  // touch the attachment. Release values after unlocking the registry.
  for (const auto &token : released) if (token) token->propertyStorage.reset();
}
} // namespace gea::framework::identity
