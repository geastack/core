// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/media.h"
#include "host/rtc_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gea::host {

namespace rtc {
struct DescriptionResult {
  std::atomic<bool> ready{false};
  std::string sdp, error;
};
using DescriptionCallback = std::function<void(const std::string&, const std::string&)>;
}


using NativeRtcPeerHandle = std::uint32_t;

class RTCPeerConnectionOnIceCandidateProperty {
 public:
  NativeRtcPeerHandle handle = 0;
  constexpr RTCPeerConnectionOnIceCandidateProperty() = default;
  explicit constexpr RTCPeerConnectionOnIceCandidateProperty(NativeRtcPeerHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const RTCPeerConnectionOnIceCandidateProperty &operator=(gea_cpp_value callback) const;
#endif
};

class RTCPeerConnectionOnTrackProperty {
 public:
  NativeRtcPeerHandle handle = 0;
  constexpr RTCPeerConnectionOnTrackProperty() = default;
  explicit constexpr RTCPeerConnectionOnTrackProperty(NativeRtcPeerHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const RTCPeerConnectionOnTrackProperty &operator=(gea_cpp_value callback) const;
#endif
};

class RTCPeerConnectionOnConnectionStateProperty {
 public:
  NativeRtcPeerHandle handle = 0;
  constexpr RTCPeerConnectionOnConnectionStateProperty() = default;
  explicit constexpr RTCPeerConnectionOnConnectionStateProperty(NativeRtcPeerHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const RTCPeerConnectionOnConnectionStateProperty &operator=(gea_cpp_value callback) const;
#endif
};

class RTCPeerConnection {
 public:
  NativeRtcPeerHandle nativeHandle = 0;
  mutable RTCPeerConnectionOnIceCandidateProperty onicecandidate;
  mutable RTCPeerConnectionOnTrackProperty ontrack;
  mutable RTCPeerConnectionOnConnectionStateProperty onconnectionstatechange;
  mutable RTCPeerConnectionOnConnectionStateProperty oniceconnectionstatechange;

  RTCPeerConnection() = default;
  explicit RTCPeerConnection(NativeRtcPeerHandle h)
      : nativeHandle(h),
        onicecandidate(h),
        ontrack(h),
        onconnectionstatechange(h),
        oniceconnectionstatechange(h), lifetime_(rtc::retain_peer(h)) {}
  explicit RTCPeerConnection(double h) : RTCPeerConnection(static_cast<NativeRtcPeerHandle>(h)) {}
  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  std::string connectionState() const;
  std::string iceConnectionState() const;
  RTCRtpSender addTrack(MediaStreamTrack track, MediaStream stream = {}) const;
  RTCRtpSender addTrack(MediaStreamTrack track, const std::vector<MediaStream>& streams) const;
  void removeTrack(RTCRtpSender sender) const;
  RTCRtpTransceiver addTransceiver(MediaStreamTrack, const RTCRtpTransceiverInit& = {}) const;
  RTCRtpTransceiver addTransceiver(const std::string& kind, const RTCRtpTransceiverInit& = {}) const;
  std::vector<RTCRtpTransceiver> getTransceivers() const;
  std::vector<RTCRtpSender> getSenders() const;
  std::vector<RTCRtpReceiver> getReceivers() const;
  RTCDataChannel createDataChannel(const std::string&, const RTCDataChannelInit& = {}) const;
  RTCConfiguration getConfiguration() const;
  std::string signalingState() const;
  std::optional<RTCSessionDescriptionObject> localDescription() const;
  std::optional<RTCSessionDescriptionObject> remoteDescription() const;
  std::string createOffer() const;
  std::string createAnswer() const;
  void requestDescription(bool offer, rtc::DescriptionCallback callback) const;
  void setLocalDescription(const std::string &sdp_type, const std::string &sdp) const;
  void setRemoteDescription(const std::string &sdp_type, const std::string &sdp) const;
  void addIceCandidate(const std::string &candidate, const std::string &sdp_mid, double sdp_mline_index) const;
  void close() const;
 private:
  std::shared_ptr<rtc::PeerLifetime> lifetime_;
};

namespace rtc {

struct CallbackTable {
  std::function<void(const std::string &, const std::string &, int)> on_ice_candidate;
  std::function<void(RTCTrackEvent)> on_track;
  std::function<void()> on_connection_state_change;
  std::function<void()> on_ice_connection_state_change;
  std::function<void()> on_signaling_state_change, on_negotiation_needed;
  std::function<void(RTCDataChannel)> on_data_channel;
};

std::unordered_map<NativeRtcPeerHandle, CallbackTable> &callbackTable();

NativeRtcPeerHandle create_handle();
NativeRtcPeerHandle create_handle_with_config(const std::string &ice_servers_json);

NativeRtcPeerHandle create_handle(const RTCConfiguration& config);
// Generated record adaptation is supplied by the compiler's typed host adapter.
void destroy_handle(NativeRtcPeerHandle handle);

void runCallbacks();

// Producer hooks called from platform code (libpeer callbacks on ESP, tests on host).
void enqueue_ice_candidate(NativeRtcPeerHandle h, const std::string &candidate, const std::string &sdp_mid, int idx);
void enqueue_track(NativeRtcPeerHandle h, NativeMediaTrackHandle track, std::vector<MediaStream> streams = {});
void enqueue_connection_state_change(NativeRtcPeerHandle h, const std::string &new_state);
void enqueue_ice_connection_state_change(NativeRtcPeerHandle h, const std::string &new_state);

}  // namespace rtc

#ifdef GEA_CPP_VALUE_AVAILABLE

inline const RTCPeerConnectionOnIceCandidateProperty &RTCPeerConnectionOnIceCandidateProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    rtc::callbackTable()[handle].on_ice_candidate = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  rtc::callbackTable()[handle].on_ice_candidate = [callable](const std::string &candidate, const std::string &sdpMid, int sdpMLineIndex) {
    gea_cpp_value event;
    event.kind = gea_cpp_value::kind_t::record;
    event.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
    if (candidate.empty()) {
      event.record_set_literal("candidate", gea_cpp_value::missing());
    } else {
      gea_cpp_value inner;
      inner.kind = gea_cpp_value::kind_t::record;
      inner.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
      inner.record_set_literal("candidate", gea_cpp_value(candidate));
      inner.record_set_literal("sdpMid", gea_cpp_value(sdpMid));
      inner.record_set_literal("sdpMLineIndex", gea_cpp_value(static_cast<double>(sdpMLineIndex)));
      event.record_set_literal("candidate", inner);
    }
    (*callable)(std::vector<gea_cpp_value>{event});
  };
  return *this;
}

inline const RTCPeerConnectionOnTrackProperty &RTCPeerConnectionOnTrackProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    rtc::callbackTable()[handle].on_track = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  rtc::callbackTable()[handle].on_track = [callable](RTCTrackEvent trackEvent) {
    gea_cpp_value event;
    event.kind = gea_cpp_value::kind_t::record;
    event.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
    event.record_set_literal("track", gea_cpp_value(static_cast<double>(trackEvent.track().nativeHandle)));
    (*callable)(std::vector<gea_cpp_value>{event});
  };
  return *this;
}

inline const RTCPeerConnectionOnConnectionStateProperty &RTCPeerConnectionOnConnectionStateProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    rtc::callbackTable()[handle].on_connection_state_change = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  rtc::callbackTable()[handle].on_connection_state_change = [callable]() {
    (*callable)(std::vector<gea_cpp_value>{});
  };
  return *this;
}

#endif  // GEA_CPP_VALUE_AVAILABLE

}  // namespace gea::host
