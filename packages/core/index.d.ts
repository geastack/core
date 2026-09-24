/// <reference path="./css.d.ts" />

// Opt-in native 64-bit integer. Structurally a `number` (assign/read without
// casts), but where a binding, parameter, field, or return is annotated `int`,
// geatsc lowers it to a C++ `long long` and emits exact integer arithmetic
// instead of `double` — avoiding software-emulated double math on FPU-less
// targets (ESP32-S3/P4) and the 2^53 double-faithfulness analysis. The caller
// takes responsibility for 64-bit (wrapping) integer semantics. Declared GLOBAL
// (like GeaEmbeddedImage) so it needs no import — a type-only import is dropped
// by the gea source transform, leaving `: int` unresolved (it would box).
declare const __gea_int_brand: unique symbol
declare global {
  type int = number & { readonly [__gea_int_brand]?: never }
}

// Opt-in single-precision float. Structurally a `number` (assign/read without
// casts), but where a binding, parameter, field, or return is annotated `f32`,
// geatsc lowers it to `gea_f32` (a `float`) and computes on the hardware FPU
// instead of software-emulated `double`. ALWAYS single-precision on every
// target (portable, predictable rounding). Branded (`number & {…}`) so the
// checker keeps it distinct — a plain `type f32 = number` loses its alias
// symbol. Declared GLOBAL (like `int`) so it needs no import.
declare const __gea_f32_brand: unique symbol
declare global {
  type f32 = number & { readonly [__gea_f32_brand]?: never }
  type float32 = number & { readonly [__gea_f32_brand]?: never }

  interface ImportMetaEnv {
    readonly [key: string]: string
  }

  interface ImportMeta {
    readonly env: ImportMetaEnv
  }
}

export interface GeaJsxElement {}

export interface GeaJsxElementChildrenAttribute {
  children: {}
}

export interface GeaIntrinsicElements {
  body: NativeViewProps
  canvas: CanvasProps
  camera: CameraProps
  div: NativeViewProps
  span: TextProps
  // The inline-level tags `LayoutNodePass`/`isInlineLevelTag` (engine/ui/layout.cpp)
  // already classifies. They are ordinary view nodes that the layout flows in a
  // row rather than a column; leaving them undeclared did not stop them
  // rendering, it only made every use of one a checker error.
  a: TextProps
  b: TextProps
  i: TextProps
  em: TextProps
  strong: TextProps
  small: TextProps
  label: TextProps
  code: TextProps
  u: TextProps
  sub: TextProps
  sup: TextProps
  mark: TextProps
  p: NativeViewProps
  h1: NativeViewProps
  h2: NativeViewProps
  h3: NativeViewProps
  h4: NativeViewProps
  h5: NativeViewProps
  h6: NativeViewProps
  audio: AudioProps
  button: NativeButtonProps
  img: ImageProps
  input: NativeInputElementProps
  textarea: NativeTextAreaElementProps
  'virtual-list': NativeVirtualListProps
}

export interface GeaWindow {
  readonly innerWidth: number
  readonly innerHeight: number
}

declare global {
  const document: Document
  const window: GeaWindow

  /**
   * The engine's own console (`gea::host::console`), declared here because the
   * host is what backs it.
   *
   * Four example apps carried this same declaration in their own `env.d.ts` /
   * `css.d.ts`, each with the note that the name comes from gea's engine and
   * not from lib.dom -- which is exactly the argument for declaring it once, in
   * the file that speaks for the framework. A per-app copy also gives the global
   * an anonymous object type, so nothing can recognize it as the host path it
   * is, and a compiler with only that to go on emits a record global nothing
   * defines (`geatsc-plugin-gea/src/host-shims.ts`, the `console` row).
   *
   * `log` and `error` only, because those are the two functions the host
   * namespace actually defines. `warn`/`info`/`debug` would compile and then
   * fail to link, which is worse than not being declared.
   */
  interface Console {
    log(message: string): void
    error(message: string): void
  }
  const console: Console

  function requestAnimationFrame(callback: (timestampMs: number) => void): number

  // Native one-shot / repeating timers (gea::host::setTimeout/setInterval,
  // engine/../host/include/host/timers.h), NOT Node's `NodeJS.Timeout` (there is
  // no `dom` or `node` lib here, so without this declaration a `setTimeout`
  // call has no ambient type at all). The native id is a plain numeric handle
  // (`double setTimeout(TimerCallback, double)`), so both the id and the delay
  // are `number`, and the callback takes no arguments (unlike the browser's,
  // which forwards extra `setTimeout` arguments to it).
  function setTimeout(callback: () => void, delayMs: number): number
  function clearTimeout(id: number): void
  function setInterval(callback: () => void, delayMs: number): number
  function clearInterval(id: number): void

  /**
   * A colour already in this board's NATIVE pixel format (RGB565 on the amoled,
   * RGBA8888 on an 8888 target) — what `rgb565()` returns. geatsc lowers it to
   * a tiny native-pixel marker. Unlike the `0xRRGGBBAA` number `rgb()` returns,
   * a `Rgb565` carries no 8-8-8-8 form, so the numeric canvas APIs draw it with
   * ZERO conversion (the colour was born native).
   */
  type Rgb565 = number & { readonly __geaRgb565: unique symbol }

  /**
   * Author a colour directly in this board's native pixel format. An ambient host
   * primitive (like `requestAnimationFrame`) — geatsc lowers it to the `constexpr
   * pixel::nativeColorValue(r,g,b)`, so a literal `rgb565(255,0,0)` folds to a
   * native pixel constant AT BUILD TIME (zero runtime cost) and a runtime call
   * runs the one conversion once at the call site. The result is a `Rgb565`
   * (native pixel), so it flows through `fillCircleRgb565`/`fillCirclesRgb565`/
   * `fillStyle` with no per-draw `0xRRGGBBAA -> native` re-pack. Prefer it over
   * `rgb()` for stored palettes drawn every frame (e.g.
   * `const COLORS: Rgb565[] = [rgb565(...), ...]`).
   */
  function rgb565(r: number, g: number, b: number): Rgb565

  interface CanvasRenderingContext2D {
    drawImage(image: GeaEmbeddedImage, dx: number, dy: number): void
    drawImage(image: GeaEmbeddedImage, dx: number, dy: number, width: number, height: number): void
    // Draw an image masked to a circle inscribed in the destination box. Lets an
    // OPAQUE image (a JPEG, which has no alpha channel) render as a round tile
    // with no alpha plane: the blit skips the pixels outside the shape rather
    // than blending them. A square opaque tile would otherwise paint its
    // corners over whatever it overlaps.
    // Width in px of `text` in the context's CURRENT font. textAlign is a no-op
    // on this canvas and fillText anchors the TOP-LEFT at (x, y), so centring
    // means subtracting half of this.
    measureText(text: string): number
    // Offset from fillText's y anchor to the vertical CENTRE of the text's ink.
    // fillText anchors the top of the line box, which carries ascender and
    // descender the glyphs do not fill, so subtracting half the font size sags.
    measureTextInkCenter(text: string): number
    drawImageCircle(image: GeaEmbeddedImage, dx: number, dy: number, width: number, height: number): void
    drawImageRotated90CW(image: GeaEmbeddedImage, dx: number, dy: number, width: number, height: number): void
    drawImageTiledX(image: GeaEmbeddedImage, dx: number, dy: number, width: number): void
  }

  namespace JSX {
    interface Element extends GeaJsxElement {}
    interface ElementChildrenAttribute extends GeaJsxElementChildrenAttribute {}
    interface IntrinsicElements extends GeaIntrinsicElements {}
  }
}

export type StyleLength =
  | number
  | `${number}px`
  | `${number}%`
  | `${number}vw`
  | `${number}vh`
  | `${number}dvw`
  | `${number}dvh`
  | `calc(${string})`
  | `min(${string})`
  | `max(${string})`
  | `clamp(${string})`
export type StyleBox = StyleLength | string
export type ClassMap = Record<string, string | number | boolean | null | undefined>
export type ClassValue = string | ClassMap

export interface EmbeddedDefaults {
  mirror?: boolean
  wifi?: boolean
}

/** Initialize and advertise every BLEServer registered by the application. */
export function initBleServers(): void
/** Register a fully constructed BLEServer before calling initBleServers(). */
export function registerBleServer(server: BLEServer): void

export declare const defaults: EmbeddedDefaults

export interface TouchSample {
  touching: boolean
  x: number
  y: number
}

export interface TouchHost {
  read(): TouchSample
}

export declare const touch: TouchHost

export interface Style {
  display?: 'block' | 'flex' | 'grid' | 'none'
  flexDirection?: 'row' | 'column'
  flexWrap?: 'nowrap' | 'wrap'
  justifyContent?: 'flex-start' | 'center' | 'flex-end' | 'space-between' | 'space-around'
  alignItems?: 'flex-start' | 'center' | 'flex-end' | 'stretch'
  alignSelf?: 'auto' | 'flex-start' | 'center' | 'flex-end' | 'stretch'
  gap?: StyleLength
  width?: StyleLength
  height?: StyleLength
  minWidth?: StyleLength
  minHeight?: StyleLength
  maxWidth?: StyleLength
  maxHeight?: StyleLength
  flex?: number
  padding?: StyleBox
  paddingTop?: StyleLength
  paddingRight?: StyleLength
  paddingBottom?: StyleLength
  paddingLeft?: StyleLength
  margin?: StyleBox
  marginTop?: StyleLength
  marginRight?: StyleLength
  marginBottom?: StyleLength
  marginLeft?: StyleLength
  position?: 'relative' | 'absolute'
  top?: StyleLength
  left?: StyleLength
  right?: StyleLength
  bottom?: StyleLength
  zIndex?: number
  backgroundColor?: string
  color?: string
  opacity?: number
  blinkInterval?: number
  borderWidth?: StyleLength
  borderColor?: string
  borderRadius?: StyleBox
  borderTopLeftRadius?: StyleLength
  borderTopRightRadius?: StyleLength
  borderBottomRightRadius?: StyleLength
  borderBottomLeftRadius?: StyleLength
  fontFamily?: string
  fontSize?: StyleLength
  textAlign?: 'left' | 'center' | 'right'
  overflow?: 'visible' | 'hidden' | 'scroll' | 'auto'
  overflowX?: 'visible' | 'hidden' | 'scroll' | 'auto'
  overflowY?: 'visible' | 'hidden' | 'scroll' | 'auto'
  transform?: string | number
  rotate?: string | number
  scale?: string | number
  translateX?: StyleLength
  translateY?: StyleLength
  transformOrigin?: string
  filter?: string
}

export interface DataAttributes {
  [name: `data-${string}`]: string | number | boolean | undefined
}

export interface Event {
  readonly type: string
  readonly target: EventTarget
  readonly currentTarget: EventTarget
  preventDefault(): void
  stopPropagation(): void
}

export interface EventTarget {
  addEventListener<K extends keyof GeaEventMap>(type: K, listener: (event: GeaEventMap[K]) => void): void
  addEventListener(type: string, listener: (event: Event) => void): void
}

export interface Node extends EventTarget {
  readonly nodeType?: number
  appendChild(child: Node): void
  remove(): void
}

export interface ScrollIntoViewOptions {
  behavior?: 'auto' | 'instant' | 'smooth'
  block?: 'start' | 'center' | 'end' | 'nearest'
  inline?: 'start' | 'center' | 'end' | 'nearest'
}

export interface DOMTokenList {
  add(...tokens: string[]): void
  remove(...tokens: string[]): void
  contains(token: string): boolean
  toggle(token: string, force?: boolean): boolean
}

export interface Element extends Node {
  className?: string
  readonly children: readonly Element[]
  scrollLeft?: number
  scrollTop?: number
  readonly classList: DOMTokenList
  setAttribute(name: string, value: string | number | boolean): void
  // Always a string: the engine answers `""` for an absent attribute, an
  // out-of-range node and a null name alike (`NodeAttributeStore::get` /
  // `Tree::getAttribute`, engine/ui/tree_events.cpp), so there is no absent
  // value for a wider type to carry. Declared `any` it was a dynamic value in
  // every program that read one -- a box for a `const char *`.
  getAttribute(name: string): string
  removeAttribute(name: string): void
  hasAttribute(name: string): boolean
  toggleAttribute(name: string, force?: boolean): boolean
  scrollIntoView(arg?: boolean | ScrollIntoViewOptions): void
  // Text-input focus: `focus()` makes this element the tree's active input,
  // which is what raises the virtual keyboard and starts the caret blink;
  // `blur()` clears it again (and does nothing when this element was not the
  // one focused). Backed by `NodeHandle::focus()`/`blur()` over
  // `Tree::setActiveInput` (engine/ui/node.cpp).
  focus(): void
  blur(): void
}

export interface VirtualListElement extends Element {
  scrollTop: number
  /**
   * Measured row height in CSS px — the rendered layout height of the first
   * slot child (driven entirely by the slot template's CSS). Read this to
   * position/window the recycled slots from one source of truth instead of
   * recomputing an item height in JS and hand-syncing it to the CSS. 0 until
   * the first slot has been laid out.
   */
  readonly rowHeight: number
}

export interface Document {
  readonly body: Element
  getElementById(id: string): Element | null
  querySelector(selector: string): Element | null
  querySelectorAll(selector: string): readonly Element[]
  createElement(tag: 'audio'): HTMLAudioElement
  createElement(tag: 'virtual-list'): VirtualListElement
  createElement(tag: string): Element
  createTextNode(text: string): Node
  createDocumentFragment(): Node
}

export interface PressEventTarget extends Element {
  dataset: Record<string, any>
}

export interface PointerEvent extends Event {
  readonly target: Element
  readonly currentTarget: Element
  // Identifies the finger/contact: the primary pointer is 1, the first extra
  // finger is 2, etc. (web convention, mirrored by the device touch runtime).
  // Multi-touch handlers route gestures by this id.
  readonly pointerId: number
  readonly x: number
  readonly y: number
  readonly clientX: number
  readonly clientY: number
  readonly pageX: number
  readonly pageY: number
  readonly screenX: number
  readonly screenY: number
}

export interface PressEvent extends PointerEvent {
  readonly pressId: number
  readonly pressValue: number
  readonly target: PressEventTarget
  readonly currentTarget: PressEventTarget
}

export type PressEventArgument = number & PressEvent
export type PressHandler = (event: PressEventArgument) => void

export interface TouchPoint {
  readonly identifier: number
  readonly target: Element
  readonly screenX: number
  readonly screenY: number
  readonly clientX: number
  readonly clientY: number
  readonly pageX: number
  readonly pageY: number
}

export interface TouchEvent extends PointerEvent {
  readonly type: 'touchstart' | 'touchmove' | 'touchend'
  readonly touches: readonly TouchPoint[]
  readonly targetTouches: readonly TouchPoint[]
  readonly changedTouches: readonly TouchPoint[]
}

export interface RotaryEvent extends Event {
  readonly type: 'rotary'
  readonly target: Element
  readonly currentTarget: Element
  readonly delta: number
}

/**
 * Every event type `addEventListener` can be given a typed handler for, which
 * is exactly the set the engine's own `eventTypeIndex`
 * (engine/ui/tree_events.cpp) resolves to a listener slot. A type absent here
 * falls to the `(type: string, listener: (event: Event) => void)` overload,
 * where a handler that reads anything more than `Event` no longer typechecks --
 * which is what `sky-hop` hit binding `pointerdown`, and why `maps` had to cast
 * its document to a structural shim to bind pan/zoom at all.
 *
 * The pointer trio is not a web-only spelling: `eventTypeIndex` gives
 * `pointerdown`/`pointermove`/`pointerup` the very slots their `touch*`
 * siblings use, so one handler runs whether a browser raised a pointer event or
 * the device controller synthesized a touch. `NativeTouchEventAttributes`
 * below already says so for the JSX props; this is the same fact for the
 * imperative API.
 *
 * `keydown` is the engine's hardware-button/shortcut event and carries a
 * `KeyEvent`; `input` carries an `InputEvent`. Both are dispatched off-tree by
 * the runtime (`dispatch_key_input`, and `virtual_keyboard.cpp`'s synthetic
 * pair). `keyup` and `pointercancel` are deliberately ABSENT: `eventTypeIndex`
 * returns -1 for both, so `addEventListener` drops them and the handler would
 * never run. Declaring them would promise a callback the engine does not make.
 */
export interface GeaEventMap {
  click: PressEvent
  touchstart: TouchEvent
  touchmove: TouchEvent
  touchend: TouchEvent
  pointerdown: PointerEvent
  pointermove: PointerEvent
  pointerup: PointerEvent
  input: InputEvent
  keydown: KeyEvent
  rotary: RotaryEvent
  scroll: Event
}

export type TouchEventHandler = (event: TouchEvent) => void
export type PointerEventHandler = (event: PointerEvent) => void
export type RotaryEventHandler = (event: RotaryEvent) => void

export interface NativeTouchEventAttributes {
  onTouchStart?: TouchEventHandler
  onTouchEnd?: TouchEventHandler
  onTouchMove?: TouchEventHandler
  // Pointer events are the engine's cross-platform input API, not a web-only
  // spelling: `eventTypeIndex` (engine/ui/tree_events.cpp:64) gives
  // `pointerdown`/`pointermove`/`pointerup` the very slots their `touch*`
  // siblings use, so the same handler runs whether the browser raised a pointer
  // event or the device controller synthesized a touch. Declaring them here is
  // what makes the pair typeable -- `maps` binds all three and had no prop to
  // bind them to, which reads as an app error for something the engine has
  // supported all along.
  onPointerDown?: PointerEventHandler
  onPointerMove?: PointerEventHandler
  onPointerUp?: PointerEventHandler
}

export interface NativeEventAttributes extends NativeTouchEventAttributes {
  onClick?: PressHandler
  onPress?: PressHandler
  onKeyDown?: (event: KeyEvent) => void
  onRotary?: RotaryEventHandler
}

export interface InputEventTarget extends Element {
  value: string
}

export interface InputEvent {
  target: InputEventTarget
  currentTarget: InputEventTarget
}

export interface KeyEvent {
  keyCode: number
  which: number
}

export interface ViewProps extends DataAttributes {
  id?: string
  class?: ClassValue
  style?: Style
  pressId?: number
  pressValue?: number
  onClick?: PressHandler
  onPress?: PressHandler
  children?: any
}

export interface NativeViewProps extends DataAttributes, NativeEventAttributes {
  key?: string | number
  /** Accepted on both targets so a shared component can carry it; the native tree has no accessibility layer and ignores it. */
  role?: string
  id?: string
  class?: ClassValue
  style?: Style
  ref?: GeaElement | null
  pressId?: number
  pressValue?: number
  momentum?: boolean | string
  children?: any
}

/**
 * The props of an inline text element (`span`, `b`, `i`, ...).
 *
 * `Document::createElement` builds every one of these with `createView()` --
 * the same node a `div` gets, differing only in the tag name it stores -- so an
 * inline element accepts exactly what a view accepts: an `id`, `data-*`
 * attributes, event handlers, a `ref`. Declaring a narrower set here did not
 * make those unsupported, it only made them untypeable: `<span id="x"
 * data-anim="fade">` is what the CSS animation system reads, and it was a
 * checker error while working perfectly at runtime.
 */
export interface TextProps extends NativeViewProps {}

export interface GeaNode extends Node {}
export interface GeaElement extends Element {}

export interface GeaCanvasElement extends GeaElement {
  getContext(kind: '2d'): CanvasRenderingContext2D
}

export interface CanvasProps extends DataAttributes, NativeEventAttributes {
  id?: string
  class?: ClassValue
  style?: Style
  width?: number | string
  height?: number | string
  ref?: GeaCanvasElement | null
  children?: any
}

/**
 * A live camera preview surface. It is a real native leaf element — size and
 * position it with CSS exactly like a `<canvas>` (full screen or a small
 * rounded sub-rect). Opens the requested camera on first render and streams
 * each frame into its computed box. Pair it with the imperative `Camera` class
 * (capture, record, exposure/white-balance/focus controls) — both operate on
 * the one active camera.
 */
export interface CameraProps extends DataAttributes {
  class?: ClassValue
  style?: Style
  /** Which camera to open. Defaults to "back". */
  facing?: CameraFacing
  /** Specific device id (overrides `facing`); from `Camera.getDevices()`. */
  device?: string
  /** Capture resolution hint; the closest supported size is chosen. */
  width?: number | string
  height?: number | string
  /** Mirror the preview horizontally (default: true when facing="front"). */
  mirror?: boolean
  /** How the frame fills the element box. Defaults to "cover". */
  fit?: 'cover' | 'contain' | 'fill'
  ref?: GeaCameraElement | null
  children?: any
}

export interface GeaCameraElement extends GeaElement {}

export interface AudioProps extends DataAttributes {
  id?: string
  class?: ClassValue
  style?: Style
  src?: string
  autoplay?: boolean | string | number
  controls?: boolean | string | number
  loop?: boolean | string | number
  ref?: HTMLAudioElement | null
  children?: any
}

export type ImageSource = string | ArrayBuffer | Uint8Array | GeaEmbeddedImage

export interface ImageProps extends DataAttributes {
  id?: string
  class?: ClassValue
  // Optional: a static template can leave this unset and apply the source
  // imperatively via `element.setAttribute('src', path)` after mount (see
  // `Document::createImage` / `ImageElement::create` in engine/ui/image.h,
  // which creates a blank image node with no id until one is assigned) — the
  // e-reader's CoverArt does exactly this to stay natively mounted instead of
  // dropping into a reactive-binding bridge.
  src?: ImageSource
  style?: Style
  ref?: GeaElement | null
  fit?: 'contain' | 'cover' | 'fill' | 'none' | 'scale-down'
  playing?: boolean
  loop?: number | 'infinite'
  onLoad?: () => void
  onError?: (error: string) => void
  onFrame?: (frame: number) => void
}

export interface GeaEmbeddedImage {
  readonly width: number
  readonly height: number
  readonly frameCount: number
  readonly isAnimated: boolean
  play(): void
  pause(): void
  seek(frame: number): void
  dispose(): void
}

export interface LoadImageOptions {
  /**
   * Treat the image as opaque-with-color-key transparency: anti-aliased edges
   * (0 < alpha < 255) are snapped to fully opaque, but pixels with alpha == 0
   * remain transparent. If the source has no alpha == 0 pixels, the alpha
   * buffer is dropped entirely so blits hit a row-memcpy fast path. Use for
   * tile/sprite art where you don't need anti-aliased blending.
   */
  opaque?: boolean
}

// Decoding is synchronous (the bytes are already in memory), so this returns the
// native `GeaEmbeddedImage` handle directly rather than a Promise — `await` still
// works and keeps the value (and any `GeaEmbeddedImage[]` cache) a concrete
// struct instead of a boxed gea_cpp_value.
export declare function loadImage(src: string | ArrayBuffer | Uint8Array, options?: LoadImageOptions): GeaEmbeddedImage
export declare function loadImageWithOpaque(src: Uint8Array, opaque: boolean): GeaEmbeddedImage

// Load + decode an image directly from a persistent-cache file (e.g. a microSD
// tile cache at /sdcard/...). Synchronous; the returned handle has `width === 0`
// when the file is missing or fails to decode, so treat that as a cache miss and
// fetch over the network instead.
export declare function loadImageFile(path: string, options?: LoadImageOptions): GeaEmbeddedImage

// Load + decode a BUILD-EMBEDDED asset by its path relative to the app directory
// (e.g. 'assets/icons/00-music.png') — the same bytes `<img src="...">` uses.
// That JSX path lowers a string LITERAL directly to the generated asset symbol,
// so a canvas app choosing among N bundled images at runtime cannot use it; this
// goes through the generated lookup table instead. Decode is deferred to first
// draw and memoized per asset. `width === 0` means no such asset was bundled.
export declare function loadAssetImage(path: string): GeaEmbeddedImage

// Persist raw bytes to the file cache (creating parent directories). Returns
// false when no writable storage is mounted (so callers can ignore the result).
export declare function writeCacheFile(path: string, bytes: Uint8Array): boolean

// Read a whole file's raw bytes from the persistent cache (e.g. a .pmtiles
// pushed to /sdcard). Empty Uint8Array when the file is absent / no storage.
export declare function readCacheFile(path: string): Uint8Array

// Read a .pmtiles archive flashed into the spare ota_1 partition over USB
// (esptool). Empty Uint8Array when none present (or off-device).
export declare function readMapArchive(): Uint8Array

// Read a byte range [offset, offset+length) from a persistent-cache file
// (e.g. one tile out of a .pmtiles on microSD) without loading the whole file.
export declare function readFileRange(path: string, offset: number, length: number): Uint8Array

// List a directory's regular-file names (non-recursive) from persistent storage
// (e.g. the EPUBs in /sdcard/books). Empty array when the directory is missing
// or no storage is mounted.
export declare function listCacheFiles(path: string): string[]

// Synchronous HTTP GET over WiFi — the body as bytes (e.g. an MVT tile) or text
// (e.g. a TileJSON). Empty when WiFi is down or the request fails.
export declare function fetchBytes(url: string): Uint8Array
export declare function fetchText(url: string): string

// Wrap an already-decoded image-store slot id (e.g. one delivered by the async
// tile loader's poll/imageId) in the native GeaEmbeddedImage handle.
export declare function imageFromId(id: number): GeaEmbeddedImage

export class Store {}

export class BLEServer extends Store {
  constructor(deviceName: string, appearance: number, macAddress: string)
  deviceName: string
  appearance: number
  macAddress: string
  onConnected(): void
  onDisconnected(): void
  onBound(): void
  startAdvertising(): void
  stopAdvertising(): void
}

export class Component<RootElement extends GeaElement = GeaElement, Props = void> {
  readonly el: RootElement | null
  // `Props` is what a `<Tag ... />` hands this component, stated by the
  // component itself -- the same shape React's `Component<Props>` has, and for
  // the same reason. It replaces `template(...args: any[]): any`, a declaration
  // saying the framework does not know what a template takes or produces; a
  // compiler is obliged to believe that, so every component in the corpus
  // carried a boxed argument array and a boxed result.
  //
  // The default is `void` because the overwhelming majority of templates take
  // nothing, and the parameter is OPTIONAL for a physical reason. `void` in a
  // stored position -- which a parameter slot is -- is `undefined`; that is
  // what `representation/primitives.ts`'s `storedCarrier` says, and the ABI
  // projected from the callable's own type applies it. The binding cell the
  // body initializes does not, so a REQUIRED `props: void` is bound as `void`
  // against an ABI declaring `undefined` and the whole translation unit is
  // refused (`parameter 0 is bound as "void" but the ABI declares
  // "undefined"`). Writing `props?: Props` makes the declared type
  // `void | undefined`, which is `undefined` on both sides, and the question
  // never arises. An override that declares no parameter at all is legal
  // either way -- fewer parameters always is -- so `template() {}` needs no
  // type argument and no change.
  //
  // A component that DOES take props states them: `class BookCover extends
  // Component<GeaElement, { book: BookRow }>`. That is not optional -- a method
  // override's parameters are checked bivariantly, and `undefined` is not
  // assignable to a props object nor it to `undefined`, so an unstated props
  // type is a checker error rather than a silent boxing.
  // `gea-embedded-compat-transform` states it for the function components it
  // rewrites into this shape.
  //
  // The result is `GeaJsxElement | null`, not `RootElement`: what a template
  // evaluates to is `JSX.Element`, which this file declares as extending
  // `GeaJsxElement` (line 27) -- an element in the JSX sense, which is NOT the
  // same thing as the mounted host node `el` points at. `null` is the base's
  // own stub answer in `runtime/compiler.ts`.
  template(props?: Props): GeaJsxElement | null
}

// Opt-in base for a component that holds its OWN reactive state — its instance
// fields are reactive and its methods/getters can mutate/derive them, so a
// component can be self-contained without a separate `Store` singleton. On the
// embedded target this compiles to a lean component-as-store (typed state +
// compile-time-wired reactivity), so plain `Component` subclasses keep zero
// reactive overhead. Extend this only when the component manages its own state.
export class ReactiveComponent<
  RootElement extends GeaElement = GeaElement,
  Props = void
> extends Component<RootElement, Props> {}

export interface BluetoothKeyboard {
  tap(hidCode: number): void
  down(modifier: number, hidCode: number): void
  up(): void
}

export interface BluetoothMouse {
  move(dx: number, dy: number, buttons?: number, wheel?: number): void
  click(button: number): void
}

export interface BluetoothMidi {
  /**
   * Register the BLE-MIDI service (MIDI over GATT, service UUID
   * 03B80E5A-EDE8-4B33-A751-6CE34EC4C700) with the BLE host and include it
   * in advertising. Idempotent; call once before startAdvertising().
   */
  enable(): void
  /** True when a connected central has subscribed to MIDI notifications. */
  bound(): boolean
  /**
   * Send one already-framed BLE-MIDI packet (header byte, timestamp byte,
   * then MIDI message bytes). Routed to whichever role is active: as a
   * notification on our MIDI I/O characteristic (peripheral role, DAW
   * connected to us) or as a Write Without Response to the peer's MIDI I/O
   * characteristic (central role, we connected to a pedal/WIDI adapter).
   * Max ATT_MTU-3 bytes.
   */
  send(bytes: number[]): void
  /**
   * Central role: start scanning for BLE-MIDI peripherals (advertised MIDI
   * service UUID, or names starting with WIDI/PuckCC). Stops advertising —
   * one role is active at a time. Poll results with scanCount()/scanNameAt().
   */
  startScan(): void
  stopScan(): void
  scanning(): boolean
  scanCount(): number
  scanNameAt(index: number): string
  /** Connect to scan result `index` as a GATT client and resolve its MIDI characteristic. */
  connect(index: number): void
  disconnect(): void
}

export interface BluetoothHidHost {
  /**
   * HID host role: this device is the central and a remote HID peripheral
   * (a keyboard/macro pad such as the XPPen ACK05) is the input source.
   * Start scanning for HID peripherals (advertised HID service 0x1812).
   * Poll results with scanCount()/scanNameAt().
   */
  startScan(): void
  stopScan(): void
  scanning(): boolean
  scanCount(): number
  scanNameAt(index: number): string
  /**
   * Connect to scan result `index`, discover its HID service, subscribe to
   * every input-report characteristic, and initiate encryption/bonding.
   */
  connect(index: number): void
  disconnect(): void
  /** True while a hosted HID peripheral is connected and subscribed. */
  bound(): boolean
  /**
   * Raw input-report queue, oldest first. Each entry is one notification:
   * reportIdAt() gives the HID report ID it arrived on, reportLenAt()/
   * reportByteAt() expose the payload. Drain with clearReports() after
   * decoding — the queue is bounded and drops oldest on overflow.
   */
  reportCount(): number
  reportIdAt(index: number): number
  reportLenAt(index: number): number
  reportByteAt(index: number, byteIndex: number): number
  clearReports(): void
}

export interface BluetoothConnections {
  /**
   * Live registry of every concurrent BLE link this device holds, across
   * roles. kind: 0 = HID central peer (a desktop using this device as
   * keyboard/mouse), 1 = MIDI central peer (a DAW), 2 = HID peripheral we
   * host (macro pad), 3 = BLE-MIDI peripheral we drive (pedal/WIDI).
   */
  count(): number
  kindAt(index: number): number
  nameAt(index: number): string
}

export interface BluetoothConfig {
  /**
   * Config service (custom GATT, service A1E0F000-1B2C-4D3E-9F80-1234567890AB).
   * A Web Bluetooth portal reads the app's current config blob and, once
   * paired with the 4-digit code shown on-device, writes a new one.
   *
   * Publish the current config blob (opaque UTF-8 JSON, <= 4096 bytes) so the
   * portal can read it back. Bytes are 0..255; call whenever the app's config
   * changes.
   */
  setDocument(bytes: number[]): void
  /**
   * Length of a committed inbound blob the portal wrote, or 0 if none pending.
   * Poll each frame; on a non-zero length, read pendingByteAt(0..len-1), parse,
   * apply, then consumePending().
   */
  pendingLength(): number
  pendingByteAt(index: number): number
  /** Drop the pending inbound blob after the app has read and applied it. */
  consumePending(): void
  /** True while the on-device pairing overlay should be shown. */
  pairing(): boolean
  /** The 4-digit pairing code to display, or "" when not pairing. */
  pairCode(): string
  /** Close the pairing overlay (user backed out / timed out). */
  dismissPairing(): void
  /**
   * Device -> portal activity push. When a control fires on the device, call
   * this with the control's index so a subscribed portal can light up the
   * matching control live (CTRL notify, opcode 0xA0). No-op when no portal is
   * subscribed to the config CTRL characteristic.
   */
  pushActivity(index: number): void
}

export interface BluetoothController {
  readonly keyboard: BluetoothKeyboard
  readonly mouse: BluetoothMouse
  readonly midi: BluetoothMidi
  readonly hidHost: BluetoothHidHost
  readonly connections: BluetoothConnections
  readonly config: BluetoothConfig
  enabled(): boolean
  setEnabled(enabled: boolean): void
  connected(): boolean
  bound(): boolean
  batteryLevel(): number
  mac(): string
  deviceName(): string
  init(deviceName: string, appearance?: number, macAddress?: string): void
  startAdvertising(): void
  stopAdvertising(): void
}

export declare const BLE: BluetoothController
export declare const bluetooth: BluetoothController

export interface AudioController {
  getVolume(): number
  setVolume(volume: number): void
}

export declare const Audio: AudioController

export interface CanvasRenderingContext2D {
  fillStyle: string | number
  strokeStyle: string | number
  globalAlpha: number
  lineWidth: number
  font: string
  textBaseline: string
  textAlign: string
  clear(): void
  clearRect(x: number, y: number, width: number, height: number): void
  fillRect(x: number, y: number, width: number, height: number): void
  strokeRect(x: number, y: number, width: number, height: number): void
  fillCircle(x: number, y: number, radius: number, fill?: string | number): void
  strokeCircle(x: number, y: number, radius: number): void
  fillCircleRgb565(x: number, y: number, radius: number, color: number): void
  fillTriangleRgb565(x0: number, y0: number, x1: number, y1: number, x2: number, y2: number, color: number): void
  fillCirclesRgb565(xs: ArrayLike<number>, ys: ArrayLike<number>, radius: number, colors: ArrayLike<number>, count?: number): void
  fillCirclesRgb565Uniform(xs: ArrayLike<number>, ys: ArrayLike<number>, radius: number, color: number): void
  /**
   * Batched opaque triangles as ONE recorded present command. `order[0..count)`
   * indexes the coordinate/colour arrays in paint (depth) order; colours are
   * authored 0xRRGGBBAA. Replaces per-triangle fillTriangleRgb565 loops whose
   * per-command record/dispatch overhead dominates at hundreds of triangles.
   */
  fillTrianglesRgb565Sorted(
    x0s: ArrayLike<number>,
    y0s: ArrayLike<number>,
    x1s: ArrayLike<number>,
    y1s: ArrayLike<number>,
    x2s: ArrayLike<number>,
    y2s: ArrayLike<number>,
    colors: ArrayLike<number>,
    order: ArrayLike<number>,
    count?: number,
  ): void
  beginPath(): void
  arc(x: number, y: number, radius: number, startAngle: number, endAngle: number): void
  moveTo(x: number, y: number): void
  lineTo(x: number, y: number): void
  closePath(): void
  fill(): void
  stroke(): void
  fillText(text: string, x: number, y: number): void
  // Width in px of `text` in the context's CURRENT font. textAlign is a no-op
  // on this canvas and fillText anchors the TOP-LEFT at (x, y), so centring
  // means subtracting half of this.
  measureText(text: string): number
  // Offset from fillText's y anchor to the vertical CENTRE of the text's ink.
  // fillText anchors the top of the line box, which carries ascender and
  // descender the glyphs do not fill, so subtracting half the font size sags.
  measureTextInkCenter(text: string): number
  drawImage(image: GeaEmbeddedImage, dx: number, dy: number): void
  drawImage(image: GeaEmbeddedImage, dx: number, dy: number, width: number, height: number): void
  // Draw an image masked to a circle inscribed in the destination box. Lets an
  // OPAQUE image (a JPEG, which has no alpha channel) render as a round tile
  // with no alpha plane: the blit skips the pixels outside the shape rather
  // than blending them. A square opaque tile would otherwise paint its
  // corners over whatever it overlaps.
  drawImageCircle(image: GeaEmbeddedImage, dx: number, dy: number, width: number, height: number): void
  drawImageRotated90CW(image: GeaEmbeddedImage, dx: number, dy: number, width: number, height: number): void
  drawImageTiledX(image: GeaEmbeddedImage, dx: number, dy: number, width: number): void
  flush(): void
  beginBatch(): void
  endBatch(): void
}

export interface DisplayFlushConfig {
  rows: number
  depth: number
}

export interface DisplayMemoryConfig {
  commandBufferCommands?: number
  backgroundCache?: boolean
}

// E-paper refresh policy + custom waveforms. All fields optional — omitted
// fields keep their current value. LUTs are the panel controller's raw
// 159-byte waveform tables ([] restores the board's vendor default).
// fastStreakWindowMs: updates arriving within this window of the previous
// refresh use the fast waveform; 0 disables fast mode. No-op on boards
// without an e-paper panel (and on web).
export interface DisplayEpaperRefreshConfig {
  fullRefreshEveryPartials?: number
  fullRefreshHardCapPartials?: number
  fastStreakWindowMs?: number
  partialLut?: number[]
  fastLut?: number[]
  /**
   * Render the framebuffer in 4-level grayscale (white / light grey / dark
   * grey / black) on e-paper panels that support it (e.g. SSD1681). Gray mode
   * trades the fast partial waveform for a slower full refresh (~1 s) that
   * drives 4 reflectance levels instead of 1-bit dithering. No-op on boards
   * without grayscale support. Omit to leave the current mode unchanged.
   */
  grayscale?: boolean
  /**
   * Whether a partial update that covers the whole screen (e.g. an e-reader
   * page turn) is promoted to a flashing full refresh. A reading app that
   * prefers flash-free page turns sets false and relies on
   * fullRefreshEveryPartials to clear ghosting. Omit to leave unchanged.
   */
  fullOnCover?: boolean
}

export type DisplayOrientation = 'portrait-primary' | 'portrait-secondary' | 'landscape-primary' | 'landscape-secondary'

export type DisplayOrientationSupport = DisplayOrientation | 'portrait' | 'landscape' | 'all'

export type DisplayPixelFormat = 'rgb565' | 'rgb888' | 'rgb8888' | 'argb8888'

export interface DisplayController {
  readonly ctx: CanvasRenderingContext2D
  /** Logical viewport width in px after the current display orientation is applied. */
  readonly width: number
  /** Logical viewport height in px after the current display orientation is applied. */
  readonly height: number
  /** Physical panel width in px before display orientation is applied. */
  readonly nativeWidth: number
  /** Physical panel height in px before display orientation is applied. */
  readonly nativeHeight: number
  orientation: DisplayOrientation
  supportedOrientations: DisplayOrientationSupport[]
  autoRotate: boolean
  pixelFormat: DisplayPixelFormat
  readonly panelPixelFormat: DisplayPixelFormat
  readonly supportedPixelFormats: DisplayPixelFormat[]
  getBrightness(): number
  setBrightness(brightness: number): void
  getOrientation(): DisplayOrientation
  setOrientation(orientation: DisplayOrientation): void
  getSupportedOrientations(): DisplayOrientationSupport[]
  setSupportedOrientations(orientations: DisplayOrientationSupport | DisplayOrientationSupport[]): void
  getAutoRotate(): boolean
  setAutoRotate(enabled: boolean): void
  /**
   * Opt into tearing sync (TE/VBlank). When enabled on a board that wires a TE
   * line, frames are aligned to the panel's vertical blank so pans and animations
   * present without tearing. No-op on boards without a TE line. Off by default.
   */
  setVSync(on: boolean): void
  /**
   * Opt into the large-static-text raster cache. When enabled, big text
   * (font-size ≳ 40px — HUD badges, clocks, counters) is rasterized to a coverage
   * sprite once and blitted on subsequent frames instead of re-running the glyph
   * rasterizer every frame. Saves CPU/power when large text shares dirty regions
   * with moving content; no effect on small UI text. Off by default.
   */
  setTextRasterCache(on: boolean): void
  /**
   * Declare that text always draws over ONE solid color (0xRRGGBB). On packed
   * 4-bit grayscale targets (e-paper) each glyph is then pre-blended against
   * that backdrop once and stamped as straight byte copies — no per-pixel
   * framebuffer read or blend. Glyphs whose color equals the backdrop (e.g.
   * inverted rows) automatically fall back to the true blend. Pass a negative
   * value to disable. No-op on full-color targets.
   */
  setTextSolidBackdrop(rrggbb: number): void
  /**
   * Mark the next present as a full-screen change ("damage-all"). The present
   * then skips the dirty-rect diff and its persistent previous-frame copy — both
   * exist only to flush less when little changed, which is pure waste during a
   * full-screen pan (the diff would conclude "everything changed" and full-flush
   * anyway). Call it once per frame while actively panning/animating the whole
   * screen; stop calling it when motion settles so idle gets the cheap diff back.
   */
  invalidate(): void
  getPixelFormat(): DisplayPixelFormat
  setPixelFormat(format: DisplayPixelFormat): void
  getPanelPixelFormat(): DisplayPixelFormat
  getSupportedPixelFormats(): DisplayPixelFormat[]
  getDevicePixelRatio(): number
  setDevicePixelRatio(devicePixelRatio: number): void
  getFrameIntervalMs(): number
  setFrameIntervalMs(intervalMs: number): void
  getFrameRate(): number
  setFrameRate(fps: number): void
  setAA(samples: number): void
  setFlushConfig(config: DisplayFlushConfig): void
  setMemoryConfig(config: DisplayMemoryConfig): void
  setEpaperRefreshConfig(config: DisplayEpaperRefreshConfig): void
  epaperFullRefresh(): void
}

export declare const Display: DisplayController

export interface EmbeddedMemoryStats {
  internalFree: number
  internalLargestFreeBlock: number
  internalMinimumFree: number
  psramFree: number
  currentTaskStackHighWaterMark: number
  allocationSramCount: number
  allocationPsramCount: number
  allocationSramBytes: number
  allocationPsramBytes: number
  allocationSramPeakBytes: number
  allocationPsramPeakBytes: number
}

export interface EmbeddedMemoryConfig {
  geaMainStackBytes: number
  geaInitStackBytes: number
  appFrameStackWords: number
  appFrameStackBytes: number
  displayFlushConfiguredRows: number
  displayFlushConfiguredDepth: number
  displayFlushBufferMaxBytes: number
  displayFlushRows: number
  displayFlushDepth: number
  displayFlushBufferBytes: number
}

export interface MemoryController {
  internalFree(): number
  internalLargestFreeBlock(): number
  internalMinimumFree(): number
  psramFree(): number
  currentTaskStackHighWaterMark(): number
  geaMainStackBytes(): number
  geaInitStackBytes(): number
  appFrameStackWords(): number
  appFrameStackBytes(): number
  displayFlushConfiguredRows(): number
  displayFlushConfiguredDepth(): number
  displayFlushBufferMaxBytes(): number
  displayFlushRows(): number
  displayFlushDepth(): number
  displayFlushBufferBytes(): number
  allocationSramCount(): number
  allocationPsramCount(): number
  allocationSramBytes(): number
  allocationPsramBytes(): number
  allocationSramPeakBytes(): number
  allocationPsramPeakBytes(): number
  stats(): EmbeddedMemoryStats
  config(): EmbeddedMemoryConfig
}

export declare const Memory: MemoryController

export interface InputController {
  // Reads-and-clears the one-shot back-button-pressed flag. Set by the
  // platform launcher-button task. Apps poll this in their rAF loop and dismiss overlays.
  consumeBackButton(): boolean
}

export declare const Input: InputController

export interface GpioController {
  // Drive a pin. Returns whether this board did it: a pin the chip does not
  // have, or a target with no GPIO at all, reports false rather than throwing,
  // so an app written for one board degrades on another instead of crashing.
  configureOutput(pin: number): boolean
  configureInput(pin: number, pullUp: boolean): boolean
  write(pin: number, level: boolean): boolean
  read(pin: number): boolean
}

export declare const Gpio: GpioController

export interface LedController {
  // A WS2812/NeoPixel strand on one pin — what "the onboard LED" is on most
  // modern ESP32 modules.
  //
  // set() is the default and usually the only call an app needs: it attaches
  // the pin if it is not already attached, writes the colour and clocks it out.
  // off() is set() with black, which is what "off" means on a WS2812.
  set(pin: number, r: number, g: number, b: number): boolean
  off(pin: number): boolean
  // The multi-pixel path, for a real strand where one transmit per frame beats
  // one per pixel. show() returns once the strand has latched the buffer.
  attach(pin: number, count: number): boolean
  setPixel(index: number, r: number, g: number, b: number): boolean
  show(): boolean
  // Hand the RMT channel back; the ESP32-S3 has four.
  detach(): void
}

export declare const Led: LedController

export interface ClockController {
  // Milliseconds since the Unix epoch from the platform real-time clock
  // (gettimeofday). Reflects a host-set time (GEADEV SETTIME / the companion
  // app on launch). Unlike Date.now() — which reads a monotonic clock on
  // ESP32 — this advances as real wall-clock time once synced, and returns a
  // small value (< ~1.6e12) before the device clock has ever been set.
  epochMs(): number
}

export declare const Clock: ClockController

export interface ProfilerController {
  // Monotonic microseconds from the platform high-resolution timer. Intended for
  // temporary app/runtime instrumentation; use Date.now() or Clock.epochMs() for
  // user-visible time.
  nowUs(): number
  /** Raw CPU cycle count (single register read; far cheaper than nowUs). 32-bit, wraps ~18s at 240MHz. */
  nowCycles(): number
}

export declare const Profiler: ProfilerController

export interface GeaLocalStorage {
  // Browser-compatible localStorage: a synchronous key/value store that PERSISTS
  // across reboots / power cycles (backed by NVS on device). Reached through the
  // global `localStorage` — no import. Safe to read/write from anywhere,
  // including a store's init(). getItem returns '' for a missing key.
  readonly length: number
  getItem(key: string): string
  setItem(key: string, value: string): void
  removeItem(key: string): void
  clear(): void
  key(index: number): string
}

declare global {
  const localStorage: GeaLocalStorage
}

export interface BatteryController {
  // Battery charge percentage (0-100) read from the platform PMU.
  level(): number
}

export declare const Battery: BatteryController

export interface NotifyController {
  // Transient host->app notification channel (e.g. pushed from the companion
  // via GEADEV NOTIFY). text() is the latest message; seq() increments on each
  // post so an app can detect a new one and show a transient banner.
  text(): string
  seq(): number
}

export declare const Notify: NotifyController

export interface DeviceControlController {
  // Run a host shell command, return combined stdout+stderr. macOS companion
  // only — drives the device-control + install/uninstall scripts over USB.
  exec(command: string): string
}

export declare const DeviceControl: DeviceControlController

export interface AppsController {
  launch(appId: string): number
}

export declare const Apps: AppsController

export type OscillatorType = 'sine' | 'square' | 'sawtooth' | 'triangle'

export interface AudioParam {
  value: number
  setValueAtTime(value: number, startTime: number): void
}

export interface AudioDestinationNode {}

export interface AudioBuffer {
  readonly sampleRate: number
  readonly length: number
  readonly duration: number
  readonly numberOfChannels: number
  copyFromChannel(destination: Float32Array<ArrayBuffer>, channelNumber: number, bufferOffset?: number): void
  copyToChannel(source: Float32Array<ArrayBuffer>, channelNumber: number, bufferOffset?: number): void
  getChannelData(channel: number): Float32Array<ArrayBuffer>
}

export interface AudioBufferSourceNode {
  buffer: AudioBuffer
  connect(destination: AudioDestinationNode): AudioDestinationNode
  start(when?: number): void
  stop(when?: number): void
}

export interface OscillatorNode {
  type: OscillatorType
  frequency: AudioParam
  connect(destination: AudioDestinationNode): AudioDestinationNode
  start(when?: number): void
  stop(when?: number): void
}

export interface AudioContext {
  readonly currentTime: number
  readonly destination: AudioDestinationNode
  createOscillator(): OscillatorNode
  createBufferSource(): AudioBufferSourceNode
  decodeAudioData(audioData: ArrayBuffer | Uint8Array | GeaAudioBlob): Promise<AudioBuffer>
}

export interface AudioContextConstructor {
  new (): AudioContext
}

export declare const audioContext: AudioContext

export interface AccelerometerReading {
  x: number
  y: number
  z: number
  tiltX: number
  tiltY: number
  gyroscopeX: number
  gyroscopeY: number
  gyroscopeZ: number
  timestamp: number
}

export interface AccelerometerController {
  readonly x: number
  readonly y: number
  readonly z: number
  readonly accelerationX: number
  readonly accelerationY: number
  readonly accelerationZ: number
  readonly gyroscopeX: number
  readonly gyroscopeY: number
  readonly gyroscopeZ: number
  readonly tiltX: number
  readonly tiltY: number
  start(): void
  close(): void
  calibrateBias(): void
}

export declare const Accelerometer: AccelerometerController

export type CameraFacing = 'front' | 'back' | 'external'
export type CameraFlashMode = 'off' | 'on' | 'auto' | 'torch'

export interface CameraOpenOptions {
  facing?: CameraFacing
  width?: number
  height?: number
}

export interface CameraDevice {
  readonly id: string
  readonly facing: CameraFacing
}

export interface CameraCaptureOptions {
  mirror?: boolean
}

export interface CameraPhoto {
  readonly imageId: number
  readonly width: number
  readonly height: number
  readonly orientation: number
  dispose(): void
}

export type CameraExposureMode = 'auto' | 'continuous' | 'locked' | 'manual'
export type CameraWhiteBalanceMode = 'auto' | 'continuous' | 'locked'
export type CameraFocusMode = 'auto' | 'continuous' | 'locked'

export interface CameraExposureOptions {
  mode?: CameraExposureMode
  /** Exposure compensation in EV stops (auto/continuous modes). */
  bias?: number
  /** ISO / sensor gain (manual mode). */
  iso?: number
  /** Shutter duration in milliseconds (manual mode). */
  durationMs?: number
}

export interface CameraWhiteBalanceOptions {
  mode?: CameraWhiteBalanceMode
  /** Correlated colour temperature in Kelvin (locked mode). */
  temperature?: number
  /** Green/magenta tint (where supported). */
  tint?: number
}

export interface CameraFocusOptions {
  mode?: CameraFocusMode
  /** Normalized focus point of interest [0..1] (where supported). */
  point?: { x: number; y: number }
}

export interface CameraRecordOptions {
  /** Sink path for the clip (esp32-p4 writes Motion-JPEG). */
  path: string
  /** Target frames per second (default 15). */
  fps?: number
}

export interface CameraClip {
  /** Recorded clip duration in milliseconds. */
  readonly durationMs: number
  /** The sink path the clip was written to. */
  readonly path: string
}

export interface CameraController {
  isAvailable(): boolean
  hasPermission(): boolean
  requestPermission(): Promise<boolean>
  open(options?: CameraOpenOptions | CameraFacing): boolean
  close(): void
  isOpen(): boolean
  readonly facing: CameraFacing
  readonly width: number
  readonly height: number
  readonly orientation: number
  draw(x: number, y: number, destWidth?: number, destHeight?: number): void
  /** Capture a still frame. `capturePhoto` is the preferred alias of `capture`. */
  capture(options?: CameraCaptureOptions): Promise<CameraPhoto>
  capturePhoto(options?: CameraCaptureOptions): Promise<CameraPhoto>
  /** Record a video clip (esp32-p4: Motion-JPEG; iOS movie: later). */
  startRecording(options: CameraRecordOptions): Promise<void>
  stopRecording(): Promise<CameraClip>
  isRecording(): boolean
  setFlash(mode: CameraFlashMode): void
  setZoom(factor: number): void
  setMirror(mirror: boolean): void
  /** Exposure / AE controls. */
  setExposure(options: CameraExposureOptions): void
  /** White balance controls. */
  setWhiteBalance(options: CameraWhiteBalanceOptions): void
  /** Focus controls. */
  setFocus(options: CameraFocusOptions): void
  /** Torch / video light. */
  setTorch(mode: 'off' | 'on' | 'auto', level?: number): void
  /** Switch to another camera by facing or device id (reopens the stream). */
  switchCamera(target: CameraFacing | string): boolean
  getDevices(): CameraDevice[]
}

export declare const Camera: CameraController

export interface GeolocationCoordinates {
  readonly latitude: number
  readonly longitude: number
  readonly altitude: number
  readonly accuracy: number
  readonly altitudeAccuracy: number
  readonly heading: number
  readonly speed: number
}

export interface GeolocationPosition {
  readonly coords: GeolocationCoordinates
  readonly timestamp: number
  readonly hasFix: boolean
}

export interface GeolocationPositionError {
  readonly code: number
  readonly message: string
}

export interface PositionOptions {
  enableHighAccuracy?: boolean
  timeout?: number
  maximumAge?: number
}

export interface GeolocationController {
  hasFix(): boolean
  currentPosition(): GeolocationPosition
  coords(): GeolocationCoordinates
  latitude(): number
  longitude(): number
  accuracy(): number
  getCurrentPosition(
    success: (position: GeolocationPosition) => void,
    error?: (error: GeolocationPositionError) => void,
    options?: PositionOptions
  ): void
  watchPosition(
    success: (position: GeolocationPosition) => void,
    error?: (error: GeolocationPositionError) => void,
    options?: PositionOptions
  ): number
  clearWatch(id: number): void
}

export declare const Geolocation: GeolocationController
export declare const geolocation: GeolocationController

export interface WiFiController {
  enabled(): boolean
  setEnabled(enabled: boolean): void
  connected(): boolean
  rssi(): number
  ssid(): string
  ip(): string
  mac(): string
  configure(ssid: string, password: string): void
  waitForConnection(timeoutMs: number): boolean
  startScan(): void
  scanning(): boolean
  scanCount(): number
  scanSsidAt(index: number): string
  scanRssiAt(index: number): number
  scanSecuredAt(index: number): boolean
}

export declare const WiFi: WiFiController
export declare const wifi: WiFiController

export interface ButtonProps {
  class?: ClassValue
  style?: Style
  pressId?: number
  pressValue?: number
  onClick?: PressHandler
  onPress?: PressHandler
  children?: any
}

export interface NativeButtonProps extends NativeEventAttributes {
  key?: string | number
  class?: ClassValue
  style?: Style
  ref?: GeaElement | null
  pressId?: number
  pressValue?: number
  children?: any
}

export interface NativeVirtualListProps extends NativeEventAttributes {
  onScroll?: (event: Event) => void
  id?: string
  class?: ClassValue
  style?: Style
  ref?: GeaElement | null
  children?: any
  momentum?: boolean | string
  itemCount?: number | string
  itemHeight?: number | string
  labelPrefix?: string
  showPixels?: boolean | string
  'item-count'?: number | string
  'item-height'?: number | string
  'label-prefix'?: string
  'show-pixels'?: boolean | string
}

export interface InputElementProps {
  class?: ClassValue
  style?: Style
  // The three shapes the renderer actually materializes: `range` becomes an
  // NSSlider and `checkbox` an NSSwitch, everything else an NSTextField
  // (`apple/targets/macos/main/macos_renderer.mm`, `makeViewForType`). The union
  // had drifted to the text-field spellings alone, so an app writing the switch
  // the renderer has always drawn did not typecheck.
  type?: 'text' | 'password' | 'button' | 'range' | 'checkbox'
  // Optional, because the renderer treats it as one: every reader guards with
  // `if (value && value[0])`, and `applySwitchProps` never reads it at all --
  // a switch's state is `checked`. Requiring it made `<input type="checkbox">`
  // impossible to write. The cost is that a text field with a forgotten value
  // no longer fails to typecheck; the union arm that would keep both is a
  // larger change than the drift being fixed here.
  value?: string
  // Read back off the node by the renderer as attributes, hence the string form
  // alongside the natural one -- `min`/`max` bound the slider, `checked` is the
  // switch's state and is written back on toggle.
  min?: number | string
  max?: number | string
  checked?: boolean | string
  placeholder?: string
  autoFocus?: boolean | number
  pressId?: number
  pressValue?: number
  onClick?: PressHandler
  onInput?: (event: InputEvent) => void
  onFocus?: () => void
  onBlur?: () => void
  onKeyDown?: (event: KeyEvent) => void
  input?: (value: string) => void
  focus?: () => void
  blur?: () => void
  keydown?: (keyCode: number) => void
}

export interface NativeInputElementProps extends InputElementProps {
  ref?: GeaElement | null
}

export interface TextAreaElementProps {
  class?: ClassValue
  style?: Style
  value: string
  placeholder?: string
  onInput?: (event: InputEvent) => void
  onFocus?: () => void
  onBlur?: () => void
}

export interface NativeTextAreaElementProps extends TextAreaElementProps {
  ref?: GeaElement | null
}

export declare function mount(component: new () => Component): void

/**
 * Pack 8-bit channels into the canonical canvas colour number (0xRRGGBBAA, red in
 * the high byte). Pass the result to the numeric canvas colour APIs
 * (`fillCircleRgb565`, `fillTriangleRgb565`, …). geatsc lowers it to this board's
 * native pixel — no per-app pixel packing, and literal calls fold at build time.
 */
export declare function rgb(r: number, g: number, b: number): number
export declare function rgba(r: number, g: number, b: number, a: number): number

export interface FetchHeadersInit {
  [name: string]: string
}

export interface FetchRequestInit {
  method?: string
  headers?: FetchHeadersInit
  body?: string | ArrayBuffer | Uint8Array
}

export interface FetchHeaders {
  get(name: string): string | null
  has(name: string): boolean
}

export interface FetchResponse {
  readonly ok: boolean
  readonly status: number
  readonly statusText: string
  readonly headers: FetchHeaders
  /** Raw response body bytes (binary-safe). */
  readonly body: Uint8Array
  text(): string
  json(): unknown
  /** Binary body — feed straight to `loadImage`. Both spellings are equivalent. */
  arrayBuffer(): Uint8Array
  bytes(): Uint8Array
}

export declare function fetchAsync(url: string, init?: FetchRequestInit): number
export declare function fetchReady(id: number): boolean
export declare function fetchResult(id: number): FetchResponse
export declare function fetchRelease(id: number): void

/**
 * Streaming multipart file upload (POST). The request body is sent as
 * `prefix` + the file at `filePath` (streamed from disk in chunks, never loaded
 * into RAM) + `suffix`, so a large recording uploads without an OOM. `auth` and
 * `contentType` set the Authorization / Content-Type headers (pass '' to skip).
 * Async: returns a job id, polled/collected with fetchReady/fetchResult/fetchRelease.
 */
export declare function fetchUploadFileAsync(
  url: string,
  auth: string,
  contentType: string,
  prefix: string,
  filePath: string,
  suffix: string
): number

/**
 * Progress (0..1) of the single in-flight streaming upload, or -1 when no upload
 * is active. Sync uploads one note at a time, so this tracks the current one.
 */
/**
 * Streaming download: GET `url`, writing the response body to `destPath` on the
 * device (SD), in chunks — never buffered whole in RAM (unlike `fetch`, capped
 * at 2 MB). Async: returns a job id, polled with fetchReady/fetchResult/fetchRelease.
 * Progress shares the fetchUploadProgress/fetchUploadSent/fetchUploadTotal counters.
 */
export declare function fetchDownloadFileAsync(url: string, auth: string, destPath: string): number
export declare function fetchUploadProgress(id: number): number
/** Bytes streamed / total bytes of the active upload (0 when idle) — for MB and speed readouts. */
export declare function fetchUploadSent(id: number): number
export declare function fetchUploadTotal(id: number): number

declare global {
  function fetch(url: string, init?: FetchRequestInit): FetchResponse
  function fetchAsync(url: string, init?: FetchRequestInit): number
  function fetchUploadFileAsync(
    url: string,
    auth: string,
    contentType: string,
    prefix: string,
    filePath: string,
    suffix: string
  ): number
  function fetchDownloadFileAsync(url: string, auth: string, destPath: string): number
  function fetchUploadProgress(id: number): number
  function fetchUploadSent(id: number): number
  function fetchUploadTotal(id: number): number
  function fetchReady(id: number): boolean
  function fetchResult(id: number): FetchResponse
  function fetchRelease(id: number): void
  function atob(data: string): string
  function btoa(data: string | ArrayBuffer | Uint8Array): string
}

export interface WebSocketMessageEvent {
  readonly data: string
}

export interface WebSocketCloseEvent {
  readonly code: number
  readonly reason: string
  readonly wasClean: boolean
}

export interface WebSocketErrorEvent {
  readonly message: string
}

export interface WebSocketInstance {
  readonly url: string
  readonly readyState: number
  onopen: (() => void) | null
  onmessage: ((event: WebSocketMessageEvent) => void) | null
  onclose: ((event: WebSocketCloseEvent) => void) | null
  onerror: ((event: WebSocketErrorEvent) => void) | null
  send(data: string): void
  close(code?: number, reason?: string): void
}

export interface WebSocketConstructor {
  new (url: string): WebSocketInstance
  readonly CONNECTING: 0
  readonly OPEN: 1
  readonly CLOSING: 2
  readonly CLOSED: 3
}

declare global {
  const WebSocket: WebSocketConstructor
}

/**
 * A device-hosted HTTP server, modeled after Node's `http` package. The device
 * connects to Wi-Fi (STA) and serves over its LAN IP, so a phone or laptop on
 * the same network can browse and download from it.
 *
 * Unlike Node, the handler does not get a mutable `res`: it RETURNS a plain
 * reply object. This keeps every request a pure value crossing the JS<->native
 * boundary (like a WebSocket message), and lets large responses stream straight
 * off disk — a reply that sets `file` streams that path on the server worker
 * without ever loading the bytes into JS.
 */
export interface IncomingMessage {
  /** "GET", "POST", ... */
  readonly method: string
  /** Full request target including the query string (e.g. "/n/3.wav?x=1"). */
  readonly url: string
  /** Request path with the query stripped (e.g. "/n/3.wav"). */
  readonly path: string
  /** Raw query string after '?', or "" when absent. */
  readonly query: string
}

export interface HttpReply {
  /** HTTP status code. Defaults to 200. */
  status?: number
  /** Response Content-Type. Defaults to "text/plain". */
  contentType?: string
  /** Inline response body. Ignored when `file` is set. */
  body?: string
  /** Stream this file path straight from disk; large files never enter JS. */
  file?: string
  /** When set, send as a download with this attachment filename. */
  download?: string
}

export type HttpRequestHandler = (req: IncomingMessage) => HttpReply

export interface HttpServer {
  /** Start listening on `port`. Returns false if the socket could not bind. */
  listen(port: number): boolean
  /** Stop the server and release the port. */
  close(): void
  /** Numeric handle — persist this (not the object) to close later via `http.close`. */
  id(): number
}

export interface HttpModule {
  createServer(handler: HttpRequestHandler): HttpServer
  /** Stop + release a server by the handle from `HttpServer.id()`. */
  close(handle: number): void
}

export declare const http: HttpModule

export interface MediaStreamTrack {
  readonly id: string
  readonly kind: 'audio'
  enabled: boolean
  readonly readyState: 'live' | 'ended'
  stop(): void
}

export interface MediaStream {
  readonly id: string
  getAudioTracks(): MediaStreamTrack[]
  getTracks(): MediaStreamTrack[]
}

export interface MediaStreamConstructor {
  new (): MediaStream
}

export interface MediaStreamConstraints {
  audio?: boolean | { sampleRate?: number; channelCount?: number; echoCancellation?: boolean }
  video?: false
}

export interface MediaDevices {
  getUserMedia(constraints: MediaStreamConstraints): Promise<MediaStream>
}

export interface GeaAudioBlob extends Blob {
  readonly path: string
}

export interface MediaRecorderDataAvailableEvent {
  readonly data: GeaAudioBlob
}

export type MediaRecorderState = 'inactive' | 'recording' | 'paused'

export interface MediaRecorderOptions {
  mimeType?: string
  audioBitsPerSecond?: number
  path?: string
}

export interface MediaRecorder {
  readonly stream: MediaStream
  readonly mimeType: string
  readonly state: MediaRecorderState
  ondataavailable: ((event: MediaRecorderDataAvailableEvent) => void) | null
  onstop: (() => void) | null
  start(timeslice?: number): void
  stop(): void
}

export interface MediaRecorderConstructor {
  new (stream: MediaStream, options?: MediaRecorderOptions): MediaRecorder
  isTypeSupported(type: string): boolean
}

export interface HTMLAudioElement extends Element {
  src: string
  currentTime: number
  volume: number
  loop: boolean
  play(): void
  pause(): void
}

export interface AudioConstructor {
  new (src?: string): HTMLAudioElement
}

declare global {
  interface Blob {
    readonly path: string
  }

  interface MediaRecorderOptions {
    path?: string
  }

  interface MediaRecorderDataAvailableEvent {
    readonly data: GeaAudioBlob
  }

  interface BaseAudioContext {
    decodeAudioData(audioData: Uint8Array | GeaAudioBlob): Promise<AudioBuffer>
  }

  interface Navigator {
    readonly mediaDevices: MediaDevices
    readonly geolocation: GeolocationController
  }
  const navigator: Navigator
  const MediaStream: MediaStreamConstructor
  const MediaRecorder: MediaRecorderConstructor
  const Audio: AudioConstructor
  const AudioContext: AudioContextConstructor
}

export interface RTCSessionDescriptionInit {
  type: 'offer' | 'answer' | 'pranswer' | 'rollback'
  sdp?: string
}

export interface RTCIceCandidateInit {
  candidate: string
  sdpMid?: string | null
  sdpMLineIndex?: number | null
}

export interface RTCIceServer {
  urls: string | string[]
  username?: string
  credential?: string
}

export interface RTCConfiguration {
  iceServers?: RTCIceServer[]
}

export interface RTCPeerConnectionIceEvent {
  readonly candidate: RTCIceCandidateInit | null
}

export interface RTCTrackEvent {
  readonly track: MediaStreamTrack
  readonly streams: readonly MediaStream[]
}

export interface RTCPeerConnectionInstance {
  readonly connectionState: 'new' | 'connecting' | 'connected' | 'disconnected' | 'failed' | 'closed'
  readonly iceConnectionState: 'new' | 'checking' | 'connected' | 'completed' | 'failed' | 'disconnected' | 'closed'

  onicecandidate: ((event: RTCPeerConnectionIceEvent) => void) | null
  ontrack: ((event: RTCTrackEvent) => void) | null
  onconnectionstatechange: (() => void) | null
  oniceconnectionstatechange: (() => void) | null

  addTrack(track: MediaStreamTrack, stream: MediaStream): void
  createOffer(): Promise<RTCSessionDescriptionInit>
  createAnswer(): Promise<RTCSessionDescriptionInit>
  setLocalDescription(desc: RTCSessionDescriptionInit): Promise<void>
  setRemoteDescription(desc: RTCSessionDescriptionInit): Promise<void>
  addIceCandidate(candidate: RTCIceCandidateInit): Promise<void>
  close(): void
}

export interface RTCPeerConnectionConstructor {
  new (config?: RTCConfiguration): RTCPeerConnectionInstance
}

declare global {
  const RTCPeerConnection: RTCPeerConnectionConstructor
  function Image(props: ImageProps): GeaJsxElement
}
