// SPDX-License-Identifier: Apache-2.0
#include "host/rtc.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <mutex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#endif

namespace gea::host {

namespace rtc {
struct PeerLifetime {
  explicit PeerLifetime(uint32_t id) : handle(id) {}
  ~PeerLifetime() { destroy_handle(handle); }
  uint32_t handle;
};
}

namespace {

struct PendingDescription {
  NativeRtcPeerHandle peer;
  std::shared_ptr<rtc::DescriptionResult> result;
  rtc::DescriptionCallback callback;
  std::shared_ptr<rtc::PeerLifetime> owner;
};
// Owned and dispatched only by the UI thread. Workers publish native strings
// through DescriptionResult; they never touch a JS promise or callback.
std::vector<PendingDescription> pendingDescriptions;

struct PeerState {
  std::string connectionState = "new";
  std::string iceConnectionState = "new";
  bool open = true;
  RTCConfiguration config;
  std::string signaling = "stable";
  std::optional<RTCSessionDescriptionObject> local, remote;
  std::vector<uint32_t> transceivers;
};

std::unordered_map<NativeRtcPeerHandle, PeerState> &peerTable() {
  // Generated module globals can release their final RTC wrapper after C++
  // function statics have been destroyed. Registry storage must outlive those
  // wrappers; per-peer entries are still erased by destroy_handle as usual.
  static auto* table = new std::unordered_map<NativeRtcPeerHandle, PeerState>();
  return *table;
}

std::mutex &peerMutex() {
  static auto* m = new std::mutex();
  return *m;
}

// Separate from peerMutex: object wrappers are also constructed while the
// state registry is locked. These registries hold only weak references.
std::mutex& lifetimeMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}
using LifetimeTable = std::unordered_map<uint32_t, std::weak_ptr<rtc::PeerLifetime>>;
LifetimeTable& peerLifetimeTable() {
  static auto* table = new LifetimeTable();
  return *table;
}
LifetimeTable& objectLifetimeTable() {
  static auto* table = new LifetimeTable();
  return *table;
}
void registerObjectLifetime(uint32_t object, uint32_t peer) {
  std::lock_guard<std::mutex> lock(lifetimeMutex());
  const auto it = peerLifetimeTable().find(peer);
  if (it != peerLifetimeTable().end()) objectLifetimeTable()[object] = it->second;
}

std::atomic<NativeRtcPeerHandle> &nextHandle() {
  static std::atomic<NativeRtcPeerHandle> id{1};
  return id;
}

struct TransceiverState {
  uint32_t peer = 0;
  NativeMediaTrackHandle local = 0, remote = 0;
  std::string direction = "sendrecv";
  std::optional<std::string> mid, current;
  bool stopped = false;
  std::vector<MediaStream> streams;
  std::string kind = "audio";
};
struct ChannelState {
  uint32_t peer = 0;
  std::string label, state = "connecting", binary_type = "arraybuffer";
  RTCDataChannelInit config;
  std::optional<uint16_t> stream;
  double buffered = 0, low_threshold = 0;
};
auto& transceiverTable() {
  static auto* table = new std::unordered_map<uint32_t, TransceiverState>();
  return *table;
}
auto& channelTable() {
  static auto* table = new std::unordered_map<uint32_t, ChannelState>();
  return *table;
}
auto& channelCallbackTable() {
  static auto* table = new std::unordered_map<uint32_t, rtc::DataChannelCallbacks>();
  return *table;
}
std::atomic<uint32_t> nextObject{1};
void requireDirection(const std::string& direction) {
  if (direction != "sendrecv" && direction != "sendonly" && direction != "recvonly" && direction != "inactive")
    throw std::invalid_argument("Invalid RTCRtpTransceiver direction");
}
struct MediaSection {
  std::string mid, direction = "sendrecv";
  bool found = false, rejected = false;
};
MediaSection mediaSection(const std::string& sdp, const std::string& kind) {
  MediaSection section;
  bool inSection = false, inMedia = false;
  std::string sessionDirection = "sendrecv", line;
  std::istringstream lines(sdp);
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("m=", 0) == 0) {
      if (inSection) break;
      inMedia = true;
      inSection = line.rfind("m=" + kind + " ", 0) == 0;
      if (inSection) { section.found = true; section.rejected = line.rfind("m=" + kind + " 0 ", 0) == 0; section.direction = sessionDirection; }
    } else if (line == "a=sendrecv" || line == "a=recvonly" || line == "a=sendonly" || line == "a=inactive") {
      if (!inMedia) sessionDirection = line.substr(2);
      if (inSection) section.direction = line.substr(2);
    } else if (inSection && line.rfind("a=mid:", 0) == 0) section.mid = line.substr(6);
  }
  return section;
}
// Called with peerMutex held, only when an answer has actually been accepted.
void applyNegotiatedMedia(PeerState& peer) {
  if (peer.signaling != "stable" || !peer.local || !peer.remote) return;
  for (auto id : peer.transceivers) {
    auto& tr = transceiverTable().at(id);
    if (tr.stopped) continue;
    const auto local = mediaSection(peer.local->sdp(), tr.kind), remote = mediaSection(peer.remote->sdp(), tr.kind);
    if (!local.found || !remote.found) continue;
    const bool send = !local.rejected && !remote.rejected && (local.direction == "sendrecv" || local.direction == "sendonly") && (remote.direction == "sendrecv" || remote.direction == "recvonly");
    const bool receive = !local.rejected && !remote.rejected && (local.direction == "sendrecv" || local.direction == "recvonly") && (remote.direction == "sendrecv" || remote.direction == "sendonly");
    tr.mid = local.mid;
    tr.current = send ? (receive ? "sendrecv" : "sendonly") : (receive ? "recvonly" : "inactive");
  }
}
PeerState& requirePeer(uint32_t id) {
  auto it = peerTable().find(id);
  if (it == peerTable().end() || !it->second.open) throw std::runtime_error("InvalidStateError: peer is closed");
  return it->second;
}
TransceiverState& requireTransceiver(uint32_t id) {
  auto it = transceiverTable().find(id);
  if (it == transceiverTable().end()) throw std::runtime_error("InvalidStateError: transceiver is gone");
  requirePeer(it->second.peer);
  return it->second;
}
ChannelState& requireChannel(uint32_t id) {
  auto it = channelTable().find(id);
  if (it == channelTable().end()) throw std::runtime_error("InvalidStateError: data channel is gone");
  return it->second;
}

enum class EventKind { IceCandidate, Track, ConnectionStateChange, IceConnectionStateChange, Signaling, Negotiation, DataChannel, ChannelOpen, ChannelClose, ChannelMessage, ChannelLow };

struct PendingEvent {
  NativeRtcPeerHandle handle = 0;
  EventKind kind = EventKind::ConnectionStateChange;
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;
  NativeMediaTrackHandle track = 0;
  uint32_t channel = 0;
  std::vector<uint8_t> bytes;
  bool text = false;
  RTCTrackEvent track_event;
#ifdef ESP_PLATFORM
  int64_t queued_us = 0;
#endif
};

std::mutex &queueMutex() {
  static auto* m = new std::mutex();
  return *m;
}

std::vector<PendingEvent> &eventQueue() {
  static std::vector<PendingEvent> q;
  return q;
}

void enqueue_internal(PendingEvent ev) {
#ifdef ESP_PLATFORM
  ev.queued_us = esp_timer_get_time();
#endif
  std::lock_guard<std::mutex> lock(queueMutex());
  eventQueue().push_back(std::move(ev));
}

}  // namespace

namespace rtc {

std::unordered_map<NativeRtcPeerHandle, CallbackTable> &callbackTable() {
  static auto* table = new std::unordered_map<NativeRtcPeerHandle, CallbackTable>();
  return *table;
}

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)
void platform_request_description(NativeRtcPeerHandle, bool, std::shared_ptr<DescriptionResult>);
void platform_create(NativeRtcPeerHandle, const RTCConfiguration &);
void platform_direction(NativeRtcPeerHandle, const std::string&, const std::string&);
void platform_create_channel(NativeRtcPeerHandle, const std::string&, const RTCDataChannelInit&);
void platform_send_data(NativeRtcPeerHandle, uint16_t, const uint8_t*, size_t, bool);
void platform_close_channel(NativeRtcPeerHandle, const std::string&);
void platform_destroy(NativeRtcPeerHandle);
void platform_add_track(NativeRtcPeerHandle, NativeMediaTrackHandle);
std::string platform_create_offer(NativeRtcPeerHandle);
std::string platform_create_answer(NativeRtcPeerHandle);
void platform_set_local_description(NativeRtcPeerHandle, const std::string &, const std::string &);
void platform_set_remote_description(NativeRtcPeerHandle, const std::string &, const std::string &);
void platform_add_ice_candidate(NativeRtcPeerHandle, const std::string &, const std::string &, int);
void platform_close(NativeRtcPeerHandle);
#else
__attribute__((weak)) void platform_create(NativeRtcPeerHandle, const RTCConfiguration &) {}
__attribute__((weak)) void platform_direction(NativeRtcPeerHandle, const std::string&, const std::string&) {}
__attribute__((weak)) void platform_create_channel(NativeRtcPeerHandle, const std::string&, const RTCDataChannelInit&) { throw std::runtime_error("NotSupportedError: no data channel backend"); }
__attribute__((weak)) void platform_send_data(NativeRtcPeerHandle, uint16_t, const uint8_t*, size_t, bool) { throw std::runtime_error("NotSupportedError: no data channel backend"); }
__attribute__((weak)) void platform_close_channel(NativeRtcPeerHandle, const std::string&) {}
__attribute__((weak)) void platform_destroy(NativeRtcPeerHandle) {}
__attribute__((weak)) void platform_add_track(NativeRtcPeerHandle, NativeMediaTrackHandle) {}
__attribute__((weak)) std::string platform_create_offer(NativeRtcPeerHandle) { return "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\n"; }
__attribute__((weak)) std::string platform_create_answer(NativeRtcPeerHandle) { return "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\n"; }
__attribute__((weak)) void platform_set_local_description(NativeRtcPeerHandle, const std::string &, const std::string &) {}
__attribute__((weak)) void platform_set_remote_description(NativeRtcPeerHandle, const std::string &, const std::string &) {}
__attribute__((weak)) void platform_add_ice_candidate(NativeRtcPeerHandle, const std::string &, const std::string &, int) {}
__attribute__((weak)) void platform_close(NativeRtcPeerHandle) {}
__attribute__((weak)) void platform_request_description(NativeRtcPeerHandle id, bool offer, std::shared_ptr<DescriptionResult> result) {
  try { result->sdp = offer ? platform_create_offer(id) : platform_create_answer(id); }
  catch (const std::exception& error) { result->error = error.what(); }
  result->ready.store(true, std::memory_order_release);
}

#endif

NativeRtcPeerHandle create_handle() { return create_handle(RTCConfiguration{}); }
std::shared_ptr<PeerLifetime> retain_peer(uint32_t id) {
  if (!id) return {};
  std::lock_guard<std::mutex> lock(lifetimeMutex());
  auto& weak = peerLifetimeTable()[id];
  auto owner = weak.lock();
  if (!owner) { owner = std::make_shared<PeerLifetime>(id); weak = owner; }
  return owner;
}
std::shared_ptr<PeerLifetime> retain_object(uint32_t id) {
  std::lock_guard<std::mutex> lock(lifetimeMutex());
  const auto it = objectLifetimeTable().find(id);
  return it == objectLifetimeTable().end() ? nullptr : it->second.lock();
}
NativeRtcPeerHandle create_handle_with_config(const std::string &json) {
  if (!json.empty()) throw std::invalid_argument("Use typed RTCConfiguration; JSON ICE configuration is not supported");
  return create_handle();
}
NativeRtcPeerHandle create_handle(const RTCConfiguration& config) {
  if (config.iceTransportPolicy != "all" && config.iceTransportPolicy != "relay")
    throw std::invalid_argument("Invalid ICE transport policy");
  auto handle = nextHandle()++;
  { std::lock_guard<std::mutex> lock(peerMutex()); peerTable()[handle].config = config; }
  try { platform_create(handle, config); }
  catch (...) { std::lock_guard<std::mutex> lock(peerMutex()); peerTable().erase(handle); throw; }
  return handle;
}
RTCConfiguration configuration(uint32_t handle) {
  std::lock_guard<std::mutex> lock(peerMutex());
  return requirePeer(handle).config;
}
void destroy_handle(NativeRtcPeerHandle handle) {
  platform_destroy(handle);
  // Destroy callback captures AFTER releasing registry locks: a callback may
  // own the final peer/channel wrapper and reenter this cleanup.
  CallbackTable discardedPeer;
  std::vector<DataChannelCallbacks> discardedChannels;
  {
  std::lock_guard<std::mutex> lock(peerMutex());
  std::lock_guard<std::mutex> owners(lifetimeMutex());
  for (auto it = transceiverTable().begin(); it != transceiverTable().end();) {
    if (it->second.peer == handle) {
      objectLifetimeTable().erase(it->first);
      it = transceiverTable().erase(it);
    } else ++it;
  }
  for (auto it = channelTable().begin(); it != channelTable().end();) {
    if (it->second.peer == handle) {
      auto cb = channelCallbackTable().find(it->first);
      if (cb != channelCallbackTable().end()) {
        discardedChannels.push_back(std::move(cb->second)); channelCallbackTable().erase(cb);
      }
      objectLifetimeTable().erase(it->first);
      it = channelTable().erase(it);
    } else ++it;
  }
  peerTable().erase(handle);
  auto cb = callbackTable().find(handle);
  if (cb != callbackTable().end()) {
    discardedPeer = std::move(cb->second); callbackTable().erase(cb);
  }
  peerLifetimeTable().erase(handle);
  }
}

void runCallbacks() {
  std::vector<PendingDescription> completed;
  for (auto it = pendingDescriptions.begin(); it != pendingDescriptions.end();) {
    bool closed;
    {
      std::lock_guard<std::mutex> lock(peerMutex());
      const auto peer = peerTable().find(it->peer);
      closed = peer == peerTable().end() || !peer->second.open;
    }
    if (closed) {
      // Do not mutate the worker's result while it is still publishing.
      auto cancelled = std::make_shared<DescriptionResult>();
      cancelled->error = "AbortError: peer closed during SDP creation";
      it->result = std::move(cancelled);
    }
    if (closed || it->result->ready.load(std::memory_order_acquire)) {
      completed.push_back(std::move(*it));
      it = pendingDescriptions.erase(it);
    } else ++it;
  }
  for (auto& operation : completed) {
    if (operation.result->sdp.empty() && operation.result->error.empty())
      operation.result->error = "OperationError: SDP creation returned no description";
    operation.callback(operation.result->sdp, operation.result->error);
  }

  std::vector<PendingEvent> drained;
  {
    std::lock_guard<std::mutex> lock(queueMutex());
    drained.swap(eventQueue());
  }
#ifdef ESP_PLATFORM
  if (!drained.empty()) {
    const auto delay_us = esp_timer_get_time() - drained.front().queued_us;
    if (delay_us > 100000)
      ESP_LOGW("gea::host::rtc", "event dispatch delayed: oldest_ms=%lld events=%u",
        static_cast<long long>(delay_us / 1000), unsigned(drained.size()));
  }
#endif
  for (const auto &ev : drained) {
    // A handler may close/destroy the peer. Copy callbacks before invoking
    // user code; never keep an unordered_map iterator alive across a callback.
    if (ev.channel && ev.kind != EventKind::DataChannel) {
      auto it = channelCallbackTable().find(ev.channel);
      if (it == channelCallbackTable().end()) continue;
      auto cb = it->second;
      if (ev.kind == EventKind::ChannelOpen && cb.on_open) cb.on_open();
      if (ev.kind == EventKind::ChannelClose && cb.on_close) cb.on_close();
      if (ev.kind == EventKind::ChannelLow && cb.on_buffered_amount_low) cb.on_buffered_amount_low();
      if (ev.kind == EventKind::ChannelMessage && cb.on_message) cb.on_message(ev.bytes, ev.text);
      continue;
    }
    auto it = callbackTable().find(ev.handle);
    if (it == callbackTable().end()) continue;
    auto cb = it->second;
#ifdef ESP_PLATFORM
    ESP_LOGI("gea::host::rtc", "dispatch begin peer=%u kind=%u", unsigned(ev.handle), unsigned(ev.kind));
#endif
    switch (ev.kind) {
      case EventKind::IceCandidate: if (cb.on_ice_candidate) cb.on_ice_candidate(ev.candidate, ev.sdp_mid, ev.sdp_mline_index); break;
      case EventKind::Track: if (cb.on_track) cb.on_track(ev.track_event); break;
      case EventKind::ConnectionStateChange: if (cb.on_connection_state_change) cb.on_connection_state_change(); break;
      case EventKind::IceConnectionStateChange: if (cb.on_ice_connection_state_change) cb.on_ice_connection_state_change(); break;
      case EventKind::Signaling: if (cb.on_signaling_state_change) cb.on_signaling_state_change(); break;
      case EventKind::Negotiation: if (cb.on_negotiation_needed) cb.on_negotiation_needed(); break;
      case EventKind::DataChannel: if (cb.on_data_channel) cb.on_data_channel(RTCDataChannel(ev.channel)); break;
      default: break;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI("gea::host::rtc", "dispatch end peer=%u kind=%u", unsigned(ev.handle), unsigned(ev.kind));
#endif
  }
  // close() terminates transport immediately. Deliver its queued close events
  // first, then release subscriptions that can retain the peer through a JS
  // closure. Native state stays available while any peer/RTP/channel object
  // is still referenced; the last wrapper releases that state automatically.
  std::vector<CallbackTable> closedPeers;
  std::vector<DataChannelCallbacks> closedChannels;
  std::vector<uint32_t> pendingPeers;
  {
    std::lock_guard<std::mutex> lock(queueMutex());
    for (const auto& event : eventQueue()) pendingPeers.push_back(event.handle);
  }
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    for (const auto& [id, peer] : peerTable()) if (!peer.open) {
      // A handler may have closed its own peer during this dispatch. Its
      // resulting events are in the next batch and must keep their callbacks.
      if (std::find(pendingPeers.begin(), pendingPeers.end(), id) != pendingPeers.end()) continue;
      auto cb = callbackTable().find(id);
      if (cb != callbackTable().end()) {
        closedPeers.push_back(std::move(cb->second)); callbackTable().erase(cb);
      }
      for (const auto& [channelId, channel] : channelTable()) if (channel.peer == id) {
        auto ch = channelCallbackTable().find(channelId);
        if (ch != channelCallbackTable().end()) {
          closedChannels.push_back(std::move(ch->second)); channelCallbackTable().erase(ch);
        }
      }
    }
  }
}

void enqueue_ice_candidate(NativeRtcPeerHandle h, const std::string &candidate, const std::string &sdp_mid, int idx) {
  enqueue_internal(PendingEvent{h, EventKind::IceCandidate, candidate, sdp_mid, idx, 0});
}
void enqueue_track(NativeRtcPeerHandle h, NativeMediaTrackHandle t, std::vector<MediaStream> streams) {
  const auto kind = MediaStreamTrack(t).kind();
  if (kind != "audio" && kind != "video") throw std::invalid_argument("Unknown received media track kind");
  uint32_t transceiver = 0;
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    auto peer = peerTable().find(h);
    if (peer == peerTable().end() || !peer->second.open) return;
    for (const auto id : peer->second.transceivers) {
      const auto& candidate = transceiverTable().at(id);
      if (!candidate.stopped && candidate.kind == kind) {
        transceiver = id;
        break;
      }
    }
    if (!transceiver) {
      const auto id = nextObject++;
      transceiverTable()[id] = TransceiverState{h, 0, t, "recvonly"};
      transceiverTable()[id].kind = kind;
      registerObjectLifetime(id, h);
      peer->second.transceivers.push_back(id);
      transceiver = id;
    } else transceiverTable().at(transceiver).remote = t;
    applyNegotiatedMedia(peer->second);
  }
  PendingEvent event{h, EventKind::Track, {}, {}, 0, t};
  event.track_event = RTCTrackEvent(MediaStreamTrack(t), RTCRtpTransceiver(transceiver), std::move(streams));
  enqueue_internal(std::move(event));
}
void enqueue_connection_state_change(NativeRtcPeerHandle h, const std::string &new_state) {
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    auto it = peerTable().find(h);
    if (it == peerTable().end() || !it->second.open) return;
    it->second.connectionState = new_state;
  }
  enqueue_internal(PendingEvent{h, EventKind::ConnectionStateChange, {}, {}, 0, 0});
}
void enqueue_ice_connection_state_change(NativeRtcPeerHandle h, const std::string &new_state) {
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    auto it = peerTable().find(h);
    if (it == peerTable().end() || !it->second.open) return;
    it->second.iceConnectionState = new_state;
  }
  enqueue_internal(PendingEvent{h, EventKind::IceConnectionStateChange, {}, {}, 0, 0});
}

}  // namespace rtc

std::string RTCPeerConnection::connectionState() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = peerTable().find(nativeHandle);
  return it == peerTable().end() ? std::string("closed") : it->second.connectionState;
}

std::string RTCPeerConnection::iceConnectionState() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = peerTable().find(nativeHandle);
  return it == peerTable().end() ? std::string("closed") : it->second.iceConnectionState;
}

RTCRtpSender RTCPeerConnection::addTrack(MediaStreamTrack track, MediaStream stream) const {
  return addTrack(track, stream.nativeHandle ? std::vector<MediaStream>{stream} : std::vector<MediaStream>{});
}

RTCRtpSender RTCPeerConnection::addTrack(MediaStreamTrack track, const std::vector<MediaStream>& streams) const {
  for (const auto stream : streams)
    if (!stream.nativeHandle) throw std::invalid_argument("TypeError: null MediaStream");
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    const auto& peer = requirePeer(nativeHandle);
    for (const auto id : peer.transceivers) {
      const auto& sender = transceiverTable().at(id);
      if (!sender.stopped && sender.local == track.nativeHandle)
        throw std::invalid_argument("InvalidAccessError: track is already attached");
    }
  }
  const auto sender = addTransceiver(track).sender();
  std::lock_guard<std::mutex> lock(peerMutex());
  auto& state = requireTransceiver(sender.nativeHandle);
  for (const auto stream : streams) {
    if (std::none_of(state.streams.begin(), state.streams.end(), [&](const auto& prior) { return prior.nativeHandle == stream.nativeHandle; }))
      state.streams.push_back(stream);
  }
  return sender;
}

namespace {
std::string describeLocalStreams(uint32_t handle, const std::string& sdp) {
  if (sdp.empty()) return sdp;
  std::string attributes;
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    for (const auto id : requirePeer(handle).transceivers) {
      const auto& sender = transceiverTable().at(id);
      if (sender.stopped || !sender.local) continue;
      const auto track = MediaStreamTrack(sender.local).id();
      if (sender.streams.empty()) attributes += "a=msid:- " + track + "\r\n";
      for (const auto stream : sender.streams) attributes += "a=msid:" + stream.id() + " " + track + "\r\n";
    }
  }
  if (attributes.empty()) return sdp;
  std::istringstream input(sdp);
  std::string output, line;
  bool audio = false, media = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("a=msid-semantic:", 0) == 0) continue;
    if (!media && line.rfind("m=", 0) == 0) {
      output += "a=msid-semantic: WMS *\r\n";
      media = true;
    }
    if (line.rfind("m=", 0) == 0) audio = line.rfind("m=audio ", 0) == 0;
    if (audio && (line.rfind("a=msid:", 0) == 0 || (line.rfind("a=ssrc:", 0) == 0 && line.find(" msid:") != std::string::npos))) continue;
    output += line + "\r\n";
    if (line.rfind("m=audio ", 0) == 0) output += attributes;
  }
  return output;
}
}

void RTCPeerConnection::requestDescription(bool offer, rtc::DescriptionCallback callback) const {
  { std::lock_guard<std::mutex> lock(peerMutex()); requirePeer(nativeHandle); }
  auto result = std::make_shared<rtc::DescriptionResult>();
  rtc::platform_request_description(nativeHandle, offer, result);
  pendingDescriptions.push_back({nativeHandle, result,
    [handle = nativeHandle, callback = std::move(callback)](const std::string& sdp, const std::string& error) {
      bool closed;
      {
        std::lock_guard<std::mutex> lock(peerMutex());
        const auto peer = peerTable().find(handle);
        closed = peer == peerTable().end() || !peer->second.open;
      }
      if (closed) callback({}, "AbortError: peer closed during SDP creation");
      else callback(error.empty() ? describeLocalStreams(handle, sdp) : std::string{}, error);
    }, lifetime_});
}

std::string RTCPeerConnection::createOffer() const {
  return describeLocalStreams(nativeHandle, rtc::platform_create_offer(nativeHandle));
}

std::string RTCPeerConnection::createAnswer() const {
  return describeLocalStreams(nativeHandle, rtc::platform_create_answer(nativeHandle));
}

void RTCPeerConnection::setLocalDescription(const std::string &sdp_type, const std::string &sdp) const {
  { std::lock_guard<std::mutex> lock(peerMutex()); requirePeer(nativeHandle); }
  if (sdp_type != "offer" && sdp_type != "answer") throw std::invalid_argument("NotSupportedError: SDP type");
  rtc::platform_set_local_description(nativeHandle, sdp_type, sdp);
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& p = requirePeer(nativeHandle);
    p.local = RTCSessionDescriptionObject(RTCSessionDescription{sdp_type, sdp}); p.signaling = sdp_type == "offer" ? "have-local-offer" : "stable";
    applyNegotiatedMedia(p); }
  enqueue_internal(PendingEvent{nativeHandle, EventKind::Signaling});
}

void RTCPeerConnection::setRemoteDescription(const std::string &sdp_type, const std::string &sdp) const {
  { std::lock_guard<std::mutex> lock(peerMutex()); requirePeer(nativeHandle); }
  if (sdp_type != "offer" && sdp_type != "answer") throw std::invalid_argument("NotSupportedError: SDP type");
  rtc::platform_set_remote_description(nativeHandle, sdp_type, sdp);
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& p = requirePeer(nativeHandle);
    p.remote = RTCSessionDescriptionObject(RTCSessionDescription{sdp_type, sdp}); p.signaling = sdp_type == "offer" ? "have-remote-offer" : "stable";
    applyNegotiatedMedia(p); }
  enqueue_internal(PendingEvent{nativeHandle, EventKind::Signaling});
}

void RTCPeerConnection::addIceCandidate(const std::string &candidate, const std::string &sdp_mid, double sdp_mline_index) const {
  rtc::platform_add_ice_candidate(nativeHandle, candidate, sdp_mid, static_cast<int>(sdp_mline_index));
}

void RTCPeerConnection::close() const {
  {
    std::lock_guard<std::mutex> lock(peerMutex());
    auto it = peerTable().find(nativeHandle);
    if (it != peerTable().end()) {
      it->second.open = false;
      it->second.signaling = "closed";
      for (auto id : it->second.transceivers) { transceiverTable()[id].stopped = true; transceiverTable()[id].current.reset(); }
      for (auto& [id, ch] : channelTable()) if (ch.peer == nativeHandle && ch.state != "closed") {
        ch.state = "closed"; PendingEvent ev{nativeHandle, EventKind::ChannelClose}; ev.channel = id; enqueue_internal(std::move(ev));
      }
      it->second.connectionState = "closed";
      it->second.iceConnectionState = "closed";
    }
  }
  rtc::platform_close(nativeHandle);
}

#include "rtc_objects.inc"

}  // namespace gea::host
