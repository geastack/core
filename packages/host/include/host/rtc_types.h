// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/media.h"
#include "host/rtc_ice_candidate.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gea::host {
namespace rtc {
struct PeerLifetime;
std::shared_ptr<PeerLifetime> retain_peer(uint32_t handle);
std::shared_ptr<PeerLifetime> retain_object(uint32_t handle);
}
struct RTCIceServer {
  std::vector<std::string> urls;
  std::string username;
  std::string credential;
};
struct RTCConfiguration {
  std::vector<RTCIceServer> iceServers;
  std::string iceTransportPolicy = "all";
};
struct RTCSessionDescription {
  std::string type;
  std::string sdp;
};

// Description getters preserve object identity until the peer replaces its
// SDP snapshot. JSON dictionaries are separate, mutable copies.
class RTCSessionDescriptionObject {
 public:
  RTCSessionDescriptionObject() = default;
  explicit RTCSessionDescriptionObject(RTCSessionDescription fields)
      : fields_(std::make_shared<RTCSessionDescription>(std::move(fields))) { validateType(fields_->type); }
  std::string type() const { return fields().type; }
  std::string sdp() const { return fields().sdp; }
  const RTCSessionDescription &fields() const {
    if (!fields_) throw std::logic_error("null RTCSessionDescription");
    return *fields_;
  }
  explicit operator bool() const { return static_cast<bool>(fields_); }
  bool operator==(const RTCSessionDescriptionObject &other) const { return fields_ == other.fields_; }
  bool operator==(std::nullptr_t) const { return !fields_; }

 private:
  static void validateType(const std::string &type) {
    if (type != "offer" && type != "answer" && type != "pranswer" && type != "rollback")
      throw std::invalid_argument("invalid RTCSessionDescription type");
  }
  std::shared_ptr<const RTCSessionDescription> fields_;
};
struct RTCDataChannelInit {
  bool ordered = true;
  std::optional<double> maxPacketLifeTime;
  std::optional<double> maxRetransmits;
  std::string protocol;
  bool negotiated = false;
  std::optional<double> id;
};
struct RTCRtpTransceiverInit {
  std::string direction = "sendrecv";
};
class RTCDataChannel {
 public:
  uint32_t nativeHandle = 0;
  RTCDataChannel() = default;
  explicit RTCDataChannel(uint32_t handle) : nativeHandle(handle), lifetime_(rtc::retain_object(handle)) {}
  explicit operator bool() const { return nativeHandle != 0; }
  bool operator==(std::nullptr_t) const { return nativeHandle == 0; }
  bool operator!=(std::nullptr_t) const { return nativeHandle != 0; }
  bool operator==(const RTCDataChannel& other) const { return nativeHandle == other.nativeHandle; }
  std::string label() const;
  std::string protocol() const;
  std::string readyState() const;
  std::string binaryType() const;
  void setBinaryType(const std::string&) const;
  bool ordered() const;
  std::optional<double> id() const;
  std::optional<double> maxRetransmits() const;
  std::optional<double> maxPacketLifeTime() const;
  double bufferedAmount() const;
  double bufferedAmountLowThreshold() const;
  void setBufferedAmountLowThreshold(double) const;
  void send(const std::string&) const;
  void send(const std::vector<uint8_t>&) const;
  void close() const;
 private:
  std::shared_ptr<rtc::PeerLifetime> lifetime_;
};
class RTCRtpSender {
 public:
  uint32_t nativeHandle = 0;
  RTCRtpSender() = default;
  explicit RTCRtpSender(uint32_t handle) : nativeHandle(handle), lifetime_(rtc::retain_object(handle)) {}
  std::optional<MediaStreamTrack> track() const;
  void replaceTrack(MediaStreamTrack) const;
  void replaceTrack() const;
 private:
  std::shared_ptr<rtc::PeerLifetime> lifetime_;
};
class RTCRtpReceiver {
 public:
  uint32_t nativeHandle = 0;
  RTCRtpReceiver() = default;
  explicit RTCRtpReceiver(uint32_t handle) : nativeHandle(handle), lifetime_(rtc::retain_object(handle)) {}
  std::optional<MediaStreamTrack> track() const;
 private:
  std::shared_ptr<rtc::PeerLifetime> lifetime_;
};
class RTCRtpTransceiver {
 public:
  uint32_t nativeHandle = 0;
  RTCRtpTransceiver() = default;
  explicit RTCRtpTransceiver(uint32_t handle) : nativeHandle(handle), lifetime_(rtc::retain_object(handle)) {}
  RTCRtpSender sender() const { return RTCRtpSender(nativeHandle); }
  RTCRtpReceiver receiver() const { return RTCRtpReceiver(nativeHandle); }
  std::optional<std::string> mid() const;
  std::string direction() const;
  void setDirection(const std::string&) const;
  std::optional<std::string> currentDirection() const;
  bool stopped() const;
  void stop() const;
 private:
  std::shared_ptr<rtc::PeerLifetime> lifetime_;
};
class RTCTrackEvent {
 public:
  RTCTrackEvent() = default;
  RTCTrackEvent(MediaStreamTrack track, RTCRtpTransceiver transceiver, std::vector<MediaStream> streams)
      : fields_(std::make_shared<const Fields>(Fields{track, transceiver, std::move(streams)})) {}
  MediaStreamTrack track() const { return fields().track; }
  RTCRtpTransceiver transceiver() const { return fields().transceiver; }
  RTCRtpReceiver receiver() const { return transceiver().receiver(); }
  const std::vector<MediaStream>& streams() const { return fields().streams; }
  bool operator==(const RTCTrackEvent& other) const { return fields_ == other.fields_; }
 private:
  struct Fields {
    MediaStreamTrack track;
    RTCRtpTransceiver transceiver;
    std::vector<MediaStream> streams;
  };
  const Fields& fields() const {
    if (!fields_) throw std::logic_error("null RTCTrackEvent");
    return *fields_;
  }
  std::shared_ptr<const Fields> fields_;
};
namespace rtc {
struct DataChannelCallbacks {
  std::function<void()> on_open, on_close, on_buffered_amount_low;
  std::function<void(const std::string&)> on_error;
  std::function<void(const std::vector<uint8_t>&, bool)> on_message;
};
DataChannelCallbacks& channel_callbacks(uint32_t channel);
void enqueue_channel_open(uint32_t peer, const std::string& label, uint16_t stream);
void enqueue_channel_close(uint32_t peer, uint16_t stream);
void enqueue_channel_message(uint32_t peer, uint16_t stream, const uint8_t* bytes, size_t size, bool text);
RTCConfiguration configuration(uint32_t peer);
}
}
