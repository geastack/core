#define GEA_RUNTIME_REALMS 1
#include "gea/worker-runtime.h"
#include <cassert>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>

using Port = gea::host::workers::MessagePort;
using Buffer = gea::Ref<gea::ArrayBuffer>;
using Payload = gea::TaggedUnion<std::string, Buffer>;
struct Event {
  Payload data;
  gea::Ref<gea::ArrayObject<Port>> ports;
};
static int received = 0;
static void receive(void *, Event event) {
  assert(event.data.index() == 1);
  const auto buffer = event.data.template get<1>();
  assert(buffer->size() == 1920 && buffer->data()[10] == 42);
  received++;
}
static std::atomic<int> destroyed{0};
struct RealmOwned {
  Port port;
  std::thread::id owner = std::this_thread::get_id();
  ~RealmOwned() { assert(owner == std::this_thread::get_id()); ++destroyed; }
};
int main() {
  gea::runtime::hostworker::installRealmRuntime();
  auto realm = gea::host::workers::Context::create("typed_cycle");
  std::atomic<bool> ready{false};
  realm->start([&] {
    auto value = gea::makeRef<RealmOwned>();
    gea::host::workers::MessageChannel direct;
    value->port = direct.port1;
    value->port.setOnMessage([value](gea::host::workers::MessageEvent &) {});
    ready = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ready && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(ready);
  realm->stop();
  while (!destroyed && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(destroyed == 1);

  struct Options { std::string type; };
  bool rejected = false;
  try { gea::runtime::hostworker::createWorker("unused", Options{"classic"}); }
  catch (const std::exception &error) { rejected = std::string(error.what()).find("module") != std::string::npos; }
  assert(rejected);
  gea::host::workers::MessageChannel channel;
  gea::runtime::hostworker::setOnMessage(channel.port2, gea::CallableObject<void(Event)>(&receive, nullptr));
  auto buffer = gea::makeRef<gea::ArrayBuffer>(std::size_t(1920), std::uint8_t(42));
  auto alias = buffer;
  auto transfers = gea::makeRef<gea::ArrayObject<Buffer>>();
  transfers->push(buffer);
  gea::runtime::hostworker::postMessage(channel.port1, buffer, transfers);
  assert(alias->detached() && alias->size() == 0);
  gea::host::workers::Context::main()->runPending();
  assert(received == 1);
  auto copy = gea::makeRef<gea::ArrayBuffer>(std::size_t(1920), std::uint8_t(42));
  gea::runtime::hostworker::postMessage(channel.port1, copy);
  assert(!copy->detached());
  copy->data()[10] = 0;
  gea::host::workers::Context::main()->runPending();
  assert(received == 2);
  channel.port1.close(); channel.port2.close();
  std::puts("worker bridge: typed event, detached transfer and structured buffer clone OK");
}
