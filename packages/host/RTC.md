# WebRTC host support

The standard JavaScript APIs bind to `host/rtc.h`. Application code uses
`RTCPeerConnection`, data channels and media tracks; the ESP implementation uses
`esp_peer` for ICE, DTLS, SRTP and SCTP. There is no LiveKit-specific native API.

Implemented here:

- ICE URLs, TURN credentials and `iceTransportPolicy` survive typed construction.
- Audio RTP senders, receivers and transceivers retain stable handle identity;
  requested direction is separate from the direction accepted in SDP.
- Text and binary data channels support ordered/unordered delivery and partial
  reliability. Native callbacks are delivered by the normal Gea frame-loop pump.
- Signaling state and local/remote SDP snapshots are retained.
- `RTCSessionDescription` exposes read-only type/SDP fields and creates fresh
  dictionaries through `toJSON()`. Repeated native peer reads retain the same
  snapshot identity until SDP changes. Generated JavaScript executes construction,
  JSON serialization, null checks and passing a description back into the peer.
- PCM is encoded to Opus before sending, and received Opus is decoded before it
  enters a media track. Frames are 20 ms, 16 kHz mono PCM, with 24 kbps Opus.
  SDP uses the required `opus/48000/2` signaling, independent of PCM bandwidth
  ([RFC 7587 section 7](https://www.rfc-editor.org/rfc/rfc7587.html#section-7)).
- Remote audio tracks do not start microphone capture. Disabling a local track
  detaches it from capture and clears stale captured samples; reenabling attaches
  it once. Stop is terminal and idempotent.
- `new MediaStream()` creates an empty container; stream and track-list
  constructors create independent containers sharing track identities. Track
  membership, lookup and active-state APIs work without starting capture or
  stopping a track when it is removed from a container.
- Native audio sinks can register independent PCM readers on a track. Readers
  share one bounded buffer, start at live audio, and report samples lost when a
  consumer stalls. Transport reads do not consume another reader's audio;
  muting/stopping clears queued samples for every reader. This is the playback
  data source; connecting browser media elements to the output driver remains
  outstanding.
- SDP creation returns a pending typed promise. The ESP control worker does
  transport setup and waits for SDP; only the UI event pump settles the promise.
  Closing rejects pending descriptions and ignores late worker results.
- Closing an ESP peer detaches it immediately and queues worker joins and native
  teardown in the background, preserving state until its workers have stopped.
- Streaming audio pause discards buffered PCM at pause time and retains the
  reader for subsequent packets; resuming does not discard a new reply prefix.
  The display meter distinguishes empty reads from real PCM silence. Its 250 ms
  stale-telemetry expiry never controls playback or microphone capture.

## Current limits

This is not yet a complete browser WebRTC implementation. ESP peers currently
support one audio transceiver per connection and cannot change direction after
opening. Video, negotiated channel IDs, channel protocols, ICE restart, receiver
playout statistics, and complete browser `EventTarget`/media-element semantics
remain unsupported. Unsupported channel options and direction changes reject
explicitly instead of silently succeeding.

`esp_peer_send_data` accepting a packet is the current buffered-amount boundary;
this is not a measurement of its internal SCTP retransmission queue.

Native handle registry entries remain until `rtc::destroy_handle`; peer `close()`
releases the ESP transport/tasks, but automatic JavaScript finalization of the
remaining registry entries is not wired yet. Repeated room creation therefore
also needs lifecycle integration before a long-running device deployment.

JavaScript equality between two native host objects still refuses compilation;
the compiler needs an explicit host identity-comparison contract. Null checks
work, and description identity is separately verified at the native boundary.

The unmodified LiveKit JavaScript SDK remains the integration target. Passing
these focused tests is not proof that an entire LiveKit room can run on a board.

## Focused verification

After building `packages/geatsc-plugin-gea`, from the core repository:

```sh
bash packages/core/test/run-rtc-tests.sh
```

The tests run with AddressSanitizer and UndefinedBehaviorSanitizer. The codec
test requires desktop `libopus` and `pkg-config`; firmware declares `78/esp-opus`
in target component manifests.

From the sibling compiler repository:

```sh
node test/native-rtc.mjs
```

This compiles real TypeScript using the core declarations and Gea plugin, checks
native carrier emission, links and runs the emitted C++ against a transport test
double, and executes the typed RTC callback/configuration adapters. No dynamic
boxing is used for RTC handles or SDP dictionaries; thrown JavaScript errors
retain their legitimate dynamic exception carrier. No network room is opened.
