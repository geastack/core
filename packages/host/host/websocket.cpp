// SPDX-License-Identifier: Apache-2.0
#include "host/websocket.h"
#include "host/worker.h"
#include "host/websocket_message.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <functional>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gea::host {

namespace {

struct WebSocketInstance {
  std::string url;
  int readyState = WebSocket::CONNECTING;
  websocket::TextMessageAssembler messages;
  websocket::TextConsumer consumer;
};

std::unordered_map<NativeWebSocketHandle, WebSocketInstance> &instances() {
  static std::unordered_map<NativeWebSocketHandle, WebSocketInstance> map;
  return map;
}

std::mutex &instancesMutex() {
  static std::mutex m;
  return m;
}

std::atomic<NativeWebSocketHandle> &nextHandle() {
  static std::atomic<NativeWebSocketHandle> id{1};
  return id;
}

enum class EventKind { Open, Message, Close, Error };

struct PendingEvent {
  NativeWebSocketHandle handle = 0;
  EventKind kind = EventKind::Open;
  std::string text;
  int code = 0;
};

std::mutex ownerMutex;
std::unordered_map<NativeWebSocketHandle, std::weak_ptr<workers::Context>> owners;
void dispatch(PendingEvent ev);

void enqueue(PendingEvent ev) {
  std::shared_ptr<workers::Context> owner;
  {
    std::lock_guard lock(ownerMutex);
    auto it = owners.find(ev.handle);
    if (it == owners.end()) return;
    owner = it->second.lock();
  }
  if (!owner || owner->stopped()) return;
  const auto bytes = ev.text.size();
  const auto handle = ev.handle;
  if (!owner->post([ev = std::move(ev)]() mutable { dispatch(std::move(ev)); }, bytes)) {
    owner->post([handle] {
      dispatch(PendingEvent{handle, EventKind::Error, "WebSocket receive queue exceeded bounded capacity", 0});
    });
  }
}

}  // namespace

namespace websocket {

std::unordered_map<NativeWebSocketHandle, CallbackTable> &callbackTable() {
  using Table = std::unordered_map<NativeWebSocketHandle, CallbackTable>;
  static std::mutex mutex;
  static Table mainTable;
  static std::unordered_map<workers::Context *, std::weak_ptr<Table>> tables;
  const auto context = workers::Context::current();
  if (context->isMain()) return mainTable;
  std::lock_guard lock(mutex);
  auto table = tables[context.get()].lock();
  if (!table) {
    table = std::make_shared<Table>();
    tables[context.get()] = table;
    context->addCleanup([table, key = context.get()] {
      table->clear();
      std::lock_guard lock(mutex);
      tables.erase(key);
    });
  }
  return *table;
}

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)
void platform_open(NativeWebSocketHandle, const std::string &, const std::string &);
std::size_t platform_buffered(NativeWebSocketHandle);
void platform_send(NativeWebSocketHandle, const std::string &);
void platform_close(NativeWebSocketHandle);
void platform_destroy(NativeWebSocketHandle);
void platform_reap();
void platform_set_producer(NativeWebSocketHandle, TextProducer);
#else
__attribute__((weak)) void platform_open(NativeWebSocketHandle, const std::string &, const std::string &) {}
__attribute__((weak)) std::size_t platform_buffered(NativeWebSocketHandle) { return 0; }
__attribute__((weak)) void platform_send(NativeWebSocketHandle, const std::string &) {}
__attribute__((weak)) void platform_close(NativeWebSocketHandle) {}
__attribute__((weak)) void platform_destroy(NativeWebSocketHandle) {}
__attribute__((weak)) void platform_reap() {}
__attribute__((weak)) void platform_set_producer(NativeWebSocketHandle, TextProducer) {}
#endif

void setTextProducer(NativeWebSocketHandle handle, TextProducer producer) {
  // The producer runs exclusively on the native socket sender, whose priority
  // platform_set_producer raises for audio. Promoting the caller's realm here
  // also promotes the UI that attached it and starves background video decode.
  platform_set_producer(handle, std::move(producer));
}

void setTextConsumer(NativeWebSocketHandle handle, TextConsumer consumer) {
  std::lock_guard lock(instancesMutex());
  auto it = instances().find(handle);
  if (it == instances().end()) throw std::invalid_argument("Closed WebSocket native sink");
  it->second.consumer = std::move(consumer);
}

NativeWebSocketHandle create_handle(const std::string &url, const std::string &protocols) {
  auto handle = nextHandle()++;
  {
    std::lock_guard<std::mutex> lock(instancesMutex());
    instances()[handle] = WebSocketInstance{url, WebSocket::CONNECTING, {}};
  }
  auto owner = workers::Context::current();
  (void)callbackTable();
  { std::lock_guard lock(ownerMutex); owners[handle] = owner; }
  if (!owner->isMain()) owner->addCleanup([handle] { destroy_handle(handle); });
  platform_open(handle, url, protocols);
  return handle;
}

void destroy_handle(NativeWebSocketHandle handle) {
  platform_destroy(handle);
  { std::lock_guard lock(ownerMutex); owners.erase(handle); }
  std::lock_guard<std::mutex> lock(instancesMutex());
  instances().erase(handle);
  callbackTable().erase(handle);
}

void runCallbacks() {
  platform_reap();
  workers::Context::current()->runPending();
}

void test_inject_open(NativeWebSocketHandle handle) {
  enqueue(PendingEvent{handle, EventKind::Open, {}, 0});
}
void test_inject_message(NativeWebSocketHandle handle, const std::string &data) {
  enqueue(PendingEvent{handle, EventKind::Message, data, 0});
}
void test_inject_close(NativeWebSocketHandle handle, int code, const std::string &reason) {
  enqueue(PendingEvent{handle, EventKind::Close, reason, code});
}

void test_set_open_callback(NativeWebSocketHandle handle, std::function<void()> cb) {
  callbackTable()[handle].on_open = std::move(cb);
}
void test_set_message_callback(NativeWebSocketHandle handle, std::function<void(const std::string &)> cb) {
  callbackTable()[handle].on_message = std::move(cb);
}
void test_set_close_callback(NativeWebSocketHandle handle, std::function<void(int, const std::string &)> cb) {
  callbackTable()[handle].on_close = std::move(cb);
}
void test_set_error_callback(NativeWebSocketHandle handle, std::function<void(const std::string &)> cb) {
  callbackTable()[handle].on_error = std::move(cb);
}

}  // namespace websocket

namespace {
void dispatch(PendingEvent ev) {
  auto &table = websocket::callbackTable();
  const auto it = table.find(ev.handle);
  if (it == table.end()) return;
  switch (ev.kind) {
    case EventKind::Open: {
      const auto callback = it->second.on_open;
      if (callback) callback();
      break;
    }
    case EventKind::Message: {
      const auto callback = it->second.on_message;
      if (callback) callback(ev.text);
      break;
    }
    case EventKind::Close: {
      websocket::platform_close(ev.handle);
      const auto callback = std::move(it->second.on_close);
      table.erase(it);
      if (callback) callback(ev.code, ev.text);
      break;
    }
    case EventKind::Error: {
      const auto callback = it->second.on_error;
      if (callback) callback(ev.text);
      break;
    }
  }
}
}  // namespace

std::string WebSocket::url() const {
  std::lock_guard<std::mutex> lock(instancesMutex());
  auto it = instances().find(nativeHandle);
  return it == instances().end() ? std::string{} : it->second.url;
}

double WebSocket::readyState() const {
  std::lock_guard<std::mutex> lock(instancesMutex());
  auto it = instances().find(nativeHandle);
  return it == instances().end() ? CLOSED : static_cast<double>(it->second.readyState);
}

double WebSocket::bufferedAmount() const { return websocket::platform_buffered(nativeHandle); }

void WebSocket::send(const std::string &data) const {
  websocket::platform_send(nativeHandle, data);
}

void WebSocket::close() const {
  websocket::platform_close(nativeHandle);
}

void WebSocket::close(double) const {
  websocket::platform_close(nativeHandle);
}

void WebSocket::close(double, const std::string &) const {
  websocket::platform_close(nativeHandle);
}

}  // namespace gea::host

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED)

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "http_parser.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/api.h"
#include "freertos/queue.h"
#include "lwip/priv/tcp_priv.h"
#include <new>
#include <cerrno>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace gea::host::websocket {

namespace {

std::recursive_mutex platformMutex;

struct TaskCpuTelemetry {
  std::int64_t wallAt = 0;
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
  configRUN_TIME_COUNTER_TYPE cpuAt = 0;

  static configRUN_TIME_COUNTER_TYPE currentCpu() {
    TaskStatus_t status{};
    vTaskGetInfo(nullptr, &status, pdFALSE, eRunning);
    return status.ulRunTimeCounter;
  }
#endif

  void sample(const char *direction) {
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    const auto now = esp_timer_get_time();
    if (wallAt && now - wallAt < 1000000) return;
    const auto cpu = currentCpu();
    if (wallAt) {
      const auto elapsed = now - wallAt;
      const auto used = static_cast<configRUN_TIME_COUNTER_TYPE>(cpu - cpuAt);
      ESP_LOGI("gea::host::ws", "%s cpu_us=%llu wall_us=%llu core=%d",
               direction, static_cast<unsigned long long>(used),
               static_cast<unsigned long long>(elapsed), int(xPortGetCoreID()));
    }
    wallAt = now;
    cpuAt = cpu;
#endif
  }
};

struct EspWsState {
  NativeWebSocketHandle handle = 0;
  TaskCpuTelemetry receiveCpu;
  esp_websocket_client_handle_t client = nullptr;
  esp_transport_handle_t transport = nullptr;
  esp_transport_handle_t parentTransport = nullptr;
  std::uint16_t localPort = 0;
  std::chrono::steady_clock::time_point bufferedSnapshotAt{};
  std::chrono::steady_clock::time_point receiveSlowLogAt{};

  ~EspWsState() {
    // An external transport remains caller-owned. The client and its RX task
    // have already stopped before the final state owner releases these handles.
    if (transport) esp_transport_destroy(transport);
    if (parentTransport) esp_transport_destroy(parentTransport);
  }
  std::mutex mutex;
  std::deque<std::string> outgoing;
  TextProducer producer;
  std::size_t bytes = 0;
  std::atomic<bool> closing{false};
  std::atomic<bool> finished{false};
  TaskHandle_t worker = nullptr;
};

std::unordered_map<NativeWebSocketHandle, std::shared_ptr<EspWsState>> &espStateTable() {
  static std::unordered_map<NativeWebSocketHandle, std::shared_ptr<EspWsState>> table;
  return table;
}

std::vector<std::shared_ptr<EspWsState>> &retiredStates() {
  static std::vector<std::shared_ptr<EspWsState>> states;
  return states;
}

// Inspect lwIP only on its own TCP/IP task. At most one snapshot may be queued;
// the request carries scalar identity, never a PCB pointer or socket owner.
std::atomic<bool> tcpSnapshotPending{false};
std::atomic<bool> tcpSnapshotReady{false};

struct TcpSnapshot {
  NativeWebSocketHandle handle = 0;
  std::uint16_t localPort = 0;
  long sendMs = 0;
  bool found = false;
  unsigned state = 0, flags = 0, refused = 0, sndBuf = 0, sndQueue = 0;
  unsigned cwnd = 0, sndWnd = 0, rcvWnd = 0, nrtx = 0;
  unsigned unacked = 0, unsent = 0, unackedBytes = 0, unsentBytes = 0;
  int receiveQueue = -1, rtime = 0, rto = 0, sa = 0, sv = 0;
};
TcpSnapshot tcpSnapshot;
std::int64_t tcpSnapshotAt = 0; // Accessed only while owning pending.

void printTcpSnapshot() {
  if (!tcpSnapshotReady.exchange(false, std::memory_order_acquire)) return;
  const auto snapshot = tcpSnapshot;
  // The copied value is now independent of the next TCP/IP callback. Keep
  // both logging and heap inspection on the caller, never on the TCP/IP task.
  tcpSnapshotPending.store(false, std::memory_order_release);
  if (!snapshot.found) {
    ESP_LOGI("gea::host::ws", "tcp handle=%u send_ms=%ld pcb=closed", unsigned(snapshot.handle), snapshot.sendMs);
    return;
  }
  ESP_LOGI("gea::host::ws", "tcp handle=%u send_ms=%ld state=%u flags=%u recv_queue=%d refused=%u snd_buf=%u snd_queue=%u cwnd=%u snd_wnd=%u rcv_wnd=%u nrtx=%u rtime=%d rto=%d sa=%d sv=%d unacked=%u/%u unsent=%u/%u heap=%u largest=%u",
           unsigned(snapshot.handle), snapshot.sendMs, snapshot.state, snapshot.flags,
           snapshot.receiveQueue, snapshot.refused, snapshot.sndBuf, snapshot.sndQueue,
           snapshot.cwnd, snapshot.sndWnd, snapshot.rcvWnd, snapshot.nrtx,
           snapshot.rtime, snapshot.rto, snapshot.sa, snapshot.sv,
           snapshot.unacked, snapshot.unackedBytes, snapshot.unsent, snapshot.unsentBytes,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

void requestTcpSnapshot(const EspWsState &state, long sendMs) {
  if (!state.localPort || tcpSnapshotPending.exchange(true)) return;
  const auto now = esp_timer_get_time();
  if (now - tcpSnapshotAt < 1000000) {
    tcpSnapshotPending.store(false);
    return;
  }
  tcpSnapshotAt = now;
  tcpSnapshot = TcpSnapshot{};
  tcpSnapshot.handle = state.handle;
  tcpSnapshot.localPort = state.localPort;
  tcpSnapshot.sendMs = sendMs;
  const auto queued = tcpip_try_callback([](void *arg) {
    auto& snapshot = *static_cast<TcpSnapshot *>(arg);
    unsigned scanned = 0;
    for (auto *pcb = tcp_active_pcbs; pcb && scanned++ < 32; pcb = pcb->next) {
      if (pcb->local_port != snapshot.localPort) continue;
      snapshot.found = true;
      for (auto *segment = pcb->unacked; segment && snapshot.unacked < 64; segment = segment->next) {
        ++snapshot.unacked;
        snapshot.unackedBytes += segment->len;
      }
      for (auto *segment = pcb->unsent; segment && snapshot.unsent < 64; segment = segment->next) {
        ++snapshot.unsent;
        snapshot.unsentBytes += segment->len;
      }
      if (auto *connection = static_cast<netconn *>(pcb->callback_arg);
          connection && connection->pcb.tcp == pcb && sys_mbox_valid(&connection->recvmbox)) {
        snapshot.receiveQueue = int(uxQueueMessagesWaiting(reinterpret_cast<QueueHandle_t>(&connection->recvmbox->os_mbox)));
      }
      snapshot.state = unsigned(pcb->state);
      snapshot.flags = unsigned(pcb->flags);
      snapshot.refused = pcb->refused_data ? unsigned(pcb->refused_data->tot_len) : 0u;
      snapshot.sndBuf = unsigned(pcb->snd_buf);
      snapshot.sndQueue = unsigned(pcb->snd_queuelen);
      snapshot.cwnd = unsigned(pcb->cwnd);
      snapshot.sndWnd = unsigned(pcb->snd_wnd);
      snapshot.rcvWnd = unsigned(pcb->rcv_wnd);
      snapshot.nrtx = unsigned(pcb->nrtx);
      snapshot.rtime = int(pcb->rtime);
      snapshot.rto = int(pcb->rto);
      snapshot.sa = int(pcb->sa);
      snapshot.sv = int(pcb->sv);
      break;
    }
    tcpSnapshotReady.store(true, std::memory_order_release);
  }, &tcpSnapshot);
  if (queued != ERR_OK) {
    tcpSnapshotPending.store(false);
  }
}

void handleWsEvent(void *arg, esp_event_base_t, int32_t event_id, void *data) {
  auto *state = static_cast<EspWsState *>(arg);
  const auto handle = state->handle;
  // The synthetic close from the TX task must not contaminate RX CPU totals.
  if (event_id == WEBSOCKET_EVENT_CONNECTED || event_id == WEBSOCKET_EVENT_DATA)
    state->receiveCpu.sample("rx");
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
      // The SDK writes the tiny WebSocket header and payload as separate TLS
      // records. Nagle can otherwise hold the payload behind the header's ACK.
      const int socket = esp_transport_get_socket(state->transport);
      sockaddr_storage address{};
      socklen_t addressLength = sizeof(address);
      if (socket >= 0 && getsockname(socket, reinterpret_cast<sockaddr *>(&address), &addressLength) == 0) {
        if (address.ss_family == AF_INET)
          state->localPort = ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
#if LWIP_IPV6
        else if (address.ss_family == AF_INET6)
          state->localPort = ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
#endif
      }
      const int enabled = 1;
      int actual = 0;
      socklen_t optionLength = sizeof(actual);
      if (socket < 0 || setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
              &enabled, sizeof(enabled)) != 0 ||
          getsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &actual, &optionLength) != 0 ||
          actual != 1) {
        ESP_LOGE("gea::host::ws", "TCP_NODELAY failed errno=%d", errno);
        state->closing.store(true);
        enqueue(PendingEvent{handle, EventKind::Error, "WebSocket low-latency socket configuration failed", 0});
        break;
      }
      ESP_LOGI("gea::host::ws", "connected; tcp_nodelay=%d task stack free=%u",
               actual, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      requestTcpSnapshot(*state, 0);
      {
        std::lock_guard<std::mutex> lock(gea::host::instancesMutex());
        if (auto it = gea::host::instances().find(handle); it != gea::host::instances().end()) {
          if (it->second.readyState != WebSocket::CONNECTING) return;
          it->second.readyState = WebSocket::OPEN;
        }
      }
      gea::host::enqueue(gea::host::PendingEvent{handle, gea::host::EventKind::Open, {}, 0});
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      const auto *event = static_cast<esp_websocket_event_data_t *>(data);
      if (event && event->data_len >= 0 && event->payload_offset >= 0 && event->payload_len >= 0) {
        const auto began = std::chrono::steady_clock::now();
        const auto lockBegan = std::chrono::steady_clock::now();
        auto acquired = lockBegan;
        std::string complete;
        bool hasMessage = false;
        TextConsumer consumer;
        {
          std::lock_guard<std::mutex> lock(gea::host::instancesMutex());
          acquired = std::chrono::steady_clock::now();
          auto it = gea::host::instances().find(handle);
          if (it == gea::host::instances().end() || it->second.readyState != WebSocket::OPEN) break;
          auto &messages = it->second.messages;
          const auto result = messages.append(event->op_code, event->fin,
              event->payload_offset, event->payload_len,
              std::string_view(event->data_ptr ? event->data_ptr : "", event->data_len));
          if (result == TextMessageAssembler::Result::Complete) {
            complete = messages.take();
            hasMessage = true;
            consumer = it->second.consumer;
          } else if (result == TextMessageAssembler::Result::Invalid) {
            gea::host::enqueue(gea::host::PendingEvent{
                handle, gea::host::EventKind::Error, "Invalid or oversized WebSocket message", 0});
          }
        }
        if (hasMessage) {
          // Native PCM receive is as time-critical as its sender. Leaving the
          // SDK RX task at priority 5 while PCM TX runs at 23 can prevent RX
          // from draining the socket during a capture burst and close its TCP
          // window. Only native sinks opt in; ordinary JS sockets stay at the
          // SDK's default priority. This callback runs on the RX task itself.
          if (consumer && uxTaskPriorityGet(nullptr) != configMAX_PRIORITIES - 2)
            vTaskPrioritySet(nullptr, configMAX_PRIORITIES - 2);
          try {
            if (!consumer || !consumer(complete))
              enqueue(PendingEvent{handle, EventKind::Message, std::move(complete), 0});
          } catch (const std::exception& error) {
            enqueue(PendingEvent{handle, EventKind::Error, error.what(), 0});
          }
        }
        const auto finished = std::chrono::steady_clock::now();
        const auto callbackUs = std::chrono::duration_cast<std::chrono::microseconds>(finished - began).count();
        if (callbackUs > 5000 && finished - state->receiveSlowLogAt >= std::chrono::seconds(1)) {
          state->receiveSlowLogAt = finished;
          const auto lockUs = std::chrono::duration_cast<std::chrono::microseconds>(acquired - lockBegan).count();
          ESP_LOGI("gea::host::ws", "rx done handle=%u offset=%d callback_us=%ld lock_us=%ld",
                   unsigned(handle), event->payload_offset, long(callbackUs), long(lockUs));
        }
      }
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED: {
      {
        std::lock_guard<std::mutex> lock(gea::host::instancesMutex());
        if (auto it = gea::host::instances().find(handle); it != gea::host::instances().end()) {
          if (it->second.readyState == WebSocket::CLOSED) return;
          it->second.readyState = WebSocket::CLOSED;
        }
      }
      gea::host::enqueue(gea::host::PendingEvent{handle, gea::host::EventKind::Close, std::string("closed"), 1000});
      break;
    }
    case WEBSOCKET_EVENT_ERROR: {
      if (const auto* event = static_cast<esp_websocket_event_data_t*>(data)) {
        const auto& error = event->error_handle;
        // SDK leaves transport fields uninitialized for other error kinds.
        const bool transportError = error.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT;
        ESP_LOGE("gea::host::ws", "error handle=%u type=%d transport=%d tls=%d errno=%d http=%d",
                 unsigned(handle), int(error.error_type), transportError ? int(error.esp_tls_last_esp_err) : 0,
                 transportError ? error.esp_tls_stack_err : 0, transportError ? error.esp_transport_sock_errno : 0,
                 error.esp_ws_handshake_status_code);
        if (event->data_ptr && event->data_len > 0)
          ESP_LOGE("gea::host::ws", "error detail=%.*s", std::min(event->data_len, 512), event->data_ptr);
      }
      gea::host::enqueue(gea::host::PendingEvent{handle, gea::host::EventKind::Error, std::string("error"), 0});
      break;
    }
  }
}

}  // namespace

// One worker owns sends and teardown. The UI only enqueues bounded messages;
// TLS backpressure and close handshakes never hold the render/event thread.
struct SendWorker {
  NativeWebSocketHandle handle;
  std::shared_ptr<EspWsState> state;
};

void sendWorker(void *arg) {
  std::unique_ptr<SendWorker> job(static_cast<SendWorker *>(arg));
  const auto state = job->state;
  auto telemetryAt = std::chrono::steady_clock::now();
  auto snapshotAt = telemetryAt;
  auto slowSendLogAt = telemetryAt;
  TaskCpuTelemetry sendCpu;
  auto sendCpuAt = telemetryAt;
  unsigned sentPackets = 0;
  std::size_t sentBytes = 0;
  long maxSendMs = 0;
  while (!state->closing.load()) {
    printTcpSnapshot();
    sendCpu.sample("tx");
    std::string message;
    TextProducer producer;
    bool queued = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->outgoing.empty()) {
        message = std::move(state->outgoing.front());
        state->outgoing.pop_front();
        queued = true;
      } else {
        producer = state->producer;
      }
    }
    // Protocol/control messages go first. Capture then runs directly on this
    // worker: no UI timer, JS callback, or intermediate outgoing audio queue.
    if (producer) {
      try {
        message = producer();
      } catch (const std::exception &error) {
        enqueue(PendingEvent{job->handle, EventKind::Error, error.what(), 0});
        state->closing.store(true);
        continue;
      }
    }
    if (state->closing.load()) break;
    if (message.empty()) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const auto sendStarted = std::chrono::steady_clock::now();
    if (sendStarted - snapshotAt >= std::chrono::seconds(1)) {
      requestTcpSnapshot(*state, -1);
      snapshotAt = sendStarted;
    }
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    const bool measureSendCpu = sendStarted - sendCpuAt >= std::chrono::seconds(1);
    const auto cpuBefore = measureSendCpu ? TaskCpuTelemetry::currentCpu() : 0;
#endif
    // TCP's initial retransmit timeout is 1.5 s. A 1 s application deadline
    // aborted healthy connections before TCP could recover a single lost packet.
    const auto sent = esp_websocket_client_send_text(state->client, message.data(),
        static_cast<int>(message.size()), pdMS_TO_TICKS(5000));
    const auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sendStarted).count();
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    if (measureSendCpu) {
      const auto cpuUsed = static_cast<configRUN_TIME_COUNTER_TYPE>(TaskCpuTelemetry::currentCpu() - cpuBefore);
      ESP_LOGI("gea::host::ws", "send cpu_us=%llu wall_ms=%ld bytes=%u",
               static_cast<unsigned long long>(cpuUsed), long(sendMs), unsigned(message.size()));
      sendCpuAt = std::chrono::steady_clock::now();
    }
#endif
    const auto sendFinished = std::chrono::steady_clock::now();
    if (sendMs > 80 && sendFinished - slowSendLogAt >= std::chrono::seconds(1)) {
      slowSendLogAt = sendFinished;
      ESP_LOGW("gea::host::ws", "send bytes=%u duration_ms=%ld", unsigned(message.size()), long(sendMs));
      requestTcpSnapshot(*state, long(sendMs));
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (queued) state->bytes -= message.size();
    }
    if (sent != static_cast<int>(message.size())) {
      enqueue(PendingEvent{job->handle, EventKind::Error, "WebSocket send failed", 0});
      state->closing.store(true);
    } else {
      ++sentPackets;
      sentBytes += message.size();
      maxSendMs = std::max(maxSendMs, long(sendMs));
      const auto now = std::chrono::steady_clock::now();
      const auto spanMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - telemetryAt).count();
      if (spanMs >= 2000) {
        std::size_t queuedBytes;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          queuedBytes = state->bytes;
        }
        ESP_LOGI("gea::host::ws", "sent packets=%u bytes=%u span_ms=%ld max_send_ms=%ld queued_bytes=%u",
                 sentPackets, unsigned(sentBytes), long(spanMs), maxSendMs, unsigned(queuedBytes));
        sentPackets = 0;
        sentBytes = 0;
        maxSendMs = 0;
        telemetryAt = now;
      }
    }
  }
  // Stop also interrupts CONNECTING. Client destruction waits for its event
  // task here, not in a UI handler. State remains alive until both are done.
  esp_websocket_client_stop(state->client);
  esp_websocket_client_destroy(state->client);
  state->client = nullptr;
  printTcpSnapshot();
  handleWsEvent(state.get(), nullptr, WEBSOCKET_EVENT_DISCONNECTED, nullptr);
  job.reset();
  // Release the final local reference before deleting the FreeRTOS task.
  // (C++ automatic destructors don't run after vTaskDelete.)
  // The scope below is provided by the task trampoline in platform_open.
}

void platform_open(NativeWebSocketHandle handle, const std::string &url, const std::string &protocols) {
  std::lock_guard platformLock(platformMutex);
  esp_websocket_client_config_t config = {};
  config.uri = url.c_str();
  config.subprotocol = protocols.empty() ? nullptr : protocols.c_str();
  config.task_stack = 8192;
  config.buffer_size = 4096;
  config.disable_auto_reconnect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  auto state = std::make_shared<EspWsState>();
  state->handle = handle;
  http_parser_url parsed{};
  http_parser_url_init(&parsed);
  if (http_parser_parse_url(url.c_str(), url.size(), 0, &parsed) != 0) {
    enqueue(PendingEvent{handle, EventKind::Error, "Invalid WebSocket URL", 0});
    return;
  }
  const auto field = [&](http_parser_url_fields name) {
    return url.substr(parsed.field_data[name].off, parsed.field_data[name].len);
  };
  const auto scheme = field(UF_SCHEMA);
  const bool secure = scheme == "wss";
  if ((!secure && scheme != "ws") || parsed.field_data[UF_USERINFO].len ||
      (parsed.field_set & (1 << UF_FRAGMENT))) {
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket URL must use ws/wss without credentials or a fragment", 0});
    return;
  }
  // Own the standard SDK transports to access their public socket API. Keep
  // certificate verification, SNI, TLS, frame parsing and masking in the SDK.
  state->parentTransport = secure ? esp_transport_ssl_init() : esp_transport_tcp_init();
  if (state->parentTransport) state->transport = esp_transport_ws_init(state->parentTransport);
  if (!state->transport) {
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket transport allocation failed", 0});
    return;
  }
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (secure) esp_transport_ssl_crt_bundle_attach(state->parentTransport, esp_crt_bundle_attach);
#endif
  esp_transport_set_default_port(state->parentTransport, secure ? 443 : 80);
  esp_transport_set_default_port(state->transport, secure ? 443 : 80);
  auto path = field(UF_PATH);
  if (path.empty()) path = "/";
  if (parsed.field_set & (1 << UF_QUERY)) path += "?" + field(UF_QUERY);
  esp_transport_ws_config_t transportConfig{};
  transportConfig.ws_path = path.c_str();
  transportConfig.sub_protocol = config.subprotocol;
  transportConfig.propagate_control_frames = true;
  if (esp_transport_ws_set_config(state->transport, &transportConfig) != ESP_OK) {
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket transport configuration failed", 0});
    return;
  }
  config.ext_transport = state->transport;
  state->client = esp_websocket_client_init(&config);
  if (!state->client) {
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket initialization failed", 0});
    return;
  }
  esp_websocket_register_events(state->client, WEBSOCKET_EVENT_ANY, handleWsEvent, state.get());
  espStateTable()[handle] = state;
  if (esp_websocket_client_start(state->client) != ESP_OK) {
    esp_websocket_client_destroy(state->client);
    espStateTable().erase(handle);
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket startup failed", 0});
    return;
  }
  auto *job = new SendWorker{handle, state};
  const auto stackCaps = MALLOC_CAP_8BIT | (heap_caps_get_total_size(MALLOC_CAP_SPIRAM)
      ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
  if (xTaskCreateWithCaps([](void *p) {
        auto *state = static_cast<SendWorker *>(p)->state.get();
        sendWorker(p); // All C++ owners unwind before publishing completion.
        state->finished.store(true, std::memory_order_release);
        vTaskSuspend(nullptr); // UI reaps the stopped task and its PSRAM stack.
      }, "gea-ws-send", 6144, job, 5, &state->worker,
      stackCaps) != pdPASS) {
    delete job;
    esp_websocket_client_stop(state->client);
    esp_websocket_client_destroy(state->client);
    espStateTable().erase(handle);
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket worker allocation failed", 0});
  }
}

std::size_t platform_buffered(NativeWebSocketHandle handle) {
  std::lock_guard platformLock(platformMutex);
  const auto it = espStateTable().find(handle);
  if (it == espStateTable().end()) return 0;
  const auto now = std::chrono::steady_clock::now();
  if (now - it->second->bufferedSnapshotAt >= std::chrono::seconds(1)) {
    requestTcpSnapshot(*it->second, -2);
    it->second->bufferedSnapshotAt = now;
  }
  std::lock_guard<std::mutex> lock(it->second->mutex);
  return it->second->bytes;
}

void platform_set_producer(NativeWebSocketHandle handle, TextProducer producer) {
  std::lock_guard platformLock(platformMutex);
  const auto it = espStateTable().find(handle);
  if (it == espStateTable().end() || it->second->closing.load())
    throw std::runtime_error("WebSocket is not available for PCM streaming");
  std::lock_guard<std::mutex> lock(it->second->mutex);
  it->second->producer = std::move(producer);
  // A native PCM producer is a realtime audio resource. The sender must not
  // sit at generic background priority while the UI runs at priority 23.
  // Capture/mixer remain above both tasks at priority 24.
  if (it->second->producer && it->second->worker)
    vTaskPrioritySet(it->second->worker, configMAX_PRIORITIES - 2);
}

void platform_send(NativeWebSocketHandle handle, const std::string &data) {
  std::lock_guard platformLock(platformMutex);
  const auto it = espStateTable().find(handle);
  if (it == espStateTable().end() || it->second->closing.load()) return;
  auto &state = *it->second;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.bytes + data.size() > 512 * 1024) {
    state.closing.store(true);
    enqueue(PendingEvent{handle, EventKind::Error, "WebSocket send queue exceeded 512 KiB", 0});
    return;
  }
  state.outgoing.push_back(data);
  state.bytes += data.size();
}

void platform_close(NativeWebSocketHandle handle) {
  std::lock_guard platformLock(platformMutex);
  {
    std::lock_guard<std::mutex> lock(instancesMutex());
    const auto it = instances().find(handle);
    if (it != instances().end() && it->second.readyState != WebSocket::CLOSED)
      it->second.readyState = WebSocket::CLOSING;
  }
  const auto it = espStateTable().find(handle);
  if (it == espStateTable().end()) return;
  it->second->closing.store(true);
  retiredStates().push_back(it->second);
  espStateTable().erase(it);
}

void platform_destroy(NativeWebSocketHandle handle) { platform_close(handle); }

void platform_reap() {
  std::lock_guard platformLock(platformMutex);
  auto &retired = retiredStates();
  for (auto it = retired.begin(); it != retired.end();) {
    if (!(*it)->finished.load(std::memory_order_acquire) ||
        eTaskGetState((*it)->worker) != eSuspended) { ++it; continue; }
    vTaskDeleteWithCaps((*it)->worker);
    it = retired.erase(it);
  }
}

}  // namespace gea::host::websocket

#endif  // ESP_PLATFORM && network capability
