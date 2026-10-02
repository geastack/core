#include "host/websocket.h"

#include <string>
#include <vector>

struct FakeWsRecord {
  std::string url;
  std::vector<std::string> sends;
  bool closed = false;
};

std::vector<FakeWsRecord> fake_ws_records;
std::string fake_ws_protocols;

void fake_ws_reset() {
  fake_ws_records.clear();
}

namespace gea::host::websocket {

void platform_open(NativeWebSocketHandle, const std::string &url, const std::string &protocols) {
  fake_ws_protocols = protocols;
  fake_ws_records.push_back(FakeWsRecord{url, {}, false});
}

void platform_send(NativeWebSocketHandle, const std::string &data) {
  if (!fake_ws_records.empty()) {
    fake_ws_records.back().sends.push_back(data);
  }
}

void platform_close(NativeWebSocketHandle) {
  if (!fake_ws_records.empty()) {
    fake_ws_records.back().closed = true;
  }
}

void platform_destroy(NativeWebSocketHandle) {}

}  // namespace gea::host::websocket
