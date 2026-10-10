#pragma once

#include "pixel.h"

#include <cstdint>
#include <utility>

#if GEA_EMBEDDED_SINGLE_THREAD_IMAGE_STORE
namespace gea::framework::graphics {
struct ImageStoreMutex {
	void lock() noexcept {}
	void unlock() noexcept {}
};
}  // namespace gea::framework::graphics
#else
#include <mutex>
#endif

namespace gea::framework::graphics {

#if !GEA_EMBEDDED_SINGLE_THREAD_IMAGE_STORE
using ImageStoreMutex = std::mutex;
#endif

// Decoded-image slot pool. Slots are lazy (pixel buffers allocated only on
// decode), so a larger pool costs almost nothing until filled. 96 lets a
// tile-grid app (maps) keep ~4 screens of decoded tiles resident in PSRAM for
// instant pan-back, on top of its persistent SD cache. ~128 KiB/tile (RGB565
// 256x256) when full → ~12 MiB; the P4 has ample PSRAM.
#ifndef GEA_EMBEDDED_MAX_IMAGES
#define GEA_EMBEDDED_MAX_IMAGES 96
#endif
inline constexpr int kImageMax = GEA_EMBEDDED_MAX_IMAGES;

enum class ImageFit {
	Fill = 0,
	Contain = 1,
	Cover = 2,
	None = 3,
	ScaleDown = 4,
};

enum class ImageFormat {
	Unknown = 0,
	Jpeg = 1,
	Png = 2,
	Gif = 3,
};

struct ImageSlot {
	int width = 0;
	int height = 0;
	int frameCount = 0;
	ImageFormat format = ImageFormat::Unknown;
	int loopCount = 0;
	int currentFrame = 0;
	bool playing = false;
	int elapsedMs = 0;
	int currentDelayMs = 0;

	// Decoded pixels in this target's native framebuffer format (RGB565 on
	// esp32/geaos, RGBA8888 on iOS) — set at compile time by pixel::native_t.
	// Full colour on RGBA8888 targets (the decoder's 8-bit channels are kept, not
	// quantized to 565), so native UIImageView presents stills at full colour.
	pixel::native_t *pixels = nullptr;
	std::uint8_t *alpha = nullptr;
	bool ownsPixels = false;
	// Registered by the app (registerBuffer): the app writes these pixels in
	// place between frames, so the same pointer does not mean the same image.
	bool mutablePixels = false;

	void *gifDecoder = nullptr;
	std::uint8_t *gifData = nullptr;
	int gifDataLength = 0;

	// Deferred decode (decodeDeferred): the encoded bytes live at a stable
	// address (embedded flash assets); only the header was probed for dims.
	// The first pixel access (record time) materializes the decode.
	const std::uint8_t *deferredData = nullptr;
	int deferredLength = 0;
	// stbi_info reports the encoded channel count while probing dimensions.
	// Preserve whether the source has no alpha channel so materialization can
	// discard the synthetic all-255 alpha plane and use the opaque blit path.
	bool deferredOpaque = false;
};

class ImageStore {
public:
	static ImageStore &instance();

	int decode(const std::uint8_t *data, int length, int preferredSlot = -1);
	int decodeOpaque(const std::uint8_t *data, int length, int preferredSlot = -1);
	// Deferred decode for encoded bytes at a STABLE address (embedded flash
	// assets): probes only the header for dimensions (layout needs them at
	// mount) and decodes on first pixel access (record time). Cuts boot-time
	// decoding to the icons actually visible. GIFs decode eagerly.
	int decodeDeferred(const std::uint8_t *data, int length, int preferredSlot = -1);
	// Materialize ONE pending deferred decode (idle-time pre-decode). Returns
	// false when none are pending. Lets the frame loop drain deferred slots
	// during idle frames so a later first-draw (e.g. switching weather cities)
	// doesn't bunch every decode into a single visible hitch.
	bool materializeOneDeferred();
	// Register an externally-owned native-format pixel buffer. `takeOwnership`
	// transfers ownership to the store (freed on dispose) — used by the iOS
	// camera capture, which hands over a freshly-allocated RGBA8888 still.
	int registerBuffer(pixel::native_t *pixels, int width, int height, int preferredSlot = -1, bool takeOwnership = false);

	// True when `pixels` point into a registerBuffer() image, whose content the
	// app rewrites in place. The present diff never treats such an image as
	// unchanged from the previous frame.
	bool pixelsMayChange(const pixel::native_t *pixels) const;

	int width(int id) const;
	int height(int id) const;
	int frameCount(int id) const;
	bool isAnimated(int id) const;
	ImageFormat format(int id) const;
	void setPlaying(int id, bool playing);
	bool playing(int id) const;
	void seek(int id, int frame);
	int currentFrame(int id) const;
	bool advance(int id, int deltaMs);
	const pixel::native_t *framePixels(int id, int frame) const;
	const pixel::native_t *currentPixels(int id) const;
	const std::uint8_t *currentAlpha(int id) const;
	// Pins keep decoded pixels alive for nodes and retained render commands.
	bool retain(int id);
	void release(int id);
	int idForPixels(const pixel::native_t *pixels) const;
	void dispose(int id);
	void disposeAll();
	ImageFormat detectFormat(const std::uint8_t *data, int length) const;

private:
	ImageSlot *slot(int id);
	const ImageSlot *slot(int id) const;
	int findFreeSlot(int preferredSlot) const;
	int decodeWithHint(const std::uint8_t *data, int length, int preferredSlot, bool opaqueHint);
	// Materializes a deferred decode in place (no-op when already decoded).
	void materializeDeferred(int id) const;
	int decodeGif(ImageSlot &image, const std::uint8_t *data, int length);
	int decodeStatic(ImageSlot &image, const std::uint8_t *data, int length, bool opaqueHint);
	// Native JPEG backends, selected automatically when provided by the target.
	int decodeJpegEsp(ImageSlot &image, const std::uint8_t *data, int length);
	int decodeJpegTjpgd(ImageSlot &image, const std::uint8_t *data, int length);
	int decodeNextGifFrame(ImageSlot &image);
	// dispose() body without taking slotMutex_ (callers hold it).
	void disposeLocked(int id);

 public:
	// Rotate a SQUARE opaque image 90deg so its pixels match the portrait
	// panel orientation (panel-native landscape rendering: tiles rotate once
	// at decode instead of the display rotating every frame). No-op for
	// non-square or alpha-carrying images.
	bool rotate90(int id);

 private:

	ImageSlot images_[kImageMax]{};
	bool used_[kImageMax]{};
	unsigned references_[kImageMax]{};
	bool retired_[kImageMax]{};
	// Guards slot ALLOCATION and DISPOSAL only (reservation-style): the async
	// tile loader decodes on the second core while the frame task blits and
	// evicts. A decode reserves its slot under the lock, then fills it
	// unlocked — the slot id isn't visible to the app until the loader
	// delivers it, so nothing reads a half-decoded slot. Pixel READS of
	// already-delivered slots stay lock-free.
	mutable ImageStoreMutex slotMutex_;
};

// Copyable ownership for retained commands; raw pixel pointers alone do not
// keep a decoded slot alive.
class ImageReference {
public:
	ImageReference() = default;
	explicit ImageReference(int id) : id_(ImageStore::instance().retain(id) ? id : -1) {}
	ImageReference(const ImageReference &other) : ImageReference(other.id_) {}
	ImageReference(ImageReference &&other) noexcept : id_(other.id_) { other.id_ = -1; }
	~ImageReference() { ImageStore::instance().release(id_); }
	ImageReference &operator=(const ImageReference &other) {
		if (this != &other) {
			ImageReference next(other);
			std::swap(id_, next.id_);
		}
		return *this;
	}
	ImageReference &operator=(ImageReference &&other) noexcept {
		if (this != &other) {
			ImageStore::instance().release(id_);
			id_ = other.id_;
			other.id_ = -1;
		}
		return *this;
	}
private:
	int id_ = -1;
};

}  // namespace gea::framework::graphics
