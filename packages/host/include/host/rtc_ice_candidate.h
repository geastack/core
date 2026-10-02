// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gea::host {
struct RTCIceCandidateInit {
  std::string candidate;
  std::optional<std::string> sdpMid;
  std::optional<double> sdpMLineIndex;
  std::optional<std::string> usernameFragment;
  std::optional<std::string> relayProtocol;
  std::optional<std::string> url;
};

class RTCIceCandidate {
  struct Fields {
    RTCIceCandidateInit init;
    std::optional<std::string> foundation, component, address, protocol, type, tcpType, relatedAddress;
    std::optional<double> priority, port, relatedPort;
  };

 public:
  RTCIceCandidate() = default;
  explicit RTCIceCandidate(RTCIceCandidateInit init) {
    if (!init.sdpMid && !init.sdpMLineIndex)
      throw std::invalid_argument("RTCIceCandidate requires sdpMid or sdpMLineIndex");
    auto fields = std::make_shared<Fields>();
    fields->init = std::move(init);
    // Malformed candidate text is retained verbatim; none of its derived
    // attributes may leak out of a partially successful parse.
    Fields parsed;
    if (parse(fields->init.candidate, parsed)) {
      parsed.init = std::move(fields->init);
      *fields = std::move(parsed);
    }
    fields_ = std::move(fields);
  }
  const RTCIceCandidateInit& fields() const { return data().init; }
  std::string candidate() const { return fields().candidate; }
  std::optional<std::string> sdpMid() const { return fields().sdpMid; }
  std::optional<double> sdpMLineIndex() const { return fields().sdpMLineIndex; }
  std::optional<std::string> usernameFragment() const { return fields().usernameFragment; }
  std::optional<std::string> relayProtocol() const { return fields().relayProtocol; }
  std::optional<std::string> url() const { return fields().url; }
#define GEA_ICE_GETTER(Type, Name) std::optional<Type> Name() const { return data().Name; }
  GEA_ICE_GETTER(std::string, foundation)
  GEA_ICE_GETTER(std::string, component)
  GEA_ICE_GETTER(std::string, address)
  GEA_ICE_GETTER(std::string, protocol)
  GEA_ICE_GETTER(std::string, type)
  GEA_ICE_GETTER(std::string, tcpType)
  GEA_ICE_GETTER(std::string, relatedAddress)
  GEA_ICE_GETTER(double, priority)
  GEA_ICE_GETTER(double, port)
  GEA_ICE_GETTER(double, relatedPort)
#undef GEA_ICE_GETTER
  explicit operator bool() const { return static_cast<bool>(fields_); }
  bool operator==(std::nullptr_t) const { return !fields_; }
  bool operator==(const RTCIceCandidate& other) const { return fields_ == other.fields_; }

 private:
  const Fields& data() const {
    if (!fields_) throw std::logic_error("null RTCIceCandidate");
    return *fields_;
  }
  static bool unsignedNumber(std::string_view text, uint64_t max, double& result) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (const char ch : text) {
      if (ch < '0' || ch > '9') return false;
      const auto digit = static_cast<uint64_t>(ch - '0');
      if (value > max / 10 || (value == max / 10 && digit > max % 10)) return false;
      value = value * 10 + digit;
    }
    result = static_cast<double>(value);
    return true;
  }
  static std::string lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return out;
  }
  static bool parse(const std::string& text, Fields& result) {
    if (!text.starts_with("candidate:")) return false;
    std::vector<std::string_view> tokens;
    const std::string_view source(text);
    size_t start = 10;
    for (size_t i = start; i <= source.size(); ++i) {
      if (i < source.size() && source[i] != ' ') {
        if (static_cast<unsigned char>(source[i]) < 0x21 || static_cast<unsigned char>(source[i]) > 0x7e) return false;
        continue;
      }
      if (i == start) return false;
      tokens.push_back(source.substr(start, i - start));
      start = i + 1;
    }
    if (tokens.size() < 8 || tokens.size() % 2 != 0 || tokens[0].size() > 32) return false;
    for (char ch : tokens[0])
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '+' || ch == '/')) return false;
    double component, priority, port;
    if (!unsignedNumber(tokens[1], 2, component) || component == 0 ||
        !unsignedNumber(tokens[3], UINT32_MAX, priority) || !unsignedNumber(tokens[5], UINT16_MAX, port)) return false;
    const auto protocol = lower(tokens[2]);
    const auto type = lower(tokens[7]);
    if ((protocol != "udp" && protocol != "tcp") || lower(tokens[6]) != "typ" ||
        (type != "host" && type != "srflx" && type != "prflx" && type != "relay")) return false;
    result.foundation = tokens[0];
    result.component = component == 1 ? "rtp" : "rtcp";
    result.priority = priority;
    result.address = tokens[4];
    result.protocol = protocol;
    result.port = port;
    result.type = type;
    for (size_t i = 8; i < tokens.size(); i += 2) {
      const auto key = lower(tokens[i]);
      if (key == "raddr") {
        if (result.relatedAddress) return false;
        result.relatedAddress = tokens[i + 1];
      } else if (key == "rport") {
        double relatedPort;
        if (result.relatedPort || !unsignedNumber(tokens[i + 1], UINT16_MAX, relatedPort)) return false;
        result.relatedPort = relatedPort;
      } else if (key == "tcptype") {
        const auto tcpType = lower(tokens[i + 1]);
        if (result.tcpType || protocol != "tcp" || (tcpType != "active" && tcpType != "passive" && tcpType != "so")) return false;
        result.tcpType = tcpType;
      }
    }
    return protocol != "tcp" || result.tcpType.has_value();
  }
  std::shared_ptr<const Fields> fields_;
};
}
