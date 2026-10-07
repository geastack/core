// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "geolocation.h"

namespace gea::framework::sensors {

class AccelerometerBackend {
public:
  static void init();
  static void close();
  static void calibrateBias();
  static double tiltX();
  static double tiltY();
  static double accelerationX();
  static double accelerationY();
  static double accelerationZ();
  static double gyroscopeX();
  static double gyroscopeY();
  static double gyroscopeZ();
};

} // namespace gea::framework::sensors

namespace gea::framework::camera {

class CameraBackend {
public:
  static bool isAvailable();
  static bool hasPermission();
  static bool requestPermission();

  // facing: "front" | "back" | <device id from deviceIdAt>
  // preferredWidth/Height: 0 means "platform default"
  static bool open(const std::string &facing, double preferredWidth,
                   double preferredHeight);
  static void close();
  static bool isOpen();

  static double width();
  static double height();
  static double orientation();
  static std::string facing();

  static double deviceCount();
  static std::string deviceIdAt(double index);
  static std::string deviceFacingAt(double index);

  // Blit the latest preview frame into the framebuffer.
  // destWidth/destHeight = 0 means draw at native size.
  static void draw(double x, double y, double destWidth, double destHeight);

  // Capture a still image; returns the ImageStore id or -1 on failure.
  static double capture();
  static double captureMirrored();
  static std::string captureFrame();

  // Video recording (esp32-p4: MJPEG). startRecording returns true on success;
  // stopRecording returns the clip duration in ms, or -1 on failure.
  static bool startRecording(const std::string &path, double fps);
  static double stopRecording();
  static bool isRecording();

  static void setFlash(const std::string &mode);
  static void setZoom(double factor);
  static void setMirror(bool mirror);

  // Capture controls forwarded to the platform backend. See camera.h for the
  // accepted `mode` keywords; numeric args default to 0 ("leave as is").
  static void setExposure(const std::string &mode, double bias, double iso,
                          double durationMs);
  static void setWhiteBalance(const std::string &mode, double temperatureK,
                              double tint);
  static void setFocus(const std::string &mode, double pointX, double pointY);
  static void setTorch(const std::string &mode, double level);

  // Options-record open form. Header-only template so the generated
  // cpp can pass any record-shaped value (geatsc emits gea_cpp_value or
  // a struct with named fields). Reads facing/width/height when present.
  template <typename Options>
  static bool openWithOptions(const Options &options) {
    std::string facing;
    double width = 0.0;
    double height = 0.0;
    if constexpr (requires { options.facing; }) {
      facing = static_cast<std::string>(options.facing);
    } else if constexpr (requires { options.record_get_literal("facing"); }) {
      auto value = options.record_get_literal("facing");
      if (!value.is_nullish())
        facing = static_cast<std::string>(value);
    }
    if (facing.empty())
      facing = "back";
    if constexpr (requires { options.width; }) {
      width = static_cast<double>(options.width);
    } else if constexpr (requires { options.record_get_literal("width"); }) {
      auto value = options.record_get_literal("width");
      if (!value.is_nullish())
        width = static_cast<double>(value);
    }
    if constexpr (requires { options.height; }) {
      height = static_cast<double>(options.height);
    } else if constexpr (requires { options.record_get_literal("height"); }) {
      auto value = options.record_get_literal("height");
      if (!value.is_nullish())
        height = static_cast<double>(value);
    }
    return open(facing, width, height);
  }

  // Options-record capture form. Reads `mirror` to decide which path.
  template <typename Options>
  static double captureWithOptions(const Options &options) {
    bool mirror = false;
    if constexpr (requires { options.mirror; }) {
      mirror = static_cast<bool>(options.mirror);
    } else if constexpr (requires { options.record_get_literal("mirror"); }) {
      auto value = options.record_get_literal("mirror");
      if (!value.is_nullish())
        mirror = static_cast<bool>(value);
    }
    return mirror ? captureMirrored() : capture();
  }

  // Field readers shared by the options-record control forms below. The
  // dynamic (gea_cpp_value record) arm reads through record_get_literal; typed
  // options — geatsc `__gea_type_*` interface structs and the plugin's
  // anonymous options structs — are read field-by-field in each control form
  // below, since member selection needs the field name at compile time.
  template <typename Options>
  static std::string optionString(const Options &options, const char *field,
                                  const std::string &fallback) {
    if constexpr (requires { options.record_get_literal(field); }) {
      auto value = options.record_get_literal(field);
      if (!value.is_nullish())
        return static_cast<std::string>(value);
    }
    return fallback;
  }
  template <typename Options>
  static double optionNumber(const Options &options, const char *field,
                             double fallback) {
    if constexpr (requires { options.record_get_literal(field); }) {
      auto value = options.record_get_literal(field);
      if (!value.is_nullish())
        return static_cast<double>(value);
    }
    return fallback;
  }

// Per-field typed-first read: prefer a real struct member (guarded by the
// geatsc `__gea_has_<field>` presence flag when the struct carries one; the
// plugin's anonymous options structs only contain fields the literal wrote,
// so bare presence of the member means present), falling back to the dynamic
// record arm, then the default. Expression-shaped so the readers stay
// declarative; undefined behind this header's include boundary.
#define GEA_CAMERA_OPTION_FIELD(options, field, fallback, castT)               \
  ([&]() {                                                                     \
    if constexpr (requires { (options).field; }) {                             \
      bool __gea_present = true;                                               \
      if constexpr (requires { (options).__gea_has_##field; })                 \
        __gea_present = (options).__gea_has_##field;                           \
      if (__gea_present)                                                       \
        return static_cast<castT>((options).field);                            \
    }                                                                          \
    return static_cast<castT>(                                                 \
        optionsFieldFallback_##castT((options), #field, fallback));            \
  }())

  template <typename Options>
  static std::string
  optionsFieldFallback_OptionStringT(const Options &options, const char *field,
                                     const std::string &fallback) {
    return optionString(options, field, fallback);
  }
  template <typename Options>
  static double optionsFieldFallback_OptionNumberT(const Options &options,
                                                   const char *field,
                                                   double fallback) {
    return optionNumber(options, field, fallback);
  }
  using OptionStringT = std::string;
  using OptionNumberT = double;

  // Options-record control forms — mirror open/capture so call sites can pass
  // the same `{ ... }` record shape the TypeScript API uses, whether it lowers
  // to a typed struct (preferred) or a dynamic gea_cpp_value record (legacy).
  template <typename Options>
  static void setExposureWithOptions(const Options &options) {
    setExposure(
        GEA_CAMERA_OPTION_FIELD(options, mode, std::string("continuous"),
                                OptionStringT),
        GEA_CAMERA_OPTION_FIELD(options, bias, 0.0, OptionNumberT),
        GEA_CAMERA_OPTION_FIELD(options, iso, 0.0, OptionNumberT),
        GEA_CAMERA_OPTION_FIELD(options, durationMs, 0.0, OptionNumberT));
  }
  template <typename Options>
  static void setWhiteBalanceWithOptions(const Options &options) {
    setWhiteBalance(
        GEA_CAMERA_OPTION_FIELD(options, mode, std::string("auto"),
                                OptionStringT),
        GEA_CAMERA_OPTION_FIELD(options, temperature, 0.0, OptionNumberT),
        GEA_CAMERA_OPTION_FIELD(options, tint, 0.0, OptionNumberT));
  }
  template <typename Options>
  static void setFocusWithOptions(const Options &options) {
    double px = 0.0;
    double py = 0.0;
    if constexpr (requires { options.point; }) {
      auto &&point = options.point;
      px = GEA_CAMERA_OPTION_FIELD(point, x, 0.0, OptionNumberT);
      py = GEA_CAMERA_OPTION_FIELD(point, y, 0.0, OptionNumberT);
    } else if constexpr (requires { options.record_get_literal("point"); }) {
      auto point = options.record_get_literal("point");
      if (!point.is_nullish()) {
        px = optionNumber(point, "x", 0.0);
        py = optionNumber(point, "y", 0.0);
      }
    }
    setFocus(GEA_CAMERA_OPTION_FIELD(options, mode, std::string("continuous"),
                                     OptionStringT),
             px, py);
  }
  template <typename Options>
  static bool startRecordingWithOptions(const Options &options) {
    return startRecording(
        GEA_CAMERA_OPTION_FIELD(options, path, std::string(""), OptionStringT),
        GEA_CAMERA_OPTION_FIELD(options, fps, 0.0, OptionNumberT));
  }
#undef GEA_CAMERA_OPTION_FIELD
};

// Registers the platform camera backend with the runtime's board-independent
// preview seam (gea::embedded::ui::CameraSurface). Targets that link a camera
// backend call this during init (e.g. from app_main), the same way Wifi
// registers its driver. Defined in host/camera.cpp.
void registerCameraSurface();

} // namespace gea::framework::camera

namespace gea::framework::network {

class WifiBackend {
public:
  static bool enabled();
  static void setEnabled(bool enabled);
  static bool connected();
  static double rssi();
  static std::string ssid();
  static std::string ip();
  static std::string mac();
  static void configure(const std::string &ssid, const std::string &password);
  static bool startAccessPoint(const std::string &ssid,
                               const std::string &password);
  static void stopAccessPoint();
  static std::string accessPointMac();
  static bool startCaptivePortal();
  static void stopCaptivePortal();
  static bool waitForConnection(double timeoutMs);
  static void startScan();
  static bool scanning();
  static double scanCount();
  static std::string scanSsidAt(double index);
  static double scanRssiAt(double index);
  static bool scanSecuredAt(double index);
};

} // namespace gea::framework::network

namespace gea::framework::geolocation {

class GeolocationBackend {
public:
  static bool hasFix();
  static GeolocationPosition currentPosition();
  static double latitude();
  static double longitude();
  static double accuracy();
};

} // namespace gea::framework::geolocation

namespace gea::framework::bluetooth {

class HidBackend {
public:
  static void init(const std::string &device_name, double appearance,
                   const std::string &mac_address);
  static bool enabled();
  static void setEnabled(bool enabled);
  static void startAdvertising();
  static void stopAdvertising();
  static bool connected();
  static bool bound();
  static double batteryLevel();
  static std::string mac();
  static std::string deviceName();
  static void keyTap(double hid_code);
  static void keyDown(double modifier, double hid_code);
  static void keyUp();
  static void mouseMove(double dx, double dy, double buttons, double wheel);
  static void mouseClick(double button);
  static void midiEnable();
  static bool midiBound();
  static void midiSend(const std::vector<double> &bytes);
  static void midiStartScan();
  static void midiStopScan();
  static bool midiScanning();
  static double midiScanCount();
  static std::string midiScanNameAt(double index);
  static void midiConnect(double index);
  static void midiDisconnect();
  static void hidHostStartScan();
  static void hidHostStopScan();
  static bool hidHostScanning();
  static double hidHostScanCount();
  static std::string hidHostScanNameAt(double index);
  static void hidHostConnect(double index);
  static void hidHostDisconnect();
  static bool hidHostBound();
  static double hidHostReportCount();
  static double hidHostReportIdAt(double index);
  static double hidHostReportLenAt(double index);
  static double hidHostReportByteAt(double index, double byteIndex);
  static void hidHostClearReports();
  static double connectionCount();
  static double connectionKindAt(double index);
  static std::string connectionNameAt(double index);
  static void configSetDocument(const std::vector<double> &bytes);
  static double configPendingLength();
  static double configPendingByteAt(double index);
  static void configConsumePending();
  static bool configPairing();
  static std::string configPairCode();
  static void configDismissPairing();
  static void configPushActivity(double index);
};

} // namespace gea::framework::bluetooth

namespace gea::framework::audio {

class AudioBackend {
public:
  static double volume();
  static void setVolume(double volume);
};

} // namespace gea::framework::audio

namespace gea::framework::display {

class DisplayBackend {
public:
  static double brightness();
  static void setBrightness(double brightness);
  static void setAA(double samples);
  static void setFlushConfig(double rows, double depth);
  // E-paper refresh policy + custom waveforms (weak-hooked: no-op on boards
  // without an e-paper panel). Negative numbers = leave unchanged. LUTs must
  // be the panel's full waveform table (159 bytes on SSD1681-class panels);
  // hasLut=true with an empty vector restores the board's vendor default.
  static void setEpaperRefreshConfig(
      double fullRefreshEveryPartials, double fullRefreshHardCapPartials,
      double fastStreakWindowMs, const std::vector<std::uint8_t> &partialLut,
      bool hasPartialLut, const std::vector<std::uint8_t> &fastLut,
      bool hasFastLut);
  static void epaperFullRefresh();
  // Toggle 4-level grayscale rendering on e-paper boards that support it
  // (weak-hooked no-op elsewhere). Gray mode trades the fast partial waveform
  // for a slower full-refresh that drives 4 reflectance levels.
  static void setEpaperGrayscale(bool enabled);
  static void setEpaperFullOnCover(bool enabled);
  static double nativeWidth();
  static double nativeHeight();
  static std::string orientation();
  static void setOrientation(const std::string &orientation);
  static std::vector<std::string> supportedOrientations();
  static void
  setSupportedOrientations(const std::vector<std::string> &orientations);
  static void setSupportedOrientations(const std::string &orientation);
  static bool autoRotate();
  static void setAutoRotate(bool enabled);
  // Opt-in tearing sync (TE/VBlank). No-op on targets without a TE line.
  static void setVSync(bool on);
  // Opt-in large-static-text sprite cache (default off). Forwards to the shared
  // software rasterizer (Canvas); no per-platform behavior.
  static void setTextRasterCache(bool on);
  // Opt-in pre-packed glyph stamping over a declared solid backdrop (packed
  // 4bpp targets; no-op elsewhere). 0xRRGGBB; negative disables.
  static void setTextSolidBackdrop(int rrggbb);
  // Mark the next present as a full-screen damage-all (skip the dirty-rect diff
  // and the persistent previous-frame copy). No-op on targets without a present
  // diff.
  static void invalidate();
  static std::string pixelFormat();
  static void setPixelFormat(const std::string &format);
  static std::string panelPixelFormat();
  static std::vector<std::string> supportedPixelFormats();
  static void updateAutoRotationFromAccelerometer();
  static void updateAutoRotation(double acceleration_x, double acceleration_y,
                                 double acceleration_z);
};

} // namespace gea::framework::display

namespace gea::framework::memory {

class MemoryBackend {
public:
  static double internalFree();
  static double internalLargestFreeBlock();
  static double internalMinimumFree();
  static double psramFree();
  static double currentTaskStackHighWaterMark();
  static double geaMainStackBytes();
  static double geaInitStackBytes();
  static double appFrameStackWords();
  static double appFrameStackBytes();
  static double displayFlushConfiguredRows();
  static double displayFlushConfiguredDepth();
  static double displayFlushBufferMaxBytes();
  static double displayFlushRows();
  static double displayFlushDepth();
  static double displayFlushBufferBytes();
  static double allocationSramCount();
  static double allocationPsramCount();
  static double allocationSramBytes();
  static double allocationPsramBytes();
  static double allocationSramPeakBytes();
  static double allocationPsramPeakBytes();
};

} // namespace gea::framework::memory

namespace gea::framework::input {

class InputBackend {
public:
  // Reads and clears the one-shot back-button-pressed flag. Polled by the
  // launcher's rAF loop; when true and Settings is visible, the launcher
  // closes Settings. Set from the platform's launcher-button task (BOOT
  // short press on ESP32) and from an application's `_settings_toggle()`
  // entry stub (fired by `Application::toggleSettings()` for `SettingsToggle`
  // events posted via `AppManager::queueSettingsToggle()`).
  static bool consumeBackButton();
};

} // namespace gea::framework::input

namespace gea::framework::gpio {

// Digital pins, for the wiring a board exposes that no other facade covers: an
// LED, a relay, a button that is not the launcher button. A board with no
// display reaches the outside world through these and nothing else.
//
// Every call answers "did this board do it", not "was the call well-formed":
// an out-of-range pin, a pin the chip cannot drive, or a target with no GPIO at
// all all report false rather than aborting, so an app written for one board
// degrades on another instead of crashing on it.
class GpioBackend {
public:
  static bool configureOutput(double pin);
  static bool configureInput(double pin, bool pull_up);
  static bool write(double pin, bool level);
  static bool read(double pin);
};

// A WS2812/NeoPixel strand on one pin — the "onboard LED" of most modern ESP32
// modules, which is addressable rather than a plain diode.
//
// `set` is the default: one call attaches the pin if needed, writes the colour
// and clocks it out, which is the whole of what a board's single onboard LED
// needs. `off` is `set` with black. The attach/setPixel/show trio underneath is
// for a real strand, where one transmit per frame beats one per pixel; show
// returns once the strand has latched, so a true means the LED is showing what
// was asked for rather than that the write was queued.
class AddressableLedBackend {
public:
  static bool set(double pin, double r, double g, double b);
  static bool off(double pin);
  static bool attach(double pin, double count);
  static bool setPixel(double index, double r, double g, double b);
  static bool show();
  static void detach();
};

} // namespace gea::framework::gpio
