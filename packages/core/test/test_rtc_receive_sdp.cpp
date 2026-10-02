// SPDX-License-Identifier: Apache-2.0
#include "host/rtc_receive_sdp.h"
#include <cassert>
#include <iostream>

using namespace gea::host::rtc::receive;

static std::string replace(std::string source, const std::string& old, const std::string& value) {
  const auto at = source.find(old);
  assert(at != source.npos);
  source.replace(at, old.size(), value);
  return source;
}

int main() {
  const std::string digest =
      "01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20";
  const std::string ice =
      "a=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuvwxyz\r\n"
      "a=candidate:1 1 udp 100 192.0.2.1 40000 typ host\r\na=end-of-candidates\r\n";
  const std::string transport = ice + "a=fingerprint:sha-256 " + digest + "\r\na=setup:actpass\r\n";
  const std::string sdp =
      "v=0\r\no=- 10000 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\na=ice-lite\r\n"
      "m=audio 7 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\na=sendonly\r\na=rtcp-mux\r\n" +
      transport + "a=rtpmap:111 opus/48000/2\r\na=ssrc:123 cname:audio\r\na=ssrc:123 msid:daily audio\r\n"
      "m=video 7 UDP/TLS/RTP/SAVPF 101\r\na=mid:1\r\na=sendonly\r\na=rtcp-mux\r\na=rtcp-rsize\r\n" +
      transport + "a=rtpmap:101 VP8/90000\r\na=ssrc:456 cname:video\r\na=rtcp-fb:101 nack pli\r\n";
  const auto offer = parseOffer(sdp);
  assert(offer && offer->tracks.size() == 2);
  assert(offer->tracks[0].ssrc == 123 && offer->tracks[1].payloadType == 101);
  assert(offer->peerFingerprint.front() == 1 && offer->peerFingerprint.back() == 32);
  assert(offer->iceDescription.find("a=ice-lite\r\n") != std::string::npos);
  const auto local = answer(*offer, ice, digest);
  assert(local && local->find("a=setup:active\r\n") != std::string::npos);
  assert(local->find("a=recvonly\r\n") != std::string::npos);
  assert(local->find("a=rtcp-fb:101 nack pli\r\n") != std::string::npos);
  assert(local->find("a=rtcp-fb:101 nack\r\n") == std::string::npos);
  assert(local->find("a=ice-lite") == std::string::npos);
  assert(!answer(*offer, "a=ice-pwd:abcd\r\n", digest));
  assert(!answer(*offer, ice, "invalid"));
  auto audioOnly = replace(sdp.substr(0, sdp.find("m=video")), "BUNDLE 0 1", "BUNDLE 0");
  const auto audioOffer = parseOffer(audioOnly);
  assert(audioOffer && audioOffer->tracks.size() == 1 && audioOffer->tracks[0].kind == "audio");
  const auto audioAnswer = answer(*audioOffer, ice, digest);
  assert(audioAnswer && audioAnswer->find("m=video") == std::string::npos);
  assert(audioAnswer->find("a=group:BUNDLE 0\r\n") != std::string::npos);
  assert(!parseOffer(replace(audioOnly, "a=ssrc:123", "a=ssrc:0")));
  assert(!parseOffer(replace(audioOnly, "a=sendonly", "a=recvonly")));

  for (const auto& [old, value] : std::vector<std::pair<std::string, std::string>>{
      {"a=group:BUNDLE 0 1", "a=group:BUNDLE 0 0"},
      {"a=group:BUNDLE 0 1", "a=group:BUNDLE 0 2"},
      {"a=mid:1", "a=mid:0"},
      {"a=sendonly", "a=sendrecv"},
      {"UDP/TLS/RTP/SAVPF", "RTP/AVP"},
      {"m=video 7", "m=video 0"},
      {"a=rtcp-mux", "a=invalid"},
      {"a=setup:actpass", "a=setup:active"},
      {"a=fingerprint:sha-256", "a=fingerprint:sha-1"},
      {digest, replace(digest, "01:", "FF:")}, // Different certificate across BUNDLE.
      {"a=ice-ufrag:abcd", "a=ice-ufrag:different"},
      {"a=ice-pwd:abcdefghijklmnopqrstuvwxyz", "a=ice-pwd:short"},
      {"opus/48000/2", "opus/16000/1"},
      {"VP8/90000", "H264/90000"},
      {"a=rtpmap:101", "a=rtpmap:102"},
      {"a=ssrc:456", "a=ssrc:123"},
      {"a=rtcp-fb:101 nack pli", "a=rtcp-fb:101 nack"},
      {"m=video 7 UDP/TLS/RTP/SAVPF 101", "m=video 7 UDP/TLS/RTP/SAVPF 101 102"},
  }) {
    if (parseOffer(replace(sdp, old, value))) {
      std::cerr << "Unexpectedly accepted " << old << " -> " << value << '\n';
      return 1;
    }
  }
  assert(!parseOffer(sdp + "a=ssrc:789 cname:another\r\n"));
  assert(!parseOffer(sdp + std::string("\0", 1)));
  assert(!fingerprint(replace(digest, "01:", "0G:")));
  assert(!fingerprint(digest.substr(1)));
  const std::string microphoneAnswer =
      "v=0\r\no=- 10001 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\na=ice-lite\r\n"
      "m=audio 7 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\na=recvonly\r\na=rtcp-mux\r\n" +
      ice + "a=fingerprint:sha-256 " + digest + "\r\na=setup:passive\r\na=rtpmap:111 opus/48000/2\r\n";
  const auto microphone = parseMicrophoneAnswer(microphoneAnswer);
  assert(microphone && microphone->tracks.size() == 1);
  assert(microphone->peerFingerprint == offer->peerFingerprint);
  assert(!parseOffer(microphoneAnswer));
  assert(!parseMicrophoneAnswer(sdp));
  for (const auto& [old, value] : std::vector<std::pair<std::string, std::string>>{
      {"a=recvonly", "a=sendonly"}, {"a=recvonly", "a=sendrecv"},
      {"a=setup:passive", "a=setup:actpass"}, {"a=setup:passive", "a=setup:active"},
      {"a=group:BUNDLE 0", "a=group:BUNDLE 0 1"}, {"a=mid:0", "a=mid:1"},
      {"m=audio 7", "m=audio 0"}, {"m=audio", "m=video"},
      {"SAVPF 111", "SAVPF 112"}, {"a=rtpmap:111", "a=rtpmap:112"},
      {"opus/48000/2", "opus/16000/1"}, {"a=rtcp-mux", "a=invalid"},
      {"a=fingerprint:sha-256", "a=fingerprint:sha-1"},
      {"a=ice-pwd:abcdefghijklmnopqrstuvwxyz", "a=ice-pwd:short"},
  }) assert(!parseMicrophoneAnswer(replace(microphoneAnswer, old, value)));
  assert(!parseMicrophoneAnswer(microphoneAnswer + "m=audio 7 UDP/TLS/RTP/SAVPF 111\r\n"));
  assert(!parseMicrophoneAnswer(microphoneAnswer + std::string("\0", 1)));
  const auto micOffer = gea::host::rtc::receive::microphoneOffer(ice, digest, 123456);
  assert(micOffer && micOffer->find("a=ssrc:123456 cname:gea-mic\r\n") != std::string::npos);
  assert(micOffer->find("a=sendonly\r\n") != std::string::npos);
  assert(micOffer->find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos);
  assert(micOffer->find("a=setup:actpass\r\n") != std::string::npos);
  assert(!gea::host::rtc::receive::microphoneOffer(ice, digest, 0));
  assert(!gea::host::rtc::receive::microphoneOffer(ice, "invalid", 123));
  assert(!gea::host::rtc::receive::microphoneOffer("a=ice-ufrag:abcd\r\n", digest, 123));
  // Session-level ICE and certificate attributes are inherited by both media.
  auto inherited = replace(sdp, "a=ice-lite\r\n", "a=ice-lite\r\n" + transport);
  assert(parseOffer(inherited));
  std::cout << "VP8/Opus receive and microphone send SDP, BUNDLE and fingerprint validation passed\n";
}
