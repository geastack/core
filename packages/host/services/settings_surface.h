// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// gea::framework — watchOS-style settings surface (the "control center"). A
// full-screen overlay toggled by the SettingsToggle event from host controls.
// Shows the running app + an interactive brightness control (-/+), with a Done
// button to dismiss.
//
// Header-only singleton (links into runtime.cpp's TU without a build source-list
// change). Built directly in the live UI tree on top of the running app, the
// While it is up, runtime.cpp renders RefreshOnly
// (the app underneath is frozen, so it doesn't reconcile the overlay away) and
// routes taps here. See docs/geaos-grand-vision.md (M10).
//
// Coordinates: C++-built surfaces use PHYSICAL device pixels (410×502 here),
// NOT the CSS÷DPR space that geatsc TSX apps lay out in — taps arrive in the same
// physical pixels, so handleTap hit-tests the layout rects directly.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <display.h>
#include "geaos.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"

namespace gea::framework {

class SettingsSurface {
public:
  static SettingsSurface &instance()
  {
    static SettingsSurface s;
    return s;
  }

  bool active() const { return active_; }

  void toggle()
  {
    if (active_)
      close();
    else
      open();
  }

  // Handle a tap (physical pixels) while the surface is up. Modal — always
  // consumes the tap; returns true so the caller skips routing it to the app.
  bool handleTap(int x, int y)
  {
    if (!active_) return false;
    if (inRect(x, y, kMinusX, kBtnY, kBtnSize, kBtnSize))
      adjustBrightness(-10);
    else if (inRect(x, y, kPlusX, kBtnY, kBtnSize, kBtnSize))
      adjustBrightness(10);
    else if (inRect(x, y, kDoneX, kDoneY, kDoneW, kDoneH))
      close();
    return true;
  }

private:
  SettingsSurface() = default;

  using Document = gea::embedded::ui::Document;
  using Property = gea::embedded::ui::Property;
  using Style = gea::embedded::ui::Style;
  using ViewElement = gea::embedded::ui::ViewElement;
  using TextElement = gea::embedded::ui::TextElement;
  using NodeHandle = gea::embedded::ui::NodeHandle;

  void open()
  {
    brightness_ = clampPct(static_cast<int>(gea::platform::display::Display::brightness()));
    openedBrightness_ = brightness_;
    buildPanel();
    active_ = true;
  }

  void close()
  {
    // The brightness the user left it at is the device's from now on
    // (geaos settings, applied at boot); written once, not per step.
    if (active_ && brightness_ != openedBrightness_) {
      auto &settings = gea::geaos::deviceSettings();
      settings.setNumber(gea::geaos::settingKeys::kBrightness, brightness_);
      if (!settings.path().empty()) settings.save(gea::geaos::posixFs());
    }
    if (rootId_ >= 0) NodeHandle(rootId_).remove();
    rootId_ = -1;
    valueId_ = -1;
    active_ = false;
  }

  static int clampPct(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
  static bool inRect(int x, int y, int rx, int ry, int rw, int rh)
  {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
  }

  void adjustBrightness(int delta)
  {
    brightness_ = clampPct(brightness_ + delta);
    gea::platform::display::Display::setBrightness(brightness_);
    if (valueId_ >= 0) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d%%", brightness_);
      NodeHandle(valueId_).setText(buf);
    }
  }

  TextElement label(const char *text, int left, int top, int fontSize, int rgb565, int width, int textAlign)
  {
    TextElement t = Document::instance().createText(text);
    Style s = t.style();
    s.set(Property::Position, 1);
    s.set(Property::Left, left);
    s.set(Property::Top, top);
    if (width > 0) s.set(Property::Width, width);
    s.set(Property::FontSize, fontSize);
    s.color(rgb565);
    if (textAlign >= 0) s.set(Property::TextAlign, textAlign);
    return t;
  }

  ViewElement chip(const char *glyph, int left, int rgb565)
  {
    ViewElement v = Document::instance().createView();
    Style s = v.style();
    s.set(Property::Position, 1);
    s.set(Property::Left, left);
    s.set(Property::Top, kBtnY);
    s.set(Property::Width, kBtnSize);
    s.set(Property::Height, kBtnSize);
    s.set(Property::BorderRadiusTopLeft, 14);
    s.set(Property::BorderRadiusTopRight, 14);
    s.set(Property::BorderRadiusBottomRight, 14);
    s.set(Property::BorderRadiusBottomLeft, 14);
    s.set(Property::HasBackground, 1);
    s.backgroundColor(rgb565);
    s.set(Property::Display, 0);
    s.set(Property::JustifyContent, 1);
    s.set(Property::AlignItems, 1);
    TextElement g = Document::instance().createText(glyph);
    g.style().color(0xFFFF);
    g.style().set(Property::FontSize, 34);
    v.appendChild(g);
    return v;
  }

  void buildPanel()
  {
    Document &doc = Document::instance();
    ViewElement root = doc.createView();
    rootId_ = root.id();
    {
      Style s = root.style();
      s.set(Property::Position, 1);
      s.set(Property::Left, 0);
      s.set(Property::Top, 0);
      s.set(Property::Width, 410);
      s.set(Property::Height, 502);
      s.set(Property::HasBackground, 1);
      s.backgroundColor(0x0863);  // #0B0F19 — opaque control-center scrim
    }
    doc.body().appendChild(root);

    root.appendChild(label("Settings", 0, 40, 30, 0xFFFF, 410, 1));
    root.appendChild(label("Brightness", 40, 150, 22, 0x9CD3, -1, -1));

    root.appendChild(chip("-", kMinusX, 0x3186));
    TextElement value = label("100%", 150, 222, 30, 0xFFFF, 110, 1);
    valueId_ = value.id();
    {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "%d%%", brightness_);
      NodeHandle(valueId_).setText(buf);
    }
    root.appendChild(value);
    root.appendChild(chip("+", kPlusX, 0x3186));

    const char *appId = gea::geaos::Launcher::currentId();
    char appLine[80];
    std::snprintf(appLine, sizeof(appLine), "App   %s", appId ? appId : "?");
    root.appendChild(label(appLine, 40, 320, 20, 0x9CD3, 330, -1));

    ViewElement done = doc.createView();
    {
      Style s = done.style();
      s.set(Property::Position, 1);
      s.set(Property::Left, kDoneX);
      s.set(Property::Top, kDoneY);
      s.set(Property::Width, kDoneW);
      s.set(Property::Height, kDoneH);
      s.set(Property::BorderRadiusTopLeft, 14);
      s.set(Property::BorderRadiusTopRight, 14);
      s.set(Property::BorderRadiusBottomRight, 14);
      s.set(Property::BorderRadiusBottomLeft, 14);
      s.set(Property::HasBackground, 1);
      s.backgroundColor(0x1A8B);
      s.set(Property::Display, 0);
      s.set(Property::JustifyContent, 1);
      s.set(Property::AlignItems, 1);
    }
    TextElement doneLabel = doc.createText("Done");
    doneLabel.style().color(0xFFFF);
    doneLabel.style().set(Property::FontSize, 24);
    done.appendChild(doneLabel);
    root.appendChild(done);
  }

  // Physical-pixel layout rects (410×502). Buttons hit-tested in handleTap.
  static constexpr int kBtnY = 205;
  static constexpr int kBtnSize = 70;
  static constexpr int kMinusX = 40;
  static constexpr int kPlusX = 300;
  static constexpr int kDoneX = 125;
  static constexpr int kDoneY = 410;
  static constexpr int kDoneW = 160;
  static constexpr int kDoneH = 64;

  bool active_ = false;
  int openedBrightness_ = -1;
  int brightness_ = 80;
  int rootId_ = -1;
  int valueId_ = -1;
};

}  // namespace gea::framework
