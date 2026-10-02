#include "host/rtc.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace gea::host;
namespace {
uint32_t created = 0;
RTCConfiguration configured;
std::vector<uint8_t> sent;
bool sentText = false;
std::string lastDirection;
unsigned destroyed = 0;
std::shared_ptr<rtc::DescriptionResult> pendingDescription;
}
namespace gea::host {
std::string MediaStreamTrack::kind() const { return nativeHandle == 999 ? "video" : "audio"; }
std::string MediaStreamTrack::id() const { return "track-" + std::to_string(nativeHandle); }
std::string MediaStream::id() const { return "stream-" + std::to_string(nativeHandle); }
namespace rtc {
void platform_request_description(uint32_t, bool, std::shared_ptr<DescriptionResult> result) {
  pendingDescription = std::move(result);
}
std::string platform_create_offer(uint32_t) { return "v=0\r\na=msid-semantic: WMS old\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=msid:old oldtrack\r\na=mid:0\r\nm=video 0 UDP/TLS/RTP/SAVPF 96\r\n"; }
void platform_create(uint32_t id, const RTCConfiguration& config) { created = id; configured = config; }
void platform_destroy(uint32_t) { ++destroyed; }
void platform_direction(uint32_t, const std::string&, const std::string& value) { lastDirection = value; }
void platform_create_channel(uint32_t, const std::string&, const RTCDataChannelInit&) {}
void platform_send_data(uint32_t, uint16_t, const uint8_t* bytes, size_t size, bool text) { sent.assign(bytes, bytes + size); sentText = text; }
}
}
template<class Fn> void rejects(Fn fn) { bool rejected = false; try { fn(); } catch (const std::exception&) { rejected = true; } assert(rejected); }
int main() {
  {
    const auto baseline = destroyed;
    bool completed = false;
    RTCPeerConnection(rtc::create_handle()).requestDescription(true,
      [&](const std::string& sdp, const std::string& error) { completed = !sdp.empty() && error.empty(); });
    assert(destroyed == baseline); // Pending native operation retains its peer.
    pendingDescription->sdp = "v=0\r\n";
    pendingDescription->ready.store(true, std::memory_order_release);
    rtc::runCallbacks();
    assert(completed && destroyed == baseline + 1);
  }
  {
    RTCPeerConnection peer(rtc::create_handle());
    bool completed = false;
    peer.requestDescription(true, [&](const std::string& sdp, const std::string& error) {
      assert(error.empty() && sdp.find("v=0") == 0);
      completed = true;
    });
    rtc::runCallbacks();
    assert(!completed); // Pending SDP does not block the UI/event pump.
    pendingDescription->sdp = "v=0\r\n";
    pendingDescription->ready.store(true, std::memory_order_release);
    assert(!completed); // Worker publication cannot execute UI callbacks.
    rtc::runCallbacks();
    assert(completed);
    completed = false;
    peer.requestDescription(true, [&](const std::string&, const std::string& error) {
      assert(error.find("AbortError") == 0);
      completed = true;
    });
    peer.close();
    rtc::enqueue_connection_state_change(peer.nativeHandle, "connected");
    assert(peer.connectionState() == "closed");
    rtc::runCallbacks();
    assert(completed); // Close rejects without waiting for the SDP worker.
    pendingDescription->sdp = "late SDP";
    pendingDescription->ready.store(true, std::memory_order_release);
    rtc::runCallbacks(); // Late worker result is safely ignored.
  }
  {
    RTCPeerConnection peer(rtc::create_handle());
    bool failed = false;
    peer.requestDescription(true, [&](const std::string&, const std::string& error) { failed = !error.empty(); });
    pendingDescription->ready.store(true, std::memory_order_release);
    rtc::runCallbacks();
    assert(failed); // Empty SDP must reject rather than POST an empty offer.
  }
  {
    const auto id = rtc::create_handle();
    RTCPeerConnection peer(id);
    const auto audio = peer.addTransceiver("audio", {"recvonly"});
    const auto video = peer.addTransceiver("video", {"recvonly"});
    assert(peer.getTransceivers().size() == 2);
    rejects([&] { peer.addTransceiver("video", {"recvonly"}); });
    rejects([&] { video.setDirection("sendrecv"); });
    peer.setRemoteDescription("offer", "v=0\r\nm=audio 7 UDP/TLS/RTP/SAVPF 100\r\na=mid:0\r\na=sendonly\r\nm=video 7 UDP/TLS/RTP/SAVPF 107\r\na=mid:1\r\na=sendonly\r\n");
    peer.setLocalDescription("answer", "v=0\r\nm=audio 7 UDP/TLS/RTP/SAVPF 100\r\na=mid:0\r\na=recvonly\r\nm=video 7 UDP/TLS/RTP/SAVPF 107\r\na=mid:1\r\na=recvonly\r\n");
    assert(audio.mid() == "0" && video.mid() == "1");
    assert(audio.currentDirection() == "recvonly" && video.currentDirection() == "recvonly");
    peer.close();
  }
  {
    // Match the JS lifecycle: close and release wrappers, without manually
    // reaching into the native registry to destroy a handle.
    const auto baseline = destroyed;
    const auto id = rtc::create_handle();
    RTCDataChannel retainedChannel;
    RTCRtpSender retainedSender;
    std::weak_ptr<int> capture;
    bool closeDelivered = false;
    {
      RTCPeerConnection peer(id);
      auto copied = peer;
      retainedChannel = peer.createDataChannel("lifetime");
      retainedSender = peer.addTransceiver("audio").sender();
      auto marker = std::make_shared<int>(42);
      capture = marker;
      rtc::callbackTable()[id].on_connection_state_change = [peer, marker] { peer.close(); };
      rtc::channel_callbacks(retainedChannel.nativeHandle).on_close = [&] { closeDelivered = true; };
      rtc::enqueue_connection_state_change(id, "connected");
      rtc::runCallbacks(); // close enqueues its event for the next dispatch
      assert(!closeDelivered && !capture.expired());
    }
    rtc::runCallbacks();
    assert(closeDelivered && capture.expired());
    assert(destroyed == baseline); // Child objects still own the closed state.
    assert(retainedChannel.label() == "lifetime" && retainedChannel.readyState() == "closed");
    retainedChannel = {};
    assert(destroyed == baseline);
    retainedSender = {};
    assert(destroyed == baseline + 1);
    assert(rtc::callbackTable().find(id) == rtc::callbackTable().end());
  }
  {
    const auto id = rtc::create_handle();
    const RTCPeerConnection peer(id);
    const MediaStreamTrack track(uint32_t{45});
    rejects([&] { peer.addTrack(track, std::vector<MediaStream>{MediaStream{}}); });
    assert(peer.getSenders().empty());
    peer.addTrack(track, std::vector<MediaStream>{MediaStream(uint32_t{7}), MediaStream(uint32_t{8}), MediaStream(uint32_t{7})});
    const auto offer = peer.createOffer();
    assert(offer.find("a=msid-semantic: WMS *\r\n") != std::string::npos);
    assert(offer.find("a=msid:stream-7 track-45\r\n") != std::string::npos);
    assert(offer.find("a=msid:stream-8 track-45\r\n") != std::string::npos);
    assert(offer.find("a=msid:old") == std::string::npos);
    const auto first = offer.find("a=msid:stream-7");
    assert(offer.find("a=msid:stream-7", first + 1) == std::string::npos);
    rejects([&] { peer.addTrack(track); });
    peer.close();
    rtc::destroy_handle(id);
  }
  RTCIceCandidateInit candidateInit;
  candidateInit.candidate = "candidate:842163049 1 UDP 1677729535 192.0.2.1 5000 typ srflx raddr 10.0.0.2 rport 8998 generation 0";
  candidateInit.sdpMid = "audio";
  candidateInit.usernameFragment = "ice-user";
  RTCIceCandidate candidate(candidateInit);
  assert(candidate.foundation() == "842163049" && candidate.component() == "rtp");
  assert(candidate.priority() == 1677729535 && candidate.port() == 5000);
  assert(candidate.address() == "192.0.2.1" && candidate.protocol() == "udp" && candidate.type() == "srflx");
  assert(candidate.relatedAddress() == "10.0.0.2" && candidate.relatedPort() == 8998);
  assert(!candidate.tcpType() && !candidate.sdpMLineIndex() && candidate.usernameFragment() == "ice-user");
  auto sameCandidate = candidate;
  assert(sameCandidate == candidate);
  candidateInit.candidate = "candidate:tcp 2 tcp 4294967295 2001:db8::1 9 typ host tcptype passive";
  RTCIceCandidate tcp(candidateInit);
  assert(tcp.tcpType() == "passive" && tcp.component() == "rtcp" && tcp.address() == "2001:db8::1");
  assert(candidate.protocol() == "udp");
  candidateInit.candidate = "candidate:mdns 1 udp 100 laptop.local 5000 typ host";
  assert(RTCIceCandidate(candidateInit).address() == "laptop.local");
  for (const auto& malformed : {
      "", "not a candidate", "candidate:x 1 udp 4294967296 192.0.2.1 1 typ host",
      "candidate:x 1 udp 1 192.0.2.1 65536 typ host", "candidate:x 3 udp 1 192.0.2.1 1 typ host",
      "candidate:x 1 tcp 1 192.0.2.1 1 typ host", "candidate:x 1 udp 1 192.0.2.1 1 typ host rport -1",
      "candidate:x 1 udp 1 192.0.2.1 1 typ host dangling", "candidate:x 1 udp 1 192.0.2.1 1 typ unknown",
      "candidate:x 1 tcp 1 192.0.2.1 1 typ host tcptype unknown"}) {
    candidateInit.candidate = malformed;
    const RTCIceCandidate invalid(candidateInit);
    assert(invalid.candidate() == malformed && !invalid.foundation() && !invalid.protocol() && !invalid.port());
    assert(invalid.sdpMid() == "audio" && invalid.usernameFragment() == "ice-user");
  }
  rejects([] { RTCIceCandidate noMedia(RTCIceCandidateInit{}); });
  RTCIceCandidateInit endInit;
  endInit.sdpMLineIndex = 0;
  const RTCIceCandidate end(endInit);
  assert(end.candidate().empty() && !end.foundation() && end.sdpMLineIndex() == 0);
  RTCConfiguration cfg{{{{"turn:relay.invalid:3478", "stun:stun.invalid:3478"}, "user", "credential"}}, "relay"};
  auto handle = rtc::create_handle(cfg);
  RTCPeerConnection pc(handle);
  assert(created == handle && configured.iceServers.size() == 1);
  assert(configured.iceServers[0].urls.size() == 2 && configured.iceServers[0].credential == "credential");
  assert(pc.getConfiguration().iceTransportPolicy == "relay");
  auto tr = pc.addTransceiver(MediaStreamTrack(uint32_t{10}), {"sendonly"});
  assert(lastDirection == "sendonly" && tr.direction() == "sendonly");
  assert(!tr.mid() && !tr.currentDirection());
  assert(pc.getSenders()[0].nativeHandle == tr.sender().nativeHandle);
  assert(tr.sender().track()->nativeHandle == 10);
  tr.sender().replaceTrack(MediaStreamTrack(uint32_t{11}));
  assert(tr.sender().track()->nativeHandle == 11);
  rejects([&] { tr.sender().replaceTrack(MediaStreamTrack(uint32_t{999})); });
  assert(tr.sender().track()->nativeHandle == 11);
  pc.setLocalDescription("offer", "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio0\r\na=sendrecv\r\n");
  assert(!tr.currentDirection());
  pc.setRemoteDescription("answer", "v=0\r\na=recvonly\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio0\r\n");
  assert(tr.mid() == "audio0" && tr.currentDirection() == "sendonly");
  rejects([&] { tr.setDirection("send-everywhere"); });
  pc.removeTrack(tr.sender());
  assert(!tr.sender().track() && tr.direction() == "inactive");
  assert(tr.currentDirection() == "sendonly"); // remains negotiated until next answer
  pc.setLocalDescription("offer", "v=0\r\n");
  assert(pc.signalingState() == "have-local-offer" && pc.localDescription()->type() == "offer");
  pc.setRemoteDescription("answer", "v=0\r\n");
  assert(pc.signalingState() == "stable");
  RTCDataChannelInit lossy; lossy.ordered = false; lossy.maxRetransmits = 0;
  auto dc = pc.createDataChannel("_lossy", lossy);
  assert(!dc.ordered() && dc.maxRetransmits() == 0 && !dc.id());
  rejects([&] { dc.send(std::string("not open")); });
  bool opened = false, low = false, received = false, closed = false;
  rtc::channel_callbacks(dc.nativeHandle).on_open = [&] { opened = true; };
  rtc::channel_callbacks(dc.nativeHandle).on_buffered_amount_low = [&] { low = true; };
  rtc::channel_callbacks(dc.nativeHandle).on_close = [&] { closed = true; };
  rtc::channel_callbacks(dc.nativeHandle).on_message = [&](const std::vector<uint8_t>& bytes, bool text) {
    assert(!text && bytes == std::vector<uint8_t>({0, 128, 255})); received = true;
  };
  rtc::enqueue_channel_open(handle, "_lossy", 7);
  rtc::runCallbacks(); assert(opened && dc.readyState() == "open" && dc.id() == 7);
  dc.send(std::vector<uint8_t>{0, 128, 255});
  assert(!sentText && sent.size() == 3 && sent[2] == 255 && dc.bufferedAmount() == 0);
  rtc::runCallbacks(); assert(low);
  uint8_t payload[] = {0, 128, 255};
  rtc::enqueue_channel_message(handle, 7, payload, 3, false);
  payload[0] = 9; // queued message owns its bytes
  rtc::runCallbacks(); assert(received);
  RTCDataChannelInit invalid; invalid.maxPacketLifeTime = 10; invalid.maxRetransmits = 0;
  rejects([&] { pc.createDataChannel("invalid", invalid); });
  bool incoming = false;
  rtc::callbackTable()[handle].on_data_channel = [&](RTCDataChannel remote) { incoming = remote.label() == "_reliable"; };
  rtc::enqueue_channel_open(handle, "_reliable", 2); rtc::runCallbacks(); assert(incoming);
  pc.close(); rtc::runCallbacks();
  assert(closed && dc.readyState() == "closed" && tr.stopped() && pc.signalingState() == "closed");
  rejects([&] { dc.send(std::string("closed")); });
  rejects([&] { pc.addTransceiver("audio"); });
  rtc::destroy_handle(handle);
  // A subscriber can receive its first track after SDP negotiation finishes.
  auto subscriberHandle = rtc::create_handle();
  RTCPeerConnection subscriber(subscriberHandle);
  subscriber.setRemoteDescription("offer", "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:remote0\r\na=sendonly\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:remote1\r\na=sendonly\r\n");
  subscriber.setLocalDescription("answer", "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:remote0\r\na=recvonly\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:remote1\r\na=inactive\r\n");
  RTCTrackEvent deliveredTrack;
  bool delivered = false;
  rtc::callbackTable()[subscriberHandle].on_track = [&](RTCTrackEvent event) { deliveredTrack = event; delivered = true; };
  rtc::enqueue_track(subscriberHandle, 12, {MediaStream(uint32_t(91))});
  rtc::runCallbacks();
  assert(delivered && deliveredTrack.track().nativeHandle == 12);
  assert(deliveredTrack.streams().size() == 1 && deliveredTrack.streams()[0].nativeHandle == 91);
  auto incomingTransceiver = subscriber.getTransceivers().at(0);
  assert(incomingTransceiver.mid() == "remote0" && incomingTransceiver.currentDirection() == "recvonly");
  assert(incomingTransceiver.receiver().track()->nativeHandle == 12);
  assert(deliveredTrack.transceiver().nativeHandle == incomingTransceiver.nativeHandle);
  assert(deliveredTrack.receiver().nativeHandle == incomingTransceiver.receiver().nativeHandle);
  auto savedEvent = deliveredTrack;
  assert(savedEvent == deliveredTrack);
  // Media kinds have separate receiver identities and negotiated directions.
  // A video callback must not overwrite the receiver feeding the speaker.
  rtc::enqueue_track(subscriberHandle, 999, {MediaStream(uint32_t(92))});
  rtc::runCallbacks();
  assert(subscriber.getTransceivers().size() == 2);
  const auto videoTransceiver = deliveredTrack.transceiver();
  assert(videoTransceiver.nativeHandle != incomingTransceiver.nativeHandle);
  assert(videoTransceiver.mid() == "remote1" && videoTransceiver.currentDirection() == "inactive");
  assert(videoTransceiver.receiver().track()->nativeHandle == 999);
  assert(incomingTransceiver.receiver().track()->nativeHandle == 12);
  assert(incomingTransceiver.currentDirection() == "recvonly");
  rtc::enqueue_track(subscriberHandle, 13, {MediaStream(uint32_t(93))});
  rtc::runCallbacks();
  assert(subscriber.getTransceivers().size() == 2);
  assert(deliveredTrack.transceiver().nativeHandle == incomingTransceiver.nativeHandle);
  assert(videoTransceiver.receiver().track()->nativeHandle == 999);
  subscriber.close(); rtc::destroy_handle(subscriberHandle);
  // A callback can destroy its own peer without invalidating dispatch storage.
  auto second = rtc::create_handle();
  rtc::callbackTable()[second].on_connection_state_change = [=] { rtc::destroy_handle(second); };
  rtc::enqueue_connection_state_change(second, "connected"); rtc::runCallbacks();
  std::cout << "RTC objects: ICE, RTP identity/direction, binary channels, and callback lifetime passed\n";
}
