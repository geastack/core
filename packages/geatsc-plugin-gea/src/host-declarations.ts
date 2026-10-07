export const geaHostDeclarations = [
  '#ifndef GEA_HOST_DECLARED',
  '#define GEA_HOST_DECLARED 1',
  '#include "gea/embedded.h"',
  '#endif',
]
// The `nativeBench` global is backed by an app-owned native translation unit
// (examples/apps/gea-bench/native/native_bench.{h,cpp}), not by the core
// embedded runtime. Its declarations live in "native_bench.h", which the app's
// build wires onto the include path — pull it in wherever a native_bench.* call
// is emitted so the generated module sees the prototypes.
const geaNativeBenchDeclarations = [...geaHostDeclarations, '#include "native_bench.h"']
// App-owned EPUB ZIP/DEFLATE implementation. The e-reader contributes this
// header and translation unit through gea.nativeSources.
const geaEpubArchiveDeclarations = [...geaHostDeclarations, '#include "epub_archive.h"']
// App-owned device-info facade. The diagnostics app contributes this header and
// translation unit through gea.nativeSources (portable, per-platform backends).
const geaDeviceInfoDeclarations = [...geaHostDeclarations, '#include "device_info.h"']
// `<virtual-list>`'s measured row height is `VirtualListRenderer::rowHeight`,
// declared in the engine's own "ui/internal.h" rather than re-exported by
// "gea/embedded.h" -- the same header this plugin's DOM runtime
// (host_document_gea.cpp) already includes to read it. The scroll offsets
// beside it need nothing extra: `Tree::instance().scrollTop` IS reachable
// through the public header.
const geaVirtualListDeclarations = [...geaHostDeclarations, '#include "ui/internal.h"']
const geaBase64Declarations = [...geaHostDeclarations, '#include "gea/base64-runtime.h"']

/**
 * The `<virtual-list>` row-height getter's template, stated once.
 *
 * A preamble is keyed by the very text of the member template it belongs to,
 * so the row and its include have to spell that text identically. Exporting
 * the string and using it in both places is what makes that true by
 * construction instead of by matching two literals by eye.
 */
export const geaVirtualListRowHeightEmit =
  'static_cast<double>(gea::embedded::ui::VirtualListRenderer::rowHeight(({receiver}).id()))'
export const geaHostHeaderDeclarationToken = '__gea_cpp_api_header'
const geaImageHostMethods = [
  'removeFile',
  'loadBytes',
  'loadBytesOpaque',
  'draw',
  'width',
  'height',
  'frameCount',
  'isAnimated',
  'setPlaying',
  'seek',
  'advance',
  'dispose',
]
const geaTouchHostMethods = ['read']
const geaGeolocationHostMethods = [
  'hasFix',
  'currentPosition',
  'coords',
  'latitude',
  'longitude',
  'accuracy',
  'getCurrentPosition',
  'watchPosition',
  'clearWatch',
]
const geaInputHostMethods = ['consumeBackButton']
const geaGpioHostMethods = ['configureOutput', 'configureInput', 'write', 'read']
const geaLedHostMethods = ['set', 'off', 'attach', 'setPixel', 'show', 'detach']
const geaClockHostMethods = ['epochMs', 'setEpochMs']
const geaProfilerHostMethods = ['nowUs', 'nowCycles']
const geaStorageHostMethods = ['getItem', 'setItem', 'removeItem', 'clear', 'key']
const geaBatteryHostMethods = ['level', 'charging']
const geaNotifyHostMethods = ['text', 'seq']
const geaDeviceControlHostMethods = ['exec']
const geaNativeBenchHostMethods = [
  'loopOverhead', 'arrayRead', 'arrayWrite', 'closure', 'methodCalls', 'mathIntensive',
  'modulo', 'factorial', 'fibonacci', 'nestedLoops', 'matrixMultiply', 'mandelbrot',
  'binaryTrees', 'primeSieve', 'stringConcat', 'objectCreate', 'jsonStringify', 'jsonParse',
  'ttfFontReady', 'ttfFontBytes', 'ttfCMalloc64', 'ttfScaleForSize', 'ttfGlyphIndex',
  'ttfGlyphAdvance', 'ttfGlyphWidth', 'ttfGlyphHeight', 'ttfShapeCount', 'ttfShapeChecksum',
  'ttfFlattenGlyph', 'ttfRasterEmptyStatic', 'ttfManualBoxStatic', 'ttfSortEdgesStatic',
  'ttfScanEdgesStatic', 'ttfRasterBoxSimple', 'ttfRasterBoxStatic', 'ttfRasterGlyphStatic',
  'ttfSpecimenWidth', 'ttfSpecimenHeight', 'ttfSpecimenReady', 'ttfSpecimenColumnBits',
  'ttfColdGlyph', 'ttfColdRun', 'ttfCachedRun',
]
const geaEpubArchiveHostMethods = ['inflateRaw', 'parse', 'meta', 'chapter', 'readTextFile', 'writeTextFile', 'coverBytes', 'cacheCover', 'thumbnailCover', 'imageBytes']
const geaDeviceInfoHostMethods = ['deviceId', 'platform', 'chipModel', 'chipArch', 'chipRevision', 'cores', 'cpuMhz', 'flashSizeBytes', 'ramSizeBytes', 'uniqueId', 'osVersion', 'resetReason', 'uptimeMs', 'dieTemperatureC']
// The `gea3dNative` global is backed by the gea3d library's native engine
// (gea3d/src/native/gea3d_native.{h,cpp}, vendored into apps under
// src/gea3d/native/ and wired via the app's `gea.nativeSources`).
const geaGea3dHostMethods = [
  'createBuffer', 'bufferDataF32', 'bufferDataU32', 'beginFrame',
  'ambientLight', 'directionalLight', 'drawElements', 'endFrame', 'stat',
  // scene graph
  'deleteBuffer', 'createGeometry', 'deleteGeometry', 'geometryBuffers',
  'createMaterial', 'deleteMaterial', 'materialSet', 'createNode',
  'deleteNode', 'nodeParent', 'nodeTransform', 'nodeMatrix', 'nodeFlags',
  'nodeGeometry', 'nodeInstances', 'nodeLight', 'nodeLightTarget',
  'cameraProjection', 'sceneEnvironment', 'renderOptions', 'renderTarget',
  'createSurface', 'drawScene', 'raycast', 'rayResult',
]
const geaGea3dDeclarations = [...geaHostDeclarations, '#include "gea3d_native.h"']
const geaMemoryHostMethods = [
  'internalFree',
  'internalLargestFreeBlock',
  'internalMinimumFree',
  'psramFree',
  'currentTaskStackHighWaterMark',
  'geaMainStackBytes',
  'geaInitStackBytes',
  'appFrameStackWords',
  'appFrameStackBytes',
  'displayFlushConfiguredRows',
  'displayFlushConfiguredDepth',
  'displayFlushBufferMaxBytes',
  'displayFlushRows',
  'displayFlushDepth',
  'displayFlushBufferBytes',
  'allocationSramCount',
  'allocationPsramCount',
  'allocationSramBytes',
  'allocationPsramBytes',
  'allocationSramPeakBytes',
  'allocationPsramPeakBytes',
]

const rawGeaHostExternDeclarations: Record<string, string[]> = Object.fromEntries([
      ['gea::runtime::hostbase64::encode', geaBase64Declarations],
      ['gea::runtime::hostbase64::decode', geaBase64Declarations],
      [geaHostHeaderDeclarationToken, geaHostDeclarations],
      ['gea::host::navigator', geaHostDeclarations],
      ['gea::host::window', geaHostDeclarations],
      // The `localStorage` global resolves to this facade object; the bare-object
      // decl ensures `localStorage.length` (a member read) pulls in the header.
      ['gea::host::Storage', geaHostDeclarations],
      ['gea::host::window.innerWidth', geaHostDeclarations],
      ['gea::host::window.innerHeight', geaHostDeclarations],
      ['gea::host::navigator.userAgent', geaHostDeclarations],
      ['gea::host::navigator.language', geaHostDeclarations],
      ['gea::host::navigator.platform', geaHostDeclarations],
      ['gea::host::navigator.onLine', geaHostDeclarations],
      ...geaImageHostMethods.map((method): [string, string[]] => [`gea::host::image.${method}`, geaHostDeclarations]),
      ...geaTouchHostMethods.map((method): [string, string[]] => [`gea::host::touch.${method}`, geaHostDeclarations]),
      ...geaMemoryHostMethods.map((method): [string, string[]] => [`gea::host::Memory.${method}`, geaHostDeclarations]),
      ...geaInputHostMethods.map((method): [string, string[]] => [`gea::host::Input.${method}`, geaHostDeclarations]),
      ...geaGpioHostMethods.map((method): [string, string[]] => [`gea::host::Gpio.${method}`, geaHostDeclarations]),
      ...geaLedHostMethods.map((method): [string, string[]] => [`gea::host::Led.${method}`, geaHostDeclarations]),
      ...geaClockHostMethods.map((method): [string, string[]] => [`gea::host::Clock.${method}`, geaHostDeclarations]),
      ...geaProfilerHostMethods.map((method): [string, string[]] => [`gea::host::Profiler.${method}`, geaHostDeclarations]),
      ...geaStorageHostMethods.map((method): [string, string[]] => [`gea::host::Storage.${method}`, geaHostDeclarations]),
      ...geaBatteryHostMethods.map((method): [string, string[]] => [`gea::host::Battery.${method}`, geaHostDeclarations]),
      ...geaNotifyHostMethods.map((method): [string, string[]] => [`gea::host::Notify.${method}`, geaHostDeclarations]),
      ...geaDeviceControlHostMethods.map((method): [string, string[]] => [`gea::host::DeviceControl.${method}`, geaHostDeclarations]),
      ...geaNativeBenchHostMethods.map((method): [string, string[]] => [`gea::host::native_bench::${method}`, geaNativeBenchDeclarations]),
      ...geaEpubArchiveHostMethods.map((method): [string, string[]] => [`gea::host::epub_archive::${method}`, geaEpubArchiveDeclarations]),
      ...geaDeviceInfoHostMethods.map((method): [string, string[]] => [`gea::host::device_info::${method}`, geaDeviceInfoDeclarations]),
      ...geaGea3dHostMethods.map((method): [string, string[]] => [`gea::host::gea3d::${method}`, geaGea3dDeclarations]),

      [geaVirtualListRowHeightEmit, geaVirtualListDeclarations],

      ['gea::host::requestAnimationFrame', geaHostDeclarations],
      ['gea::host::fetch', geaHostDeclarations],
      ['gea::host::fetchAsync', geaHostDeclarations],
      ['gea::host::fetchUploadFileAsync', geaHostDeclarations],
      ['gea::host::fetchDownloadFileAsync', geaHostDeclarations],
      ['gea::host::fetchUploadProgress', geaHostDeclarations],
      ['gea::host::fetchUploadSent', geaHostDeclarations],
      ['gea::host::fetchUploadTotal', geaHostDeclarations],
      ['gea::host::fetchReady', geaHostDeclarations],
      ['gea::host::fetchResult', geaHostDeclarations],
      ['gea::host::fetchRelease', geaHostDeclarations],
      ['gea::host::websocket::create_handle', geaHostDeclarations],
      ['gea::host::WebSocket', geaHostDeclarations],
      ['gea::host::http::create_typed_server', [...geaHostDeclarations, '#include "gea/http-native-runtime.h"']],
      ['gea::host::Haptics.vibrate', geaHostDeclarations],
      ['gea::host::HttpServer', geaHostDeclarations],
      ['gea::host::media::get_user_media_audio', geaHostDeclarations],
      ...geaGeolocationHostMethods.map((method): [string, string[]] => [`gea::host::navigator.geolocation.${method}`, geaHostDeclarations]),
      ['gea::host::MediaStream', geaHostDeclarations],
      ['gea::host::MediaStreamTrack', geaHostDeclarations],
      ['gea::host::MediaRecorder', geaHostDeclarations],
      ['gea::host::MediaRecorderDataEvent', geaHostDeclarations],
      ['gea::host::GeaAudioBlob', geaHostDeclarations],
      ['gea::host::AudioContext', geaHostDeclarations],
      ['gea::host::AudioDestinationNode', geaHostDeclarations],
      ['gea::host::AudioBuffer', geaHostDeclarations],
      ['gea::host::AudioBufferSourceNode', geaHostDeclarations],
      ['gea::host::AudioParam', geaHostDeclarations],
      ['gea::host::OscillatorNode', geaHostDeclarations],
      ['gea::host::rtc::create_handle', geaHostDeclarations],
      ['gea::host::RTCPeerConnection', geaHostDeclarations],
      ['gea::host::Math.random', geaHostDeclarations],
      ['gea::host::HTMLAudioElement', geaHostDeclarations],
      ['gea::host::PcmAudioStream', geaHostDeclarations],
      ['gea::host::HTMLVideoElement', geaHostDeclarations],
      ['gea::host::sharedAudioContext()', geaHostDeclarations],
      ['gea::host::sharedAudioContext().createOscillator', geaHostDeclarations],
      ['gea::host::sharedAudioContext().createBufferSource', geaHostDeclarations],
      ['gea::host::sharedAudioContext().createBuffer', geaHostDeclarations],
      ['gea::host::sharedAudioContext().decodeAudioData', geaHostDeclarations],
      ['gea::host::sharedAudioContext().currentTime', geaHostDeclarations],
      ['gea::host::sharedAudioContext().destination', geaHostDeclarations],
      ['gea::host::apps.launch', geaHostDeclarations],
      ['gea::host::Audio.getVolume', geaHostDeclarations],
      ['gea::host::Audio.setVolume', geaHostDeclarations],
      ['gea::host::Display.ctx', geaHostDeclarations],
      ['gea::host::Display.width', geaHostDeclarations],
      ['gea::host::Display.height', geaHostDeclarations],
      ['gea::host::Display.nativeWidth', geaHostDeclarations],
      ['gea::host::Display.nativeHeight', geaHostDeclarations],
      ['gea::host::Display.getBrightness', geaHostDeclarations],
      ['gea::host::Display.setBrightness', geaHostDeclarations],
      ['gea::host::Display.getDevicePixelRatio', geaHostDeclarations],
      ['gea::host::Display.setDevicePixelRatio', geaHostDeclarations],
      ['gea::host::Display.getFrameIntervalMs', geaHostDeclarations],
      ['gea::host::Display.setFrameIntervalMs', geaHostDeclarations],
      ['gea::host::Display.getFrameRate', geaHostDeclarations],
      ['gea::host::Display.setFrameRate', geaHostDeclarations],
      ['gea::host::Display.orientation', geaHostDeclarations],
      ['gea::host::Display.getOrientation', geaHostDeclarations],
      ['gea::host::Display.setOrientation', geaHostDeclarations],
      ['gea::host::Display.supportedOrientations', geaHostDeclarations],
      ['gea::host::Display.getSupportedOrientations', geaHostDeclarations],
      ['gea::host::Display.setSupportedOrientations', geaHostDeclarations],
      ['gea::host::Display.autoRotate', geaHostDeclarations],
      ['gea::host::Display.getAutoRotate', geaHostDeclarations],
      ['gea::host::Display.setAutoRotate', geaHostDeclarations],
      ['gea::host::Display.setVSync', geaHostDeclarations],
      ['gea::host::Display.setTextRasterCache', geaHostDeclarations],
      ['gea::host::Display.invalidate', geaHostDeclarations],
      ['gea::host::Display.pixelFormat', geaHostDeclarations],
      ['gea::host::Display.getPixelFormat', geaHostDeclarations],
      ['gea::host::Display.setPixelFormat', geaHostDeclarations],
      ['gea::host::Display.panelPixelFormat', geaHostDeclarations],
      ['gea::host::Display.getPanelPixelFormat', geaHostDeclarations],
      ['gea::host::Display.supportedPixelFormats', geaHostDeclarations],
      ['gea::host::Display.getSupportedPixelFormats', geaHostDeclarations],
      ['gea::host::Display.setAA', geaHostDeclarations],
      ['gea::host::Display.setFlushConfig', geaHostDeclarations],
      ['gea::host::Display.setMemoryConfig', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.init', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.enabled', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.setEnabled', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.startAdvertising', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.stopAdvertising', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.connected', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.bound', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.batteryLevel', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.mac', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.deviceName', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.keyboard.tap', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.keyboard.down', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.keyboard.up', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.mouse.move', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.mouse.click', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.enable', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.bound', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.send', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.startScan', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.stopScan', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.scanning', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.scanCount', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.scanNameAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.connect', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.midi.disconnect', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.startScan', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.stopScan', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.scanning', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.scanCount', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.scanNameAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.connect', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.disconnect', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.bound', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.reportCount', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.reportIdAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.reportLenAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.reportByteAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.hidHost.clearReports', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.connections.count', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.connections.kindAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.connections.nameAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.setDocument', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.pendingLength', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.pendingByteAt', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.consumePending', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.pairing', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.pairCode', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.dismissPairing', geaHostDeclarations],
      ['gea::host::navigator.bluetooth.config.pushActivity', geaHostDeclarations],
      ['gea::host::navigator.wifi.enabled', geaHostDeclarations],
      ['gea::host::navigator.wifi.setEnabled', geaHostDeclarations],
      ['gea::host::navigator.wifi.connected', geaHostDeclarations],
      ['gea::host::navigator.wifi.rssi', geaHostDeclarations],
      ['gea::host::navigator.wifi.ssid', geaHostDeclarations],
      ['gea::host::navigator.wifi.ip', geaHostDeclarations],
      ['gea::host::navigator.wifi.mac', geaHostDeclarations],
      ['gea::host::navigator.wifi.configure', geaHostDeclarations],
      ['gea::host::navigator.wifi.waitForConnection', geaHostDeclarations],
      ['gea::host::navigator.wifi.startScan', geaHostDeclarations],
      ['gea::host::navigator.wifi.scanning', geaHostDeclarations],
      ['gea::host::navigator.wifi.scanCount', geaHostDeclarations],
      ['gea::host::navigator.wifi.network', geaHostDeclarations],
      ['gea::host::navigator.wifi.scanSsidAt', geaHostDeclarations],
      ['gea::host::navigator.wifi.scanRssiAt', geaHostDeclarations],
      ['gea::host::navigator.wifi.scanSecuredAt', geaHostDeclarations],
      ['gea::host::Accelerometer.start', geaHostDeclarations],
      ['gea::host::Accelerometer.init', geaHostDeclarations],
      ['gea::host::Accelerometer.close', geaHostDeclarations],
      ['gea::host::Accelerometer.calibrateBias', geaHostDeclarations],
      ['gea::host::Accelerometer.tiltX', geaHostDeclarations],
      ['gea::host::Accelerometer.tiltY', geaHostDeclarations],
      ['gea::host::Accelerometer.x', geaHostDeclarations],
      ['gea::host::Accelerometer.y', geaHostDeclarations],
      ['gea::host::Accelerometer.z', geaHostDeclarations],
      ['gea::host::Accelerometer.accelerationX', geaHostDeclarations],
      ['gea::host::Accelerometer.accelerationY', geaHostDeclarations],
      ['gea::host::Accelerometer.accelerationZ', geaHostDeclarations],
      ['gea::host::Accelerometer.gyroscopeX', geaHostDeclarations],
      ['gea::host::Accelerometer.gyroscopeY', geaHostDeclarations],
      ['gea::host::Accelerometer.gyroscopeZ', geaHostDeclarations],
      ['gea::host::Camera.isAvailable', geaHostDeclarations],
      ['gea::host::Camera.hasPermission', geaHostDeclarations],
      ['gea::host::Camera.requestPermission', geaHostDeclarations],
      ['gea::host::Camera.open', geaHostDeclarations],
      ['gea::host::Camera.close', geaHostDeclarations],
      ['gea::host::Camera.isOpen', geaHostDeclarations],
      ['gea::host::Camera.width', geaHostDeclarations],
      ['gea::host::Camera.height', geaHostDeclarations],
      ['gea::host::Camera.orientation', geaHostDeclarations],
      ['gea::host::Camera.facing', geaHostDeclarations],
      ['gea::host::Camera.deviceCount', geaHostDeclarations],
      ['gea::host::Camera.deviceIdAt', geaHostDeclarations],
      ['gea::host::Camera.deviceFacingAt', geaHostDeclarations],
      ['gea::host::Camera.draw', geaHostDeclarations],
      ['gea::host::Camera.capture', geaHostDeclarations],
      ['gea::host::Camera.captureMirrored', geaHostDeclarations],
      ['gea::host::Camera.captureFrame', geaHostDeclarations],
      ['gea::host::Camera.setFlash', geaHostDeclarations],
      ['gea::host::Camera.setZoom', geaHostDeclarations],
      ['gea::host::Camera.setMirror', geaHostDeclarations]

])

export const geaHostExternDeclarations: Record<string, string[]> = rawGeaHostExternDeclarations
