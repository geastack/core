import type { HostShimDefinitions } from "./types.js";
import { geaHostExternDeclarations, geaVirtualListRowHeightEmit } from "./host-declarations.js";
import {
  CSS_STYLE_DECLARATION_NATIVE_TYPES,
  CSS_STYLE_DECLARATION_NATIVE_MEMBER_METHODS,
} from "./css-style-declaration-shims.js";

// Receiver types a DOM-node property getter binding matches: the concrete C++
// handle plus every DOM interface name that lowers to it (see `nativeTypes`).
// geatsc probes a property read with both the TS type display name and the C++
// storage type, so listing both forms makes the binding fire regardless of
// which the checker resolved.
const NODE_HANDLE_RECEIVER_TYPES = [
  "gea::embedded::ui::NodeHandle",
  "Node",
  "ChildNode",
  "ParentNode",
  "Element",
  "HTMLElement",
  "DocumentFragment",
  "Text",
  "Comment",
  "GeaNode",
  "GeaElement",
  "GeaCanvasElement",
  "GeaCameraElement",
  "VirtualListElement",
];

// Receiver type names for the event a listener is handed. One native struct
// backs every gea event (`gea::framework::events::PointerEvent`), so the
// qualified storage type and every declared interface that lowers to it are
// listed together -- the same "one runtime type, many declared names" join
// `NODE_HANDLE_RECEIVER_TYPES` above states.
const EVENT_RECEIVER_TYPES = [
  "gea::framework::events::PointerEvent",
  // The base interface every event extends, and the parameter `onScroll` and
  // `EventTarget.addEventListener(type, listener)` declare.
  "Event",
  "PointerEvent",
  "TouchEvent",
  "RotaryEvent",
  // `InputEvent` and `KeyEvent` (index.d.ts) are the same one runtime struct:
  // the engine dispatches `PointerEvent&` for `input`/`keydown` exactly as it
  // does for the pointer types, with `target`/`currentTarget`/`keyCode` already
  // filled in (`GeaTextAreaView textDidChange:` sets type + targetId, the
  // listener adapter sets currentTarget). Leaving them off this list -- and off
  // `nativeTypes` -- gave an `onInput={e => ...}` handler's parameter no host
  // carrier, so it lowered to a synthesized record and the runtime listener
  // adapter took its `abort()` arm on the FIRST keystroke.
  "InputEvent",
  "KeyEvent",
];

// Receiver type names for the two WebRTC/WebSocket host wrappers. Both the
// qualified storage type and the interface name a declaration site spells
// (`index.d.ts` names the instance interfaces `WebSocketInstance` /
// `RTCPeerConnectionInstance`; the bare `WebSocket` / `RTCPeerConnection` are
// the ambient CONSTRUCTOR globals), the same join every list here states.
const WEB_SOCKET_RECEIVER_TYPES = ["gea::host::WebSocket", "WebSocketInstance"];

const RTC_PEER_CONNECTION_RECEIVER_TYPES = [
  "gea::host::RTCPeerConnection",
  "RTCPeerConnectionInstance",
];

// Receiver type names for the node an event names as its target. The engine
// hands `gea::framework::events::EventTarget` (a node id plus attribute
// accessors), which is what `InputEvent.target` / `.currentTarget` -- declared
// as `InputEventTarget` in index.d.ts -- actually are at runtime.
const EVENT_TARGET_RECEIVER_TYPES = [
  "gea::framework::events::EventTarget",
  "InputEventTarget",
];

// Receiver type names for a concrete `gea::host::GeaEmbeddedImage` value: the
// qualified storage type, the unqualified TS interface name, and the `LoadedImage`
// alias used by app code (TS resolves the alias, but list it defensively).
const IMAGE_RECEIVER_TYPES = [
  "gea::host::GeaEmbeddedImage",
  "GeaEmbeddedImage",
  "LoadedImage",
];

const DISPLAY_NO_THROW_METHODS = [
  "getBrightness",
  "setBrightness",
  "getDevicePixelRatio",
  "setDevicePixelRatio",
  "getFrameIntervalMs",
  "setFrameIntervalMs",
  "getFrameRate",
  "setFrameRate",
  "getOrientation",
  "setOrientation",
  "getSupportedOrientations",
  "setSupportedOrientations",
  "getAutoRotate",
  "setAutoRotate",
  "setVSync",
  "setTextRasterCache",
  "getPixelFormat",
  "setPixelFormat",
  "getPanelPixelFormat",
  "getSupportedPixelFormats",
  "setAA",
  "setFlushConfig",
  "setMemoryConfig",
  "setEpaperRefreshConfig",
  "epaperFullRefresh",
];

const MEMORY_NO_THROW_METHODS = [
  "internalFree",
  "internalLargestFreeBlock",
  "internalMinimumFree",
  "psramFree",
  "currentTaskStackHighWaterMark",
  "geaMainStackBytes",
  "geaInitStackBytes",
  "appFrameStackWords",
  "appFrameStackBytes",
  "displayFlushConfiguredRows",
  "displayFlushConfiguredDepth",
  "displayFlushBufferMaxBytes",
  "displayFlushRows",
  "displayFlushDepth",
  "displayFlushBufferBytes",
];

const HOST_NATIVE_CALLABLE_SIDECARS: NonNullable<
  HostShimDefinitions["frameworkProtocols"]
> = [
  ...(
    [
      ["reactive-text-value", "reactiveTextValue", 3],
      ["reactive-text", "reactiveText", 3],
      ["reactive-attr", "reactiveAttr", 4],
      ["reactive-class", "reactiveClass", 3],
      ["reactive-class-name", "reactiveClassName", 3],
      ["reactive-style", "reactiveStyle", 3],
      ["reactive-style-prop", "reactiveStyleProp", 4],
      ["reactive-bool-attr", "reactiveBoolAttr", 4],
      ["reactive-bool", "reactiveBool", 4],
      ["reactive-value-read", "reactiveValueRead", 3],
      ["reactive-value", "reactiveValue", 3],
      ["reactive-html", "reactiveHtml", 3],
    ] as const
  ).map(([id, exportName, parameterIndex]) => ({
    kind: "host-native-callable-sidecar-v1" as const,
    semanticCategory: "framework-protocol" as const,
    id: `gea.host-native-callable.${id}`,
    version: 1 as const,
    payload: {
      callable: { moduleSpecifier: "/", exportName },
      operations: {
        disposerContainedCallbacks: { parameterIndices: [parameterIndex] },
        structuralDynamicParameters: [
          {
            parameterIndex: 2,
            structuralType: "object" as const,
            transport: "declared-dynamic" as const,
          },
        ],
      },
    },
  })),
  {
    kind: "host-native-callable-sidecar-v1",
    semanticCategory: "framework-protocol",
    id: "gea.host-native-callable.conditional",
    version: 1,
    payload: {
      callable: { moduleSpecifier: "/", exportName: "conditional" },
      operations: {
        structuralDynamicParameters: [
          {
            parameterIndex: 3,
            structuralType: "object",
            transport: "declared-dynamic",
          },
        ],
        retainedCleanupLifecycle: {
          disposerParameterIndex: 2,
          registrationTarget: {
            owner: {
              moduleSpecifier: "/runtime/disposer.ts",
              exportName: "Disposer",
            },
            instanceMethod: "add",
          },
          cleanupParameterIndex: 0,
        },
      },
    },
  },
  {
    kind: "host-native-callable-sidecar-v1",
    semanticCategory: "framework-protocol",
    id: "gea.host-native-callable.with-tracking",
    version: 1,
    payload: {
      callable: { moduleSpecifier: "/", exportName: "withTracking" },
      operations: {
        disposerContainedCallbacks: { parameterIndices: [2] },
      },
    },
  },
  {
    kind: "host-native-callable-sidecar-v1",
    semanticCategory: "framework-protocol",
    id: "gea.host-native-callable.bind-apply",
    version: 1,
    payload: {
      callable: {
        moduleSpecifier: "/",
        exportName: "bind",
      },
      operations: {
        disposerContainedCallbacks: { parameterIndices: [3] },
      },
    },
  },
  {
    kind: "host-native-callable-sidecar-v1",
    semanticCategory: "framework-protocol",
    id: "gea.host-native-callable.delegate-click",
    version: 1,
    payload: {
      callable: {
        moduleSpecifier: "/",
        exportName: "delegateClick",
      },
      operations: {
        disposerContainedCallbacks: { parameterIndices: [1] },
        callableExpandoProperties: [
          {
            key: "__gc",
            receiverTypes: ["gea::embedded::ui::NodeHandle"],
            receiverRole: "host-native-handle",
            valueRole: "callable",
            parameterTransports: [
              {
                kind: "dynamic-record",
                protocolId: "gea.pointer-event-record-v1",
              },
            ],
          },
        ],
        dataExpandoProperties: [
          {
            key: "__gdc",
            receiverTypes: ["gea::embedded::ui::Document"],
            receiverRole: "host-native-handle",
            valueRole: "dynamic",
          },
        ],
      },
    },
  },
];

export function createGeaHostShims(): HostShimDefinitions {
  return {
    // Gea embedded has no dynamic Symbol type: a `unique symbol` const used as a
    // property key lowers to its named string key (`__sym_<name>`) and well-known
    // symbol protocols read named members, so nothing boxes to a `gea_cpp_value`
    // symbol. (The general TypeScript/test262 path leaves this off.)
    lowerSymbolsAsNamedMembers: true,
    // On the gea embedded/IR backend a rendered node IS a `NodeHandle` (a typed
    // id into the retained UI tree). The DOM-shaped `Node`/`Element`/… types the
    // gea runtime is written against all lower to that one concrete handle, so
    // component templates flow as `NodeHandle` end-to-end with no `gea_cpp_value`
    // record bridge.
    nativeTypes: {
      // The ambient DOM global `document` (lib.dom.d.ts's `Document`) — a
      // singleton, not a tree node, so it gets its own storage rather than
      // sharing `NodeHandle`. Registering it here is what lets
      // `hostInterfaceCarrierRule` (`driver/type-carriers/host-interfaces.ts`,
      // via `hostNativeStorageForType`) seal a real carrier for the RECEIVER of
      // `document.<method>(...)`: without an entry, `document`'s receiver
      // operand resolved no representation at all and every typed
      // `document.getElementById(...)` (etc.) rejected with
      // `missing-operand-source-representation` on top of the template
      // mismatch fixed alongside this entry. `host-document-calls.ts`'s own
      // plans never actually load this value (its templates call
      // `Document::instance()` themselves — `document` is a singleton, so no
      // receiver value has to flow), so this entry exists purely to satisfy the
      // semantic-operation graph's requirement that the receiver operand of a
      // property-access callee resolve SOME carrier.
      Document: "gea::embedded::ui::Document",
      Node: "gea::embedded::ui::NodeHandle",
      ChildNode: "gea::embedded::ui::NodeHandle",
      ParentNode: "gea::embedded::ui::NodeHandle",
      Element: "gea::embedded::ui::NodeHandle",
      HTMLElement: "gea::embedded::ui::NodeHandle",
      DOMTokenList: "gea::embedded::ui::ClassList",
      DocumentFragment: "gea::embedded::ui::NodeHandle",
      Text: "gea::embedded::ui::NodeHandle",
      Comment: "gea::embedded::ui::NodeHandle",
      // An event target IS a DOM node on this backend; without this mapping
      // `event.target` flows as a boxed __gea_type_EventTarget record that won't
      // convert to NodeHandle (delegate-click assigns it to one).
      EventTarget: "gea::embedded::ui::NodeHandle",
      // The event a listener DECLARES (`onTouchMove={e => pan(e.clientX,
      // e.clientY)}`). One native struct backs every gea event -- the engine
      // hands `gea::framework::events::PointerEvent&` to every listener it
      // dispatches, and that struct already carries the full surface these
      // interfaces declare (`clientX`/`clientY`/`x`/`y`/`pageX`/`pageY`/
      // `screenX`/`screenY`/`pointerId`/`target`/`currentTarget`/`touches`).
      // So this is the same "one runtime type, many declared names" join the
      // DOM-alias rows above already state, not several carriers.
      //
      // Without these rows an event PARAMETER has no carrier at all, so the
      // emitter registered the handler as an arity-1 callable it could not
      // call: the runtime's listener overload refused at dispatch. And because
      // that refusal registered on the node rather than the body, the engine --
      // which dispatches to the hit-test target and does not walk ancestors --
      // never reached it either. A wrapper `<div>`'s drag handlers received
      // nothing, silently, and the app just did not respond.
      PointerEvent: "gea::framework::events::PointerEvent",
      TouchEvent: "gea::framework::events::PointerEvent",
      RotaryEvent: "gea::framework::events::PointerEvent",
      // The base interface, for `onScroll={e => ...}` and any listener typed by
      // `EventTarget.addEventListener`. Without it `e` lowered to a synthesized
      // record the listener adapter cannot build from `PointerEvent&`, so the
      // app aborted on the first scroll.
      Event: "gea::framework::events::PointerEvent",
      TouchPoint: "gea::framework::events::TouchPoint",
      // The text-input half of the same join. `onInput={e => ...}` /
      // `onKeyDown={e => ...}` declare `InputEvent` / `KeyEvent` (index.d.ts),
      // and the engine dispatches the SAME `PointerEvent&` for them: the macOS
      // text bridge writes the edited string into the node's `value` attribute
      // and posts `PointerEventType::Input` with `targetId` set
      // (`apple/targets/macos/main/macos_renderer.mm`), and the keydown path
      // fills `keyCode`. Without these two rows the handler parameter had no
      // carrier, so it lowered to a synthesized record
      // (`std::shared_ptr<gea_record_type_NNN>`) that
      // `gea::jsx::detail::addNodeListener` cannot construct from a
      // `PointerEvent&` -- its `if constexpr` fell to the `std::abort()` arm and
      // the app died on the first keystroke in any `<input>` / `<textarea>`.
      InputEvent: "gea::framework::events::PointerEvent",
      KeyEvent: "gea::framework::events::PointerEvent",
      // `InputEvent.target` / `.currentTarget`. Declared as `InputEventTarget`
      // (an `Element` with a `value: string`); at runtime it is the engine's own
      // `EventTarget` -- a node id plus attribute accessors -- which is where
      // `value` actually lives (the text bridge stores it as the `value`
      // attribute). v1 emitted exactly this:
      // `event.currentTarget.getAttribute("value")`.
      InputEventTarget: "gea::framework::events::EventTarget",
      // gea-embedded's own element interfaces (index.d.ts) — without these a
      // `Component<GeaCanvasElement>`'s `this.el` lowers to a record-alias struct
      // of std::functions whose boxed form has no `__gea_node_id`, so
      // `getContext('2d')` binds a CanvasRenderingContext2D to node -1 and every
      // draw silently no-ops (black screen, zero flush).
      GeaNode: "gea::embedded::ui::NodeHandle",
      GeaElement: "gea::embedded::ui::NodeHandle",
      GeaCanvasElement: "gea::embedded::ui::NodeHandle",
      GeaCameraElement: "gea::embedded::ui::NodeHandle",
      // `<virtual-list>`'s own element interface (index.d.ts). Same family and
      // same reason as the three above: without the mapping a
      // `VirtualListElement | null` field is not a node at all, and its
      // `rowHeight`/`scrollTop` reads fall to the dynamic path.
      VirtualListElement: "gea::embedded::ui::NodeHandle",
      // The result of `fetch(...)` flows as the concrete C++ struct (which carries
      // `ok`/`status`/`body` data members plus `text()`/`bytes()`/`arrayBuffer()`
      // methods), not a `gea_cpp_value` record — so `await fetch(url).arrayBuffer()`
      // hands the raw body bytes straight to `loadImage` with no boxing.
      FetchResponse: "gea::host::FetchResponse",
      // Device-hosted HTTP server handle (Node-style `http.createServer`). Keeps
      // `const server = http.createServer(...)` a concrete `gea::host::HttpServer`
      // (a thin handle) so `server.listen(...)` / `server.close()` lower to direct
      // native calls instead of boxing into a gea_cpp_value record.
      HttpServer: "gea::host::HttpServer",
      // `loadImage(...)` yields this concrete handle (id + cached width/height/
      // frameCount/isAnimated + play/pause/seek/dispose methods), so decoded
      // images — including a `GeaEmbeddedImage[]` tile cache — flow as native
      // structs with no `gea_cpp_value` boxing. `canvasImageId` reads `.id`.
      GeaEmbeddedImage: "gea::host::GeaEmbeddedImage",
      // `touch.read()` yields this concrete {touching,x,y} struct. Without the
      // native mapping the binding boxes it to a gea_cpp_value whose fields the
      // host struct can't expose, so `sample.touching` reads nullish — the device
      // touch poll then clears every frame and on-screen buttons never register.
      TouchSample: "gea::host::TouchSample",
      GeolocationCoordinates: "gea::host::GeolocationCoordinates",
      GeolocationPosition: "gea::host::GeolocationPosition",
      GeolocationPositionError: "gea::host::GeolocationPositionError",
      PositionOptions: "gea::host::GeolocationOptions",
      MediaStream: "gea::host::MediaStream",
      MediaStreamTrack: "gea::host::MediaStreamTrack",
      MediaRecorder: "gea::host::MediaRecorder",
      MediaRecorderDataAvailableEvent: "gea::host::MediaRecorderDataEvent",
      GeaAudioBlob: "gea::host::GeaAudioBlob",
      HTMLAudioElement: "gea::host::HTMLAudioElement",
      // The three ambient CONSTRUCTOR interfaces (`declare var Audio:
      // AudioConstructor`, etc.) share their instance type's own carrier
      // above: `new Audio(...)` and every `audio.play()` afterward flow
      // through the identical `gea::host::HTMLAudioElement`, so this is the
      // same "one runtime type, many declared names" join the file's other
      // DOM-alias rows already state (see `NODE_HANDLE_RECEIVER_TYPES`'s own
      // comment) — never a second, constructor-only C++ type. See
      // `nativeConstructors` below for the actual `new` spelling.
      AudioConstructor: "gea::host::HTMLAudioElement",
      MediaStreamConstructor: "gea::host::MediaStream",
      MediaRecorderConstructor: "gea::host::MediaRecorder",
      AudioContext: "gea::host::AudioContext",
      AudioDestinationNode: "gea::host::AudioDestinationNode",
      AudioParam: "gea::host::AudioParam",
      OscillatorNode: "gea::host::OscillatorNode",
      AudioBuffer: "gea::host::AudioBuffer",
      AudioBufferSourceNode: "gea::host::AudioBufferSourceNode",
      // A colour already in the board's native pixel (`rgb565()` -> constexpr
      // `pixel::nativeColorValue`). Keep it wrapped as `NativeColor` instead of
      // aliasing it to `native_t`: on RGBA8888 targets `native_t` is uint32_t, the
      // same C++ type as `Uint32Array` authored 0xRRGGBBAA colours. The wrapper
      // preserves the zero-cost native palette path without letting plain JS
      // numbers masquerade as native pixels.
      Rgb565: "gea::framework::graphics::pixel::NativeColor",
      // Raw byte buffers are native vectors, end to end. Without this mapping a
      // `Uint8Array` binding (e.g. `const bytes: Uint8Array = await
      // res.arrayBuffer()`) stores as gea_cpp_value, which boxes EVERY BYTE into a
      // ~292-byte gea_cpp_value at the conversion boundary — a 25 KB PNG tile
      // becomes a ~7 MB contiguous temporary that fragments PSRAM and aborts on
      // device. `FetchResponse.bytes()/arrayBuffer()` and the `image.loadBytes*` /
      // `image.writeFile` host methods already speak std::vector<std::uint8_t>
      // natively, so with this mapping the whole fetch→decode→write path is
      // zero-boxing.
      Uint8Array: "std::vector<std::uint8_t>",
      ArrayBuffer: "std::vector<std::uint8_t>",
      // Real, unboxed handle wrappers already shipped for the device target
      // (packages/host/include/host/websocket.h, rtc.h) -- nativeHandle plus
      // real methods (send/close, addTrack/createOffer/setLocalDescription/...)
      // and mutable on-event property slots. Instance and constructor share
      // one carrier, the same "one runtime type, many declared names" join
      // HTMLAudioElement/AudioConstructor already state above.
      WebSocket: "gea::host::WebSocket",
      WebSocketConstructor: "gea::host::WebSocket",
      RTCPeerConnection: "gea::host::RTCPeerConnection",
      RTCPeerConnectionConstructor: "gea::host::RTCPeerConnection",
      // The INSTANCE interfaces, which are the names an app actually annotates
      // with: `index.d.ts` declares the globals as
      // `const WebSocket: WebSocketConstructor` /
      // `const RTCPeerConnection: RTCPeerConnectionConstructor`, so
      // `WebSocket`/`RTCPeerConnection` above are value names this table --
      // keyed by TYPE name -- never matches, and `WebSocketInstance` /
      // `RTCPeerConnectionInstance` are what a declaration site spells
      // (`shared/Dialer/store.tsx`: `pc: RTCPeerConnectionInstance | null`).
      // Without these rows the constructor call emitted the right host type and
      // the variable holding it derived a synthesized record, so clang rejected
      // `v33 = gea::host::RTCPeerConnection(...)` -- "no known conversion to
      // Ref<gea_record_type_265>" -- on a program with zero refusals.
      WebSocketInstance: "gea::host::WebSocket",
      RTCPeerConnectionInstance: "gea::host::RTCPeerConnection",
      ...CSS_STYLE_DECLARATION_NATIVE_TYPES,
    },
    // `new <AmbientConstructor>(...)` spellings for the three WebAudio/media
    // constructors above — real `gea::host` constructors already shipped for
    // the device target (`packages/host/include/host/audio.h`,
    // `.../media.h`), so this states the call rather than adding any new C++.
    // Keyed by the CONSTRUCTOR interface's own declared name (see
    // `nativeConstructors`'s own field comment, types.ts, for why).
    nativeConstructors: {
      // `new Audio(src?: string): HTMLAudioElement` -> `HTMLAudioElement(const
      // std::string&)` (audio.h). One argument: the checker's own optional
      // `src` still reaches this template as an argument slot, padded to an
      // empty string when a call omits it (the shared `paddedArguments`
      // machinery every other constructor carrier's trailing-optional
      // argument goes through).
      AudioConstructor: "gea::host::HTMLAudioElement({arg0})",
      // `new MediaStream(): MediaStream` -> the default constructor
      // (media.h). No arguments in either the declaration or the C++ type.
      MediaStreamConstructor: "gea::host::MediaStream()",
      // `new MediaRecorder(stream, options?): MediaRecorder` -> the two-
      // argument constructor (media.h's `MediaRecorder(MediaStream, const
      // Options&)` template, `Options` deduced). A call that omits `options`
      // is padded the same way `AudioConstructor` pads `src` -- the templated
      // constructor reads only the members an actually-passed options object
      // states (`media::recorder_path_from_options`/`_mime_from_options`,
      // both SFINAE'd on `requires { options.path/mimeType; }`), so a padded
      // placeholder with neither member compiles and falls to the same
      // default path/mimeType the JS call's own omission means.
      MediaRecorderConstructor: "gea::host::MediaRecorder({arg0}, {arg1})",
      // `new WebSocket(url: string): WebSocket` -> `WebSocket(NativeWebSocketHandle)`
      // wrapping `gea::host::websocket::create_handle(const std::string&)`
      // (websocket.h). One argument, no padding needed -- the declared
      // constructor takes `url` as required.
      WebSocketConstructor: "gea::host::WebSocket(gea::host::websocket::create_handle({arg0}))",
      // `new RTCPeerConnection(config): RTCPeerConnection` -> the templated
      // `gea::host::rtc::create_handle(const Config&)` overload (rtc.h),
      // which accepts the emitted config record's own C++ type directly (its
      // own comment: "for now we ignore the contents and fall back to the
      // no-arg form" -- a pre-existing, already-shipped limitation this
      // binding does not change or paper over).
      RTCPeerConnectionConstructor: "gea::host::RTCPeerConnection(gea::host::rtc::create_handle({arg0}))",
    },
    hostGlobalObjects: {
      navigator: "gea::host::navigator",
      window: "gea::host::window",
      // `localStorage` (a browser global, no import) resolves to the persistent
      // NVS-backed facade rather than the standalone in-memory runtime store. This
      // also makes the `localStorage.length` property read lower to the facade.
      localStorage: "gea::host::Storage",
    },
    // The C++ type of each object named above, read off the host's own headers
    // (`packages/host/include/host/{navigator,window,storage}.h`). See
    // `HostShimDefinitions.hostGlobalObjectTypes` for why the value spelling
    // alone is not enough.
    hostGlobalObjectTypes: {
      // `inline constexpr Navigator navigator{}` -- navigator.h.
      navigator: "gea::host::Navigator",
      // `inline constexpr WindowFacade window{}` -- window.h.
      window: "gea::host::WindowFacade",
      // `inline StorageFacade Storage` -- storage.h. The global is spelled
      // `gea::host::Storage` and its type is `gea::host::StorageFacade`; the
      // two names differ here where they coincide for the other two rows,
      // which is precisely why this table cannot be derived from its sibling.
      localStorage: "gea::host::StorageFacade",
    },
    hostNamespaces: {
      navigator: "gea::host::navigator",
      // The viewport globals. `gea::host::window` is a real facade object
      // (packages/host/include/host/window.h) whose `innerWidth`/`innerHeight`
      // are METHODS, which is why they are listed as accessors below --
      // `navigator`'s siblings are plain fields and are not.
      window: "gea::host::window",
      image: "image",
      // Async tile/image loader (second-core worker: SD read -> HTTP fetch ->
      // decode, drained per frame). See packages/core/host/tile_loader.h.
      tiles: "tiles",
      touch: "touch",
      apps: "apps",
      Apps: "gea::host::apps",
      BLE: "gea::host::navigator.bluetooth",
      bluetooth: "gea::host::navigator.bluetooth",
      Geolocation: "gea::host::navigator.geolocation",
      geolocation: "gea::host::navigator.geolocation",
      WiFi: "gea::host::navigator.wifi",
      wifi: "gea::host::navigator.wifi",
      http: "gea::host::http",
      Accelerometer: "Accelerometer",
      __gea_Accelerometer: "gea::host::Accelerometer",
      Camera: "gea::host::Camera",
      __gea_Camera: "gea::host::Camera",
      Display: "gea::host::Display",
      display: "gea::host::Display",
      Memory: "gea::host::Memory",
      memory: "gea::host::Memory",
      Input: "gea::host::Input",
      Gpio: "gea::host::Gpio",
      Led: "gea::host::Led",
      Clock: "gea::host::Clock",
      Profiler: "gea::host::Profiler",
      Battery: "gea::host::Battery",
      Notify: "gea::host::Notify",
      DeviceControl: "gea::host::DeviceControl",
      nativeBench: "gea::host::native_bench",
      epubArchive: "gea::host::epub_archive",
      deviceInfo: "gea::host::device_info",
      gea3dNative: "gea::host::gea3d",
      __gea_Audio: "audio",
      __gea_Display: "gea::host::Display",
      __gea_Memory: "gea::host::Memory",
      __gea_Input: "gea::host::Input",
      __gea_Gpio: "gea::host::Gpio",
      __gea_Led: "gea::host::Led",
      __gea_Clock: "gea::host::Clock",
      __gea_Profiler: "gea::host::Profiler",
      __gea_Battery: "gea::host::Battery",
      __gea_Notify: "gea::host::Notify",
      __gea_DeviceControl: "gea::host::DeviceControl",
      // `console` is a PATH, not a value. An app that declares its own
      // `declare const console: { log(message: string): void }` (several in
      // `examples/` do, in their `env.d.ts`) gives the global an anonymous
      // object type, so no protocol named `Console` matches it and a compiler
      // with only that declaration to go on materializes a record global --
      // `extern std::shared_ptr<gea_record_type_222> console;`, which nothing
      // defines and which surfaces only at link. Stating the path here is the
      // package answering for its own host, the same way `window` and
      // `navigator` are answered above.
      console: "gea::host::console",
    },
    hostNamespaceIdentities: {
      Display: { moduleSpecifier: "/", exportName: "Display" },
    },
    hostNamespaceMethods: {
      // Free functions in `gea::host::console` (geatsc's `gea_runtime.h`
      // declares them unconditionally, outside any `GEA_HOST_DECLARED` guard,
      // so they are reachable from a gea build too). `warn`/`info`/`debug` are
      // deliberately absent: no runtime function backs them, and a member no
      // row claims is refused by name rather than silently accepted.
      console: {
        log: "gea::host::console::log",
        error: "gea::host::console::error",
      },
      "navigator.mediaDevices": {
        getUserMedia: "gea::host::media::get_user_media_audio",
      },
      nativeBench: {
        loopOverhead: "gea::host::native_bench::loopOverhead",
        arrayRead: "gea::host::native_bench::arrayRead",
        arrayWrite: "gea::host::native_bench::arrayWrite",
        closure: "gea::host::native_bench::closure",
        methodCalls: "gea::host::native_bench::methodCalls",
        mathIntensive: "gea::host::native_bench::mathIntensive",
        modulo: "gea::host::native_bench::modulo",
        factorial: "gea::host::native_bench::factorial",
        fibonacci: "gea::host::native_bench::fibonacci",
        nestedLoops: "gea::host::native_bench::nestedLoops",
        matrixMultiply: "gea::host::native_bench::matrixMultiply",
        mandelbrot: "gea::host::native_bench::mandelbrot",
        binaryTrees: "gea::host::native_bench::binaryTrees",
        primeSieve: "gea::host::native_bench::primeSieve",
        stringConcat: "gea::host::native_bench::stringConcat",
        objectCreate: "gea::host::native_bench::objectCreate",
        jsonStringify: "gea::host::native_bench::jsonStringify",
        jsonParse: "gea::host::native_bench::jsonParse",
        ttfFontReady: "gea::host::native_bench::ttfFontReady",
        ttfFontBytes: "gea::host::native_bench::ttfFontBytes",
        ttfCMalloc64: "gea::host::native_bench::ttfCMalloc64",
        ttfScaleForSize: "gea::host::native_bench::ttfScaleForSize",
        ttfGlyphIndex: "gea::host::native_bench::ttfGlyphIndex",
        ttfGlyphAdvance: "gea::host::native_bench::ttfGlyphAdvance",
        ttfGlyphWidth: "gea::host::native_bench::ttfGlyphWidth",
        ttfGlyphHeight: "gea::host::native_bench::ttfGlyphHeight",
        ttfShapeCount: "gea::host::native_bench::ttfShapeCount",
        ttfShapeChecksum: "gea::host::native_bench::ttfShapeChecksum",
        ttfFlattenGlyph: "gea::host::native_bench::ttfFlattenGlyph",
        ttfRasterEmptyStatic: "gea::host::native_bench::ttfRasterEmptyStatic",
        ttfManualBoxStatic: "gea::host::native_bench::ttfManualBoxStatic",
        ttfSortEdgesStatic: "gea::host::native_bench::ttfSortEdgesStatic",
        ttfScanEdgesStatic: "gea::host::native_bench::ttfScanEdgesStatic",
        ttfRasterBoxSimple: "gea::host::native_bench::ttfRasterBoxSimple",
        ttfRasterBoxStatic: "gea::host::native_bench::ttfRasterBoxStatic",
        ttfRasterGlyphStatic: "gea::host::native_bench::ttfRasterGlyphStatic",
        ttfSpecimenWidth: "gea::host::native_bench::ttfSpecimenWidth",
        ttfSpecimenHeight: "gea::host::native_bench::ttfSpecimenHeight",
        ttfSpecimenReady: "gea::host::native_bench::ttfSpecimenReady",
        ttfSpecimenColumnBits: "gea::host::native_bench::ttfSpecimenColumnBits",
        ttfColdGlyph: "gea::host::native_bench::ttfColdGlyph",
        ttfColdRun: "gea::host::native_bench::ttfColdRun",
        ttfCachedRun: "gea::host::native_bench::ttfCachedRun",
      },
      epubArchive: {
        inflateRaw: "gea::host::epub_archive::inflateRaw",
        parse: "gea::host::epub_archive::parse",
        meta: "gea::host::epub_archive::meta",
        chapter: "gea::host::epub_archive::chapter",
        readTextFile: "gea::host::epub_archive::readTextFile",
        writeTextFile: "gea::host::epub_archive::writeTextFile",
        coverBytes: "gea::host::epub_archive::coverBytes",
        cacheCover: "gea::host::epub_archive::cacheCover",
        thumbnailCover: "gea::host::epub_archive::thumbnailCover",
        imageBytes: "gea::host::epub_archive::imageBytes",
      },
      deviceInfo: {
        deviceId: "gea::host::device_info::deviceId",
        platform: "gea::host::device_info::platform",
        chipModel: "gea::host::device_info::chipModel",
        chipArch: "gea::host::device_info::chipArch",
        chipRevision: "gea::host::device_info::chipRevision",
        cores: "gea::host::device_info::cores",
        cpuMhz: "gea::host::device_info::cpuMhz",
        flashSizeBytes: "gea::host::device_info::flashSizeBytes",
        ramSizeBytes: "gea::host::device_info::ramSizeBytes",
        uniqueId: "gea::host::device_info::uniqueId",
        osVersion: "gea::host::device_info::osVersion",
        resetReason: "gea::host::device_info::resetReason",
        uptimeMs: "gea::host::device_info::uptimeMs",
        dieTemperatureC: "gea::host::device_info::dieTemperatureC",
      },
      gea3dNative: {
        createBuffer: "gea::host::gea3d::createBuffer",
        bufferDataF32: "gea::host::gea3d::bufferDataF32",
        bufferDataU32: "gea::host::gea3d::bufferDataU32",
        beginFrame: "gea::host::gea3d::beginFrame",
        ambientLight: "gea::host::gea3d::ambientLight",
        directionalLight: "gea::host::gea3d::directionalLight",
        drawElements: "gea::host::gea3d::drawElements",
        endFrame: "gea::host::gea3d::endFrame",
        stat: "gea::host::gea3d::stat",
      },
      "navigator.geolocation": {
        hasFix: "gea::host::navigator.geolocation.hasFix",
        currentPosition: "gea::host::navigator.geolocation.currentPosition",
        coords: "gea::host::navigator.geolocation.coords",
        latitude: "gea::host::navigator.geolocation.latitude",
        longitude: "gea::host::navigator.geolocation.longitude",
        accuracy: "gea::host::navigator.geolocation.accuracy",
        getCurrentPosition:
          "gea::host::navigator.geolocation.getCurrentPosition",
        watchPosition: "gea::host::navigator.geolocation.watchPosition",
        clearWatch: "gea::host::navigator.geolocation.clearWatch",
      },
      tiles: {
        request: "gea::host::tiles.request",
        setNetworkAllowed: "gea::host::tiles.setNetworkAllowed",
        purgeQueued: "gea::host::tiles.purgeQueued",
        pruneLevels: "gea::host::tiles.pruneLevels",
        poll: "gea::host::tiles.poll",
        key: "gea::host::tiles.key",
        keyNum: "gea::host::tiles.keyNum",
        imageId: "gea::host::tiles.imageId",
        status: "gea::host::tiles.status",
      },
      image: {
        loadBytes: "gea::host::image.loadBytes",
        loadBytesOpaque: "gea::host::image.loadBytesOpaque",
        loadFile: "gea::host::image.loadFile",
        loadFileOpaque: "gea::host::image.loadFileOpaque",
        loadAssetPath: "gea::host::image.loadAssetPath",
        writeFile: "gea::host::image.writeFile",
        readFile: "gea::host::image.readFile",
        readMapArchive: "gea::host::image.readMapArchive",
        readFileRange: "gea::host::image.readFileRange",
        listFiles: "gea::host::image.listFiles",
        fetchBytes: "gea::host::image.fetchBytes",
        fetchText: "gea::host::image.fetchText",
        draw: "gea::host::image.draw",
        width: "gea::host::image.width",
        height: "gea::host::image.height",
        frameCount: "gea::host::image.frameCount",
        isAnimated: "gea::host::image.isAnimated",
        setPlaying: "gea::host::image.setPlaying",
        seek: "gea::host::image.seek",
        advance: "gea::host::image.advance",
        dispose: "gea::host::image.dispose",
      },
      touch: {
        read: "gea::host::touch.read",
      },
      apps: {
        launch: "gea::host::apps.launch",
      },
      Apps: {
        launch: "gea::host::apps.launch",
      },
      BLE: {
        init: "gea::host::navigator.bluetooth.init",
        enabled: "gea::host::navigator.bluetooth.enabled",
        setEnabled: "gea::host::navigator.bluetooth.setEnabled",
        startAdvertising: "gea::host::navigator.bluetooth.startAdvertising",
        stopAdvertising: "gea::host::navigator.bluetooth.stopAdvertising",
        connected: "gea::host::navigator.bluetooth.connected",
        bound: "gea::host::navigator.bluetooth.bound",
        batteryLevel: "gea::host::navigator.bluetooth.batteryLevel",
        mac: "gea::host::navigator.bluetooth.mac",
        deviceName: "gea::host::navigator.bluetooth.deviceName",
      },
      bluetooth: {
        init: "gea::host::navigator.bluetooth.init",
        enabled: "gea::host::navigator.bluetooth.enabled",
        setEnabled: "gea::host::navigator.bluetooth.setEnabled",
        startAdvertising: "gea::host::navigator.bluetooth.startAdvertising",
        stopAdvertising: "gea::host::navigator.bluetooth.stopAdvertising",
        connected: "gea::host::navigator.bluetooth.connected",
        bound: "gea::host::navigator.bluetooth.bound",
        batteryLevel: "gea::host::navigator.bluetooth.batteryLevel",
        mac: "gea::host::navigator.bluetooth.mac",
        deviceName: "gea::host::navigator.bluetooth.deviceName",
      },
      "navigator.bluetooth": {
        init: "gea::host::navigator.bluetooth.init",
        enabled: "gea::host::navigator.bluetooth.enabled",
        setEnabled: "gea::host::navigator.bluetooth.setEnabled",
        startAdvertising: "gea::host::navigator.bluetooth.startAdvertising",
        stopAdvertising: "gea::host::navigator.bluetooth.stopAdvertising",
        connected: "gea::host::navigator.bluetooth.connected",
        bound: "gea::host::navigator.bluetooth.bound",
        batteryLevel: "gea::host::navigator.bluetooth.batteryLevel",
        mac: "gea::host::navigator.bluetooth.mac",
        deviceName: "gea::host::navigator.bluetooth.deviceName",
      },
      "BLE.keyboard": {
        tap: "gea::host::navigator.bluetooth.keyboard.tap",
        down: "gea::host::navigator.bluetooth.keyboard.down",
        up: "gea::host::navigator.bluetooth.keyboard.up",
      },
      "bluetooth.keyboard": {
        tap: "gea::host::navigator.bluetooth.keyboard.tap",
        down: "gea::host::navigator.bluetooth.keyboard.down",
        up: "gea::host::navigator.bluetooth.keyboard.up",
      },
      "navigator.bluetooth.keyboard": {
        tap: "gea::host::navigator.bluetooth.keyboard.tap",
        down: "gea::host::navigator.bluetooth.keyboard.down",
        up: "gea::host::navigator.bluetooth.keyboard.up",
      },
      "BLE.mouse": {
        move: "gea::host::navigator.bluetooth.mouse.move",
        click: "gea::host::navigator.bluetooth.mouse.click",
      },
      "BLE.midi": {
        enable: "gea::host::navigator.bluetooth.midi.enable",
        bound: "gea::host::navigator.bluetooth.midi.bound",
        send: "gea::host::navigator.bluetooth.midi.send",
        startScan: "gea::host::navigator.bluetooth.midi.startScan",
        stopScan: "gea::host::navigator.bluetooth.midi.stopScan",
        scanning: "gea::host::navigator.bluetooth.midi.scanning",
        scanCount: "gea::host::navigator.bluetooth.midi.scanCount",
        scanNameAt: "gea::host::navigator.bluetooth.midi.scanNameAt",
        connect: "gea::host::navigator.bluetooth.midi.connect",
        disconnect: "gea::host::navigator.bluetooth.midi.disconnect",
      },
      "bluetooth.midi": {
        enable: "gea::host::navigator.bluetooth.midi.enable",
        bound: "gea::host::navigator.bluetooth.midi.bound",
        send: "gea::host::navigator.bluetooth.midi.send",
        startScan: "gea::host::navigator.bluetooth.midi.startScan",
        stopScan: "gea::host::navigator.bluetooth.midi.stopScan",
        scanning: "gea::host::navigator.bluetooth.midi.scanning",
        scanCount: "gea::host::navigator.bluetooth.midi.scanCount",
        scanNameAt: "gea::host::navigator.bluetooth.midi.scanNameAt",
        connect: "gea::host::navigator.bluetooth.midi.connect",
        disconnect: "gea::host::navigator.bluetooth.midi.disconnect",
      },
      "navigator.bluetooth.midi": {
        enable: "gea::host::navigator.bluetooth.midi.enable",
        bound: "gea::host::navigator.bluetooth.midi.bound",
        send: "gea::host::navigator.bluetooth.midi.send",
        startScan: "gea::host::navigator.bluetooth.midi.startScan",
        stopScan: "gea::host::navigator.bluetooth.midi.stopScan",
        scanning: "gea::host::navigator.bluetooth.midi.scanning",
        scanCount: "gea::host::navigator.bluetooth.midi.scanCount",
        scanNameAt: "gea::host::navigator.bluetooth.midi.scanNameAt",
        connect: "gea::host::navigator.bluetooth.midi.connect",
        disconnect: "gea::host::navigator.bluetooth.midi.disconnect",
      },
      "BLE.hidHost": {
        startScan: "gea::host::navigator.bluetooth.hidHost.startScan",
        stopScan: "gea::host::navigator.bluetooth.hidHost.stopScan",
        scanning: "gea::host::navigator.bluetooth.hidHost.scanning",
        scanCount: "gea::host::navigator.bluetooth.hidHost.scanCount",
        scanNameAt: "gea::host::navigator.bluetooth.hidHost.scanNameAt",
        connect: "gea::host::navigator.bluetooth.hidHost.connect",
        disconnect: "gea::host::navigator.bluetooth.hidHost.disconnect",
        bound: "gea::host::navigator.bluetooth.hidHost.bound",
        reportCount: "gea::host::navigator.bluetooth.hidHost.reportCount",
        reportIdAt: "gea::host::navigator.bluetooth.hidHost.reportIdAt",
        reportLenAt: "gea::host::navigator.bluetooth.hidHost.reportLenAt",
        reportByteAt: "gea::host::navigator.bluetooth.hidHost.reportByteAt",
        clearReports: "gea::host::navigator.bluetooth.hidHost.clearReports",
      },
      "bluetooth.hidHost": {
        startScan: "gea::host::navigator.bluetooth.hidHost.startScan",
        stopScan: "gea::host::navigator.bluetooth.hidHost.stopScan",
        scanning: "gea::host::navigator.bluetooth.hidHost.scanning",
        scanCount: "gea::host::navigator.bluetooth.hidHost.scanCount",
        scanNameAt: "gea::host::navigator.bluetooth.hidHost.scanNameAt",
        connect: "gea::host::navigator.bluetooth.hidHost.connect",
        disconnect: "gea::host::navigator.bluetooth.hidHost.disconnect",
        bound: "gea::host::navigator.bluetooth.hidHost.bound",
        reportCount: "gea::host::navigator.bluetooth.hidHost.reportCount",
        reportIdAt: "gea::host::navigator.bluetooth.hidHost.reportIdAt",
        reportLenAt: "gea::host::navigator.bluetooth.hidHost.reportLenAt",
        reportByteAt: "gea::host::navigator.bluetooth.hidHost.reportByteAt",
        clearReports: "gea::host::navigator.bluetooth.hidHost.clearReports",
      },
      "navigator.bluetooth.hidHost": {
        startScan: "gea::host::navigator.bluetooth.hidHost.startScan",
        stopScan: "gea::host::navigator.bluetooth.hidHost.stopScan",
        scanning: "gea::host::navigator.bluetooth.hidHost.scanning",
        scanCount: "gea::host::navigator.bluetooth.hidHost.scanCount",
        scanNameAt: "gea::host::navigator.bluetooth.hidHost.scanNameAt",
        connect: "gea::host::navigator.bluetooth.hidHost.connect",
        disconnect: "gea::host::navigator.bluetooth.hidHost.disconnect",
        bound: "gea::host::navigator.bluetooth.hidHost.bound",
        reportCount: "gea::host::navigator.bluetooth.hidHost.reportCount",
        reportIdAt: "gea::host::navigator.bluetooth.hidHost.reportIdAt",
        reportLenAt: "gea::host::navigator.bluetooth.hidHost.reportLenAt",
        reportByteAt: "gea::host::navigator.bluetooth.hidHost.reportByteAt",
        clearReports: "gea::host::navigator.bluetooth.hidHost.clearReports",
      },
      "BLE.connections": {
        count: "gea::host::navigator.bluetooth.connections.count",
        kindAt: "gea::host::navigator.bluetooth.connections.kindAt",
        nameAt: "gea::host::navigator.bluetooth.connections.nameAt",
      },
      "bluetooth.connections": {
        count: "gea::host::navigator.bluetooth.connections.count",
        kindAt: "gea::host::navigator.bluetooth.connections.kindAt",
        nameAt: "gea::host::navigator.bluetooth.connections.nameAt",
      },
      "navigator.bluetooth.connections": {
        count: "gea::host::navigator.bluetooth.connections.count",
        kindAt: "gea::host::navigator.bluetooth.connections.kindAt",
        nameAt: "gea::host::navigator.bluetooth.connections.nameAt",
      },
      "BLE.config": {
        setDocument: "gea::host::navigator.bluetooth.config.setDocument",
        pendingLength: "gea::host::navigator.bluetooth.config.pendingLength",
        pendingByteAt: "gea::host::navigator.bluetooth.config.pendingByteAt",
        consumePending: "gea::host::navigator.bluetooth.config.consumePending",
        pairing: "gea::host::navigator.bluetooth.config.pairing",
        pairCode: "gea::host::navigator.bluetooth.config.pairCode",
        dismissPairing: "gea::host::navigator.bluetooth.config.dismissPairing",
        pushActivity: "gea::host::navigator.bluetooth.config.pushActivity",
      },
      "bluetooth.config": {
        setDocument: "gea::host::navigator.bluetooth.config.setDocument",
        pendingLength: "gea::host::navigator.bluetooth.config.pendingLength",
        pendingByteAt: "gea::host::navigator.bluetooth.config.pendingByteAt",
        consumePending: "gea::host::navigator.bluetooth.config.consumePending",
        pairing: "gea::host::navigator.bluetooth.config.pairing",
        pairCode: "gea::host::navigator.bluetooth.config.pairCode",
        dismissPairing: "gea::host::navigator.bluetooth.config.dismissPairing",
        pushActivity: "gea::host::navigator.bluetooth.config.pushActivity",
      },
      "navigator.bluetooth.config": {
        setDocument: "gea::host::navigator.bluetooth.config.setDocument",
        pendingLength: "gea::host::navigator.bluetooth.config.pendingLength",
        pendingByteAt: "gea::host::navigator.bluetooth.config.pendingByteAt",
        consumePending: "gea::host::navigator.bluetooth.config.consumePending",
        pairing: "gea::host::navigator.bluetooth.config.pairing",
        pairCode: "gea::host::navigator.bluetooth.config.pairCode",
        dismissPairing: "gea::host::navigator.bluetooth.config.dismissPairing",
        pushActivity: "gea::host::navigator.bluetooth.config.pushActivity",
      },
      "bluetooth.mouse": {
        move: "gea::host::navigator.bluetooth.mouse.move",
        click: "gea::host::navigator.bluetooth.mouse.click",
      },
      "navigator.bluetooth.mouse": {
        move: "gea::host::navigator.bluetooth.mouse.move",
        click: "gea::host::navigator.bluetooth.mouse.click",
      },
      Geolocation: {
        hasFix: "gea::host::navigator.geolocation.hasFix",
        currentPosition: "gea::host::navigator.geolocation.currentPosition",
        coords: "gea::host::navigator.geolocation.coords",
        latitude: "gea::host::navigator.geolocation.latitude",
        longitude: "gea::host::navigator.geolocation.longitude",
        accuracy: "gea::host::navigator.geolocation.accuracy",
        getCurrentPosition:
          "gea::host::navigator.geolocation.getCurrentPosition",
        watchPosition: "gea::host::navigator.geolocation.watchPosition",
        clearWatch: "gea::host::navigator.geolocation.clearWatch",
      },
      geolocation: {
        hasFix: "gea::host::navigator.geolocation.hasFix",
        currentPosition: "gea::host::navigator.geolocation.currentPosition",
        coords: "gea::host::navigator.geolocation.coords",
        latitude: "gea::host::navigator.geolocation.latitude",
        longitude: "gea::host::navigator.geolocation.longitude",
        accuracy: "gea::host::navigator.geolocation.accuracy",
        getCurrentPosition:
          "gea::host::navigator.geolocation.getCurrentPosition",
        watchPosition: "gea::host::navigator.geolocation.watchPosition",
        clearWatch: "gea::host::navigator.geolocation.clearWatch",
      },
      WiFi: {
        enabled: "gea::host::navigator.wifi.enabled",
        setEnabled: "gea::host::navigator.wifi.setEnabled",
        connected: "gea::host::navigator.wifi.connected",
        isConnected: "gea::host::navigator.wifi.connected",
        rssi: "gea::host::navigator.wifi.rssi",
        getRSSI: "gea::host::navigator.wifi.rssi",
        ssid: "gea::host::navigator.wifi.ssid",
        getSSID: "gea::host::navigator.wifi.ssid",
        ip: "gea::host::navigator.wifi.ip",
        getIP: "gea::host::navigator.wifi.ip",
        mac: "gea::host::navigator.wifi.mac",
        configure: "gea::host::navigator.wifi.configure",
        waitForConnection: "gea::host::navigator.wifi.waitForConnection",
        startScan: "gea::host::navigator.wifi.startScan",
        scanning: "gea::host::navigator.wifi.scanning",
        scanCount: "gea::host::navigator.wifi.scanCount",
        scanSsidAt: "gea::host::navigator.wifi.scanSsidAt",
        scanRssiAt: "gea::host::navigator.wifi.scanRssiAt",
        scanSecuredAt: "gea::host::navigator.wifi.scanSecuredAt",
      },
      wifi: {
        enabled: "gea::host::navigator.wifi.enabled",
        setEnabled: "gea::host::navigator.wifi.setEnabled",
        connected: "gea::host::navigator.wifi.connected",
        isConnected: "gea::host::navigator.wifi.connected",
        rssi: "gea::host::navigator.wifi.rssi",
        getRSSI: "gea::host::navigator.wifi.rssi",
        ssid: "gea::host::navigator.wifi.ssid",
        getSSID: "gea::host::navigator.wifi.ssid",
        ip: "gea::host::navigator.wifi.ip",
        getIP: "gea::host::navigator.wifi.ip",
        mac: "gea::host::navigator.wifi.mac",
        configure: "gea::host::navigator.wifi.configure",
        waitForConnection: "gea::host::navigator.wifi.waitForConnection",
        startScan: "gea::host::navigator.wifi.startScan",
        scanning: "gea::host::navigator.wifi.scanning",
        scanCount: "gea::host::navigator.wifi.scanCount",
        scanSsidAt: "gea::host::navigator.wifi.scanSsidAt",
        scanRssiAt: "gea::host::navigator.wifi.scanRssiAt",
        scanSecuredAt: "gea::host::navigator.wifi.scanSecuredAt",
      },
      "navigator.wifi": {
        enabled: "gea::host::navigator.wifi.enabled",
        setEnabled: "gea::host::navigator.wifi.setEnabled",
        connected: "gea::host::navigator.wifi.connected",
        isConnected: "gea::host::navigator.wifi.connected",
        rssi: "gea::host::navigator.wifi.rssi",
        getRSSI: "gea::host::navigator.wifi.rssi",
        ssid: "gea::host::navigator.wifi.ssid",
        getSSID: "gea::host::navigator.wifi.ssid",
        ip: "gea::host::navigator.wifi.ip",
        getIP: "gea::host::navigator.wifi.ip",
        mac: "gea::host::navigator.wifi.mac",
        configure: "gea::host::navigator.wifi.configure",
        waitForConnection: "gea::host::navigator.wifi.waitForConnection",
        startScan: "gea::host::navigator.wifi.startScan",
        scanning: "gea::host::navigator.wifi.scanning",
        scanCount: "gea::host::navigator.wifi.scanCount",
        scanSsidAt: "gea::host::navigator.wifi.scanSsidAt",
        scanRssiAt: "gea::host::navigator.wifi.scanRssiAt",
        scanSecuredAt: "gea::host::navigator.wifi.scanSecuredAt",
      },
      Accelerometer: {
        start: "gea::host::Accelerometer.start",
        init: "gea::host::Accelerometer.init",
        close: "gea::host::Accelerometer.close",
        calibrateBias: "gea::host::Accelerometer.calibrateBias",
      },
      __gea_Accelerometer: {
        start: "gea::host::Accelerometer.start",
        init: "gea::host::Accelerometer.init",
        close: "gea::host::Accelerometer.close",
        calibrateBias: "gea::host::Accelerometer.calibrateBias",
      },
      Camera: {
        isAvailable: "gea::host::Camera.isAvailable",
        hasPermission: "gea::host::Camera.hasPermission",
        requestPermission: "gea::host::Camera.requestPermission",
        open: "gea::host::Camera.open",
        close: "gea::host::Camera.close",
        isOpen: "gea::host::Camera.isOpen",
        draw: "gea::host::Camera.draw",
        // capture/capturePhoto/stopRecording return typed synchronous promises
        // (CameraPhotoPromise/CameraClipPromise with a native `then`) so the
        // app-facing `.then(photo => …)` chain stays box-free. The raw id/ms
        // forms remain on the __gea_Camera table for the runtime shim's own use.
        capture: "gea::host::Camera.capturePhoto",
        capturePhoto: "gea::host::Camera.capturePhoto",
        captureMirrored: "gea::host::Camera.captureMirrored",
        startRecording: "gea::host::Camera.startRecording",
        stopRecording: "gea::host::Camera.stopRecordingClip",
        isRecording: "gea::host::Camera.isRecording",
        setFlash: "gea::host::Camera.setFlash",
        setZoom: "gea::host::Camera.setZoom",
        setMirror: "gea::host::Camera.setMirror",
        setExposure: "gea::host::Camera.setExposure",
        setWhiteBalance: "gea::host::Camera.setWhiteBalance",
        setFocus: "gea::host::Camera.setFocus",
        setTorch: "gea::host::Camera.setTorch",
        switchCamera: "gea::host::Camera.open",
        deviceIdAt: "gea::host::Camera.deviceIdAt",
        deviceFacingAt: "gea::host::Camera.deviceFacingAt",
      },
      __gea_Camera: {
        isAvailable: "gea::host::Camera.isAvailable",
        hasPermission: "gea::host::Camera.hasPermission",
        requestPermission: "gea::host::Camera.requestPermission",
        open: "gea::host::Camera.open",
        close: "gea::host::Camera.close",
        isOpen: "gea::host::Camera.isOpen",
        draw: "gea::host::Camera.draw",
        capture: "gea::host::Camera.capture",
        captureMirrored: "gea::host::Camera.captureMirrored",
        startRecording: "gea::host::Camera.startRecording",
        stopRecording: "gea::host::Camera.stopRecording",
        isRecording: "gea::host::Camera.isRecording",
        setFlash: "gea::host::Camera.setFlash",
        setZoom: "gea::host::Camera.setZoom",
        setMirror: "gea::host::Camera.setMirror",
        setExposure: "gea::host::Camera.setExposure",
        setWhiteBalance: "gea::host::Camera.setWhiteBalance",
        setFocus: "gea::host::Camera.setFocus",
        setTorch: "gea::host::Camera.setTorch",
        deviceIdAt: "gea::host::Camera.deviceIdAt",
        deviceFacingAt: "gea::host::Camera.deviceFacingAt",
      },
      __gea_audioContext: {
        createOscillator: "gea::host::audioContext.createOscillator",
        createBufferSource: "gea::host::audioContext.createBufferSource",
        decodeAudioData: "gea::host::audioContext.decodeAudioData",
      },
      audioContext: {
        createOscillator: "gea::host::audioContext.createOscillator",
        createBufferSource: "gea::host::audioContext.createBufferSource",
        decodeAudioData: "gea::host::audioContext.decodeAudioData",
      },
      __gea_Audio: {
        getVolume: "gea::host::Audio.getVolume",
        setVolume: "gea::host::Audio.setVolume",
      },
      Display: {
        getBrightness: "gea::host::Display.getBrightness",
        setBrightness: "gea::host::Display.setBrightness",
        getDevicePixelRatio: "gea::host::Display.getDevicePixelRatio",
        setDevicePixelRatio: "gea::host::Display.setDevicePixelRatio",
        getFrameIntervalMs: "gea::host::Display.getFrameIntervalMs",
        setFrameIntervalMs: "gea::host::Display.setFrameIntervalMs",
        getFrameRate: "gea::host::Display.getFrameRate",
        setFrameRate: "gea::host::Display.setFrameRate",
        getOrientation: "gea::host::Display.getOrientation",
        setOrientation: "gea::host::Display.setOrientation",
        getSupportedOrientations: "gea::host::Display.getSupportedOrientations",
        setSupportedOrientations: "gea::host::Display.setSupportedOrientations",
        getAutoRotate: "gea::host::Display.getAutoRotate",
        setAutoRotate: "gea::host::Display.setAutoRotate",
        setVSync: "gea::host::Display.setVSync",
        setTextRasterCache: "gea::host::Display.setTextRasterCache",
        setTextSolidBackdrop: "gea::host::Display.setTextSolidBackdrop",
        getPixelFormat: "gea::host::Display.getPixelFormat",
        setPixelFormat: "gea::host::Display.setPixelFormat",
        getPanelPixelFormat: "gea::host::Display.getPanelPixelFormat",
        getSupportedPixelFormats: "gea::host::Display.getSupportedPixelFormats",
        setAA: "gea::host::Display.setAA",
        setFlushConfig: "gea::host::Display.setFlushConfig",
        setMemoryConfig: "gea::host::Display.setMemoryConfig",
        setEpaperRefreshConfig: "gea::host::Display.setEpaperRefreshConfig",
        epaperFullRefresh: "gea::host::Display.epaperFullRefresh",
      },
      display: {
        getBrightness: "gea::host::Display.getBrightness",
        setBrightness: "gea::host::Display.setBrightness",
        getDevicePixelRatio: "gea::host::Display.getDevicePixelRatio",
        setDevicePixelRatio: "gea::host::Display.setDevicePixelRatio",
        getFrameIntervalMs: "gea::host::Display.getFrameIntervalMs",
        setFrameIntervalMs: "gea::host::Display.setFrameIntervalMs",
        getFrameRate: "gea::host::Display.getFrameRate",
        setFrameRate: "gea::host::Display.setFrameRate",
        getOrientation: "gea::host::Display.getOrientation",
        setOrientation: "gea::host::Display.setOrientation",
        getSupportedOrientations: "gea::host::Display.getSupportedOrientations",
        setSupportedOrientations: "gea::host::Display.setSupportedOrientations",
        getAutoRotate: "gea::host::Display.getAutoRotate",
        setAutoRotate: "gea::host::Display.setAutoRotate",
        setVSync: "gea::host::Display.setVSync",
        setTextRasterCache: "gea::host::Display.setTextRasterCache",
        setTextSolidBackdrop: "gea::host::Display.setTextSolidBackdrop",
        getPixelFormat: "gea::host::Display.getPixelFormat",
        setPixelFormat: "gea::host::Display.setPixelFormat",
        getPanelPixelFormat: "gea::host::Display.getPanelPixelFormat",
        getSupportedPixelFormats: "gea::host::Display.getSupportedPixelFormats",
        setAA: "gea::host::Display.setAA",
        setFlushConfig: "gea::host::Display.setFlushConfig",
        setMemoryConfig: "gea::host::Display.setMemoryConfig",
        setEpaperRefreshConfig: "gea::host::Display.setEpaperRefreshConfig",
        epaperFullRefresh: "gea::host::Display.epaperFullRefresh",
      },
      __gea_Display: {
        getBrightness: "gea::host::Display.getBrightness",
        setBrightness: "gea::host::Display.setBrightness",
        getDevicePixelRatio: "gea::host::Display.getDevicePixelRatio",
        setDevicePixelRatio: "gea::host::Display.setDevicePixelRatio",
        getFrameIntervalMs: "gea::host::Display.getFrameIntervalMs",
        setFrameIntervalMs: "gea::host::Display.setFrameIntervalMs",
        getFrameRate: "gea::host::Display.getFrameRate",
        setFrameRate: "gea::host::Display.setFrameRate",
        getOrientation: "gea::host::Display.getOrientation",
        setOrientation: "gea::host::Display.setOrientation",
        getSupportedOrientations: "gea::host::Display.getSupportedOrientations",
        setSupportedOrientations: "gea::host::Display.setSupportedOrientations",
        getAutoRotate: "gea::host::Display.getAutoRotate",
        setAutoRotate: "gea::host::Display.setAutoRotate",
        setVSync: "gea::host::Display.setVSync",
        setTextRasterCache: "gea::host::Display.setTextRasterCache",
        setTextSolidBackdrop: "gea::host::Display.setTextSolidBackdrop",
        getPixelFormat: "gea::host::Display.getPixelFormat",
        setPixelFormat: "gea::host::Display.setPixelFormat",
        getPanelPixelFormat: "gea::host::Display.getPanelPixelFormat",
        getSupportedPixelFormats: "gea::host::Display.getSupportedPixelFormats",
        setAA: "gea::host::Display.setAA",
        setFlushConfig: "gea::host::Display.setFlushConfig",
        setMemoryConfig: "gea::host::Display.setMemoryConfig",
        setEpaperRefreshConfig: "gea::host::Display.setEpaperRefreshConfig",
        epaperFullRefresh: "gea::host::Display.epaperFullRefresh",
      },
      Memory: {
        internalFree: "gea::host::Memory.internalFree",
        internalLargestFreeBlock: "gea::host::Memory.internalLargestFreeBlock",
        internalMinimumFree: "gea::host::Memory.internalMinimumFree",
        psramFree: "gea::host::Memory.psramFree",
        currentTaskStackHighWaterMark:
          "gea::host::Memory.currentTaskStackHighWaterMark",
        geaMainStackBytes: "gea::host::Memory.geaMainStackBytes",
        geaInitStackBytes: "gea::host::Memory.geaInitStackBytes",
        appFrameStackWords: "gea::host::Memory.appFrameStackWords",
        appFrameStackBytes: "gea::host::Memory.appFrameStackBytes",
        displayFlushConfiguredRows:
          "gea::host::Memory.displayFlushConfiguredRows",
        displayFlushConfiguredDepth:
          "gea::host::Memory.displayFlushConfiguredDepth",
        displayFlushBufferMaxBytes:
          "gea::host::Memory.displayFlushBufferMaxBytes",
        displayFlushRows: "gea::host::Memory.displayFlushRows",
        displayFlushDepth: "gea::host::Memory.displayFlushDepth",
        displayFlushBufferBytes: "gea::host::Memory.displayFlushBufferBytes",
        allocationSramCount: "gea::host::Memory.allocationSramCount",
        allocationPsramCount: "gea::host::Memory.allocationPsramCount",
        allocationSramBytes: "gea::host::Memory.allocationSramBytes",
        allocationPsramBytes: "gea::host::Memory.allocationPsramBytes",
        allocationSramPeakBytes: "gea::host::Memory.allocationSramPeakBytes",
        allocationPsramPeakBytes: "gea::host::Memory.allocationPsramPeakBytes",
      },
      memory: {
        internalFree: "gea::host::Memory.internalFree",
        internalLargestFreeBlock: "gea::host::Memory.internalLargestFreeBlock",
        internalMinimumFree: "gea::host::Memory.internalMinimumFree",
        psramFree: "gea::host::Memory.psramFree",
        currentTaskStackHighWaterMark:
          "gea::host::Memory.currentTaskStackHighWaterMark",
        geaMainStackBytes: "gea::host::Memory.geaMainStackBytes",
        geaInitStackBytes: "gea::host::Memory.geaInitStackBytes",
        appFrameStackWords: "gea::host::Memory.appFrameStackWords",
        appFrameStackBytes: "gea::host::Memory.appFrameStackBytes",
        displayFlushConfiguredRows:
          "gea::host::Memory.displayFlushConfiguredRows",
        displayFlushConfiguredDepth:
          "gea::host::Memory.displayFlushConfiguredDepth",
        displayFlushBufferMaxBytes:
          "gea::host::Memory.displayFlushBufferMaxBytes",
        displayFlushRows: "gea::host::Memory.displayFlushRows",
        displayFlushDepth: "gea::host::Memory.displayFlushDepth",
        displayFlushBufferBytes: "gea::host::Memory.displayFlushBufferBytes",
        allocationSramCount: "gea::host::Memory.allocationSramCount",
        allocationPsramCount: "gea::host::Memory.allocationPsramCount",
        allocationSramBytes: "gea::host::Memory.allocationSramBytes",
        allocationPsramBytes: "gea::host::Memory.allocationPsramBytes",
        allocationSramPeakBytes: "gea::host::Memory.allocationSramPeakBytes",
        allocationPsramPeakBytes: "gea::host::Memory.allocationPsramPeakBytes",
      },
      __gea_Memory: {
        internalFree: "gea::host::Memory.internalFree",
        internalLargestFreeBlock: "gea::host::Memory.internalLargestFreeBlock",
        internalMinimumFree: "gea::host::Memory.internalMinimumFree",
        psramFree: "gea::host::Memory.psramFree",
        currentTaskStackHighWaterMark:
          "gea::host::Memory.currentTaskStackHighWaterMark",
        geaMainStackBytes: "gea::host::Memory.geaMainStackBytes",
        geaInitStackBytes: "gea::host::Memory.geaInitStackBytes",
        appFrameStackWords: "gea::host::Memory.appFrameStackWords",
        appFrameStackBytes: "gea::host::Memory.appFrameStackBytes",
        displayFlushConfiguredRows:
          "gea::host::Memory.displayFlushConfiguredRows",
        displayFlushConfiguredDepth:
          "gea::host::Memory.displayFlushConfiguredDepth",
        displayFlushBufferMaxBytes:
          "gea::host::Memory.displayFlushBufferMaxBytes",
        displayFlushRows: "gea::host::Memory.displayFlushRows",
        displayFlushDepth: "gea::host::Memory.displayFlushDepth",
        displayFlushBufferBytes: "gea::host::Memory.displayFlushBufferBytes",
        allocationSramCount: "gea::host::Memory.allocationSramCount",
        allocationPsramCount: "gea::host::Memory.allocationPsramCount",
        allocationSramBytes: "gea::host::Memory.allocationSramBytes",
        allocationPsramBytes: "gea::host::Memory.allocationPsramBytes",
        allocationSramPeakBytes: "gea::host::Memory.allocationSramPeakBytes",
        allocationPsramPeakBytes: "gea::host::Memory.allocationPsramPeakBytes",
      },
      Input: {
        consumeBackButton: "gea::host::Input.consumeBackButton",
      },
      __gea_Input: {
        consumeBackButton: "gea::host::Input.consumeBackButton",
      },
      Gpio: {
        configureOutput: "gea::host::Gpio.configureOutput",
        configureInput: "gea::host::Gpio.configureInput",
        write: "gea::host::Gpio.write",
        read: "gea::host::Gpio.read",
      },
      __gea_Gpio: {
        configureOutput: "gea::host::Gpio.configureOutput",
        configureInput: "gea::host::Gpio.configureInput",
        write: "gea::host::Gpio.write",
        read: "gea::host::Gpio.read",
      },
      Led: {
        set: "gea::host::Led.set",
        off: "gea::host::Led.off",
        attach: "gea::host::Led.attach",
        setPixel: "gea::host::Led.setPixel",
        show: "gea::host::Led.show",
        detach: "gea::host::Led.detach",
      },
      __gea_Led: {
        set: "gea::host::Led.set",
        off: "gea::host::Led.off",
        attach: "gea::host::Led.attach",
        setPixel: "gea::host::Led.setPixel",
        show: "gea::host::Led.show",
        detach: "gea::host::Led.detach",
      },
      Clock: {
        epochMs: "gea::host::Clock.epochMs",
      },
      __gea_Clock: {
        epochMs: "gea::host::Clock.epochMs",
      },
      Profiler: {
        nowUs: "gea::host::Profiler.nowUs",
        nowCycles: "gea::host::Profiler.nowCycles",
      },
      __gea_Profiler: {
        nowUs: "gea::host::Profiler.nowUs",
        nowCycles: "gea::host::Profiler.nowCycles",
      },
      localStorage: {
        getItem: "gea::host::Storage.getItem",
        setItem: "gea::host::Storage.setItem",
        removeItem: "gea::host::Storage.removeItem",
        clear: "gea::host::Storage.clear",
        key: "gea::host::Storage.key",
      },
      Battery: {
        level: "gea::host::Battery.level",
      },
      __gea_Battery: {
        level: "gea::host::Battery.level",
      },
      Notify: {
        text: "gea::host::Notify.text",
        seq: "gea::host::Notify.seq",
      },
      __gea_Notify: {
        text: "gea::host::Notify.text",
        seq: "gea::host::Notify.seq",
      },
      DeviceControl: {
        exec: "gea::host::DeviceControl.exec",
      },
      __gea_DeviceControl: {
        exec: "gea::host::DeviceControl.exec",
      },
    },
    hostNamespaceNoThrowMethods: {
      navigator: ["bluetooth", "wifi", "geolocation"],
      Display: DISPLAY_NO_THROW_METHODS,
      display: DISPLAY_NO_THROW_METHODS,
      __gea_Display: DISPLAY_NO_THROW_METHODS,
      Memory: MEMORY_NO_THROW_METHODS,
      memory: MEMORY_NO_THROW_METHODS,
      __gea_Memory: MEMORY_NO_THROW_METHODS,
      Input: ["consumeBackButton"],
      __gea_Input: ["consumeBackButton"],
      Gpio: ["configureOutput", "configureInput", "write", "read"],
      __gea_Gpio: ["configureOutput", "configureInput", "write", "read"],
      Led: ["set", "off", "attach", "setPixel", "show", "detach"],
      __gea_Led: ["set", "off", "attach", "setPixel", "show", "detach"],
      Clock: ["epochMs"],
      __gea_Clock: ["epochMs"],
      Profiler: ["nowUs"],
      __gea_Profiler: ["nowUs"],
      Battery: ["level"],
      __gea_Battery: ["level"],
      Notify: ["seq"],
      __gea_Notify: ["seq"],
    },
    hostNamespaceProperties: {
      window: {
        innerWidth: "gea::host::window.innerWidth",
        innerHeight: "gea::host::window.innerHeight",
      },
      navigator: {
        userAgent: "gea::host::navigator.userAgent",
        language: "gea::host::navigator.language",
        platform: "gea::host::navigator.platform",
        onLine: "gea::host::navigator.onLine",
      },
      Display: {
        ctx: "gea::host::Display.ctx",
        width: "gea::host::Display.width",
        height: "gea::host::Display.height",
        nativeWidth: "gea::host::Display.nativeWidth",
        nativeHeight: "gea::host::Display.nativeHeight",
        orientation: "gea::host::Display.orientation",
        supportedOrientations: "gea::host::Display.supportedOrientations",
        autoRotate: "gea::host::Display.autoRotate",
        pixelFormat: "gea::host::Display.pixelFormat",
        panelPixelFormat: "gea::host::Display.panelPixelFormat",
        supportedPixelFormats: "gea::host::Display.supportedPixelFormats",
      },
      display: {
        ctx: "gea::host::Display.ctx",
        width: "gea::host::Display.width",
        height: "gea::host::Display.height",
        nativeWidth: "gea::host::Display.nativeWidth",
        nativeHeight: "gea::host::Display.nativeHeight",
        orientation: "gea::host::Display.orientation",
        supportedOrientations: "gea::host::Display.supportedOrientations",
        autoRotate: "gea::host::Display.autoRotate",
        pixelFormat: "gea::host::Display.pixelFormat",
        panelPixelFormat: "gea::host::Display.panelPixelFormat",
        supportedPixelFormats: "gea::host::Display.supportedPixelFormats",
      },
      __gea_Display: {
        ctx: "gea::host::Display.ctx",
        width: "gea::host::Display.width",
        height: "gea::host::Display.height",
        nativeWidth: "gea::host::Display.nativeWidth",
        nativeHeight: "gea::host::Display.nativeHeight",
        orientation: "gea::host::Display.orientation",
        supportedOrientations: "gea::host::Display.supportedOrientations",
        autoRotate: "gea::host::Display.autoRotate",
        pixelFormat: "gea::host::Display.pixelFormat",
        panelPixelFormat: "gea::host::Display.panelPixelFormat",
        supportedPixelFormats: "gea::host::Display.supportedPixelFormats",
      },
      Accelerometer: {
        tiltX: "gea::host::Accelerometer.tiltX",
        tiltY: "gea::host::Accelerometer.tiltY",
        x: "gea::host::Accelerometer.x",
        y: "gea::host::Accelerometer.y",
        z: "gea::host::Accelerometer.z",
        accelerationX: "gea::host::Accelerometer.accelerationX",
        accelerationY: "gea::host::Accelerometer.accelerationY",
        accelerationZ: "gea::host::Accelerometer.accelerationZ",
        gyroscopeX: "gea::host::Accelerometer.gyroscopeX",
        gyroscopeY: "gea::host::Accelerometer.gyroscopeY",
        gyroscopeZ: "gea::host::Accelerometer.gyroscopeZ",
      },
      __gea_Accelerometer: {
        tiltX: "gea::host::Accelerometer.tiltX",
        tiltY: "gea::host::Accelerometer.tiltY",
        x: "gea::host::Accelerometer.x",
        y: "gea::host::Accelerometer.y",
        z: "gea::host::Accelerometer.z",
        accelerationX: "gea::host::Accelerometer.accelerationX",
        accelerationY: "gea::host::Accelerometer.accelerationY",
        accelerationZ: "gea::host::Accelerometer.accelerationZ",
        gyroscopeX: "gea::host::Accelerometer.gyroscopeX",
        gyroscopeY: "gea::host::Accelerometer.gyroscopeY",
        gyroscopeZ: "gea::host::Accelerometer.gyroscopeZ",
      },
      Camera: {
        width: "gea::host::Camera.width",
        height: "gea::host::Camera.height",
        orientation: "gea::host::Camera.orientation",
        facing: "gea::host::Camera.facing",
        deviceCount: "gea::host::Camera.deviceCount",
      },
      __gea_Camera: {
        width: "gea::host::Camera.width",
        height: "gea::host::Camera.height",
        orientation: "gea::host::Camera.orientation",
        facing: "gea::host::Camera.facing",
        deviceCount: "gea::host::Camera.deviceCount",
      },
    },
    hostNamespacePropertyAccessors: {
      window: ["innerWidth", "innerHeight"],
      Display: [
        "ctx",
        "width",
        "height",
        "nativeWidth",
        "nativeHeight",
        "orientation",
        "supportedOrientations",
        "autoRotate",
        "pixelFormat",
        "panelPixelFormat",
        "supportedPixelFormats",
      ],
      display: [
        "ctx",
        "width",
        "height",
        "nativeWidth",
        "nativeHeight",
        "orientation",
        "supportedOrientations",
        "autoRotate",
        "pixelFormat",
        "panelPixelFormat",
        "supportedPixelFormats",
      ],
      __gea_Display: [
        "ctx",
        "width",
        "height",
        "nativeWidth",
        "nativeHeight",
        "orientation",
        "supportedOrientations",
        "autoRotate",
        "pixelFormat",
        "panelPixelFormat",
        "supportedPixelFormats",
      ],
    },
    hostNamespacePropertySetters: {
      Display: {
        orientation: "gea::host::Display.setOrientation",
        supportedOrientations: "gea::host::Display.setSupportedOrientations",
        autoRotate: "gea::host::Display.setAutoRotate",
        pixelFormat: "gea::host::Display.setPixelFormat",
      },
      display: {
        orientation: "gea::host::Display.setOrientation",
        supportedOrientations: "gea::host::Display.setSupportedOrientations",
        autoRotate: "gea::host::Display.setAutoRotate",
        pixelFormat: "gea::host::Display.setPixelFormat",
      },
      __gea_Display: {
        orientation: "gea::host::Display.setOrientation",
        supportedOrientations: "gea::host::Display.setSupportedOrientations",
        autoRotate: "gea::host::Display.setAutoRotate",
        pixelFormat: "gea::host::Display.setPixelFormat",
      },
    },
    domElementMethods: {
      getContext: "gea_ir::getCanvasContext($receiver, $args)",
    },
    // The tags v1 collapses into a text node when their content is a single
    // text run: `isTextNodeTag` in `cpp-template-renderer.ts`, verbatim. A
    // heading is in the list for the same reason a span is -- it is a leaf that
    // carries characters -- and `<div>`/`<button>` are not, because they are
    // containers whose children are laid out.
    //
    // Why it matters, measured: `<span class="cube-face-label">front</span>`
    // built as a view meant the classed node carried no text and emitted no
    // draw command at all (css-3d-cube), and `<button><span>L</span></button>`
    // put one extra level between the button and its characters, so the app's
    // own pressed-state binding was on a node nothing looked at (sky-hop-jsx).
    elementTextLeafTags: ["span", "p", "h1", "h2", "h3", "h4", "h5", "h6"],
    nativeDocumentMethods: {
      // Typed generic-tag factory (Document::createElement maps known embedded
      // tags to their typed creators, anything else to a tagged view). The
      // NodeHandle return keeps the whole document.createElement path native —
      // a dynamic slot (`let input: any = …`) still boxes at the ASSIGNMENT via
      // the gea_cpp_key(NodeHandle) → nodeValue bridge, so the record protocol
      // (setAttribute/getAttribute/…) stays available there. Without a binding
      // the call falls through to the generic member-call emit, which for a
      // record-typed `document` (an app-side ambient `declare const document:
      // {…}`) produces `document()->createElement(…)` — a compile error on the
      // by-reference host Document.
      // The argument passes through untouched — Document::createElement/createText
      // take both `const char *` and `const std::string &`, covering every form
      // geatsc emits for the argument (no .c_str() adaptation in the template).
      createElement: {
        emit: "gea::embedded::ui::Document::instance().createElement(({arg0}))",
        returnType: "gea::embedded::ui::NodeHandle",
      },
      createTextNode: {
        emit: "gea::embedded::ui::Document::instance().createText(({arg0}))",
        returnType: "gea::embedded::ui::NodeHandle",
      },
      // Like `createElement`/`createText` above, the argument passes through
      // untouched: `Document::getElementById`/`querySelector`/`querySelectorAll`
      // now overload both `const char *` and `const std::string &` (see
      // `packages/engine/ui/document.h`), so no `.c_str()` adaptation belongs in
      // the template. `host-document-calls.ts`'s `callSymbolFromTemplate` only
      // ever plans a template shaped exactly `symbol({arg0}, ...)` — a
      // `.c_str()` suffix (the previous spelling here) is a shape it cannot
      // read, so every typed `document.getElementById(id)` reached lowering
      // with no call plan at all, even though this binding, the runtime method,
      // and the receiver's own native carrier were all in place.
      getElementById: {
        emit: "gea::embedded::ui::Document::instance().getElementById(({arg0}))",
        returnType: "gea::embedded::ui::NodeHandle",
      },
      querySelector: {
        emit: "gea::embedded::ui::Document::instance().querySelector(({arg0}))",
        returnType: "gea::embedded::ui::NodeHandle",
      },
      querySelectorAll: {
        emit: "gea::embedded::ui::Document::instance().querySelectorAll(({arg0}))",
        returnType: "std::vector<gea::embedded::ui::NodeHandle>",
      },
      // The compiled-component runtime builds a fragment for batched list /
      // conditional children. Map it to the native embedded Document so it returns
      // a real NodeHandle (children append, then it appends to the parent) instead
      // of falling through to the boxed gea_cpp_value path -- which produced a
      // 'gea_cpp_value -> NodeHandle' conversion error at `NodeHandle frag = ...`.
      createDocumentFragment: {
        emit: "gea::embedded::ui::Document::instance().createDocumentFragment()",
        returnType: "gea::embedded::ui::NodeHandle",
      },
      // The embedded UI has no comment node, and does not need one: every use of
      // `createComment` in the framework is a SENTINEL — a non-rendering node
      // parked in the tree so a later `insertBefore` has a stable anchor
      // (`conditional-truthy.ts`'s `sentinel`, the keyed-list row anchors). An
      // empty text node is exactly that on this target: it occupies a position,
      // renders nothing, and is a real `NodeHandle`. Without a binding here the
      // call had no plan at all and took its whole function out of the program.
      createComment: {
        emit: "gea::embedded::ui::Document::instance().createText(({arg0}))",
        returnType: "gea::embedded::ui::NodeHandle",
      },
    },
    // DOM `Node` accessor PROPERTIES on a concrete NodeHandle receiver. The gea
    // backend flows DOM nodes as `gea::embedded::ui::NodeHandle` (see nativeTypes
    // above), which carries methods (appendChild/remove/…) but not data members,
    // so geatsc would otherwise emit `node.nodeType` / `node.parentNode` as struct
    // field reads that don't exist. Route them through the gea_ir accessors, which
    // resolve against the retained tree — matching the gea_cpp_value record
    // protocol (nodeValue) used when a node is value-bridged instead of typed.
    nativeMemberPropertyGetters: {
      // `document.body`, the one PROPERTY the document singleton publishes.
      // `Document::body()` (engine/ui/document.h) is a method in C++ and a
      // readonly property in TypeScript (`Document.body: Element`), which is
      // exactly the shape this table exists to bridge -- the document's own
      // METHODS are stated in `nativeDocumentMethods`, and that table has no
      // entry that could ever answer a property read. Without this row a typed
      // `document.body.appendChild(node)` refused at emission ("Document.body
      // is claimed by no host member table"), which is the fail-closed guard
      // correctly naming the missing statement.
      body: [
        {
          emit: "gea::embedded::ui::Document::instance().body()",
          returnType: "gea::embedded::ui::NodeHandle",
          receiverTypes: ["gea::embedded::ui::Document", "Document"],
        },
      ],
      // The event a listener DECLARES. Every field the TS `PointerEvent` /
      // `TouchEvent` / `RotaryEvent` interfaces publish is already a data
      // member of the engine's own event struct (events.h), so each getter is
      // the member read itself -- no bridge, no boxed record. Widened to
      // `double` because the engine stores each as `int` while the declared
      // property is a `number`, exactly as the virtual-list geometry rows
      // below are widened.
      //
      // Without these the event parameter had a carrier (`nativeTypes`) but no
      // member spellings, so emission refused `TouchEvent.clientX` by name --
      // the fail-closed guard reporting, correctly, that this table was the
      // thing missing.
      // `Event.type`: the engine keeps the kind as an enum and names it with
      // `typeName()` (events.cpp), the same string a listener was bound for.
      type: [
        {
          emit: "std::string(({receiver}).typeName())",
          returnType: "std::string",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      pointerId: [
        {
          emit: "static_cast<double>(({receiver}).pointerId)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      // `KeyEvent.keyCode` / `.which` (index.d.ts). One engine field backs both,
      // exactly as the DOM's own two spellings alias one value; `keydown` is
      // filled by `Tree::dispatchDocumentKey` and by the macOS key bridge.
      keyCode: [
        {
          emit: "static_cast<double>(({receiver}).keyCode)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      which: [
        {
          emit: "static_cast<double>(({receiver}).keyCode)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      // The node an event names. Both are real members of the engine's event
      // struct, so the getter is the member read itself -- but it must be
      // STATED, because without a row here the emitter has no spelling for
      // `event.currentTarget` and the whole `onInput` handler falls to the
      // boxed path (or refuses).
      target: [
        {
          emit: "(({receiver}).target)",
          returnType: "gea::framework::events::EventTarget",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      currentTarget: [
        {
          emit: "(({receiver}).currentTarget)",
          returnType: "gea::framework::events::EventTarget",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      x: [
        {
          emit: "static_cast<double>(({receiver}).x)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      y: [
        {
          emit: "static_cast<double>(({receiver}).y)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      clientX: [
        {
          emit: "static_cast<double>(({receiver}).clientX)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      clientY: [
        {
          emit: "static_cast<double>(({receiver}).clientY)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      pageX: [
        {
          emit: "static_cast<double>(({receiver}).pageX)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      pageY: [
        {
          emit: "static_cast<double>(({receiver}).pageY)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      screenX: [
        {
          emit: "static_cast<double>(({receiver}).screenX)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      screenY: [
        {
          emit: "static_cast<double>(({receiver}).screenY)",
          returnType: "double",
          receiverTypes: EVENT_RECEIVER_TYPES,
        },
      ],
      // `RotaryEvent.delta` -- the engine's own field, written by
      // `dispatchDocumentRotary` (`engine/ui/tree_events.cpp`) as
      // `event.delta = delta`. Not `keyCode`: `PointerEvent` carries both, and
      // a rotary tick would have read the key code instead.
      delta: [
        {
          emit: "static_cast<double>(({receiver}).delta)",
          returnType: "double",
          receiverTypes: ["gea::framework::events::PointerEvent", "RotaryEvent"],
        },
      ],
      // `<virtual-list>`'s scroll geometry, read off the engine's own
      // accessors rather than through the dynamic element-property table.
      // `scrollTop`/`scrollLeft` are the standard layout scroll offsets every
      // scrollable node has (`Tree::scrollTop`, tree_internal.h); `rowHeight`
      // is the measured height of the first slot child, which
      // `VirtualListRenderer` caches while computing the virtual content
      // height (internal.h) and which the app windows its recycled slots
      // from. Each is an engine `int`, widened here because the declared
      // property is a `number`.
      rowHeight: [
        {
          emit: geaVirtualListRowHeightEmit,
          returnType: "double",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      scrollTop: [
        {
          emit: "static_cast<double>(gea::embedded::ui::Tree::instance().scrollTop(({receiver}).id()))",
          returnType: "double",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      scrollLeft: [
        {
          emit: "static_cast<double>(gea::embedded::ui::Tree::instance().scrollLeft(({receiver}).id()))",
          returnType: "double",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      classList: [
        {
          emit: "gea::runtime::host::domClassList({receiver})",
          returnType: "gea::embedded::ui::ClassList",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      // `fetch(...)` returns a concrete `gea::host::FetchResponse` whose `ok` /
      // `status` / `statusText` are plain C++ data members (see
      // packages/core/include/host/fetch.h). Without a getter binding a typed
      // `if (!response.ok)` finds no native accessor and falls through to the
      // dynamic `record_get` path, which for a native struct SFINAEs out to
      // `gea_cpp_value::missing()` — a boxed value AND a correctness bug, since
      // `!missing()` is constant-true so the failure branch always runs. Read the
      // fields directly. The `.text()`/`.bytes()` methods are in nativeMemberMethods.
      ok: [
        {
          emit: "({receiver}).ok",
          returnType: "bool",
          receiverTypes: ["gea::host::FetchResponse"],
        },
      ],
      status: [
        {
          emit: "({receiver}).status",
          returnType: "double",
          receiverTypes: ["gea::host::FetchResponse"],
        },
      ],
      statusText: [
        {
          emit: "({receiver}).status_text",
          returnType: "std::string",
          receiverTypes: ["gea::host::FetchResponse"],
        },
      ],
      // `loadImage(...)` / cached tiles flow as a concrete `gea::host::GeaEmbeddedImage`
      // (see packages/host/include/host/image.h): `width`/`height`/`frameCount` are
      // plain C++ `int` DATA MEMBERS and `isAnimated` a `bool` member, while
      // `play()`/`pause()`/`seek()`/`dispose()` are methods (handled by the default
      // native method-call path). Without these getter bindings a typed
      // `image.width` finds no native accessor and the property-read path falls
      // through to a getter-method-call form (`image.width()`), which fails to
      // compile because the field is not callable. Read the fields directly. The
      // type alias `LoadedImage = GeaEmbeddedImage` resolves to `GeaEmbeddedImage`,
      // so the unqualified name covers aliased receivers too.
      width: [
        {
          emit: "({receiver}).width",
          returnType: "double",
          receiverTypes: IMAGE_RECEIVER_TYPES,
        },
      ],
      height: [
        {
          emit: "({receiver}).height",
          returnType: "double",
          receiverTypes: IMAGE_RECEIVER_TYPES,
        },
      ],
      frameCount: [
        {
          emit: "({receiver}).frameCount",
          returnType: "double",
          receiverTypes: IMAGE_RECEIVER_TYPES,
        },
      ],
      isAnimated: [
        {
          emit: "({receiver}).isAnimated",
          returnType: "bool",
          receiverTypes: IMAGE_RECEIVER_TYPES,
        },
      ],
      // Flat `gea_ir::domStyle({receiver})` call, NOT the dot-call `({receiver}).style()`
      // this used to read: `solveHostNativeMemberPropertyRead`
      // (`property-access-plan-solver.ts`) only plans a property-GET whose emit
      // template is exactly `symbol({receiver})` — a dot-call has no template slot
      // at all (see that function's own doc comment, which used to name `.style`
      // as one of the two accessors "declined... nothing regresses, since neither
      // has a plan today either", alongside `.content`). Without a plan here
      // `const style = el.style` (gea's own `reactiveStyle`/`reactiveStyleProp`
      // runtime helpers, packages/gea/src/runtime/reactive-style.ts) failed
      // `missing-property-access-plan` before ever reaching `.setProperty`/
      // `.removeProperty`. `gea_ir::domStyle` (cpp-ir-dom-style.ts) is the flat
      // wrapper `node.style()` needed to make this producer's shape match.
      style: [
        {
          emit: "gea_ir::domStyle({receiver})",
          returnType: "gea::embedded::ui::Style",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      nodeType: [
        {
          emit: "gea_ir::domNodeType({receiver})",
          returnType: "double",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      parentNode: [
        {
          emit: "gea_ir::domParentNode({receiver})",
          returnType: "gea_cpp_value",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      // `node.ownerDocument` -> the (singleton) document, boxed so the caller's
      // capability probe (record_get('createDocumentFragment')) resolves. Without
      // it delegate-click's `root.ownerDocument` hit `gea_cpp_value` with no such
      // member.
      ownerDocument: [
        {
          emit: "gea_ir::domOwnerDocument({receiver})",
          returnType: "gea_cpp_value",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      // Embedded templates retain their parsed children directly on the template
      // node, so `template.content` IS the receiver: the same NodeHandle, not a
      // boxed copy of it. `DocumentFragment` lowers to NodeHandle, so boxing here
      // would hand a statically typed handle to the dynamic carrier for nothing.
      // Keep the row itself: without it geatsc falls back to a missing dynamic
      // property and lowers `template.content.firstChild` to
      // `domFirstChild(missing())`, which makes every multi-node JSX template
      // mount an invalid root.
      content: [
        {
          emit: "({receiver})",
          returnType: "gea::embedded::ui::NodeHandle",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      firstChild: [
        {
          emit: "gea_ir::domFirstChild({receiver})",
          returnType: "gea_cpp_value",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      // `node.nextSibling` -> a typed NodeHandle (not boxed): the compiled-component
      // keyed-list reconciler does `NodeHandle ref = entry.element.nextSibling` then
      // `container.insertBefore(ref, ...)`, so keeping it native avoids a box/unbox
      // round-trip on the reconcile hot path.
      nextSibling: [
        {
          emit: "gea_ir::domNextSibling({receiver})",
          returnType: "gea::embedded::ui::NodeHandle",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      childNodes: [
        {
          emit: "gea_ir::domChildNodes({receiver})",
          returnType: "gea_cpp_value",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      children: [
        {
          emit: "gea_ir::domChildren({receiver})",
          returnType: "std::vector<gea::embedded::ui::NodeHandle>",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      id: [
        {
          emit: "({receiver}).id()",
          returnType: "std::string",
          receiverTypes: [
            "gea::host::MediaStream",
            "MediaStream",
            "gea::host::MediaStreamTrack",
            "MediaStreamTrack",
          ],
        },
      ],
      kind: [
        {
          emit: "({receiver}).kind()",
          returnType: "std::string",
          receiverTypes: ["gea::host::MediaStreamTrack", "MediaStreamTrack"],
        },
      ],
      readyState: [
        {
          emit: "({receiver}).readyState()",
          returnType: "std::string",
          receiverTypes: ["gea::host::MediaStreamTrack", "MediaStreamTrack"],
        },
      ],
      // `pc.connectionState` -- a TS PROPERTY (`readonly connectionState:
      // 'new' | 'connecting' | ...`) and a C++ ACCESSOR (`std::string
      // connectionState() const`, `host/rtc.h`), which is exactly why it is a
      // property-getter row whose template calls: the access itself is a
      // value, and the parentheses belong to the host's spelling of it rather
      // than to the program's syntax.
      //
      // Read-only on both sides, so no `nativeMemberPropertySetters` twin: the
      // engine publishes the state and `enqueue_connection_state_change` is
      // the only thing that writes it. A write is refused by name at the site.
      //
      // Every arm of the declared union is a string literal, so the carrier
      // collapses to `std::string` -- the same type the accessor returns, and
      // what makes `cs === 'connected'` an ordinary string comparison.
      connectionState: [
        {
          emit: "({receiver}).connectionState()",
          returnType: "std::string",
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
        },
      ],
      enabled: [
        {
          emit: "({receiver}).enabled()",
          returnType: "bool",
          receiverTypes: ["gea::host::MediaStreamTrack", "MediaStreamTrack"],
        },
      ],
      state: [
        {
          emit: "static_cast<std::string>(({receiver}).state)",
          returnType: "std::string",
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
        },
      ],
      mimeType: [
        {
          emit: "({receiver}).mimeType()",
          returnType: "std::string",
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
        },
      ],
      ondataavailable: [
        {
          emit: "({receiver}).ondataavailable",
          returnType: "std::function<void(gea::host::MediaRecorderDataEvent)>",
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
        },
      ],
      onstop: [
        {
          emit: "({receiver}).onstop",
          returnType: "std::function<void()>",
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
        },
      ],
      // What the `ondataavailable` handler is HANDED. The event and the blob
      // are both plain engine structs (`MediaRecorderDataEvent { GeaAudioBlob
      // data; }`, `GeaAudioBlob { std::string path; ... }` in `host/media.h`),
      // so each read is the data-member read itself -- no bridge, no boxed
      // record. `nativeTypes` above already gives both declarations a carrier;
      // without these two rows the carrier existed and the member spellings did
      // not, which is the same one-sided statement the recorder's own `stop`
      // and `ondataavailable` write were missing. Keyed by carrier, so the very
      // generic names `data` and `path` answer only for these two structs.
      data: [
        {
          emit: "({receiver}).data",
          returnType: "gea::host::GeaAudioBlob",
          receiverTypes: [
            "gea::host::MediaRecorderDataEvent",
            "MediaRecorderDataAvailableEvent",
          ],
        },
      ],
      path: [
        {
          emit: "({receiver}).path",
          returnType: "std::string",
          receiverTypes: ["gea::host::GeaAudioBlob", "GeaAudioBlob"],
        },
      ],
      currentTime: [
        {
          emit: "static_cast<double>(({receiver}).currentTime)",
          returnType: "double",
          receiverTypes: ["gea::host::AudioContext", "AudioContext"],
        },
      ],
      destination: [
        {
          emit: "static_cast<gea::host::AudioDestinationNode>(({receiver}).destination)",
          returnType: "gea::host::AudioDestinationNode",
          receiverTypes: ["gea::host::AudioContext", "AudioContext"],
        },
      ],
      frequency: [
        {
          emit: "static_cast<gea::host::AudioParam>(({receiver}).frequency)",
          returnType: "gea::host::AudioParam",
          receiverTypes: ["gea::host::OscillatorNode", "OscillatorNode"],
        },
      ],
      value: [
        {
          emit: "static_cast<double>(({receiver}).value)",
          returnType: "double",
          receiverTypes: ["gea::host::AudioParam", "AudioParam"],
        },
        // `InputEvent.currentTarget.value` -- the edited text. Not a data member
        // anywhere: the engine keeps an input's text in the node's `value`
        // ATTRIBUTE (the macOS text bridge writes it in `textDidChange:`, the
        // renderer reads it back to seed the field), and `EventTarget` publishes
        // exactly that accessor. `getAttribute` never answers `nullptr` (it
        // returns `""` for a missing attribute and for an invalid node), so the
        // `std::string` construction is total. This is byte-for-byte the
        // spelling v1 emitted for `event.currentTarget.value`.
        {
          emit: 'std::string(({receiver}).getAttribute("value"))',
          returnType: "std::string",
          receiverTypes: EVENT_TARGET_RECEIVER_TYPES,
        },
        // The same property on a NODE receiver, for `el.value` written against
        // a `Component`'s own element rather than off an event.
        {
          emit: 'std::string(gea::embedded::ui::Tree::instance().getAttribute(({receiver}).id(), "value"))',
          returnType: "std::string",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
        },
      ],
      buffer: [
        {
          emit: "static_cast<gea::host::AudioBuffer>(({receiver}).buffer)",
          returnType: "gea::host::AudioBuffer",
          receiverTypes: [
            "gea::host::AudioBufferSourceNode",
            "AudioBufferSourceNode",
          ],
        },
      ],
    },
    nativeMemberPropertySetters: {
      // Installing an event handler on a WebSocket / RTCPeerConnection.
      //
      // `packages/host/include/host/websocket.h` and `.../rtc.h` already ship
      // the per-handle `CallbackTable`, the platform producers and
      // `runCallbacks()`. Their `on*Property::operator=` overloads, though,
      // take v1's `gea_cpp_value` and are compiled only
      // `#ifdef GEA_CPP_VALUE_AVAILABLE`, so on the unboxed backend
      // `ws.onopen = fn` resolved no overload at all and `examples/dialer`
      // could not compile -- a socket it opened could never call back.
      //
      // These rows route each assignment to the native adapter in the
      // compiler's own runtime (`gea::runtime::hostevent`), which builds the
      // declared event record field by field from the native callback's
      // arguments. The adapters live there rather than on the host classes
      // because they name `gea::Ref`/`gea::Optional`/`gea::CallableObject` --
      // runtime types `gea/embedded-host.h` exists to keep the host package
      // free of.
      onopen: [
        {
          emit: "gea::runtime::hostevent::setWebSocketOnOpen({receiver}, {value})",
          receiverTypes: WEB_SOCKET_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      onmessage: [
        {
          emit: "gea::runtime::hostevent::setWebSocketOnMessage({receiver}, {value})",
          receiverTypes: WEB_SOCKET_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      onclose: [
        {
          emit: "gea::runtime::hostevent::setWebSocketOnClose({receiver}, {value})",
          receiverTypes: WEB_SOCKET_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      onerror: [
        {
          emit: "gea::runtime::hostevent::setWebSocketOnError({receiver}, {value})",
          receiverTypes: WEB_SOCKET_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      onicecandidate: [
        {
          emit: "gea::runtime::hostevent::setRtcOnIceCandidate({receiver}, {value})",
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      onconnectionstatechange: [
        {
          emit: "gea::runtime::hostevent::setRtcOnConnectionStateChange({receiver}, {value})",
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      oniceconnectionstatechange: [
        {
          emit: "gea::runtime::hostevent::setRtcOnIceConnectionStateChange({receiver}, {value})",
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      // The write half of the two scroll offsets above. `setScrollTop` narrows
      // to the engine's `int`, exactly as the dynamic path already does before
      // calling the same setter.
      scrollTop: [
        {
          emit: "gea::embedded::ui::Tree::instance().setScrollTop(({receiver}).id(), static_cast<int>({value}))",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      scrollLeft: [
        {
          emit: "gea::embedded::ui::Tree::instance().setScrollLeft(({receiver}).id(), static_cast<int>({value}))",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      // Generated JSX template factories initialize retained DOM nodes through
      // these two standard properties. Keep the templates in geatsc's exact
      // flat-call setter shape so the sealed property plan can name one symbol
      // and the typed emitter can pass receiver/value without interpreting C++.
      innerHTML: [
        {
          emit: "gea_ir::domSetInnerHtml({receiver}, {value})",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      textContent: [
        {
          emit: "gea_ir::domSetTextContent({receiver}, {value})",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      nodeValue: [
        {
          emit: "gea_ir::domSetTextContent({receiver}, {value})",
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          returnType: "void",
        },
      ],
      display: [
        {
          emit: "({receiver}).display(gea_ir::styleDisplayValue({value}))",
          receiverTypes: ["gea::embedded::ui::Style"],
        },
      ],
      enabled: [
        {
          emit: "({receiver}).setEnabled({value})",
          receiverTypes: ["gea::host::MediaStreamTrack", "MediaStreamTrack"],
        },
      ],
      buffer: [
        {
          emit: "({receiver}).buffer = {value}",
          receiverTypes: [
            "gea::host::AudioBufferSourceNode",
            "AudioBufferSourceNode",
          ],
        },
      ],
      type: [
        {
          emit: "({receiver}).type = {value}",
          receiverTypes: ["gea::host::OscillatorNode", "OscillatorNode"],
        },
      ],
      value: [
        {
          emit: "({receiver}).value = {value}",
          receiverTypes: ["gea::host::AudioParam", "AudioParam"],
        },
      ],
      // The recorder's two callbacks. `MediaRecorder.ondataavailable` and
      // `.onstop` are DECLARED settable (`core/index.d.ts`: `ondataavailable:
      // ((event) => void) | null`) and are plain public `std::function` data
      // members on the engine class (`host/media.h`), so the write is the
      // member assignment itself. The read half was already stated above and
      // the write was not, which is exactly the asymmetry that made
      // `recorder.ondataavailable = handler` refuse by name ("claimed by no
      // host member table as a settable property") while `recorder.onstop`
      // read fine -- one member, one direction stated.
      ondataavailable: [
        {
          emit: "({receiver}).ondataavailable = {value}",
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
          returnType: "void",
        },
      ],
      onstop: [
        {
          emit: "({receiver}).onstop = {value}",
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
          returnType: "void",
        },
      ],
    },
    canvasContextMethods: {
      clear: "gea_ir::canvasClear($receiver /*$args*/)",
      clearRect: "gea_ir::canvasClearRect($receiver, $args)",
      fillRect: "gea_ir::canvasFillRect($receiver, $args)",
      strokeRect: "gea_ir::canvasStrokeRect($receiver, $args)",
      fillCircle: "gea_ir::canvasFillCircle($receiver, $args)",
      strokeCircle: "gea_ir::canvasStrokeCircle($receiver, $args)",
      fillCircleRgb565: "gea_ir::canvasFillCircleRgb565($receiver, $args)",
      fillTriangleRgb565: "gea_ir::canvasFillTriangleRgb565($receiver, $args)",
      fillCirclesRgb565: "gea_ir::canvasFillCirclesRgb565($receiver, $args)",
      fillCirclesRgb565Uniform:
        "gea_ir::canvasFillCirclesRgb565Uniform($receiver, $args)",
      fillTrianglesRgb565Sorted:
        "gea_ir::canvasFillTrianglesRgb565Sorted($receiver, $args)",
      beginPath: "gea_ir::canvasBeginPath($receiver /*$args*/)",
      arc: "gea_ir::canvasArc($receiver, $args)",
      moveTo: "gea_ir::canvasMoveTo($receiver, $args)",
      lineTo: "gea_ir::canvasLineTo($receiver, $args)",
      closePath: "gea_ir::canvasClosePath($receiver /*$args*/)",
      fill: "gea_ir::canvasFill($receiver /*$args*/)",
      stroke: "gea_ir::canvasStroke($receiver /*$args*/)",
      fillText: "gea_ir::canvasFillText($receiver, $args)",
      drawImage: "gea_ir::canvasDrawImage($receiver, $args)",
      measureText: "gea_ir::canvasMeasureText($receiver, $args)",
      measureTextInkCenter:
        "gea_ir::canvasMeasureTextInkCenter($receiver, $args)",
      drawImageCircle: "gea_ir::canvasDrawImageCircle($receiver, $args)",
      drawImageRotated90CW:
        "gea_ir::canvasDrawImageRotated90CW($receiver, $args)",
      drawImageTiledX: "gea_ir::canvasDrawImageTiledX($receiver, $args)",
      flush: "gea_ir::canvasFlush($receiver /*$args*/)",
      beginBatch: "gea_ir::canvasBeginBatch($receiver /*$args*/)",
      endBatch: "gea_ir::canvasEndBatch($receiver /*$args*/)",
    },
    canvasContextNoThrowMethods: [
      "clear",
      "clearRect",
      "fillRect",
      "strokeRect",
      "strokeCircle",
      "fillCircleRgb565",
      "fillTriangleRgb565",
      "fillCirclesRgb565",
      "fillCirclesRgb565Uniform",
      "fillTrianglesRgb565Sorted",
      "beginPath",
      "arc",
      "moveTo",
      "lineTo",
      "closePath",
      "fill",
      "stroke",
      "flush",
      "beginBatch",
      "endBatch",
    ],
    canvasContextPropertySetters: {
      fillStyle: "gea_ir::canvasSetFillStyle($receiver, $value)",
      strokeStyle: "gea_ir::canvasSetStrokeStyle($receiver, $value)",
      globalAlpha: "gea_ir::canvasSetGlobalAlpha($receiver, $value)",
      lineWidth: "gea_ir::canvasSetLineWidth($receiver, $value)",
      font: "gea_ir::canvasSetFont($receiver, $value)",
      textBaseline: "gea_ir::canvasSetTextBaseline($receiver, $value)",
      textAlign: "gea_ir::canvasSetTextAlign($receiver, $value)",
    },
    frameworkProtocols: [
      {
        kind: "runtime-record-layout-v1",
        semanticCategory: "framework-protocol",
        id: "gea.runtime-record-layout",
        version: 1,
        payload: {
          keyedEntry: {
            fields: {
              key: "key",
              item: "item",
              element: "element",
              disposer: "disposer",
              observer: "obs",
            },
            factory: "gea_ir::keyedEntryRecord",
          },
          change: {
            previousValueField: "previousValue",
            newValueField: "newValue",
            factory: "gea_ir::changeRecord",
          },
          arrayItemChange: {
            markerField: "aipu",
            markerValue: true,
            indexField: "arix",
            previousValueField: "previousValue",
            newValueField: "newValue",
            itemDirtyField: "itemDirty",
            factory: "gea_ir::arrayItemChange",
          },
          listChange: {
            typeField: "type",
            startField: "start",
            countField: "count",
            appendType: "append",
            removeType: "remove",
            reorderType: "reorder",
            updateType: "update",
            factory: "gea_ir::listChangeRecord",
          },
        },
      },
      ...HOST_NATIVE_CALLABLE_SIDECARS,
    ],
    // `subscribe.ts`'s local `Observable` interface (`observe(path, handler):
    // () => void`) is duck-typed (`isObservable`) with no `implements` link to
    // any class. `Store` is its one runtime implementer in gea today — this is
    // NOT re-derived or verified by the compiler; it is the same kind of
    // framework-author knowledge `frameworkProtocols` above already
    // declares. Framework and compiler ship together: if gea ever grows a
    // second `Observable` implementer, this fact goes stale and it is this
    // plugin's job to update it, not the compiler's job to catch the drift.
    declaredClosedWorldInterfaceDispatch: [
      {
        interfaceName: "Observable",
        implementingClasses: [
          { moduleSpecifier: "/store.ts", exportName: "Store" },
        ],
      },
    ],
    nullishPropertyFallbackProtocols: [
      {
        propertyName: "id",
        helper: "gea_cpp_default_keyed_list_key",
      },
    ],
    classFactoryHooks: [
      {
        baseClassName: "BLEServer",
        returnedValueStatements: ["bleServers.push_back({value});"],
        defaultValueStatements: ["bleServers.push_back({out});"],
      },
    ],
    hostExternDeclarations: geaHostExternDeclarations,
    embeddedHostFunctions: {
      requestAnimationFrame: "gea::host::requestAnimationFrame",
      // The WHATWG timer globals, from the same engine header and the same
      // translation unit as `requestAnimationFrame` above
      // (`core/packages/host/include/host/timers.h`, implemented in
      // `host/timers.cpp`) -- so a program that links the engine at all
      // already links these.
      //
      // They belong in THIS table for the reason `requestAnimationFrame` does:
      // a host global is a PATH, not a value. Left unstated, `setTimeout` is
      // read as an object and emitted as `extern gea::CallableObject<...>
      // setTimeout;` -- a symbol nothing anywhere defines, which compiles
      // cleanly and fails at `ld` (`examples/weather`: "undefined symbol:
      // setTimeout"). Naming the spelling makes the call
      // `gea::host::setTimeout(cb, ms)`, which is the function the engine
      // actually exports.
      setTimeout: "gea::host::setTimeout",
      clearTimeout: "gea::host::clearTimeout",
      setInterval: "gea::host::setInterval",
      clearInterval: "gea::host::clearInterval",
      // Author a native pixel directly. Lowers to the constexpr
      // nativeColorValue(r,g,b), so literal args fold to a native pixel constant at
      // build time and the result is a NativeColor marker (see the Rgb565
      // nativeType) -- no 0xRRGGBBAA carrier, no per-draw repack.
      rgb565: "gea::framework::graphics::pixel::nativeColorValue",
      fetch: "gea::host::fetch",
      fetchAsync: "gea::host::fetchAsync",
      fetchUploadFileAsync: "gea::host::fetchUploadFileAsync",
      fetchDownloadFileAsync: "gea::host::fetchDownloadFileAsync",
      fetchUploadProgress: "gea::host::fetchUploadProgress",
      fetchUploadSent: "gea::host::fetchUploadSent",
      fetchUploadTotal: "gea::host::fetchUploadTotal",
      fetchReady: "gea::host::fetchReady",
      fetchResult: "gea::host::fetchResult",
      fetchRelease: "gea::host::fetchRelease",
    },
    embeddedHostNoThrowFunctions: [
      "requestAnimationFrame",
      // Same as `requestAnimationFrame`: registering or cancelling a timer
      // schedules work, it does not run the callback, so none of the four has
      // a throwing path of its own.
      "setTimeout",
      "clearTimeout",
      "setInterval",
      "clearInterval",
      "fetchUploadProgress",
      "fetchUploadSent",
      "fetchUploadTotal",
      "fetchReady",
      "fetchRelease",
    ],
    // `fetch(...)` yields the concrete C++ struct so the result binds as
    // `gea::host::FetchResponse response = gea::host::fetch(...)` (typed members +
    // methods), not a boxed `gea_cpp_value` record. Pairs with the `FetchResponse`
    // entry in `nativeTypes` for explicit `FetchResponse` annotations.
    embeddedHostFunctionReturnTypes: {
      // Native-pixel marker. Registered so an inferred `const COLORS =
      // [rgb565(...), ...]` (no annotation) still lowers to a
      // `std::vector<NativeColor>`, and the value flows native through the canvas
      // APIs while staying distinct from authored uint32 colour arrays.
      rgb565: "gea::framework::graphics::pixel::NativeColor",
      fetch: "gea::host::FetchResponse",
      fetchAsync: "double",
      fetchUploadFileAsync: "double",
      fetchDownloadFileAsync: "double",
      fetchUploadProgress: "double",
      fetchUploadSent: "double",
      fetchUploadTotal: "double",
      fetchReady: "bool",
      fetchResult: "gea::host::FetchResponse",
      fetchRelease: "void",
    },
    // `image.make(id)` yields the native `GeaEmbeddedImage` handle. The return type
    // is registered here (not just an emit string) so `loadImage()`'s inferred
    // return — and therefore `await loadImage(...)` and any `GeaEmbeddedImage[]`
    // cache — stays the concrete struct, even though the bundle erased the
    // declared `make(id): GeaEmbeddedImage` type.
    nativeNamespaceMethods: {
      "navigator.mediaDevices": {
        getUserMedia: {
          emit: "gea::host::MediaStream(gea::host::media::get_user_media_audio({args}))",
          returnType: "gea::host::MediaStream",
        },
      },
      Geolocation: {
        currentPosition: {
          emit: "gea::host::navigator.geolocation.currentPosition({args})",
          returnType: "gea::host::GeolocationPosition",
          noThrow: true,
        },
        coords: {
          emit: "gea::host::navigator.geolocation.coords({args})",
          returnType: "gea::host::GeolocationCoordinates",
          noThrow: true,
        },
      },
      geolocation: {
        currentPosition: {
          emit: "gea::host::navigator.geolocation.currentPosition({args})",
          returnType: "gea::host::GeolocationPosition",
          noThrow: true,
        },
        coords: {
          emit: "gea::host::navigator.geolocation.coords({args})",
          returnType: "gea::host::GeolocationCoordinates",
          noThrow: true,
        },
      },
      Battery: {
        level: {
          emit: "gea::host::Battery.level({args})",
          returnType: "double",
          noThrow: true,
        },
      },
      __gea_Battery: {
        level: {
          emit: "gea::host::Battery.level({args})",
          returnType: "double",
          noThrow: true,
        },
      },
      Profiler: {
        nowUs: {
          emit: "gea::host::Profiler.nowUs({args})",
          returnType: "double",
          noThrow: true,
        },
      },
      __gea_Profiler: {
        nowUs: {
          emit: "gea::host::Profiler.nowUs({args})",
          returnType: "double",
          noThrow: true,
        },
      },
      __gea_audioContext: {
        createOscillator: {
          emit: "gea::host::audioContext.createOscillator({args})",
          returnType: "gea::host::OscillatorNode",
        },
        createBufferSource: {
          emit: "gea::host::audioContext.createBufferSource({args})",
          returnType: "gea::host::AudioBufferSourceNode",
        },
        decodeAudioData: {
          emit: "gea::host::audioContext.decodeAudioData({args})",
          returnType: "gea::host::AudioBuffer",
        },
      },
      audioContext: {
        createOscillator: {
          emit: "gea::host::audioContext.createOscillator({args})",
          returnType: "gea::host::OscillatorNode",
        },
        createBufferSource: {
          emit: "gea::host::audioContext.createBufferSource({args})",
          returnType: "gea::host::AudioBufferSourceNode",
        },
        decodeAudioData: {
          emit: "gea::host::audioContext.decodeAudioData({args})",
          returnType: "gea::host::AudioBuffer",
        },
      },
      image: {
        make: {
          emit: "gea::host::image.make({args})",
          returnType: "gea::host::GeaEmbeddedImage",
        },
        // `image.readFile(path)` returns the raw file bytes; register the return
        // type so the binding stores as std::vector<std::uint8_t> (not a boxed
        // per-byte gea_cpp_value record) — same reasoning as FetchResponse.bytes().
        readFile: {
          emit: "gea::host::image.readFile({args})",
          returnType: "std::vector<std::uint8_t>",
        },
        readMapArchive: {
          emit: "gea::host::image.readMapArchive({args})",
          returnType: "std::vector<std::uint8_t>",
        },
        readFileRange: {
          emit: "gea::host::image.readFileRange({args})",
          returnType: "std::vector<std::uint8_t>",
        },
        // `image.listFiles(dir)` returns the newline-joined regular-file names of
        // a directory (e.g. the EPUBs on a microSD).
        listFiles: {
          emit: "gea::host::image.listFiles({args})",
          returnType: "std::string",
        },
        fetchBytes: {
          emit: "gea::host::image.fetchBytes({args})",
          returnType: "std::vector<std::uint8_t>",
        },
        fetchText: {
          emit: "gea::host::image.fetchText({args})",
          returnType: "std::string",
        },
      },
      // `touch.read()` returns the native {touching,x,y} sample as a concrete
      // struct (not a boxed gea_cpp_value record) so device-touch polling reads
      // the real fields — on-screen buttons depend on it.
      touch: {
        read: {
          emit: "gea::host::touch.read({args})",
          returnType: "gea::host::TouchSample",
        },
      },
      // `http.createServer(handler)` yields the native HttpServer handle. The
      // handler arg is a boxed callable (dynamic boundary, like a WebSocket
      // callback); registering the return type keeps `const server = ...` a
      // concrete `gea::host::HttpServer` instead of a gea_cpp_value record.
      http: {
        // create_server returns the numeric handle; wrap it in the HttpServer
        // handle type (its ctors are explicit) so the binding stores native.
        createServer: {
          emit: "gea::host::HttpServer(gea::host::http::create_server({args}))",
          returnType: "gea::host::HttpServer",
        },
        close: {
          emit: "gea::host::http::close_server({args})",
          returnType: "void",
        },
      },
    },
    // Typed returns for methods on native struct receivers. The bundle erases the
    // TS signatures, so without these the storage type of `res.arrayBuffer()` is
    // unknown → `await` wraps it in gea_cpp_await_value and the binding stores as
    // gea_cpp_value — boxing the raw body bytes PER BYTE (a 25 KB tile becomes a
    // ~7 MB temporary; OOM-aborts on device). With the return type registered the
    // await elides and the bytes flow as std::vector<std::uint8_t> end to end.
    // returnType-only bindings (no `emit`) keep the normal direct method-call
    // emit; receiverTypes bound the match to FetchResponse.
    nativeMemberMethods: {
      // `Event.preventDefault()` / `.stopPropagation()` (index.d.ts:269-270),
      // which every event interface inherits. Both are real methods on the
      // engine's own event struct -- `PointerEvent::preventDefault` /
      // `::stopPropagation`, declared in `packages/core/include/events.h:86-87`
      // and used by the engine itself (`ui/virtual_keyboard.cpp:733`) -- so the
      // spelling is the method call, with no bridge and nothing boxed.
      //
      // The declaration was always there; the STATEMENT was not, so emission
      // refused `PointerEvent.preventDefault` by name ("claimed by no host
      // member table"). That is the fail-closed guard reporting this table as
      // the missing thing, exactly as it did for `TouchEvent.clientX` before
      // the getters below it were written.
      preventDefault: [
        {
          receiverTypes: EVENT_RECEIVER_TYPES,
          emit: "({receiver}).preventDefault()",
          returnType: "void",
        },
      ],
      stopPropagation: [
        {
          receiverTypes: EVENT_RECEIVER_TYPES,
          emit: "({receiver}).stopPropagation()",
          returnType: "void",
        },
      ],
      add: [
        {
          receiverTypes: ["gea::embedded::ui::ClassList"],
          emit: "gea::runtime::host::domClassListAdd({receiver}, {arg0})",
          returnType: "void",
        },
      ],
      contains: [
        {
          receiverTypes: ["gea::embedded::ui::ClassList"],
          emit: "gea::runtime::host::domClassListContains({receiver}, {arg0})",
          returnType: "bool",
        },
      ],
      toggle: [
        {
          receiverTypes: ["gea::embedded::ui::ClassList"],
          emit: "gea::runtime::host::domClassListToggle({receiver}, {arg0})",
          returnType: "bool",
        },
        {
          receiverTypes: ["gea::embedded::ui::ClassList"],
          emit: "gea::runtime::host::domClassListToggleForce({receiver}, {arg0}, {arg1})",
          returnType: "bool",
        },
      ],
      // Document values are allowed to flow through aliases (`rt`,
      // `ownerDocument`, conditional selection). This is therefore a typed
      // host-runtime member operation, not a special case for the ambient
      // `document` identifier. The receiver carrier and operation role are
      // authenticated by host-runtime-v1 before this physical symbol is used.
      addEventListener: [
        {
          receiverTypes: ["gea::embedded::ui::Document"],
          emit: "gea::runtime::host::geaDocumentAddEventListener({receiver}, {arg0}, {arg1})",
          returnType: "gea_cpp_value",
          dynamicRecordSchemas: [
            {
              protocolId: "gea.pointer-event-record-v1",
              fields: [{ key: "target", value: "checker-carrier" }],
            },
          ],
          argumentRoles: [
            "dynamic-value",
            {
              kind: "callback",
              parameters: [
                {
                  kind: "dynamic-record",
                  protocolId: "gea.pointer-event-record-v1",
                },
              ],
              result: "void",
              lifetime: "retained",
            },
          ],
        },
        // The same member on a NODE, which is a different receiver carrier and
        // therefore a different physical symbol -- `el.addEventListener('touchstart',
        // handler)` written imperatively on a `Component<GeaCanvasElement>`'s `this.el`,
        // the mirror of the `onClick={...}` a template states declaratively.
        //
        // Registration is the engine's own `NodeHandle::addEventListener`
        // (`packages/engine/ui/node.h`), reached through the compiler runtime's
        // adapter rather than named directly, because two facts about a node
        // listener are not in the call the program wrote:
        //
        //   - the engine dispatches a pointer event to the node the hit test
        //     named and does NOT walk ancestors, so a listener that must fire for
        //     a descendant hit is registered on `body()` behind a
        //     `Tree::containsNode` guard, with `currentTarget`/`eventPhase` set
        //     for the handler's duration -- exactly what `cpp-mounted-lowering.ts`
        //     emits for a mounted element listener (its `mount_*` output puts
        //     every click listener on the body, none on a node);
        //   - `addEventListener` takes `const char *` and a
        //     `gea::framework::events::EventListener`, and the handler the
        //     program passes is a compiler-side callable carrier.
        //
        // Both are properties of the compiler's own carriers, so the adapter is
        // where they belong; this table states WHICH adapter, which is the
        // question a shim table exists to answer. `void`, not the boxed
        // `gea_cpp_value` the `Document` row above returns: nothing about a node
        // listener registration is dynamic, and the declaration says `void`.
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::jsx::detail::addNodeListener({receiver}, {arg0}, {arg1})",
          returnType: "void",
        },
      ],
      appendChild: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea_ir::domAppendChild({receiver}, {arg0})",
        },
      ],
      // `Node.insertBefore(newNode, referenceNode)` — argument order maps
      // straight onto `domInsertBefore(parent, child, reference)`
      // (`compiler/packages/geatsc/src/targets/cpp/runtime/host_document_gea.cpp`,
      // namespace `gea::runtime::host`, fully typed `NodeHandle` params, `void`
      // result — a perfect fit for a receiver-carrying flat symbol call). This
      // is geatsc's OWN runtime, not a `gea_ir`-generated overload: there is no
      // `gea_ir::domInsertBefore` (the doc comment at `cpp-ir.ts:1826-1827`
      // documents only `domNextSibling`'s role in feeding `container.
      // insertBefore(ref, ...)`'s reference argument, not a codegen'd
      // `insertBefore` binding itself), so this entry is qualified with the
      // runtime's real namespace instead of the plugin-generated one.
      insertBefore: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domInsertBefore({receiver}, {arg0}, {arg1})",
          returnType: "void",
        },
      ],
      // `parent.removeChild(child)`. The engine models removal on the CHILD
      // (`NodeHandle::remove()`), so the runtime helper validates the parent and
      // detaches the child; void, matching appendChild/insertBefore above.
      removeChild: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domRemoveChild({receiver}, {arg0})",
          returnType: "void",
        },
      ],
      // `parent.replaceChild(replacement, child)`. Keep both operands on the
      // exact NodeHandle ABI and discard the DOM return because this configured
      // role is consumed only for its tree mutation. The runtime helper
      // rechecks that `child` belongs to `parent` before replacement.
      replaceChild: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domReplaceChild({receiver}, {arg0}, {arg1})",
          returnType: "void",
        },
      ],
      // The DOM patch runtime calls `Element.removeAttribute(name)` through
      // arbitrary NodeHandle aliases. Model it as one authenticated DOM
      // protocol operation, rather than teaching the compiler about the
      // source method name or the statement that happens to invoke it.
      removeAttribute: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domRemoveAttribute({receiver}, {arg0})",
          returnType: "void",
        },
      ],
      hasAttribute: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domHasAttribute({receiver}, {arg0})",
          returnType: "bool",
        },
      ],
      remove: [
        {
          receiverTypes: ["gea::embedded::ui::ClassList"],
          emit: "gea::runtime::host::domClassListRemove({receiver}, {arg0})",
          returnType: "void",
        },
      ],
      setAttribute: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domSetAttribute({receiver}, {arg0}, {arg1})",
          returnType: "void",
        },
      ],
      toggleAttribute: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::runtime::host::domToggleAttribute({receiver}, {arg0}, {arg1})",
          returnType: "void",
        },
      ],
      scrollIntoView: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "({receiver}).scrollIntoView()",
          returnType: "void",
        },
      ],
      // The read half of the attribute pair, whose write half
      // (`setAttribute`/`removeAttribute`) has been stated here all along.
      // `NodeHandle::getAttribute` returns `const char *` and NEVER null -- an
      // absent attribute, an out-of-range node and a null name all answer `""`
      // (engine/ui/tree_events.cpp, `NodeAttributeStore::get` and
      // `Tree::getAttribute`) -- so wrapping it in `std::string` is total, not
      // a guess, and the declared TypeScript return is an ordinary `string`.
      getAttribute: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "std::string(({receiver}).getAttribute({arg0}))",
          returnType: "std::string",
        },
      ],
      // Text-input focus. `NodeHandle::focus()`/`blur()` (engine/ui/node.cpp)
      // set and clear `Tree`'s active input, which is what raises the virtual
      // keyboard and starts the caret blink. Without these rows the only way to
      // reach that state from an app was an untyped receiver, which is a boxed
      // dispatch for a call the engine has a direct method for.
      focus: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "({receiver}).focus()",
          returnType: "void",
        },
      ],
      blur: [
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "({receiver}).blur()",
          returnType: "void",
        },
      ],
      bytes: [
        {
          receiverTypes: ["gea::host::FetchResponse"],
          returnType: "std::vector<std::uint8_t>",
        },
      ],
      listen: [
        {
          receiverTypes: ["gea::host::HttpServer"],
          emit: "({receiver}).listen({args})",
          returnType: "bool",
        },
      ],
      close: [
        {
          receiverTypes: ["gea::host::HttpServer"],
          emit: "({receiver}).close({args})",
          returnType: "void",
        },
        // `ws.close()` / `ws.close(code)` / `ws.close(code, reason)`
        // (index.d.ts `WebSocketInstance.close(code?, reason?)`) against the
        // THREE C++ overloads `packages/host/include/host/websocket.h`
        // declares -- `close()`, `close(double)`, `close(double,
        // const std::string&)`. No fixed arity describes that, and no
        // per-overload row could pick between them: `{args}` passes however
        // many the call wrote and C++'s own overload resolution decides at the
        // call, exactly as `ctx.fillRect`/`ctx.drawImage` already do.
        //
        // The declaration and the class both existed; the STATEMENT did not,
        // so emission refused `WebSocketInstance.close` by name and
        // `examples/dialer` -- whose `hangup()` is nothing but this call and
        // the peer's -- emitted no C++ at all.
        {
          receiverTypes: WEB_SOCKET_RECEIVER_TYPES,
          emit: "({receiver}).close({args})",
          returnType: "void",
        },
        // `pc.close()` -- `void close() const` on the engine class
        // (`packages/host/include/host/rtc.h`). One overload, no arguments,
        // and the TS declaration takes none either.
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "({receiver}).close()",
          returnType: "void",
        },
      ],
      // `ws.send(data)` -- `void send(const std::string &) const`
      // (`host/websocket.h`). The TS declaration is `send(data: string): void`,
      // so both sides agree on one string parameter and the spelling is the
      // method call itself.
      send: [
        {
          receiverTypes: WEB_SOCKET_RECEIVER_TYPES,
          emit: "({receiver}).send({arg0})",
          returnType: "void",
        },
      ],
      // `pc.addTrack(track, stream)` -- `void addTrack(MediaStreamTrack,
      // MediaStream) const` (`host/rtc.h`). The declared parameters are the
      // two host carriers `nativeTypes` already maps (`gea::host::
      // MediaStreamTrack`, `gea::host::MediaStream`), so the arguments cross
      // as themselves and nothing is boxed or converted.
      addTrack: [
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "({receiver}).addTrack({arg0}, {arg1})",
          returnType: "void",
        },
      ],
      // The four members where the DECLARED WebRTC API and the engine's own
      // signature genuinely disagree, routed through the typed adapters in the
      // compiler's runtime (`gea::runtime::hostrtc`, `gea_runtime.h`).
      //
      // The app is written against the standard WebRTC API -- an
      // `RTCSessionDescriptionInit` record in and out, a `Promise` around each
      // call -- and `host/rtc.h` implements a FLATTENED variant of the same
      // operations: `std::string createOffer()` (the SDP alone; the type is
      // implied by which method was called), `setLocalDescription(sdp_type,
      // sdp)`, `setRemoteDescription(sdp_type, sdp)`, `addIceCandidate(
      // candidate, sdp_mid, sdp_mline_index)`. Neither side is wrong: the host
      // deliberately keeps its boundary to strings and doubles, and the
      // program deliberately speaks the standard shape. What was missing is
      // the STATEMENT of the correspondence between them.
      //
      // This is the same joint -- and the same cure -- as the seven `on*`
      // handlers in `nativeMemberPropertySetters` above: the host ships flat
      // callback arguments (`on_ice_candidate(candidate, sdpMid, idx)`) and
      // the declaration names a record, so `gea::runtime::hostevent` builds
      // the record field by field. These are that adapter's argument-side
      // twins, and they live in the compiler's runtime for the identical
      // reason: they name the PROGRAM's own generated record type, which
      // neither the host package (which must not depend on runtime types) nor
      // a fixed `returnType` string here can spell.
      createOffer: [
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "gea::runtime::hostrtc::createOffer({receiver})",
          returnType: "gea::runtime::hostrtc::SessionDescription",
        },
      ],
      createAnswer: [
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "gea::runtime::hostrtc::createAnswer({receiver})",
          returnType: "gea::runtime::hostrtc::SessionDescription",
        },
      ],
      setLocalDescription: [
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "gea::runtime::hostrtc::setLocalDescription({receiver}, {arg0})",
          returnType: "gea::Promise<void>",
        },
      ],
      setRemoteDescription: [
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "gea::runtime::hostrtc::setRemoteDescription({receiver}, {arg0})",
          returnType: "gea::Promise<void>",
        },
      ],
      addIceCandidate: [
        {
          receiverTypes: RTC_PEER_CONNECTION_RECEIVER_TYPES,
          emit: "gea::runtime::hostrtc::addIceCandidate({receiver}, {arg0})",
          returnType: "gea::Promise<void>",
        },
      ],
      id: [
        {
          receiverTypes: ["gea::host::HttpServer"],
          emit: "({receiver}).id({args})",
          returnType: "double",
        },
      ],
      getAudioTracks: [
        {
          receiverTypes: ["gea::host::MediaStream", "MediaStream"],
          returnType: "std::vector<gea::host::MediaStreamTrack>",
        },
      ],
      getTracks: [
        {
          receiverTypes: ["gea::host::MediaStream", "MediaStream"],
          returnType: "std::vector<gea::host::MediaStreamTrack>",
        },
      ],
      play: [
        {
          // A receiver actually typed `HTMLAudioElement` already IS the real
          // `gea::host::HTMLAudioElement` object (an unboxed compiler holds it
          // by value/handle, never behind a raw NodeHandle id), so this row
          // calls straight on `{receiver}` -- the same direct-call convention
          // `AudioContext.createOscillator` etc. use lower in this same file.
          // This is distinct from the NODE_HANDLE_RECEIVER_TYPES row below,
          // which reconstructs the wrapper from a generic DOM node id and is
          // untouched.
          receiverTypes: ["gea::host::HTMLAudioElement", "HTMLAudioElement"],
          emit: "({receiver}).play()",
          returnType: "bool",
        },
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::host::HTMLAudioElement({receiver}).play()",
          returnType: "bool",
        },
        {
          receiverTypes: IMAGE_RECEIVER_TYPES,
          emit: "({receiver}).play()",
          returnType: "void",
        },
      ],
      pause: [
        {
          // See `play` above: a statically HTMLAudioElement-typed receiver
          // calls directly, no reconstruction from a node id.
          receiverTypes: ["gea::host::HTMLAudioElement", "HTMLAudioElement"],
          emit: "({receiver}).pause()",
          returnType: "void",
        },
        {
          receiverTypes: NODE_HANDLE_RECEIVER_TYPES,
          emit: "gea::host::HTMLAudioElement({receiver}).pause()",
          returnType: "void",
        },
        {
          receiverTypes: IMAGE_RECEIVER_TYPES,
          emit: "({receiver}).pause()",
          returnType: "void",
        },
      ],
      // `GeaEmbeddedImage`'s own four methods. The struct
      // (`core/packages/host/include/host/image.h:19-29`) is held by value --
      // `IMAGE_RECEIVER_TYPES` is what the property getters below already read
      // it through -- so each is a direct call on the receiver, exactly like
      // the statically-typed `HTMLAudioElement` rows above. `play`/`pause` are
      // stated as extra rows on the entries already here rather than as a
      // second entry of the same name, which is why they sit alongside rather
      // than below.
      //
      // Without `dispose` an e-reader that frees a decoded page refuses at
      // emission ("GeaEmbeddedImage.dispose is claimed by no host member
      // table"), which is correct of the compiler and a gap in this table: the
      // member exists in the header and in `index.d.ts`, and only the spelling
      // that joins them was missing.
      dispose: [
        {
          receiverTypes: IMAGE_RECEIVER_TYPES,
          emit: "({receiver}).dispose()",
          returnType: "void",
        },
      ],
      seek: [
        {
          receiverTypes: IMAGE_RECEIVER_TYPES,
          emit: "({receiver}).seek({args})",
          returnType: "void",
        },
      ],
      createOscillator: [
        {
          receiverTypes: ["gea::host::AudioContext", "AudioContext"],
          emit: "({receiver}).createOscillator({args})",
          returnType: "gea::host::OscillatorNode",
        },
      ],
      createBufferSource: [
        {
          receiverTypes: ["gea::host::AudioContext", "AudioContext"],
          emit: "({receiver}).createBufferSource({args})",
          returnType: "gea::host::AudioBufferSourceNode",
        },
      ],
      decodeAudioData: [
        {
          receiverTypes: ["gea::host::AudioContext", "AudioContext"],
          emit: "({receiver}).decodeAudioData({args})",
          returnType: "gea::host::AudioBuffer",
        },
      ],
      connect: [
        {
          receiverTypes: [
            "gea::host::AudioBufferSourceNode",
            "AudioBufferSourceNode",
          ],
          emit: "({receiver}).connect({args})",
          returnType: "gea::host::AudioDestinationNode",
        },
        {
          receiverTypes: ["gea::host::OscillatorNode", "OscillatorNode"],
          emit: "({receiver}).connect({args})",
          returnType: "void",
        },
      ],
      start: [
        {
          receiverTypes: [
            "gea::host::AudioBufferSourceNode",
            "AudioBufferSourceNode",
          ],
          emit: "({receiver}).start({args})",
          returnType: "void",
        },
        {
          receiverTypes: ["gea::host::OscillatorNode", "OscillatorNode"],
          emit: "({receiver}).start({args})",
          returnType: "void",
        },
        // `MediaRecorder.start(timeslice?)` -- `void start(double
        // timesliceMs = 0.0) const` on the engine class (`host/media.h`), the
        // optional argument carried by the C++ default rather than by an
        // overload, so `{args}` passes however many the call wrote.
        {
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
          emit: "({receiver}).start({args})",
          returnType: "void",
        },
      ],
      stop: [
        // `MediaStreamTrack.stop()` (index.d.ts:1693) -- the engine's own method
        // on the handle (`packages/host/include/host/media.h:37`). The
        // declaration existed and the STATEMENT did not, so emission refused
        // `dialer` by name on `MediaStreamTrack.stop`. One member name, three
        // receivers: the rows are keyed by receiver carrier, which is what lets
        // the audio nodes below share the spelling without sharing a type.
        {
          receiverTypes: ["gea::host::MediaStreamTrack", "MediaStreamTrack"],
          emit: "({receiver}).stop()",
          returnType: "void",
        },
        {
          receiverTypes: [
            "gea::host::AudioBufferSourceNode",
            "AudioBufferSourceNode",
          ],
          emit: "({receiver}).stop({args})",
          returnType: "void",
        },
        {
          receiverTypes: ["gea::host::OscillatorNode", "OscillatorNode"],
          emit: "({receiver}).stop({args})",
          returnType: "void",
        },
        // `MediaRecorder.stop()` -- `void stop() const` on the engine class
        // (`host/media.h`), the third receiver under this one member name.
        // The recorder's whole method half was missing while its property half
        // (`state`, `mimeType`, the two callbacks) was stated, so
        // `recorder.stop()` refused by name against a class the package
        // otherwise describes in full.
        {
          receiverTypes: ["gea::host::MediaRecorder", "MediaRecorder"],
          emit: "({receiver}).stop()",
          returnType: "void",
        },
      ],
      setValueAtTime: [
        {
          receiverTypes: ["gea::host::AudioParam", "AudioParam"],
          emit: "({receiver}).setValueAtTime({args})",
          returnType: "void",
        },
      ],
      arrayBuffer: [
        {
          receiverTypes: ["gea::host::FetchResponse"],
          returnType: "std::vector<std::uint8_t>",
        },
        {
          receiverTypes: ["gea::host::GeaAudioBlob", "GeaAudioBlob"],
          returnType: "std::vector<std::uint8_t>",
        },
      ],
      text: [
        {
          receiverTypes: ["gea::host::FetchResponse"],
          returnType: "std::string",
        },
        {
          receiverTypes: ["gea::host::GeaAudioBlob", "GeaAudioBlob"],
          returnType: "std::string",
        },
      ],
      ...CSS_STYLE_DECLARATION_NATIVE_MEMBER_METHODS,
    },
    embeddedHostConstants: {
      audioContext: {
        emit: "gea::host::audioContext",
        type: "gea::host::AudioContext",
      },
      __gea_audioContext: {
        emit: "gea::host::audioContext",
        type: "gea::host::AudioContext",
      },
    },
    embeddedHostClasses: {
      AudioContext: {
        wrapper: "gea::host::AudioContext",
        construct: "gea::host::AudioContext({args})",
        allowConcrete: true,
      },
      Audio: {
        wrapper: "gea::host::HTMLAudioElement",
        construct: "gea::host::HTMLAudioElement({args})",
        allowConcrete: true,
      },
      MediaStream: {
        wrapper: "gea::host::MediaStream",
        construct: "gea::host::MediaStream({args})",
        allowConcrete: true,
      },
      MediaRecorder: {
        wrapper: "gea::host::MediaRecorder",
        construct: "gea::host::MediaRecorder({args})",
        allowConcrete: true,
      },
      WebSocket: {
        factory: "gea::host::websocket::create_handle",
        wrapper: "gea::host::WebSocket",
      },
      RTCPeerConnection: {
        factory: "gea::host::rtc::create_handle",
        wrapper: "gea::host::RTCPeerConnection",
      },
    },
  };
}

export const geaHostShims: HostShimDefinitions = createGeaHostShims();
