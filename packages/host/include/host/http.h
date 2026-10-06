// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// A lightweight device-hosted HTTP server, modeled after Node's `http` package:
// `http.createServer(handler)` returns a server; the handler is called once per
// request and RETURNS a plain reply record `{ status, contentType, body, file,
// download }`. Like the WebSocket facade, the handler crosses the JS<->native
// boundary as a boxed callable and is invoked on the FRAME task (drained by
// `runRequests()` from the app frame loop), so app logic stays in TypeScript.
//
// The reply is data, never a live object: there is no `res` with mutating
// methods. A reply that sets `file` streams that path straight off disk on the
// server worker task, so large recordings (tens of MB of WAV) are sent without
// ever materializing in JS or the frame task.

namespace gea::host {

using NativeHttpServerHandle = std::uint32_t;

// The request a handler sees (built natively from the incoming HTTP request).
struct HttpRequest {
  std::string method; // "GET", "POST", ...
  std::string path;   // request path, query stripped (e.g. "/note_003.wav")
  std::string query;  // raw query string after '?' (e.g. "tag=Idea"), or ""
};

// What a handler returns. Either an inline `body`, or a `file` path to stream
// from disk. `download` (when set) turns the response into an attachment.
struct HttpReply {
  int status = 200;
  std::string contentType = "text/plain";
  std::string body;
  std::string file;
  std::string download;
};

namespace http {

// Handler signature on the native side. Runs on the frame task. Pure value
// types in the signature (no gea_cpp_value), so this file's non-generated TUs
// can store and call it without the dynamic-value runtime.
using RequestHandler = std::function<HttpReply(const HttpRequest &)>;

// Registers a handler, allocates a server handle, returns it. Defined in
// http.cpp; called by create_server() below.
NativeHttpServerHandle register_handler(RequestHandler handler);
void unregister_handler(NativeHttpServerHandle handle);

std::unordered_map<NativeHttpServerHandle, RequestHandler> &handlerTable();

// Frame-loop pump: drains requests posted by the server worker, runs each
// handler on the frame task, fills the reply, and releases the waiting worker.
void runRequests();

// Platform transport (esp_http_server on device; weak no-ops elsewhere).
bool platform_listen(NativeHttpServerHandle handle, int port);
void platform_close(NativeHttpServerHandle handle);

// Stop + unregister a server by its handle. Lets an app persist just the
// numeric handle (a plain double cell — no boxing) instead of the HttpServer
// object across module scope. Handle 0 is a no-op.
inline void close_server(double handle) {
  const NativeHttpServerHandle h = static_cast<NativeHttpServerHandle>(handle);
  if (h == 0)
    return;
  platform_close(h);
  unregister_handler(h);
}

// Test seam (non-ESP): dispatch a request synchronously through the registered
// handler and return its reply, without any sockets.
HttpReply test_dispatch(NativeHttpServerHandle handle,
                        const std::string &method, const std::string &path,
                        const std::string &query);

#ifdef GEA_CPP_VALUE_AVAILABLE
// Builds a native RequestHandler that marshals the request into a JS record,
// invokes the boxed JS callable, and reads back the reply record. Instantiated
// only in generated TUs (where gea_cpp_value exists), matching the WebSocket
// callback lowering.
inline NativeHttpServerHandle create_server(gea_cpp_value handler) {
  if (handler.kind != gea_cpp_value::kind_t::callable || !handler.callable) {
    return register_handler(nullptr);
  }
  auto callable = handler.callable;
  RequestHandler native = [callable](const HttpRequest &req) -> HttpReply {
    gea_cpp_value request;
    request.kind = gea_cpp_value::kind_t::record;
    request.entries =
        std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
    request.record_set_literal("method", gea_cpp_value(req.method));
    request.record_set_literal("path", gea_cpp_value(req.path));
    request.record_set_literal(
        "url", gea_cpp_value(req.query.empty() ? req.path
                                               : req.path + "?" + req.query));
    request.record_set_literal("query", gea_cpp_value(req.query));

    const gea_cpp_value result =
        (*callable)(std::vector<gea_cpp_value>{request});

    HttpReply reply;
    if (result.kind != gea_cpp_value::kind_t::record) {
      // A handler that returns nothing useful => 404.
      reply.status = 404;
      reply.contentType = "text/plain";
      reply.body = "Not found";
      return reply;
    }

    const gea_cpp_value status = result.record_get_literal("status");
    if (status.kind == gea_cpp_value::kind_t::number && status.number > 0) {
      reply.status = static_cast<int>(status.number);
    }
    const gea_cpp_value contentType = result.record_get_literal("contentType");
    if (contentType.kind == gea_cpp_value::kind_t::string &&
        !contentType.text.empty()) {
      reply.contentType = contentType.text;
    }
    const gea_cpp_value file = result.record_get_literal("file");
    if (file.kind == gea_cpp_value::kind_t::string && !file.text.empty()) {
      reply.file = file.text;
    }
    const gea_cpp_value download = result.record_get_literal("download");
    if (download.kind == gea_cpp_value::kind_t::string &&
        !download.text.empty()) {
      reply.download = download.text;
    }
    const gea_cpp_value body = result.record_get_literal("body");
    if (body.kind == gea_cpp_value::kind_t::string) {
      reply.body = body.text;
    }
    return reply;
  };
  return register_handler(std::move(native));
}
#endif // GEA_CPP_VALUE_AVAILABLE

} // namespace http

// A server handle. `createServer()` lowers to `http::create_server(...)`, whose
// double return is wrapped back into this by the plugin's nativeTypes mapping,
// so `server.listen(...)` / `server.close()` lower to direct native calls.
class HttpServer {
public:
  NativeHttpServerHandle nativeHandle = 0;

  constexpr HttpServer() = default;
  explicit HttpServer(NativeHttpServerHandle h) : nativeHandle(h) {}
  explicit HttpServer(double h)
      : nativeHandle(static_cast<NativeHttpServerHandle>(h)) {}

  constexpr operator double() const {
    return static_cast<double>(nativeHandle);
  }

  // Numeric handle, so apps can persist it as a plain number and close later
  // via `http.close(handle)` without storing (and boxing) the object.
  double id() const { return static_cast<double>(nativeHandle); }
  bool listen(double port) const {
    return http::platform_listen(nativeHandle, static_cast<int>(port));
  }
  void close() const {
    http::platform_close(nativeHandle);
    http::unregister_handler(nativeHandle);
  }
};

} // namespace gea::host
