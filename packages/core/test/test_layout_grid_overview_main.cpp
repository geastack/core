// SPDX-License-Identifier: Apache-2.0
// Layout work for a 1fr/1fr grid of flex-column key/value cells inside a
// scroller -- the diagnostics home overview, which laid every cell out ~88
// times per pass on device: each nested `position: absolute; inset: 0` box was
// shrunk to its content, laid out, then stretched and laid out again, doubling
// the work per level. Fails when a pass exceeds the per-node call budget.
#include "native_test_harness.h"
#include "graphics/font.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/refresh_perf.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
using namespace gea::embedded::ui;
using namespace gea::embedded::test;

static NodeHandle span(const char *text) {
  auto s = Document::instance().createElement("span");
  s.appendChild(Document::instance().createText(text));
  return s;
}

int main(int argc, char **argv) {
  const int maxCallsPerNode = argc > 1 ? std::atoi(argv[1]) : 8;
  resetNativeHost(); setNativeDisplaySize(640, 172); setViewportMetrics(640, 172, 1.0);
  auto &doc = Document::instance();
  auto root = doc.createView();
  root.style().setProperty("width", "100%");
  root.style().setProperty("height", "100%");
  // diag-root > diag-body: nested `position: absolute; inset: 0` boxes.
  auto diagRoot = doc.createView();
  diagRoot.style().setProperty("position", "absolute");
  diagRoot.style().setProperty("inset", "0");
  diagRoot.style().setProperty("overflow", "hidden");
  root.appendChild(diagRoot);
  auto body = doc.createView();
  body.style().setProperty("position", "absolute");
  body.style().setProperty("inset", "0");
  diagRoot.appendChild(body);
  auto scroller = doc.createView();
  scroller.style().setProperty("position", "absolute");
  scroller.style().setProperty("inset", "0");
  scroller.style().setProperty("overflow-y", "auto");
  body.appendChild(scroller);
  auto card = doc.createView();
  card.style().setProperty("padding", "12px 18px");
  scroller.appendChild(card);
  auto head = doc.createView();
  head.style().setProperty("display", "flex");
  head.style().setProperty("flex-direction", "column");
  head.appendChild(span("Waveshare LCD 3.49"));
  head.appendChild(span("ESP32-S3 (Xtensa)"));
  card.appendChild(head);
  auto grid = doc.createView();
  grid.style().setProperty("display", "grid");
  grid.style().setProperty("grid-template-columns", "1fr 1fr");
  grid.style().setProperty("gap", "8px 16px");
  card.appendChild(grid);
  const char *keys[] = {"CORES", "CPU", "FLASH", "PSRAM/RAM", "MAC / ID", "OS / SDK", "RESET", "UPTIME", "DIE TEMP"};
  for (const char *k : keys) {
    auto cell = doc.createView();
    cell.style().setProperty("display", "flex");
    cell.style().setProperty("flex-direction", "column");
    cell.appendChild(span(k));
    cell.appendChild(span("value 123"));
    grid.appendChild(cell);
  }
  for (int i = 0; i < 12; ++i) {
    auto row = doc.createView();
    row.style().setProperty("display", "flex");
    row.style().setProperty("align-items", "center");
    row.style().setProperty("padding", "10px 18px");
    auto text = doc.createView();
    text.style().setProperty("display", "flex");
    text.style().setProperty("flex-direction", "column");
    text.style().setProperty("flex", "1");
    text.appendChild(span("MENU ITEM"));
    text.appendChild(span("detail"));
    row.appendChild(text);
    row.appendChild(span("LIVE"));
    scroller.appendChild(row);
  }
  auto &tree = Tree::instance();
  int worst = 0;
  for (int frame = 0; frame < 3; ++frame) {
    refreshPerfStatsMutable() = {};
    tree.mount(root.id(), 640, 172);
    const auto stats = refreshPerfStatsMutable();
    std::uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < tree.nodeCount(); ++i) {
      const auto &b = tree.node(i).layout;
      for (int v : {int(b.x), int(b.y), int(b.width), int(b.height)}) hash = (hash ^ std::uint32_t(v)) * 1099511628211ull;
    }
    std::printf("frame=%d nodes=%d calls=%d hits=%d hash=%016llx\n", frame, tree.nodeCount(), stats.treeLayoutNodeCalls,
                stats.treeLayoutMemoHits, static_cast<unsigned long long>(hash));
    worst = std::max(worst, stats.treeLayoutNodeCalls / std::max(1, tree.nodeCount()));
  }
  if (worst > maxCallsPerNode) {
    std::printf("FAIL: %d layout calls per node exceeds %d\n", worst, maxCallsPerNode);
    return 1;
  }
  return 0;
}
