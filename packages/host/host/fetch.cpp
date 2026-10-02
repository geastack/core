// SPDX-License-Identifier: Apache-2.0
#ifdef GEA_CPP_VALUE_AVAILABLE
#define GEA_FETCH_RESTORE_CPP_VALUE_AVAILABLE 1
#undef GEA_CPP_VALUE_AVAILABLE
#endif
#include "host/fetch.h"
#ifdef GEA_FETCH_RESTORE_CPP_VALUE_AVAILABLE
#define GEA_CPP_VALUE_AVAILABLE 1
#undef GEA_FETCH_RESTORE_CPP_VALUE_AVAILABLE
#endif

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>
#endif

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)

#include "gea/embedded-host.h"
#include "memory.h"
#include "wifi.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#if GEA_EMBEDDED_ENABLE_HTTPS
#include "esp_crt_bundle.h"
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace gea::framework::host {

namespace {

// Keep-alive connection cache, ONE PER TASK (thread_local): each caller —
// the frame task, each async tile-loader worker — keeps its own
// esp_http_client, so sequential requests to the same scheme://host[:port]
// pay the TCP + TLS handshake once, while different tasks fetch fully in
// PARALLEL (no shared handle, no serializing mutex; esp_http_client handles
// are not thread-safe, which per-task ownership sidesteps entirely).
thread_local esp_http_client_handle_t g_client = nullptr;
thread_local std::string g_client_origin;
thread_local std::int64_t g_client_last_use_us = 0;

std::string originOf(const std::string &url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  const std::size_t path = url.find('/', scheme + 3);
  return path == std::string::npos ? url : url.substr(0, path);
}

void dropCachedClient() {
  if (g_client) {
    esp_http_client_cleanup(g_client);  // closes the socket + frees the handle
    g_client = nullptr;
  }
  g_client_origin.clear();
}

// Connection reuse (keep-alive) only works through esp_http_client_perform(),
// which delivers the body in chunks via HTTP_EVENT_ON_DATA. This accumulator
// collects those chunks. It is thread_local like the client: the event handler
// runs synchronously inside perform() on the calling task, so each task's
// accumulator only ever sees its own request.
struct BodyAccumulator {
  static constexpr std::size_t kMaxBytes = 2u * 1024u * 1024u;
  std::uint8_t *buffer = nullptr;
  std::size_t capacity = 0;
  std::size_t size = 0;

  void reset() { size = 0; }  // keep the buffer allocated for reuse across tiles

  void release() {
    gea::framework::memory::Allocator::free(buffer);
    buffer = nullptr;
    capacity = 0;
    size = 0;
  }

  void append(const char *data, int len) {
    if (len <= 0) return;
    std::size_t need = size + static_cast<std::size_t>(len);
    if (need > kMaxBytes) need = kMaxBytes;
    if (need > capacity) {
      std::size_t next = capacity ? capacity : 4096;
      while (next < need) next *= 2;
      if (next > kMaxBytes) next = kMaxBytes;
      auto *grown = static_cast<std::uint8_t *>(
          gea::framework::memory::Allocator::reallocatePreferSpiram(buffer, next));
      if (!grown) return;  // realloc failed — keep what we have
      buffer = grown;
      capacity = next;
    }
    const std::size_t room = capacity - size;
    const std::size_t take = static_cast<std::size_t>(len) < room ? static_cast<std::size_t>(len) : room;
    std::memcpy(buffer + size, data, take);
    size += take;
  }
};

thread_local BodyAccumulator g_body;

esp_err_t onHttpEvent(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
    g_body.append(static_cast<const char *>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

}  // namespace

class FetchResponseBuilder {
 public:
  static constexpr std::size_t max_body_bytes = 2u * 1024u * 1024u;
  static constexpr std::size_t initial_body_capacity = 4096;
  static constexpr int timeout_ms = 30000;

  FetchResponseBuilder(const std::string &url, const gea::host::FetchRequestInit &init)
      : url_(url), init_(init) {}

  gea::host::FetchResponse get() const {
    gea::host::FetchResponse result{};
    result.ok = false;
    result.status = 0;

    // WiFi/lwip is opt-in and only initialized during deferred WiFi bring-up
    // (see docs/esp32-connectivity-and-ram.md). Calling esp_http_client before
    // the TCP/IP stack is up triggers an lwip "Invalid mbox" assert that panics
    // and reboots the whole device. Bail with a normal failed response instead,
    // matching browser semantics where fetch rejects on network failure.
    if (!gea::framework::network::wifi().connected()) {
      ESP_LOGW(logTag(), "fetch(%s) skipped: WiFi not connected", url_.c_str());
      return result;
    }

    // Acquire THIS TASK's connection: reuse the thread-local keep-alive client
    // when the origin matches (skips the handshake), otherwise (re)create it.
    // Keep-alive reuse requires esp_http_client_perform() (the manual open/read
    // API always opens a fresh socket and never reuses), so the body arrives
    // via onHttpEvent into the thread-local accumulator. No locking: every
    // task owns its own client + accumulator, so fetches run in parallel.
    g_body.reset();
    const std::string origin = originOf(url_);
    static std::mutex s_handshake_gate;
    // Reuse the parked connection only while it's plausibly still open on the
    // server side. Most servers (open-meteo's nginx front included) drop idle
    // keep-alive sockets after a few seconds, and esp_http_client_perform() on
    // a dead socket can block for up to the timeout before failing. Outside the
    // window we reconnect proactively instead of gambling on the stall; inside
    // it, ms-apart tile bursts ride the warm connection.
    constexpr std::int64_t kReuseWindowUs = 8 * 1000 * 1000;
    bool reuse = g_client && g_client_origin == origin &&
                 (esp_timer_get_time() - g_client_last_use_us) < kReuseWindowUs;
    // Per-request timeout: callers (e.g. the tile loader) set a short timeout so
    // a transient WiFi/TCP stall fails fast and is retried on a fresh socket,
    // instead of waiting out lwip's multi-RTO retransmit (~15s). Default callers
    // (weather, nominatim) leave it 0 and keep the full timeout_ms.
    const int effective_timeout_ms = init_.timeout_ms > 0 ? init_.timeout_ms : timeout_ms;

    for (int attempt = 0; attempt < 2; ++attempt) {
      // Serialize COLD connections only: a TLS handshake needs ~50KB of INTERNAL
      // ram (mbedTLS), and two at once during a tile burst exhausted the heap
      // (operator-new abort on zoom-out). A request riding an already-warm
      // keep-alive connection skips the gate entirely, so parallel tile
      // downloads stay parallel after each worker's first fetch.
      std::unique_lock<std::mutex> handshake_lock(s_handshake_gate, std::defer_lock);
      if (!reuse) handshake_lock.lock();
      esp_http_client_handle_t client = nullptr;
      if (reuse) {
        client = g_client;
        esp_http_client_set_url(client, url_.c_str());
        esp_http_client_set_method(client, methodFromInit());
        // Apply the per-request timeout to the reused socket too, so a stalled
        // reuse fails fast and the retry below re-inits a fresh connection.
        esp_http_client_set_timeout_ms(client, effective_timeout_ms);
      } else {
        dropCachedClient();
        esp_http_client_config_t config = {};
        config.url = url_.c_str();
        config.timeout_ms = effective_timeout_ms;
        config.method = methodFromInit();
        config.keep_alive_enable = true;
        config.event_handler = onHttpEvent;
        config.user_data = &g_body;
#if GEA_EMBEDDED_ENABLE_HTTPS
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        client = esp_http_client_init(&config);
        if (!client) {
          ESP_LOGE(logTag(), "init failed for %s", url_.c_str());
          return result;
        }
        g_client = client;
        g_client_origin = origin;
      }

      // OSM's tile usage policy (and other policy-bound APIs) throttle or block
      // the generic esp_http_client default User-Agent ("ESP32 HTTP Client/1.0"),
      // which manifests as intermittent multi-second tarpitted responses. Send an
      // identifying UA on every request; caller headers below may override it.
      esp_http_client_set_header(client, "User-Agent", "gea-embedded/1.0");
      for (const auto &kv : init_.headers) {
        esp_http_client_set_header(client, kv.first.c_str(), kv.second.c_str());
      }
      if (!init_.body.empty()) {
        esp_http_client_set_post_field(client, reinterpret_cast<const char *>(init_.body.data()),
                                       static_cast<int>(init_.body.size()));
      }

      const esp_err_t err = esp_http_client_perform(client);
      if (err != ESP_OK) {
        ESP_LOGE(logTag(), "perform failed for %s: %s%s", url_.c_str(), esp_err_to_name(err),
                 reuse ? " (reused connection went stale; retrying on a fresh one)" : "");
        // An HTTP rejection is still a Fetch Response. ESP-IDF's automatic
        // auth handler can return NOT_SUPPORTED for a Bearer 401 without a
        // WWW-Authenticate challenge; do not turn that into opaque HTTP 0.
        const int status = esp_http_client_get_status_code(client);
        if (status >= 400) {
          result.status = status;
          readResponseHeaders(result, client);
          if (g_body.size) result.body.assign(g_body.buffer, g_body.buffer + g_body.size);
          // perform() delivers body bytes through HTTP_EVENT_ON_DATA without
          // populating esp_http_client_read()'s cache. Reading that cache after
          // an auth error dereferences raw_data=null with raw_len still set.
          // Preserve the body already delivered to our event handler.
          ESP_LOGW(logTag(), "HTTP rejection status=%d bytes=%u", status,
            static_cast<unsigned>(result.body.size()));
          dropCachedClient();
          return result;
        }
        dropCachedClient();
        if (reuse) {
          // The parked socket died inside the reuse window — retry exactly once
          // on a fresh connection so a server-side early close doesn't surface
          // as a failed fetch to the app.
          reuse = false;
          g_body.reset();
          continue;
        }
        return result;
      }
      g_client_last_use_us = esp_timer_get_time();

      result.status = static_cast<double>(esp_http_client_get_status_code(client));
      readResponseHeaders(result, client);
      result.body.assign(g_body.buffer, g_body.buffer + g_body.size);
      // No close/cleanup: keep_alive_enable parks the connection for the next
      // same-origin request. dropCachedClient() runs on origin change or error.

      result.ok = result.status >= 200.0 && result.status < 300.0;
      ESP_LOGI(logTag(), "%s %s -> %.0f %u bytes", init_.method.c_str(), url_.c_str(), result.status,
               static_cast<unsigned>(result.body.size()));
      return result;
    }
    return result;
  }

 private:
  esp_http_client_method_t methodFromInit() const {
    const std::string &m = init_.method;
    if (m == "POST") return HTTP_METHOD_POST;
    if (m == "PUT") return HTTP_METHOD_PUT;
    if (m == "DELETE") return HTTP_METHOD_DELETE;
    if (m == "PATCH") return HTTP_METHOD_PATCH;
    if (m == "HEAD") return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
  }

  static const char *logTag() {
    return "gea::host::fetch";
  }

  static std::string lowercase(const std::string &s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
  }

  static void readResponseHeaders(gea::host::FetchResponse &result, esp_http_client_handle_t client) {
    // esp_http_client doesn't expose a header iterator; sniff the common ones explicitly.
    // Keys are stored lowercased for browser-parity case-insensitive lookup.
    static constexpr const char *common_headers[] = {
      "content-type", "content-length", "set-cookie", "location",
      "www-authenticate", "cache-control",
    };
    for (const char *name : common_headers) {
      char *value = nullptr;
      if (esp_http_client_get_header(client, name, &value) == ESP_OK && value != nullptr) {
        result.headers[lowercase(name)] = value;
      }
    }
  }

  static void *allocate(std::size_t size) {
    return gea::framework::memory::Allocator::allocatePreferSpiram(size);
  }

  static void *reallocate(void *ptr, std::size_t size) {
    return gea::framework::memory::Allocator::reallocatePreferSpiram(ptr, size);
  }

  static std::size_t initialCapacity(int content_length) {
    std::size_t capacity = content_length > 0 ? static_cast<std::size_t>(content_length) : initial_body_capacity;
    return capacity > max_body_bytes ? max_body_bytes : capacity;
  }

  static bool grow(std::uint8_t *&buffer, std::size_t &capacity) {
    std::size_t next_capacity = capacity * 2u;
    if (next_capacity > max_body_bytes) next_capacity = max_body_bytes;
    if (next_capacity <= capacity) return false;

    auto *grown = static_cast<std::uint8_t *>(reallocate(buffer, next_capacity));
    if (!grown) {
      ESP_LOGW(logTag(), "realloc(%u) failed; truncating", static_cast<unsigned>(next_capacity));
      return false;
    }

    buffer = grown;
    capacity = next_capacity;
    return true;
  }

  static void readInto(gea::host::FetchResponse &result, esp_http_client_handle_t client) {
    const int content_length = esp_http_client_fetch_headers(client);
    result.status = static_cast<double>(esp_http_client_get_status_code(client));

    std::size_t capacity = initialCapacity(content_length);
    std::uint8_t *buffer = static_cast<std::uint8_t *>(allocate(capacity));
    if (!buffer) {
      ESP_LOGE(logTag(), "body allocation failed (%u bytes)", static_cast<unsigned>(capacity));
      return;
    }

    std::size_t total = 0;
    while (total < max_body_bytes) {
      if (total == capacity && !grow(buffer, capacity)) break;

      const int to_read = static_cast<int>(capacity - total);
      const int read_len = esp_http_client_read(client, reinterpret_cast<char *>(buffer) + total, to_read);
      if (read_len <= 0) {
        break;
      }
      total += static_cast<std::size_t>(read_len);
    }

    result.body.assign(buffer, buffer + total);
    gea::framework::memory::Allocator::free(buffer);
  }

  const std::string &url_;
  const gea::host::FetchRequestInit &init_;
};

class FetchHost {
 public:
  static gea::host::FetchResponse fetch(const std::string &url) {
    return fetch(url, gea::host::FetchRequestInit{});
  }
  static gea::host::FetchResponse fetch(const std::string &url, const gea::host::FetchRequestInit &init) {
    return FetchResponseBuilder(url, init).get();
  }
  // Closes the calling task's keep-alive connection and frees its response buffer. A task that is about
  // to exit must call this: both are thread_local, so nothing else would ever close or free them.
  static void releaseTaskState() {
    dropCachedClient();
    g_body.release();
  }
};

// Streaming multipart upload over a RAW TLS socket (esp_tls), NOT esp_http_client:
// its manual open/write streaming-POST path leaves the socket stuck in EINPROGRESS
// on this IDF/board (every write poll-fails "Connection already in progress"). This
// mirrors the proven pala_note reference (WiFiClientSecure): connect, write the
// request line + headers + `prefix` + the file streamed in 4 KB chunks off disk +
// `suffix`, then read + de-chunk the response. The file never sits in RAM.
class UploadStreamer {
 public:
  // Write all `len` bytes, tolerating partial writes and TLS WANT_READ/WANT_WRITE
  // (slow link) by briefly waiting and retrying. False only after a long stall.
  static bool tlsWriteAll(esp_tls_t *tls, const char *data, std::size_t len) {
    std::size_t sent = 0;
    int stalls = 0;
    while (sent < len) {
      const ssize_t w = esp_tls_conn_write(tls, data + sent, len - sent);
      if (w > 0) {
        sent += static_cast<std::size_t>(w);
        stalls = 0;
        continue;
      }
      if (w == ESP_TLS_ERR_SSL_WANT_WRITE || w == ESP_TLS_ERR_SSL_WANT_READ || w == 0) {
        if (++stalls > 6000) {
          std::printf("[upload-diag] write STALL-timeout sent=%u internal_free=%u\n",
                      static_cast<unsigned>(sent),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
          return false;  // ~60 s with no progress -> give up
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      std::printf("[upload-diag] write HARD-ERR esp_tls=%d (-0x%04X) sent=%u internal_free=%u\n",
                  static_cast<int>(w), static_cast<unsigned>(-w), static_cast<unsigned>(sent),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      return false;  // hard TLS/socket error
    }
    return true;
  }

  // Strip HTTP/1.1 chunked transfer-encoding framing from a body (raw TLS gives
  // us the framing that esp_http_client would otherwise remove).
  static std::string dechunk(const std::string &body) {
    std::string out;
    std::size_t i = 0;
    while (i < body.size()) {
      const std::size_t eol = body.find("\r\n", i);
      if (eol == std::string::npos) break;
      const long size = std::strtol(body.substr(i, eol - i).c_str(), nullptr, 16);
      if (size <= 0) break;
      const std::size_t start = eol + 2;
      if (start + static_cast<std::size_t>(size) > body.size()) {
        out.append(body, start, body.size() - start);
        break;
      }
      out.append(body, start, static_cast<std::size_t>(size));
      i = start + static_cast<std::size_t>(size) + 2;  // skip chunk + trailing CRLF
    }
    return out;
  }

  static gea::host::FetchResponse upload(const std::string &url, const std::string &authorization,
                                         const std::string &contentType, const std::string &prefix,
                                         const std::string &filePath, const std::string &suffix) {
    gea::host::FetchResponse result{};
    result.ok = false;
    result.status = 0;
    if (!gea::framework::network::wifi().connected()) {
      ESP_LOGW("gea::host::fetch", "uploadFile(%s) skipped: WiFi not connected", url.c_str());
      result.status = -10;  // WiFi link down (e.g. dropped during a prior upload)
      return result;
    }

    // Split url -> host + path (https://host/path...).
    const std::size_t scheme = url.find("://");
    const std::size_t hostStart = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t pathStart = url.find('/', hostStart);
    const std::string host =
        url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    const std::string path = pathStart == std::string::npos ? "/" : url.substr(pathStart);

    FILE *f = std::fopen(filePath.c_str(), "rb");
    if (!f) {
      ESP_LOGE("gea::host::fetch", "uploadFile: cannot open %s", filePath.c_str());
      result.status = -11;  // SD open failed
      return result;
    }
    std::fseek(f, 0, SEEK_END);
    const long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileSize < 0) {
      std::fclose(f);
      return result;
    }
    const long contentLength =
        static_cast<long>(prefix.size()) + fileSize + static_cast<long>(suffix.size());
    // Reset progress to this upload's body size so the Sync screen bar reflects
    // bytes streamed (prefix + file + suffix). The HTTP head is tiny and not
    // counted. One upload runs at a time, so a single global counter suffices.
    gea::host::uploadProgressReset(contentLength);

    esp_tls_cfg_t cfg = {};
#if GEA_EMBEDDED_ENABLE_HTTPS
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    cfg.timeout_ms = 300000;
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
      std::fclose(f);
      return result;
    }
    if (esp_tls_conn_new_sync(host.c_str(), static_cast<int>(host.size()), 443, &cfg, tls) != 1) {
      ESP_LOGE("gea::host::fetch", "uploadFile: TLS connect to %s failed", host.c_str());
      esp_tls_conn_destroy(tls);
      std::fclose(f);
      result.status = -12;  // TLS/TCP connect failed (link up but couldn't reach host)
      return result;
    }

    std::string head = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
    // Match the headers esp_http_client sends automatically — OpenAI/Cloudflare
    // can reject a request that omits User-Agent/Accept.
    head += "User-Agent: gea-embedded/1.0\r\nAccept: */*\r\n";
    if (!authorization.empty()) head += "Authorization: " + authorization + "\r\n";
    if (!contentType.empty()) head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + std::to_string(contentLength) + "\r\nConnection: close\r\n\r\n";

    bool writeOk = tlsWriteAll(tls, head.data(), head.size()) &&
                   (prefix.empty() || tlsWriteAll(tls, prefix.data(), prefix.size()));
    if (writeOk && !prefix.empty()) gea::host::uploadProgressAdd(static_cast<long long>(prefix.size()));
    long bytesStreamed = 0;
    // Diagnostic failure code surfaced as result.status (negative) so the Sync
    // screen distinguishes WHY the body write failed without needing serial:
    // -7 = buffer alloc, -8 = SD short-read (RAM/SD), -9 = TLS/socket write error.
    int failKind = 0;
    if (writeOk) {
      // Read SD -> a DMA-capable INTERNAL buffer so the SDMMC driver streams
      // directly into it. A PSRAM (or otherwise non-DMA) read target forces
      // sdmmc to allocate a same-sized internal bounce buffer PER read. With
      // WiFi up, internal DMA RAM is nearly exhausted (a few KB free) and
      // fragments under sustained TLS load, so a large bounce alloc fails
      // partway through a multi-MB upload (allocate_dma_buf NO_MEM); fread then
      // returns 0 (indistinguishable from EOF) and we ship a body shorter than
      // Content-Length -> the server waits for the rest -> no response ->
      // "upload HTTP 0", consistently near the end. One small persistent
      // internal buffer (read straight into it, no bounce) avoids that.
      constexpr std::size_t kUploadChunk = 2048;
      char *buffer = static_cast<char *>(heap_caps_malloc(kUploadChunk, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
      if (buffer) {
        std::size_t n;
        while ((n = std::fread(buffer, 1, kUploadChunk, f)) > 0) {
          if (!tlsWriteAll(tls, buffer, n)) {
            writeOk = false;
            failKind = -9;
            break;
          }
          bytesStreamed += static_cast<long>(n);
          gea::host::uploadProgressAdd(static_cast<long long>(n));
        }
        // A short read (SD error, or fewer bytes than the file) truncates the
        // body; fail loudly instead of shipping a partial upload that the
        // server hangs waiting to complete.
        if (writeOk && (std::ferror(f) || bytesStreamed != fileSize)) {
          std::printf("[upload-diag] SHORT-READ %ld/%ld bytes ferror=%d internal_free=%u\n",
                      bytesStreamed, fileSize, std::ferror(f),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
          writeOk = false;
          failKind = -8;
        }
        heap_caps_free(buffer);
      } else {
        writeOk = false;
        failKind = -7;
      }
    }
    std::fclose(f);
    if (writeOk && !suffix.empty()) {
      writeOk = tlsWriteAll(tls, suffix.data(), suffix.size());
      if (writeOk) gea::host::uploadProgressAdd(static_cast<long long>(suffix.size()));
      else failKind = -9;
    }
    if (!writeOk) {
      ESP_LOGE("gea::host::fetch", "uploadFile TLS write failed for %s", url.c_str());
      esp_tls_conn_destroy(tls);
      // -9 default covers head/prefix write failures (the only other writeOk path).
      result.status = failKind ? failKind : -9;
      return result;
    }

    // Read the whole response (Connection: close).
    std::string resp;
    char rbuf[1024];
    for (;;) {
      const ssize_t r = esp_tls_conn_read(tls, rbuf, sizeof(rbuf));
      if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (r <= 0) break;  // 0 = closed, <0 = error/EOF
      resp.append(rbuf, static_cast<std::size_t>(r));
      if (resp.size() > 256u * 1024u) break;
    }
    esp_tls_conn_destroy(tls);

    const std::size_t statusSp = resp.find(' ');
    if (statusSp != std::string::npos) result.status = std::strtod(resp.c_str() + statusSp + 1, nullptr);
    const std::size_t bodyStart = resp.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
      std::string headers = resp.substr(0, bodyStart);
      std::transform(headers.begin(), headers.end(), headers.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      std::string body = resp.substr(bodyStart + 4);
      if (headers.find("transfer-encoding: chunked") != std::string::npos) body = dechunk(body);
      result.body.assign(body.begin(), body.end());
    }
    // The body was fully written (the !writeOk early-return above is the only
    // path that keeps status 0), so a still-zero status here means the response
    // never arrived — the connection dropped/timed out after the upload. Surface
    // it as -4 to distinguish from a write failure (0).
    if (result.status == 0.0) result.status = -4;
    result.ok = result.status >= 200.0 && result.status < 300.0;
    std::printf("[upload-diag] DONE status=%.0f resp_bytes=%u body_bytes=%u internal_free=%u\n",
                result.status, static_cast<unsigned>(resp.size()),
                static_cast<unsigned>(result.body.size()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    ESP_LOGI("gea::host::fetch", "upload %s -> %.0f (body=%u total=%u file=%ld)", url.c_str(),
             result.status, static_cast<unsigned>(result.body.size()),
             static_cast<unsigned>(resp.size()), fileSize);
    if (!result.ok) {
      ESP_LOGW("gea::host::fetch", "upload error resp: %.300s", resp.c_str());
    }
    return result;
  }
};

}  // namespace gea::framework::host

gea::host::FetchResponse gea::host::fetch(const std::string &url) {
  return gea::framework::host::FetchHost::fetch(url, FetchRequestInit{});
}

gea::host::FetchResponse gea::host::fetch(const std::string &url, const FetchRequestInit &init) {
  return gea::framework::host::FetchHost::fetch(url, init);
}

gea::host::FetchResponse gea::host::uploadFile(const std::string &url, const std::string &authorization,
                                               const std::string &contentType, const std::string &prefix,
                                               const std::string &filePath, const std::string &suffix) {
  return gea::framework::host::UploadStreamer::upload(url, authorization, contentType, prefix, filePath, suffix);
}

// Streaming GET -> SD file. Uses esp_http_client's manual open/read so the body
// is written to `destPath` chunk-by-chunk (PSRAM scratch), never buffered whole
// in RAM (unlike the perform()-based FetchHost path, which caps at 2 MB). The SD
// is a separate peripheral, so these PSRAM writes never hit a flash-cache-disable
// window. Runs on the async worker; one transfer at a time shares the progress
// byte counters.
gea::host::FetchResponse gea::host::downloadFile(const std::string &url, const std::string &authorization,
                                                 const std::string &destPath) {
  gea::host::FetchResponse result{};
  result.ok = false;
  result.status = 0;
  if (!gea::framework::network::wifi().connected()) {
    ESP_LOGW("gea::host::fetch", "downloadFile(%s) skipped: WiFi not connected", url.c_str());
    return result;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 300000;
  config.method = HTTP_METHOD_GET;
#if GEA_EMBEDDED_ENABLE_HTTPS
  config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE("gea::host::fetch", "downloadFile: init failed for %s", url.c_str());
    return result;
  }
  if (!authorization.empty()) esp_http_client_set_header(client, "Authorization", authorization.c_str());
  esp_http_client_set_header(client, "User-Agent", "gea-embedded/1.0");
  esp_http_client_set_header(client, "Accept", "*/*");

  if (esp_http_client_open(client, 0) != ESP_OK) {  // 0 = no request body (GET)
    ESP_LOGE("gea::host::fetch", "downloadFile: open failed for %s", url.c_str());
    esp_http_client_cleanup(client);
    return result;
  }
  const int64_t contentLength = esp_http_client_fetch_headers(client);  // -1 if chunked/unknown
  result.status = static_cast<double>(esp_http_client_get_status_code(client));
  result.ok = result.status >= 200.0 && result.status < 300.0;
  if (!result.ok) {
    ESP_LOGW("gea::host::fetch", "downloadFile %s -> %.0f", url.c_str(), result.status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
  }

  FILE *f = std::fopen(destPath.c_str(), "wb");
  if (!f) {
    ESP_LOGE("gea::host::fetch", "downloadFile: cannot open %s for write", destPath.c_str());
    result.ok = false;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
  }

  gea::host::uploadProgressReset(contentLength > 0 ? static_cast<long long>(contentLength) : 0);

  constexpr std::size_t kChunk = 16384;
  char *buffer = static_cast<char *>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) buffer = static_cast<char *>(heap_caps_malloc(kChunk, MALLOC_CAP_8BIT));
  bool ok = buffer != nullptr;
  long long received = 0;
  if (ok) {
    while (true) {
      const int r = esp_http_client_read(client, buffer, kChunk);
      if (r < 0) {  // read error
        ok = false;
        break;
      }
      if (r == 0) break;  // EOF / connection closed
      if (std::fwrite(buffer, 1, static_cast<std::size_t>(r), f) != static_cast<std::size_t>(r)) {
        ok = false;  // SD write failed (full?)
        break;
      }
      received += r;
      gea::host::uploadProgressAdd(static_cast<long long>(r));
    }
    heap_caps_free(buffer);
  }
  std::fclose(f);
  // A short read against a known Content-Length means the transfer was truncated.
  if (ok && contentLength > 0 && received != contentLength) ok = false;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  result.ok = result.ok && ok;
  ESP_LOGI("gea::host::fetch", "download %s -> %.0f (%lld bytes -> %s)", url.c_str(), result.status,
           received, destPath.c_str());
  return result;
}

#else  // ESP_PLATFORM

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace gea::framework::host {

#ifdef __EMSCRIPTEN__

namespace {

EM_ASYNC_JS(char *, gea_web_fetch_bridge, (const char *url_ptr, int url_len, const char *method_ptr, int method_len,
                                           const char *body_ptr, int body_len, const char *headers_ptr, int headers_len), {
  const url = UTF8ToString(url_ptr, url_len);
  const method = UTF8ToString(method_ptr, method_len) || "GET";
  const requestBody = body_len > 0 ? HEAPU8.slice(body_ptr, body_ptr + body_len) : new Uint8Array(0);
  const headerText = UTF8ToString(headers_ptr, headers_len);

  // Pack [int32 headerLen][int32 bodyLen][header utf8][raw body bytes] so binary
  // bodies (e.g. PNG tiles) survive the JS->WASM boundary intact. The header is
  // "status\nstatusText\nname: value\n...".
  function pack(status, statusText, responseHeaders, bodyU8) {
    const headerU8 = new TextEncoder().encode(status + "\n" + statusText + "\n" + responseHeaders);
    const out = _malloc(8 + headerU8.length + bodyU8.length);
    const view = new DataView(HEAPU8.buffer, out, 8);
    view.setInt32(0, headerU8.length, true);
    view.setInt32(4, bodyU8.length, true);
    HEAPU8.set(headerU8, out + 8);
    HEAPU8.set(bodyU8, out + 8 + headerU8.length);
    return out;
  }

  try {
    const headers = {};
    if (headerText.length > 0) {
      const parts = headerText.split("\n");
      for (let i = 0; i + 1 < parts.length; i += 2) {
        if (parts[i].length > 0) headers[parts[i]] = parts[i + 1];
      }
    }

    const init = { method, headers };
    if (requestBody.length > 0 && method !== "GET" && method !== "HEAD") init.body = requestBody;

    const response = await fetch(url, init);
    let responseHeaders = "";
    response.headers.forEach((value, key) => {
      responseHeaders += key + ": " + value + "\n";
    });
    const bytes = new Uint8Array(await response.arrayBuffer());
    return pack(String(response.status), String(response.statusText), responseHeaders, bytes);
  } catch (error) {
    const message = error && error.message ? error.message : String(error);
    return pack("0", message, "", new Uint8Array(0));
  }
});

}  // namespace

class BrowserFetchBridge {
 public:
  static gea::host::FetchResponse fetch(const std::string &url, const gea::host::FetchRequestInit &init) {
    gea::host::FetchResponse result{};
    std::string headers = serializeHeaders(init.headers);

    auto *raw = gea_web_fetch_bridge(url.data(), static_cast<int>(url.size()),
                                     init.method.data(), static_cast<int>(init.method.size()),
                                     init.body.data(), static_cast<int>(init.body.size()),
                                     headers.data(), static_cast<int>(headers.size()));

    if (!raw) return result;
    const auto *p = reinterpret_cast<const std::uint8_t *>(raw);
    std::int32_t header_len = 0;
    std::int32_t body_len = 0;
    std::memcpy(&header_len, p, sizeof(header_len));
    std::memcpy(&body_len, p + 4, sizeof(body_len));
    if (header_len < 0) header_len = 0;
    if (body_len < 0) body_len = 0;
    std::string header(reinterpret_cast<const char *>(p + 8), static_cast<std::size_t>(header_len));
    const std::uint8_t *body_ptr = p + 8 + header_len;
    result.body.assign(body_ptr, body_ptr + body_len);
    std::free(raw);

    parseHeaderBlock(header, result);
    result.ok = result.status >= 200.0 && result.status < 300.0;
    return result;
  }

 private:
  static std::string serializeHeaders(const gea::host::FetchHeaderMap &headers) {
    std::string out;
    for (const auto &kv : headers) {
      out += kv.first;
      out += '\n';
      out += kv.second;
      out += '\n';
    }
    return out;
  }

  static void trimTrailingCarriageReturn(std::string &line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
  }

  static void parseHeaders(const std::string &header_block, gea::host::FetchResponse &result) {
    std::istringstream stream(header_block);
    std::string line;
    while (std::getline(stream, line)) {
      trimTrailingCarriageReturn(line);
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;

      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!name.empty()) result.headers[name] = value;
    }
  }

  // Parses the text metadata block "status\nstatusText\nname: value\n..."; the
  // raw body is read separately from the length-prefixed binary payload.
  static void parseHeaderBlock(const std::string &header, gea::host::FetchResponse &result) {
    const std::size_t status_end = header.find('\n');
    if (status_end == std::string::npos) {
      result.status = std::strtod(header.c_str(), nullptr);
      return;
    }
    result.status = std::strtod(header.substr(0, status_end).c_str(), nullptr);

    const std::size_t text_end = header.find('\n', status_end + 1);
    if (text_end == std::string::npos) {
      result.status_text = header.substr(status_end + 1);
      trimTrailingCarriageReturn(result.status_text);
      return;
    }
    result.status_text = header.substr(status_end + 1, text_end - status_end - 1);
    trimTrailingCarriageReturn(result.status_text);
    parseHeaders(header.substr(text_end + 1), result);
  }
};

#else

// Test hooks — symbols defined in packages/core/test/fake_esp_http_client.cpp.
// In real non-ESP builds (e.g. the simulator's WASM build) these are weak no-ops.
__attribute__((weak)) void test_record_request(const std::string &, const gea::host::FetchRequestInit &) {}
__attribute__((weak)) gea::host::FetchResponse test_canned_response(const std::string &) {
  return {};
}

#endif

class FetchHost {
 public:
  static gea::host::FetchResponse fetch(const std::string &url) {
    return fetch(url, gea::host::FetchRequestInit{});
  }
  static gea::host::FetchResponse fetch(const std::string &url, const gea::host::FetchRequestInit &init) {
#ifdef __EMSCRIPTEN__
    return BrowserFetchBridge::fetch(url, init);
#else
    test_record_request(url, init);
    return test_canned_response(url);
#endif
  }
};

}  // namespace gea::framework::host

gea::host::FetchResponse gea::host::fetch(const std::string &url) {
  return gea::framework::host::FetchHost::fetch(url, FetchRequestInit{});
}

gea::host::FetchResponse gea::host::fetch(const std::string &url, const FetchRequestInit &init) {
  return gea::framework::host::FetchHost::fetch(url, init);
}

// Off-device (browser/sim/tests): no streaming upload from a device file path.
gea::host::FetchResponse gea::host::uploadFile(const std::string &, const std::string &,
                                               const std::string &, const std::string &,
                                               const std::string &, const std::string &) {
  return gea::host::FetchResponse{};
}

// Off-device: no streaming download to a device file path.
gea::host::FetchResponse gea::host::downloadFile(const std::string &, const std::string &,
                                                 const std::string &) {
  return gea::host::FetchResponse{};
}

#endif  // ESP_PLATFORM && network capability

namespace gea::host {
namespace {

struct AsyncFetchJob {
  std::string url;
  FetchRequestInit init;
  FetchResponse response;
  bool ready = false;
  // Streaming-upload mode (fetchUploadFileAsync): when is_upload is set, the job
  // runs uploadFile(...) instead of fetch(...), streaming up_path off disk.
  bool is_upload = false;
  std::string up_auth;
  std::string up_ctype;
  std::string up_prefix;
  std::string up_path;
  std::string up_suffix;
  // Streaming-download mode (fetchDownloadFileAsync): GET `url` -> `dl_path` on
  // disk, streamed in chunks (never buffered whole in RAM).
  bool is_download = false;
  std::string dl_auth;
  std::string dl_path;
};

std::mutex &asyncFetchMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<int, std::shared_ptr<AsyncFetchJob>> &asyncFetchJobs() {
  static std::map<int, std::shared_ptr<AsyncFetchJob>> jobs;
  return jobs;
}

int nextAsyncFetchId() {
  static int next = 1;
  return next++;
}

void runAsyncFetchJob(const std::shared_ptr<AsyncFetchJob> &job) {
  FetchResponse response =
      job->is_download ? downloadFile(job->url, job->dl_auth, job->dl_path)
      : job->is_upload ? uploadFile(job->url, job->up_auth, job->up_ctype, job->up_prefix, job->up_path, job->up_suffix)
                       : fetch(job->url, job->init);
  std::lock_guard<std::mutex> lock(asyncFetchMutex());
  job->response = response;
  job->ready = true;
}

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)
constexpr uint32_t kAsyncFetchStackBytes = 32768;
constexpr UBaseType_t kAsyncFetchQueueDepth = 16;
constexpr int kAsyncFetchWorkers = 2;

// A small pool of long-lived workers runs the async fetches. A task per request needed a fresh contiguous
// 32 KB stack each time, which a long-running app with fragmented PSRAM eventually cannot find: the
// request then never starts. The workers' stacks are taken once, at the first request, and a second
// worker keeps short requests moving while a long upload or download holds the first.
void asyncFetchWorker(void *raw) {
  auto queue = static_cast<QueueHandle_t>(raw);
  for (;;) {
    std::shared_ptr<AsyncFetchJob> *payload = nullptr;
    if (xQueueReceive(queue, &payload, portMAX_DELAY) != pdTRUE) continue;
    std::unique_ptr<std::shared_ptr<AsyncFetchJob>> holder(payload);
    runAsyncFetchJob(*holder);
    // Drop the job (request body and response) and the keep-alive connection and response buffer
    // between requests, so an idle worker holds no socket and no large buffer.
    holder.reset();
    gea::framework::host::FetchHost::releaseTaskState();
  }
}

// Starts the workers on first use; null while none can be started (the next request tries again).
QueueHandle_t asyncFetchQueue() {
  static std::mutex startMutex;
  static QueueHandle_t queue = nullptr;
  std::lock_guard<std::mutex> lock(startMutex);
  if (queue) return queue;

  QueueHandle_t created = xQueueCreate(kAsyncFetchQueueDepth, sizeof(std::shared_ptr<AsyncFetchJob> *));
  if (!created) return nullptr;

  int started = 0;
  for (int i = 0; i < kAsyncFetchWorkers; ++i) {
    const BaseType_t ok = xTaskCreateWithCaps(asyncFetchWorker, "gea_fetch", kAsyncFetchStackBytes, created, 5,
                                              nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) ++started;
  }
  if (started == 0) {
    vQueueDelete(created);
    return nullptr;
  }

  queue = created;
  return queue;
}
#endif

double spawnAsyncJob(const std::shared_ptr<AsyncFetchJob> &job) {
  int id = 0;
  {
    std::lock_guard<std::mutex> lock(asyncFetchMutex());
    id = nextAsyncFetchId();
    asyncFetchJobs()[id] = job;
  }

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)
  auto *payload = new std::shared_ptr<AsyncFetchJob>(job);
  QueueHandle_t queue = asyncFetchQueue();
  if (!queue || xQueueSend(queue, &payload, 0) != pdTRUE) {
    delete payload;
    std::lock_guard<std::mutex> lock(asyncFetchMutex());
    asyncFetchJobs().erase(id);
    ESP_LOGE("gea::host::fetch", "async fetch not queued (worker missing or queue full); internal largest=%u psram largest=%u",
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    return 0;
  }
#elif defined(__EMSCRIPTEN__)
  runAsyncFetchJob(job);
#else
  std::thread([job]() {
    runAsyncFetchJob(job);
  }).detach();
#endif

  return static_cast<double>(id);
}

double enqueueAsyncFetch(const std::string &url, const FetchRequestInit &init) {
  auto job = std::make_shared<AsyncFetchJob>();
  job->url = url;
  job->init = init;
  return spawnAsyncJob(job);
}

double enqueueAsyncUpload(const std::string &url, const std::string &authorization,
                          const std::string &contentType, const std::string &prefix,
                          const std::string &filePath, const std::string &suffix) {
  auto job = std::make_shared<AsyncFetchJob>();
  job->is_upload = true;
  job->url = url;
  job->up_auth = authorization;
  job->up_ctype = contentType;
  job->up_prefix = prefix;
  job->up_path = filePath;
  job->up_suffix = suffix;
  return spawnAsyncJob(job);
}

double enqueueAsyncDownload(const std::string &url, const std::string &authorization,
                            const std::string &destPath) {
  auto job = std::make_shared<AsyncFetchJob>();
  job->is_download = true;
  job->url = url;
  job->dl_auth = authorization;
  job->dl_path = destPath;
  return spawnAsyncJob(job);
}

int fetchId(double id) {
  return static_cast<int>(id);
}

}  // namespace

double fetchAsync(const std::string &url) {
  return fetchAsync(url, FetchRequestInit{});
}

double fetchAsync(const std::string &url, const FetchRequestInit &init) {
  return enqueueAsyncFetch(url, init);
}

double fetchUploadFileAsync(const std::string &url, const std::string &authorization,
                            const std::string &contentType, const std::string &prefix,
                            const std::string &filePath, const std::string &suffix) {
  return enqueueAsyncUpload(url, authorization, contentType, prefix, filePath, suffix);
}

double fetchDownloadFileAsync(const std::string &url, const std::string &authorization,
                              const std::string &destPath) {
  return enqueueAsyncDownload(url, authorization, destPath);
}

bool fetchReady(double id) {
  std::lock_guard<std::mutex> lock(asyncFetchMutex());
  const auto it = asyncFetchJobs().find(fetchId(id));
  return it != asyncFetchJobs().end() && it->second->ready;
}

FetchResponse fetchResult(double id) {
  std::lock_guard<std::mutex> lock(asyncFetchMutex());
  const auto it = asyncFetchJobs().find(fetchId(id));
  return it == asyncFetchJobs().end() ? FetchResponse{} : it->second->response;
}

void fetchRelease(double id) {
  std::lock_guard<std::mutex> lock(asyncFetchMutex());
  asyncFetchJobs().erase(fetchId(id));
}

}  // namespace gea::host
