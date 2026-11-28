// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace gea::host::css {

// CSSOM's identifier serialization. Non-ASCII characters pass through,
// so the special cases can be handled directly on the UTF-8 bytes.
inline std::string escape(std::string_view identifier) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(identifier.size());
  for (std::size_t index = 0; index < identifier.size(); ++index) {
    const auto byte = static_cast<unsigned char>(identifier[index]);
    const bool digit = byte >= '0' && byte <= '9';
    if (byte == 0) {
      result += "\xEF\xBF\xBD";
    } else if (byte <= 0x1f || byte == 0x7f ||
               (digit && (index == 0 || (index == 1 && identifier.front() == '-')))) {
      result += '\\';
      if (byte >= 16) result += hex[byte >> 4];
      result += hex[byte & 15];
      result += ' ';
    } else {
      const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
      const bool loneHyphen = byte == '-' && identifier.size() == 1;
      if (loneHyphen || !(byte >= 0x80 || byte == '-' || byte == '_' || digit || letter)) result += '\\';
      result += identifier[index];
    }
  }
  return result;
}

}  // namespace gea::host::css
