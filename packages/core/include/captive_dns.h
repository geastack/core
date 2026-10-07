// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gea::framework::network {

// A bounded DNS responder for AP captive portals. Rebuild the question name
// to avoid retaining compression pointers into discarded additional records.
inline std::size_t captiveDnsReply(const std::uint8_t *query,
                                   std::size_t length,
                                   const std::uint8_t address[4],
                                   std::uint8_t *reply, std::size_t capacity) {
  if (length < 12 || length > 512 || capacity < 12 || (query[2] & 0xf8) ||
      query[4] != 0 || query[5] != 1)
    return 0;
  std::memcpy(reply, query, 12);
  reply[2] = 0x80 | (query[2] & 1);
  reply[3] = 0x80;
  std::memset(reply + 6, 0, 6);
  std::size_t source = 12, target = 12, questionEnd = 0, jumps = 0,
              nameBytes = 0;
  for (;;) {
    if (source >= length)
      return 0;
    const auto count = query[source++];
    if ((count & 0xc0) == 0xc0) {
      if (source >= length || ++jumps > 8)
        return 0;
      const auto offset = ((count & 0x3f) << 8) | query[source++];
      if (!questionEnd)
        questionEnd = source;
      if (offset >= length)
        return 0;
      source = offset;
      continue;
    }
    if (count > 63 || source + count > length ||
        target + count + 1 > capacity || (nameBytes += count + 1) > 255)
      return 0;
    reply[target++] = count;
    if (!count)
      break;
    std::memcpy(reply + target, query + source, count);
    target += count;
    source += count;
  }
  if (!questionEnd)
    questionEnd = source;
  if (questionEnd + 4 > length || target + 4 > capacity)
    return 0;
  const auto type = (query[questionEnd] << 8) | query[questionEnd + 1];
  const auto klass = (query[questionEnd + 2] << 8) | query[questionEnd + 3];
  std::memcpy(reply + target, query + questionEnd, 4);
  target += 4;
  // AAAA and other record types receive an empty successful answer so clients
  // can issue their IPv4 query; this portal only owns its AP IPv4 address.
  if (klass != 1 || (type != 1 && type != 255))
    return target;
  if (target + 16 > capacity)
    return 0;
  constexpr std::uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
  std::memcpy(reply + target, answer, sizeof(answer));
  std::memcpy(reply + target + sizeof(answer), address, 4);
  reply[7] = 1;
  return target + 16;
}

} // namespace gea::framework::network
