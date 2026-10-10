#include "events.h"
#include <cassert>

int main() {
  using gea::framework::events::PointerEvent;
  PointerEvent original{};
  auto compiledCallback = [](PointerEvent copy) {
    copy.preventDefault();
    copy.stopPropagation();
  };
  compiledCallback(original);
  assert(original.defaultPrevented);
  assert(original.propagationStopped);
  PointerEvent sibling = original;
  original.defaultPrevented = false;
  assert(!sibling.defaultPrevented);
  PointerEvent independent{};
  assert(!independent.propagationStopped);
  independent.cancelable = false;
  compiledCallback(independent);
  assert(!independent.defaultPrevented);
  assert(independent.propagationStopped);
  PointerEvent retained{};
  {
    PointerEvent temporary{};
    retained = temporary;
    temporary.preventDefault();
  }
  assert(retained.defaultPrevented);
  return 0;
}
