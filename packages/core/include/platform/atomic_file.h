// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace gea::platform::storage {

struct FileOperations {
  using Handle = std::FILE *;
  Handle open(const std::string &path) {
    return std::fopen(path.c_str(), "wb");
  }
  std::size_t write(Handle file, const std::vector<std::uint8_t> &bytes) {
    return std::fwrite(bytes.data(), 1, bytes.size(), file);
  }
  bool close(Handle file) { return std::fclose(file) == 0; }
  bool move(const std::string &from, const std::string &to) {
    return std::rename(from.c_str(), to.c_str()) == 0;
  }
  bool remove(const std::string &path) {
    return std::remove(path.c_str()) == 0 || errno == ENOENT;
  }
  bool destinationExists(const std::string &path) const {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0;
  }
};

// POSIX/FAT can replace a destination with rename; SPIFFS requires moving it
// aside first. Never truncate the current file while writing its replacement.
// If the second rename fails, restore the backup (and retain it on disk even
// when the filesystem also rejects that recovery rename).
template <typename Operations>
bool writeFileAtomically(const std::string &path,
                         const std::vector<std::uint8_t> &bytes,
                         Operations &files) {
  const auto temporary = path + ".tmp";
  auto file = files.open(temporary);
  if (!file)
    return false;
  const bool written = files.write(file, bytes) == bytes.size();
  const bool closed = files.close(file);
  if (!written || !closed) {
    files.remove(temporary);
    return false;
  }
  if (files.move(temporary, path))
    return true;
  if (!files.destinationExists(path)) {
    files.remove(temporary);
    return false;
  }
  const auto backup = path + ".bak";
  if (!files.remove(backup) || !files.move(path, backup)) {
    files.remove(temporary);
    return false;
  }
  if (!files.move(temporary, path)) {
    files.move(backup, path);
    files.remove(temporary);
    return false;
  }
  files.remove(backup);
  return true;
}

} // namespace gea::platform::storage
