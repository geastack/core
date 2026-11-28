// Gea embedded framework.
//
// Generated app code targets the framework UI, canvas, services, and host
// device classes directly. Platform glue stays below this layer.

#pragma once

// Full framework umbrella = host platform layer + the UI engine (document) and
// reactive (signal) layers. App/engine code includes this; host package code
// includes gea/embedded-host.h instead (no engine/reactive dependency).
#include "gea/embedded-host.h"
#include "ui/document.h"
#include "ui/child_node_list.h"
#include "ui/signal.h"
// Debug builds: attributes JSX listeners to nodes for DevTools.
#include "ui/debugger_listeners.h"

// Flat, typed DOM entry points shared by generated C++ translation units.
namespace gea_ir {
inline gea::embedded::ui::NodeHandle domNextSibling(gea::embedded::ui::NodeHandle node) {
  return node.nextSiblingHandle();
}

inline gea::embedded::ui::Style domStyle(gea::embedded::ui::NodeHandle node) {
  return node.style();
}

inline void domStyleSetProperty(gea::embedded::ui::Style style, const std::string &property, const std::string &value) {
  style.setProperty(property, value);
}

inline bool domStyleRemoveProperty(gea::embedded::ui::Style style, const std::string &property) {
  return style.removeProperty(property);
}
}
