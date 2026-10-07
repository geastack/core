// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(GEA_NATIVE_DEBUGGER) && GEA_NATIVE_DEBUGGER
#include "internal.h"
#include "tree_internal.h"
#include "tree_state.h"
#include <algorithm>
#include <cstdint>

namespace gea::embedded::ui {
// Owned by the app/render lock. A lease clears the overlay even if the host dies.
struct DebuggerOverlay {
  int slot = -1;
  std::uint32_t identity = 0;
  std::int64_t deadline = 0;
  bool repaint = false;
  gea::framework::graphics::pixel::native_t color = 0;

  void prepare() const {
    auto &state = treeState();
    if ((slot >= 0 || repaint) && state.mountedRoot >= 0) {
      // Overlay commands are separate from node paint ranges. Re-record while
      // visible so incremental, fused and banded replay all see current bounds.
      state.displayListDirty = true;
      state.displayListRebuildStructural = true;
      state.nodes[state.mountedRoot].render.dirty = true;
    }
  }
  void set(int node, gea::framework::graphics::pixel::native_t outline) {
    slot = node;
    identity = node >= 0 ? treeState().nodes[node].debugger_identity : 0;
    color = outline;
    deadline = std::int64_t(treeState().lastFrameMs) + 2500;
    repaint = true;
    prepare();
  }
  void record(int root, int width, int height) {
    auto &state = treeState();
    repaint = false;
    if (slot < 0) return;
    if (slot >= state.nodeCount || !state.nodeActive[slot] ||
        state.nodes[slot].debugger_identity != identity ||
        std::int64_t(state.lastFrameMs) >= deadline ||
        !Tree::instance().containsNode(root, slot)) {
      slot = -1;
      identity = 0;
      return;
    }
    for (int parent = slot; parent >= 0; parent = state.nodes[parent].parent)
      if (state.nodes[parent].computedStyle().display == kDisplayNone) return;
    int16_t xs[4], ys[4];
    ViewRenderer::transformedCorners(state.nodes[slot], false, xs, ys);
    int x0 = *std::min_element(xs, xs + 4), y0 = *std::min_element(ys, ys + 4);
    int x1 = *std::max_element(xs, xs + 4), y1 = *std::max_element(ys, ys + 4);
    if (x1 <= x0 || y1 <= y0) return;
    auto fill = [&](int x, int y, int w, int h) {
      const int right = std::min(width, x + w), bottom = std::min(height, y + h);
      x = std::max(0, x); y = std::max(0, y);
      w = right - x; h = bottom - y;
      if (w <= 0 || h <= 0) return;
      if (auto *command = DisplayList::instance().append()) {
        command->type = DisplayCommandType::FillRect;
        command->bx = command->fill.x = x;
        command->by = command->fill.y = y;
        command->bw = command->fill.w = w;
        command->bh = command->fill.h = h;
        command->fill.color = color;
      }
    };
    fill(x0, y0, x1 - x0, 2);
    fill(x0, y1 - 2, x1 - x0, 2);
    fill(x0, y0, 2, y1 - y0);
    fill(x1 - 2, y0, 2, y1 - y0);
  }
};
inline DebuggerOverlay debuggerOverlay;
} // namespace gea::embedded::ui
#endif
