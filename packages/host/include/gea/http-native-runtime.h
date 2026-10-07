// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gea_runtime.h"
#include "host/http.h"

namespace gea::host::http {
namespace detail {

template <typename T> T &mutableRecord(T &value) { return value; }

template <typename T> T &mutableRecord(gea::Ref<T> &value) {
  value = gea::makeRef<T>();
  return *value;
}

template <typename T> const T &record(const T &value) { return value; }

template <typename T> const T &record(const gea::Ref<T> &value) {
  return *value;
}

template <typename T> T field(const T &value, const T &) { return value; }

template <typename T>
T field(const gea::Optional<T> &value, const T &fallback) {
  return value.has_value() ? *value : fallback;
}

template <typename T> gea::Ref<T> arrayField(const gea::Ref<T> &value) {
  return value;
}

template <typename T>
gea::Ref<T> arrayField(const gea::Optional<gea::Ref<T>> &value) {
  return value.has_value() ? *value : gea::Ref<T>{};
}

template <typename T> struct RequestArgument {
  static T make(const HttpRequest &request) {
    T value{};
    value.method = request.method;
    value.path = request.path;
    value.query = request.query;
    value.url = request.query.empty() ? request.path
                                      : request.path + "?" + request.query;
    if constexpr (requires { value.body; }) {
      value.body =
          gea::makeRef<gea::TypedArray<std::uint8_t>>(request.body.size());
      std::copy(request.body.begin(), request.body.end(), value.body->data());
    }
    if constexpr (requires { value.headers; }) {
      using Array =
          typename std::remove_cvref_t<decltype(value.headers)>::element_type;
      using Header =
          std::remove_cvref_t<decltype(std::declval<Array>().cells[0].value)>;
      value.headers = gea::makeRef<Array>();
      for (const auto &header : request.headers) {
        Header entry{};
        auto &fields = mutableRecord(entry);
        fields.name = header.name;
        fields.value = header.value;
        value.headers->push(entry);
      }
    }
    return value;
  }
};

template <typename T> struct RequestArgument<gea::Ref<T>> {
  static gea::Ref<T> make(const HttpRequest &request) {
    return gea::makeRef<T>(RequestArgument<T>::make(request));
  }
};

template <typename Handler, typename Argument>
NativeHttpServerHandle registerTypedHandler(Handler handler) {
  return register_handler([handler](const HttpRequest &request) -> HttpReply {
    const auto result =
        handler(RequestArgument<std::remove_cvref_t<Argument>>::make(request));
    const auto &value = record(result);
    HttpReply reply;
    if constexpr (requires { value.status; }) {
      reply.status = static_cast<int>(field(value.status, 200.0));
    }
    if constexpr (requires { value.contentType; }) {
      reply.contentType = field(value.contentType, std::string("text/plain"));
    }
    if constexpr (requires { value.body; }) {
      reply.body = field(value.body, std::string{});
    }
    if constexpr (requires { value.file; }) {
      reply.file = field(value.file, std::string{});
    }
    if constexpr (requires { value.download; }) {
      reply.download = field(value.download, std::string{});
    }
    if constexpr (requires { value.headers; }) {
      const auto headers = arrayField(value.headers);
      if (headers) {
        for (const auto &cell : headers->cells) {
          const auto &header = record(cell.value);
          reply.headers.push_back({header.name, header.value});
        }
      }
    }
    return reply;
  });
}

} // namespace detail

template <typename Result, typename Argument>
NativeHttpServerHandle
create_typed_server(gea::CallableObject<Result(Argument)> handler) {
  return detail::registerTypedHandler<decltype(handler), Argument>(
      std::move(handler));
}

template <typename Result, typename Argument>
NativeHttpServerHandle
create_typed_server(std::function<Result(Argument)> handler) {
  return detail::registerTypedHandler<decltype(handler), Argument>(
      std::move(handler));
}

} // namespace gea::host::http
