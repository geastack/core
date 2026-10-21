# Desktop WebSocket transport

`host/websocket.cpp` owns handles, ready state, native consumers, and worker-owned
script dispatch. `host/websocket_platform.h` is the typed transport event boundary;
network threads never run script callbacks directly.

Merely compiling `websocket.cpp` selects weak fallback hooks; a handle from those
hooks does **not** establish a connection. Native desktop target manifests select
a strong provider explicitly: macOS uses the Apple provider, Raspberry Pi OS and
Sailfish use the curl provider.

## Apple provider

`host/websocket_apple.mm` implements the provider with
`NSURLSessionWebSocketTask`. Link it with ARC, blocks and Foundation.

The provider uses an ephemeral session without cookie or credential storage,
10-second connection requests, a 1 MiB receive-message limit and a 512 KiB send
queue (also capped at 1,024 messages). NSURLSession performs framing, ping/pong
and system TLS verification, and the peer's close code is preserved.

Native producers run on a serial timer queue; native consumers run on the receive
callback. Producer/consumer functions must obey their existing native-only API
contract. Closing or destruction prevents further sends, and closed sockets refuse
new producers. Terminal transports are reaped independently of explicit
`destroy_handle`; closed facade handles retain `url` and `CLOSED` state. Terminal
native-consumer captures and receive buffers are released before the close
callback. Producer replacement, closure and destruction retire callback captures
outside provider locks, so a captured-object destructor may reenter socket queries
and producer replacement. The facade's metadata map is not cleaned up
automatically.

## Linux provider

`host/websocket_curl.cpp` is the Linux desktop provider, linked with libcurl
>= 7.86. It builds against a libcurl without ws/wss (Debian 12 and Raspberry Pi OS
Bookworm ship 7.88.1 without them); there every connection fails with an error
event, "WebSocket protocol not enabled in libcurl". Connecting needs a libcurl that
lists ws/wss in `curl-config --protocols`, such as Debian 13's. macOS's system
libcurl lacks ws/wss, so Linux runtime behavior has to be tested on Linux.

The Linux worker alone owns libcurl calls. Sends have byte/entry bounds; complete
text is assembled and UTF-8 checked within 1 MiB. Script events use the same owner
queue as Apple. Closing drains already-admitted frames before sending CLOSE;
destruction cancels the worker without joining on the caller. Finished workers
are joined only during terminal reaping. System TLS verification remains enabled.
Partial sends follow the
[libcurl partial-send contract](https://curl.se/libcurl/c/curl_ws_send.html):
the remaining payload and returned byte count with `fragsize=0`.

## Tests

Run the loopback test:

```sh
node packages/core/test/run_websocket_desktop.mjs
```

It compiles the real provider and host facade, binds only an ephemeral loopback
listener owned by the fixture, then checks subprotocol/custom headers, masked
client text, fragmented/large/empty text, binary exclusion, ping/pong, native
producer/consumer delivery, owner-thread script callbacks, remote/local close,
close before opening, rejected upgrade, invalid URL, destruction and transport
reclamation across reconnect without explicit handle destruction.

The Linux fixture adds `GEA_TEST_CURL_PARTIAL_SEND`: only its socket option sets a
small send buffer, while the production send algorithm is unchanged. The peer
delays reading and verifies a complete 500,000-byte message followed by CLOSE; the
fixture requires an actual short send or AGAIN result. Selection tests execute
the macOS and Raspberry Pi OS source-array blocks and reject missing providers.
TLS certificate/refusal integration is not covered by these tests.

## Main-realm callback ownership

The main Context registers one facade cleanup, which destroys its socket handles
and releases script callbacks during permanent main-task retirement. Dedicated
worker ownership is unchanged. All typed runtime, boxed property and fixture
handler assignments, including the compiler's typed CallableObject overloads, use
the checked `set_open_callback`, `set_message_callback`, `set_close_callback` and
`set_error_callback` SDK functions. Retired or destroyed handles refuse new
captures. Replacement first swaps out the old callback, then finishes table
mutation before releasing it, so a captured destructor may reenter handle
destruction without invalidating a live table operation. Close dispatch also
detaches all callbacks before invoking transport or user code.
