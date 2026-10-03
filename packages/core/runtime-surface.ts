export * from './runtime/compiler'
export type * from './workers'
export type * from './audio-worklet'
export { flushAudioWorkletOutput } from './runtime/audio-worklet'
export * from './runtime/host'
export * from './runtime/ble'
export * from './runtime/images'
export * from './runtime/color'
export { mount } from './runtime/primitives'
// The embedded/geatsc pipeline aliases '@geastack/core' to runtime.ts, so an
// app's `import type { GeaCanvasElement, GeaEmbeddedImage } from
// '@geastack/core'` resolves HERE, not through the package's index.d.ts.
// Without these re-exports those names silently fail to resolve, the checker
// types the annotated binding `any`, and geatsc lowers it to a boxed
// `gea_cpp_value` instead of the typed native handle (NodeHandle /
// gea::host::GeaEmbeddedImage) the annotation exists to pin.
export type {
  GeaNode,
  GeaElement,
  GeaCanvasElement,
  GeaCameraElement,
  GeaEmbeddedImage,
  FetchResponse,
  TouchSample,
  // Also a lib.dom global. An app annotating a context `CanvasRenderingContext2D`
  // and resolving it here got the DOM one (or nothing), not the framework's --
  // which declares the whole embedded surface (`fillCirclesRgb565`, `flush`,
  // `beginBatch`) that the DOM interface has never heard of.
  CanvasRenderingContext2D,
  Element,
} from './index'

/**
 * Every remaining type `index.d.ts` publishes, re-exported so the name an app
 * imports resolves whichever file the specifier landed on.
 *
 * The package answers `"."` with two files -- `types: ./index.d.ts` for an
 * editor, `default: ./runtime.ts` (which is this module) for anything that
 * compiles the framework from source. A name declared only in the first is a
 * name the second does not have, and an app that imports it there gets
 * `Module '"@geastack/core"' has no exported member 'X'` -- after which the
 * checker types the annotated binding `any` and a compiler lowers it to a
 * boxed dynamic value instead of the native handle the annotation exists to
 * pin. `VirtualListElement` cost `examples/virtual-list` 25 boxed values
 * exactly that way.
 *
 * The block above states the names whose resolution was already known to
 * matter, with the reasoning for each; this states the rest, derived by
 * DIFFERENCE rather than by hand -- every type-only export of `index.d.ts`
 * that this surface did not already carry. Adding them one at a time as each
 * app trips over one is how the gap appeared in the first place.
 *
 * Type-only, deliberately. `index.d.ts` also declares values (`fetchAsync`
 * and its siblings, the two brand constants) that nothing here defines;
 * re-exporting one of those would make a program that calls it compile and
 * then fail to link, which is worse than the honest "no exported member" it
 * gets today.
 */
export type {
  AccelerometerController,
  AccelerometerReading,
  AppsController,
  AudioBuffer,
  AudioBufferSourceNode,
  AudioConstructor,
  AudioContext,
  AudioContextConstructor,
  AudioController,
  AudioDestinationNode,
  AudioParam,
  AudioProps,
  BatteryController,
  BluetoothConfig,
  BluetoothConnections,
  BluetoothController,
  BluetoothHidHost,
  BluetoothKeyboard,
  BluetoothMidi,
  BluetoothMouse,
  ButtonProps,
  CameraController,
  CameraProps,
  CanvasProps,
  ClassMap,
  ClassValue,
  ClockController,
  DOMTokenList,
  DataAttributes,
  DeviceControlController,
  DisplayController,
  DisplayEpaperRefreshConfig,
  DisplayFlushConfig,
  DisplayMemoryConfig,
  DisplayOrientation,
  DisplayOrientationSupport,
  DisplayPixelFormat,
  Document,
  EmbeddedDefaults,
  Event,
  EventTarget,
  FetchHeaders,
  FetchHeadersInit,
  FetchRequestInit,
  GeaAudioBlob,
  GeaEventMap,
  GeaJsxElement,
  GeaJsxElementChildrenAttribute,
  GeaLocalStorage,
  GeaWindow,
  GeolocationController,
  GeolocationCoordinates,
  GeolocationPosition,
  GeolocationPositionError,
  HTMLAudioElement,
  PcmAudioStream,
  PcmAudioStreamConstructor,
  HTMLVideoElement,
  VideoProps,
  HttpModule,
  HttpRequestHandler,
  HttpServer,
  ImageData565,
  ImageProps,
  ImageSource,
  IncomingMessage,
  InputController,
  InputElementProps,
  InputEvent,
  InputEventTarget,
  KeyEvent,
  MediaDevices,
  MediaRecorder,
  MediaRecorderConstructor,
  MediaRecorderDataAvailableEvent,
  MediaRecorderOptions,
  MediaRecorderState,
  MediaStream,
  MediaStreamConstraints,
  MediaStreamConstructor,
  MediaStreamTrack,
  MemoryController,
  NativeButtonProps,
  NativeEventAttributes,
  NativeInputElementProps,
  NativeTextAreaElementProps,
  NativeTouchEventAttributes,
  NativeViewProps,
  NativeVirtualListProps,
  Node,
  NotifyController,
  OscillatorNode,
  OscillatorType,
  PointerEvent,
  PositionOptions,
  PressEvent,
  PressEventArgument,
  PressEventTarget,
  PressHandler,
  ProfilerController,
  RTCConfiguration,
  RTCIceCandidateInit,
  RTCIceServer,
  RTCPeerConnectionConstructor,
  RTCPeerConnectionIceEvent,
  RTCPeerConnectionInstance,
  RTCSessionDescriptionInit,
  RTCTrackEvent,
  RotaryEvent,
  RotaryEventHandler,
  ScrollIntoViewOptions,
  Style,
  StyleBox,
  StyleLength,
  TextAreaElementProps,
  TextProps,
  TouchEvent,
  TouchEventHandler,
  TouchHost,
  TouchPoint,
  ViewProps,
  VirtualListElement,
  WebSocketCloseEvent,
  WebSocketConstructor,
  WebSocketErrorEvent,
  WebSocketInstance,
  WebSocketMessageEvent,
  WiFiController,
} from './index'
