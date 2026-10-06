// SPDX-License-Identifier: Apache-2.0
// This library TU provides only the platform/transport parts of the http
// facade (register_handler, runRequests, platform_listen). The gea_cpp_value
// reply-parsing `create_server` is header-inline and compiled in the geatsc app
// TU, where the full runtime value type exists. The native test harness defines
// GEA_CPP_VALUE_AVAILABLE globally, so disable it here (matching
// host/fetch.cpp) to keep that value-dependent block out of this TU, which
// lacks the type.
#ifdef GEA_CPP_VALUE_AVAILABLE
#define GEA_HTTP_RESTORE_CPP_VALUE_AVAILABLE 1
#undef GEA_CPP_VALUE_AVAILABLE
#endif
#include "host/http.h"
#ifdef GEA_HTTP_RESTORE_CPP_VALUE_AVAILABLE
#define GEA_CPP_VALUE_AVAILABLE 1
#undef GEA_HTTP_RESTORE_CPP_VALUE_AVAILABLE
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace gea::host::http {

namespace {

// A request handed from a server worker task to the frame task and back. The
// worker fills `req`, posts it, and waits on `done`; the frame task runs the
// handler, fills `reply`, and sets `done`.
struct PendingRequest {
  NativeHttpServerHandle server = 0;
  HttpRequest req;
  HttpReply reply;
  std::atomic<bool> done{false};
};

std::mutex &queueMutex() {
  static std::mutex m;
  return m;
}

std::vector<std::shared_ptr<PendingRequest>> &requestQueue() {
  static std::vector<std::shared_ptr<PendingRequest>> q;
  return q;
}

std::mutex &handlerMutex() {
  static std::mutex m;
  return m;
}

NativeHttpServerHandle nextHandle() {
  static std::atomic<NativeHttpServerHandle> id{1};
  return id++;
}

// Post a request and block until the frame task has produced a reply (or the
// deadline passes). Returns the filled reply. Called on the server worker task.
HttpReply dispatchAndWait(NativeHttpServerHandle server, HttpRequest req,
                          int timeoutMs, int pollMs,
                          const std::function<void(int)> &sleep) {
  auto pending = std::make_shared<PendingRequest>();
  pending->server = server;
  pending->req = std::move(req);
  {
    std::lock_guard<std::mutex> lock(queueMutex());
    requestQueue().push_back(pending);
  }
  int waited = 0;
  while (!pending->done.load(std::memory_order_acquire)) {
    if (waited >= timeoutMs) {
      HttpReply timeout;
      timeout.status = 504;
      timeout.contentType = "text/plain";
      timeout.body = "Gateway Timeout";
      // The frame task may still resolve `pending` later; it just won't be
      // read. The shared_ptr keeps it alive until both sides drop it.
      return timeout;
    }
    sleep(pollMs);
    waited += pollMs;
  }
  return pending->reply;
}

} // namespace

std::unordered_map<NativeHttpServerHandle, RequestHandler> &handlerTable() {
  static std::unordered_map<NativeHttpServerHandle, RequestHandler> table;
  return table;
}

NativeHttpServerHandle register_handler(RequestHandler handler) {
  const NativeHttpServerHandle handle = nextHandle();
  std::lock_guard<std::mutex> lock(handlerMutex());
  handlerTable()[handle] = std::move(handler);
  return handle;
}

void unregister_handler(NativeHttpServerHandle handle) {
  std::lock_guard<std::mutex> lock(handlerMutex());
  handlerTable().erase(handle);
}

void runRequests() {
  std::vector<std::shared_ptr<PendingRequest>> drained;
  {
    std::lock_guard<std::mutex> lock(queueMutex());
    if (requestQueue().empty())
      return;
    drained.swap(requestQueue());
  }
  for (const auto &pending : drained) {
    RequestHandler handler;
    {
      std::lock_guard<std::mutex> lock(handlerMutex());
      auto it = handlerTable().find(pending->server);
      if (it != handlerTable().end())
        handler = it->second;
    }
    if (handler) {
      pending->reply = handler(pending->req);
    } else {
      pending->reply.status = 404;
      pending->reply.contentType = "text/plain";
      pending->reply.body = "Not found";
    }
    pending->done.store(true, std::memory_order_release);
  }
}

HttpReply test_dispatch(NativeHttpServerHandle handle,
                        const std::string &method, const std::string &path,
                        const std::string &query) {
  RequestHandler handler;
  {
    std::lock_guard<std::mutex> lock(handlerMutex());
    auto it = handlerTable().find(handle);
    if (it != handlerTable().end())
      handler = it->second;
  }
  HttpReply reply;
  if (!handler) {
    reply.status = 404;
    reply.body = "Not found";
    return reply;
  }
  HttpRequest req;
  req.method = method;
  req.path = path;
  req.query = query;
  return handler(req);
}

} // namespace gea::host::http

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace gea::host::http {

namespace {

constexpr const char *kTag = "gea::host::http";
// The frame task drains requests every frame; e-paper frames can be ~1s, so
// allow generous slack before declaring a gateway timeout.
constexpr int kRendezvousTimeoutMs = 8000;
constexpr int kRendezvousPollMs = 10;
constexpr std::size_t kStreamChunkBytes = 4096;

std::unordered_map<NativeHttpServerHandle, httpd_handle_t> &espServers() {
  static std::unordered_map<NativeHttpServerHandle, httpd_handle_t> map;
  return map;
}

std::string methodName(int method) {
  switch (method) {
  case HTTP_POST:
    return "POST";
  case HTTP_PUT:
    return "PUT";
  case HTTP_DELETE:
    return "DELETE";
  case HTTP_HEAD:
    return "HEAD";
  case HTTP_PATCH:
    return "PATCH";
  default:
    return "GET";
  }
}

const char *statusLine(int status) {
  switch (status) {
  case 200:
    return "200 OK";
  case 204:
    return "204 No Content";
  case 303:
    return "303 See Other";
  case 400:
    return "400 Bad Request";
  case 404:
    return "404 Not Found";
  case 500:
    return "500 Internal Server Error";
  case 504:
    return "504 Gateway Timeout";
  default:
    return "200 OK";
  }
}

// Stream a file from disk to the client in chunks, so multi-MB recordings never
// have to fit in RAM.
esp_err_t sendFile(httpd_req_t *r, const HttpReply &reply) {
  FILE *f = std::fopen(reply.file.c_str(), "rb");
  if (!f) {
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_sendstr(r, "File not found");
    return ESP_OK;
  }
  httpd_resp_set_status(r, statusLine(reply.status));
  httpd_resp_set_type(r, reply.contentType.c_str());
  if (!reply.download.empty()) {
    const std::string disposition =
        "attachment; filename=\"" + reply.download + "\"";
    httpd_resp_set_hdr(r, "Content-Disposition", disposition.c_str());
  }
  auto *buffer = static_cast<char *>(std::malloc(kStreamChunkBytes));
  if (!buffer) {
    std::fclose(f);
    httpd_resp_send_chunk(r, nullptr, 0);
    return ESP_FAIL;
  }
  esp_err_t result = ESP_OK;
  for (;;) {
    const std::size_t read = std::fread(buffer, 1, kStreamChunkBytes, f);
    if (read > 0 && httpd_resp_send_chunk(r, buffer, read) != ESP_OK) {
      result = ESP_FAIL;
      break;
    }
    if (read < kStreamChunkBytes)
      break; // EOF (short read)
  }
  std::free(buffer);
  std::fclose(f);
  if (result == ESP_OK)
    httpd_resp_send_chunk(r, nullptr, 0); // terminate
  return result;
}

esp_err_t wildcardHandler(httpd_req_t *r) {
  const auto server = static_cast<NativeHttpServerHandle>(
      reinterpret_cast<std::uintptr_t>(r->user_ctx));

  // r->uri is a fixed char array (never null); construct directly to avoid a
  // -Werror=address "address will never be NULL" warning.
  std::string uri =
      (r->uri[0] != '\0') ? std::string(r->uri) : std::string("/");
  std::string path = uri;
  std::string query;
  const std::size_t q = uri.find('?');
  if (q != std::string::npos) {
    path = uri.substr(0, q);
    query = uri.substr(q + 1);
  }

  HttpRequest req;
  req.method = methodName(r->method);
  req.path = path;
  req.query = query;

  const HttpReply reply = dispatchAndWait(
      server, std::move(req), kRendezvousTimeoutMs, kRendezvousPollMs,
      [](int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });

  if (!reply.file.empty())
    return sendFile(r, reply);

  httpd_resp_set_status(r, statusLine(reply.status));
  httpd_resp_set_type(r, reply.contentType.c_str());
  if (!reply.download.empty()) {
    const std::string disposition =
        "attachment; filename=\"" + reply.download + "\"";
    httpd_resp_set_hdr(r, "Content-Disposition", disposition.c_str());
  }
  httpd_resp_send(r, reply.body.data(), reply.body.size());
  return ESP_OK;
}

} // namespace

bool platform_listen(NativeHttpServerHandle handle, int port) {
  if (espServers().count(handle))
    return true; // already listening

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = static_cast<std::uint16_t>(port);
  // Every esp_http_server instance defaults to the SAME internal control-socket
  // port (32768); a second instance (the diagnostics server already owns the
  // default) then fails httpd_start with ESP_FAIL. Derive a unique ctrl port
  // from the listen port so the portal coexists with diagnostics/OTA servers.
  config.ctrl_port = static_cast<std::uint16_t>(32768 + (port % 1000) + 1);
  config.stack_size =
      4096; // esp_http_server default; 8192 risked NO_MEM post-WiFi
  config.max_uri_handlers = 2;
  // A personal portal serves a page + an audio stream; a few sockets is plenty.
  // The default (7) competes with the diagnostics/OTA servers for the limited
  // LWIP socket pool, so keep this small.
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
  // Worker tasks block on the frame-task rendezvous; the recv/send timeouts
  // must exceed it so a slow frame doesn't drop the socket mid-request.
  config.recv_wait_timeout = 15;
  config.send_wait_timeout = 15;

  ESP_LOGI(
      kTag,
      "httpd_start :%d heap int8[free=%u largest=%u] dma[free=%u largest=%u]",
      port,
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
  httpd_handle_t server = nullptr;
  const esp_err_t startErr = httpd_start(&server, &config);
  if (startErr != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed on port %d: %s", port,
             esp_err_to_name(startErr));
    return false;
  }

  httpd_uri_t wildcard = {};
  wildcard.uri = "/*";
  wildcard.method = HTTP_GET;
  wildcard.handler = wildcardHandler;
  wildcard.user_ctx =
      reinterpret_cast<void *>(static_cast<std::uintptr_t>(handle));
  httpd_register_uri_handler(server, &wildcard);

  espServers()[handle] = server;
  ESP_LOGI(kTag, "listening on :%d (handle %u)", port,
           static_cast<unsigned>(handle));
  return true;
}

void platform_close(NativeHttpServerHandle handle) {
  auto it = espServers().find(handle);
  if (it == espServers().end())
    return;
  if (it->second)
    httpd_stop(it->second);
  espServers().erase(it);
  ESP_LOGI(kTag, "stopped (handle %u)", static_cast<unsigned>(handle));
}

} // namespace gea::host::http

#else // no ESP network capability

namespace gea::host::http {

// Off-device (host tools, simulator, unit tests): no real socket. listen/close
// are no-ops; tests drive handlers through test_dispatch(). Weak so a platform
// can override with a real implementation.
__attribute__((weak)) bool platform_listen(NativeHttpServerHandle, int) {
  return false;
}
__attribute__((weak)) void platform_close(NativeHttpServerHandle) {}

} // namespace gea::host::http

#endif // ESP_PLATFORM && network capability
