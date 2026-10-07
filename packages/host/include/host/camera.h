// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "backends.h"
#include "image.h"

#include <string>
#include <utility>

namespace gea::host {

// Typed still-capture result. `Camera.capturePhoto()` (and the runtime's
// `capture()` alias) resolve synchronously on-device, so the promise carries
// the photo struct natively and `then(f)` invokes `f` immediately — the app's
// `Camera.capturePhoto().then(photo => photo.imageId …)` lowers to native
// member reads with no gea_cpp_value boxing.
struct CameraPhoto {
  double imageId = -1.0;
  double width = 0.0;
  double height = 0.0;
  double orientation = 0.0;
  void dispose() const { image.dispose(imageId); }
};

struct CameraPhotoPromise {
  CameraPhoto photo;
  template <typename F>
  auto then(F &&onResolved) const {
    return std::forward<F>(onResolved)(photo);
  }
};

// Typed recording-stop result; same synchronous-promise shape as capture.
struct CameraClip {
  double durationMs = -1.0;
  std::string path;
};

struct CameraClipPromise {
  CameraClip clip;
  template <typename F>
  auto then(F &&onResolved) const {
    return std::forward<F>(onResolved)(clip);
  }
};

// Path of the in-flight recording so `stopRecording()` can return it on the
// clip (mirrors the TS runtime shim's module-level `cameraRecordingPath`).
inline std::string &cameraRecordingPath() {
  static std::string path;
  return path;
}

inline CameraPhoto makePhoto(double imageId) {
  return CameraPhoto{
      imageId,
      gea::framework::camera::CameraBackend::width(),
      gea::framework::camera::CameraBackend::height(),
      gea::framework::camera::CameraBackend::orientation(),
  };
}

class CameraValueProperty {
public:
  using Getter = double (*)();

  constexpr CameraValueProperty() = default;
  explicit constexpr CameraValueProperty(Getter getter) : getter_(getter) {}

  double operator()() const { return getter_ ? getter_() : 0.0; }
  operator double() const { return (*this)(); }

private:
  Getter getter_ = nullptr;
};

class CameraStringProperty {
public:
  using Getter = std::string (*)();

  constexpr CameraStringProperty() = default;
  explicit constexpr CameraStringProperty(Getter getter) : getter_(getter) {}

  std::string operator()() const { return getter_ ? getter_() : std::string(); }
  operator std::string() const { return (*this)(); }

private:
  Getter getter_ = nullptr;
};

class CameraFacade {
public:
  constexpr CameraFacade()
      : width(gea::framework::camera::CameraBackend::width),
        height(gea::framework::camera::CameraBackend::height),
        orientation(gea::framework::camera::CameraBackend::orientation),
        deviceCount(gea::framework::camera::CameraBackend::deviceCount),
        facing(gea::framework::camera::CameraBackend::facing) {}

  bool isAvailable() const { return gea::framework::camera::CameraBackend::isAvailable(); }
  bool hasPermission() const { return gea::framework::camera::CameraBackend::hasPermission(); }
  bool requestPermission() const { return gea::framework::camera::CameraBackend::requestPermission(); }

  bool open(const std::string &facing, double width, double height) const {
    return gea::framework::camera::CameraBackend::open(facing, width, height);
  }
  bool open(const std::string &facing) const { return open(facing, 0.0, 0.0); }
  bool open() const { return open(std::string("back"), 0.0, 0.0); }

  // Single-arg form accepting an options record. Fields are
  // resolved by name on the structured record at the call site
  // (geatsc lowers the options literal to gea_cpp_value).
  template <typename Options>
  bool open(const Options &options) const {
    return gea::framework::camera::CameraBackend::openWithOptions(options);
  }

  void close() const { gea::framework::camera::CameraBackend::close(); }
  bool isOpen() const { return gea::framework::camera::CameraBackend::isOpen(); }

  // Draw the latest preview frame into the destination region (framebuffer
  // pixels). When destWidth/destHeight are 0 or omitted, the frame is drawn
  // at its native size.
  void draw(double x, double y) const {
    gea::framework::camera::CameraBackend::draw(x, y, 0.0, 0.0);
  }
  void draw(double x, double y, double destWidth, double destHeight) const {
    gea::framework::camera::CameraBackend::draw(x, y, destWidth, destHeight);
  }

  // Returns image id of a still capture, suitable to pass to image.draw().
  double capture() const { return gea::framework::camera::CameraBackend::capture(); }
  double captureMirrored() const { return gea::framework::camera::CameraBackend::captureMirrored(); }
  std::string captureFrame() const { return gea::framework::camera::CameraBackend::captureFrame(); }

  // Single-arg form accepting an options record. Reads `mirror` to choose
  // the path; ignores unknown fields. Lets call sites use the same shape
  // as `open({...})`.
  template <typename Options>
  double capture(const Options &options) const {
    return gea::framework::camera::CameraBackend::captureWithOptions(options);
  }

  // Typed still capture: the TS `Camera.capture()`/`capturePhoto()` calls
  // lower here. Resolves synchronously with the photo's id + dimensions.
  CameraPhotoPromise capturePhoto() const {
    return {makePhoto(gea::framework::camera::CameraBackend::capture())};
  }
  template <typename Options>
  CameraPhotoPromise capturePhoto(const Options &options) const {
    return {makePhoto(gea::framework::camera::CameraBackend::captureWithOptions(options))};
  }

  // Video recording. Returns true when recording started; stopRecording
  // returns the clip duration in ms (-1 on failure).
  bool startRecording(const std::string &path, double fps) const {
    cameraRecordingPath() = path;
    return gea::framework::camera::CameraBackend::startRecording(path, fps);
  }
  bool startRecording(const std::string &path) const { return startRecording(path, 0.0); }
  // Options-record form (mirrors open({...})): reads `path` and `fps`. A typed
  // options struct (geatsc interface struct / plugin anonymous struct) reads
  // the member directly; a dynamic gea_cpp_value record reads through
  // record_get_literal via optionString.
  template <typename Options>
  bool startRecording(const Options &options) const {
    if constexpr (requires { options.path; })
      cameraRecordingPath() = static_cast<std::string>(options.path);
    else
      cameraRecordingPath() = gea::framework::camera::CameraBackend::optionString(options, "path", "");
    return gea::framework::camera::CameraBackend::startRecordingWithOptions(options);
  }
  double stopRecording() const { return gea::framework::camera::CameraBackend::stopRecording(); }
  // Typed stop: the TS `Camera.stopRecording()` call lowers here. Resolves
  // synchronously with the clip duration and the path recording started with.
  CameraClipPromise stopRecordingClip() const {
    return {CameraClip{gea::framework::camera::CameraBackend::stopRecording(), cameraRecordingPath()}};
  }
  bool isRecording() const { return gea::framework::camera::CameraBackend::isRecording(); }

  void setFlash(const std::string &mode) const { gea::framework::camera::CameraBackend::setFlash(mode); }
  void setZoom(double factor) const { gea::framework::camera::CameraBackend::setZoom(factor); }
  void setMirror(bool mirror) const { gea::framework::camera::CameraBackend::setMirror(mirror); }

  // Capture controls. Numeric args default to 0 ("leave as is"); see camera.h
  // for the accepted `mode` keywords. Each control also has an options-record
  // form (mirrors open({...})) so call sites can pass the TypeScript shape.
  void setExposure(const std::string &mode, double bias, double iso, double durationMs) const {
    gea::framework::camera::CameraBackend::setExposure(mode, bias, iso, durationMs);
  }
  void setExposure(const std::string &mode) const {
    gea::framework::camera::CameraBackend::setExposure(mode, 0.0, 0.0, 0.0);
  }
  template <typename Options>
  void setExposure(const Options &options) const {
    gea::framework::camera::CameraBackend::setExposureWithOptions(options);
  }
  void setWhiteBalance(const std::string &mode, double temperatureK, double tint) const {
    gea::framework::camera::CameraBackend::setWhiteBalance(mode, temperatureK, tint);
  }
  void setWhiteBalance(const std::string &mode) const {
    gea::framework::camera::CameraBackend::setWhiteBalance(mode, 0.0, 0.0);
  }
  template <typename Options>
  void setWhiteBalance(const Options &options) const {
    gea::framework::camera::CameraBackend::setWhiteBalanceWithOptions(options);
  }
  void setFocus(const std::string &mode, double pointX, double pointY) const {
    gea::framework::camera::CameraBackend::setFocus(mode, pointX, pointY);
  }
  void setFocus(const std::string &mode) const {
    gea::framework::camera::CameraBackend::setFocus(mode, 0.0, 0.0);
  }
  template <typename Options>
  void setFocus(const Options &options) const {
    gea::framework::camera::CameraBackend::setFocusWithOptions(options);
  }
  void setTorch(const std::string &mode, double level) const {
    gea::framework::camera::CameraBackend::setTorch(mode, level);
  }
  void setTorch(const std::string &mode) const {
    gea::framework::camera::CameraBackend::setTorch(mode, 0.0);
  }

  std::string deviceIdAt(double index) const { return gea::framework::camera::CameraBackend::deviceIdAt(index); }
  std::string deviceFacingAt(double index) const { return gea::framework::camera::CameraBackend::deviceFacingAt(index); }

  CameraValueProperty width;
  CameraValueProperty height;
  CameraValueProperty orientation;
  CameraValueProperty deviceCount;
  CameraStringProperty facing;
};

inline constexpr CameraFacade Camera{};

}  // namespace gea::host
