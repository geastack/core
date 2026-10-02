// Includes the compiler-emitted TypeScript fixture from the existing test output.
#include "test_audio_worklet_compiled.cpp"
#include <cassert>

namespace {
unsigned registrations = 0;
}
namespace gea::host {
AudioWorkletProcessor::AudioWorkletProcessor() = default;
namespace audio_worklet {
workers::MessagePort processorPort() { return {}; }
void registerProcessor(const std::string& name, ProcessorFactory) {
  assert(name == "typed-copy");
  ++registrations;
}
}
namespace websocket {
std::unordered_map<NativeWebSocketHandle, CallbackTable>& callbackTable() {
  static std::unordered_map<NativeWebSocketHandle, CallbackTable> table;
  return table;
}
}
}

int main() {
  // A normal typed processor registers without reading dynamic constructor properties.
  gea_worklet_probe_entry();
  assert(registrations == 1);

  // Standard event handler removal must release captures, and cleanup of an
  // already closed socket must not resurrect a callback-table entry.
  using namespace gea::runtime::hostevent;
  gea::host::WebSocket socket(gea::host::NativeWebSocketHandle{77});
  auto& table = gea::host::websocket::callbackTable();
  setWebSocketOnOpen(socket, nullptr);
  setWebSocketOnMessage(socket, nullptr);
  setWebSocketOnClose(socket, nullptr);
  setWebSocketOnError(socket, nullptr);
  assert(table.empty());
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> released = capture;
  auto& callbacks = table[socket.nativeHandle];
  callbacks.on_open = [capture] {};
  callbacks.on_message = [capture](const std::string&) {};
  callbacks.on_close = [capture](int, const std::string&) {};
  callbacks.on_error = [capture](const std::string&) {};
  capture.reset();
  setWebSocketOnOpen(socket, nullptr);
  setWebSocketOnMessage(socket, nullptr);
  setWebSocketOnClose(socket, nullptr);
  setWebSocketOnError(socket, nullptr);
  assert(released.expired());
}
