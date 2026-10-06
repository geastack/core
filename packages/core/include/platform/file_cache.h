#pragma once

namespace gea::platform::storage {

// Ensure the persistent file-cache filesystem is mounted; returns true when a
// writable root is available. Calls the registered mount provider (idempotent —
// safe on every file op). With no provider registered it returns false, so
// targets without persistent storage transparently fall back to no caching.
bool ensureMounted();

// Register the platform's mount provider. A target with persistent storage
// (e.g. a microSD card) calls this at startup with its mount function. Done via
// explicit registration rather than a weak symbol so the provider's translation
// unit is guaranteed to be linked in (see the waveshare-p4-7 sdcard_mount.cpp,
// wired from app_main).
void setMountProvider(bool (*provider)());

} // namespace gea::platform::storage
