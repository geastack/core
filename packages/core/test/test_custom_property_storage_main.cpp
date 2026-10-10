// SPDX-License-Identifier: Apache-2.0
// Count the complete allocator cost of sparse, copied and mutated CSS variables.
#ifndef GEA_STORE_TEST_ONLY
#include "native_test_harness.h"
#endif
#include "graphics/font.h"
#include "ui/tree_state.h"
#include <array>
#include <cassert>
#include <cstdio>
#include "heap_census.h"
#include <vector>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
using namespace gea::embedded::ui;
struct Sample { const char *phase; HeapCensus heap{}; unsigned long long hash = 1469598103934665603ull; };
int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifndef GEA_STORE_TEST_ONLY
  gea::embedded::test::resetNativeHost();
#endif
  const std::array<CssAtomId,4> names{internCssAtom("--lit"),internCssAtom("--halo"),internCssAtom("--ink"),internCssAtom("--off")};
  const std::array<const char *,4> colors{"#e5c875", "#91bdd0", "#82bca9", "#d39aad"};
  std::vector<NodeCustomPropertyStore> authored(32), resolved(32);
  std::array<Sample,5> samples{};
  auto capture = [&](int phase, const char *name) {
    auto &sample = samples[phase]; sample.phase = name;
    captureHeapCensus(&sample.heap);
    for (const auto *stores : {&authored, &resolved}) for (const auto &store : *stores) for (auto key : names) {
      const auto *entry = store.getEntry(key);
      sample.hash = (sample.hash ^ (entry ? entry->flags : 255)) * 1099511628211ull;
      if (entry) {
        sample.hash = (sample.hash ^ entry->colorStyle) * 1099511628211ull;
        sample.hash = (sample.hash ^ entry->colorNative) * 1099511628211ull;
        for (unsigned char c : *entry->value) sample.hash = (sample.hash ^ c) * 1099511628211ull;
      }
    }
  };
  // The full-color specialization must preserve the high/sign bit too.
#if GEA_PIXEL_FORMAT_IS_8888
  authored[0].setColor(names[0], "wide-color", INT32_MIN, -1, 128);
  assert(authored[0].getEntry(names[0])->colorStyle == INT32_MIN);
  assert(authored[0].getEntry(names[0])->colorNative == -1);
  authored[0].clear();
#endif
  capture(0,"empty-maps");
  for (auto &store : authored) for (int i=0;i<4;++i) store.setColor(names[i], colors[i], 65535-i, 65000-i, 255);
  capture(1,"authored-variables");
  for (int i=0;i<32;++i) resolved[i] = authored[i];
  capture(2,"copied-variables");
  for (int i=0;i<32;++i) {
    resolved[i].set(names[0], "changed");
    assert(*authored[i].get(names[0]) == colors[0]);
    assert(*resolved[i].get(names[0]) == "changed");
  }
  capture(3,"independent-mutations");
  for (auto &store : resolved) store.clear();
  capture(4,"cleared-copies");
  for (const auto &sample : samples) std::printf("%s,%zu,%u,%016llx\n", sample.phase, sample.heap.size_in_use, sample.heap.blocks_in_use, sample.hash);
}
