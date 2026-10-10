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
#include "ui/signal.h"
// Debug builds: attributes JSX listeners to nodes for DevTools.
#include "ui/debugger_listeners.h"
