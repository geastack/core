// SPDX-License-Identifier: Apache-2.0
// This TU is native platform/image-store code. Boxed gea_cpp_value overloads
// are header-inline in host/image.h and compile only in generated app TUs that
// already include the JS runtime. Keep this TU runtime-free.
#ifdef GEA_CPP_VALUE_AVAILABLE
#define GEA_IMAGE_RESTORE_CPP_VALUE_AVAILABLE 1
#undef GEA_CPP_VALUE_AVAILABLE
#endif
#include "host/image.h"

#include "image.h"
#include "gea/embedded-host.h"
#include "display.h"
#include "host/fetch.h"  // gea::host::fetch for fetchBytes/fetchText (WiFi tile download)
#include "platform/file_cache.h"

#ifdef GEA_IMAGE_RESTORE_CPP_VALUE_AVAILABLE
#define GEA_CPP_VALUE_AVAILABLE 1
#undef GEA_IMAGE_RESTORE_CPP_VALUE_AVAILABLE
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_partition.h"  // readMapArchive() reads a .pmtiles flashed to ota_1
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // xTaskCreateWithCaps (force internal stack)
#include "freertos/semphr.h"
#endif

namespace gea::platform::storage {
// Mount provider registered by the target at startup (null => no persistent
// storage => file cache disabled). Explicit registration (not a weak symbol)
// keeps the provider's TU linked.
namespace {
bool (*g_mount_provider)() = nullptr;
}
void setMountProvider(bool (*provider)()) { g_mount_provider = provider; }
bool ensureMounted() { return g_mount_provider ? g_mount_provider() : false; }
}  // namespace gea::platform::storage

namespace gea::host {

extern "C" bool gea_embedded_asset_lookup(const char *path, const unsigned char **data, unsigned long *length) __attribute__((weak));

namespace {

gea::framework::graphics::ImageStore &store() {
  return gea::framework::graphics::ImageStore::instance();
}

int integer(double value) {
  return static_cast<int>(value);
}

// mkdir -p of the parent directories in `path` (FAT/VFS); existing dirs are fine.
void makeParentDirs(const std::string &path) {
  for (std::size_t i = 1; i < path.size(); i++) {
    if (path[i] != '/') continue;
    const std::string dir = path.substr(0, i);
    if (!dir.empty()) ::mkdir(dir.c_str(), 0777);
  }
}

#ifdef ESP_PLATFORM
// Flash reads briefly disable the cache, so the calling task's stack must live
// in INTERNAL RAM (a PSRAM stack is inaccessible cache-off → panic). The app's
// own task runs on a PSRAM stack, so the ota_1 read runs on this short-lived
// helper task with an internal stack instead. The destination vector may be in
// PSRAM — esp_partition_read bounces external destinations through an internal
// buffer, so only the stack matters.
struct MapReadCtx {
  const esp_partition_t *p;
  std::vector<std::uint8_t> *out;
  SemaphoreHandle_t done;
};

void mapReadTask(void *arg) {
  auto *c = static_cast<MapReadCtx *>(arg);
  std::uint8_t hdr[127];
  if (esp_partition_read(c->p, 0, hdr, sizeof(hdr)) == ESP_OK && hdr[0] == 0x50 && hdr[1] == 0x4d) {
    std::uint64_t off = 0;
    std::uint64_t len = 0;
    for (int i = 7; i >= 0; i--) {
      off = off * 256 + hdr[56 + i];  // tile_data_offset (u64 LE @56)
      len = len * 256 + hdr[64 + i];  // tile_data_length (u64 LE @64)
    }
    const std::uint64_t total = off + len;
    if (total > 0 && total <= c->p->size) {
      c->out->resize(static_cast<std::size_t>(total));
      if (esp_partition_read(c->p, 0, c->out->data(), static_cast<std::size_t>(total)) != ESP_OK) c->out->clear();
    }
  }
  xSemaphoreGive(c->done);
  vTaskDeleteWithCaps(nullptr);  // matches xTaskCreateWithCaps allocation
}
#endif

std::vector<std::uint8_t> readWholeFile(const std::string &path) {
  std::vector<std::uint8_t> out;
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), f);
    out.resize(got);
  }
  std::fclose(f);
  return out;
}

}  // namespace

double ImageService::loadBytes(std::vector<std::uint8_t> bytes) const {
  const int id = store().decode(bytes.data(), static_cast<int>(bytes.size()), -1);
  return static_cast<double>(id);
}

double ImageService::loadBytesOpaque(std::vector<std::uint8_t> bytes) const {
  const int id = store().decodeOpaque(bytes.data(), static_cast<int>(bytes.size()), -1);
  return static_cast<double>(id);
}

double ImageService::loadBytesPtr(const std::uint8_t *data, std::size_t length) const {
  if (!data || length == 0) return -1.0;
  // One decode per asset; an <img> that remounts reuses the same slot instead
  // of leaking a fresh decode each time. The embedded symbol address is stable,
  // so it's a safe cache key.
  static std::unordered_map<const void *, int> cache;
  const auto cached = cache.find(data);
  if (cached != cache.end()) return static_cast<double>(cached->second);
  // Embedded-asset bytes live in flash at a stable address: defer the
  // decode to first draw so boot only pays for visible images.
  const int id = store().decodeDeferred(data, static_cast<int>(length), -1);
  cache.emplace(data, id);
  return static_cast<double>(id);
}

double ImageService::loadAssetPath(const char *path) const {
  if (!path) return -1.0;
  // First: a bundled/flash asset (the common <img src="/logo.png"> case).
  if (gea_embedded_asset_lookup) {
    const unsigned char *data = nullptr;
    unsigned long length = 0;
    if (gea_embedded_asset_lookup(path, &data, &length)) return loadBytesPtr(data, static_cast<std::size_t>(length));
  }
  // Fallback: an absolute filesystem path (e.g. an EPUB cover extracted to the
  // SD card). Lets `<img src="/sdcard/...">` show runtime images through the
  // framework's own decode/render path instead of only bundled assets.
  if (path[0] == '/') return loadFile(std::string(path));
  return -1.0;
}

double ImageService::loadFile(const std::string &path) const {
  if (path.empty() || !gea::platform::storage::ensureMounted()) return -1.0;
  const std::vector<std::uint8_t> bytes = readWholeFile(path);
  if (bytes.empty()) return -1.0;
  return static_cast<double>(store().decode(bytes.data(), static_cast<int>(bytes.size()), -1));
}

double ImageService::loadFileOpaque(const std::string &path) const {
  if (path.empty() || !gea::platform::storage::ensureMounted()) return -1.0;
  const std::vector<std::uint8_t> bytes = readWholeFile(path);
  if (bytes.empty()) return -1.0;
  return static_cast<double>(store().decodeOpaque(bytes.data(), static_cast<int>(bytes.size()), -1));
}

bool ImageService::writeFile(const std::string &path, const std::vector<std::uint8_t> &bytes) const {
  if (path.empty() || bytes.empty() || !gea::platform::storage::ensureMounted()) return false;
  makeParentDirs(path);
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::size_t wrote = std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return wrote == bytes.size();
}

std::vector<std::uint8_t> ImageService::readFile(const std::string &path) const {
  if (path.empty() || !gea::platform::storage::ensureMounted()) return {};
  return readWholeFile(path);
}

std::vector<std::uint8_t> ImageService::fetchBytes(const std::string &url) const {
  // Synchronous HTTP GET over WiFi (blocks the calling task until the body is
  // in), returning the raw bytes — e.g. one MVT tile. Empty when WiFi is down
  // or the request fails.
  return gea::host::fetch(url).body;
}

std::string ImageService::fetchText(const std::string &url) const {
  return gea::host::fetch(url).text();
}

std::vector<std::uint8_t> ImageService::readFileRange(const std::string &path, double offset, double length) const {
  // Read just [offset, offset+length) — PMTiles' random-access pattern: the
  // directory + each tile are small ranges, so the whole archive never loads
  // into RAM. SD reads (fopen/fread) don't disable the flash cache, so this is
  // safe straight from the app task.
  if (path.empty() || length <= 0 || !gea::platform::storage::ensureMounted()) return {};
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
  std::fseek(f, static_cast<long>(offset), SEEK_SET);
  const std::size_t got = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(got);
  return out;
}

std::string ImageService::listFiles(const std::string &path) const {
  if (path.empty() || !gea::platform::storage::ensureMounted()) return {};
  DIR *dir = opendir(path.c_str());
  if (!dir) return {};
  std::string out;
  while (const dirent *entry = readdir(dir)) {
    // Some VFS drivers (e.g. FATFS on ESP-IDF) don't fill d_type; stat the
    // entry when the type is unknown so directories stay excluded either way.
    if (entry->d_name[0] == '.') continue;
    bool regular = entry->d_type == DT_REG;
    if (entry->d_type == DT_UNKNOWN) {
      struct stat st {};
      const std::string full = path + "/" + entry->d_name;
      regular = stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }
    if (!regular) continue;
    if (!out.empty()) out += '\n';
    out += entry->d_name;
  }
  closedir(dir);
  return out;
}

std::vector<std::uint8_t> ImageService::readMapArchive() const {
  // Read a .pmtiles archive flashed (via esptool) into the spare ota_1 app
  // partition — a reliable USB path that doesn't depend on the SD card or the
  // lossy serial console. Returns exactly the archive's bytes (its size comes
  // from the PMTiles header: tile_data_offset + tile_data_length). Empty off
  // esp32 or when no valid archive is present.
#ifdef ESP_PLATFORM
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!p) return {};
  std::vector<std::uint8_t> out;
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) return {};
  MapReadCtx ctx{p, &out, done};
  TaskHandle_t task = nullptr;
  // 8 KB INTERNAL-RAM stack: a flash read disables the cache, so the reading
  // task's stack must not be in PSRAM (the app's own task stack is).
  if (xTaskCreateWithCaps(&mapReadTask, "map_read", 8192, &ctx, 6, &task, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
    xSemaphoreTake(done, portMAX_DELAY);
  }
  vSemaphoreDelete(done);
  return out;
#else
  return {};
#endif
}

void ImageService::draw(double id, double x, double y) const {
  const int slot = integer(id);
  const gea::framework::graphics::pixel::native_t *pixels = store().currentPixels(slot);
  if (!pixels) return;

  const unsigned char *alpha = store().currentAlpha(slot);
  const int width = store().width(slot);
  const int height = store().height(slot);
  gea::platform::display::Display::blitImage(pixels, alpha, width, height, integer(x), integer(y));
}

double ImageService::width(double id) const {
  return static_cast<double>(store().width(integer(id)));
}

double ImageService::height(double id) const {
  return static_cast<double>(store().height(integer(id)));
}

double ImageService::frameCount(double id) const {
  return static_cast<double>(store().frameCount(integer(id)));
}

bool ImageService::isAnimated(double id) const {
  return store().isAnimated(integer(id));
}

void ImageService::setPlaying(double id, double playing) const {
  store().setPlaying(integer(id), playing != 0.0);
}

void ImageService::seek(double id, double frame) const {
  store().seek(integer(id), integer(frame));
}

bool ImageService::advance(double id, double deltaMs) const {
  return store().advance(integer(id), integer(deltaMs));
}

void ImageService::dispose(double id) const {
  store().dispose(integer(id));
}

GeaEmbeddedImage ImageService::make(double id) const {
  const int slot = integer(id);
  GeaEmbeddedImage handle;
  handle.id = slot;
  handle.width = store().width(slot);
  handle.height = store().height(slot);
  handle.frameCount = store().frameCount(slot);
  handle.isAnimated = store().isAnimated(slot);
  return handle;
}

bool GeaEmbeddedImage::decode() const { return store().currentPixels(id) != nullptr; }
void GeaEmbeddedImage::play() const { store().setPlaying(id, true); }
void GeaEmbeddedImage::pause() const { store().setPlaying(id, false); }
void GeaEmbeddedImage::seek(int frame) const { store().seek(id, frame); }
void GeaEmbeddedImage::dispose() const { store().dispose(id); }

}  // namespace gea::host

extern "C" double gea_host_image_load_asset_path(const char *path) {
  return gea::host::image.loadAssetPath(path);
}
