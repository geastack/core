#pragma once

#include "pixel.h"

#include <cstdint>
#include <atomic>
#include <string>

namespace gea::platform::camera {

enum class Facing {
	Back = 0,
	Front = 1,
	External = 2,
};

struct DeviceInfo {
	std::string id;
	Facing facing = Facing::Back;
	int sensorWidth = 0;
	int sensorHeight = 0;
};

// Optional encoded-frame producer. The device owns capture/encoding on its
// worker; reading a frame never waits for a sensor or codec on the UI thread.
class CameraFrameProvider {
public:
	virtual ~CameraFrameProvider() = default;
	virtual std::string takeFrameDataUrl() = 0;
};

// Static facade — one active camera at a time on the device. The platform
// layer maintains the live preview buffer and exposes a small surface that
// the host bridge forwards into.
class Camera {
public:
	static bool isAvailable();
	static bool hasPermission();
	static bool requestPermission();

	// facingHint: "front"|"back"|"external"|<device id>
	// preferredWidth/Height: 0 = platform default. The platform may pick the
	// closest supported size; query width()/height() after open() to read back.
	static bool open(const std::string &facingHint, int preferredWidth, int preferredHeight);
	static void close();
	static bool isOpen();

	static void setFrameProvider(CameraFrameProvider *provider) { frameProvider().store(provider); }
	static std::string captureFrame() {
		auto *provider = frameProvider().load();
		return provider ? provider->takeFrameDataUrl() : std::string();
	}

	static int width();
	static int height();
	static int orientation();
	static Facing currentFacing();
	static std::string currentFacingString();

	static int deviceCount();
	static DeviceInfo deviceAt(int index);

	// Render the latest preview frame into the framebuffer at (x, y). When
	// destW/destH are >0 the frame is scaled; otherwise it's drawn at its
	// native capture size.
	static void drawPreview(int x, int y, int destW, int destH);

	// How the platform composites the live preview for a <camera> leaf node:
	//   0 = Framebuffer  — the runtime owns an RGB565 buffer the backend fills
	//                      (fillPreview) and blits through the normal display
	//                      list, so the preview obeys CSS clip / z-order.
	//   1 = NativeOverlay — the backend owns a native preview surface (e.g. an
	//                      iOS AVCaptureVideoPreviewLayer) which the runtime
	//                      positions over the node's computed rect.
	static int previewMode();

	// Framebuffer mode: scale + convert the latest frame into the caller's
	// native-pixel buffer of size dstWidth x dstHeight (pixel::native_t is RGB565
	// on 16-bit targets, RGBA8888 on full-colour targets like iOS). `fit` is
	// 0=cover, 1=contain, 2=fill; `mirror` flips horizontally. Returns true if a
	// frame was written.
	static bool fillPreview(gea::framework::graphics::pixel::native_t *dst, int dstWidth, int dstHeight, int fit, bool mirror);

	// NativeOverlay mode: position / hide the native preview surface. The rect
	// is in framebuffer pixels (the <camera> node's computed layout box).
	static void positionPreviewLayer(int x, int y, int width, int height);
	static void hidePreviewLayer();

	// NativeOverlay mode: present one fresh frame into the positioned rect (called
	// once per frame by the render tick). On the ESP32-P4 this PPA-scales the
	// frame straight into the display framebuffer and flushes just that rect.
	static void presentNativeOverlay();

	// Captures a still and registers it with the embedded ImageStore so it
	// can be drawn via image.draw(id, ...). Returns -1 on failure.
	static int capture(bool mirror);

	// Video recording. startRecording opens a sink at `path` (platform decides
	// the container/codec — MJPEG on esp32-p4). stopRecording finalizes and
	// returns the duration in milliseconds, or -1 on failure.
	static bool startRecording(const std::string &path, double fps);
	static double stopRecording();
	static bool isRecording();

	static void setFlash(const std::string &mode);
	static void setZoom(double factor);
	static void setMirror(bool mirror);

	// Capture controls. `mode` is a lowercase keyword the backend maps to its
	// native control set:
	//   exposure:      "auto" | "continuous" | "locked" | "manual"
	//   whiteBalance:  "auto" | "continuous" | "locked"
	//   focus:         "auto" | "continuous" | "locked"
	//   torch:         "off"  | "on"        | "auto"
	// Unused numeric args are 0 (meaning "platform default / leave as is").
	static void setExposure(const std::string &mode, double bias, double iso, double durationMs);
	static void setWhiteBalance(const std::string &mode, double temperatureK, double tint);
	static void setFocus(const std::string &mode, double pointX, double pointY);
	static void setTorch(const std::string &mode, double level);

private:
	static std::atomic<CameraFrameProvider *> &frameProvider() {
		static std::atomic<CameraFrameProvider *> provider{nullptr};
		return provider;
	}
};

}  // namespace gea::platform::camera
