// SPDX-License-Identifier: Apache-2.0
#pragma once

// Debug builds only. JSX listeners are delegated to the body, so DevTools
// cannot learn which node a handler belongs to from the event system. The
// runtime names the node through GEA_JSX_NODE_LISTENER_HOOK; this records the
// event type and the registering call stack so the host can link each handler
// to the TSX line that attached it.
#if defined(ESP_PLATFORM) && defined(GEA_NATIVE_DEBUGGER) && GEA_NATIVE_DEBUGGER
#include "tree_state.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(__XTENSA__)
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#endif

namespace gea::embedded::ui {
struct DebuggerListener {
  // Registration passes through runtime helpers whose depth varies with
  // inlining; the host picks the first frame that maps to app source.
  static constexpr int kSites = 6;
  std::string type;
  std::uint32_t sites[kSites] = {};
  int count = 0;
};
// Keyed by lifetime identity, never by slot: a recycled slot must not inherit
// the previous node's handlers. Owned by the app/render lock.
inline std::unordered_map<std::uint32_t, std::vector<DebuggerListener>> &debuggerListeners() {
  static std::unordered_map<std::uint32_t, std::vector<DebuggerListener>> listeners;
  return listeners;
}
// Drop registrations of nodes that no longer exist so list churn cannot grow
// the registry without bound while no debugger is connected.
inline void debuggerPruneListeners() {
  auto &state = treeState();
  std::unordered_set<std::uint32_t> live;
  for (int i = 0; i < kMaxNodes; ++i)
    if (state.nodeActive[i])
      live.insert(state.nodes[i].debugger_identity);
  auto &listeners = debuggerListeners();
  for (auto it = listeners.begin(); it != listeners.end();)
    it = live.count(it->first) ? std::next(it) : listeners.erase(it);
}
// Not inlined: the backtrace starts at this function's caller.
[[gnu::noinline]] inline void debuggerNoteListener(int slot, const char *type) {
  auto &state = treeState();
  if (slot < 0 || slot >= kMaxNodes || !state.nodeActive[slot] || !type)
    return;
  DebuggerListener listener;
  listener.type = type;
#if defined(__XTENSA__)
  esp_backtrace_frame_t frame = {};
  esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
  while (listener.count < DebuggerListener::kSites) {
    // The call instruction itself, not the return address after it.
    listener.sites[listener.count++] = esp_cpu_process_stack_pc(frame.pc);
    if (!frame.next_pc || !esp_backtrace_get_next_frame(&frame))
      break;
  }
#else
  listener.sites[listener.count++] =
      std::uint32_t(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
#endif
  static unsigned notes = 0;
  if (++notes % 128 == 0)
    debuggerPruneListeners();
  auto &list = debuggerListeners()[state.nodes[slot].debugger_identity];
  if (list.size() < 16)
    list.push_back(std::move(listener));
}
} // namespace gea::embedded::ui

#ifndef GEA_JSX_NODE_LISTENER_HOOK
#define GEA_JSX_NODE_LISTENER_HOOK(node, type) ::gea::embedded::ui::debuggerNoteListener((node), (type))
#endif
#endif
