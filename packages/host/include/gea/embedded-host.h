// SPDX-License-Identifier: Apache-2.0
// Gea host platform umbrella — device + host services ONLY.
//
// Deliberately excludes the UI engine (ui/document.h) and the reactive layer
// (ui/signal.h). Host package .cpp include THIS, so @geastack/host carries no
// transitive dependency on engine/reactive. The full framework umbrella
// (gea/embedded.h) is this + ui/document.h + ui/signal.h, for engine/app code.

#pragma once

#include "display.h"
#include "host/backends.h"
#include "host/fetch.h"
#include "geolocation.h"
#include "host/websocket.h"
#include "host/http.h"
#include "host/media.h"
#include "host/rtc.h"
#include "host/touch.h"
#include "host/apps.h"
#include "host/image.h"
#include "host/tile_loader.h"
#include "host/memory.h"
#include "host/input.h"
#include "host/gpio.h"
#include "host/clock.h"
#include "host/profiler.h"
#include "host/performance.h"
#include "host/storage.h"
#include "host/battery.h"
#include "host/notify.h"
#include "host/device_control.h"
#include "input.h"
#include "host/audio.h"
#include "host/display.h"
#include "host/navigator.h"
#include "host/camera.h"
#include "host/timers.h"
#include "host/window.h"
