#include "host/websocket.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

struct Observed {
  int opens = 0;
  std::vector<std::string> messages;
  int closes = 0;
};

static Observed observed;

int main() {
  auto handle = gea::host::websocket::create_handle(std::string("ws://test/"));
  gea::host::WebSocket ws(handle);
  (void)ws;

  gea::host::websocket::test_set_open_callback(handle, [] { observed.opens++; });
  gea::host::websocket::test_set_message_callback(handle, [](const std::string &data) {
    observed.messages.push_back(data);
  });
  gea::host::websocket::test_set_close_callback(handle, [](int, const std::string &) {
    observed.closes++;
  });

  gea::host::websocket::test_inject_open(handle);
  gea::host::websocket::test_inject_message(handle, std::string("hello"));
  gea::host::websocket::test_inject_message(handle, std::string("world"));
  gea::host::websocket::test_inject_close(handle, 1000, std::string("normal"));

  assert(observed.opens == 0);
  assert(observed.messages.size() == 0);
  assert(observed.closes == 0);

  gea::host::websocket::runCallbacks();

  assert(observed.opens == 1);
  assert(observed.messages.size() == 2);
  assert(observed.messages[0] == "hello");
  assert(observed.messages[1] == "world");
  assert(observed.closes == 1);

  // A slow consumer must yield without losing or reordering the rest of a burst.
  auto burst = gea::host::websocket::create_handle("ws://burst/");
  std::vector<std::string> received;
  gea::host::websocket::test_set_message_callback(burst, [&](const std::string &data) {
    received.push_back(data);
    std::this_thread::sleep_for(std::chrono::milliseconds(6));
  });
  for (const char *message : {"one", "two", "three"})
    gea::host::websocket::test_inject_message(burst, message);
  gea::host::websocket::runCallbacks();
  assert(received == std::vector<std::string>{"one"});
  gea::host::websocket::runCallbacks();
  assert((received == std::vector<std::string>{"one", "two"}));
  gea::host::websocket::runCallbacks();
  assert((received == std::vector<std::string>{"one", "two", "three"}));
  gea::host::websocket::destroy_handle(burst);

  std::puts("websocket callbacks OK");
  return 0;
}
