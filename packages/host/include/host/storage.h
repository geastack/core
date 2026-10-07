// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "services/storage_service.h"
#include "platform/internal_stack.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gea::host {

// Browser-compatible `localStorage` for app code: a synchronous, in-RAM
// key/value store that PERSISTS across reboots / power cycles. It mirrors the
// DOM Storage API exactly — getItem / setItem / removeItem / clear / key(index)
// / length — and is reached through the global `localStorage`, with no import.
//
// WHY IT IS SAFE TO CALL FROM ANYWHERE (the old "frame task only" caveat is
// gone): the full key/value set lives in `entries_` in RAM, so every getItem /
// setItem / removeItem / clear / key / length is a pure in-memory operation —
// safe from ANY task, at any time (a store constructor, init(), tick(), or an
// event handler). A mutation only flips a dirty flag; the actual flash write is
// deferred to flushPending(), which the runtime calls once per frame. Flash
// writes run on the shared internal-stack worker when the runtime stack is in
// PSRAM, so a cache-disabled flash operation cannot invalidate its own stack.
// load() runs during boot on the internal bring-up stack.
//
// Persistence is delegated to gea::framework::services::StorageService, which
// stores the whole set as one opaque blob (an NVS blob on esp32, RAM elsewhere),
// so this header stays target-agnostic and link-light: it is header-only and its
// methods are instantiated only where an app actually uses localStorage.
struct StorageFacade {
  // Number of stored keys, exposed as the `localStorage.length` property. Kept
  // in sync on every mutation (read directly as a member, no accessor call).
  double length = 0;

  std::string getItem(const std::string &key) const {
    for (const auto &entry : entries_)
      if (entry.first == key) return entry.second;
    return std::string();
  }
  void setItem(const std::string &key, const std::string &value) {
    for (auto &entry : entries_)
      if (entry.first == key) {
        if (entry.second != value) {
          entry.second = value;
          dirty_ = true;
        }
        return;
      }
    entries_.emplace_back(key, value);
    length = static_cast<double>(entries_.size());
    dirty_ = true;
  }
  void removeItem(const std::string &key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
      if (it->first == key) {
        entries_.erase(it);
        length = static_cast<double>(entries_.size());
        dirty_ = true;
        return;
      }
  }
  void clear() {
    if (entries_.empty()) return;
    entries_.clear();
    length = 0;
    dirty_ = true;
  }
  std::string key(double index) const {
    const auto raw = static_cast<std::size_t>(index);
    return raw < entries_.size() ? entries_[raw].first : std::string();
  }

  // --- runtime plumbing (not part of the app-facing localStorage API) ------
  // Restore the persisted set. Called once at boot from the frame task.
  void load() {
    std::string blob;
    gea::framework::services::StorageService::loadKv(blob);
    deserialize(blob);
    dirty_ = false;
  }
  // Persist if a mutation happened since the last flush. Called once per frame
  // from the main frame task — the only context that is allowed to touch flash.
  void flushPending() {
    if (!dirty_) return;
    const auto blob = serialize();
    gea::platform::onInternalStack([&] {
      gea::framework::services::StorageService::saveKv(blob);
    });
    dirty_ = false;
  }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  bool dirty_ = false;

  // Blob layout: each entry is two length-prefixed chunks (4-byte host-order
  // length + bytes), key then value. Binary-safe: keys/values may contain NUL.
  // Same device serializes and deserializes, so host byte order round-trips.
  std::string serialize() const {
    std::string out;
    for (const auto &entry : entries_) {
      appendChunk(out, entry.first);
      appendChunk(out, entry.second);
    }
    return out;
  }
  void deserialize(const std::string &blob) {
    entries_.clear();
    std::size_t pos = 0;
    while (true) {
      std::string k;
      std::string v;
      if (!readChunk(blob, pos, k)) break;
      if (!readChunk(blob, pos, v)) break;
      entries_.emplace_back(std::move(k), std::move(v));
    }
    length = static_cast<double>(entries_.size());
  }
  static void appendChunk(std::string &out, const std::string &chunk) {
    const std::uint32_t len = static_cast<std::uint32_t>(chunk.size());
    char header[sizeof(len)];
    std::memcpy(header, &len, sizeof(len));
    out.append(header, sizeof(header));
    out.append(chunk);
  }
  static bool readChunk(const std::string &blob, std::size_t &pos, std::string &out) {
    if (pos + sizeof(std::uint32_t) > blob.size()) return false;
    std::uint32_t len = 0;
    std::memcpy(&len, blob.data() + pos, sizeof(len));
    pos += sizeof(len);
    if (pos + len > blob.size()) return false;
    out.assign(blob.data() + pos, len);
    pos += len;
    return true;
  }
};

inline StorageFacade Storage;

}  // namespace gea::host
