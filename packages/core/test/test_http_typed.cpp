// SPDX-License-Identifier: Apache-2.0
#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#include "gea/http-native-runtime.h"
#include <cassert>

struct Header {
  std::string name, value;
};
using Headers = gea::Ref<gea::ArrayObject<gea::Ref<Header>>>;

struct Request {
  std::string method, path, query, url;
  gea::Ref<gea::TypedArray<std::uint8_t>> body;
  Headers headers;
};

struct Reply {
  gea::Optional<double> status;
  gea::Optional<Headers> headers;
  gea::Optional<std::string> contentType, body, file, download;
};

int main() {
  gea::CallableObject<gea::Ref<Reply>(gea::Ref<Request>)> handler(
      [](void *, gea::Ref<Request> request) {
        assert(request->url == "/upload?slot=2");
        assert(request->body->size() == 4);
        assert(request->body->data()[0] == 255);
        assert(request->headers->size() == 1);
        assert(request->headers->cells[0].value->name == "Content-Type");
        assert(request->headers->cells[0].value->value == "image/jpeg");
        auto reply = gea::makeRef<Reply>();
        reply->status = 201.0;
        reply->body = std::string("stored");
        auto headers = gea::makeRef<gea::ArrayObject<gea::Ref<Header>>>();
        auto location = gea::makeRef<Header>();
        location->name = "Location";
        location->value = "http://192.168.4.1/";
        headers->push(location);
        reply->headers = headers;
        return reply;
      },
      nullptr);
  const auto server = gea::host::http::create_typed_server(handler);
  gea::host::HttpRequest request{"POST",
                                 "/upload",
                                 "slot=2",
                                 {255, 216, 255, 217},
                                 {{"Content-Type", "image/jpeg"}}};
  const auto reply = gea::host::http::handlerTable().at(server)(request);
  assert(reply.status == 201);
  assert(reply.contentType == "text/plain");
  assert(reply.body == "stored");
  assert(reply.file.empty());
  assert(reply.headers.size() == 1);
  assert(reply.headers[0].name == "Location");
  assert(reply.headers[0].value == "http://192.168.4.1/");
  constexpr char raw[] =
      "Host: watch\r\nContent-Type: image/jpeg\0\0X-File-Name: photo.jpg\0";
  const auto parsed =
      gea::host::parseHttpHeaders(std::string_view(raw, sizeof(raw) - 1));
  assert(parsed.size() == 3);
  assert(parsed[1].value == "image/jpeg");
  assert(!gea::host::validHttpHeader({"Bad Name", "value"}));
  assert(!gea::host::validHttpHeader({"Location", "value\r\nInjected: yes"}));
  gea::host::http::unregister_handler(server);
  assert(!gea::host::http::handlerTable().contains(server));
}
