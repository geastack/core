// SPDX-License-Identifier: Apache-2.0
// Linux desktop provider: link libcurl >= 7.86. Without ws/wss in the linked
// libcurl (Debian 12's 7.88.1) it still builds, and connections fail at runtime.
#if defined(__linux__) || defined(GEA_TEST_CURL_WEBSOCKET)
#include "host/websocket_platform.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <curl/curl.h>
#include <curl/websockets.h>
#include <deque>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unordered_map>
#if defined(GEA_TEST_CURL_PARTIAL_SEND)
#include <sys/socket.h>
#endif
#if LIBCURL_VERSION_NUM < 0x075600
#error "Desktop WebSocket requires libcurl >= 7.86"
#endif
namespace gea::host::websocket::curl_transport {
using Clock = std::chrono::steady_clock;
#if defined(GEA_TEST_CURL_PARTIAL_SEND)
std::atomic<unsigned> partialSends{0};
unsigned partial_send_count_for_test() { return partialSends.load(); }
#endif
CURLcode initialize() {
  static const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
  return result;
}
constexpr std::size_t maxBytes = 512 * 1024, maxMessages = 1024,
                      maxReceive = 1024 * 1024;
bool utf8(const std::string &text) {
  std::size_t i = 0;
  while (i < text.size()) {
    auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80)
      continue;
    unsigned count;
    unsigned scalar;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      scalar = c & 31;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      scalar = c & 15;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      scalar = c & 7;
    } else
      return false;
    const unsigned minimum = count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000;
    if (text.size() - i < count)
      return false;
    while (count--) {
      auto d = static_cast<unsigned char>(text[i++]);
      if ((d & 0xc0) != 0x80)
        return false;
      scalar = (scalar << 6) | (d & 63);
    }
    if (scalar < minimum || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff))
      return false;
  }
  return true;
}
struct State {
  NativeWebSocketHandle handle;
  std::string url, protocols, headers;
  std::mutex mutex;
  std::deque<std::string> outgoing;
  std::size_t bytes = 0, entries = 0;
  TextProducer producer;
  std::atomic<bool> stop{false}, closing{false}, opened{false}, finished{false};
  std::thread worker;
  explicit State(NativeWebSocketHandle h) : handle(h) {}
  ~State() {
    stop = true;
    if (worker.joinable())
      worker.join();
  }
  void fail(const char *message) {
    opened = false;
    if (!stop.load()) {
      transport_error(handle, message);
      transport_close(handle, 1006, "WebSocket transport failed");
    }
  }
  bool enqueue(std::string text) {
    std::lock_guard lock(mutex);
    if (!opened.load() || closing.load() || stop.load())
      return false;
    if (entries >= maxMessages || text.size() > maxBytes - bytes)
      throw std::length_error("WebSocket send queue exceeds bounded capacity");
    bytes += text.size();
    ++entries;
    outgoing.push_back(std::move(text));
    return true;
  }
  void run();
};
struct Registry {
  std::mutex mutex;
  std::unordered_map<NativeWebSocketHandle, std::shared_ptr<State>> live;
  std::vector<std::shared_ptr<State>> retired;
  ~Registry() {
    for (auto &[id, s] : live)
      s->stop = true;
    for (auto &s : retired)
      s->stop = true;
  }
};
Registry &registry() {
  static Registry r;
  return r;
}
std::shared_ptr<State> find(NativeWebSocketHandle h) {
  auto &r = registry();
  std::lock_guard lock(r.mutex);
  auto i = r.live.find(h);
  return i == r.live.end() ? nullptr : i->second;
}
struct Curl {
  CURL *easy = curl_easy_init();
  curl_slist *headers = nullptr;
  ~Curl() {
    if (headers)
      curl_slist_free_all(headers);
    if (easy)
      curl_easy_cleanup(easy);
  }
};
void State::run() {
  struct Done {
    State &s;
    ~Done() {
      s.opened = false;
      TextProducer retired;
      {
        std::lock_guard lock(s.mutex);
        s.outgoing.clear();
        retired = std::move(s.producer);
        s.bytes = s.entries = 0;
      }
      retired = {};
      s.finished.store(true, std::memory_order_release);
    }
  } done{*this};
  try {
    if (stop.load())
      return;
    if (closing.load()) {
      transport_close(handle, 1000, "WebSocket closed locally");
      return;
    }
    if (initialize() != CURLE_OK) {
      fail("WebSocket libcurl initialization failed");
      return;
    }
    const auto *version = curl_version_info(CURLVERSION_NOW);
    bool supported = false;
    const bool secure = url.rfind("wss://", 0) == 0;
    for (const char *const *p = version->protocols; p && *p; ++p)
      if (std::strcmp(*p, secure ? "wss" : "ws") == 0)
        supported = true;
    if (!supported) {
      fail("WebSocket protocol not enabled in libcurl");
      return;
    }
    Curl c;
    if (!c.easy) {
      fail("WebSocket allocation failed");
      return;
    }
    curl_easy_setopt(c.easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.easy, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c.easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(c.easy, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(c.easy, CURLOPT_NOSIGNAL, 1L);
#if defined(GEA_TEST_CURL_PARTIAL_SEND)
    // Test-only kernel backpressure; the production send algorithm is
    // unchanged.
    curl_easy_setopt(
        c.easy, CURLOPT_SOCKOPTFUNCTION,
        +[](void *, curl_socket_t socket, curlsocktype) -> int {
          const int bytes = 4096;
          return setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &bytes,
                            sizeof(bytes)) == 0
                     ? CURL_SOCKOPT_OK
                     : CURL_SOCKOPT_ERROR;
        });
#endif
    curl_easy_setopt(c.easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c.easy, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(
        c.easy, CURLOPT_XFERINFOFUNCTION,
        +[](void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          auto *s = static_cast<State *>(p);
          return s->stop.load() || s->closing.load();
        });
    // No redirects, cookies, credentials or TLS-verification overrides.
    if (!protocols.empty()) {
      c.headers = curl_slist_append(
          c.headers, ("Sec-WebSocket-Protocol: " + protocols).c_str());
      if (!c.headers)
        throw std::bad_alloc();
    }
    std::size_t at = 0;
    while (at < headers.size()) {
      auto end = headers.find("\r\n", at);
      auto line = headers.substr(at, end - at);
      auto next = curl_slist_append(c.headers, line.c_str());
      if (!next)
        throw std::bad_alloc();
      c.headers = next;
      at = end + 2;
    }
    if (c.headers)
      curl_easy_setopt(c.easy, CURLOPT_HTTPHEADER, c.headers);
    const auto connected = curl_easy_perform(c.easy);
    if (stop.load())
      return;
    if (closing.load()) {
      transport_close(handle, 1000, "WebSocket closed locally");
      return;
    }
    if (connected != CURLE_OK) {
      fail("WebSocket connection or upgrade failed");
      return;
    }
    opened = true;
    if (!transport_open(handle))
      return;
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c.easy, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) {
      fail("WebSocket socket unavailable");
      return;
    }
    std::string active, message, control;
    std::size_t sentAt = 0, frameAt = 0;
    bool haveActive = false, messageOpen = false;
    int messageType = 0;
    bool closeSent = false;
    auto sendDeadline = Clock::now(), closeDeadline = Clock::time_point::max();
    auto sendClose = [&](const std::string &payload) {
      std::size_t offset = 0;
      const auto deadline = Clock::now() + std::chrono::seconds(2);
      do {
        std::size_t sent = 0;
        const auto rc =
            curl_ws_send(c.easy, payload.data() + offset,
                         payload.size() - offset, &sent, 0, CURLWS_CLOSE);
        if (rc != CURLE_OK && rc != CURLE_AGAIN)
          return false;
        offset += sent;
        if (rc == CURLE_OK && offset == payload.size())
          return true;
        pollfd fd{socket, POLLOUT, 0};
        ::poll(&fd, 1, 10);
      } while (!stop.load() && Clock::now() < deadline);
      return false;
    };
    while (!stop.load()) {
      bool pending;
      {
        std::lock_guard lock(mutex);
        pending = entries != 0;
      }
      if (closing.load() && !closeSent && !pending) {
        if (!sendClose(std::string("\x03\xe8", 2))) {
          transport_close(handle, 1000, "WebSocket closed locally");
          return;
        }
        closeSent = true;
        closeDeadline = Clock::now() + std::chrono::seconds(2);
      }
      if (closeSent && Clock::now() >= closeDeadline) {
        transport_close(handle, 1000, "WebSocket closed locally");
        return;
      }
      if (!closing.load()) {
        TextProducer next;
        {
          std::lock_guard lock(mutex);
          next = producer;
        }
        if (next) {
          auto produced = next();
          if (!produced.empty())
            enqueue(std::move(produced));
        }
      }
      if (!haveActive) {
        std::lock_guard lock(mutex);
        if (!outgoing.empty()) {
          active = std::move(outgoing.front());
          outgoing.pop_front();
          sentAt = 0;
          haveActive = true;
          sendDeadline = Clock::now() + std::chrono::seconds(10);
        }
      }
      if (haveActive) {
        if (!utf8(active)) {
          fail("WebSocket text must be valid UTF-8");
          return;
        }
        std::size_t sent = 0;
        const auto rc =
            curl_ws_send(c.easy, active.data() + sentAt, active.size() - sentAt,
                         &sent, 0, CURLWS_TEXT);
        if (rc != CURLE_OK && rc != CURLE_AGAIN) {
          fail("WebSocket send failed");
          return;
        }
#if defined(GEA_TEST_CURL_PARTIAL_SEND)
        if (rc == CURLE_AGAIN || sent < active.size() - sentAt)
          ++partialSends;
#endif
        sentAt += sent;
        if (rc == CURLE_OK && sentAt == active.size()) {
          std::lock_guard lock(mutex);
          bytes -= active.size();
          --entries;
          haveActive = false;
          active.clear();
        } else if (Clock::now() >= sendDeadline) {
          fail("WebSocket send timed out");
          return;
        }
      }
      char buffer[16384];
      std::size_t received = 0;
#if LIBCURL_VERSION_NUM >= 0x080200
      const curl_ws_frame *meta = nullptr;
#else
      // curl_ws_recv took a non-const frame pointer until 8.2.0.
      curl_ws_frame *meta = nullptr;
#endif
      const auto rc =
          curl_ws_recv(c.easy, buffer, sizeof(buffer), &received, &meta);
      if (rc == CURLE_OK && meta) {
        const int flags = meta->flags;
        const auto offset = meta->offset, left = meta->bytesleft;
        if (offset < 0 || left < 0 ||
            static_cast<unsigned long long>(offset) + received +
                    static_cast<unsigned long long>(left) >
                maxReceive) {
          fail("WebSocket message exceeds 1 MiB");
          return;
        }
        if (flags & CURLWS_CLOSE) {
          if (offset == 0)
            control.clear();
          if (static_cast<std::size_t>(offset) != control.size() ||
              control.size() + received > 125) {
            fail("Invalid WebSocket close frame");
            return;
          }
          control.append(buffer, received);
          if (left == 0) {
            int code = 1000;
            std::string reason;
            if (control.size() >= 2) {
              code = (static_cast<unsigned char>(control[0]) << 8) |
                     static_cast<unsigned char>(control[1]);
              reason = control.substr(2);
            }
            if (!utf8(reason)) {
              fail("Invalid WebSocket close text");
              return;
            }
            if (!closeSent && !haveActive)
              sendClose(control);
            transport_close(handle, code, reason);
            return;
          }
        } else if (flags & (CURLWS_TEXT | CURLWS_BINARY)) {
          const int type = flags & CURLWS_TEXT ? 1 : 2;
          if (offset == 0) {
            frameAt = 0;
            if (!messageOpen) {
              message.clear();
              messageType = type;
            } else if (messageType != type) {
              fail("Invalid WebSocket message type");
              return;
            }
          }
          if (static_cast<std::size_t>(offset) != frameAt ||
              received > maxReceive - message.size()) {
            fail("Invalid or oversized WebSocket frame");
            return;
          }
          frameAt += received;
          message.append(buffer, received);
          if (left == 0) {
            messageOpen = (flags & CURLWS_CONT) != 0;
            if (!messageOpen) {
              if (messageType == 1) {
                if (!utf8(message)) {
                  fail("Invalid WebSocket UTF-8 text");
                  return;
                }
                transport_message(handle, std::move(message));
              }
              message.clear();
            }
          }
        }
        // libcurl automatically answers pings; controls never alter text
        // assembly.
      } else if (rc != CURLE_AGAIN) {
        if (closing.load())
          transport_close(handle, 1000, "WebSocket closed locally");
        else
          fail("WebSocket receive failed");
        return;
      }
      if (rc == CURLE_AGAIN) {
        pollfd fd{
            socket,
            static_cast<short>(
                POLLIN |
                ((haveActive || (closing.load() && !closeSent)) ? POLLOUT : 0)),
            0};
        ::poll(&fd, 1, 10);
      }
    }
  } catch (...) {
    fail("WebSocket transport failed");
  }
}
} // namespace gea::host::websocket::curl_transport
namespace gea::host::websocket {
void platform_open(NativeWebSocketHandle h, const std::string &url,
                   const std::string &protocols, const std::string &headers) {
  if (curl_transport::initialize() != CURLE_OK) {
    transport_error(h, "WebSocket libcurl initialization failed");
    transport_close(h, 1006, "WebSocket connection rejected");
    return;
  }
  // URL parser is libcurl's; credentials and fragments are never admitted.
  auto *parsed = curl_url();
  char *scheme = nullptr, *host = nullptr, *part = nullptr;
  bool valid = parsed;
  if (valid)
    valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(),
                         CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK;
  if (valid)
    valid = curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
            (std::strcmp(scheme, "ws") == 0 || std::strcmp(scheme, "wss") == 0);
  if (valid)
    valid =
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && *host;
  for (auto field : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT})
    if (parsed && curl_url_get(parsed, field, &part, 0) == CURLUE_OK) {
      valid = false;
      curl_free(part);
      part = nullptr;
    }
  if (parsed)
    curl_url_cleanup(parsed);
  curl_free(scheme);
  curl_free(host);
  valid = valid && url.find('\0') == std::string::npos &&
          protocols.find_first_of("\r\n") == std::string::npos &&
          protocols.find('\0') == std::string::npos;
  std::size_t at = 0;
  while (valid && at < headers.size()) {
    auto end = headers.find("\r\n", at), colon = headers.find(':', at);
    if (end == std::string::npos || colon == std::string::npos ||
        colon >= end || colon == at) {
      valid = false;
      break;
    }
    const auto name = headers.substr(at, colon - at),
               value = headers.substr(colon + 1, end - colon - 1);
    valid =
        name.find_first_not_of(
            "!#$%&'*+-.^_`|~"
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") ==
            std::string::npos &&
        value.find_first_of("\r\n") == std::string::npos &&
        value.find('\0') == std::string::npos;
    at = end + 2;
  }
  if (!valid) {
    transport_error(h, "Invalid WebSocket URL or headers");
    transport_close(h, 1006, "WebSocket connection rejected");
    return;
  }
  auto state = std::make_shared<curl_transport::State>(h);
  state->url = url;
  state->protocols = protocols;
  state->headers = headers;
  auto &r = curl_transport::registry();
  std::lock_guard lock(r.mutex);
  r.live.emplace(h, state);
  try {
    state->worker = std::thread([raw = state.get()] { raw->run(); });
  } catch (...) {
    r.live.erase(h);
    transport_error(h, "WebSocket worker allocation failed");
    transport_close(h, 1006, "WebSocket connection rejected");
  }
}
std::size_t platform_buffered(NativeWebSocketHandle h) {
  auto s = curl_transport::find(h);
  if (!s)
    return 0;
  std::lock_guard lock(s->mutex);
  return s->bytes;
}
void platform_send(NativeWebSocketHandle h, const std::string &text) {
  auto s = curl_transport::find(h);
  if (!s || !s->enqueue(text))
    throw std::runtime_error("WebSocket is not open");
}
void platform_close(NativeWebSocketHandle h) {
  auto s = curl_transport::find(h);
  if (!s)
    return;
  s->closing = true;
  transport_closing(h);
}
void platform_destroy(NativeWebSocketHandle h) {
  auto &r = curl_transport::registry();
  std::lock_guard lock(r.mutex);
  auto i = r.live.find(h);
  if (i == r.live.end())
    return;
  i->second->stop = true;
  r.retired.push_back(std::move(i->second));
  r.live.erase(i);
}
void platform_reap() {
  std::vector<std::shared_ptr<curl_transport::State>> done;
  auto &r = curl_transport::registry();
  {
    std::lock_guard lock(r.mutex);
    for (auto i = r.live.begin(); i != r.live.end();)
      if (i->second->finished.load(std::memory_order_acquire)) {
        done.push_back(std::move(i->second));
        i = r.live.erase(i);
      } else
        ++i;
    for (auto i = r.retired.begin(); i != r.retired.end();)
      if ((*i)->finished.load(std::memory_order_acquire)) {
        done.push_back(std::move(*i));
        i = r.retired.erase(i);
      } else
        ++i;
  }
}
void platform_set_producer(NativeWebSocketHandle h, TextProducer producer) {
  auto s = curl_transport::find(h);
  if (!s)
    throw std::invalid_argument("Closed WebSocket producer");
  TextProducer retired;
  {
    std::lock_guard lock(s->mutex);
    if (s->stop.load() || s->closing.load() || s->finished.load() ||
        WebSocket(h).readyState() >= WebSocket::CLOSING)
      throw std::invalid_argument("Closed WebSocket producer");
    retired = std::move(s->producer);
    s->producer = std::move(producer);
  }
}
namespace curl_transport {
std::size_t transport_count_for_test() {
  auto &r = registry();
  std::lock_guard lock(r.mutex);
  return r.live.size() + r.retired.size();
}
} // namespace curl_transport
} // namespace gea::host::websocket
#endif
