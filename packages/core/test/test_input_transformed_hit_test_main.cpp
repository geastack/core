#include "native_test_harness.h"

#include "ui/internal.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
} // namespace gea::framework::graphics::generated

int main() {
  using namespace gea::embedded::test;
  using namespace gea::embedded::ui;

  resetNativeHost();
  StyleSheet::instance().clear();
  setNativeDisplaySize(120, 120);
  gea::embedded::ui::setViewportMetrics(120, 120, 1.0);

  Tree &tree = Tree::instance();
  const int rootId = tree.createView();
  const int buttonId = tree.createButton();
  NodeHandle(rootId).appendChild(NodeHandle(buttonId));

  Node &root = tree.node(rootId);
  root.layout.x = 0;
  root.layout.y = 0;
  root.layout.width = 120;
  root.layout.height = 120;

  StyleSheet::instance().registerRule("translated-hit-button", "transform",
                                      "translate(-50%, -50%)");
  NodeHandle(buttonId).classList().set("translated-hit-button");

  Node &button = tree.node(buttonId);
  button.layout.x = 60;
  button.layout.y = 60;
  button.layout.width = 40;
  button.layout.height = 40;

  if (tree.hitTestNode(45, 45) != buttonId) {
    std::fprintf(stderr,
                 "[test_input_transformed_hit_test] expected translated visual "
                 "button area to be hit-tested at (45,45)\n");
    return 1;
  }

  if (tree.hitTestNode(95, 95) == buttonId) {
    std::fprintf(
        stderr, "[test_input_transformed_hit_test] expected untransformed "
                "button-only area to stop hit-testing the translated button\n");
    return 1;
  }

  const int staleButtonId = tree.createButton();
  NodeHandle(rootId).appendChild(NodeHandle(staleButtonId));
  NodeHandle(staleButtonId).classList().set("translated-hit-button");
  Node &staleButton = tree.node(staleButtonId);
  staleButton.layout.x = 358;
  staleButton.layout.y = 358;
  staleButton.layout.width = 480;
  staleButton.layout.height = 16;

  int16_t xs[4], ys[4];
  ViewRenderer::transformedRectCorners(
      staleButton, false, staleButton.layout.x, staleButton.layout.y,
      staleButton.layout.width, staleButton.layout.height, xs, ys);

  staleButton.layout.width = 47;
  staleButton.layout.height = 47;

  if (tree.hitTestNode(360, 340) != staleButtonId) {
    std::fprintf(stderr,
                 "[test_input_transformed_hit_test] expected stale transform "
                 "cache to be invalidated after layout size changes\n");
    return 1;
  }

  if (tree.hitTestNode(120, 360) == staleButtonId) {
    std::fprintf(stderr,
                 "[test_input_transformed_hit_test] expected stale pre-layout "
                 "translated area to stop hit-testing the button\n");
    return 1;
  }

  const int scaledCenterId = tree.createView();
  NodeHandle(rootId).appendChild(NodeHandle(scaledCenterId));
  StyleSheet::instance().registerRule("scaled-center-origin",
                                      "transform-origin", "50% 50%");
  NodeHandle(scaledCenterId).classList().set("scaled-center-origin");
  NodeHandle(scaledCenterId).style().scale(0.5);
  Node &scaledCenter = tree.node(scaledCenterId);
  scaledCenter.layout.x = 20;
  scaledCenter.layout.y = 20;
  scaledCenter.layout.width = 40;
  scaledCenter.layout.height = 40;
  ViewRenderer::transformedRectCorners(
      scaledCenter, false, scaledCenter.layout.x, scaledCenter.layout.y,
      scaledCenter.layout.width, scaledCenter.layout.height, xs, ys);
  if (xs[0] != 30 || ys[0] != 30 || xs[2] != 50 || ys[2] != 50) {
    std::fprintf(stderr,
                 "[test_input_transformed_hit_test] expected style().scale to "
                 "honor center transform-origin, got tl=(%d,%d) br=(%d,%d)\n",
                 xs[0], ys[0], xs[2], ys[2]);
    return 1;
  }

  return 0;
}
