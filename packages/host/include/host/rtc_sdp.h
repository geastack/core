// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace gea::host::rtc {
// RFC 7587: the RTP clock stays 48000/2. These receiver hints describe the
// actual PCM sink, avoiding decoding bandwidth the device cannot reproduce.
inline std::string opusPcmCapabilities(const std::string &sdp, int rate, int channels) {
  const std::string newline = sdp.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const bool terminated = !sdp.empty() && sdp.back() == '\n';
  std::vector<std::string> lines;
  for (std::size_t at = 0; at < sdp.size();) {
    auto end = sdp.find('\n', at);
    auto line = sdp.substr(at, end == std::string::npos ? end : end - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string::npos) break;
    at = end + 1;
  }
  for (std::size_t start = 0; start < lines.size();) {
    if (lines[start].rfind("m=audio ", 0) != 0) { ++start; continue; }
    std::size_t end = start + 1;
    while (end < lines.size() && lines[end].rfind("m=", 0) != 0) ++end;
    std::vector<std::string> payloads;
    for (auto i = start + 1; i < end; ++i) {
      if (lines[i].rfind("a=rtpmap:", 0) != 0) continue;
      const auto space = lines[i].find(' ');
      if (space != std::string::npos && lines[i].substr(space + 1) == "opus/48000/2")
        payloads.push_back(lines[i].substr(9, space - 9));
    }
    for (const auto &payload : payloads) {
      const auto prefix = "a=fmtp:" + payload + " ";
      std::size_t index = start + 1;
      while (index < end && lines[index].rfind(prefix, 0) != 0) ++index;
      std::string parameters;
      if (index < end) {
        const auto existing = lines[index].substr(prefix.size());
        for (std::size_t at = 0; at < existing.size();) {
          const auto next = existing.find(';', at);
          auto parameter = existing.substr(at, next == std::string::npos ? next : next - at);
          const auto first = parameter.find_first_not_of(" \t");
          if (first != std::string::npos) parameter.erase(0, first);
          const auto equal = parameter.find('=');
          auto key = parameter.substr(0, equal);
          const auto last = key.find_last_not_of(" \t");
          if (last != std::string::npos) key.resize(last + 1);
          if (!parameter.empty() && key != "maxplaybackrate" && key != "sprop-maxcapturerate" &&
              key != "stereo" && key != "sprop-stereo") parameters += parameter + ";";
          if (next == std::string::npos) break;
          at = next + 1;
        }
      }
      parameters += "maxplaybackrate=" + std::to_string(rate) +
          ";sprop-maxcapturerate=" + std::to_string(rate) +
          ";stereo=" + (channels == 1 ? "0" : "1") +
          ";sprop-stereo=" + (channels == 1 ? "0" : "1");
      if (index < end) lines[index] = prefix + parameters;
      else { lines.insert(lines.begin() + end, prefix + parameters); ++end; }
    }
    start = end;
  }
  std::string result;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    result += lines[i];
    if (i + 1 < lines.size() || terminated) result += newline;
  }
  return result;
}
} // namespace gea::host::rtc
