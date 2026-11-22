# @geastack/host

Platform service sources for GeaStack native targets: the C++ host layer
between the framework runtime and the device. It defines the host contract
(display, touch and input, timers, storage, networking, audio, camera, media,
images, memory, clock, battery, window, notifications) and ships portable
implementations of the services a platform can share, plus the cross-cutting
runtime services such as diagnostics, app state and display mirroring.

```sh
npm install @geastack/host
```

## How it is used

A target build does not list these sources by hand. It points
`@geastack/core`'s source manifest at this package and receives the sources and
include roots for the platform being built. A new target implements the host
headers it cannot satisfy with the portable sources.

There is no JavaScript entry point; the package exists to be compiled.

On ESP32 the native WebSocket client applies the connection headers passed to
the `@geastack/core` WebSocket constructor (since host 0.1.16 and core 0.1.41).
Headers are applied to both the ESP-IDF WebSocket client and its owned
transport, so bearer authentication works on the actual HTTP upgrade request.
Connections opened with only a URL and subprotocol keep their defaults.

## License

Apache-2.0. See `LICENSE`.
