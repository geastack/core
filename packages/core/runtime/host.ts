// These names ALSO exist as lib.dom globals, so leaving them unimported does
// not fail to compile — it silently binds the DOM interfaces instead of ours.
// That is not cosmetic: lib.dom's GeolocationCoordinates/GeolocationPosition
// carry `toJSON(): any`, a callable member, so geatsc's record identity
// declines the shape and every method returning one seals `unresolved`, which
// fails the representation coverage guard for the whole `Geolocation` literal
// below. lib.dom's GeolocationPositionError also carries PERMISSION_DENIED /
// POSITION_UNAVAILABLE / TIMEOUT, which our host object never sets.
import type {
  CanvasRenderingContext2D,
  GeolocationCoordinates,
  GeolocationPosition,
  GeolocationPositionError,
  PositionOptions,
  TouchSample,
  CameraCaptureOptions,
  CameraClip,
  CameraDevice,
  CameraExposureMode,
  CameraExposureOptions,
  CameraFacing,
  CameraFlashMode,
  CameraFocusMode,
  CameraFocusOptions,
  CameraOpenOptions,
  CameraPhoto,
  CameraRecordOptions,
  CameraWhiteBalanceMode,
  CameraWhiteBalanceOptions,
  EmbeddedMemoryConfig,
  EmbeddedMemoryStats,
  HttpReply,
  IncomingMessage,
  HttpModule,
  HttpServer,
  AudioContext,
  WiFiController
} from '../index'

// These 17 used to be declared HERE as well as in `../index`, byte for byte.
// A duplicate is not free: the record-alias candidate registry is keyed by
// NAME across every file in the program and the last writer wins, so one
// declaration silently shadowed the other and the struct emitted for a literal
// stopped matching the struct emitted for its annotation. Re-exported so
// existing `from './runtime/host'` importers keep working.
export type {
  CameraCaptureOptions,
  CameraClip,
  CameraDevice,
  CameraExposureMode,
  CameraExposureOptions,
  CameraFacing,
  CameraFlashMode,
  CameraFocusMode,
  CameraFocusOptions,
  CameraOpenOptions,
  CameraPhoto,
  CameraRecordOptions,
  CameraWhiteBalanceMode,
  CameraWhiteBalanceOptions,
  EmbeddedMemoryConfig,
  EmbeddedMemoryStats,
  HttpReply
} from '../index'

declare const apps: {
  launch(appId: string): number
}

declare const image: {
  loadBytes(bytes: Uint8Array): number
  loadBytesOpaque(bytes: Uint8Array): number
  width(id: number): number
  height(id: number): number
  frameCount(id: number): number
  isAnimated(id: number): boolean
  setPlaying(id: number, playing: number): void
  seek(id: number, frame: number): void
  dispose(id: number): void
}

declare const navigator: {
  geolocation: {
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
  wifi: WiFiController
  bluetooth: {
    keyboard: {
      tap(hidCode: number): void
      down(modifier: number, hidCode: number): void
      up(): void
    }
    mouse: {
      move(dx: number, dy: number, buttons?: number, wheel?: number): void
      click(button: number): void
    }
    midi: {
      enable(): void
      bound(): boolean
      send(bytes: number[]): void
      startScan(): void
      stopScan(): void
      scanning(): boolean
      scanCount(): number
      scanNameAt(index: number): string
      connect(index: number): void
      disconnect(): void
    }
    hidHost: {
      startScan(): void
      stopScan(): void
      scanning(): boolean
      scanCount(): number
      scanNameAt(index: number): string
      connect(index: number): void
      disconnect(): void
      bound(): boolean
      reportCount(): number
      reportIdAt(index: number): number
      reportLenAt(index: number): number
      reportByteAt(index: number, byteIndex: number): number
      clearReports(): void
    }
    connections: {
      count(): number
      kindAt(index: number): number
      nameAt(index: number): string
    }
    config: {
      setDocument(bytes: number[]): void
      pendingLength(): number
      pendingByteAt(index: number): number
      consumePending(): void
      pairing(): boolean
      pairCode(): string
      dismissPairing(): void
      pushActivity(index: number): void
    }
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
}

declare const __gea_Accelerometer: {
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

declare const __gea_Camera: {
  readonly width: number
  readonly height: number
  readonly orientation: number
  readonly facing: string
  readonly deviceCount: number
  isAvailable(): boolean
  hasPermission(): boolean
  requestPermission(): boolean
  open(facing: string, width: number, height: number): boolean
  close(): void
  isOpen(): boolean
  draw(x: number, y: number, destWidth: number, destHeight: number): void
  capture(): number
  captureMirrored(): number
  captureFrame(): string
  startRecording(path: string, fps: number): boolean
  stopRecording(): number
  isRecording(): boolean
  setFlash(mode: string): void
  setZoom(factor: number): void
  setMirror(mirror: boolean): void
  setExposure(mode: string, bias: number, iso: number, durationMs: number): void
  setWhiteBalance(mode: string, temperature: number, tint: number): void
  setFocus(mode: string, pointX: number, pointY: number): void
  setTorch(mode: string, level: number): void
  deviceIdAt(index: number): string
  deviceFacingAt(index: number): string
}

// The audio surface is NATIVE. The geatsc gea plugin maps `AudioContext`,
// `AudioDestinationNode`, `AudioParam` and `OscillatorNode` to
// `gea::host::…` handles by NAME (host-shims.ts `nativeTypes`), so the
// `Embedded`-prefixed copies that used to live here matched nothing and were
// lowered as ordinary records instead. `AudioDestinationNode` is an EMPTY
// interface, and an empty record mints no shape at all — its field reached
// emission with no frozen representation and the whole build failed closed.
// Use the published names so the native mapping applies.
export type { AudioContext as EmbeddedAudioContext } from '../index'

declare const __gea_audioContext: AudioContext
declare const __gea_Audio: {
  getVolume(): number
  setVolume(volume: number): void
}

type EmbeddedDisplayOrientation = 'portrait-primary' | 'portrait-secondary' | 'landscape-primary' | 'landscape-secondary'

type EmbeddedDisplayOrientationSupport = EmbeddedDisplayOrientation | 'portrait' | 'landscape' | 'all'

type EmbeddedDisplayPixelFormat = 'rgb565' | 'rgb888' | 'rgb8888' | 'argb8888'

declare const __gea_Display: {
  readonly ctx: CanvasRenderingContext2D
  readonly width: number
  readonly height: number
  readonly nativeWidth: number
  readonly nativeHeight: number
  orientation: EmbeddedDisplayOrientation
  supportedOrientations: EmbeddedDisplayOrientationSupport[]
  autoRotate: boolean
  pixelFormat: EmbeddedDisplayPixelFormat
  readonly panelPixelFormat: EmbeddedDisplayPixelFormat
  readonly supportedPixelFormats: EmbeddedDisplayPixelFormat[]
  getBrightness(): number
  setBrightness(brightness: number): void
  getOrientation(): EmbeddedDisplayOrientation
  setOrientation(orientation: EmbeddedDisplayOrientation): void
  getSupportedOrientations(): EmbeddedDisplayOrientationSupport[]
  setSupportedOrientations(orientations: EmbeddedDisplayOrientationSupport | EmbeddedDisplayOrientationSupport[]): void
  getAutoRotate(): boolean
  setAutoRotate(enabled: boolean): void
  setVSync(on: boolean): void
  setTextRasterCache(on: boolean): void
  // Declare that text always draws over ONE solid color (0xRRGGBB). On packed
  // 4-bit grayscale targets (e-paper) each glyph is then pre-blended against
  // that backdrop once and stamped as straight byte copies -- no per-pixel
  // framebuffer read or blend. Glyphs whose color equals the backdrop (e.g.
  // inverted rows) automatically fall back to the true blend. Pass a negative
  // value to disable. No-op on full-color targets.
  //
  // Declared in `index.d.ts` and implemented by the engine
  // (`packages/host/include/host/display.h`, `packages/engine/canvas.cpp`)
  // since it was added; this surface -- the one a real build type-checks
  // against -- had been left out, so `e-reader`'s own `index.tsx` did not
  // compile against the framework it ships with.
  setTextSolidBackdrop(rrggbb: number): void
  // Mark the next present as a full-screen change ("damage-all"). The present
  // then skips the dirty-rect diff and its persistent previous-frame copy --
  // both exist only to flush less when little changed, which is pure waste
  // during a full-screen pan (the diff would conclude "everything changed"
  // and full-flush anyway). Call it once per frame while actively
  // panning/animating the whole screen; stop calling it when motion settles
  // so idle gets the cheap diff back.
  invalidate(): void
  getPixelFormat(): EmbeddedDisplayPixelFormat
  setPixelFormat(format: EmbeddedDisplayPixelFormat): void
  getPanelPixelFormat(): EmbeddedDisplayPixelFormat
  getSupportedPixelFormats(): EmbeddedDisplayPixelFormat[]
  getDevicePixelRatio(): number
  setDevicePixelRatio(devicePixelRatio: number): void
  getFrameIntervalMs(): number
  setFrameIntervalMs(intervalMs: number): void
  getFrameRate(): number
  setFrameRate(fps: number): void
  setAA(samples: number): void
  setFlushConfig(config: { rows: number; depth: number }): void
  setMemoryConfig(config: { commandBufferCommands?: number; backgroundCache?: boolean }): void
  // E-paper boards only (no-op elsewhere): refresh policy + custom waveforms.
  // LUTs are the panel's raw 159-byte waveform tables; [] restores the vendor
  // default. fastStreakWindowMs: 0 disables the fast waveform entirely.
  setEpaperRefreshConfig(config: {
    fullRefreshEveryPartials?: number
    fullRefreshHardCapPartials?: number
    fastStreakWindowMs?: number
    partialLut?: number[]
    fastLut?: number[]
    // 4-level grayscale on supported e-paper panels (SSD1681): full refresh,
    // 4 reflectance levels instead of 1-bit dither. Omit to leave unchanged.
    grayscale?: boolean
    // Whether a full-screen-covering partial (an e-reader page turn) is
    // promoted to a flashing full refresh. Omit to leave unchanged.
    fullOnCover?: boolean
  }): void
  // Force a full (flashing, anti-ghost) refresh of the current frame now.
  epaperFullRefresh(): void
}
declare const __gea_Memory: {
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
}
declare const __gea_Input: {
  // Reads-and-clears the one-shot back-button-pressed flag. Set by the
  // platform launcher-button task (BOOT short press on ESP32) and by any
  // application's `_settings_toggle()` stub when a `SettingsToggle` event
  // is dispatched. Apps that own UI overlays (Settings panel) poll this in
  // their rAF loop and dismiss the overlay if visible.
  consumeBackButton(): boolean
}
declare const __gea_Gpio: {
  // Digital pins, for the wiring a board exposes that no other facade covers:
  // an LED, a relay, a button that is not the launcher button. Each call
  // answers "did this board do it" — a pin the chip does not have reports
  // false rather than throwing.
  configureOutput(pin: number): boolean
  configureInput(pin: number, pullUp: boolean): boolean
  write(pin: number, level: boolean): boolean
  read(pin: number): boolean
}
declare const __gea_Led: {
  // A WS2812/NeoPixel strand on one pin — what "the onboard LED" is on most
  // modern ESP32 modules. set() is the one-call default (attach if needed,
  // write, transmit); off() is set() with black. show() returns once the strand
  // has latched the buffer, so a true means the LED is showing what was asked
  // for rather than that the write was queued.
  set(pin: number, r: number, g: number, b: number): boolean
  off(pin: number): boolean
  attach(pin: number, count: number): boolean
  setPixel(index: number, r: number, g: number, b: number): boolean
  show(): boolean
  detach(): void
}
declare const __gea_Clock: {
  setEpochMs(timestamp: number): boolean
  // Milliseconds since the Unix epoch from the platform real-time clock
  // (gettimeofday). Reflects a host-set time (GEADEV SETTIME); unlike
  // Date.now() — monotonic on this ESP32 build — it tracks real wall-clock
  // time once synced. Returns a small value before the clock is ever set.
  epochMs(): number
}
declare const __gea_Profiler: {
  // Monotonic microseconds from the platform high-resolution timer.
  nowUs(): number
  // Hardware cycle count on ESP32; nanoseconds on desktop hosts.
  nowCycles(): number
}
declare const __gea_Haptics: {
  vibrate(durationMs: number, strength: number): boolean
}
declare const __gea_Battery: {
  charging(): boolean
  // Battery charge percentage (0-100) from the platform PMU.
  level(): number
}
declare const __gea_Notify: {
  // Transient host->app notification channel. text() is the latest message;
  // seq() increments on each post so an app can detect a new one.
  text(): string
  seq(): number
}
declare const __gea_DeviceControl: {
  // Run a host shell command, return combined stdout+stderr (macOS companion).
  exec(command: string): string
}

// The `__gea_embedded_touch` global this used to reach for is registered by
// NOTHING in this repository — `touch.read()` has always returned the zero
// sample on every board. Reaching for it was not free, though: the
// `globalThis as EmbeddedHostGlobals` cast minted a file-scope record out of
// the entire global surface, and the hook's own `read()` is a callable member,
// which D0 record identity declines by design (a callable key is coarser than
// the physical ABI). The field therefore reached emission with no frozen
// representation authority and `widenValueClassFieldForFileScope` failed
// closed, taking every embedded build down with it.
//
// Keep the public surface (`export declare const touch: TouchHost`) and the
// observable behaviour exactly as they were. When a board does grow a raw
// touch source, give it a real ambient `declare const __gea_*` host object
// like every other one in this file rather than a globalThis intersection.

export const defaults = {}

export const touch = {
  read(): TouchSample {
    return { touching: false, x: 0, y: 0 }
  }
}

export const Apps = {
  launch(appId: string): number {
    return apps.launch(appId)
  }
}

// Case-variant aliases for host accessors. Keep these lazy: direct top-level
// `navigator.*` aliases survive bundling as bare reads even in Display-only
// apps, which bloats embedded canvas demos.
export const BLE: typeof navigator.bluetooth = {
  keyboard: {
    tap(hidCode: number): void {
      navigator.bluetooth.keyboard.tap(hidCode)
    },
    down(modifier: number, hidCode: number): void {
      navigator.bluetooth.keyboard.down(modifier, hidCode)
    },
    up(): void {
      navigator.bluetooth.keyboard.up()
    }
  },
  mouse: {
    move(dx: number, dy: number, buttons?: number, wheel?: number): void {
      navigator.bluetooth.mouse.move(dx, dy, buttons, wheel)
    },
    click(button: number): void {
      navigator.bluetooth.mouse.click(button)
    }
  },
  // BLE-MIDI (MIDI over GATT). `enable()` registers the MIDI service +
  // advertises its UUID; `send()` transmits ONE already-framed BLE-MIDI
  // packet (header + timestamp + MIDI bytes) as a notification.
  midi: {
    enable(): void {
      navigator.bluetooth.midi.enable()
    },
    bound(): boolean {
      return navigator.bluetooth.midi.bound()
    },
    send(bytes: number[]): void {
      navigator.bluetooth.midi.send(bytes)
    },
    // Central role (scan for WIDI adapters / BLE-MIDI pedals and connect as
    // a GATT client). Scan results follow the WiFi scan polling idiom.
    startScan(): void {
      navigator.bluetooth.midi.startScan()
    },
    stopScan(): void {
      navigator.bluetooth.midi.stopScan()
    },
    scanning(): boolean {
      return navigator.bluetooth.midi.scanning()
    },
    scanCount(): number {
      return navigator.bluetooth.midi.scanCount()
    },
    scanNameAt(index: number): string {
      return navigator.bluetooth.midi.scanNameAt(index)
    },
    connect(index: number): void {
      navigator.bluetooth.midi.connect(index)
    },
    disconnect(): void {
      navigator.bluetooth.midi.disconnect()
    }
  },
  // HID host role: this device is the central; a remote HID peripheral (a
  // keyboard/macro pad like the XPPen ACK05) is the input source. Raw input
  // reports queue driver-side; the app polls and decodes them itself.
  hidHost: {
    startScan(): void {
      navigator.bluetooth.hidHost.startScan()
    },
    stopScan(): void {
      navigator.bluetooth.hidHost.stopScan()
    },
    scanning(): boolean {
      return navigator.bluetooth.hidHost.scanning()
    },
    scanCount(): number {
      return navigator.bluetooth.hidHost.scanCount()
    },
    scanNameAt(index: number): string {
      return navigator.bluetooth.hidHost.scanNameAt(index)
    },
    connect(index: number): void {
      navigator.bluetooth.hidHost.connect(index)
    },
    disconnect(): void {
      navigator.bluetooth.hidHost.disconnect()
    },
    bound(): boolean {
      return navigator.bluetooth.hidHost.bound()
    },
    reportCount(): number {
      return navigator.bluetooth.hidHost.reportCount()
    },
    reportIdAt(index: number): number {
      return navigator.bluetooth.hidHost.reportIdAt(index)
    },
    reportLenAt(index: number): number {
      return navigator.bluetooth.hidHost.reportLenAt(index)
    },
    reportByteAt(index: number, byteIndex: number): number {
      return navigator.bluetooth.hidHost.reportByteAt(index, byteIndex)
    },
    clearReports(): void {
      navigator.bluetooth.hidHost.clearReports()
    }
  },
  // Live connection registry across every concurrent BLE role. kind: 0 =
  // HID central peer (desktop), 1 = MIDI central peer (DAW), 2 = HID
  // peripheral we host (macro pad), 3 = BLE-MIDI peripheral we drive
  // (pedal/WIDI adapter).
  connections: {
    count(): number {
      return navigator.bluetooth.connections.count()
    },
    kindAt(index: number): number {
      return navigator.bluetooth.connections.kindAt(index)
    },
    nameAt(index: number): string {
      return navigator.bluetooth.connections.nameAt(index)
    }
  },
  // Config service (custom GATT). A Web Bluetooth portal live-programs the
  // device: it reads the app's current config blob and, once paired with the
  // 4-digit code shown on-device, writes a new one. `setDocument` publishes
  // the blob the portal reads back; the app polls `pendingLength` each frame
  // and drains a committed inbound blob via `pendingByteAt`/`consumePending`.
  // The pairing trio drives the on-device pairing overlay.
  config: {
    setDocument(bytes: number[]): void {
      navigator.bluetooth.config.setDocument(bytes)
    },
    pendingLength(): number {
      return navigator.bluetooth.config.pendingLength()
    },
    pendingByteAt(index: number): number {
      return navigator.bluetooth.config.pendingByteAt(index)
    },
    consumePending(): void {
      navigator.bluetooth.config.consumePending()
    },
    pairing(): boolean {
      return navigator.bluetooth.config.pairing()
    },
    pairCode(): string {
      return navigator.bluetooth.config.pairCode()
    },
    dismissPairing(): void {
      navigator.bluetooth.config.dismissPairing()
    },
    // Device -> portal activity push: when a control fires on the device, tell
    // any subscribed portal which control (by index) lit up, so the portal can
    // highlight it live. No-op on the device when no portal is subscribed.
    pushActivity(index: number): void {
      navigator.bluetooth.config.pushActivity(index)
    }
  },
  enabled(): boolean {
    return navigator.bluetooth.enabled()
  },
  setEnabled(enabled: boolean): void {
    navigator.bluetooth.setEnabled(enabled)
  },
  connected(): boolean {
    return navigator.bluetooth.connected()
  },
  bound(): boolean {
    return navigator.bluetooth.bound()
  },
  batteryLevel(): number {
    return navigator.bluetooth.batteryLevel()
  },
  mac(): string {
    return navigator.bluetooth.mac()
  },
  deviceName(): string {
    return navigator.bluetooth.deviceName()
  },
  init(deviceName: string, appearance?: number, macAddress?: string): void {
    navigator.bluetooth.init(deviceName, appearance, macAddress)
  },
  startAdvertising(): void {
    navigator.bluetooth.startAdvertising()
  },
  stopAdvertising(): void {
    navigator.bluetooth.stopAdvertising()
  }
}
export const bluetooth = BLE
export const WiFi: typeof navigator.wifi = {
  accessPointMac(): string {
    return navigator.wifi.accessPointMac()
  },

  startCaptivePortal(): boolean {
    return navigator.wifi.startCaptivePortal()
  },

  stopCaptivePortal(): void {
    navigator.wifi.stopCaptivePortal()
  },

  startAccessPoint(ssid: string, password = ''): boolean {
    return navigator.wifi.startAccessPoint(ssid, password)
  },

  stopAccessPoint(): void {
    navigator.wifi.stopAccessPoint()
  },
  enabled(): boolean {
    return navigator.wifi.enabled()
  },
  setEnabled(enabled: boolean): void {
    navigator.wifi.setEnabled(enabled)
  },
  connected(): boolean {
    return navigator.wifi.connected()
  },
  rssi(): number {
    return navigator.wifi.rssi()
  },
  ssid(): string {
    return navigator.wifi.ssid()
  },
  ip(): string {
    return navigator.wifi.ip()
  },
  mac(): string {
    return navigator.wifi.mac()
  },
  configure(ssid: string, password: string): void {
    navigator.wifi.configure(ssid, password)
  },
  waitForConnection(timeoutMs: number): boolean {
    return navigator.wifi.waitForConnection(timeoutMs)
  },
  startScan(): void {
    navigator.wifi.startScan()
  },
  scanning(): boolean {
    return navigator.wifi.scanning()
  },
  scanCount(): number {
    return navigator.wifi.scanCount()
  },
  scanSsidAt(index: number): string {
    return navigator.wifi.scanSsidAt(index)
  },
  scanRssiAt(index: number): number {
    return navigator.wifi.scanRssiAt(index)
  },
  scanSecuredAt(index: number): boolean {
    return navigator.wifi.scanSecuredAt(index)
  }
}
export const wifi = WiFi

// Device-hosted HTTP server. On hardware, geatsc lowers `http.createServer(...)`
// to the native esp_http_server facade (packages/core/host/http.cpp); this JS
// fallback reports unsupported use because browsers cannot open a listening socket.
export type HttpIncomingMessage = IncomingMessage
export interface HttpServerInstance {
  listen(port: number): boolean
  close(): void
  id(): number
}
declare const __gea_http: HttpModule

export const http = {
  createServer(handler: (req: HttpIncomingMessage) => HttpReply): HttpServer {
    if (typeof __gea_http !== 'undefined') {
      return __gea_http.createServer(handler)
    }
    throw new Error('HTTP servers are unavailable in this browser runtime')
  },
  close(handle: number): void {
    if (typeof __gea_http !== 'undefined') __gea_http.close(handle)
  },
}
export const Geolocation: typeof navigator.geolocation = {
  hasFix(): boolean {
    return navigator.geolocation.hasFix()
  },
  currentPosition(): GeolocationPosition {
    return navigator.geolocation.currentPosition()
  },
  coords(): GeolocationCoordinates {
    return navigator.geolocation.coords()
  },
  latitude(): number {
    return navigator.geolocation.latitude()
  },
  longitude(): number {
    return navigator.geolocation.longitude()
  },
  accuracy(): number {
    return navigator.geolocation.accuracy()
  },
  getCurrentPosition(
    success: (position: GeolocationPosition) => void,
    error?: (error: GeolocationPositionError) => void,
    options?: PositionOptions
  ): void {
    navigator.geolocation.getCurrentPosition(success, error, options)
  },
  watchPosition(
    success: (position: GeolocationPosition) => void,
    error?: (error: GeolocationPositionError) => void,
    options?: PositionOptions
  ): number {
    return navigator.geolocation.watchPosition(success, error, options)
  },
  clearWatch(id: number): void {
    navigator.geolocation.clearWatch(id)
  }
}
export const geolocation = Geolocation
// `__gea_Accelerometer` is only injected by the vite plugin when an app actually
// uses the accelerometer (the imu-capability gate keys on `Accelerometer.` calls).
// Referencing it unconditionally here threw `ReferenceError: __gea_Accelerometer
// is not defined` at module load for every app that doesn't use it — blanking the
// whole page in the browser runtime. `typeof` on the bare identifier is safe
// whether or not the global was injected, and apps that do use it still get the
// real binding.
export const Accelerometer =
  typeof __gea_Accelerometer !== 'undefined' ? __gea_Accelerometer : (undefined as unknown as typeof __gea_Accelerometer)

// Remembers the most recent recording sink so stopRecording can report it on
// the resolved CameraClip (the host only returns a duration).
let cameraRecordingPath = ''

// Live preview + still-capture + recording + controls API. Same shape on every
// target that ships a Camera backend (esp32-p4, geaos; iOS follows). The
// `<camera>` JSX element provides the matching CSS-sized preview surface; the
// optional <CameraView /> wrapper lives in @geastack/elements.
export const Camera = {
  isAvailable(): boolean {
    return __gea_Camera.isAvailable()
  },
  hasPermission(): boolean {
    return __gea_Camera.hasPermission()
  },
  requestPermission(): Promise<boolean> {
    return Promise.resolve(__gea_Camera.requestPermission())
  },
  open(options?: CameraOpenOptions | CameraFacing): boolean {
    if (typeof options === 'string') return __gea_Camera.open(options, 0, 0)
    if (!options) return __gea_Camera.open('back', 0, 0)
    const facing = options.facing ?? 'back'
    const width = options.width ?? 0
    const height = options.height ?? 0
    return __gea_Camera.open(facing, width, height)
  },
  close(): void {
    __gea_Camera.close()
  },
  isOpen(): boolean {
    return __gea_Camera.isOpen()
  },
  get facing(): CameraFacing {
    return __gea_Camera.facing as CameraFacing
  },
  get width(): number {
    return __gea_Camera.width
  },
  get height(): number {
    return __gea_Camera.height
  },
  get orientation(): number {
    return __gea_Camera.orientation
  },
  draw(x: number, y: number, destWidth?: number, destHeight?: number): void {
    __gea_Camera.draw(x, y, destWidth ?? 0, destHeight ?? 0)
  },
  capture(options?: CameraCaptureOptions): Promise<CameraPhoto> {
    const id = options?.mirror ? __gea_Camera.captureMirrored() : __gea_Camera.capture()
    const photo: CameraPhoto = {
      imageId: id,
      width: __gea_Camera.width,
      height: __gea_Camera.height,
      orientation: __gea_Camera.orientation,
      dispose(): void {
        image.dispose(id)
      }
    }
    return Promise.resolve(photo)
  },
  capturePhoto(options?: CameraCaptureOptions): Promise<CameraPhoto> {
    return this.capture(options)
  },
  captureFrame(): string {
    return __gea_Camera.captureFrame()
  },
  startRecording(options: CameraRecordOptions): Promise<void> {
    cameraRecordingPath = options.path
    __gea_Camera.startRecording(options.path, options.fps ?? 0)
    return Promise.resolve()
  },
  stopRecording(): Promise<CameraClip> {
    // durationMs is -1 when no recording was active; callers can check it.
    const durationMs = __gea_Camera.stopRecording()
    const clip: CameraClip = { durationMs, path: cameraRecordingPath }
    return Promise.resolve(clip)
  },
  isRecording(): boolean {
    return __gea_Camera.isRecording()
  },
  setFlash(mode: CameraFlashMode): void {
    __gea_Camera.setFlash(mode)
  },
  setZoom(factor: number): void {
    __gea_Camera.setZoom(factor)
  },
  setMirror(mirror: boolean): void {
    __gea_Camera.setMirror(mirror)
  },
  setExposure(options: CameraExposureOptions): void {
    __gea_Camera.setExposure(options.mode ?? 'continuous', options.bias ?? 0, options.iso ?? 0, options.durationMs ?? 0)
  },
  setWhiteBalance(options: CameraWhiteBalanceOptions): void {
    __gea_Camera.setWhiteBalance(options.mode ?? 'auto', options.temperature ?? 0, options.tint ?? 0)
  },
  setFocus(options: CameraFocusOptions): void {
    __gea_Camera.setFocus(options.mode ?? 'continuous', options.point?.x ?? 0, options.point?.y ?? 0)
  },
  setTorch(mode: 'off' | 'on' | 'auto', level?: number): void {
    __gea_Camera.setTorch(mode, level ?? 0)
  },
  switchCamera(target: CameraFacing | string): boolean {
    if (this.isOpen()) __gea_Camera.close()
    return __gea_Camera.open(target, 0, 0)
  },
  getDevices(): CameraDevice[] {
    const count = __gea_Camera.deviceCount
    const out: CameraDevice[] = []
    for (let i = 0; i < count; i++) {
      out.push({
        id: __gea_Camera.deviceIdAt(i),
        facing: __gea_Camera.deviceFacingAt(i) as CameraFacing
      })
    }
    return out
  }
}

// Same guard as Accelerometer above: these embedded host globals are not injected
// in the browser runtime (which renders via real web APIs), so a bare top-level
// reference ReferenceErrors at module load and blanks the page. `typeof` keeps the
// real binding on embedded targets where the plugin injects them.
export const audioContext: AudioContext =
  typeof __gea_audioContext !== 'undefined' ? __gea_audioContext : (undefined as unknown as AudioContext)
// Method wrapper, NOT the `typeof … ? … : undefined as …` guard the
// property-bearing host objects above use. Two reasons. (1) The guard exists
// only to stop a bare top-level reference from ReferenceError-ing at module
// load in the browser runtime; a reference inside a method body is evaluated
// on call, so it needs no guard — which is why `Memory`/`Input`/`Clock` below
// are already written this way. (2) `typeof __gea_Audio` is an object whose
// every member is callable, and the conditional over it has no plannable
// representation ("coverage gap for ConditionalExpression"). The plugin cannot
// paper over that with a `hostNamespaces` entry either: the name `Audio` is
// already claimed by `embeddedHostClasses` for the DOM `new Audio()`
// constructor (gea::host::HTMLAudioElement).
export const Audio = {
  getVolume(): number {
    return __gea_Audio.getVolume()
  },
  setVolume(volume: number): void {
    __gea_Audio.setVolume(volume)
  }
}
export const Display = typeof __gea_Display !== 'undefined' ? __gea_Display : (undefined as unknown as typeof __gea_Display)
export const Memory = {
  internalFree(): number {
    return __gea_Memory.internalFree()
  },
  internalLargestFreeBlock(): number {
    return __gea_Memory.internalLargestFreeBlock()
  },
  internalMinimumFree(): number {
    return __gea_Memory.internalMinimumFree()
  },
  psramFree(): number {
    return __gea_Memory.psramFree()
  },
  currentTaskStackHighWaterMark(): number {
    return __gea_Memory.currentTaskStackHighWaterMark()
  },
  geaMainStackBytes(): number {
    return __gea_Memory.geaMainStackBytes()
  },
  geaInitStackBytes(): number {
    return __gea_Memory.geaInitStackBytes()
  },
  appFrameStackWords(): number {
    return __gea_Memory.appFrameStackWords()
  },
  appFrameStackBytes(): number {
    return __gea_Memory.appFrameStackBytes()
  },
  displayFlushConfiguredRows(): number {
    return __gea_Memory.displayFlushConfiguredRows()
  },
  displayFlushConfiguredDepth(): number {
    return __gea_Memory.displayFlushConfiguredDepth()
  },
  displayFlushBufferMaxBytes(): number {
    return __gea_Memory.displayFlushBufferMaxBytes()
  },
  displayFlushRows(): number {
    return __gea_Memory.displayFlushRows()
  },
  displayFlushDepth(): number {
    return __gea_Memory.displayFlushDepth()
  },
  displayFlushBufferBytes(): number {
    return __gea_Memory.displayFlushBufferBytes()
  },
  allocationSramCount(): number {
    return __gea_Memory.allocationSramCount()
  },
  allocationPsramCount(): number {
    return __gea_Memory.allocationPsramCount()
  },
  allocationSramBytes(): number {
    return __gea_Memory.allocationSramBytes()
  },
  allocationPsramBytes(): number {
    return __gea_Memory.allocationPsramBytes()
  },
  allocationSramPeakBytes(): number {
    return __gea_Memory.allocationSramPeakBytes()
  },
  allocationPsramPeakBytes(): number {
    return __gea_Memory.allocationPsramPeakBytes()
  },
  stats(): EmbeddedMemoryStats {
    return {
      internalFree: __gea_Memory.internalFree(),
      internalLargestFreeBlock: __gea_Memory.internalLargestFreeBlock(),
      internalMinimumFree: __gea_Memory.internalMinimumFree(),
      psramFree: __gea_Memory.psramFree(),
      currentTaskStackHighWaterMark: __gea_Memory.currentTaskStackHighWaterMark(),
      allocationSramCount: __gea_Memory.allocationSramCount(),
      allocationPsramCount: __gea_Memory.allocationPsramCount(),
      allocationSramBytes: __gea_Memory.allocationSramBytes(),
      allocationPsramBytes: __gea_Memory.allocationPsramBytes(),
      allocationSramPeakBytes: __gea_Memory.allocationSramPeakBytes(),
      allocationPsramPeakBytes: __gea_Memory.allocationPsramPeakBytes()
    }
  },
  config(): EmbeddedMemoryConfig {
    return {
      geaMainStackBytes: __gea_Memory.geaMainStackBytes(),
      geaInitStackBytes: __gea_Memory.geaInitStackBytes(),
      appFrameStackWords: __gea_Memory.appFrameStackWords(),
      appFrameStackBytes: __gea_Memory.appFrameStackBytes(),
      displayFlushConfiguredRows: __gea_Memory.displayFlushConfiguredRows(),
      displayFlushConfiguredDepth: __gea_Memory.displayFlushConfiguredDepth(),
      displayFlushBufferMaxBytes: __gea_Memory.displayFlushBufferMaxBytes(),
      displayFlushRows: __gea_Memory.displayFlushRows(),
      displayFlushDepth: __gea_Memory.displayFlushDepth(),
      displayFlushBufferBytes: __gea_Memory.displayFlushBufferBytes()
    }
  }
}
export const Input = {
  consumeBackButton(): boolean {
    return __gea_Input.consumeBackButton()
  }
}
export const Gpio = {
  configureOutput(pin: number): boolean {
    return __gea_Gpio.configureOutput(pin)
  },
  configureInput(pin: number, pullUp: boolean): boolean {
    return __gea_Gpio.configureInput(pin, pullUp)
  },
  write(pin: number, level: boolean): boolean {
    return __gea_Gpio.write(pin, level)
  },
  read(pin: number): boolean {
    return __gea_Gpio.read(pin)
  }
}
export const Led = {
  set(pin: number, r: number, g: number, b: number): boolean {
    return __gea_Led.set(pin, r, g, b)
  },
  off(pin: number): boolean {
    return __gea_Led.off(pin)
  },
  attach(pin: number, count: number): boolean {
    return __gea_Led.attach(pin, count)
  },
  setPixel(index: number, r: number, g: number, b: number): boolean {
    return __gea_Led.setPixel(index, r, g, b)
  },
  show(): boolean {
    return __gea_Led.show()
  },
  detach(): void {
    __gea_Led.detach()
  }
}
export const Clock = {
  setEpochMs(timestamp: number): boolean {
    return __gea_Clock.setEpochMs(timestamp)
  },

  epochMs(): number {
    return __gea_Clock.epochMs()
  },
}
// Same method-wrapper reasoning as `Audio` above: the host global is only
// touched when the method runs, and the conditional form has no plannable
// representation. The `Date.now()` fallback is preserved for the runtimes that
// do not inject the global.
export const Profiler = {
  nowUs(): number {
    if (typeof __gea_Profiler === 'undefined') return Date.now() * 1000
    return __gea_Profiler.nowUs()
  },

  nowCycles(): number {
    // Desktop native hosts also expose nanoseconds instead of CPU cycles.
    if (typeof __gea_Profiler === 'undefined') return Date.now() * 1000000
    return __gea_Profiler.nowCycles()
  },
}
export const Haptics = {
  vibrate(durationMs: number, strength = 100): boolean {
    return __gea_Haptics.vibrate(durationMs, strength)
  },
}
export const Battery = {
  charging(): boolean {
    return __gea_Battery.charging()
  },

  level(): number {
    return __gea_Battery.level()
  },
}
export const Notify = {
  text(): string {
    return __gea_Notify.text()
  },
  seq(): number {
    return __gea_Notify.seq()
  }
}
export const DeviceControl = {
  exec(command: string): string {
    return __gea_DeviceControl.exec(command)
  }
}
// Method wrapper for the same two reasons as `Audio`/`Profiler` above: a
// reference inside a method body cannot ReferenceError at module load, and the
// `typeof … ? … : undefined as …` conditional over an all-callable object type
// has no plannable representation.
export const imageHost = {
  loadBytes(bytes: Uint8Array): number {
    return image.loadBytes(bytes)
  },
  loadBytesOpaque(bytes: Uint8Array): number {
    return image.loadBytesOpaque(bytes)
  },
  width(id: number): number {
    return image.width(id)
  },
  height(id: number): number {
    return image.height(id)
  },
  frameCount(id: number): number {
    return image.frameCount(id)
  },
  isAnimated(id: number): boolean {
    return image.isAnimated(id)
  },
  setPlaying(id: number, playing: number): void {
    image.setPlaying(id, playing)
  },
  seek(id: number, frame: number): void {
    image.seek(id, frame)
  },
  dispose(id: number): void {
    image.dispose(id)
  }
}
