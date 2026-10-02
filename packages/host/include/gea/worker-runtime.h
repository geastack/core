// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifndef GEA_HOST_DECLARED
#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#endif
#include "gea_runtime.h"
#include "host/worker.h"
#include <unordered_set>

namespace gea::runtime::hostworker {
namespace native = gea::host::workers;

template <typename T> struct EventFactory {
  static T make() { return {}; }
  static T &fields(T &event) { return event; }
};
template <typename T> struct EventFactory<gea::Ref<T>> {
  static gea::Ref<T> make() { return gea::makeRef<T>(); }
  static T &fields(gea::Ref<T> &event) { return *event; }
};
template <typename Target, typename Head, typename... Tail>
constexpr std::size_t armIndex() {
  if constexpr (std::is_same_v<Target, Head>) return 0;
  else { static_assert(sizeof...(Tail) > 0, "Event field cannot hold this message payload"); return 1 + armIndex<Target, Tail...>(); }
}
template <typename Slot, typename T> void assign(Slot &slot, const T &value) { slot = value; }
template <typename... Arms, typename T>
void assign(gea::TaggedUnion<Arms...> &slot, const T &value) {
  slot = gea::TaggedUnion<Arms...>::template ofArm<armIndex<T, Arms...>()>(value);
}

inline void installRealmRuntime() {
  static const bool installed = [] {
    native::setRealmRunner([](std::function<void()> run) {
#if defined(GEA_RUNTIME_REALMS) && GEA_RUNTIME_REALMS
      gea::detail::RuntimeRealm realm;
      gea::detail::RuntimeRealmScope scope(realm);
      native::Context::current()->setPump([] { gea::detail::drainPromiseJobs(); });
      run();
      realm.clear();
#else
      throw std::runtime_error("Workers require GEA_RUNTIME_REALMS=1");
#endif
    });
    return true;
  }();
  (void)installed;
}
inline native::Worker createWorker(const std::string &url) {
  installRealmRuntime();
  return native::Worker::create(url);
}
template <typename T, typename Fn> void present(const T &value, Fn &&fn);
template <typename Options>
native::Worker createWorker(const std::string &url, const Options &options) {
  if constexpr (requires { options->type; }) {
    if (options) return createWorker(url, *options);
  } else if constexpr (requires { options.type; }) {
    bool provided = true;
    if constexpr (requires { options.gea_present_type; }) provided = options.gea_present_type;
    if (provided) present(options.type, [](const auto &type) {
      if (type != "module") throw std::runtime_error("Only module workers are supported");
    });
  }
  return createWorker(url);
}

// These adapters keep compiler handles on their creating task. Only owned byte
// vectors and native port handles enter the inter-task queue.
template <typename T, typename Fn>
void visit(const T &value, Fn &&fn) {
  if constexpr (requires { T::arity; value.index(); }) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((value.index() == I ? (visit(value.template get<I>(), fn), 0) : 0), ...);
    }(std::make_index_sequence<T::arity>{});
  } else fn(value);
}
template <typename T, typename Fn>
void present(const T &value, Fn &&fn) {
  if constexpr (std::is_same_v<T, std::nullptr_t> || std::is_same_v<T, gea::Undefined>) return;
  else if constexpr (requires { value.has_value(); *value; }) { if (value.has_value()) present(*value, fn); }
  else if constexpr (requires { T::arity; value.index(); }) visit(value, [&](const auto &item) { present(item, fn); });
  else fn(value);
}
template <typename Buffer> struct BufferTransferList {
  std::vector<gea::Ref<Buffer>> buffers;
  std::vector<native::MessagePort> ports;
};
using TransferList = BufferTransferList<gea::ArrayBuffer>;
template <typename List>
TransferList transfers(const List &input) {
  TransferList result;
  std::unordered_set<const void *> seen;
  auto inspect = [&](const auto &array) {
    for (std::size_t i = 0; i < array.size(); ++i) visit(array.at(i), [&](const auto &entry) {
      using T = std::decay_t<decltype(entry)>;
      if constexpr (std::is_same_v<T, gea::Ref<gea::ArrayBuffer>>) {
        if (!entry || entry->detached() || !seen.insert(entry.operator->()).second)
          throw std::runtime_error("DataCloneError: invalid ArrayBuffer transfer list");
        result.buffers.push_back(entry);
      } else if constexpr (std::is_same_v<T, native::MessagePort>) {
        if (entry.detached() || !seen.insert(entry.identity()).second)
          throw std::runtime_error("DataCloneError: invalid MessagePort transfer list");
        result.ports.push_back(entry);
      } else {
        static_assert(std::is_same_v<T, void>, "Unsupported transferable type");
      }
    });
  };
  if constexpr (requires { input->size(); }) inspect(*input);
  else inspect(input);
  return result;
}
// Keep buffer operations dependent on the instantiated bridge. Older compiler
// runtimes have no detachment API, but must still be able to include the host
// declarations in applications that do not use workers. An actual worker bridge
// still requires the real detachment API; no copy or no-op substitutes for it.
template <typename Data, typename Buffer>
native::MessageData cloneData(const Data &data, BufferTransferList<Buffer> &list) {
  native::MessageData result;
  visit(data, [&](const auto &value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_convertible_v<T, std::string>) result = std::string(value);
    else if constexpr (std::is_same_v<T, gea::Ref<gea::ArrayBuffer>>) {
      if (!value || value->detached()) throw std::runtime_error("DataCloneError: detached ArrayBuffer");
      const bool transfer = std::any_of(list.buffers.begin(), list.buffers.end(),
          [&](const auto &candidate) { return candidate.operator->() == value.operator->(); });
      if (transfer) result = value->detachBytes();
      else result = native::MessageData::Bytes(value->begin(), value->end());
    } else static_assert(std::is_same_v<T, void>, "Only string and ArrayBuffer messages are implemented");
  });
  for (const auto &buffer : list.buffers) if (!buffer->detached()) buffer->detachBytes();
  return result;
}
template <typename Owner, typename Data>
void postMessage(const Owner &owner, const Data &data) {
  TransferList list;
  owner.postMessage(cloneData(data, list));
}
template <typename Owner, typename Data, typename List>
void postMessage(const Owner &owner, const Data &data, const List &transfer) {
  auto list = transfers(transfer);
  owner.validateTransfers(list.ports);
  owner.postMessage(cloneData(data, list), list.ports);
}
template <typename R, typename E>
void deliver(const gea::CallableObject<R(E)> &handler, native::MessageEvent &message) {
  E event = EventFactory<E>::make();
  auto &fields = EventFactory<E>::fields(event);
  if (std::holds_alternative<std::string>(message.data.value)) {
    assign(fields.data, std::get<std::string>(message.data.value));
  } else {
    assign(fields.data, gea::makeRef<gea::ArrayBuffer>(
        std::move(std::get<native::MessageData::Bytes>(message.data.value))));
  }
  if constexpr (requires { fields.ports; }) {
    auto ports = gea::makeRef<gea::ArrayObject<native::MessagePort>>();
    for (const auto &port : message.ports) ports->push(port);
    assign(fields.ports, ports);
  }
  handler.call(event);
}
template <typename R>
void deliver(const gea::CallableObject<R()> &handler, native::MessageEvent &) { handler.call(); }
template <typename Owner, typename Handler>
void setOnMessage(const Owner &owner, const Handler &handler) {
  owner.setOnMessage({});
  present(handler, [&](const auto &fn) {
    owner.setOnMessage([fn](native::MessageEvent &message) { deliver(fn, message); });
  });
}
template <typename R, typename E>
void deliverError(const gea::CallableObject<R(E)> &handler, const std::string &message) {
  E event = EventFactory<E>::make();
  if constexpr (requires { EventFactory<E>::fields(event).message; })
    assign(EventFactory<E>::fields(event).message, message);
  handler.call(event);
}
template <typename R>
void deliverError(const gea::CallableObject<R()> &handler, const std::string &) { handler.call(); }
template <typename Handler>
void setOnError(const native::Worker &worker, const Handler &handler) {
  worker.setOnError({});
  present(handler, [&](const auto &fn) {
    worker.setOnError([fn](const std::string &message) { deliverError(fn, message); });
  });
}
}  // namespace gea::runtime::hostworker
