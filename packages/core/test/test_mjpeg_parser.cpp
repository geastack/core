// SPDX-License-Identifier: Apache-2.0
#include "host/mjpeg_parser.h"
#include "host/h264_packet_queue.h"
#include <cassert>
#include <iostream>
#include <iterator>

using namespace gea::host::video;

int main(int argc, char**) {
  if (argc > 1) {
    const std::string encoded(std::istreambuf_iterator<char>(std::cin), {});
    assert(!encoded.empty());
    const auto makePart = [](const std::string& body, unsigned width) {
      return "--frame\r\nContent-Type: video/h264\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nX-Width: " + std::to_string(width) + "\r\nX-Height: 448\r\n\r\n" + body + "\r\n";
    };
    const auto part = makePart(encoded, 368);
    for (const size_t chunk : {1, 3, 4096}) {
      MjpegParser parser;
      unsigned frames = 0;
      for (size_t offset = 0; offset < part.size(); offset += chunk)
        assert(parser.push(std::span(reinterpret_cast<const uint8_t*>(part.data() + offset),
            std::min(chunk, part.size() - offset)), [&](JpegPacket packet) {
          ++frames;
          assert(packet.h264 && packet.h264Keyframe && !packet.rawFrame && packet.bytes.size() == encoded.size());
        }));
      assert(frames == 1);
    }
    auto predictive = encoded;
    unsigned prefix = 0;
    for (size_t start = gea::host::rtc::h264::startCode(reinterpret_cast<const uint8_t*>(predictive.data()),
         predictive.size(), 0, prefix); start < predictive.size();) {
      const auto nal = start + prefix;
      if ((uint8_t(predictive[nal]) & 31) == 5) predictive[nal] = char((uint8_t(predictive[nal]) & ~31) | 1);
      start = gea::host::rtc::h264::startCode(reinterpret_cast<const uint8_t*>(predictive.data()),
          predictive.size(), nal, prefix);
    }
    for (const auto bad : {makePart(encoded, 352), makePart(predictive, 368)}) {
      MjpegParser parser;
      assert(!parser.push(std::span(reinterpret_cast<const uint8_t*>(bad.data()), bad.size()),
          [](JpegPacket) { assert(false); }));
    }
    MjpegParser continuing;
    unsigned decoded = 0;
    const auto continuation = part + makePart(predictive, 368);
    assert(continuing.push(std::span(reinterpret_cast<const uint8_t*>(continuation.data()), continuation.size()),
        [&](JpegPacket packet) {
          assert(packet.h264 && packet.h264Keyframe == (decoded == 0));
          ++decoded;
        }));
    assert(decoded == 2);
    std::cout << "Actual libx264 IDR and continuation accepted; orphan references and mismatched dimensions rejected\n";
  }
  {
    H264PacketQueue queue;
    const auto packet = [](unsigned sequence, bool key) {
      JpegPacket p; p.h264 = true; p.h264Keyframe = key; p.timestampMs = sequence; return p;
    };
    assert(queue.push(packet(0, false)) == 1 && queue.empty());
    assert(queue.push(packet(1, true)) == 0);
    assert(queue.push(packet(2, false)) == 0);
    assert(queue.push(packet(3, false)) == 0);
    auto first = queue.pop();
    assert(first && first->timestampMs == 1 && first->resetDecoder);
    assert(queue.push(packet(4, false)) == 0);
    assert(queue.push(packet(5, false)) == 4 && queue.empty());
    assert(queue.push(packet(6, false)) == 1);
    assert(queue.push(packet(7, true)) == 0);
    auto recovery = queue.pop();
    assert(recovery && recovery->timestampMs == 7 && recovery->resetDecoder && queue.empty());
  }
  const std::string jpeg{"\xff\xd8\x10\x20\xff\xd9", 6};
  const std::string part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 6\r\nX-Timestamp-Ms: 42\r\n\r\n" + jpeg + "\r\n";
  const auto wire = part + part;
  for (size_t chunk = 1; chunk <= wire.size(); ++chunk) {
    MjpegParser parser;
    unsigned frames = 0;
    for (size_t offset = 0; offset < wire.size(); offset += chunk) {
      const auto size = std::min(chunk, wire.size() - offset);
      assert(parser.push(std::span(reinterpret_cast<const uint8_t*>(wire.data() + offset), size),
          [&](JpegPacket packet) {
            ++frames;
            assert(packet.timestampMs == 42);
            assert(packet.bytes.size() == jpeg.size());
            assert(std::equal(packet.bytes.begin(), packet.bytes.end(), reinterpret_cast<const uint8_t*>(jpeg.data())));
          }));
    }
    assert(frames == 2);
  }
  for (const std::string bad : {
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 65537\r\n\r\n",
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: -1\r\n\r\n",
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 0\r\n\r\n",
      "--frame\r\nContent-Type: image/png\r\nContent-Length: 6\r\n\r\n",
      "--frame\r\nContent-Length: 6\r\nContent-Length: 6\r\n\r\n",
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 4\r\n\r\nXXXX",
  }) {
    MjpegParser parser;
    assert(!parser.push(std::span(reinterpret_cast<const uint8_t*>(bad.data()), bad.size()),
        [](JpegPacket) { assert(false); }));
  }
  MjpegParser oversized;
  const std::string unbounded(2049, 'x');
  assert(!oversized.push(std::span(reinterpret_cast<const uint8_t*>(unbounded.data()), unbounded.size()),
      [](JpegPacket) { assert(false); }));

  const std::string pixels{"\xf8\x00\x07\xe0\x00\x1f\xff\xff", 8};
  const std::string rawPart = "--frame\r\nContent-Type: application/x-rgb565-be\r\nContent-Length: 8\r\n"
      "X-Width: 2\r\nX-Height: 2\r\nX-Timestamp-Ms: 50\r\n\r\n" + pixels + "\r\n";
  const auto mixed = rawPart + part;
  for (size_t chunk = 1; chunk <= mixed.size(); ++chunk) {
    MjpegParser parser;
    unsigned frames = 0;
    for (size_t offset = 0; offset < mixed.size(); offset += chunk) {
      assert(parser.push(std::span(reinterpret_cast<const uint8_t*>(mixed.data() + offset),
          std::min(chunk, mixed.size() - offset)), [&](JpegPacket packet) {
        if (!frames++) {
          assert(packet.bytes.empty());
          assert(packet.rawFrame && packet.rawFrame->width == 2 && packet.rawFrame->height == 2);
          assert(packet.rawFrame->timestampMs == 50);
          assert(packet.rawFrame->panelEndian == (std::endian::native != std::endian::big));
          assert(std::memcmp(packet.rawFrame->rgb565.data(), pixels.data(), pixels.size()) == 0);
        } else assert(!packet.rawFrame && packet.bytes.size() == 6);
      }));
    }
    assert(frames == 2);
  }
  // Full native dimensions, fragmented TCP bodies, final allocation only.
  const std::string fullPixels(368 * 448 * 2, '\x5a');
  const std::string fullPart = "--frame\r\nContent-Type: application/x-rgb565-le\r\nContent-Length: 329728\r\n"
      "X-Width: 368\r\nX-Height: 448\r\n\r\n" + fullPixels + "\r\n";
  for (const size_t chunk : {1, 3, 4096, 65536}) {
    MjpegParser parser;
    unsigned frames = 0;
    for (size_t offset = 0; offset < fullPart.size(); offset += chunk) {
      assert(parser.push(std::span(reinterpret_cast<const uint8_t*>(fullPart.data() + offset),
          std::min(chunk, fullPart.size() - offset)), [&](JpegPacket packet) {
        ++frames;
        assert(packet.bytes.empty() && packet.rawFrame);
        assert(packet.rawFrame->width == 368 && packet.rawFrame->height == 448);
        assert(std::memcmp(packet.rawFrame->rgb565.data(), fullPixels.data(), fullPixels.size()) == 0);
      }));
    }
    assert(frames == 1);
  }
  MjpegParser bounded;
  std::vector<std::shared_ptr<gea::host::media::VideoFrame>> held;
  const auto hold = [&](JpegPacket packet) { held.push_back(std::move(packet.rawFrame)); };
  for (unsigned i = 0; i < 4; ++i)
    assert(bounded.push(std::span(reinterpret_cast<const uint8_t*>(rawPart.data()), rawPart.size()), hold));
  assert(held.size() == 3); // Fourth picture drops; outstanding readers stay immutable.
  for (const auto& frame : held)
    assert(std::memcmp(frame->rgb565.data(), pixels.data(), pixels.size()) == 0);
  held.erase(held.begin());
  assert(bounded.push(std::span(reinterpret_cast<const uint8_t*>(rawPart.data()), rawPart.size()), hold));
  assert(held.size() == 3);
  for (const std::string bad : {
      "Content-Length: 8\r\nX-Width: 2\r\n", // Missing height.
      "Content-Length: 8\r\nX-Width: 2\r\nX-Height: 3\r\n", // Wrong size.
      "Content-Length: 8\r\nX-Width: 513\r\nX-Height: 2\r\n",
      "Content-Length: 8\r\nX-Width: 2\r\nX-Width: 2\r\nX-Height: 2\r\n",
  }) {
    const auto malformed = "--frame\r\nContent-Type: application/x-rgb565-be\r\n" + bad + "\r\n";
    MjpegParser parser;
    assert(!parser.push(std::span(reinterpret_cast<const uint8_t*>(malformed.data()), malformed.size()),
        [](JpegPacket) { assert(false); }));
  }
  std::cout << "JPEG/RGB565 multipart fragmentation, exact dimensions, immutable pool and malformed bounds passed\n";
}
