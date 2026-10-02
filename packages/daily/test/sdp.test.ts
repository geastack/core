import assert from "node:assert/strict";
import test from "node:test";
import {
  parseConsumer,
  parseTransport,
  receiveCapabilities,
} from "../src/rtp.ts";
import {
  localDtlsParameters,
  microphoneAnswer,
  microphoneParameters,
  receiveOffer,
} from "../src/sdp.ts";

const opus = {
  mimeType: "audio/opus",
  preferredPayloadType: 100,
  clockRate: 48000,
  channels: 2,
  parameters: {},
  rtcpFeedback: [],
};
const h264 = {
  mimeType: "video/H264",
  preferredPayloadType: 107,
  clockRate: 90000,
  parameters: { "packetization-mode": 1, "profile-level-id": "42e01f" },
  rtcpFeedback: [
    { type: "nack" },
    { type: "nack", parameter: "pli" },
    { type: "transport-cc" },
  ],
};
const vp8 = {
  mimeType: "video/VP8",
  preferredPayloadType: 101,
  clockRate: 90000,
  parameters: {},
  rtcpFeedback: [
    { type: "nack" },
    { type: "nack", parameter: "pli" },
    { type: "transport-cc" },
  ],
};
const fingerprint = Array.from({ length: 32 }, () => "AB").join(":");
const transportInput = {
  id: "test-transport",
  iceParameters: {
    usernameFragment: "user",
    password: "password-for-fixture",
    iceLite: true,
  },
  iceCandidates: [
    {
      foundation: "1",
      priority: 100,
      ip: "192.0.2.1",
      port: 40000,
      protocol: "udp",
      type: "host",
    },
  ],
  dtlsParameters: {
    role: "auto",
    fingerprints: [{ algorithm: "sha-256", value: fingerprint }],
  },
};

test("receive capabilities preserve router payload IDs and exclude unsupported codecs/extensions", () => {
  const capabilities = receiveCapabilities({
    codecs: [
      opus,
      h264,
      vp8,
      { ...vp8, mimeType: "video/rtx", preferredPayloadType: 102 },
    ],
  });
  assert.deepEqual(
    capabilities.codecs.map((item) => item.preferredPayloadType),
    [100, 107, 101],
  );
  assert.deepEqual(capabilities.codecs[1]?.rtcpFeedback, [
    { type: "nack", parameter: "" },
    { type: "nack", parameter: "pli" },
  ]);
  assert.deepEqual(capabilities.headerExtensions, []);
  assert.deepEqual(capabilities.codecs[2]?.rtcpFeedback, [
    { type: "nack", parameter: "pli" },
  ]);
  assert.throws(
    () => receiveCapabilities({ codecs: [opus] }),
    /baseline H.264/,
  );
});

test("VP8-only producers can negotiate without claiming RTX or generic NACK support", () => {
  const capabilities = receiveCapabilities({ codecs: [opus, vp8] });
  assert.deepEqual(
    capabilities.codecs.map((item) => item.preferredPayloadType),
    [100, 101],
  );
  assert.throws(() =>
    receiveCapabilities({ codecs: [opus, { ...vp8, rtcpFeedback: [] }] }),
  );
  const consumer = parseConsumer({
    id: "video",
    producerId: "avatar",
    kind: "video",
    rtpParameters: {
      codecs: [
        {
          ...vp8,
          payloadType: 101,
          rtcpFeedback: capabilities.codecs[1]?.rtcpFeedback,
        },
      ],
      encodings: [{ ssrc: 7001 }],
      rtcp: { cname: "avatar", reducedSize: true },
    },
  });
  const sdp = receiveOffer(parseTransport(transportInput), [consumer], 1);
  assert.match(sdp, /a=rtpmap:101 VP8\/90000/);
  assert.match(sdp, /a=rtcp-fb:101 nack pli/);
  assert.doesNotMatch(sdp, /a=rtcp-fb:101 nack\r\n/);
  assert.doesNotMatch(sdp, /rtx|transport-cc/);
});

test("receive SDP carries both media sections on a single ICE/DTLS transport", () => {
  const transport = parseTransport(transportInput);
  const consumers = [opus, h264].map((item, index) =>
    parseConsumer({
      id: `consumer-${index}`,
      producerId: `producer-${index}`,
      kind: index ? "video" : "audio",
      rtpParameters: {
        codecs: [{ ...item, payloadType: item.preferredPayloadType }],
        encodings: [{ ssrc: 7000 + index }],
        headerExtensions: [],
        rtcp: { cname: "avatar", reducedSize: true },
      },
    }),
  );
  const sdp = receiveOffer(transport, consumers, 1);
  assert.match(sdp, /a=group:BUNDLE 0 1\r\n/);
  assert.match(sdp, /m=audio 7 UDP\/TLS\/RTP\/SAVPF 100\r\n/);
  assert.match(sdp, /m=video 7 UDP\/TLS\/RTP\/SAVPF 107\r\n/);
  assert.equal(sdp.split("a=setup:actpass").length - 1, 2);
  assert.match(sdp, /a=ssrc:7001 msid:daily consumer-1/);
  assert.match(sdp, /a=ice-lite/);
});

test("microphone negotiation uses the actual local SSRC, payload and fingerprint", () => {
  const sdp = [
    "v=0",
    `a=fingerprint:sha-256 ${fingerprint}`,
    "a=setup:actpass",
    "m=audio 9 UDP/TLS/RTP/SAVPF 111",
    "a=mid:0",
    "a=rtpmap:111 opus/48000/2",
    "a=fmtp:111 minptime=10;useinbandfec=1",
    "a=ssrc:123456 cname:microphone",
  ].join("\r\n");
  const parameters = microphoneParameters(sdp);
  assert.equal(parameters.encodings[0]?.ssrc, 123456);
  assert.equal(parameters.codecs[0]?.payloadType, 111);
  assert.equal(parameters.codecs[0]?.parameters.useinbandfec, "1");
  assert.equal(localDtlsParameters(sdp).role, "client");
  const answer = microphoneAnswer(parseTransport(transportInput), parameters);
  assert.match(answer, /a=recvonly/);
  assert.match(answer, /a=setup:passive/);
  assert.match(answer, /a=rtpmap:111 opus\/48000\/2/);
  assert.throws(() => microphoneParameters("v=0"), /incomplete/);
});

test("untrusted SFU parameters cannot inject SDP lines", () => {
  assert.throws(
    () =>
      parseTransport({
        ...transportInput,
        iceParameters: {
          ...transportInput.iceParameters,
          usernameFragment: "bad\r\na=setup:active",
        },
      }),
    /token/,
  );
  assert.throws(
    () =>
      receiveCapabilities({
        codecs: [
          opus,
          { ...h264, parameters: { "profile-level-id": "42e01f;attack=1" } },
        ],
      }),
    /parameter/,
  );
});
