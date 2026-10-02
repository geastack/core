// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gea::host {

// Typed handle for a decoded image slot. This is the native lowering of the TS
// `GeaEmbeddedImage` (see runtime/images.ts + index.d.ts) — `loadImage()` returns
// one of these by value, so cached images flow as a concrete struct (id + the
// immutable post-decode dimensions) instead of a boxed `gea_cpp_value` record.
// `img.width`/`img.height`/… are plain field reads; `img.play()`/`dispose()`/…
// delegate to the image store. `canvasImageId` reads the `id` member, so
// `ctx.drawImage(img, …)` consumes it directly.
struct GeaEmbeddedImage {
  int id = -1;
  int width = 0;
  int height = 0;
  int frameCount = 0;
  bool isAnimated = false;

  bool decode() const;
  void play() const;
  void pause() const;
  void seek(int frame) const;
  void dispose() const;

  // The absence state this handle already documents, made readable. `id = -1`
  // is what every failing producer returns -- `loadBytes`/`loadFile`/
  // `loadAssetPath` all answer -1, and `image.make(-1)` is what the TS runtime
  // hands back -- so the struct has always had a "refers to nothing" state; it
  // simply had no way to be asked. Every other gea handle answers this the same
  // way (`gea::NativeHandle` over its `valid()`, the Apple bridge's wrappers
  // over `handle != 0`), which is what lets a compiler carry `GeaEmbeddedImage |
  // null` as the handle itself instead of wrapping a second absence flag around
  // one that is already there. Explicit, so it never participates in arithmetic
  // or overload resolution -- only in a test that asks the question directly.
  explicit operator bool() const { return id >= 0; }
};

class ImageService {
public:
  double loadBytes(std::vector<std::uint8_t> bytes) const;
  double loadBytesOpaque(std::vector<std::uint8_t> bytes) const;

  // Decode a build-embedded asset from a pointer/length into its `.rodata`
  // bytes. `<img src="face.png">` lowers to a direct reference to the generated
  // `gea_asset_<path>` symbol (see generate-gea-embedded-assets.mjs) passed
  // here — that reference is what keeps the asset object in the link. Returns
  // the image id, or -1 on decode failure. Decoded once per pointer and cached,
  // so a remounting <img> reuses the same slot instead of re-decoding.
  double loadBytesPtr(const std::uint8_t *data, std::size_t length) const;
  double loadAssetPath(const char *path) const;
  // Same bundled-asset lookup for a path a canvas app computes at runtime --
  // `<img src="...">` lowers a string LITERAL to a direct gea_asset_ symbol,
  // which a ctx.drawImage() app selecting among N bundled icons cannot use.
  // Inline and delegating, so the device host and the native test host each
  // pick it up from their own loadAssetPath(const char *) definition.
  double loadAssetPath(const std::string &path) const { return loadAssetPath(path.c_str()); }

  // Persistent file cache (e.g. microSD at /sdcard). loadFile/loadFileOpaque read
  // + decode an image straight from a file, returning the slot id or -1 if the
  // file is missing / undecodable (callers treat -1 as a cache miss and fetch
  // over the network). writeFile saves raw bytes (creating parent dirs).
  // All no-op when no persistent storage is mounted.
  double loadFile(const std::string &path) const;
  double loadFileOpaque(const std::string &path) const;
  bool writeFile(const std::string &path, const std::vector<std::uint8_t> &bytes) const;
  // Read a whole file's raw bytes (e.g. a .pmtiles on microSD) into memory.
  // Empty vector when the file is missing or no storage is mounted.
  std::vector<std::uint8_t> readFile(const std::string &path) const;
  // Read a .pmtiles archive flashed into the spare ota_1 partition (esptool),
  // sized from its header. Empty off-esp32 or when absent/invalid.
  std::vector<std::uint8_t> readMapArchive() const;
  // Read a byte range [offset, offset+length) from a file (e.g. one tile out of
  // a .pmtiles on microSD), without loading the whole file.
  std::vector<std::uint8_t> readFileRange(const std::string &path, double offset, double length) const;
  // List a directory's regular-file names (non-recursive), newline-joined —
  // e.g. the EPUBs on a microSD. Empty string when the directory is missing or
  // no storage is mounted.
  std::string listFiles(const std::string &path) const;
  // Synchronous HTTP GET over WiFi: fetch a URL's body as bytes (e.g. an MVT
  // tile) or text (e.g. a TileJSON). Empty when WiFi is down / request fails.
  std::vector<std::uint8_t> fetchBytes(const std::string &url) const;
  std::string fetchText(const std::string &url) const;

#if defined(GEA_CPP_VALUE_AVAILABLE) && defined(GEA_HOST_IMAGE_BOXED_BYTE_OVERLOADS) && GEA_HOST_IMAGE_BOXED_BYTE_OVERLOADS
  double loadBytes(const gea_cpp_value &bytes) const;
  double loadBytesOpaque(const gea_cpp_value &bytes) const;
  bool writeFile(const std::string &path, const gea_cpp_value &bytes) const;
#endif

  void draw(double id, double x, double y) const;
  double width(double id) const;
  double height(double id) const;
  double frameCount(double id) const;
  bool isAnimated(double id) const;
  void setPlaying(double id, double playing) const;
  void seek(double id, double frame) const;
  bool advance(double id, double deltaMs) const;
  void dispose(double id) const;

  // Wrap an already-decoded slot id in a typed handle, snapshotting the
  // immutable dimensions. `loadImage()` (runtime/images.ts) calls this so its
  // result is a native `GeaEmbeddedImage` value rather than a boxed record.
  GeaEmbeddedImage make(double id) const;
};

inline constexpr ImageService image{};

#if defined(GEA_CPP_VALUE_AVAILABLE) && defined(GEA_HOST_IMAGE_BOXED_BYTE_OVERLOADS) && GEA_HOST_IMAGE_BOXED_BYTE_OVERLOADS
inline std::vector<std::uint8_t> image_byte_vector_from_value(const gea_cpp_value &bytes) {
  std::vector<std::uint8_t> out;
  // Typed-array values keep their backing vector inside the boxed value. Read it
  // by reference so a tile body does not become a huge temporary vector of
  // gea_cpp_value boxes before being packed back to bytes.
  if (bytes.kind == gea_cpp_value::kind_t::array_value) {
    const std::vector<gea_cpp_value> &items = bytes.array_ref();
    out.reserve(items.size());
    for (const auto &item : items) out.push_back(static_cast<std::uint8_t>(static_cast<double>(item)));
    return out;
  }
  auto items = static_cast<std::vector<gea_cpp_value>>(bytes);
  out.reserve(items.size());
  for (const auto &item : items) out.push_back(static_cast<std::uint8_t>(static_cast<double>(item)));
  return out;
}

inline double ImageService::loadBytes(const gea_cpp_value &bytes) const {
  return loadBytes(image_byte_vector_from_value(bytes));
}

inline double ImageService::loadBytesOpaque(const gea_cpp_value &bytes) const {
  return loadBytesOpaque(image_byte_vector_from_value(bytes));
}

inline bool ImageService::writeFile(const std::string &path, const gea_cpp_value &bytes) const {
  return writeFile(path, image_byte_vector_from_value(bytes));
}
#endif

}  // namespace gea::host
