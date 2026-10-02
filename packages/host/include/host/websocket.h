// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gea::host {

using NativeWebSocketHandle = std::uint32_t;

class WebSocketOnOpenProperty {
 public:
  NativeWebSocketHandle handle = 0;
  constexpr WebSocketOnOpenProperty() = default;
  explicit constexpr WebSocketOnOpenProperty(NativeWebSocketHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const WebSocketOnOpenProperty &operator=(gea_cpp_value callback) const;
#endif
};

class WebSocketOnMessageProperty {
 public:
  NativeWebSocketHandle handle = 0;
  constexpr WebSocketOnMessageProperty() = default;
  explicit constexpr WebSocketOnMessageProperty(NativeWebSocketHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const WebSocketOnMessageProperty &operator=(gea_cpp_value callback) const;
#endif
};

class WebSocketOnCloseProperty {
 public:
  NativeWebSocketHandle handle = 0;
  constexpr WebSocketOnCloseProperty() = default;
  explicit constexpr WebSocketOnCloseProperty(NativeWebSocketHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const WebSocketOnCloseProperty &operator=(gea_cpp_value callback) const;
#endif
};

class WebSocketOnErrorProperty {
 public:
  NativeWebSocketHandle handle = 0;
  constexpr WebSocketOnErrorProperty() = default;
  explicit constexpr WebSocketOnErrorProperty(NativeWebSocketHandle h) : handle(h) {}
#ifdef GEA_CPP_VALUE_AVAILABLE
  const WebSocketOnErrorProperty &operator=(gea_cpp_value callback) const;
#endif
};

class WebSocket {
 public:
  static constexpr int CONNECTING = 0;
  static constexpr int OPEN = 1;
  static constexpr int CLOSING = 2;
  static constexpr int CLOSED = 3;

  NativeWebSocketHandle nativeHandle = 0;
  mutable WebSocketOnOpenProperty onopen;
  mutable WebSocketOnMessageProperty onmessage;
  mutable WebSocketOnCloseProperty onclose;
  mutable WebSocketOnErrorProperty onerror;

  constexpr WebSocket() = default;
  explicit WebSocket(NativeWebSocketHandle h)
      : nativeHandle(h), onopen(h), onmessage(h), onclose(h), onerror(h) {}
  explicit WebSocket(double h)
      : nativeHandle(static_cast<NativeWebSocketHandle>(h)),
        onopen(nativeHandle), onmessage(nativeHandle), onclose(nativeHandle), onerror(nativeHandle) {}

  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  std::string url() const;
  double readyState() const;
  double bufferedAmount() const;
  void send(const std::string &data) const;
  void close() const;
  void close(double code) const;
  void close(double code, const std::string &reason) const;
};

namespace websocket {

struct CallbackTable {
  std::function<void()> on_open;
  std::function<void(const std::string &)> on_message;
  std::function<void(int, const std::string &)> on_close;
  std::function<void(const std::string &)> on_error;
};

std::unordered_map<NativeWebSocketHandle, CallbackTable> &callbackTable();

NativeWebSocketHandle create_handle(const std::string &url, const std::string &protocols = {});
template <typename Protocols>
NativeWebSocketHandle create_handle(const std::string &url, const Protocols &protocols) {
  std::string joined;
  for (std::size_t i = 0; i < protocols.size(); ++i) {
    if (i) joined += ",";
    joined += protocols.at(i);
  }
  return create_handle(url, joined);
}
void destroy_handle(NativeWebSocketHandle handle);

// Native streaming sources run on the socket's sender, never on the UI loop.
// An empty result means no packet is ready yet. The producer must not call JS.
using TextProducer = std::function<std::string()>;
void setTextProducer(NativeWebSocketHandle handle, TextProducer producer);
// A native sink can consume media before JS event dispatch. Returning false
// leaves an ordinary control message for the owner's normal onmessage handler.
using TextConsumer = std::function<bool(const std::string&)>;
void setTextConsumer(NativeWebSocketHandle handle, TextConsumer consumer);

void runCallbacks();

void test_inject_open(NativeWebSocketHandle handle);
void test_inject_message(NativeWebSocketHandle handle, const std::string &data);
void test_inject_close(NativeWebSocketHandle handle, int code, const std::string &reason);

void test_set_open_callback(NativeWebSocketHandle handle, std::function<void()> cb);
void test_set_message_callback(NativeWebSocketHandle handle, std::function<void(const std::string &)> cb);
void test_set_close_callback(NativeWebSocketHandle handle, std::function<void(int, const std::string &)> cb);
void test_set_error_callback(NativeWebSocketHandle handle, std::function<void(const std::string &)> cb);

}  // namespace websocket

#ifdef GEA_CPP_VALUE_AVAILABLE

inline const WebSocketOnOpenProperty &WebSocketOnOpenProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    websocket::callbackTable()[handle].on_open = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  websocket::callbackTable()[handle].on_open = [callable]() {
    (*callable)(std::vector<gea_cpp_value>{});
  };
  return *this;
}

inline const WebSocketOnMessageProperty &WebSocketOnMessageProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    websocket::callbackTable()[handle].on_message = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  websocket::callbackTable()[handle].on_message = [callable](const std::string &data) {
    gea_cpp_value event;
    event.kind = gea_cpp_value::kind_t::record;
    event.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
    event.record_set_literal("data", gea_cpp_value(data));
    (*callable)(std::vector<gea_cpp_value>{event});
  };
  return *this;
}

inline const WebSocketOnCloseProperty &WebSocketOnCloseProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    websocket::callbackTable()[handle].on_close = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  websocket::callbackTable()[handle].on_close = [callable](int code, const std::string &reason) {
    gea_cpp_value event;
    event.kind = gea_cpp_value::kind_t::record;
    event.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
    event.record_set_literal("code", gea_cpp_value(static_cast<double>(code)));
    event.record_set_literal("reason", gea_cpp_value(reason));
    event.record_set_literal("wasClean", gea_cpp_value(code == 1000));
    (*callable)(std::vector<gea_cpp_value>{event});
  };
  return *this;
}

inline const WebSocketOnErrorProperty &WebSocketOnErrorProperty::operator=(gea_cpp_value cb) const {
  if (cb.kind != gea_cpp_value::kind_t::callable || !cb.callable) {
    websocket::callbackTable()[handle].on_error = nullptr;
    return *this;
  }
  auto callable = cb.callable;
  websocket::callbackTable()[handle].on_error = [callable](const std::string &message) {
    gea_cpp_value event;
    event.kind = gea_cpp_value::kind_t::record;
    event.entries = std::make_shared<std::vector<std::pair<std::string, gea_cpp_value>>>();
    event.record_set_literal("message", gea_cpp_value(message));
    (*callable)(std::vector<gea_cpp_value>{event});
  };
  return *this;
}

#endif  // GEA_CPP_VALUE_AVAILABLE

}  // namespace gea::host
