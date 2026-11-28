// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <display.h>
#include <event_listener_options.h>
#include <ui/document.h>
#include <ui/tree_events.h>
#include <cstddef>
#include <string>
#include <type_traits>

namespace gea {
template <typename Signature> struct CallableObject;
template <typename T> class Optional;
struct Undefined;
}

namespace gea::host {

[[noreturn]] inline void throwRuntimeError(const char *kind, const std::string &message);

// A value token for the one engine-owned document. Its members dispatch
// through Document::instance(), and copies denote the same document.
struct DocumentFacade {
  explicit constexpr operator bool() const noexcept { return true; }
  constexpr bool operator==(const DocumentFacade &) const = default;
  constexpr bool valid() const noexcept { return true; }
  std::shared_ptr<void> propertyStorage(const std::shared_ptr<void> &initial = {}) const {
    return gea::embedded::ui::Document::instance().propertyStorage(initial);
  }
};

inline constexpr DocumentFacade documentValue{};

using gea::embedded::ui::getComputedStyle;

inline auto getComputedStyle(gea::embedded::ui::NodeHandle node, std::nullptr_t)
{
  return getComputedStyle(node);
}

inline auto getComputedStyle(gea::embedded::ui::NodeHandle node, const gea::Undefined &)
{
  return getComputedStyle(node);
}

template <typename T>
auto getComputedStyle(gea::embedded::ui::NodeHandle node, const gea::Optional<T> &pseudo)
{
  return pseudo.has_value() ? getComputedStyle(node, std::string_view(*pseudo)) : getComputedStyle(node);
}

inline gea::embedded::ui::NodeHandle documentCreateElementNS(const std::string &uri, const std::string &name)
{
  return gea::embedded::ui::Document::instance().createElementNS(uri, name);
}

inline gea::embedded::ui::NodeHandle documentCreateElementNS(std::nullptr_t, const std::string &name)
{
  return documentCreateElementNS(std::string{}, name);
}

template <typename T>
gea::embedded::ui::NodeHandle documentCreateElementNS(const gea::Optional<T> &uri, const std::string &name)
{
  return uri.has_value() ? documentCreateElementNS(*uri, name) : documentCreateElementNS(nullptr, name);
}

struct SelectionFacade {
  gea::embedded::ui::DocumentSelection state() const {
    return gea::embedded::ui::Document::instance().selection();
  }
  void selectAllChildren(const gea::embedded::ui::NodeHandle &node) const {
    gea::embedded::ui::Document::instance().selectAllChildren(node);
  }
  void removeAllRanges() const {
    gea::embedded::ui::Document::instance().clearSelection();
  }
};

struct WindowFacade {
  SelectionFacade getSelection() const { return {}; }
  template <typename Result, typename Argument>
  void addEventListener(const std::string &type,
                        const gea::CallableObject<Result(Argument)> &handler,
                        bool capture = false, bool once = false) const {
    if (gea::embedded::ui::windowEventListenersRetiring()) return;
    static_assert(std::is_constructible_v<std::decay_t<Argument>,
                  const gea::framework::events::PointerEvent &>,
                  "native window listener needs an installed event carrier");
    const auto held = handler;
    if (!gea::embedded::ui::addWindowEventListener(type.c_str(),
          handler.functionObjectIdentity().get(),
          gea::framework::events::explicitEventCallback([](auto& held, gea::framework::events::PointerEvent &event) {
            held.call(static_cast<std::decay_t<Argument>>(event));
          }, held), capture, once))
      throwRuntimeError("TypeError", "Native window could not register event listener: " + type);
  }
  template <typename Result>
  void addEventListener(const std::string &type,
                        const gea::CallableObject<Result()> &handler,
                        bool capture = false, bool once = false) const {
    if (gea::embedded::ui::windowEventListenersRetiring()) return;
    const auto held = handler;
    if (!gea::embedded::ui::addWindowEventListener(type.c_str(),
          handler.functionObjectIdentity().get(),
          gea::framework::events::explicitEventCallback([](auto& held, gea::framework::events::PointerEvent &) { held.call(); }, held),
          capture, once))
      throwRuntimeError("TypeError", "Native window could not register event listener: " + type);
  }
  template <typename Handler>
    requires (!requires(const Handler &handler) { handler.functionObjectIdentity(); })
  void addEventListener(const std::string &, const Handler &, bool = false, bool = false) const {
    throwRuntimeError("TypeError", "Native window listeners must be functions");
  }

  template <typename Handler>
  void removeEventListener(const std::string &type, const Handler &handler,
                           bool capture = false) const {
    if constexpr (requires { handler.functionObjectIdentity(); })
      gea::embedded::ui::removeWindowEventListener(type.c_str(), handler.functionObjectIdentity().get(), capture);
  }
  template <typename Handler, typename Options>
    requires (!std::is_same_v<std::decay_t<Options>,bool>)
  void addEventListener(const std::string &type, const Handler &handler, const Options &options) const {
    if constexpr (requires { handler.functionObjectIdentity(); }) {
      const auto parsed = gea::framework::events::listenerOptions(options);
      addEventListener(type, handler, parsed.capture, parsed.once);
    } else {
      addEventListener(type, handler);
    }
  }
  template <typename Handler, typename Options>
    requires (!std::is_same_v<std::decay_t<Options>,bool>)
  void removeEventListener(const std::string &type, const Handler &handler, const Options &options) const {
    if constexpr (requires { handler.functionObjectIdentity(); }) {
      const auto parsed = gea::framework::events::listenerOptions(options);
      removeEventListener(type, handler, parsed.capture);
    }
  }
  double innerWidth() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return static_cast<double>(gea::platform::display::kWidth);
#else
    int w = gea::embedded::ui::Document::preferredMountWidth();
    return static_cast<double>(w > 0 ? w : gea::platform::display::kWidth);
#endif
  }

  double innerHeight() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return static_cast<double>(gea::platform::display::kHeight);
#else
    int h = gea::embedded::ui::Document::preferredMountHeight();
    return static_cast<double>(h > 0 ? h : gea::platform::display::kHeight);
#endif
  }

  double scrollX() const { return gea::embedded::ui::Document::instance().viewportScrollX(); }
  double scrollY() const { return gea::embedded::ui::Document::instance().viewportScrollY(); }
  void scrollTo(double x, double y) const { gea::embedded::ui::Document::instance().scrollViewportTo(x, y); }
};

inline constexpr WindowFacade window{};

}  // namespace gea::host
