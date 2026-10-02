#include "host/h264_layout.h"
#include <cassert>
#include <vector>
#include <iostream>
using namespace gea::host::rtc::h264;

struct Writer {
  std::vector<uint8_t> bytes;
  unsigned count = 0;
  void bit(bool value) {
    if (!(count % 8)) bytes.push_back(0);
    if (value) bytes.back() |= 1 << (7 - count % 8);
    ++count;
  }
  void bits(uint32_t value, unsigned n) { while (n) bit((value >> --n) & 1); }
  void ue(uint32_t value) {
    ++value;
    unsigned n = 0;
    for (auto rest = value; rest > 1; rest >>= 1) ++n;
    for (unsigned i = 0; i < n; ++i) bit(false);
    bits(value, n + 1);
  }
};
int main() {
  Writer sps;
  sps.bits(66, 8); sps.bits(0xe0, 8); sps.bits(31, 8); sps.ue(0);
  sps.ue(0); sps.ue(0); sps.ue(0); sps.ue(1); sps.bit(false);
  sps.ue(0); sps.ue(0); sps.bit(true); sps.bit(true);
  sps.bit(true); sps.ue(0); sps.ue(1); sps.ue(0); sps.ue(2);
  sps.bit(false); sps.bit(true);
  Metadata metadata;
  assert(parseSps(sps.bytes.data(), sps.bytes.size(), metadata));
  assert(metadata.width == 16 && metadata.height == 16);
  assert(metadata.visibleWidth == 14 && metadata.visibleHeight == 12);
  assert(!parseSps(sps.bytes.data(), 2, metadata));
  sps.bytes[0] = 100;
  assert(!parseSps(sps.bytes.data(), sps.bytes.size(), metadata));
  std::vector<uint8_t> i420(16 * 16 * 3 / 2, 128);
  std::fill(i420.begin(), i420.begin() + 256, 16);
  std::vector<uint16_t> output(14 * 12, 1);
  convertI420(i420.data(), metadata, output.data());
  for (auto pixel : output) assert(pixel == 0);
  std::fill(i420.begin(), i420.begin() + 256, 235);
  convertI420(i420.data(), metadata, output.data());
  for (auto pixel : output) assert(pixel == 0xffff);
  const uint8_t truncated[] = {0, 0, 1, 0x67};
  assert(!inspectAccessUnit(truncated, sizeof(truncated), metadata));
  std::cout << "H264 crop, bounds and RGB565 conversion passed\n";
}
