// SPDX-License-Identifier: Apache-2.0
#include "camera.h"
#include "host/backends.h"
#include "host/worker.h"

#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace {
bool cameraOpen = false;
bool cameraOpenFails = false;
int cameraCloses = 0;
struct Frames : gea::platform::camera::CameraFrameProvider {
  std::string takeFrameDataUrl() override { return "data:image/jpeg;base64,/9j/2Q=="; }
} frames;
}

// The host camera bridge is real; only the physical device is substituted.
// Its preview provider's vtable requires the complete platform surface even
// though the ownership tests never render a preview or capture a still.
namespace gea::platform::camera {
bool Camera::isAvailable() { return true; }
bool Camera::hasPermission() { return true; }
bool Camera::requestPermission() { return true; }
bool Camera::open(const std::string &, int, int) {
  if (cameraOpenFails) return false;
  cameraOpen = true;
  setFrameProvider(&frames);
  return true;
}
void Camera::close() { ++cameraCloses; cameraOpen = false; setFrameProvider(nullptr); }
bool Camera::isOpen() { return cameraOpen; }
int Camera::width() { return 128; }
int Camera::height() { return 240; }
int Camera::orientation() { return 0; }
Facing Camera::currentFacing() { return Facing::External; }
std::string Camera::currentFacingString() { return "external"; }
int Camera::deviceCount() { return 1; }
DeviceInfo Camera::deviceAt(int) { return {"mock-left-dvp", Facing::External, 128, 240}; }
void Camera::drawPreview(int, int, int, int) {}
int Camera::previewMode() { return 0; }
bool Camera::fillPreview(gea::framework::graphics::pixel::native_t *, int, int, int, bool) { return false; }
void Camera::positionPreviewLayer(int, int, int, int) {}
void Camera::hidePreviewLayer() {}
void Camera::presentNativeOverlay() {}
int Camera::capture(bool) { return -1; }
bool Camera::startRecording(const std::string &, double) { return false; }
double Camera::stopRecording() { return -1; }
bool Camera::isRecording() { return false; }
void Camera::setFlash(const std::string &) {}
void Camera::setZoom(double) {}
void Camera::setMirror(bool) {}
void Camera::setExposure(const std::string &, double, double, double) {}
void Camera::setWhiteBalance(const std::string &, double, double) {}
void Camera::setFocus(const std::string &, double, double) {}
void Camera::setTorch(const std::string &, double) {}
}

int main() {
  using Camera = gea::framework::camera::CameraBackend;
  using gea::host::workers::Context;
  using gea::host::workers::ContextScope;
  assert(!Camera::isOpen() && Camera::captureFrame().empty());

  auto owner = Context::create("camera-owner");
  {
    ContextScope scope(owner);
    assert(Camera::open("external", 0, 0));
  }
  assert(Camera::isOpen() && !Camera::captureFrame().empty());
  owner->finish();
  assert(!Camera::isOpen() && Camera::captureFrame().empty() && cameraCloses == 1);
  owner->finish();
  assert(cameraCloses == 1);

  auto oldOwner = Context::create("old-camera-owner");
  auto successor = Context::create("camera-successor");
  {
    ContextScope scope(oldOwner);
    assert(Camera::open("external", 0, 0));
  }
  {
    ContextScope scope(successor);
    assert(Camera::open("external", 0, 0));
  }
  oldOwner->finish();
  assert(Camera::isOpen() && cameraCloses == 1);
  successor->finish();
  assert(!Camera::isOpen() && cameraCloses == 2);

  auto explicitOwner = Context::create("explicit-camera-close");
  {
    ContextScope scope(explicitOwner);
    assert(Camera::open("external", 0, 0));
    Camera::close();
  }
  assert(cameraCloses == 3);
  explicitOwner->finish();
  assert(!Camera::isOpen() && cameraCloses == 3);

  auto previousWorker = Context::create("previous-camera-worker");
  {
    ContextScope scope(previousWorker);
    assert(Camera::open("external", 0, 0));
  }
  assert(Context::current()->isMain());
  assert(Camera::open("external", 0, 0));
  previousWorker->finish();
  assert(Camera::isOpen() && cameraCloses == 3);
  Camera::close();
  assert(!Camera::isOpen() && cameraCloses == 4);

  auto stopped = Context::create("closed-camera-context");
  stopped->stop();
  bool rejected = false;
  {
    ContextScope scope(stopped);
    try { Camera::open("external", 0, 0); }
    catch (const std::runtime_error &) { rejected = true; }
  }
  assert(rejected && !Camera::isOpen() && Camera::captureFrame().empty() && cameraCloses == 5);
  stopped->finish();
  assert(cameraCloses == 5);

  auto failed = Context::create("missing-camera-module");
  cameraOpenFails = true;
  {
    ContextScope scope(failed);
    assert(!Camera::open("external", 0, 0));
  }
  cameraOpenFails = false;
  failed->finish();
  assert(!Camera::isOpen() && cameraCloses == 5);
  std::puts("worker camera: owner shutdown, successor generation, main ownership, explicit close and failed registration OK");
}
