# Gea Daily transport

Work in progress: typed room discovery, signaling, RTP/SDP negotiation, and media
control for a native Daily WebRTC client. This package does **not yet implement a complete Daily call** and
is not published. Browser applications should use the official Daily SDK.

The protocol is based on the public Daily 0.92.2 call-engine behavior, inspected
at `https://c.daily.co/call-machine/versioned/0.92.2/static/call-machine-object-bundle.js`.
Daily's signaling protocol is not a stable public API. This is independently
written TypeScript, not a copy of its bundled implementation.

Room lookup is a form POST. Signaling and Mediasoup RPC share the returned WSS
connection. Requests are correlated before sending, time out individually, and
are rejected on close. Closing during join also rejects the pending join.
Credentials are never included in diagnostics. The host supplies HTTP, WebSocket,
clock, and UUID facilities; no DOM, injected scripts, or server bridge is needed.

Verified against a real hosted Daily room: discovery, signaling acknowledgement,
SFU join, receive-transport creation, audio/video track announcements, `bot_ready`,
and `force-end` followed by a confirmed terminal hosted-session status. The
diagnostic lasted 15 seconds and did not capture or play audio. Twenty
deterministic `node:test` cases in `test/` (`signaling.test.ts`, `sdp.test.ts`,
`media.test.ts`) cover request correlation, close-during-join, timeouts, batched
events, track updates, SDP negotiation, malformed input, and migration failure.
The package has no test script and no runner elsewhere in this repository.

`DailyClient` owns discovery, signaling, microphone capture, and cleanup.
`DailyMedia` negotiates an Opus microphone transport and a bundled Opus/VP8 or H.264
receive transport through native `RTCPeerConnection`. RTP parameters retain
server payload types and SSRCs. External responses are validated before use.

Current status of the native media path:

- In the shared Gea host (`@geastack/host`): an H.264 receive worker, a native
  video element, VP8 RTP reordering, payload assembly and libvpx decoding, and
  an ICE/DTLS/SRTP adapter for the ESP32-S3.
- The VP8 receive adapter does not negotiate RTX and advertises only PLI
  feedback for that codec. Applications must negotiate the codec the remote
  producer offers.
- Desktop tests: `bash packages/core/test/run-vp8-tests.sh` from the core repo,
  after building `packages/geatsc-plugin-gea` (see the repository README),
  covers the VP8 path. These tests do not establish ESP32 transport support or
  decoding performance.
- `test/native-signaling.ts` is a native compiler/runtime probe of signaling,
  including outgoing RPC fields and correlation IDs.
- Worker migration closes with an explicit reconnect error; it does not keep a
  dead room alive.
- Not verified on hardware: live media in an S3 call, A/V synchronization, AEC,
  playback timing, and teardown.
