# Gea Daily transport

Work in progress: typed room discovery, signaling, RTP/SDP negotiation, and media
control for a native Daily WebRTC client. This package does **not yet implement a complete Daily call** and
is not published. The Lemon Slice browser example still uses the official SDK.

The protocol is based on the public Daily 0.92.2 call-engine behavior, inspected
at `https://c.daily.co/call-machine/versioned/0.92.2/static/call-machine-object-bundle.js`.
Daily's signaling protocol is not a stable public API. This is independently
written TypeScript, not a copy of its bundled implementation.

Room lookup is a form POST. Signaling and Mediasoup RPC share the returned WSS
connection. Requests are correlated before sending, time out individually, and
are rejected on close. Closing during join also rejects the pending join.
Credentials are never included in diagnostics. The host supplies HTTP, WebSocket,
clock, and UUID facilities; no DOM, injected scripts, or server bridge is needed.

Verified against a real hosted Zuck room: discovery, signaling acknowledgement,
SFU join, receive-transport creation, audio/video track announcements, `bot_ready`,
and `force-end` followed by a confirmed terminal hosted-session status. The
diagnostic lasted 15 seconds and did not capture or play audio. Twelve deterministic
tests cover request correlation, close-during-join, timeouts, batched events,
track updates, malformed input, and migration failure.

`DailyClient` owns discovery, signaling, microphone capture, and cleanup.
`DailyMedia` negotiates an Opus microphone transport and a bundled Opus/VP8 or H.264
receive transport through native `RTCPeerConnection`. RTP parameters retain
server payload types and SSRCs. External responses are validated before use.

The shared Gea host now has an H.264 receive worker and a native video element.
These are not yet verified in a live S3 call. SDP and media lifecycle tests pass;
they do not substitute for transport, A/V synchronization, or hardware testing.
The complete native application now builds and boots on the Waveshare S3 4B.
The previously flashed build created an audio consumer but rejected video:
the hosted Zuck producer publishes VP8, whereas that build supported H.264. A diagnostic
consumer using the router's full capabilities confirmed VP8 payload type 101
and RTX type 102. The new native receive adapter implements VP8 without RTX,
advertising only PLI feedback for that codec. The hosted `/liveai/rooms` API accepts only
`agent_id`; it exposes no producer codec selection.

The shared host now has VP8 RTP reordering, payload assembly and libvpx decoding,
covered by `bash packages/core/test/run-vp8-tests.sh` from the core repo. Those
desktop tests do not establish ESP32 transport support or decoding performance.
The new ICE/DTLS/SRTP adapter compiles for the S3. Actual media, AEC, playback timing, and teardown still need
hardware verification. A native compiler/runtime probe is in
`test/native-signaling.ts`, including outgoing RPC fields and correlation IDs.
Worker migration currently closes with an explicit reconnect error; it does not
silently keep a dead room alive.
