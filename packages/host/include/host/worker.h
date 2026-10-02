// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace gea::host::workers {

// Native execution realms. Only native ownership-safe messages may cross realms;
// generated Ref objects and callbacks remain local to their creating realm.
class Context : public std::enable_shared_from_this<Context> {
 public:
  struct State;
  static std::shared_ptr<Context> current();
  static std::shared_ptr<Context> main();
  static void runMainPending();
  static const char *mainPendingStage();
  static bool isWorkerCurrent();
  // Native scheduling only: -1 leaves affinity unset; desktop ignores core.
  static std::shared_ptr<Context> create(const std::string &name, int priority = 8, int core = -1);
  ~Context();
  bool post(std::function<void()> callback, std::size_t bytes = 0);
  void start(std::function<void()> entry);
  void stop();
  // Called on the owner task before destroying its compiler realm.
  void finish();
  bool stopped() const;
  bool isMain() const;
  void runPending(double budgetMs = 4);
  // Audio protocol callbacks must share CPU with UI, below capture/mixer.
  // Owner-task only; no application-level scheduling knob.
  void prioritizeAudio();
  void setPump(std::function<void()> pump);
  void addCleanup(std::function<void()> cleanup);
  void setErrorHandler(std::function<void(const std::string &)> handler);
  double schedule(std::function<void()> callback, double delayMs, bool repeat);
  void cancel(double id);
 private:
  explicit Context(bool main, std::string name, int priority = 8, int core = -1);
  void loop(std::function<void()> entry);
  std::unique_ptr<State> state_;
  friend class ContextScope;
};

class ContextScope {
 public:
  explicit ContextScope(std::shared_ptr<Context> context);
  ~ContextScope();
 private:
  std::shared_ptr<Context> previous_;
};

struct MessageData {
  using Bytes = std::vector<std::uint8_t>;
  std::variant<std::string, Bytes> value;
  MessageData() : value(std::string{}) {}
  MessageData(std::string text) : value(std::move(text)) {}
  MessageData(const char *text) : value(std::string(text)) {}
  MessageData(Bytes bytes) : value(std::move(bytes)) {}
  std::size_t byteSize() const;
};

struct MessageEvent;
class MessagePort {
 public:
  struct Endpoint;
  struct Ownership;
  MessagePort() = default;
  explicit operator bool() const { return bool(ownership_); }
  void postMessage(MessageData data, const std::vector<MessagePort> &transfer = {}) const;
  void validateTransfers(const std::vector<MessagePort> &transfer) const;
  void setOnMessage(std::function<void(MessageEvent &)> callback) const;
  void start() const;
  void close() const;
  bool detached() const;
  const void *identity() const { return ownership_.get(); }
  // Host-only transfer primitives. Validate the whole list before detaching any
  // port. Adoption takes place on the receiving task, never the sending task.
  MessagePort transfer() const;
  void adopt(const std::shared_ptr<Context> &owner) const;
 private:
  explicit MessagePort(std::shared_ptr<Ownership> ownership);
  MessagePort transfer(std::function<void(MessageEvent &)> &retiredHandler) const;
  std::shared_ptr<Ownership> ownership_;
  friend class MessageChannel;
};

struct MessageEvent {
  MessageData data;
  std::vector<MessagePort> ports;
};

class MessageChannel {
 public:
  MessageChannel();
  MessagePort port1;
  MessagePort port2;
};

void setRealmRunner(std::function<void(std::function<void()>)> runner);
void registerModule(const std::string &url, std::function<void()> entry);
class DedicatedWorkerGlobalScope {
 public:
  void postMessage(MessageData data, const std::vector<MessagePort> &transfer = {}) const;
  void validateTransfers(const std::vector<MessagePort> &transfer) const;
  void setOnMessage(std::function<void(MessageEvent &)> callback) const;
  void close() const;
};
DedicatedWorkerGlobalScope self();

class Worker {
 public:
  Worker() = default;
  explicit operator bool() const { return bool(state_); }
  static Worker create(const std::string &url);
  void postMessage(MessageData data, const std::vector<MessagePort> &transfer = {}) const;
  void validateTransfers(const std::vector<MessagePort> &transfer) const;
  void setOnMessage(std::function<void(MessageEvent &)> callback) const;
  void setOnError(std::function<void(const std::string &)> callback) const;
  void terminate() const;
 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace gea::host::workers
