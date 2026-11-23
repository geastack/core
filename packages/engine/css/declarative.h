// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// gea::css — declarative animations from DOM data-attributes.
//
// ESP32 applications are TSX→C++ (geatsc) and reach native code only via the
// gea::host::* binding layer. Rather than thread a gea.animate() host call through
// several geatsc lowering files, we lean on a capability geatsc already has: it
// forwards arbitrary *static* JSX attributes to NodeHandle::setAttribute (the
// renderer ignores unknown attributes, but getAttribute reads them back). So apps
// annotate elements declaratively:
//
//   <div id="b" data-anim="rotate" data-anim-from="0" data-anim-to="360"
//        data-anim-dur="1500" data-anim-ease="linear" data-anim-iter="-1" />
//
// and this scanner walks the mounted tree once after the app mounts, parses those
// attributes, and starts the corresponding gea::css animations (which the runtime
// already ticks every frame). Header-only so it links into runtime/launch-surface
// TUs without a build source-list change. Reusable by any app (watch face,
// settings, the showcase). See docs/geaos-grand-vision.md (§8c, M3).

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "engine.h"
#include "ui/tree_internal.h"

namespace gea::css {

class DeclarativeAnimations {
public:
  // Walk the mounted tree for [data-anim] elements and start each one's animation.
  // Call once after an app mounts; nowMs is the current frame clock.
  static void scanAndStart(uint32_t nowMs)
  {
#if GEA_CSS_ANIMATIONS
    gea::embedded::ui::Tree &tree = gea::embedded::ui::Tree::instance();
    const int count = tree.nodeCount();
    for (int id = 0; id < count; ++id) {
      const char *kind = tree.getAttribute(id, "data-anim");
      if (!kind || kind[0] == '\0') continue;
      startForNode(tree, id, kind, nowMs);
    }
#else
    (void)nowMs;
#endif
  }

private:
#if GEA_CSS_ANIMATIONS
  static int attrInt(gea::embedded::ui::Tree &tree, int id, const char *name, int def)
  {
    const char *v = tree.getAttribute(id, name);
    if (!v || v[0] == '\0') return def;
    if (v[0] == '#') return static_cast<int>(std::strtol(v + 1, nullptr, 16));
    return static_cast<int>(std::strtol(v, nullptr, 0));  // base 0 → 0x.. auto-detected
  }

  static Easing easingFromAttr(const char *name)
  {
    if (!name || name[0] == '\0') return Easing::ease();
    if (!std::strcmp(name, "linear")) return Easing::linear();
    if (!std::strcmp(name, "ease")) return Easing::ease();
    if (!std::strcmp(name, "ease-in")) return Easing::easeIn();
    if (!std::strcmp(name, "ease-out")) return Easing::easeOut();
    if (!std::strcmp(name, "ease-in-out")) return Easing::easeInOut();
    if (!std::strncmp(name, "steps(", 6)) {
      const int n = static_cast<int>(std::strtol(name + 6, nullptr, 10));
      return Easing::steps(n > 0 ? n : 1);
    }
    if (!std::strncmp(name, "cubic-bezier(", 13)) {
      const char *p = name + 13;
      char *end = nullptr;
      const double x1 = std::strtod(p, &end);
      const double y1 = (end && *end == ',') ? std::strtod(end + 1, &end) : 0.0;
      const double x2 = (end && *end == ',') ? std::strtod(end + 1, &end) : 1.0;
      const double y2 = (end && *end == ',') ? std::strtod(end + 1, &end) : 1.0;
      return Easing::cubicBezier(x1, y1, x2, y2);
    }
    return Easing::ease();
  }

  static Direction directionFromAttr(const char *dir)
  {
    if (!dir || dir[0] == '\0') return Direction::Normal;
    if (!std::strcmp(dir, "alternate")) return Direction::Alternate;
    if (!std::strcmp(dir, "alternate-reverse")) return Direction::AlternateReverse;
    if (!std::strcmp(dir, "reverse")) return Direction::Reverse;
    return Direction::Normal;
  }

  static void startForNode(gea::embedded::ui::Tree &tree, int id, const char *kind, uint32_t nowMs)
  {
    using Property = gea::embedded::ui::Property;

    const int from = attrInt(tree, id, "data-anim-from", 0);
    const int to = attrInt(tree, id, "data-anim-to", 0);
    const int dur = attrInt(tree, id, "data-anim-dur", 1000);
    const int iter = attrInt(tree, id, "data-anim-iter", -1);
    const int delay = attrInt(tree, id, "data-anim-delay", 0);
    const Easing ease = easingFromAttr(tree.getAttribute(id, "data-anim-ease"));
    const Direction dir = directionFromAttr(tree.getAttribute(id, "data-anim-dir"));

    auto startOne = [&](Property prop, int f, int t) {
      Animation a = Animation::transition(id, prop, f, t, static_cast<uint32_t>(dur > 0 ? dur : 1),
                                          ease, static_cast<uint32_t>(delay < 0 ? 0 : delay));
      a.iterations = iter;
      a.direction = dir;
      AnimationEngine::instance().start(a, nowMs);
    };

    // Color attributes are RGB565; 8888 style values hold 8 bits per channel.
    auto color = [](int rgb565) {
#if GEA_PIXEL_FORMAT_IS_8888
      namespace pixel = gea::framework::graphics::pixel;
      return static_cast<int>(pixel::nativeStyleValueFromRgba8888(pixel::rgba8888FromRgb565(static_cast<std::uint16_t>(rgb565))));
#else
      return rgb565;
#endif
    };

    if (!std::strcmp(kind, "rotate"))
      startOne(Property::TransformRotate, from, to);
    else if (!std::strcmp(kind, "opacity"))
      startOne(Property::Opacity, from, to);
    else if (!std::strcmp(kind, "color"))
      startOne(Property::Color, color(from), color(to));
    else if (!std::strcmp(kind, "bg") || !std::strcmp(kind, "background"))
      startOne(Property::BackgroundColor, color(from), color(to));
    else if (!std::strcmp(kind, "width"))
      startOne(Property::Width, from, to);
    else if (!std::strcmp(kind, "height"))
      startOne(Property::Height, from, to);
    else if (!std::strcmp(kind, "left") || !std::strcmp(kind, "translateX"))
      startOne(Property::Left, from, to);
    else if (!std::strcmp(kind, "top") || !std::strcmp(kind, "translateY"))
      startOne(Property::Top, from, to);
    else if (!std::strcmp(kind, "scale")) {  // no transform-scale property → drive width+height
      startOne(Property::Width, from, to);
      startOne(Property::Height, from, to);
    }
  }
#endif
};

}  // namespace gea::css
