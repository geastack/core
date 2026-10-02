// SPDX-License-Identifier: Apache-2.0
#include "host/rtc_vp8_receiver.h"
#include <cassert>
#include <iostream>

using namespace gea::host::rtc;

static std::vector<uint8_t> packet(uint16_t sequence, uint32_t timestamp,
                                 bool marker, std::initializer_list<uint8_t> payload,
                                 uint8_t payloadType = 101, uint32_t ssrc = 123) {
  std::vector<uint8_t> bytes{0x80, uint8_t(payloadType | (marker ? 0x80 : 0)),
                           uint8_t(sequence >> 8), uint8_t(sequence)};
  bytes.resize(12);
  rtp::write32(bytes.data() + 4, timestamp);
  rtp::write32(bytes.data() + 8, ssrc);
  bytes.insert(bytes.end(), payload);
  return bytes;
}

int main() {
  static_assert(sizeof(rtp::Reorder) < 8192, "Idle RTP windows must not reserve full packet buffers");
  rtp::Reorder ordered;
  for (uint16_t sequence = 0; sequence < 200; ++sequence) {
    const auto input = packet(sequence, 123, false, {42});
    bool delivered = false;
    ordered.push(input, sequence, [&](const rtp::Packet& p) {
      assert(p.payload.data() == input.data() + 12); // No in-order packet copy.
      assert(p.payload[0] == 42);
      delivered = true;
    });
    assert(delivered);
  }
  auto future = packet(201, 123, false, {17});
  ordered.push(future, 201, [](const rtp::Packet&) { assert(false); });
  future.back() = 99; // A buffered packet must own its data independently.
  unsigned delivered = 0;
  ordered.push(packet(200, 123, false, {16}), 202, [&](const rtp::Packet& p) {
    assert(p.payload[0] == (delivered++ ? 17 : 16));
  });
  assert(delivered == 2);

  // Empty timer polls after draining must not retain a stale occupancy count.
  for (unsigned i = 0; i < 100; ++i)
    ordered.poll(1000 + i, [](const rtp::Packet&) { assert(false); });
  ordered.push(packet(202, 123, false, {18}), 1100, [](const rtp::Packet& p) {
    assert(p.sequence == 202 && p.payload[0] == 18);
  });
  ordered.push(packet(204, 123, false, {20}), 1101, [](const rtp::Packet&) { assert(false); });
  ordered.reset();
  ordered.poll(1200, [](const rtp::Packet&) { assert(false); });
  ordered.push(packet(300, 123, false, {30}), 1201, [](const rtp::Packet& p) {
    assert(p.sequence == 300);
  });
  ordered.push(packet(302, 123, false, {32}), 1202, [](const rtp::Packet&) { assert(false); });
  // A large jump clears both queued data and its occupancy count.
  ordered.push(packet(1000, 123, false, {10}), 1203, [](const rtp::Packet& p) {
    assert(p.sequence == 1000);
  });
  ordered.poll(1400, [](const rtp::Packet&) { assert(false); });
  ordered.push(packet(1002, 123, false, {12}), 1401, [](const rtp::Packet&) { assert(false); });
  delivered = 0;
  ordered.push(packet(1001, 123, false, {11}), 1402, [&](const rtp::Packet& p) {
    assert(p.sequence == 1001 + delivered++);
    if (p.sequence == 1002) ordered.reset(); // Reset from a synchronous consumer.
  });
  assert(delivered == 2);
  ordered.poll(1500, [](const rtp::Packet&) { assert(false); });

  auto bytes = packet(65535, 0xffffff00, true, {0x10, 0x00});
  const auto parsed = rtp::parse(bytes);
  assert(parsed && parsed->sequence == 65535 && parsed->timestamp == 0xffffff00);
  assert(parsed->marker && parsed->ssrc == 123 && parsed->payloadType == 101);
  for (size_t size = 0; size <= 12; ++size) assert(!rtp::parse(std::span(bytes).first(size)));
  bytes[0] = 0x40;
  assert(!rtp::parse(bytes));
  bytes[0] = 0x81; // Missing CSRC.
  assert(!rtp::parse(bytes));
  bytes.insert(bytes.begin() + 12, {0, 0, 0, 1});
  assert(rtp::parse(bytes)->payload.size() == 2);
  bytes[0] |= 0x10;
  bytes.insert(bytes.begin() + 16, {0xbe, 0xde, 0, 1, 0x10, 0x12, 0, 0});
  assert(rtp::parse(bytes)->payload.size() == 2);
  bytes[19] = 0xff;
  assert(!rtp::parse(bytes));
  bytes[19] = 1;
  bytes[0] |= 0x20;
  bytes.insert(bytes.end(), {0, 0, 0, 4});
  assert(rtp::parse(bytes)->payload.size() == 2);
  bytes.back() = 0;
  assert(!rtp::parse(bytes));
  bytes.back() = 7; // Padding extends into the extension/header.
  assert(!rtp::parse(bytes));
  bytes.back() = 4;
  bytes[1] = 206;
  assert(!rtp::parse(bytes)); // RTCP is not an RTP payload.

  vp8::Receiver receiver(101, 123);
  std::vector<vp8::EncodedFrame> frames;
  auto deliver = [&](vp8::EncodedFrame frame) { frames.push_back(std::move(frame)); };
  assert(receiver.takeKeyframeRequest(0));
  assert(!receiver.takeKeyframeRequest(499));
  assert(receiver.takeKeyframeRequest(500));
  // In-order data is emitted immediately, without the reordering timeout.
  receiver.push(packet(65534, 9000, true, {0x10, 0x00}), 501, deliver);
  assert(frames.size() == 1 && frames.back().keyframe);
  assert(!receiver.takeKeyframeRequest(1001));
  // Arrival 65535, 1, 0 must reassemble as 65535, 0, 1 across sequence wrap.
  receiver.push(packet(65535, 12000, false, {0x10, 1}), 510, deliver);
  receiver.push(packet(1, 12000, true, {0, 3}), 511, deliver);
  assert(frames.size() == 1);
  receiver.push(packet(1, 12000, true, {0, 3}), 512, deliver); // Duplicate buffered tail.
  receiver.push(packet(0, 12000, false, {0, 2}), 513, deliver);
  assert(frames.size() == 2 && !frames.back().keyframe);
  assert((frames.back().bytes == vp8::EncodedFrame::Bytes{1, 2, 3}));
  receiver.push(packet(0, 12000, false, {0, 2}), 514, deliver); // Late duplicate.
  assert(frames.size() == 2 && !receiver.needsKeyframe());

  // Neither bundled audio nor another video SSRC can advance the sequence.
  receiver.push(packet(1000, 9999, true, {0x10, 0}, 111), 520, deliver);
  receiver.push(packet(1000, 9999, true, {0x10, 0}, 101, 456), 520, deliver);
  receiver.push(packet(2, 15000, true, {0x10, 1}), 521, deliver);
  assert(frames.size() == 3 && frames.back().timestamp == 15000);

  // Lost middle packet: wait briefly, then reject the entire damaged picture
  // and its dependent pictures; a later keyframe restores the stream.
  receiver.push(packet(3, 18000, false, {0x10, 1}), 530, deliver);
  receiver.push(packet(5, 18000, true, {0, 2}), 531, deliver);
  receiver.poll(560, deliver);
  assert(!receiver.needsKeyframe());
  receiver.poll(561, deliver);
  assert(receiver.needsKeyframe() && frames.size() == 3);
  assert(receiver.takeKeyframeRequest(1002));
  receiver.push(packet(6, 21000, true, {0x10, 1}), 1003, deliver);
  assert(frames.size() == 3);
  receiver.push(packet(7, 24000, true, {0x10, 0, 5}), 1004, deliver);
  assert(frames.size() == 4 && !receiver.needsKeyframe());

  // Overflow does not retain an unbounded video backlog or decode broken refs.
  receiver.push(packet(1000, 27000, true, {0x10, 1}), 1010, deliver);
  assert(receiver.needsKeyframe() && frames.size() == 4);
  receiver.push(packet(1001, 30000, true, {0x10, 0}), 1011, deliver);
  assert(frames.size() == 5);
  receiver.requireKeyframe(); // Decoder reports a bad reference or queue overflow.
  receiver.push(packet(1002, 33000, true, {0x10, 1}), 1012, deliver);
  assert(frames.size() == 5 && receiver.needsKeyframe());
  receiver.push(packet(1003, 36000, true, {0x10, 0}), 1013, deliver);
  assert(frames.size() == 6);

  const auto pli = rtp::pictureLossIndication(0x11223344, 0xabcdef01);
  assert((pli == std::array<uint8_t, 12>{0x81, 206, 0, 2, 0x11, 0x22, 0x33, 0x44,
                                        0xab, 0xcd, 0xef, 1}));
  std::cout << "RTP bounds, sequence wrap, reordering, loss recovery and PLI passed\n";
}
