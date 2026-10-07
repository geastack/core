#include "display.h"
#include "image.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/image.h"
#include "ui/internal.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <array>
#include <cassert>

namespace gea::framework::app::generated
{
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated
{
void __attribute__((weak)) ensureLinked() {}
} // namespace gea::framework::graphics::generated

int main()
{
	using namespace gea::embedded::ui;
	using namespace gea::embedded::test;
	namespace pixel = gea::framework::graphics::pixel;
	// A 4x2 PNG with opaque red/blue/white, half-alpha green and transparent blue.
	constexpr unsigned char png[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0,	  0,	0,	  0x0d, 0x49, 0x48, 0x44, 0x52, 0,
		0,	  0,	4,	  0,	0,	  0,	2,	  8,	6,	  0,	0,	  0,	0x7f, 0xa8, 0x7d, 0x63, 0,
		0,	  0,	0x1b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 8,	0x1b,
		0xc0, 0x14, 0x10, 0x30, 0x30, 0x80, 0x29, 0x30, 0x0b, 0x24, 0xf1, 0x1f, 0,	  0x22, 0x1b, 0x12, 0x6f,
		0x89, 0xcd, 0xc2, 0x0e, 0,	  0,	0,	  0,	0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
	resetNativeHost();
	setNativeDisplaySize(32, 32);
	setViewportMetrics(32, 32, 1);
	auto &document = Document::instance();
	auto root = document.createView();
	root.style().width(32);
	root.style().height(32);
	root.style().backgroundColor(pixel::fromRgb565(0));
	auto image = document.createImage();
	image.style().setProperty("position", "absolute");
	image.style().left(10);
	image.style().top(10);
	image.style().width(8);
	image.style().height(4);
	image.style().setProperty("transform-origin", "0px 0px");
	image.style().setProperty("transform", "rotate(90deg)");
	const int imageId = gea::framework::graphics::ImageStore::instance().decode(png, sizeof(png));
	assert(imageId >= 0);
	ImageElement(image.id()).imageId(imageId);
	root.appendChild(image);
	document.mount(root, 32, 32);
	const auto red = pixel::fromRgb565(0xf800);
	const auto blue = pixel::fromRgb565(0x001f);
	const auto halfGreen = pixel::blendNative(pixel::fromRgb565(0x07e0), pixel::fromRgb565(0), 128);
	assert(displayPixelAt(9, 10) == red);
	assert(displayPixelAt(7, 10) == blue);
	assert(displayPixelAt(9, 12) == halfGreen);
	assert(displayPixelAt(9, 14) == 0);
	assert(displayPixelAt(11, 10) == 0);

	// Retained off-screen replay must project identically to the displayed frame.
	std::array<std::uint16_t, 32 * 32> snapshot{};
	assert(renderRetainedSnapshotRgb565(snapshot.data(), 32, 32));
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x)
			assert(snapshot[y * 32 + x] == pixel::toRgb565(displayPixelAt(x, y)));

	// Replay clips the rotated destination, not the original image box.
	// The native test host's snapshot rebind hook is a stub; reset its canvas
	// binding before testing the next independent replay.
	setNativeDisplaySize(32, 32);
	DisplayList::instance().replayDirectDirtyRegion(8, 10, 9, 17);
	assert(displayPixelAt(9, 10) == red);
	assert(displayPixelAt(7, 10) == 0);
	assert(displayPixelAt(9, 14) == 0);

	// A transform-only update must not leave the old projected corners retained.
	image.style().setProperty("transform", "rotate(180deg)");
	Tree::instance().refresh(root.id(), 32, 32);
	assert(displayPixelAt(9, 9) == red);
	assert(displayPixelAt(9, 10) == 0);
	// Combining node opacity with per-pixel alpha must preserve both factors.
	image.style().setProperty("opacity", "0.5");
	Tree::instance().refresh(root.id(), 32, 32);
	assert(displayPixelAt(9, 9) == pixel::blendNative(red, pixel::fromRgb565(0), 128));
	assert(displayPixelAt(7, 9) == pixel::blendNative(pixel::fromRgb565(0x07e0), pixel::fromRgb565(0), 64));

	// Removing the transform returns to the existing scaled-image fast path.
	image.style().setProperty("opacity", "1");
	image.style().setProperty("transform", "none");
	Tree::instance().refresh(root.id(), 32, 32);
	assert(displayPixelAt(10, 10) == red);
	assert(displayPixelAt(9, 9) == 0);
	// A pixel pivot is absolute, even when the image is scaled. The center of
	// the same red source texel moves about (12,11), not the box center (14,12).
	image.style().setProperty("transform-origin", "2px 1px");
	image.style().setProperty("transform", "rotate(90deg)");
	Tree::instance().refresh(root.id(), 32, 32);
	assert(displayPixelAt(12, 9) == red);
	assert(displayPixelAt(10, 9) == blue);

	// Negative origins outside a box must remain valid (wheel sector labels).
	StyleSheet::instance().registerRule("outside-pivot", "transform-origin", "2px -5px");
	image.classList().set("outside-pivot");
	image.style().removeProperty("transform-origin");
	Tree::instance().refresh(root.id(), 32, 32);
	assert(displayPixelAt(6, 3) == red);
	assert(displayPixelAt(4, 3) == blue);
	assert(displayPixelAt(12, 9) == 0);

	// Changing back to percentage origins restores the established semantics.
	image.style().setProperty("transform-origin", "25% 25%");
	Tree::instance().refresh(root.id(), 32, 32);
	assert(displayPixelAt(12, 9) == red);
	gea::framework::graphics::ImageStore::instance().dispose(imageId);
	DisplayList::instance().replayDirectDirtyRegion(0, 0, 31, 31);
}
