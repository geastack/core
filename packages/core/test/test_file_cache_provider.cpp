// SPDX-License-Identifier: Apache-2.0
#include "host/image.h"
#include "platform/atomic_file.h"
#include "platform/file_cache.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <map>

bool mounted() { return true; }
bool missing() { return false; }

struct MockFiles {
  using Handle = const std::string *;
  std::map<std::string, std::vector<std::uint8_t>> data;
  std::string opened;
  bool shortWrite = false, closeFailure = false, replacementFailure = false,
       conflict = false;
  int conflictErrno = EIO;
  Handle open(const std::string &path) {
    opened = path;
    data[path] = {};
    return &opened;
  }
  std::size_t write(Handle file, const std::vector<std::uint8_t> &bytes) {
    data[*file] = bytes;
    if (shortWrite)
      data[*file].resize(bytes.size() - 1);
    return data[*file].size();
  }
  bool close(Handle) { return !closeFailure; }
  bool remove(const std::string &path) {
    data.erase(path);
    return true;
  }
  bool move(const std::string &from, const std::string &to) {
    conflict = data.contains(to);
    if (conflict ||
        (replacementFailure && from.ends_with(".tmp") && !data.contains(to))) {
      errno = conflict ? conflictErrno : EIO;
      return false;
    }
    data[to] = data.at(from);
    data.erase(from);
    return true;
  }
  bool destinationExists(const std::string &path) const {
    return data.contains(path);
  }
};

void verifyFailedReplacement(const std::vector<std::uint8_t> &jpeg) {
  const std::vector<std::uint8_t> old{255, 216, 1, 2, 3, 255, 217};
  for (int fault = 0; fault < 3; ++fault) {
    MockFiles files;
    files.data["badge.jpg"] = old;
    files.shortWrite = fault == 0;
    files.closeFailure = fault == 1;
    files.replacementFailure = fault == 2;
    assert(
        !gea::platform::storage::writeFileAtomically("badge.jpg", jpeg, files));
    assert(files.data.at("badge.jpg") == old);
    assert(!files.data.contains("badge.jpg.tmp"));
  }
  for (const int error : {EEXIST, EIO}) {
    MockFiles files;
    files.conflictErrno = error;
    files.data["badge.jpg"] = old;
    assert(
        gea::platform::storage::writeFileAtomically("badge.jpg", jpeg, files));
    assert(files.data.at("badge.jpg") == jpeg);
    assert(files.data.size() == 1);
  }
}

int main(int argc, char **argv) {
  assert(argc == 3);
  gea::host::ImageService image;
  std::ifstream source(argv[1], std::ios::binary);
  std::vector<std::uint8_t> jpeg{std::istreambuf_iterator<char>(source), {}};
  assert(jpeg.size() > 100 && jpeg[0] == 255 && jpeg[1] == 216);
  verifyFailedReplacement(jpeg);
  using namespace gea::platform::storage;
  assert(!image.writeFile(argv[2], jpeg));
  setFallbackMountProvider(mounted);
  assert(image.writeFile(argv[2], jpeg));
  assert(image.readFile(argv[2]) == jpeg);
  auto replacement = jpeg;
  replacement.push_back(0);
  assert(image.writeFile(argv[2], replacement));
  assert(image.readFile(argv[2]) == replacement);
  assert(image.writeFile(argv[2], jpeg));
  // An explicitly registered SD provider remains authoritative even when its
  // card is absent; fallback registration must not mask a failed SD mount.
  setMountProvider(missing);
  assert(!ensureMounted());
  setFallbackMountProvider(mounted);
  assert(!ensureMounted() && image.readFile(argv[2]).empty());
  setMountProvider(nullptr);
  assert(ensureMounted() && image.readFile(argv[2]) == jpeg);
  assert(image.removeFile(argv[2]));
  assert(image.readFile(argv[2]).empty());
  setFallbackMountProvider(nullptr);
  assert(!ensureMounted());
}
