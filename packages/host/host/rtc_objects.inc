// Included by rtc.cpp: one state registry and one event queue for every RTC handle.
RTCConfiguration RTCPeerConnection::getConfiguration() const { return rtc::configuration(nativeHandle); }
std::string RTCPeerConnection::signalingState() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = peerTable().find(nativeHandle);
  return it == peerTable().end() ? "closed" : it->second.signaling;
}
std::optional<RTCSessionDescriptionObject> RTCPeerConnection::localDescription() const {
  std::lock_guard<std::mutex> lock(peerMutex()); return requirePeer(nativeHandle).local;
}
std::optional<RTCSessionDescriptionObject> RTCPeerConnection::remoteDescription() const {
  std::lock_guard<std::mutex> lock(peerMutex()); return requirePeer(nativeHandle).remote;
}
namespace {
RTCRtpTransceiver addMediaTransceiver(uint32_t nativeHandle, const std::string& kind,
                                    MediaStreamTrack track, const RTCRtpTransceiverInit& init) {
  requireDirection(init.direction);
  if (kind != "audio" && kind != "video") throw std::invalid_argument("TypeError: invalid transceiver kind");
  if (kind == "video" && (track.nativeHandle || init.direction == "sendonly" || init.direction == "sendrecv"))
    throw std::invalid_argument("NotSupportedError: native video transmission is not implemented");
  { std::lock_guard<std::mutex> lock(peerMutex());
    auto& peer = requirePeer(nativeHandle);
    for (auto id : peer.transceivers) if (!transceiverTable().at(id).stopped && transceiverTable().at(id).kind == kind)
      throw std::runtime_error("NotSupportedError: ESP peer supports one transceiver of each media kind");
  }
  rtc::platform_direction(nativeHandle, kind, init.direction);
  if (track.nativeHandle) rtc::platform_add_track(nativeHandle, track.nativeHandle);
  uint32_t id = nextObject++;
  { std::lock_guard<std::mutex> lock(peerMutex());
    transceiverTable()[id] = TransceiverState{nativeHandle, track.nativeHandle, 0, init.direction};
    transceiverTable()[id].kind = kind;
    registerObjectLifetime(id, nativeHandle);
    requirePeer(nativeHandle).transceivers.push_back(id);
  }
  enqueue_internal(PendingEvent{nativeHandle, EventKind::Negotiation});
  return RTCRtpTransceiver(id);
}
} // namespace
RTCRtpTransceiver RTCPeerConnection::addTransceiver(const std::string& kind, const RTCRtpTransceiverInit& init) const {
  return addMediaTransceiver(nativeHandle, kind, MediaStreamTrack{}, init);
}
RTCRtpTransceiver RTCPeerConnection::addTransceiver(MediaStreamTrack track, const RTCRtpTransceiverInit& init) const {
  return addMediaTransceiver(nativeHandle, track.nativeHandle ? track.kind() : "audio", track, init);
}
std::vector<RTCRtpTransceiver> RTCPeerConnection::getTransceivers() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  std::vector<RTCRtpTransceiver> result;
  auto it = peerTable().find(nativeHandle);
  if (it != peerTable().end()) for (auto id : it->second.transceivers) result.emplace_back(id);
  return result;
}
std::vector<RTCRtpSender> RTCPeerConnection::getSenders() const {
  std::vector<RTCRtpSender> result;
  for (auto tr : getTransceivers()) if (!tr.stopped()) result.push_back(tr.sender());
  return result;
}
std::vector<RTCRtpReceiver> RTCPeerConnection::getReceivers() const {
  std::vector<RTCRtpReceiver> result;
  for (auto tr : getTransceivers()) if (!tr.stopped()) result.push_back(tr.receiver());
  return result;
}
void RTCPeerConnection::removeTrack(RTCRtpSender sender) const {
  std::string direction;
  { std::lock_guard<std::mutex> lock(peerMutex());
    requirePeer(nativeHandle);
    auto& tr = requireTransceiver(sender.nativeHandle);
    if (tr.peer != nativeHandle) throw std::invalid_argument("InvalidAccessError: sender belongs to another peer");
    if (!tr.local || tr.stopped) return;
    direction = tr.direction == "sendrecv" ? "recvonly" : tr.direction == "sendonly" ? "inactive" : tr.direction;
  }
  RTCRtpTransceiver(sender.nativeHandle).setDirection(direction);
  sender.replaceTrack();
}
std::optional<MediaStreamTrack> RTCRtpSender::track() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = transceiverTable().find(nativeHandle);
  if (it == transceiverTable().end() || !it->second.local) return std::nullopt;
  return MediaStreamTrack(it->second.local);
}
void RTCRtpSender::replaceTrack() const { replaceTrack(MediaStreamTrack{}); }
void RTCRtpSender::replaceTrack(MediaStreamTrack track) const {
  if (track.nativeHandle && track.kind() != "audio") throw std::invalid_argument("TypeError: replacement track kind differs");
  uint32_t peer;
  std::string kind;
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& tr = requireTransceiver(nativeHandle);
    if (tr.stopped) throw std::runtime_error("InvalidStateError: transceiver stopped");
    if (track.nativeHandle && tr.kind != track.kind()) throw std::invalid_argument("TypeError: replacement track kind differs");
    peer = tr.peer; kind = tr.kind; }
  if (kind == "audio") rtc::platform_add_track(peer, track.nativeHandle);
  { std::lock_guard<std::mutex> lock(peerMutex()); requireTransceiver(nativeHandle).local = track.nativeHandle; }
}
std::optional<MediaStreamTrack> RTCRtpReceiver::track() const {
  std::lock_guard<std::mutex> lock(peerMutex());
  auto it = transceiverTable().find(nativeHandle);
  if (it == transceiverTable().end() || !it->second.remote) return std::nullopt;
  return MediaStreamTrack(it->second.remote);
}
std::optional<std::string> RTCRtpTransceiver::mid() const {
  std::lock_guard<std::mutex> lock(peerMutex()); auto it = transceiverTable().find(nativeHandle);
  return it == transceiverTable().end() ? std::nullopt : it->second.mid;
}
std::optional<std::string> RTCRtpTransceiver::currentDirection() const {
  std::lock_guard<std::mutex> lock(peerMutex()); auto it = transceiverTable().find(nativeHandle);
  return it == transceiverTable().end() ? std::nullopt : it->second.current;
}
std::string RTCRtpTransceiver::direction() const {
  std::lock_guard<std::mutex> lock(peerMutex()); return requireTransceiver(nativeHandle).direction;
}
bool RTCRtpTransceiver::stopped() const {
  std::lock_guard<std::mutex> lock(peerMutex()); auto it = transceiverTable().find(nativeHandle);
  return it == transceiverTable().end() || it->second.stopped;
}
void RTCRtpTransceiver::setDirection(const std::string& direction) const {
  requireDirection(direction);
  uint32_t peer;
  std::string kind;
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& tr = requireTransceiver(nativeHandle);
    if (tr.stopped) throw std::runtime_error("InvalidStateError: transceiver stopped");
    if (tr.direction == direction) return;
    peer = tr.peer; kind = tr.kind;
  }
  if (kind == "video" && direction != "recvonly" && direction != "inactive")
    throw std::invalid_argument("NotSupportedError: native video transmission is not implemented");
  rtc::platform_direction(peer, kind, direction);
  { std::lock_guard<std::mutex> lock(peerMutex()); requireTransceiver(nativeHandle).direction = direction; }
  enqueue_internal(PendingEvent{peer, EventKind::Negotiation});
}
void RTCRtpTransceiver::stop() const {
  if (stopped()) return;
  setDirection("inactive");
  sender().replaceTrack();
  std::lock_guard<std::mutex> lock(peerMutex()); auto& tr = requireTransceiver(nativeHandle);
  tr.stopped = true; tr.current.reset();
}
namespace rtc {
DataChannelCallbacks& channel_callbacks(uint32_t channel) { return channelCallbackTable()[channel]; }
void enqueue_channel_open(uint32_t peer, const std::string& label, uint16_t stream) {
  uint32_t id = 0;
  bool remote = false;
  { std::lock_guard<std::mutex> lock(peerMutex());
    auto owner = peerTable().find(peer);
    if (owner == peerTable().end() || !owner->second.open) return;
    for (const auto& [key, ch] : channelTable()) if (ch.peer == peer && ch.label == label && ch.state == "connecting") { id = key; break; }
    if (!id) { id = nextObject++; channelTable()[id].peer = peer; channelTable()[id].label = label; remote = true; registerObjectLifetime(id, peer); }
    channelTable()[id].stream = stream; channelTable()[id].state = "open";
  }
  PendingEvent ev{peer, remote ? EventKind::DataChannel : EventKind::ChannelOpen}; ev.channel = id;
  enqueue_internal(std::move(ev));
}
void enqueue_channel_close(uint32_t peer, uint16_t stream) {
  std::lock_guard<std::mutex> lock(peerMutex());
  for (auto& [id, ch] : channelTable()) if (ch.peer == peer && ch.stream == stream && ch.state != "closed") {
    ch.state = "closed";
    PendingEvent ev{peer, EventKind::ChannelClose}; ev.channel = id; enqueue_internal(std::move(ev)); break;
  }
}
void enqueue_channel_message(uint32_t peer, uint16_t stream, const uint8_t* bytes, size_t size, bool text) {
  if (!bytes && size) return;
  std::lock_guard<std::mutex> lock(peerMutex());
  for (const auto& [id, ch] : channelTable()) if (ch.peer == peer && ch.stream == stream && ch.state == "open") {
    PendingEvent ev{peer, EventKind::ChannelMessage}; ev.channel = id; ev.text = text;
    if (size) ev.bytes.assign(bytes, bytes + size);
    enqueue_internal(std::move(ev)); break;
  }
}
}
RTCDataChannel RTCPeerConnection::createDataChannel(const std::string& label, const RTCDataChannelInit& init) const {
  const auto valid16 = [](const std::optional<double>& value) { return !value || (std::isfinite(*value) && *value >= 0 && *value <= 65535 && std::floor(*value) == *value); };
  if (label.size() > 65535 || init.protocol.size() > 65535 || !valid16(init.maxPacketLifeTime) || !valid16(init.maxRetransmits) || !valid16(init.id))
    throw std::invalid_argument("TypeError: invalid data channel parameters");
  if (init.maxPacketLifeTime && init.maxRetransmits) throw std::invalid_argument("TypeError: choose lifetime or retransmits");
  // esp_peer's DCEP API cannot set these; do not pretend it did.
  if (init.negotiated || init.id || !init.protocol.empty()) throw std::invalid_argument("NotSupportedError: negotiated ID/protocol");
  uint32_t id = nextObject++;
  { std::lock_guard<std::mutex> lock(peerMutex()); requirePeer(nativeHandle);
    for (const auto& [key, ch] : channelTable()) if (ch.peer == nativeHandle && ch.label == label && ch.state != "closed")
      throw std::invalid_argument("NotSupportedError: duplicate data channel label");
    channelTable()[id].peer = nativeHandle; channelTable()[id].label = label; channelTable()[id].config = init;
    registerObjectLifetime(id, nativeHandle);
  }
  try { rtc::platform_create_channel(nativeHandle, label, init); }
  catch (...) {
    std::lock_guard<std::mutex> lock(peerMutex());
    std::lock_guard<std::mutex> owners(lifetimeMutex());
    channelTable().erase(id); objectLifetimeTable().erase(id); throw;
  }
  return RTCDataChannel(id);
}
std::string RTCDataChannel::label() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).label; }
std::string RTCDataChannel::protocol() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).config.protocol; }
bool RTCDataChannel::ordered() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).config.ordered; }
std::string RTCDataChannel::binaryType() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).binary_type; }
void RTCDataChannel::setBinaryType(const std::string& type) const {
  if (type != "arraybuffer") throw std::invalid_argument("NotSupportedError: use arraybuffer data channels");
  std::lock_guard<std::mutex> lock(peerMutex()); requireChannel(nativeHandle).binary_type = type;
}
std::string RTCDataChannel::readyState() const {
  std::lock_guard<std::mutex> lock(peerMutex()); auto it = channelTable().find(nativeHandle);
  return it == channelTable().end() ? "closed" : it->second.state;
}
std::optional<double> RTCDataChannel::id() const {
  std::lock_guard<std::mutex> lock(peerMutex()); auto& c = requireChannel(nativeHandle);
  return c.stream ? std::optional<double>(*c.stream) : std::nullopt;
}
std::optional<double> RTCDataChannel::maxRetransmits() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).config.maxRetransmits; }
std::optional<double> RTCDataChannel::maxPacketLifeTime() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).config.maxPacketLifeTime; }
double RTCDataChannel::bufferedAmount() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).buffered; }
double RTCDataChannel::bufferedAmountLowThreshold() const { std::lock_guard<std::mutex> lock(peerMutex()); return requireChannel(nativeHandle).low_threshold; }
void RTCDataChannel::setBufferedAmountLowThreshold(double value) const {
  if (!std::isfinite(value) || value < 0 || value > 4294967295.0) throw std::invalid_argument("Invalid bufferedAmountLowThreshold");
  std::lock_guard<std::mutex> lock(peerMutex()); requireChannel(nativeHandle).low_threshold = std::floor(value);
}
namespace {
void sendChannel(uint32_t id, const uint8_t* bytes, size_t size, bool text) {
  uint32_t peer; uint16_t stream;
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& ch = requireChannel(id);
    requirePeer(ch.peer);
    if (ch.state != "open" || !ch.stream) throw std::runtime_error("InvalidStateError: data channel is not open");
    peer = ch.peer; stream = *ch.stream; ch.buffered += size;
  }
  try { rtc::platform_send_data(peer, stream, bytes, size, text); }
  catch (...) { std::lock_guard<std::mutex> lock(peerMutex()); requireChannel(id).buffered -= size; throw; }
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& ch = requireChannel(id);
    const bool low = ch.buffered > ch.low_threshold && ch.buffered - size <= ch.low_threshold;
    ch.buffered -= size;
    if (low) { PendingEvent ev{peer, EventKind::ChannelLow}; ev.channel = id; enqueue_internal(std::move(ev)); }
  }
}
}
void RTCDataChannel::send(const std::string& data) const { sendChannel(nativeHandle, reinterpret_cast<const uint8_t*>(data.data()), data.size(), true); }
void RTCDataChannel::send(const std::vector<uint8_t>& data) const { sendChannel(nativeHandle, data.data(), data.size(), false); }
void RTCDataChannel::close() const {
  uint32_t peer; std::string label;
  { std::lock_guard<std::mutex> lock(peerMutex()); auto& ch = requireChannel(nativeHandle);
    if (ch.state == "closed" || ch.state == "closing") return;
    ch.state = "closing"; peer = ch.peer; label = ch.label;
  }
  try { rtc::platform_close_channel(peer, label); }
  catch (...) { std::lock_guard<std::mutex> lock(peerMutex()); auto& ch = requireChannel(nativeHandle); ch.state = ch.stream ? "open" : "connecting"; throw; }
  std::lock_guard<std::mutex> lock(peerMutex());
  auto& ch = requireChannel(nativeHandle);
  if (!ch.stream && ch.state != "closed") {
    ch.state = "closed";
    PendingEvent ev{peer, EventKind::ChannelClose}; ev.channel = nativeHandle; enqueue_internal(std::move(ev));
  }
}
