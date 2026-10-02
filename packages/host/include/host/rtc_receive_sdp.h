// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gea::host::rtc::receive {

inline std::vector<std::string_view> split(std::string_view value, char delimiter) {
  std::vector<std::string_view> parts;
  while (!value.empty()) {
    const auto end = value.find(delimiter);
    parts.push_back(value.substr(0, end));
    if (end == value.npos) break;
    value.remove_prefix(end + 1);
  }
  return parts;
}

inline std::optional<uint32_t> number(std::string_view value, uint32_t maximum) {
  uint32_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > maximum)
    return std::nullopt;
  return result;
}

inline bool token(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return c >= 33 && c <= 126;
  });
}

inline std::optional<std::array<uint8_t, 32>> fingerprint(std::string_view value) {
  if (value.size() != 95) return std::nullopt;
  std::array<uint8_t, 32> digest{};
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t index = 0; index < digest.size(); ++index) {
    const auto high = hex(value[index * 3]), low = hex(value[index * 3 + 1]);
    if (high < 0 || low < 0 || (index < 31 && value[index * 3 + 2] != ':')) return std::nullopt;
    digest[index] = uint8_t(high * 16 + low);
  }
  return digest;
}

struct Transport {
  std::string ufrag, password, fingerprint, setup;
  std::vector<std::string> candidates;
};

struct Track {
  std::string kind, mid, codec;
  uint8_t payloadType = 0;
  uint32_t ssrc = 0;
  bool rtcpMux = false, reducedRtcp = false, sends = false, pli = false;
  Transport transport;
};

struct Offer {
  std::vector<Track> tracks;
  std::array<uint8_t, 32> peerFingerprint{};
  std::string iceDescription;
};

inline bool assignOnce(std::string& target, std::string_view value) {
  if (!target.empty() && target != value) return false;
  target = value;
  return true;
}

// One incoming Opus track, an Opus/VP8 BUNDLE, or an answer to our Opus microphone
// offer. Reject unsupported media/directions instead of connecting streams
// we cannot route. ICE and SHA-256 fingerprints must agree in BUNDLE.
inline std::optional<Offer> parseDescription(std::string_view sdp, bool receiving) {
  if (sdp.size() > 64 * 1024 || sdp.find('\0') != sdp.npos) return std::nullopt;
  Offer result;
  Transport session;
  std::vector<std::string_view> bundle;
  bool iceLite = false;
  for (auto line : split(sdp, '\n')) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    if (line.find('\r') != line.npos || line.size() > 2048) return std::nullopt;
    if (line.starts_with("m=")) {
      const auto fields = split(line.substr(2), ' ');
      if (fields.size() != 4 || (fields[0] != "audio" && fields[0] != "video") ||
          fields[2] != "UDP/TLS/RTP/SAVPF") return std::nullopt;
      const auto port = number(fields[1], 65535), payload = number(fields[3], 127);
      if (!port || !*port || !payload || (*payload >= 64 && *payload <= 95) ||
          result.tracks.size() == 2) return std::nullopt;
      Track track;
      track.kind = fields[0];
      track.payloadType = uint8_t(*payload);
      track.transport = session;
      result.tracks.push_back(std::move(track));
      continue;
    }
    if (line.starts_with("a=group:BUNDLE ")) {
      if (!bundle.empty() || !result.tracks.empty()) return std::nullopt;
      bundle = split(line.substr(15), ' ');
      continue;
    }
    if (line == "a=ice-lite") { iceLite = true; continue; }
    auto& transport = result.tracks.empty() ? session : result.tracks.back().transport;
    auto attribute = [&](std::string_view prefix, std::string& target) {
      const auto value = line.substr(prefix.size());
      return token(value) && assignOnce(target, value);
    };
    if (line.starts_with("a=ice-ufrag:")) {
      if (!attribute("a=ice-ufrag:", transport.ufrag)) return std::nullopt;
    } else if (line.starts_with("a=ice-pwd:")) {
      if (!attribute("a=ice-pwd:", transport.password)) return std::nullopt;
    } else if (line.starts_with("a=fingerprint:")) {
      constexpr std::string_view prefix = "a=fingerprint:sha-256 ";
      if (!line.starts_with(prefix) || !fingerprint(line.substr(prefix.size())) ||
          !assignOnce(transport.fingerprint, line.substr(prefix.size()))) return std::nullopt;
    } else if (line.starts_with("a=setup:")) {
      if (!attribute("a=setup:", transport.setup)) return std::nullopt;
    } else if (line.starts_with("a=candidate:")) {
      if (transport.candidates.size() >= 32) return std::nullopt;
      transport.candidates.emplace_back(line);
    } else if (!result.tracks.empty()) {
      auto& track = result.tracks.back();
      if (line.starts_with("a=mid:")) {
        if (!attribute("a=mid:", track.mid)) return std::nullopt;
      } else if (line == (receiving ? "a=sendonly" : "a=recvonly")) track.sends = true;
      else if (line == "a=sendonly" || line == "a=recvonly" || line == "a=sendrecv" || line == "a=inactive") return std::nullopt;
      else if (line == "a=rtcp-mux") track.rtcpMux = true;
      else if (line == "a=rtcp-rsize") track.reducedRtcp = true;
      else if (line.starts_with("a=rtpmap:")) {
        const auto fields = split(line.substr(9), ' ');
        if (fields.size() != 2 || number(fields[0], 127) != track.payloadType ||
            !assignOnce(track.codec, fields[1])) return std::nullopt;
      } else if (line.starts_with("a=ssrc:")) {
        const auto id = number(line.substr(7, line.find(' ') - 7), UINT32_MAX);
        if (!id || !*id || (track.ssrc && track.ssrc != *id)) return std::nullopt;
        track.ssrc = *id;
      } else if (line == "a=rtcp-fb:" + std::to_string(track.payloadType) + " nack pli") {
        track.pli = true;
      }
    }
  }
  if (result.tracks.empty() || (!receiving && result.tracks.size() != 1) ||
      bundle.size() != result.tracks.size()) return std::nullopt;
  const auto& first = result.tracks[0];
  if (receiving && result.tracks.size() == 2) {
    const auto& second = result.tracks[1];
    if (bundle[0] == bundle[1] || first.kind == second.kind || first.mid == second.mid ||
        first.ssrc == second.ssrc || first.payloadType == second.payloadType) return std::nullopt;
  } else if (first.kind != "audio" || (!receiving && (first.mid != "0" || first.payloadType != 111))) return std::nullopt;
  for (const auto& track : result.tracks) {
    const auto& ice = track.transport;
    if (std::find(bundle.begin(), bundle.end(), track.mid) == bundle.end() ||
        !track.sends || !track.rtcpMux || (receiving && !track.ssrc) || !token(track.mid) ||
        ice.ufrag.size() < 4 || ice.password.size() < 22 || ice.ufrag.size() > 256 ||
        ice.password.size() > 256 || ice.candidates.empty() ||
        (ice.setup != "passive" && (!receiving || ice.setup != "actpass")) ||
        ice.ufrag != first.transport.ufrag || ice.password != first.transport.password ||
        ice.fingerprint != first.transport.fingerprint || ice.setup != first.transport.setup)
      return std::nullopt;
    auto codec = track.codec;
    std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
    });
    if ((track.kind == "audio" && codec != "opus/48000/2") ||
        (track.kind == "video" && (codec != "vp8/90000" || !track.pli))) return std::nullopt;
  }
  const auto digest = fingerprint(first.transport.fingerprint);
  if (!digest) return std::nullopt;
  result.peerFingerprint = *digest;
  result.iceDescription = "a=ice-ufrag:" + first.transport.ufrag + "\r\na=ice-pwd:" +
      first.transport.password + "\r\n";
  if (iceLite) result.iceDescription += "a=ice-lite\r\n";
  for (const auto& candidate : first.transport.candidates) result.iceDescription += candidate + "\r\n";
  result.iceDescription += "a=end-of-candidates\r\n";
  return result;
}

inline std::optional<Offer> parseOffer(std::string_view sdp) {
  return parseDescription(sdp, true);
}

inline std::optional<Offer> parseMicrophoneAnswer(std::string_view sdp) {
  return parseDescription(sdp, false);
}

inline std::optional<std::string> localIceAttributes(std::string_view localIce) {
  std::string ice;
  bool ufrag = false, password = false;
  for (auto line : split(localIce, '\n')) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.find('\r') != line.npos || line.find('\0') != line.npos) return std::nullopt;
    if (line.starts_with("a=ice-ufrag:")) ufrag = token(line.substr(12));
    else if (line.starts_with("a=ice-pwd:")) password = token(line.substr(10));
    else if (!line.starts_with("a=candidate:") && line != "a=end-of-candidates") continue;
    ice += std::string(line) + "\r\n";
  }
  if (!ufrag || !password) return std::nullopt;
  return ice;
}

inline std::optional<std::string> microphoneOffer(std::string_view localIce,
    std::string_view localFingerprint, uint32_t ssrc) {
  const auto ice = localIceAttributes(localIce);
  if (!ice || !ssrc || !fingerprint(localFingerprint)) return std::nullopt;
  return "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
      "a=msid-semantic: WMS gea\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nc=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\na=sendonly\r\na=rtcp-mux\r\na=rtcp-rsize\r\n" + *ice +
      "a=fingerprint:sha-256 " + std::string(localFingerprint) + "\r\na=setup:actpass\r\n"
      "a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10;useinbandfec=1\r\n"
      "a=msid:gea microphone\r\na=ssrc:" + std::to_string(ssrc) + " cname:gea-mic\r\n"
      "a=ssrc:" + std::to_string(ssrc) + " msid:gea microphone\r\n";
}

inline std::optional<std::string> answer(const Offer& offer, std::string_view localIce,
                                        std::string_view localFingerprint) {
  const auto ice = localIceAttributes(localIce);
  if (!ice || !fingerprint(localFingerprint) || offer.tracks.empty() || offer.tracks.size() > 2)
    return std::nullopt;
  std::string result = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE";
  for (const auto& track : offer.tracks) result += " " + track.mid;
  result += "\r\n";
  for (const auto& track : offer.tracks) {
    const auto pt = std::to_string(track.payloadType);
    result += "m=" + track.kind + " 9 UDP/TLS/RTP/SAVPF " + pt + "\r\nc=IN IP4 0.0.0.0\r\na=mid:" +
        track.mid + "\r\na=recvonly\r\na=rtcp-mux\r\n" + *ice + "a=fingerprint:sha-256 " +
        std::string(localFingerprint) + "\r\na=setup:active\r\na=rtpmap:" + pt + " " + track.codec + "\r\n";
    if (track.reducedRtcp) result += "a=rtcp-rsize\r\n";
    if (track.pli) result += "a=rtcp-fb:" + pt + " nack pli\r\n";
    if (track.kind == "audio") result += "a=fmtp:" + pt + " minptime=10;useinbandfec=1\r\n";
  }
  return result;
}

} // namespace gea::host::rtc::receive
