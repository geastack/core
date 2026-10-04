#include "native_test_harness.h"
#include "canvas.h"
#include "display.h"
#include "image.h"
#include "ui/canvas_element.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/refresh_perf.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <vector>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }

using namespace gea::embedded::test;
using namespace gea::embedded::ui;
using gea::platform::display::Display;
namespace pixel = gea::framework::graphics::pixel;

// `split` 1: two side-by-side native images cover the root only together.
// `split` 2: the same with a 1px gap, so the root must still draw.
static int checkOverlay(bool nativeImage, int split = 0)
{
	constexpr int width = 410, height = 502;
	resetNativeHost();
	setNativeDisplaySize(width, height);
	setViewportMetrics(width, height, 1.0);
	StyleSheet::instance().clear();
	auto &document = Document::instance();
	auto root = document.createView();
	root.style().width(width); root.style().height(height);
	root.style().setProperty("overflow", "hidden");
	root.style().backgroundColor(pixel::nativeColor(16, 24, 39));
	NodeHandle video = nativeImage ? NodeHandle(document.createImage().id()) : NodeHandle(document.createCanvas().id());
	video.style().position(1); video.style().left(0); video.style().top(0);
	const int leftWidth = split ? width / 2 : width;
	video.style().width(leftWidth); video.style().height(height);
	root.appendChild(video);
	NodeHandle second = split ? NodeHandle(document.createImage().id()) : NodeHandle(-1);
	const int secondLeft = leftWidth + (split == 2 ? 1 : 0);
	if (split) {
		second.style().position(1); second.style().left(secondLeft); second.style().top(0);
		second.style().width(width - secondLeft); second.style().height(height);
		root.appendChild(second);
	}
	auto button = document.createView();
	button.style().position(1); button.style().left(80); button.style().top(434);
	button.style().width(250); button.style().height(50);
	button.style().setProperty("border-radius", "18px");
	button.style().backgroundColor(pixel::nativeColor(220, 81, 74));
	root.appendChild(button);
	document.mount(root, width, height);
	auto context = CanvasElement(video.id()).getContext2D();
	std::vector<pixel::native_t> imagePixels(width * height);
	auto &images = gea::framework::graphics::ImageStore::instance();
	int previousImage = -1;
	int previousSecond = -1;
	std::vector<pixel::native_t> secondPixels(width * height);
	std::vector<std::uint16_t> incremental(width * height);
	for (int frame = 0; frame < 12; ++frame) {
		int imageId = -1;
		const auto color = pixel::nativeColor(30 + frame * 10, 80, 200);
		if (nativeImage) {
			std::fill(imagePixels.begin(), imagePixels.end(), color);
			imageId = images.registerBuffer(imagePixels.data(), leftWidth, height, -1, false);
			ImageElement(video.id()).imageId(imageId);
		} else {
			context.setFillStyleRgb565(color);
			context.fillRect(0, 0, width, height);
		}
		int secondId = -1;
		if (split) {
			std::fill(secondPixels.begin(), secondPixels.end(), pixel::nativeColor(200, 120 + frame * 5, 40));
			secondId = images.registerBuffer(secondPixels.data(), width - secondLeft, height, -1, false);
			ImageElement(second.id()).imageId(secondId);
		}
		// Opacity must prevent culling the root underneath the image.
		video.style().setProperty("opacity", frame >= 8 ? "0.5" : "1");
		if (frame % 3 == 1)
			button.style().backgroundColor(pixel::nativeColor(220, 81 + frame, 74));
		refreshPerfStatsReset();
		document.refresh(root, width, height);
		const auto perf = refreshPerfStatsRead();
		if (split == 2 && frame > 1 && frame < 8 && frame % 3 != 1 && perf.treeReplayFillRectCommands == 0) {
			std::fprintf(stderr, "FAIL gap frame=%d culled the background a 1px gap shows\n", frame);
			return 1;
		}
		if (split != 2 && frame > 1 && frame < 8 && frame % 3 != 1 && perf.treeReplayFillRectCommands != 0) {
			std::fprintf(stderr, "FAIL %s frame=%d replayed %d covered background fills\n",
			             nativeImage ? "image" : "canvas", frame, perf.treeReplayFillRectCommands);
			return 1;
		}
		for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
			incremental[y * width + x] = displayPixelAt(x, y);
		Display::resetClip(); Display::setAlpha(255); Display::clearNoFlush();
		DisplayList::instance().replay();
		int different = 0, firstX = -1, firstY = -1;
		for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
			if (incremental[y * width + x] != displayPixelAt(x, y)) {
				if (!different) { firstX = x; firstY = y; }
				++different;
			}
		}
		if (different) {
			std::fprintf(stderr, "FAIL frame=%d different=%d first=(%d,%d) incremental=%04x full=%04x\n",
			             frame, different, firstX, firstY, incremental[firstY * width + firstX], displayPixelAt(firstX, firstY));
			return 1;
		}
		// Fused replay writes each DMA strip from scratch. Poison the staging
		// pixels so skipping a background/overlay cannot pass using old pixels.
		auto *canvas = Display::canvas();
		auto *framebuffer = canvas->pixels();
		std::vector<pixel::native_t> strip(width * 16);
		for (int y0 = 0; y0 < height; y0 += 16) {
			const int rows = std::min(16, height - y0);
			std::fill(strip.begin(), strip.end(), pixel::nativeColor(0, 255, 0));
			canvas->bindPixels(strip.data() - y0 * width, width, height, width);
			Display::resetClip(); Display::setAlpha(255);
			Display::pushClip(0, y0, width, rows);
			if (DisplayList::instance().canReplaySimpleDirtyRegions(width, height)) {
				DisplayList::instance().replaySimpleClippedDirtyRegion(0, y0, width - 1, y0 + rows - 1, -1);
			} else {
				const DisplayReplayRegion region{0, y0, width - 1, y0 + rows - 1, -1};
				DisplayList::instance().replayDirectDirtyRegions(&region, 1);
			}
			Display::popClip();
			for (int y = 0; y < rows; ++y) for (int x = 0; x < width; ++x) {
				if (strip[y * width + x] != incremental[(y0 + y) * width + x]) {
					std::fprintf(stderr, "FAIL %s frame=%d strip pixel=(%d,%d)\n", nativeImage ? "image" : "canvas", frame, x, y0 + y);
					canvas->bindPixels(framebuffer, width, height, width);
					return 1;
				}
			}
		}
		canvas->bindPixels(framebuffer, width, height, width);
		Display::canvas()->resetDirty();
		if (previousImage >= 0) images.dispose(previousImage);
		previousImage = imageId;
		if (previousSecond >= 0) images.dispose(previousSecond);
		previousSecond = secondId;
	}
	if (previousImage >= 0) images.dispose(previousImage);
	if (previousSecond >= 0) images.dispose(previousSecond);
	std::printf("PASS: %s updates preserve overlapping rounded button pixels, including 16-row strips; %s\n",
	            split == 1 ? "side-by-side images" : split == 2 ? "side-by-side images with a gap" : nativeImage ? "native image" : "canvas",
	            split == 2 ? "background kept under the gap" : "opaque backgrounds culled");
	return 0;
}

int main() { return checkOverlay(false) || checkOverlay(true) || checkOverlay(true, 1) || checkOverlay(true, 2); }
