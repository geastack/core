// SPDX-License-Identifier: Apache-2.0
#include "host/rtc_vp8_capture.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

using gea::host::rtc::vp8::Capture;

static Capture::Bytes key(size_t size = 10) {
  Capture::Bytes bytes(size, 0);
  bytes[3] = 0x9d; bytes[4] = 1; bytes[5] = 0x2a;
  bytes[6] = 0x8c; bytes[7] = 0x41; // 396 pixels, ignored scale bits
  bytes[8] = 0x8c; bytes[9] = 0x81;
  return bytes;
}

static uint64_t read(const char* bytes, unsigned length) {
  uint64_t value = 0;
  for (unsigned i = 0; i < length; ++i) value |= uint64_t(uint8_t(bytes[i])) << (8*i);
  return value;
}

int main() {
  Capture capture;
  assert(!capture.requested());
  assert(capture.start());
  auto delta = Capture::Bytes{1, 2, 3};
  assert(!capture.take(delta, 0, 0, 0, 0, 0)); // no partial reference chain
  assert(delta.size() == 3);
  capture.workerStarted();
  assert(!capture.start());
  auto first = key();
  assert(capture.take(first, 0xfffffff0, 100, 50, 90, 10));
  assert(first.empty()); // same allocator, ownership moved rather than copied
  assert(capture.take(delta, 0x10, 200, 75, 150, 50));
  assert(delta.empty());
  assert(!capture.save("/dev/null")); // live worker cannot write flash
  capture.workerStopped();

  char* output = nullptr;
  size_t size = 0;
  auto* stream = open_memstream(&output, &size);
  assert(stream && capture.write(stream) && std::fclose(stream) == 0);
  assert(size == 32 + 12 + 10 + 12 + 3);
  assert(!std::memcmp(output, "DKIF", 4) && !std::memcmp(output + 8, "VP80", 4));
  assert(read(output + 12, 2) == 396 && read(output + 14, 2) == 396);
  assert(read(output + 16, 4) == 1000 && read(output + 24, 4) == 2);
  assert(read(output + 32, 4) == 10 && read(output + 36, 8) == 0);
  assert(read(output + 54, 4) == 3 && read(output + 58, 8) == 32);
  assert(!std::memcmp(output + 44, key().data(), 10));
  assert(output[66] == 1 && output[67] == 2 && output[68] == 3);
  std::free(output);

  assert(capture.start());
  first = key(Capture::maxBytes);
  assert(capture.take(first, 0, 0, 0, 0, 0));
  delta = Capture::Bytes{1};
  assert(!capture.take(delta, 1, 0, 0, 0, 0));
  assert(delta.size() == 1 && !capture.requested());
  capture.cancel();
  assert(capture.start());
  first = key();
  assert(capture.take(first, 0, 0, 0, 0, 0));
  for (unsigned i = 1; i < Capture::maxFrames; ++i) {
    delta = Capture::Bytes{1, uint8_t(i)};
    assert(capture.take(delta, i, 0, 0, 0, 0));
  }
  assert(!capture.requested());
  delta = Capture::Bytes{1};
  assert(!capture.take(delta, 100, 0, 0, 0, 0));
  assert(delta.size() == 1);
  capture.cancel();
  assert(!capture.write(stdout));
  std::cout << "VP8 capture ownership, bounds, worker exclusion and IVF framing passed\n";
}
