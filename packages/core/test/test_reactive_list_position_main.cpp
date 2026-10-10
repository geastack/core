#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#include "graphics/font.h"
#include "../../../../compiler/src/targets/cpp/runtime/gea_runtime.h"
#include "native_test_harness.h"
#include <cassert>
#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() { gea::jsx::detail::drainMicrotasks(); }
}
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData* lookupFontForFamily(int, int) { return nullptr; }
}

using Node = gea::embedded::ui::NodeHandle;
using Rows = gea::Ref<gea::ArrayObject<Node>>;
struct ListThunk {
  Rows* current;
  Rows call() const { return *current; }
};

static Rows rows(int count) {
  auto result = gea::makeRef<gea::ArrayObject<Node>>();
  for (int i = 0; i < count; ++i) result->push(gea::jsx::create<Node>("div"));
  return result;
}

static void expectOrder(Node parent, Node title, Rows current, Node add) {
  Node child = parent.firstChildHandle();
  assert(child.id() == title.id());
  child = child.nextSiblingHandle();
  if (current && current->length()) {
    for (const auto& slot : current->slots()) {
      if (!slot.present) continue;
      assert(child.id() == slot.value.id());
      child = child.nextSiblingHandle();
    }
  } else {
    // Only an absent list needs a hidden position marker.
    assert(child && child.id() != add.id());
    child = child.nextSiblingHandle();
  }
  assert(child.id() == add.id());
  assert(!child.nextSiblingHandle());
}

int main() {
  for (int initial : {0, 2}) {
    Node parent = gea::jsx::create<Node>("div");
    Node title = gea::jsx::create<Node>("span");
    Node add = gea::jsx::create<Node>("button");
    gea::jsx::child(parent, title);
    Rows current = rows(initial);
    auto apply = gea::jsx::reactiveListApply(parent, ListThunk{&current}, current);
    gea::jsx::child(parent, add);
    expectOrder(parent, title, current, add);
    for (int count : {2, 1, 0, 3, 0, 0, 1}) {
      current = rows(count);
      apply();
      apply(); // Same-turn notifications coalesce.
      gea::jsx::detail::drainMicrotasks();
      expectOrder(parent, title, current, add);
    }
    // An already queued rebuild cannot operate on a disposed parent.
    apply();
    gea::jsx::detail::releaseSubtreeSubscriptions(parent);
    parent.remove();
    gea::jsx::detail::drainMicrotasks();
  }
  std::puts("reactive list sibling positions: passed");
}
