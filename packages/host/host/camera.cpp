// SPDX-License-Identifier: Apache-2.0
#include "camera.h"
#include "gea/embedded-host.h"
#include "ui/camera_element.h"
#include "host/worker.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace gea::framework::camera {

namespace {
std::mutex cameraOwnershipMutex;
std::uint64_t cameraGeneration = 0;
}

bool CameraBackend::isAvailable() {
	return gea::platform::camera::Camera::isAvailable();
}

bool CameraBackend::hasPermission() {
	return gea::platform::camera::Camera::hasPermission();
}

bool CameraBackend::requestPermission() {
	return gea::platform::camera::Camera::requestPermission();
}

bool CameraBackend::open(const std::string &facing, double preferredWidth, double preferredHeight) {
	std::lock_guard guard(cameraOwnershipMutex);
	if (!gea::platform::camera::Camera::open(facing, static_cast<int>(preferredWidth), static_cast<int>(preferredHeight))) return false;
	const auto generation = ++cameraGeneration;
	const auto owner = gea::host::workers::Context::current();
	if (!owner->isMain()) {
		try {
			owner->addCleanup([generation] {
				std::lock_guard cleanupGuard(cameraOwnershipMutex);
				// An old realm must not shut down a camera reopened by its successor.
				if (cameraGeneration != generation) return;
				++cameraGeneration;
				gea::platform::camera::Camera::close();
			});
		} catch (...) {
			++cameraGeneration;
			gea::platform::camera::Camera::close();
			throw;
		}
	}
	return true;
}

void CameraBackend::close() {
	std::lock_guard guard(cameraOwnershipMutex);
	++cameraGeneration;
	gea::platform::camera::Camera::close();
}

bool CameraBackend::isOpen() {
	return gea::platform::camera::Camera::isOpen();
}

double CameraBackend::width() {
	return static_cast<double>(gea::platform::camera::Camera::width());
}

double CameraBackend::height() {
	return static_cast<double>(gea::platform::camera::Camera::height());
}

double CameraBackend::orientation() {
	return static_cast<double>(gea::platform::camera::Camera::orientation());
}

std::string CameraBackend::facing() {
	return gea::platform::camera::Camera::currentFacingString();
}

double CameraBackend::deviceCount() {
	return static_cast<double>(gea::platform::camera::Camera::deviceCount());
}

std::string CameraBackend::deviceIdAt(double index) {
	return gea::platform::camera::Camera::deviceAt(static_cast<int>(index)).id;
}

std::string CameraBackend::deviceFacingAt(double index) {
	const auto info = gea::platform::camera::Camera::deviceAt(static_cast<int>(index));
	switch (info.facing) {
		case gea::platform::camera::Facing::Front: return "front";
		case gea::platform::camera::Facing::External: return "external";
		case gea::platform::camera::Facing::Back:
		default: return "back";
	}
}

void CameraBackend::draw(double x, double y, double destWidth, double destHeight) {
	gea::platform::camera::Camera::drawPreview(static_cast<int>(x), static_cast<int>(y),
	                                            static_cast<int>(destWidth), static_cast<int>(destHeight));
}

double CameraBackend::capture() {
	return static_cast<double>(gea::platform::camera::Camera::capture(false));
}

double CameraBackend::captureMirrored() {
	return static_cast<double>(gea::platform::camera::Camera::capture(true));
}

std::string CameraBackend::captureFrame() {
	return gea::platform::camera::Camera::captureFrame();
}

void CameraBackend::setFlash(const std::string &mode) {
	gea::platform::camera::Camera::setFlash(mode);
}

void CameraBackend::setZoom(double factor) {
	gea::platform::camera::Camera::setZoom(factor);
}

void CameraBackend::setMirror(bool mirror) {
	gea::platform::camera::Camera::setMirror(mirror);
}

bool CameraBackend::startRecording(const std::string &path, double fps) {
	return gea::platform::camera::Camera::startRecording(path, fps);
}

double CameraBackend::stopRecording() {
	return gea::platform::camera::Camera::stopRecording();
}

bool CameraBackend::isRecording() {
	return gea::platform::camera::Camera::isRecording();
}

void CameraBackend::setExposure(const std::string &mode, double bias, double iso, double durationMs) {
	gea::platform::camera::Camera::setExposure(mode, bias, iso, durationMs);
}

void CameraBackend::setWhiteBalance(const std::string &mode, double temperatureK, double tint) {
	gea::platform::camera::Camera::setWhiteBalance(mode, temperatureK, tint);
}

void CameraBackend::setFocus(const std::string &mode, double pointX, double pointY) {
	gea::platform::camera::Camera::setFocus(mode, pointX, pointY);
}

void CameraBackend::setTorch(const std::string &mode, double level) {
	gea::platform::camera::Camera::setTorch(mode, level);
}

// Bridge the runtime's board-independent camera preview seam
// (gea::embedded::ui::CameraSurface) to the platform camera backend. Targets
// with a camera call registerCameraSurface() during init (mirroring
// gea::targets::esp32::wifi::registerDriver()); camera-less targets never link
// this translation unit, so a <camera> element renders nothing there.
namespace {

class PlatformCameraSurfaceProvider final : public gea::embedded::ui::CameraSurfaceProvider {
public:
	// JSX attributes can arrive as the literal strings "undefined"/"null" when an
	// app passes an absent prop straight through; treat those as empty.
	static std::string sanitize(const std::string &value) {
		if (value == "undefined" || value == "null") return std::string();
		return value;
	}

	bool ensureOpen(const std::string &facing,
	                const std::string &device,
	                int preferredWidth,
	                int preferredHeight) override {
		const std::string dev = sanitize(device);
		const std::string face = sanitize(facing);
		std::string hint = !dev.empty() ? dev : (!face.empty() ? face : std::string("back"));
		if (gea::platform::camera::Camera::isOpen() && hint == lastHint_) return true;
		if (gea::platform::camera::Camera::isOpen() && hint != lastHint_) gea::platform::camera::Camera::close();
		const bool ok = gea::platform::camera::Camera::open(hint, preferredWidth, preferredHeight);
		if (ok) lastHint_ = hint;
		return ok;
	}

	bool isStreaming() override { return gea::platform::camera::Camera::isOpen(); }

	gea::embedded::ui::CameraPreviewMode previewMode() override {
		return gea::platform::camera::Camera::previewMode() == 1
		           ? gea::embedded::ui::CameraPreviewMode::NativeOverlay
		           : gea::embedded::ui::CameraPreviewMode::Framebuffer;
	}

	bool fillPreview(gea::framework::graphics::pixel::native_t *dst,
	                 int dstWidth,
	                 int dstHeight,
	                 gea::embedded::ui::CameraPreviewFit fit,
	                 bool mirror) override {
		return gea::platform::camera::Camera::fillPreview(dst, dstWidth, dstHeight, static_cast<int>(fit), mirror);
	}

	void positionPreviewLayer(int x, int y, int width, int height) override {
		gea::platform::camera::Camera::positionPreviewLayer(x, y, width, height);
	}

	void hidePreviewLayer() override { gea::platform::camera::Camera::hidePreviewLayer(); }

	void presentNativeOverlay() override { gea::platform::camera::Camera::presentNativeOverlay(); }

	void release(int) override {
		gea::platform::camera::Camera::hidePreviewLayer();
		gea::platform::camera::Camera::close();
		lastHint_.clear();
	}

private:
	std::string lastHint_;
};

PlatformCameraSurfaceProvider gPlatformCameraSurfaceProvider;

}  // namespace

void registerCameraSurface() {
	gea::embedded::ui::CameraSurface::setProvider(&gPlatformCameraSurfaceProvider);
}

}  // namespace gea::framework::camera
