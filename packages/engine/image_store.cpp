// SPDX-License-Identifier: Apache-2.0
#include "image.h"
#include "memory.h"
#include "pixel.h"
#include "gea_perf_config.h"

#include <cstring>
#include <limits>

#if GEA_EMBEDDED_PERF && defined(ESP_PLATFORM)
#include "esp_timer.h"
#include <atomic>
#include <cstdio>
#endif

// Baseline JPEGs can stream from the S3 ROM decoder directly into native
// pixels. Keep unsupported JPEG variants on stb's general-purpose path.
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if !defined(GEA_EMBEDDED_ROM_TJPGD) && defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#define GEA_EMBEDDED_ROM_TJPGD 1
#endif

// A linked esp_new_jpeg component supplies the S3 SIMD decoder. It writes
// straight to native RGB565; other pixel formats keep the ROM/stb paths.
#if !defined(GEA_EMBEDDED_ESP_JPEG) && defined(ESP_PLATFORM) && \
    GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565 && __has_include("esp_jpeg_dec.h")
#define GEA_EMBEDDED_ESP_JPEG 1
#endif
#if defined(GEA_EMBEDDED_ESP_JPEG) && GEA_EMBEDDED_ESP_JPEG && GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
#include "esp_jpeg_dec.h"
#endif

// PNGs can stream through the S3 ROM's inflater straight into native pixels,
// so a decode needs the result plus ~45 KB rather than stb's whole inflated
// image and an RGBA copy (3.4 MB for an 800x480 picture).
#if !defined(GEA_EMBEDDED_ROM_PNG_STREAM) && defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3) && \
    __has_include("miniz.h")
#define GEA_EMBEDDED_ROM_PNG_STREAM 1
#endif
#if defined(GEA_EMBEDDED_ROM_PNG_STREAM) && GEA_EMBEDDED_ROM_PNG_STREAM
#include "miniz.h"
#endif

// Route stb_image's allocations through the framework allocator (PSRAM-first
// on embedded targets). Plain malloc would land on the tiny SRAM newlib heap
// on the rp2350 — where pico_malloc PANICS on failure — so decoding a large
// PNG killed the boot. Mirrors rasterized_font.cpp's STBTT_malloc routing.
namespace {
void *gea_stbi_malloc(std::size_t size)
{
	return gea::framework::memory::Allocator::allocatePreferSpiram(size, alignof(std::max_align_t));
}

void gea_stbi_free(void *ptr)
{
	if (ptr) gea::framework::memory::Allocator::free(ptr);
}

void *gea_stbi_realloc_sized(void *ptr, std::size_t oldSize, std::size_t newSize)
{
	void *next = gea_stbi_malloc(newSize);
	if (!next) return nullptr;
	if (ptr) {
		std::memcpy(next, ptr, oldSize < newSize ? oldSize : newSize);
		gea_stbi_free(ptr);
	}
	return next;
}
}  // namespace

#define STBI_MALLOC(sz) gea_stbi_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) gea_stbi_realloc_sized((p), (oldsz), (newsz))
#define STBI_FREE(p) gea_stbi_free(p)

#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_BMP
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#if defined(GEA_EMBEDDED_ROM_TJPGD) && GEA_EMBEDDED_ROM_TJPGD
// The ROM emits one RGB888 MCU at a time, not a full-frame RGB(A) buffer.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp32s3/rom/tjpgd.h"
#else
#include "esp32/rom/tjpgd.h"
#endif
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#endif

#include "AnimatedGIF.h"

#if GEA_EMBEDDED_SINGLE_THREAD_IMAGE_STORE
namespace gea::framework::graphics {
class ImageStoreLock {
public:
	explicit ImageStoreLock(ImageStoreMutex &) {}
};
}  // namespace gea::framework::graphics
#else
#include <mutex>
namespace gea::framework::graphics {
using ImageStoreLock = std::lock_guard<ImageStoreMutex>;
}  // namespace gea::framework::graphics
#endif

extern "C" {
int GIF_openRAM(GIFIMAGE *pGIF, uint8_t *pData, int iDataSize, GIF_DRAW_CALLBACK *pfnDraw);
void GIF_close(GIFIMAGE *pGIF);
void GIF_begin(GIFIMAGE *pGIF, int iEndian, unsigned char ucPaletteType);
void GIF_reset(GIFIMAGE *pGIF);
int GIF_playFrame(GIFIMAGE *pGIF, int *delayMilliseconds);
int GIF_getCanvasWidth(GIFIMAGE *pGIF);
int GIF_getCanvasHeight(GIFIMAGE *pGIF);
}

namespace gea::framework::graphics {

namespace {

class GifDrawTarget {
public:
	static GifDrawTarget &instance()
	{
		static GifDrawTarget target;
		return target;
	}

	void bind(pixel::native_t *pixels, int width)
	{
		pixels_ = pixels;
		width_ = width;
	}

	void clear()
	{
		pixels_ = nullptr;
		width_ = 0;
	}

	static void draw(GIFDRAW *draw)
	{
		instance().drawFrame(draw);
	}

private:
	void drawFrame(GIFDRAW *draw)
	{
		if (!pixels_) return;

		const int canvasY = draw->iY + draw->y;
		pixel::native_t *dst = pixels_ + canvasY * width_ + draw->iX;
		std::uint16_t *palette = draw->pPalette;
		std::uint8_t *src = draw->pPixels;
		const int width = draw->iWidth;

		// Palette entries are RGB565; convert once to the native pixel (identity on
		// RGB565 targets, so this stays byte-identical there).
		if (draw->ucHasTransparency) {
			const std::uint8_t transparent = draw->ucTransparent;
			for (int i = 0; i < width; i++) {
				const std::uint8_t index = src[i];
				if (index != transparent) dst[i] = pixel::toNative(palette[index]);
			}
			return;
		}

		for (int i = 0; i < width; i++) dst[i] = pixel::toNative(palette[src[i]]);
	}

	pixel::native_t *pixels_ = nullptr;
	int width_ = 0;
};

class ImageMemory {
public:
	template <typename T>
	static T *allocate(std::size_t count = 1)
	{
		// Cache-line alignment (128B covers the P4's largest L2 line config):
		// decoded pixel buffers are DMA sources for PPA blits, and alignment
		// keeps cache writeback/invalidate exact at the buffer edges. Cost: a
		// few bytes per allocation.
		constexpr std::size_t kDmaAlign = 128;
		const std::size_t align = alignof(T) > kDmaAlign ? alignof(T) : kDmaAlign;
		return static_cast<T *>(memory::Allocator::allocatePreferSpiram(sizeof(T) * count, align));
	}

	static void release(void *ptr)
	{
		memory::Allocator::free(ptr);
	}
};

// Convert one decoded RGBA8888 source pixel to this target's native framebuffer
// pixel. RGB565 targets get the exact original truncating pack (byte-identical);
// full-colour targets keep all 8 bits per channel plus the source alpha; the
// grayscale targets take the luma level (0..15 GRAY4, 0..3 GRAY2 — native_t
// there is a gray LEVEL, not an RGB565 pack; routing through fromRgb888
// truncates a uint16 into the byte and produces garbage tones).
inline pixel::native_t rgbaToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
#if GEA_PIXEL_FORMAT_IS_8888
	return pixel::packNative8(r, g, b, a);
#elif GEA_PIXEL_FORMAT_IS_GRAY4
	(void)a;
	return pixel::gray4FromRgb888(r, g, b);
#elif GEA_PIXEL_FORMAT_IS_GRAY2
	(void)a;
	return pixel::gray2FromRgb888(r, g, b);
#else
	(void)a;
	return pixel::fromRgb888(r, g, b);
#endif
}

// Decode RGBA8888 into the native pixel buffer, building a separate 8-bit alpha
// plane for the framebuffer blit path (unchanged across targets).
int convertRgbaToNative(const std::uint8_t *rgba, pixel::native_t *out, std::uint8_t **alphaOut,
                        int pixelCount, bool opaqueHint)
{
	std::uint8_t *alpha = nullptr;
	bool hasZero = false;

	for (int i = 0; i < pixelCount; i++) {
		const int off = i * 4;
		const std::uint8_t a = rgba[off + 3];
		out[i] = rgbaToNative(rgba[off], rgba[off + 1], rgba[off + 2], a);

		if (opaqueHint) {
			if (a == 0) {
				hasZero = true;
				if (!alpha) {
					alpha = ImageMemory::allocate<std::uint8_t>(static_cast<std::size_t>(pixelCount));
					if (!alpha) return -1;
					std::memset(alpha, 255, static_cast<std::size_t>(pixelCount));
				}
				alpha[i] = 0;
			}
		} else if (a != 255) {
			if (!alpha) {
				alpha = ImageMemory::allocate<std::uint8_t>(static_cast<std::size_t>(pixelCount));
				if (!alpha) return -1;
				std::memset(alpha, 255, static_cast<std::size_t>(pixelCount));
			}
			alpha[i] = a;
		}
	}

	if (opaqueHint && alpha && !hasZero) {
		ImageMemory::release(alpha);
		alpha = nullptr;
	}

	*alphaOut = alpha;
	return 0;
}

}  // namespace

ImageStore &ImageStore::instance()
{
	static ImageStore store;
	return store;
}

ImageSlot *ImageStore::slot(int id)
{
	if (id < 0 || id >= kImageMax || !used_[id]) return nullptr;
	return &images_[id];
}

const ImageSlot *ImageStore::slot(int id) const
{
	if (id < 0 || id >= kImageMax || !used_[id]) return nullptr;
	return &images_[id];
}

ImageFormat ImageStore::detectFormat(const std::uint8_t *data, int length) const
{
	if (!data || length < 4) return ImageFormat::Unknown;
	if (data[0] == 0xFF && data[1] == 0xD8) return ImageFormat::Jpeg;
	if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return ImageFormat::Png;
	if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return ImageFormat::Gif;
	return ImageFormat::Unknown;
}

int ImageStore::findFreeSlot(int preferredSlot) const
{
	if (preferredSlot >= 0 && preferredSlot < kImageMax && !used_[preferredSlot]) return preferredSlot;
	for (int i = 0; i < kImageMax; i++) {
		if (!used_[i]) return i;
	}
	return -1;
}

int ImageStore::decodeNextGifFrame(ImageSlot &image)
{
	auto *gif = static_cast<GIFIMAGE *>(image.gifDecoder);
	if (!gif) return -1;

	GifDrawTarget::instance().bind(image.pixels, image.width);

	int delay = 0;
	const int rc = GIF_playFrame(gif, &delay);

	GifDrawTarget::instance().clear();

	if (rc < 0) return -1;
	if (delay <= 0) delay = 100;
	image.currentDelayMs = delay;
	return rc;
}

int ImageStore::decodeGif(ImageSlot &image, const std::uint8_t *data, int length)
{
	auto *gifData = static_cast<std::uint8_t *>(ImageMemory::allocate<std::uint8_t>(static_cast<std::size_t>(length)));
	if (!gifData) return -1;
	std::memcpy(gifData, data, static_cast<std::size_t>(length));

	auto *gif = static_cast<GIFIMAGE *>(ImageMemory::allocate<GIFIMAGE>());
	if (!gif) {
		ImageMemory::release(gifData);
		return -1;
	}

	GIF_begin(gif, GEA_EMBEDDED_PIXEL_PANEL_ENDIAN ? BIG_ENDIAN_PIXELS : LITTLE_ENDIAN_PIXELS, GIF_PALETTE_RGB565);
	if (!GIF_openRAM(gif, gifData, length, GifDrawTarget::draw)) {
		ImageMemory::release(gif);
		ImageMemory::release(gifData);
		return -1;
	}

	const int width = GIF_getCanvasWidth(gif);
	const int height = GIF_getCanvasHeight(gif);
	if (width <= 0 || height <= 0) {
		GIF_close(gif);
		ImageMemory::release(gif);
		ImageMemory::release(gifData);
		return -1;
	}

	auto *pixels = ImageMemory::allocate<pixel::native_t>(static_cast<std::size_t>(width) * height);
	if (!pixels) {
		GIF_close(gif);
		ImageMemory::release(gif);
		ImageMemory::release(gifData);
		return -1;
	}
	std::memset(pixels, 0, static_cast<std::size_t>(width) * height * sizeof(pixel::native_t));

	image.width = width;
	image.height = height;
	image.pixels = pixels;
	image.ownsPixels = true;
	image.gifDecoder = gif;
	image.gifData = gifData;
	image.gifDataLength = length;
	image.loopCount = -1;
	image.playing = true;
	image.frameCount = 2;

	if (decodeNextGifFrame(image) >= 0) return 0;

	ImageMemory::release(pixels);
	GIF_close(gif);
	ImageMemory::release(gif);
	ImageMemory::release(gifData);
	image = ImageSlot{};
	return -1;
}

#if defined(GEA_EMBEDDED_ESP_JPEG) && GEA_EMBEDDED_ESP_JPEG && GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
int ImageStore::decodeJpegEsp(ImageSlot &image, const std::uint8_t *data, int length)
{
#if GEA_EMBEDDED_PERF && defined(ESP_PLATFORM)
	static std::atomic<unsigned> sampleSequence{0};
	const bool measure = sampleSequence.fetch_add(1, std::memory_order_relaxed) % 10 == 0;
	const std::int64_t started = measure ? esp_timer_get_time() : 0;
#endif
	const auto result = [&](jpeg_error_t rc, const char *stage) {
#if GEA_EMBEDDED_PERF && defined(ESP_PLATFORM)
		if (measure) std::printf("[jpeg-esp] rc=%d stage=%s size=%dx%d total_us=%lld fallback=%s\n",
		                         static_cast<int>(rc), stage, image.width, image.height,
		                         static_cast<long long>(esp_timer_get_time() - started), rc == JPEG_ERR_OK ? "no" : "yes");
#else
		(void)stage;
#endif
		return rc == JPEG_ERR_OK ? 0 : -1;
	};
	struct Decoder {
		jpeg_dec_handle_t handle = nullptr;
		~Decoder() { if (handle) jpeg_dec_close(handle); }
	} decoder;
	jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
#if GEA_EMBEDDED_PIXEL_PANEL_ENDIAN
	config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
#else
	config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
#endif
	// Plain whole-image decode supports arbitrary dimensions, including 410x502.
	// Block mode, rotation and resize have stricter alignment requirements.
	jpeg_error_t rc = jpeg_dec_open(&config, &decoder.handle);
	if (rc != JPEG_ERR_OK) return result(rc, "open");
	jpeg_dec_io_t io{};
	io.inbuf = const_cast<std::uint8_t *>(data);
	io.inbuf_len = length;
	jpeg_dec_header_info_t info{};
	rc = jpeg_dec_parse_header(decoder.handle, &io, &info);
	if (rc != JPEG_ERR_OK) return result(rc, "header");
	if (!info.width || !info.height || static_cast<std::size_t>(info.width) >
	    static_cast<std::size_t>(std::numeric_limits<int>::max()) / sizeof(pixel::native_t) / info.height)
		return result(JPEG_ERR_INVALID_PARAM, "dimensions");
	const std::size_t count = static_cast<std::size_t>(info.width) * info.height;
	const int byteCount = static_cast<int>(count * sizeof(pixel::native_t));
	int outputBytes = 0;
	rc = jpeg_dec_get_outbuf_len(decoder.handle, &outputBytes);
	if (rc != JPEG_ERR_OK) return result(rc, "buffer-size");
	if (outputBytes != byteCount) return result(JPEG_ERR_INVALID_PARAM, "buffer-size");
	auto *pixels = ImageMemory::allocate<pixel::native_t>(count);
	if (!pixels) return result(JPEG_ERR_NO_MEM, "allocation");
	io.outbuf = reinterpret_cast<std::uint8_t *>(pixels);
	rc = jpeg_dec_process(decoder.handle, &io);
	if (rc != JPEG_ERR_OK || io.out_size != byteCount) {
		ImageMemory::release(pixels);
		return result(rc == JPEG_ERR_OK ? JPEG_ERR_BAD_DATA : rc, "decode");
	}
	image.width = info.width;
	image.height = info.height;
	image.frameCount = 1;
	image.pixels = pixels;
	image.ownsPixels = true;
	image.alpha = nullptr;
	image.loopCount = 0;
	image.playing = false;
	return result(JPEG_ERR_OK, "decode");
}
#endif

#if defined(GEA_EMBEDDED_ROM_TJPGD) && GEA_EMBEDDED_ROM_TJPGD
namespace {
// Shared input+output context for one ROM-tjpgd session (jd->device carries it
// to both callbacks).
struct TjpgdCtx {
	const std::uint8_t *in;
	std::size_t len;
	std::size_t pos;
	pixel::native_t *out;
	int outW;
	int outH;
};

// Stream bytes from the in-memory JPEG. A null buf means "skip nbyte".
UINT geaTjpgdInput(JDEC *jd, BYTE *buf, UINT nbyte)
{
	auto *c = static_cast<TjpgdCtx *>(jd->device);
	const std::size_t avail = c->len - c->pos;
	const std::size_t take = nbyte < avail ? nbyte : avail;
	if (buf) std::memcpy(buf, c->in + c->pos, take);
	c->pos += take;
	return static_cast<UINT>(take);
}

// Convert each MCU directly into the final image. Preserve colour and source
// dimensions: image loading must not silently resize a 502px-high video frame.
UINT geaTjpgdOutput(JDEC *jd, void *bitmap, JRECT *rect)
{
	auto *c = static_cast<TjpgdCtx *>(jd->device);
	if (!bitmap || rect->left > rect->right || rect->top > rect->bottom ||
	    rect->right >= c->outW || rect->bottom >= c->outH) return 0;
	const std::uint8_t *rgb = static_cast<const std::uint8_t *>(bitmap);
	for (int y = rect->top; y <= rect->bottom; y++) {
		for (int x = rect->left; x <= rect->right; x++) {
			const std::uint8_t r = rgb[0];
			const std::uint8_t g = rgb[1];
			const std::uint8_t b = rgb[2];
			rgb += 3;
			c->out[static_cast<std::size_t>(y) * c->outW + x] = rgbaToNative(r, g, b, 255);
		}
	}
	return 1;
}
}  // namespace

// Decode at the source dimensions with bounded scratch plus the native image.
int ImageStore::decodeJpegTjpgd(ImageSlot &image, const std::uint8_t *data, int length)
{
	TjpgdCtx ctx{data, static_cast<std::size_t>(length), 0, nullptr, 0, 0};

	// Keep the frequently accessed Huffman/quant tables in internal memory when
	// available. Allocation is per decode and falls back to PSRAM under pressure.
	constexpr std::size_t poolSize = 8 * 1024;
	void *pool = nullptr;
#ifdef ESP_PLATFORM
	pool = heap_caps_malloc(poolSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
	if (!pool) pool = memory::Allocator::allocatePreferSpiram(poolSize, alignof(std::max_align_t));
	if (!pool) return -1;
	JDEC jd{};
	const JRESULT prepareRc = jd_prepare(&jd, geaTjpgdInput, pool, poolSize, &ctx);
	if (prepareRc != JDR_OK) {
		memory::Allocator::free(pool);
		return -1;
	}
	if (jd.width == 0 || jd.height == 0 ||
	    jd.width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
	    jd.height > static_cast<UINT>(std::numeric_limits<int>::max()) ||
	    static_cast<std::size_t>(jd.width) > std::numeric_limits<std::size_t>::max() / sizeof(pixel::native_t) / jd.height) {
		memory::Allocator::free(pool);
		return -1;
	}
	ctx.outW = static_cast<int>(jd.width);
	ctx.outH = static_cast<int>(jd.height);
	auto *px = ImageMemory::allocate<pixel::native_t>(static_cast<std::size_t>(ctx.outW) * ctx.outH);
	if (!px) {
		memory::Allocator::free(pool);
		return -1;
	}
	ctx.out = px;
	const JRESULT rc = jd_decomp(&jd, geaTjpgdOutput, 0);
	memory::Allocator::free(pool);
	if (rc != JDR_OK) {
		ImageMemory::release(px);
		return -1;
	}
	image.width = ctx.outW;
	image.height = ctx.outH;
	image.frameCount = 1;
	image.pixels = px;
	image.ownsPixels = true;
	image.alpha = nullptr;
	image.loopCount = 0;
	image.playing = false;
	return 0;
}
#endif

#if defined(GEA_EMBEDDED_ROM_PNG_STREAM) && GEA_EMBEDDED_ROM_PNG_STREAM
namespace {

// Streaming PNG decode: the IDAT stream inflates through a 32 KB wrapping
// window, each scanline is unfiltered against the previous one as it
// completes and converted straight to native pixels. Peak memory is the
// result (plus an alpha plane when the image has transparency), two
// scanlines, the window and the inflater -- not the whole inflated image
// and an RGBA copy, which is what stb holds. Handles what assets are made
// of: 8-bit, non-interlaced gray, RGB, palette, gray+alpha and RGBA.
// Anything else returns kPngUnsupported and stays on stb.
constexpr int kPngUnsupported = -2;

std::uint32_t pngU32(const std::uint8_t *p)
{
	return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

struct PngStream {
	int width = 0;
	int height = 0;
	int colorType = 0;
	int channels = 0;
	std::size_t stride = 0;  // bytes per scanline, filter byte excluded
	std::uint8_t palette[256][4] = {};
	bool opaqueHint = false;
	pixel::native_t *pixels = nullptr;
	std::uint8_t *alpha = nullptr;
	bool hasZero = false;
	std::uint8_t *prev = nullptr;
	std::uint8_t *cur = nullptr;  // filter byte + stride
	std::size_t filled = 0;
	int row = 0;
	bool failed = false;

	void emitPixel(int index, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
	{
		pixels[index] = rgbaToNative(r, g, b, a);
		// Same alpha plane convertRgbaToNative builds: opaque-hinted images
		// keep only fully transparent pixels, others every non-255 value.
		const bool keep = opaqueHint ? a == 0 : a != 255;
		if (!keep) {
			if (alpha) alpha[index] = 255;
			return;
		}
		if (a == 0) hasZero = true;
		if (!alpha) {
			const std::size_t count = static_cast<std::size_t>(width) * height;
			alpha = ImageMemory::allocate<std::uint8_t>(count);
			if (!alpha) {
				failed = true;
				return;
			}
			std::memset(alpha, 255, count);
		}
		alpha[index] = opaqueHint ? 0 : a;
	}

	void finishRow()
	{
		const int bpp = channels;
		const std::uint8_t filter = cur[0];
		std::uint8_t *line = cur + 1;
		const std::uint8_t *up = prev + 1;
		for (std::size_t i = 0; i < stride; i++) {
			const int left = i >= static_cast<std::size_t>(bpp) ? line[i - bpp] : 0;
			const int above = up[i];
			const int corner = i >= static_cast<std::size_t>(bpp) ? up[i - bpp] : 0;
			int predictor = 0;
			switch (filter) {
			case 0: predictor = 0; break;
			case 1: predictor = left; break;
			case 2: predictor = above; break;
			case 3: predictor = (left + above) >> 1; break;
			case 4: {
				const int p = left + above - corner;
				const int pa = p > left ? p - left : left - p;
				const int pb = p > above ? p - above : above - p;
				const int pc = p > corner ? p - corner : corner - p;
				predictor = (pa <= pb && pa <= pc) ? left : (pb <= pc ? above : corner);
				break;
			}
			default: failed = true; return;
			}
			line[i] = static_cast<std::uint8_t>(line[i] + predictor);
		}
		const int base = row * width;
		for (int x = 0; x < width && !failed; x++) {
			const std::uint8_t *px = line + static_cast<std::size_t>(x) * channels;
			switch (colorType) {
			case 0: emitPixel(base + x, px[0], px[0], px[0], 255); break;
			case 2: emitPixel(base + x, px[0], px[1], px[2], 255); break;
			case 3: {
				const std::uint8_t *e = palette[px[0]];
				emitPixel(base + x, e[0], e[1], e[2], e[3]);
				break;
			}
			case 4: emitPixel(base + x, px[0], px[0], px[0], px[1]); break;
			default: emitPixel(base + x, px[0], px[1], px[2], px[3]); break;
			}
		}
		std::uint8_t *swap = prev;
		prev = cur;
		cur = swap;
		filled = 0;
		row++;
	}

	void consume(const std::uint8_t *bytes, std::size_t count)
	{
		while (count > 0 && row < height && !failed) {
			const std::size_t need = stride + 1 - filled;
			const std::size_t take = count < need ? count : need;
			std::memcpy(cur + filled, bytes, take);
			filled += take;
			bytes += take;
			count -= take;
			if (filled == stride + 1) finishRow();
		}
	}
};

// Offset of the next IDAT chunk at or after `pos`, or -1.
long nextIdat(const std::uint8_t *data, int length, long pos)
{
	while (pos + 12 <= length) {
		const std::uint32_t size = pngU32(data + pos);
		if (size > static_cast<std::uint32_t>(length) || pos + 12 + static_cast<long>(size) > length) return -1;
		if (std::memcmp(data + pos + 4, "IDAT", 4) == 0) return pos;
		if (std::memcmp(data + pos + 4, "IEND", 4) == 0) return -1;
		pos += 12 + static_cast<long>(size);
	}
	return -1;
}

int decodePngStreaming(ImageSlot &image, const std::uint8_t *data, int length, bool opaqueHint)
{
	static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (length < 33 || std::memcmp(data, kSignature, 8) != 0 || std::memcmp(data + 12, "IHDR", 4) != 0) return kPngUnsupported;
	const std::uint32_t width = pngU32(data + 16);
	const std::uint32_t height = pngU32(data + 20);
	const int depth = data[24];
	const int colorType = data[25];
	const int interlace = data[28];
	int channels = 0;
	switch (colorType) {
	case 0: channels = 1; break;
	case 2: channels = 3; break;
	case 3: channels = 1; break;
	case 4: channels = 2; break;
	case 6: channels = 4; break;
	default: return kPngUnsupported;
	}
	if (depth != 8 || interlace != 0 || width == 0 || height == 0 || width > 16384 || height > 16384) return kPngUnsupported;

	PngStream png;
	png.width = static_cast<int>(width);
	png.height = static_cast<int>(height);
	png.colorType = colorType;
	png.channels = channels;
	png.stride = static_cast<std::size_t>(width) * channels;
	png.opaqueHint = opaqueHint;

	// Palette and tRNS precede the image data.
	bool havePalette = false;
	for (long pos = 33; pos + 12 <= length;) {
		const std::uint32_t size = pngU32(data + pos);
		if (pos + 12 + static_cast<long>(size) > length) return kPngUnsupported;
		const std::uint8_t *body = data + pos + 8;
		if (std::memcmp(data + pos + 4, "IDAT", 4) == 0 || std::memcmp(data + pos + 4, "IEND", 4) == 0) break;
		if (std::memcmp(data + pos + 4, "PLTE", 4) == 0) {
			for (std::uint32_t i = 0; i < size / 3 && i < 256; i++) {
				png.palette[i][0] = body[i * 3];
				png.palette[i][1] = body[i * 3 + 1];
				png.palette[i][2] = body[i * 3 + 2];
				png.palette[i][3] = 255;
			}
			havePalette = true;
		} else if (std::memcmp(data + pos + 4, "tRNS", 4) == 0) {
			// Colour-key transparency on gray/RGB is rare in assets; leave it to stb.
			if (colorType != 3) return kPngUnsupported;
			for (std::uint32_t i = 0; i < size && i < 256; i++) png.palette[i][3] = body[i];
		}
		pos += 12 + static_cast<long>(size);
	}
	if (colorType == 3 && !havePalette) return kPngUnsupported;
	long idat = nextIdat(data, length, 33);
	if (idat < 0) return kPngUnsupported;

	const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
	png.pixels = ImageMemory::allocate<pixel::native_t>(pixelCount);
	png.prev = ImageMemory::allocate<std::uint8_t>(png.stride + 1);
	png.cur = ImageMemory::allocate<std::uint8_t>(png.stride + 1);
	auto *inflator = ImageMemory::allocate<tinfl_decompressor>();
	auto *window = ImageMemory::allocate<std::uint8_t>(TINFL_LZ_DICT_SIZE);
	auto cleanup = [&](bool keepResult) {
		ImageMemory::release(png.prev);
		ImageMemory::release(png.cur);
		ImageMemory::release(inflator);
		ImageMemory::release(window);
		if (!keepResult) {
			ImageMemory::release(png.pixels);
			ImageMemory::release(png.alpha);
		}
	};
	if (!png.pixels || !png.prev || !png.cur || !inflator || !window) {
		cleanup(false);
		return -1;
	}
	std::memset(png.prev, 0, png.stride + 1);
	tinfl_init(inflator);

	std::size_t windowOffset = 0;
	const std::uint8_t *in = data + idat + 8;
	std::size_t inLeft = pngU32(data + idat);
	long following = nextIdat(data, length, idat + 12 + static_cast<long>(inLeft));
	tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
	while (!png.failed) {
		std::size_t inSize = inLeft;
		std::size_t outSize = TINFL_LZ_DICT_SIZE - windowOffset;
		const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (following >= 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
		status = tinfl_decompress(inflator, in, &inSize, window, window + windowOffset, &outSize, flags);
		in += inSize;
		inLeft -= inSize;
		png.consume(window + windowOffset, outSize);
		windowOffset = (windowOffset + outSize) & (TINFL_LZ_DICT_SIZE - 1);
		if (status == TINFL_STATUS_DONE || png.row >= png.height) break;
		if (status < 0) break;
		if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inLeft == 0) {
			if (following < 0) break;
			idat = following;
			in = data + idat + 8;
			inLeft = pngU32(data + idat);
			following = nextIdat(data, length, idat + 12 + static_cast<long>(inLeft));
		}
	}
	if (png.failed || png.row < png.height) {
		cleanup(false);
		return -1;
	}
	if (opaqueHint && png.alpha && !png.hasZero) {
		ImageMemory::release(png.alpha);
		png.alpha = nullptr;
	}
	cleanup(true);
	image.width = png.width;
	image.height = png.height;
	image.frameCount = 1;
	image.pixels = png.pixels;
	image.ownsPixels = true;
	image.alpha = png.alpha;
	image.loopCount = 0;
	image.playing = false;
	return 0;
}

}  // namespace
#endif

int ImageStore::decodeStatic(ImageSlot &image, const std::uint8_t *data, int length, bool opaqueHint)
{
	int width = 0;
	int height = 0;
	int components = 0;

	// "GTHM": raw 8-bit grayscale thumbnail with a fixed 8-byte header
	// (magic, u16le width, u16le height), written by app-side thumbnailers
	// (e-reader library covers). Decodes straight to native pixels — no codec.
	if (length >= 8 && data[0] == 'G' && data[1] == 'T' && data[2] == 'H' && data[3] == 'M') {
		const int tw = data[4] | (data[5] << 8);
		const int th = data[6] | (data[7] << 8);
		if (tw <= 0 || th <= 0 || length < 8 + tw * th) return -1;
		auto *px = ImageMemory::allocate<pixel::native_t>(static_cast<std::size_t>(tw) * th);
		if (!px) return -1;
		const std::uint8_t *gray = data + 8;
		const int count = tw * th;
		for (int i = 0; i < count; i++) px[i] = rgbaToNative(gray[i], gray[i], gray[i], 255);
		image.width = tw;
		image.height = th;
		image.frameCount = 1;
		image.pixels = px;
		image.ownsPixels = true;
		image.alpha = nullptr;
		image.loopCount = 0;
		image.playing = false;
		return 0;
	}

#if defined(GEA_EMBEDDED_ESP_JPEG) && GEA_EMBEDDED_ESP_JPEG && GEA_EMBEDDED_PIXEL_FORMAT == GEA_PIXEL_RGB565
	if (detectFormat(data, length) == ImageFormat::Jpeg) {
		if (decodeJpegEsp(image, data, length) == 0) return 0;
	}
#endif

#if defined(GEA_EMBEDDED_ROM_TJPGD) && GEA_EMBEDDED_ROM_TJPGD
	// JPEG has no alpha; both loadBytes and loadBytesOpaque can use the native
	// streaming path. Unsupported encodings (e.g. progressive) fall back to stb.
	if (detectFormat(data, length) == ImageFormat::Jpeg) {
		if (decodeJpegTjpgd(image, data, length) == 0) return 0;
	}
#endif

#if (defined(GEA_EMBEDDED_DISPLAY_MONOCHROME) && GEA_EMBEDDED_DISPLAY_MONOCHROME) || \
    (defined(GEA_EMBEDDED_IMAGE_GRAYSCALE_DECODE) && GEA_EMBEDDED_IMAGE_GRAYSCALE_DECODE)
	// Grayscale panel (1-bit e-paper OR 16-level e-paper like M5Paper) + a LARGE
	// image (a full-page cover / photo): decode to a single grayscale channel
	// (1 B/px) instead of RGBA (4 B/px). The panel shows no color so this is
	// visually identical, and it cuts the decode transient 4× — a 525×735 cover
	// drops from a 1.5 MB RGBA buffer to 386 KB, which fits the device's ~1.7 MB
	// free PSRAM (the RGBA path OOM-aborted a bystander `new`, or failed soft to a
	// blank image). Gated on size (grayscale drops the alpha plane) so small
	// transparent icons keep the full RGBA path; only the memory-heavy images
	// take the grayscale route.
	{
		int infoW = 0, infoH = 0, infoC = 0;
		if (stbi_info_from_memory(data, length, &infoW, &infoH, &infoC) && static_cast<long long>(infoW) * infoH > 200000) {
			stbi_uc *gray = stbi_load_from_memory(data, length, &width, &height, &components, 1);
			if (!gray) return -1;
			// Box-downsample large covers so the RESIDENT native buffer (and the
			// transient during conversion) stays small. On the ~1.7 MB-free device
			// a full-res 525×735 native buffer (771 KB) added to the still-held
			// grayscale (386 KB) and compressed file (~450 KB) buffers overflowed
			// the heap → the native allocation failed and the decode returned -1
			// (a blank cover). Halving each axis while the source stays over the
			// panel's short side (540) cuts the native buffer 4× (→ ~190 KB); a
			// grayscale e-paper contain-fitting the result shows no visible loss.
			int factor = 1;
			while ((width / factor > 480 || height / factor > 480) && factor < 8) factor *= 2;
			const int outW = width / factor;
			const int outH = height / factor;
			const int outPixelCount = outW * outH;
			auto *grayPixels = ImageMemory::allocate<pixel::native_t>(static_cast<std::size_t>(outPixelCount));
			if (!grayPixels) {
				stbi_image_free(gray);
				return -1;
			}
			for (int oy = 0; oy < outH; oy++) {
				for (int ox = 0; ox < outW; ox++) {
					// Average the factor×factor source block for a clean downscale.
					int sum = 0;
					for (int by = 0; by < factor; by++) {
						const std::uint8_t *row = gray + static_cast<std::size_t>(oy * factor + by) * width + ox * factor;
						for (int bx = 0; bx < factor; bx++) sum += row[bx];
					}
					const std::uint8_t g = static_cast<std::uint8_t>(sum / (factor * factor));
					grayPixels[oy * outW + ox] = rgbaToNative(g, g, g, 255);
				}
			}
			stbi_image_free(gray);
			image.width = outW;
			image.height = outH;
			image.frameCount = 1;
			image.pixels = grayPixels;
			image.ownsPixels = true;
			image.alpha = nullptr;
			image.loopCount = 0;
			image.playing = false;
			return 0;
		}
	}
#endif

#if defined(GEA_EMBEDDED_ROM_PNG_STREAM) && GEA_EMBEDDED_ROM_PNG_STREAM
	if (detectFormat(data, length) == ImageFormat::Png) {
		const int rc = decodePngStreaming(image, data, length, opaqueHint);
		if (rc != kPngUnsupported) return rc;
	}
#endif

	stbi_uc *rgba = stbi_load_from_memory(data, length, &width, &height, &components, 4);
	if (!rgba) return -1;

	const int pixelCount = width * height;
	auto *pixels = ImageMemory::allocate<pixel::native_t>(static_cast<std::size_t>(pixelCount));
	if (!pixels) {
		stbi_image_free(rgba);
		return -1;
	}

	// Decode straight into the native pixel: RGB565 targets get the 565 copy
	// (byte-identical to before), full-colour targets keep the 8-bit channels +
	// source alpha — no separate retained RGBA8888 buffer, no dual-path.
	std::uint8_t *alpha = nullptr;
	if (convertRgbaToNative(rgba, pixels, &alpha, pixelCount, opaqueHint) < 0) {
		ImageMemory::release(pixels);
		stbi_image_free(rgba);
		return -1;
	}
	stbi_image_free(rgba);

	image.width = width;
	image.height = height;
	image.frameCount = 1;
	image.pixels = pixels;
	image.ownsPixels = true;
	image.alpha = alpha;
	image.loopCount = 0;
	image.playing = false;
	return 0;
}

int ImageStore::decodeWithHint(const std::uint8_t *data, int length, int preferredSlot, bool opaqueHint)
{
	if (!data || length <= 0) return -1;

	// Reserve the slot under the lock, decode without it: the async tile
	// loader decodes on the second core while the frame task allocates and
	// disposes other slots. The reserved id isn't visible to the app until the
	// loader delivers it, so nothing reads the slot mid-decode.
	int id = -1;
	{
		ImageStoreLock lock(slotMutex_);
		id = findFreeSlot(preferredSlot);
		if (id < 0) return -1;
		if (used_[id]) disposeLocked(id);
		images_[id] = ImageSlot{};
		used_[id] = true;
	}

	ImageSlot &image = images_[id];
	image.format = detectFormat(data, length);

	const int rc = image.format == ImageFormat::Gif ? decodeGif(image, data, length)
	                                                : decodeStatic(image, data, length, opaqueHint);
	if (rc < 0) {
		ImageStoreLock lock(slotMutex_);
		disposeLocked(id);
		return -1;
	}

	image.currentFrame = 0;
	image.elapsedMs = 0;
	return id;
}

int ImageStore::decodeDeferred(const std::uint8_t *data, int length, int preferredSlot)
{
	if (!data || length <= 0) return -1;
	// Animated content needs its decoder state up-front; only defer stills.
	if (detectFormat(data, length) == ImageFormat::Gif) return decode(data, length, preferredSlot);

	int width = 0;
	int height = 0;
	int components = 0;
	if (!stbi_info_from_memory(data, length, &width, &height, &components)) {
		return decode(data, length, preferredSlot);
	}

	int id = -1;
	{
		ImageStoreLock lock(slotMutex_);
		id = findFreeSlot(preferredSlot);
		if (id < 0) return -1;
		if (used_[id]) disposeLocked(id);
		images_[id] = ImageSlot{};
		used_[id] = true;
	}
	ImageSlot &image = images_[id];
	image.format = detectFormat(data, length);
	image.width = width;
	image.height = height;
	image.frameCount = 1;
	image.deferredData = data;
	image.deferredLength = length;
	image.deferredOpaque = components == 1 || components == 3;
	return id;
}

bool ImageStore::materializeOneDeferred()
{
	for (int i = 0; i < kImageMax; i++) {
		if (used_[i] && images_[i].deferredData) {
			materializeDeferred(i);
			return true;
		}
	}
	return false;
}

void ImageStore::materializeDeferred(int id) const
{
	auto *self = const_cast<ImageStore *>(this);
	ImageSlot *image = self->slot(id);
	if (!image || !image->deferredData) return;
	ImageStoreLock lock(self->slotMutex_);
	if (!image->deferredData) return;  // raced with another materializer
	const std::uint8_t *data = image->deferredData;
	const int length = image->deferredLength;
	const bool opaque = image->deferredOpaque;
	image->deferredData = nullptr;
	image->deferredLength = 0;
	image->deferredOpaque = false;
	image->sourceData = data;
	image->sourceLength = length;
	image->sourceOpaque = opaque;
	self->decodeStatic(*image, data, length, opaque);
}

#ifndef GEA_IMAGE_IDLE_DECODE_RELEASE_BYTES
#define GEA_IMAGE_IDLE_DECODE_RELEASE_BYTES (256 * 1024)
#endif

void ImageStore::releaseIdleDecodes()
{
	ImageStoreLock lock(slotMutex_);
	for (int id = 0; id < kImageMax; ++id) {
		ImageSlot &image = images_[id];
		if (!used_[id] || references_[id] != 0 || retired_[id]) continue;
		if (!image.sourceData || !image.ownsPixels || !image.pixels || image.mutablePixels) continue;
		const std::size_t bytes = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) *
		                          sizeof(pixel::native_t);
		if (bytes < GEA_IMAGE_IDLE_DECODE_RELEASE_BYTES) continue;
		ImageMemory::release(image.pixels);
		ImageMemory::release(image.alpha);
		image.pixels = nullptr;
		image.alpha = nullptr;
		image.ownsPixels = false;
		image.deferredData = image.sourceData;
		image.deferredLength = image.sourceLength;
		image.deferredOpaque = image.sourceOpaque;
		image.sourceData = nullptr;
		image.sourceLength = 0;
	}
}

int ImageStore::decode(const std::uint8_t *data, int length, int preferredSlot)
{
	return decodeWithHint(data, length, preferredSlot, false);
}

int ImageStore::decodeOpaque(const std::uint8_t *data, int length, int preferredSlot)
{
	return decodeWithHint(data, length, preferredSlot, true);
}

int ImageStore::width(int id) const { const ImageSlot *image = slot(id); return image ? image->width : 0; }
int ImageStore::height(int id) const { const ImageSlot *image = slot(id); return image ? image->height : 0; }
int ImageStore::frameCount(int id) const { const ImageSlot *image = slot(id); return image ? image->frameCount : 0; }
bool ImageStore::isAnimated(int id) const { const ImageSlot *image = slot(id); return image && image->frameCount > 1; }
ImageFormat ImageStore::format(int id) const { const ImageSlot *image = slot(id); return image ? image->format : ImageFormat::Unknown; }
bool ImageStore::playing(int id) const { const ImageSlot *image = slot(id); return image && image->playing; }
int ImageStore::currentFrame(int id) const { const ImageSlot *image = slot(id); return image ? image->currentFrame : 0; }
bool ImageStore::pixelsMayChange(const pixel::native_t *pixels) const
{
	if (!pixels) return false;
	for (int i = 0; i < kImageMax; i++) {
		// Anywhere inside the buffer: a dirty-rect putImageData draws rows that
		// start part-way into it (CanvasRenderingContext2D::drawPixelRows).
		const ImageSlot &image = images_[i];
		if (!used_[i] || !image.mutablePixels || !image.pixels) continue;
		const std::size_t count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
		if (pixels >= image.pixels && pixels < image.pixels + count) return true;
	}
	return false;
}

const pixel::native_t *ImageStore::currentPixels(int id) const { materializeDeferred(id); const ImageSlot *image = slot(id); return image ? image->pixels : nullptr; }
const std::uint8_t *ImageStore::currentAlpha(int id) const { materializeDeferred(id); const ImageSlot *image = slot(id); return image ? image->alpha : nullptr; }

void ImageStore::setPlaying(int id, bool playing)
{
	ImageSlot *image = slot(id);
	if (image) image->playing = playing;
}

void ImageStore::seek(int id, int frame)
{
	ImageSlot *image = slot(id);
	if (!image || !image->gifDecoder || image->frameCount <= 1) return;
	if (frame < 0) frame = 0;
	if (frame >= image->frameCount) frame = image->frameCount - 1;

	auto *gif = static_cast<GIFIMAGE *>(image->gifDecoder);
	GIF_reset(gif);
	std::memset(image->pixels, 0, static_cast<std::size_t>(image->width) * image->height * sizeof(pixel::native_t));
	for (int f = 0; f <= frame; f++) decodeNextGifFrame(*image);
	image->currentFrame = frame;
	image->elapsedMs = 0;
}

bool ImageStore::advance(int id, int deltaMs)
{
	ImageSlot *image = slot(id);
	if (!image || !image->playing || image->frameCount <= 1 || !image->gifDecoder) return false;

	image->elapsedMs += deltaMs;
	bool changed = false;
	while (image->elapsedMs >= image->currentDelayMs && image->currentDelayMs > 0) {
		image->elapsedMs -= image->currentDelayMs;
		const int rc = decodeNextGifFrame(*image);
		if (rc < 0) break;
		image->currentFrame++;
		if (rc == 0) image->currentFrame = 0;
		changed = true;
	}
	return changed;
}

const pixel::native_t *ImageStore::framePixels(int id, int frame) const
{
	materializeDeferred(id);
	const ImageSlot *image = slot(id);
	if (!image || !image->pixels) return nullptr;
	if (image->frameCount > 1 && frame != image->currentFrame) return nullptr;
	return image->pixels;
}

bool ImageStore::rotate90(int id)
{
	ImageSlot *image = slot(id);
	if (!image || !image->pixels || image->alpha) return false;
	const int w = image->width;
	const int h = image->height;
	if (w != h || w <= 0) return false;
	pixel::native_t *scratch = ImageMemory::allocate<pixel::native_t>(static_cast<std::size_t>(w) * h);
	if (!scratch) return false;
	// panel-local (px,py) <- landscape-local (lx,ly): px=ly, py=w-1-lx
	// => rotated[py*w+px] = src[px*w + (w-1-py)]
	for (int py = 0; py < h; py++) {
		pixel::native_t *dst = scratch + static_cast<std::size_t>(py) * w;
		const int sx = w - 1 - py;
		for (int px = 0; px < w; px++) {
			dst[px] = image->pixels[static_cast<std::size_t>(px) * w + sx];
		}
	}
	std::memcpy(image->pixels, scratch, static_cast<std::size_t>(w) * h * sizeof(pixel::native_t));
	ImageMemory::release(scratch);
	return true;
}

bool ImageStore::retain(int id)
{
	if (id < 0 || id >= kImageMax) return false;
	ImageStoreLock lock(slotMutex_);
	if (!slot(id)) return false;
	++references_[id];
	return true;
}

void ImageStore::release(int id)
{
	if (id < 0 || id >= kImageMax) return;
	ImageStoreLock lock(slotMutex_);
	if (!slot(id) || references_[id] == 0) return;
	if (--references_[id] == 0 && retired_[id]) disposeLocked(id);
}

int ImageStore::idForPixels(const pixel::native_t *pixels) const
{
	if (!pixels) return -1;
	ImageStoreLock lock(slotMutex_);
	for (int id = 0; id < kImageMax; ++id)
		if (used_[id] && images_[id].pixels == pixels) return id;
	return -1;
}

void ImageStore::dispose(int id)
{
	ImageStoreLock lock(slotMutex_);
	if (!slot(id)) return;
	retired_[id] = true;
	if (references_[id] == 0) disposeLocked(id);
}

void ImageStore::disposeLocked(int id)
{
	if (id < 0 || id >= kImageMax) return;
	ImageSlot *image = &images_[id];

	if (image->gifDecoder) {
		GIF_close(static_cast<GIFIMAGE *>(image->gifDecoder));
		ImageMemory::release(image->gifDecoder);
	}
	ImageMemory::release(image->gifData);
	if (image->ownsPixels) ImageMemory::release(image->pixels);
	ImageMemory::release(image->alpha);
	*image = ImageSlot{};
	used_[id] = false;
	references_[id] = 0;
	retired_[id] = false;
}

void ImageStore::disposeAll()
{
	ImageStoreLock lock(slotMutex_);
	for (int i = 0; i < kImageMax; i++) {
		if (used_[i]) {
			retired_[i] = true;
			if (references_[i] == 0) disposeLocked(i);
		}
	}
}

int ImageStore::registerBuffer(pixel::native_t *pixels, int width, int height, int preferredSlot, bool takeOwnership)
{
	if (!pixels || width <= 0 || height <= 0) return -1;

	ImageStoreLock lock(slotMutex_);
	const int id = findFreeSlot(preferredSlot);
	if (id < 0) return -1;
	if (used_[id]) disposeLocked(id);

	ImageSlot &image = images_[id];
	image = ImageSlot{};
	image.width = width;
	image.height = height;
	image.frameCount = 1;
	image.pixels = pixels;
	image.ownsPixels = takeOwnership;
	image.mutablePixels = true;
	image.loopCount = 0;
	image.playing = false;
	used_[id] = true;
	return id;
}

}  // namespace gea::framework::graphics
