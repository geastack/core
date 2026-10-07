// SPDX-License-Identifier: Apache-2.0
#include "captive_dns.h"
#include <array>
#include <cassert>
#include <cstring>
#include <vector>

int main() {
  using gea::framework::network::captiveDnsReply;
  const std::uint8_t ip[]{192, 168, 4, 1};
  std::array<std::uint8_t, 512> response{};
  std::vector<std::uint8_t> query{
      0x12, 0x34, 1,   0,   0,   1, 0,   0,   0,   0, 0, 0, 7, 'a', 'p',
      'p',  'l',  'e', 's', 't', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
  auto count = captiveDnsReply(query.data(), query.size(), ip, response.data(),
                               response.size());
  assert(count == query.size() + 16 && response[0] == 0x12 &&
         response[1] == 0x34);
  assert(response[2] == 0x81 && response[3] == 0x80 && response[7] == 1);
  assert(std::memcmp(response.data() + count - 4, ip, 4) == 0);
  query[query.size() - 3] =
      28; // AAAA yields no IPv6 address or fabricated IPv4 data.
  assert(captiveDnsReply(query.data(), query.size(), ip, response.data(),
                         response.size()) == query.size());
  assert(response[7] == 0);
  query[query.size() - 3] = 1;
  for (std::size_t size = 0; size < query.size(); ++size)
    assert(captiveDnsReply(query.data(), size, ip, response.data(),
                           response.size()) == 0);
  assert(captiveDnsReply(query.data(), query.size(), ip, response.data(), 12) ==
         0);
  query[12] = 64; // Invalid label length.
  assert(captiveDnsReply(query.data(), query.size(), ip, response.data(),
                         response.size()) == 0);
  query[12] = 0xc0;
  query[13] = 12; // Compression loop.
  assert(captiveDnsReply(query.data(), query.size(), ip, response.data(),
                         response.size()) == 0);
  std::vector<std::uint8_t> compressed{0, 2, 1, 0,   0,    1,   0, 0,
                                       0, 0, 0, 0,   0xc0, 18,  0, 1,
                                       0, 1, 3, 'a', 'p',  'p', 0};
  count = captiveDnsReply(compressed.data(), compressed.size(), ip,
                          response.data(), response.size());
  assert(count == 37 && response[12] == 3 && response[16] == 0);
  assert(response[17] == 0 && response[18] == 1);
  // Deterministic malformed-input coverage at every size within the UDP bound.
  std::uint32_t state = 12345;
  for (std::size_t length = 0; length <= 512; ++length) {
    std::array<std::uint8_t, 512> bytes{};
    for (auto &byte : bytes) {
      state = state * 1664525 + 1013904223;
      byte = state >> 24;
    }
    if (length >= 12) {
      bytes[2] = 1;
      bytes[4] = 0;
      bytes[5] = 1;
    }
    assert(captiveDnsReply(bytes.data(), length, ip, response.data(),
                           response.size()) <= 512);
  }
}
