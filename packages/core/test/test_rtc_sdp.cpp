#include "host/rtc_sdp.h"
#include <cassert>
#include <iostream>

int main() {
  using gea::host::rtc::opusPcmCapabilities;
  const std::string offer = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111 0\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;maxplaybackrate=48000\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=fmtp:96 profile-level-id=42e01f\r\n";
  const auto result = opusPcmCapabilities(offer, 16000, 1);
  assert(result.find("opus/48000/2\r\n") != std::string::npos);
  assert(result.find("a=fmtp:111 minptime=10;useinbandfec=1;maxplaybackrate=16000;"
      "sprop-maxcapturerate=16000;stereo=0;sprop-stereo=0\r\n") != std::string::npos);
  assert(result.find("maxplaybackrate=48000") == std::string::npos);
  assert(result.substr(result.find("m=video")) == offer.substr(offer.find("m=video")));
  assert(opusPcmCapabilities(result, 16000, 1) == result);
  const auto missing = opusPcmCapabilities("m=audio 9 RTP/AVP 109\na=rtpmap:109 opus/48000/2", 16000, 1);
  assert(missing.find("\na=fmtp:109 maxplaybackrate=16000;") != std::string::npos);
  assert(missing.back() != '\n');
  assert(opusPcmCapabilities("m=audio 9 RTP/AVP 0\na=rtpmap:0 PCMU/8000\n", 16000, 1) ==
      "m=audio 9 RTP/AVP 0\na=rtpmap:0 PCMU/8000\n");
  assert(opusPcmCapabilities("", 16000, 1).empty());
  std::cout << "RTC SDP capabilities passed\n";
}
