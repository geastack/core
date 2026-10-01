// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <display.h>
#include "backends.h"
#include "display_orientation.h"
#include "services/frame_scheduler.h"
#include "ui/document.h"
#include "ui/style.h"

namespace gea::host {

namespace detail {

template <typename Config>
double displayFlushRows(const Config &config) {
  if constexpr (requires { config.rows; }) {
    return static_cast<double>(config.rows);
  } else if constexpr (requires { config.record_get_literal("rows"); }) {
    auto rows = config.record_get_literal("rows");
    return rows.is_nullish() ? 0.0 : static_cast<double>(rows);
  } else {
    return 0.0;
  }
}

template <typename Config>
double displayFlushDepth(const Config &config) {
  if constexpr (requires { config.depth; }) {
    return static_cast<double>(config.depth);
  } else if constexpr (requires { config.record_get_literal("depth"); }) {
    auto depth = config.record_get_literal("depth");
    return depth.is_nullish() ? 0.0 : static_cast<double>(depth);
  } else {
    return 0.0;
  }
}

// Optional-number field for the e-paper refresh config: -1 = absent/unchanged.
template <typename Field>
double epaperConfigNumber(const Field &field) {
  if constexpr (requires { field.has_value(); *field; }) {
    return field.has_value() ? epaperConfigNumber(*field) : -1.0;
  } else if constexpr (requires { field.is_nullish(); }) {
    return field.is_nullish() ? -1.0 : static_cast<double>(field);
  } else {
    return static_cast<double>(field);
  }
}

// Optional LUT-bytes field: returns true when the field is present (an empty
// array means "restore the vendor default") and fills `out` with its bytes.
template <typename Field>
bool epaperConfigLut(const Field &field, std::vector<std::uint8_t> &out) {
  if constexpr (requires { field.has_value(); *field; }) {
    if (!field.has_value()) return false;
    return epaperConfigLut(*field, out);
  } else if constexpr (requires { field.is_nullish(); }) {
    if (field.is_nullish()) return false;
    if constexpr (requires { field.size(); field[0]; }) {
      for (std::size_t i = 0; i < static_cast<std::size_t>(field.size()); ++i)
        out.push_back(static_cast<std::uint8_t>(static_cast<double>(field[i])));
    }
    return true;
  } else if constexpr (requires { field.size(); field.begin(); field.end(); }) {
    for (const auto &item : field) out.push_back(static_cast<std::uint8_t>(static_cast<double>(item)));
    return true;
  } else {
    return false;
  }
}

}  // namespace detail

struct DisplayFacade {
  // Logical viewport dimensions in pixels — equal to whatever was passed
  // to Application::init by the platform target. Apps that need to size
  // content to the actual panel (bouncing balls' bounce extent, layout
  // breakpoints, etc.) can read these without hardcoding per-device sizes.
  double width() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return static_cast<double>(gea::platform::display::kWidth);
#else
    int w = gea::embedded::ui::Document::preferredMountWidth();
    return static_cast<double>(w > 0 ? w : gea::platform::display::kWidth);
#endif
  }
  double height() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return static_cast<double>(gea::platform::display::kHeight);
#else
    int h = gea::embedded::ui::Document::preferredMountHeight();
    return static_cast<double>(h > 0 ? h : gea::platform::display::kHeight);
#endif
  }
  double nativeWidth() const { return gea::framework::display::DisplayBackend::nativeWidth(); }
  double nativeHeight() const { return gea::framework::display::DisplayBackend::nativeHeight(); }

  gea::embedded::ui::CanvasRenderingContext2D ctx() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    // No document exists in the inferred canvas-only runtime.
    return gea::embedded::ui::CanvasRenderingContext2D(-1);
#else
    auto &document = gea::embedded::ui::Document::instance();
    // Idempotent getter: if a previous ctx() call already mounted a canvas
    // document, hand back a context on that node. Apps and libraries (gea3d's
    // Renderer::render, per-frame HUD helpers) re-read Display.ctx freely —
    // direct-canvas builds make that a trivial getter, and the tree path must
    // match. Rebuilding here instead would clear the document MID-FRAME while
    // the style mount batch is open: the fresh canvas node gets no layout, so
    // ensureCanvas() sees a 0x0 node and every subsequent draw is dropped.
    //
    // The flag says "the app presents the display itself, so the document
    // render can stand down". That is only true where the mounted canvas can
    // actually BE display-backed. On web it never can:
    // `canUseDisplayFramebuffer` (ui/tree_nodes.cpp) returns false under
    // __EMSCRIPTEN__ by design, because the browser framebuffer IS the surface
    // the document render paints, so a canvas presenting into it would be
    // clobbered by the same frame's document render. Web therefore uses the
    // owned-surface + document-blit path -- and setting the flag here disabled
    // exactly the blit that path depends on (Document::refreshMountedIfDirty
    // returns early on it), leaving every tree-build Display.ctx app with a
    // permanently black panel. Mark it only where display-backing is reachable.
#ifndef __EMSCRIPTEN__
#define GEA_DISPLAY_CTX_MARK_DIRECT() gea::embedded::ui::Document::markDirectCanvasContextUsed()
#else
#define GEA_DISPLAY_CTX_MARK_DIRECT() ((void)0)
#endif
    {
      const int mounted = document.mountedCanvasNode();
      if (mounted >= 0) {
        GEA_DISPLAY_CTX_MARK_DIRECT();
        return gea::embedded::ui::CanvasRenderingContext2D(mounted);
      }
    }
    document.clear();
    auto canvas = document.createCanvas();
    const int w = static_cast<int>(width());
    const int h = static_cast<int>(height());
    canvas.style().width(w);
    canvas.style().height(h);
    document.mount(canvas, w, h);
    GEA_DISPLAY_CTX_MARK_DIRECT();
#undef GEA_DISPLAY_CTX_MARK_DIRECT
    return canvas.getContext2D();
#endif
  }

  double getBrightness() const { return gea::framework::display::DisplayBackend::brightness(); }
  void setBrightness(double brightness) const { gea::framework::display::DisplayBackend::setBrightness(brightness); }
  std::string orientation() const { return gea::framework::display::DisplayBackend::orientation(); }
  std::string getOrientation() const { return orientation(); }
  void setOrientation(const std::string &orientation) const { gea::framework::display::DisplayBackend::setOrientation(orientation); }
  std::vector<std::string> supportedOrientations() const {
    return gea::framework::display::DisplayBackend::supportedOrientations();
  }
  std::vector<std::string> getSupportedOrientations() const { return supportedOrientations(); }
  template <typename Options>
  void setSupportedOrientations(const Options &orientations) const {
    gea::framework::display::detail::DisplayOrientationState::setSupportedOrientationsFrom(orientations);
  }
  bool autoRotate() const { return gea::framework::display::DisplayBackend::autoRotate(); }
  bool getAutoRotate() const { return autoRotate(); }
  void setAutoRotate(bool enabled) const { gea::framework::display::DisplayBackend::setAutoRotate(enabled); }
  void setVSync(bool on) const { gea::framework::display::DisplayBackend::setVSync(on); }
  void setTextRasterCache(bool on) const { gea::framework::display::DisplayBackend::setTextRasterCache(on); }
  void setTextSolidBackdrop(int rrggbb) const { gea::framework::display::DisplayBackend::setTextSolidBackdrop(rrggbb); }
  void invalidate() const { gea::framework::display::DisplayBackend::invalidate(); }
  std::string pixelFormat() const { return gea::framework::display::DisplayBackend::pixelFormat(); }
  std::string getPixelFormat() const { return pixelFormat(); }
  void setPixelFormat(const std::string &format) const { gea::framework::display::DisplayBackend::setPixelFormat(format); }
  std::string panelPixelFormat() const { return gea::framework::display::DisplayBackend::panelPixelFormat(); }
  std::string getPanelPixelFormat() const { return panelPixelFormat(); }
  std::vector<std::string> supportedPixelFormats() const {
    return gea::framework::display::DisplayBackend::supportedPixelFormats();
  }
  std::vector<std::string> getSupportedPixelFormats() const { return supportedPixelFormats(); }
  double getDevicePixelRatio() const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    return 1.0;
#else
    return gea::embedded::ui::devicePixelRatio();
#endif
  }
  void setDevicePixelRatio(double devicePixelRatio) const {
#if defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) && GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
    (void)devicePixelRatio;
#else
    gea::embedded::ui::setDevicePixelRatio(devicePixelRatio);
#endif
  }
  double getFrameIntervalMs() const { return gea::framework::services::FrameScheduler::frameIntervalMs(); }
  void setFrameIntervalMs(double intervalMs) const {
    gea::framework::services::FrameScheduler::setFrameIntervalMs(static_cast<int>(intervalMs));
  }
  double getFrameRate() const { return gea::framework::services::FrameScheduler::frameRate(); }
  void setFrameRate(double fps) const { gea::framework::services::FrameScheduler::setFrameRate(fps); }
	  void setAA(double samples) const { gea::framework::display::DisplayBackend::setAA(samples); }
	  template <typename Config>
	  void setFlushConfig(const Config &config) const {
	    gea::framework::display::DisplayBackend::setFlushConfig(
	      detail::displayFlushRows(config),
	      detail::displayFlushDepth(config));
	  }
	  template <typename Config>
	  void setMemoryConfig(const Config &) const {}
		  // E-paper refresh policy + custom waveform LUTs (no-op on non-e-paper
  // boards). Omitted fields keep their current value; an empty LUT array
  // restores the board's vendor waveform.
  template <typename Config>
  void setEpaperRefreshConfig(const Config &config) const {
    double fullEvery = -1.0;
    double hardCap = -1.0;
    double fastWindowMs = -1.0;
    std::vector<std::uint8_t> partialLut;
    std::vector<std::uint8_t> fastLut;
    bool hasPartialLut = false;
    bool hasFastLut = false;
    if constexpr (requires { config.fullRefreshEveryPartials; })
      fullEvery = detail::epaperConfigNumber(config.fullRefreshEveryPartials);
    else if constexpr (requires { config.record_get_literal("fullRefreshEveryPartials"); })
      fullEvery = detail::epaperConfigNumber(config.record_get_literal("fullRefreshEveryPartials"));
    if constexpr (requires { config.fullRefreshHardCapPartials; })
      hardCap = detail::epaperConfigNumber(config.fullRefreshHardCapPartials);
    else if constexpr (requires { config.record_get_literal("fullRefreshHardCapPartials"); })
      hardCap = detail::epaperConfigNumber(config.record_get_literal("fullRefreshHardCapPartials"));
    if constexpr (requires { config.fastStreakWindowMs; })
      fastWindowMs = detail::epaperConfigNumber(config.fastStreakWindowMs);
    else if constexpr (requires { config.record_get_literal("fastStreakWindowMs"); })
      fastWindowMs = detail::epaperConfigNumber(config.record_get_literal("fastStreakWindowMs"));
    if constexpr (requires { config.partialLut; })
      hasPartialLut = detail::epaperConfigLut(config.partialLut, partialLut);
    else if constexpr (requires { config.record_get_literal("partialLut"); })
      hasPartialLut = detail::epaperConfigLut(config.record_get_literal("partialLut"), partialLut);
    if constexpr (requires { config.fastLut; })
      hasFastLut = detail::epaperConfigLut(config.fastLut, fastLut);
    else if constexpr (requires { config.record_get_literal("fastLut"); })
      hasFastLut = detail::epaperConfigLut(config.record_get_literal("fastLut"), fastLut);
    gea::framework::display::DisplayBackend::setEpaperRefreshConfig(
      fullEvery, hardCap, fastWindowMs, partialLut, hasPartialLut, fastLut, hasFastLut);
    // Optional grayscale toggle: -1 = absent (leave unchanged), 0/1 = off/on.
    double grayscale = -1.0;
    if constexpr (requires { config.grayscale; })
      grayscale = detail::epaperConfigNumber(config.grayscale);
    else if constexpr (requires { config.record_get_literal("grayscale"); })
      grayscale = detail::epaperConfigNumber(config.record_get_literal("grayscale"));
    if (grayscale >= 0.0)
      gea::framework::display::DisplayBackend::setEpaperGrayscale(grayscale != 0.0);
    // Optional cover-promotion toggle: -1 = absent (leave unchanged), 0/1 =
    // whether a full-screen-covering partial is promoted to a flashing full
    // refresh. A reading app whose page turns cover the screen sets 0 and
    // relies on fullRefreshEveryPartials to clear ghosting.
    double fullOnCover = -1.0;
    if constexpr (requires { config.fullOnCover; })
      fullOnCover = detail::epaperConfigNumber(config.fullOnCover);
    else if constexpr (requires { config.record_get_literal("fullOnCover"); })
      fullOnCover = detail::epaperConfigNumber(config.record_get_literal("fullOnCover"));
    if (fullOnCover >= 0.0)
      gea::framework::display::DisplayBackend::setEpaperFullOnCover(fullOnCover != 0.0);
  }
  void epaperFullRefresh() const { gea::framework::display::DisplayBackend::epaperFullRefresh(); }
};

inline constexpr DisplayFacade Display{};

}  // namespace gea::host
