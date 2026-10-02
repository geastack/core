// SPDX-License-Identifier: Apache-2.0
#include "host/worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#else
#include <thread>
#endif

namespace gea::host::workers {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t maxQueueBytes = 2 * 1024 * 1024;
constexpr std::size_t maxQueueEntries = 1024;
thread_local std::shared_ptr<Context> activeContext;
thread_local MessagePort globalPort;
std::atomic<std::uint64_t> timerIds{1};
std::atomic<bool> mainCreated{false};
std::atomic<const char *> mainStage{"idle"};
struct Registry {
  std::mutex mutex;
  std::unordered_map<std::string, std::function<void()>> modules;
  std::function<void(std::function<void()>)> realmRunner;
};
Registry &registry() {
  // Generated module registration and adapter installation run from static
  // initializers in other translation units, before their ordering is known.
  static Registry value;
  return value;
}
}

struct Context::State {
  struct Task { std::function<void()> callback; std::size_t bytes; };
  struct Timer {
    double id;
    std::function<void()> callback;
    Clock::time_point due;
    std::chrono::milliseconds delay;
    bool repeat;
  };
  bool main;
  int priority;
  int core;
  std::string name;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Task> tasks;
  std::vector<Timer> timers;
  std::vector<std::function<void()>> cleanups;
  std::function<void()> pump;
  std::function<void(const std::string &)> error;
  std::size_t bytes = 0;
  bool lastWasTimer = false;
  std::atomic<bool> stopping{false};
  std::atomic<bool> started{false};
  State(bool isMain, std::string label, int priority, int core)
      : main(isMain), priority(priority), core(core), name(std::move(label)) {}
};

Context::Context(bool main, std::string name, int priority, int core)
    : state_(new State(main, std::move(name), priority, core)) {}
Context::~Context() = default;
std::shared_ptr<Context> Context::main() {
  static auto context = std::shared_ptr<Context>(new Context(true, "main"));
  mainCreated.store(true, std::memory_order_release);
  return context;
}
void Context::runMainPending() {
  if (mainCreated.load(std::memory_order_acquire)) main()->runPending();
}
const char *Context::mainPendingStage() { return mainStage.load(std::memory_order_relaxed); }
bool Context::isWorkerCurrent() { return activeContext && !activeContext->isMain(); }
std::shared_ptr<Context> Context::current() { return activeContext ? activeContext : main(); }
std::shared_ptr<Context> Context::create(const std::string &name, int priority, int core) {
  if (core < -1) throw std::invalid_argument("Invalid worker CPU affinity");
#ifdef ESP_PLATFORM
  if (core >= portNUM_PROCESSORS) throw std::invalid_argument("Worker CPU affinity is unavailable");
#endif
  return std::shared_ptr<Context>(new Context(false, name, priority, core));
}
ContextScope::ContextScope(std::shared_ptr<Context> context) : previous_(std::move(activeContext)) {
  activeContext = std::move(context);
}
ContextScope::~ContextScope() { activeContext = std::move(previous_); }
bool Context::isMain() const { return state_->main; }
bool Context::stopped() const { return state_->stopping.load(); }
void Context::prioritizeAudio() {
  if (state_->main || Context::current().get() != this) return;
#ifdef ESP_PLATFORM
  const auto priority = configMAX_PRIORITIES - 2;
  if (uxTaskPriorityGet(nullptr) < priority) {
    vTaskPrioritySet(nullptr, priority);
    ESP_LOGI("gea_worker", "audio protocol priority=%d", priority);
  }
#endif
}
bool Context::post(std::function<void()> callback, std::size_t bytes) {
  if (!callback) return false;
  std::lock_guard lock(state_->mutex);
  if (state_->stopping || state_->tasks.size() >= maxQueueEntries || bytes > maxQueueBytes - state_->bytes)
    return false;
  state_->tasks.push_back({std::move(callback), bytes});
  state_->bytes += bytes;
  state_->wake.notify_one();
  return true;
}
void Context::setPump(std::function<void()> pump) { state_->pump = std::move(pump); }
void Context::setErrorHandler(std::function<void(const std::string &)> handler) {
  std::lock_guard lock(state_->mutex);
  state_->error = std::move(handler);
}
void Context::addCleanup(std::function<void()> cleanup) {
  std::lock_guard lock(state_->mutex);
  if (state_->stopping) throw std::runtime_error("Worker context is closed");
  state_->cleanups.push_back(std::move(cleanup));
}
void Context::stop() {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping.store(true);
  }
  state_->wake.notify_all();
}
double Context::schedule(std::function<void()> callback, double delayMs, bool repeat) {
  const double normalized = std::isfinite(delayMs) ? std::min(delayMs, 2147483647.0) : 0;
  const auto delay = std::chrono::milliseconds(static_cast<long long>(
      std::max(repeat ? 1.0 : 0.0, normalized)));
  const double id = static_cast<double>(timerIds.fetch_add(1));
  std::lock_guard lock(state_->mutex);
  if (state_->stopping) return 0;
  state_->timers.push_back({id, std::move(callback), Clock::now() + delay, delay, repeat});
  state_->wake.notify_one();
  return id;
}
void Context::cancel(double id) {
  std::lock_guard lock(state_->mutex);
  std::erase_if(state_->timers, [id](const auto &timer) { return timer.id == id; });
}
void Context::runPending(double budgetMs) {
  struct StageScope {
    bool enabled;
    const char *previous;
    ~StageScope() { if (enabled) mainStage.store(previous, std::memory_order_relaxed); }
  } stageScope{state_->main, mainStage.load(std::memory_order_relaxed)};
  const auto stage = [this](const char *name) {
    if (state_->main) mainStage.store(name, std::memory_order_relaxed);
  };
  stage("context_scope");
  ContextScope scope(shared_from_this());
  const auto deadline = Clock::now() + std::chrono::microseconds(static_cast<long long>(budgetMs * 1000));
  do {
    std::function<void()> callback;
    {
      stage("queue_lock");
      std::lock_guard lock(state_->mutex);
      stage("queue_select");
      if (state_->stopping) return;
      // Timers must still advance during a continuous incoming packet stream.
      const auto now = Clock::now();
      auto timer = std::find_if(state_->timers.begin(), state_->timers.end(),
          [now](const auto &candidate) { return candidate.due <= now; });
      if (timer != state_->timers.end() && (state_->tasks.empty() || !state_->lastWasTimer)) {
        state_->lastWasTimer = true;
        callback = timer->callback;
        if (timer->repeat) timer->due = now + timer->delay;
        else state_->timers.erase(timer);
      } else if (!state_->tasks.empty()) {
        state_->lastWasTimer = false;
        auto task = std::move(state_->tasks.front());
        state_->tasks.pop_front();
        state_->bytes -= task.bytes;
        callback = std::move(task.callback);
      } else break;
    }
    try {
      stage("invoke_callback");
      if (callback) callback();
      stage("pump_microtasks");
      if (state_->pump) state_->pump();
    } catch (const std::exception &error) {
      std::function<void(const std::string &)> report;
      { std::lock_guard lock(state_->mutex); report = state_->error; }
      if (report) report(error.what());
      else std::fprintf(stderr, "[Worker:%s] uncaught callback exception: %s\n", state_->name.c_str(), error.what());
    } catch (...) {
      std::function<void(const std::string &)> report;
      { std::lock_guard lock(state_->mutex); report = state_->error; }
      if (report) report("Uncaught worker exception");
      else std::fprintf(stderr, "[Worker:%s] uncaught callback exception\n", state_->name.c_str());
    }
    stage("release_callback");
    callback = nullptr;
    stage("check_budget");
  } while (Clock::now() < deadline);
}
void Context::loop(std::function<void()> entry) {
  ContextScope scope(shared_from_this());
  std::function<void(std::function<void()>)> runner;
  { auto &state = registry(); std::lock_guard lock(state.mutex); runner = state.realmRunner; }
  auto body = [this, entry = std::move(entry)]() mutable {
  post(std::move(entry));
  while (!state_->stopping) {
    const auto turnStart = Clock::now();
    constexpr double turnBudgetMs = 4;
    runPending(turnBudgetMs);
    std::unique_lock lock(state_->mutex);
    if (state_->stopping) break;
    const auto now = Clock::now();
    const bool ready = !state_->tasks.empty() ||
        std::any_of(state_->timers.begin(), state_->timers.end(),
            [now](const auto &timer) { return timer.due <= now; });
    if (ready) {
      lock.unlock();
      // A bounded dispatch turn is not a scheduling boundary by itself. A
      // continuously ready worker otherwise immediately starts another turn
      // and excludes lower-priority transport/UI tasks on FreeRTOS.
      if (now - turnStart >= std::chrono::milliseconds(4)) {
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#else
        std::this_thread::yield();
#endif
      }
      continue;
    }
    if (state_->timers.empty()) {
      state_->wake.wait(lock, [this] {
        return state_->stopping || !state_->tasks.empty() || !state_->timers.empty();
      });
    } else {
      auto deadline = state_->timers.front().due;
      for (const auto &timer : state_->timers) deadline = std::min(deadline, timer.due);
      state_->wake.wait_until(lock, deadline);
    }
  }
  finish();
  };
  if (runner) runner(std::move(body)); else body();
}
void Context::finish() {
  ContextScope scope(shared_from_this());
  stop();
  // Destroy callbacks, native sockets and compiler realm state on their owner.
  std::vector<std::function<void()>> cleanups;
  std::deque<State::Task> abandonedTasks;
  std::vector<State::Timer> abandonedTimers;
  {
    std::lock_guard lock(state_->mutex);
    cleanups.swap(state_->cleanups);
    abandonedTasks.swap(state_->tasks);
    abandonedTimers.swap(state_->timers);
    state_->bytes = 0;
  }
  abandonedTasks.clear();
  abandonedTimers.clear();
  for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) (*it)();
  state_->pump = {};
  globalPort.close();
  globalPort = {};
}
void Context::start(std::function<void()> entry) {
  if (state_->main || state_->started.exchange(true)) throw std::runtime_error("Worker already started");
  auto context = shared_from_this();
#ifdef ESP_PLATFORM
  struct Start {
    std::shared_ptr<Context> context;
    std::function<void()> entry;
    bool capsStack = false;
  };
  auto *start = new Start{context, std::move(entry)};
  const auto taskEntry = [](void *argument) {
    bool capsStack;
    {
      std::unique_ptr<Start> start(static_cast<Start *>(argument));
      capsStack = start->capsStack;
      start->context->loop(std::move(start->entry));
    }
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    if (capsStack) {
      vTaskDeleteWithCaps(nullptr);
      return;
    }
#else
    (void)capsStack;
#endif
    vTaskDelete(nullptr);
  };
  const auto core = state_->core < 0 ? tskNO_AFFINITY : state_->core;
  BaseType_t result = pdFAIL;
  bool externalStack = false;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
  // Worker code executes with the flash cache enabled. Keep its sizeable
  // stack out of scarce DMA-capable RAM, including on repeated connections.
  // Publish allocation ownership before creation: the task can run immediately.
  start->capsStack = true;
  result = xTaskCreatePinnedToCoreWithCaps(taskEntry, state_->name.c_str(),
      16384, start, state_->priority, nullptr, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  externalStack = result == pdPASS;
#endif
  if (result != pdPASS) {
    start->capsStack = false;
    result = xTaskCreatePinnedToCore(taskEntry, state_->name.c_str(),
        16384, start, state_->priority, nullptr, core);
  }
  if (result != pdPASS) {
    ESP_LOGE("gea_worker", "task allocation failed name=%s stack=16384 internal_free=%u internal_largest=%u",
        state_->name.c_str(),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    delete start;
    state_->stopping = true;
    throw std::runtime_error("Cannot create worker task");
  }
  ESP_LOGI("gea_worker", "started name=%s core=%d priority=%d stack=%s", state_->name.c_str(), state_->core, state_->priority,
      externalStack ? "psram" : "internal");
#else
  std::thread([context, entry = std::move(entry)]() mutable { context->loop(std::move(entry)); }).detach();
#endif
}

std::size_t MessageData::byteSize() const {
  return std::visit([](const auto &value) { return value.size(); }, value);
}
struct MessagePort::Endpoint {
  std::mutex mutex;
  std::weak_ptr<Endpoint> peer;
  std::weak_ptr<Context> owner;
  std::function<void(MessageEvent &)> callback;
  std::deque<MessageEvent> pending;
  std::size_t bytes = 0;
  bool started = false;
  bool closed = false;
  bool scheduled = false;
  std::uint64_t generation = 0;
};
struct MessagePort::Ownership {
  std::mutex mutex;
  std::shared_ptr<Endpoint> endpoint;
};
namespace {
void dispatchPort(const std::shared_ptr<MessagePort::Endpoint> &endpoint, std::uint64_t generation);
void schedulePort(const std::shared_ptr<MessagePort::Endpoint> &endpoint) {
  // Called with endpoint mutex held.
  if (endpoint->closed || endpoint->scheduled || !endpoint->started || !endpoint->callback || endpoint->pending.empty()) return;
  const auto owner = endpoint->owner.lock();
  if (!owner || owner->stopped()) return;
  endpoint->scheduled = true;
  const auto generation = endpoint->generation;
  if (!owner->post([endpoint, generation] { dispatchPort(endpoint, generation); })) {
    endpoint->scheduled = false;
    throw std::runtime_error("MessagePort owner queue is full");
  }
}
void dispatchPort(const std::shared_ptr<MessagePort::Endpoint> &endpoint, std::uint64_t generation) {
  MessageEvent event;
  std::function<void(MessageEvent &)> callback;
  {
    std::lock_guard lock(endpoint->mutex);
    if (generation != endpoint->generation) return;
    endpoint->scheduled = false;
    if (endpoint->closed || !endpoint->started || !endpoint->callback || endpoint->pending.empty()) return;
    // A scheduled task from before a transfer must never deliver in the old realm.
    if (endpoint->owner.lock() != Context::current()) { schedulePort(endpoint); return; }
    event = std::move(endpoint->pending.front());
    endpoint->pending.pop_front();
    endpoint->bytes -= event.data.byteSize();
    callback = endpoint->callback;
  }
  for (auto &port : event.ports) port.adopt(Context::current());
  try { callback(event); } catch (...) {
    std::lock_guard lock(endpoint->mutex);
    schedulePort(endpoint);
    throw;
  }
  std::lock_guard lock(endpoint->mutex);
  schedulePort(endpoint);
}
}
MessagePort::MessagePort(std::shared_ptr<Ownership> ownership) : ownership_(std::move(ownership)) {}
bool MessagePort::detached() const {
  if (!ownership_) return true;
  std::lock_guard lock(ownership_->mutex);
  return !ownership_->endpoint;
}
MessagePort MessagePort::transfer() const {
  std::function<void(MessageEvent &)> retiredHandler;
  return transfer(retiredHandler);
}
MessagePort MessagePort::transfer(std::function<void(MessageEvent &)> &retiredHandler) const {
  if (!ownership_) throw std::runtime_error("DataCloneError: detached MessagePort");
  std::lock_guard lock(ownership_->mutex);
  if (!ownership_->endpoint) throw std::runtime_error("DataCloneError: detached MessagePort");
  auto target = std::make_shared<Ownership>();
  target->endpoint = std::move(ownership_->endpoint);
  {
    std::lock_guard endpointLock(target->endpoint->mutex);
    ++target->endpoint->generation;
    target->endpoint->scheduled = false;
    retiredHandler.swap(target->endpoint->callback);
    target->endpoint->started = false;
    target->endpoint->owner.reset();
  }
  return MessagePort(std::move(target));
}
void MessagePort::adopt(const std::shared_ptr<Context> &owner) const {
  if (!ownership_) return;
  std::shared_ptr<Endpoint> endpoint;
  { std::lock_guard lock(ownership_->mutex); endpoint = ownership_->endpoint; }
  if (!endpoint) return;
  std::uint64_t generation;
  {
    std::lock_guard lock(endpoint->mutex);
    endpoint->owner = owner;
    generation = endpoint->generation;
  }
  if (!owner->isMain()) {
    std::weak_ptr<Endpoint> weak = endpoint;
    const auto identity = owner.get();
    owner->addCleanup([weak, identity, generation] {
      if (auto endpoint = weak.lock()) {
        std::function<void(MessageEvent &)> callback;
        std::deque<MessageEvent> pending;
        {
          std::lock_guard lock(endpoint->mutex);
          // A port transferred onward belongs to its new realm's teardown.
          if (endpoint->owner.lock().get() != identity || endpoint->generation != generation) return;
          endpoint->closed = true;
          // A moved small std::function may leave its source callable intact.
          // Swap with empty to break native-port/realm-object ownership cycles.
          callback.swap(endpoint->callback);
          pending.swap(endpoint->pending);
          endpoint->bytes = 0;
        }
      }
    });
  }
}

void MessagePort::validateTransfers(const std::vector<MessagePort> &ports) const {
  std::shared_ptr<Endpoint> endpoint;
  if (ownership_) { std::lock_guard lock(ownership_->mutex); endpoint = ownership_->endpoint; }
  std::shared_ptr<Endpoint> peer;
  if (endpoint) { std::lock_guard lock(endpoint->mutex); peer = endpoint->peer.lock(); }
  std::unordered_set<const Ownership *> seen;
  for (const auto &port : ports) {
    if (!port.ownership_ || port.ownership_ == ownership_ || !seen.insert(port.ownership_.get()).second)
      throw std::runtime_error("DataCloneError: invalid MessagePort transfer list");
    std::lock_guard lock(port.ownership_->mutex);
    if (!port.ownership_->endpoint || port.ownership_->endpoint == peer)
      throw std::runtime_error("DataCloneError: invalid MessagePort transfer list");
  }
}
void MessagePort::postMessage(MessageData data, const std::vector<MessagePort> &transferPorts) const {
  if (!ownership_) return;
  std::shared_ptr<Endpoint> endpoint;
  { std::lock_guard lock(ownership_->mutex); endpoint = ownership_->endpoint; }
  if (!endpoint) return;
  std::shared_ptr<Endpoint> peer;
  { std::lock_guard lock(endpoint->mutex); if (endpoint->closed) return; peer = endpoint->peer.lock(); }
  if (!peer) return;
  std::unordered_set<const Ownership *> seen;
  for (const auto &port : transferPorts) {
    if (port.detached() || port.ownership_ == ownership_ || !seen.insert(port.ownership_.get()).second)
      throw std::runtime_error("DataCloneError: invalid MessagePort transfer list");
  }
  for (const auto &port : transferPorts) {
    std::lock_guard lock(port.ownership_->mutex);
    if (port.ownership_->endpoint == peer)
      throw std::runtime_error("DataCloneError: destination MessagePort cannot transfer itself");
  }
  // Transferring a listening port discards its old handler. Keep those handlers
  // alive until the destination lock is released: destructors may close it.
  std::vector<std::function<void(MessageEvent &)>> retiredHandlers(transferPorts.size());
  {
    std::lock_guard lock(peer->mutex);
    if (peer->closed) return;
    if (peer->pending.size() >= maxQueueEntries || data.byteSize() > maxQueueBytes - peer->bytes)
      throw std::runtime_error("MessagePort queue exceeded its bounded capacity");
    MessageEvent event{std::move(data), {}};
    event.ports.reserve(transferPorts.size());
    for (std::size_t i = 0; i < transferPorts.size(); ++i)
      event.ports.push_back(transferPorts[i].transfer(retiredHandlers[i]));
    peer->bytes += event.data.byteSize();
    peer->pending.push_back(std::move(event));
    schedulePort(peer);
  }
}
void MessagePort::setOnMessage(std::function<void(MessageEvent &)> callback) const {
  if (!ownership_) return;
  {
    std::lock_guard lock(ownership_->mutex);
    if (!ownership_->endpoint) return;
    std::lock_guard endpointLock(ownership_->endpoint->mutex);
    callback.swap(ownership_->endpoint->callback);
    ownership_->endpoint->started = true;
    schedulePort(ownership_->endpoint);
  }
  // Callback captures may close this same port when their last reference dies.
  // Release the old handler only after both native locks have been dropped.
}
void MessagePort::start() const {
  if (!ownership_) return;
  std::lock_guard lock(ownership_->mutex);
  if (!ownership_->endpoint) return;
  std::lock_guard endpointLock(ownership_->endpoint->mutex);
  ownership_->endpoint->started = true;
  schedulePort(ownership_->endpoint);
}
void MessagePort::close() const {
  if (!ownership_) return;
  std::function<void(MessageEvent &)> callback;
  std::deque<MessageEvent> pending;
  {
    std::lock_guard lock(ownership_->mutex);
    if (!ownership_->endpoint) return;
    std::lock_guard endpointLock(ownership_->endpoint->mutex);
    ownership_->endpoint->closed = true;
    callback.swap(ownership_->endpoint->callback);
    pending.swap(ownership_->endpoint->pending);
    ownership_->endpoint->bytes = 0;
  }
}
MessageChannel::MessageChannel() {
  auto a = std::make_shared<MessagePort::Endpoint>();
  auto b = std::make_shared<MessagePort::Endpoint>();
  a->peer = b; b->peer = a;
  a->owner = Context::current(); b->owner = Context::current();
  auto first = std::make_shared<MessagePort::Ownership>(); first->endpoint = a;
  auto second = std::make_shared<MessagePort::Ownership>(); second->endpoint = b;
  port1 = MessagePort(first); port2 = MessagePort(second);
  port1.adopt(Context::current());
  port2.adopt(Context::current());
}
void setRealmRunner(std::function<void(std::function<void()>)> runner) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  state.realmRunner = std::move(runner);
}
void registerModule(const std::string &url, std::function<void()> entry) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  state.modules[url] = std::move(entry);
}
DedicatedWorkerGlobalScope self() { return {}; }
void DedicatedWorkerGlobalScope::postMessage(MessageData data, const std::vector<MessagePort> &transfer) const {
  globalPort.postMessage(std::move(data), transfer);
}
void DedicatedWorkerGlobalScope::validateTransfers(const std::vector<MessagePort> &ports) const { globalPort.validateTransfers(ports); }
void DedicatedWorkerGlobalScope::setOnMessage(std::function<void(MessageEvent &)> callback) const {
  globalPort.setOnMessage(std::move(callback));
}
void DedicatedWorkerGlobalScope::close() const { Context::current()->stop(); }
struct Worker::State {
  std::shared_ptr<Context> context;
  std::shared_ptr<Context> creator;
  MessagePort port;
  std::function<void(const std::string &)> onError;
};
Worker Worker::create(const std::string &url) {
  std::function<void()> entry;
  { auto &state = registry(); std::lock_guard lock(state.mutex); auto it = state.modules.find(url);
    if (it == state.modules.end()) throw std::runtime_error("Worker module not registered: " + url);
    entry = it->second;
  }
  Worker worker;
  worker.state_ = std::make_shared<State>();
  auto state = worker.state_;
  state->context = Context::create("gea_worker");
  state->creator = Context::current();
  MessageChannel channel;
  state->port = channel.port1;
  auto target = channel.port2.transfer();
  std::weak_ptr<State> weak = state;
  state->context->setErrorHandler([weak, creator = state->creator](const std::string &error) {
    // Only the creator task may acquire a strong State reference: the last
    // reference owns callbacks containing non-atomic, realm-local compiler Refs.
    creator->post([weak, error] {
      if (auto state = weak.lock(); state && state->onError) state->onError(error);
    }, error.size());
  });
  state->context->start([target, entry = std::move(entry)] {
    target.adopt(Context::current());
    globalPort = target;
    entry();
  });
  return worker;
}
void Worker::postMessage(MessageData data, const std::vector<MessagePort> &transfer) const {
  if (state_ && !state_->context->stopped()) state_->port.postMessage(std::move(data), transfer);
}
void Worker::validateTransfers(const std::vector<MessagePort> &ports) const {
  if (state_) state_->port.validateTransfers(ports);
}
void Worker::setOnMessage(std::function<void(MessageEvent &)> callback) const {
  if (state_) state_->port.setOnMessage(std::move(callback));
}
void Worker::setOnError(std::function<void(const std::string &)> callback) const {
  if (state_) state_->onError = std::move(callback);
}
void Worker::terminate() const {
  if (!state_) return;
  state_->port.close();
  state_->context->stop();
}
}  // namespace gea::host::workers
