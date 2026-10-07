import type { GeaEmbeddedImage } from '../index'

export type LoadImageOptions = {
  opaque?: boolean
}

declare const image: {
  loadBytes(bytes: Uint8Array): number
  loadBytesOpaque(bytes: Uint8Array): number
  // Read + decode an image directly from a persistent-cache file (e.g. microSD).
  // Returns a slot id, or -1 when the file is missing/undecodable.
  loadFile(path: string): number
  loadFileOpaque(path: string): number
  // Decode a BUILD-EMBEDDED asset by its path relative to the app directory
  // (e.g. 'assets/icons/00-music.png'). Returns a slot id, or -1 when no such
  // asset was bundled.
  loadAssetPath(path: string): number
  // Save raw bytes to a persistent-cache file (creating parent dirs). Returns
  // true on success, false when no persistent storage is available.
  writeFile(path: string, bytes: Uint8Array): boolean
  // Read a whole file's raw bytes (e.g. a .pmtiles on microSD). Empty when the
  // file is missing or no storage is mounted.
  removeFile(path: string): boolean
  readFile(path: string): Uint8Array
  // Read a .pmtiles archive flashed into the spare ota_1 partition. Empty when
  // none present (or off-device).
  readMapArchive(): Uint8Array
  // Read a byte range [offset, offset+length) from a file without loading it
  // all. Empty when absent / no storage.
  readFileRange(path: string, offset: number, length: number): Uint8Array
  // List a directory's regular-file names (non-recursive), newline-joined.
  // Empty string when the directory is missing or no storage is mounted.
  listFiles(path: string): string
  // Synchronous HTTP GET over WiFi.
  fetchBytes(url: string): Uint8Array
  fetchText(url: string): string
  // Wrap a decoded slot id in the native `GeaEmbeddedImage` handle, snapshotting
  // its immutable dimensions. Lowering this through the host keeps the returned
  // value a concrete struct (no boxed record), so callers and any
  // `GeaEmbeddedImage[]` cache stay fully typed.
  make(id: number): GeaEmbeddedImage
}

export function loadImage(src: Uint8Array, options?: LoadImageOptions): GeaEmbeddedImage {
  const opaque = options ? options.opaque || false : false
  return loadImageWithOpaque(src, opaque)
}

// Scalar fast path for embedded callers whose opacity is already known. This
// keeps the hot image decode call native without constructing an options
// record at the C++ boundary.
export function loadImageWithOpaque(src: Uint8Array, opaque: boolean): GeaEmbeddedImage {
  const id = opaque ? image.loadBytesOpaque(src) : image.loadBytes(src)
  return image.make(id)
}

// Load + decode an image from a persistent-cache file path (e.g. an SD-card tile
// cache). The returned handle has width === 0 when the file is absent or fails
// to decode, so callers treat that as a cache miss and fetch over the network.
export function loadImageFile(path: string, options?: LoadImageOptions): GeaEmbeddedImage {
  const opaque = options ? options.opaque || false : false
  const id = opaque ? image.loadFileOpaque(path) : image.loadFile(path)
  return image.make(id)
}

// Load + decode a BUILD-EMBEDDED asset by its path relative to the app
// directory (e.g. 'assets/icons/00-music.png'), the same bytes `<img src>`
// would use. The JSX path lowers a string LITERAL straight to the generated
// `gea_asset_<path>` symbol, which is unavailable to a canvas app that picks
// among N bundled images at runtime -- this resolves the path through the
// generated lookup table instead. Decoding is deferred to first draw and
// memoized per asset, so calling it repeatedly for the same path is free.
// The returned handle has `width === 0` when no such asset was bundled.
export function loadAssetImage(path: string): GeaEmbeddedImage {
  // Two statements, not a nested call: the host-object lowering only qualifies
  // the OUTERMOST `image.<method>` receiver, so `image.make(image.loadX(...))`
  // emits an unqualified inner `image` and fails to compile.
  const id = image.loadAssetPath(path)
  return image.make(id)
}

// Persist raw bytes to the file cache (e.g. /sdcard/...). Returns false when no
// writable storage is mounted, so callers can ignore the result.
export function writeCacheFile(path: string, bytes: Uint8Array): boolean {
  return image.writeFile(path, bytes)
}

// Read a whole file's raw bytes from the persistent cache (e.g. a .pmtiles
// pushed to /sdcard). Returns an empty Uint8Array when absent / no storage.
export function readCacheFile(path: string): Uint8Array {
  return image.readFile(path)
}

// Read a .pmtiles archive flashed into the spare ota_1 partition over USB
// (esptool) — a reliable data path independent of the SD card. Empty when none.
export function readMapArchive(): Uint8Array {
  return image.readMapArchive()
}

// Read a byte range from a persistent-cache file (e.g. one tile out of a
// .pmtiles on microSD) without loading the whole file.
export function readFileRange(path: string, offset: number, length: number): Uint8Array {
  return image.readFileRange(path, offset, length)
}

// List a directory's regular-file names (non-recursive) from persistent storage
// (e.g. the EPUBs in /sdcard/books). Empty array when the directory is missing
// or no storage is mounted.
export function listCacheFiles(path: string): string[] {
  const joined = image.listFiles(path)
  if (joined.length == 0) return []
  return joined.split('\n')
}

// Synchronous HTTP GET over WiFi — body as bytes (e.g. an MVT tile) or text
// (e.g. a TileJSON). Empty when WiFi is down / the request fails.
export function fetchBytes(url: string): Uint8Array {
  return image.fetchBytes(url)
}

export function fetchText(url: string): string {
  return image.fetchText(url)
}

// Wrap an already-decoded image-store slot id (e.g. one delivered by the async
// tile loader) in the native GeaEmbeddedImage handle. width === 0 when the id
// is invalid.
export function imageFromId(id: number): GeaEmbeddedImage {
  return image.make(id)
}

export function removeCacheFile(path: string): boolean {
  return image.removeFile(path)
}
