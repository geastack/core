#include "host/websocket_message.h"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
  using Assembler = gea::host::websocket::TextMessageAssembler;
  using Result = Assembler::Result;
  Assembler text;
  const std::string message = "{\"data\":\"" + std::string(5000, 'x') + "\"}";
  for (std::size_t i = 0; i < message.size(); i += 1024) {
    const auto chunk = std::string_view(message).substr(i, 1024);
    assert(text.append(1, true, i, message.size(), chunk) ==
           (i + chunk.size() == message.size() ? Result::Complete : Result::Pending));
  }
  assert(text.take() == message);
  assert(text.append(1, false, 0, 3, "one") == Result::Pending);
  assert(text.append(9, true, 0, 4, "ping") == Result::Pending);
  assert(text.append(0, true, 0, 3, "t") == Result::Pending);
  assert(text.append(0, true, 1, 3, "wo") == Result::Complete);
  assert(text.take() == "onetwo");
  assert(text.append(1, true, 0, 0, "") == Result::Complete);
  assert(text.take().empty());
  assert(text.append(1, true, 0, 4, "a") == Result::Pending);
  assert(text.append(1, true, 2, 4, "bc") == Result::Invalid);
  assert(text.append(1, true, 0, 2, "ok") == Result::Complete);
  assert(text.take() == "ok");
  assert(text.append(1, true, 0, 4, "a") == Result::Pending);
  assert(text.append(1, true, 0, 2, "ok") == Result::Invalid);
  assert(text.append(1, true, 0, 2, "ok") == Result::Complete);
  assert(text.take() == "ok");
  assert(text.append(1, true, 0, 1024 * 1024 + 1, std::string(1024 * 1024 + 1, 'x')) == Result::Invalid);
  assert(text.append(2, true, 0, 2, "xx") == Result::Pending);
  assert(text.append(1, true, 0, 2, "ok") == Result::Complete);
  assert(text.take() == "ok");
  std::puts("WebSocket frame/chunk assembly passed");
}
