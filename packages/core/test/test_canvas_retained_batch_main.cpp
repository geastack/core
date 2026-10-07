#include "canvas.h"
#include "display.h"
#include "display_present.h"
#include "image.h"
#include "native_test_harness.h"
#include "ui/canvas_element.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace gea::framework::app::generated
{
void drainMicrotasks() {}
} // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated
{
void __attribute__((weak)) ensureLinked() {}
} // namespace gea::framework::graphics::generated

using namespace gea::embedded::test;
using namespace gea::embedded::ui;
namespace pixel = gea::framework::graphics::pixel;
using Pixels = std::array<pixel::native_t, 64 * 64>;

struct Scene
{
	NodeHandle root;
	NodeHandle node;
	CanvasRenderingContext2D context;
	bool owned;
};

static Scene scene(bool owned)
{
	resetNativeHost();
	setNativeDisplaySize(64, 64);
	setViewportMetrics(64, 64, 1);
	auto &document = Document::instance();
	auto root = document.createView();
	root.style().width(64);
	root.style().height(64);
	root.style().backgroundColor(pixel::nativeColor(12, 65, 123));
	auto parent = document.createView();
	parent.style().position(1);
	parent.style().left(6);
	parent.style().top(5);
	parent.style().width(31);
	parent.style().height(35);
	parent.style().setProperty("overflow", "hidden");
	root.appendChild(parent);
	auto node = document.createCanvas();
	node.style().position(1);
	node.style().left(3);
	node.style().top(4);
	node.style().width(36);
	node.style().height(40);
	parent.appendChild(node);
	auto overlay = document.createView();
	overlay.style().position(1);
	overlay.style().left(18);
	overlay.style().top(17);
	overlay.style().width(8);
	overlay.style().height(7);
	overlay.style().backgroundColor(pixel::nativeColor(220, 42, 17));
	overlay.style().setProperty("opacity", "0.5");
	root.appendChild(overlay);
	document.mount(root, 64, 64);
	return {root, node, CanvasRenderingContext2D(node.id()), owned};
}

static void begin(Scene &s)
{
	s.context.beginBatch();
	if (s.owned)
	{
		auto state = CanvasRenderingContext2D::stateFor(s.node.id());
		state->presentRecording_ = false;
		state->batchCanvas_ = Tree::instance().ensureCanvas(s.node.id());
	}
}

static void geometry(Scene &s, int frame)
{
	begin(s);
	s.context.setGlobalAlpha(1);
	s.context.setFillStyleRgb565(pixel::nativeColor(16 + frame * 18, 32, 8));
	s.context.fillRect(0, 0, 36, 40);
	s.context.fillTriangleRgb565(-6, 6, 25, 8, 19, 31, pixel::nativeColor(9, 198, 73));
	const std::vector<float> x0{8, 25}, y0{0, 3}, x1{20, -5}, y1{12, 31}, x2{4, 12}, y2{34, 22};
	const std::vector<std::uint32_t> packedColors{0x701dc9ffu, 0xf49222ffu};
	const std::vector<std::uint16_t> order{0, 1};
	s.context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, packedColors, order, 2);
	s.context.setGlobalAlpha(0.5);
	s.context.setFillStyleRgb565(pixel::nativeColor(241, 230, 120));
	s.context.fillCircle(23, 20, 11);
	s.context.setGlobalAlpha(1);
	const std::vector<std::uint16_t> xs{17, 18}, ys{31, 28};
	const std::vector<pixel::native_t> colors{pixel::nativeColor(22, 93, 231),
											  pixel::nativeColor(200, 22, 93)};
	s.context.setGlobalAlpha(0.5);
	s.context.fillCirclesRgb565(xs, ys, 4, colors);
	s.context.setGlobalAlpha(1);
	s.context.endBatch();
	assert(CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()) == !s.owned);
}

static Pixels capture(Scene &s)
{
	Document::instance().refresh(s.root, 64, 64);
	Pixels pixels;
	for (int y = 0; y < 64; ++y)
		for (int x = 0; x < 64; ++x)
			pixels[y * 64 + x] = displayPixelAt(x, y);
	// Every incremental result must equal a complete repaint, not merely the
	// other incremental implementation (which could share the same defect).
	auto *canvas = gea::platform::display::Display::canvas();
	auto *displayPixels = canvas->pixels();
	std::array<std::uint16_t, 64 * 64> full;
	assert(renderRetainedSnapshotRgb565(full.data(), 64, 64));
	for (int i = 0; i < 64 * 64; ++i)
		assert(full[i] == pixel::toRgb565(pixels[i]));
	canvas->bindPixels(displayPixels, 64, 64);
	return pixels;
}

static std::vector<Pixels> exercise(bool owned)
{
	auto s = scene(owned);
	std::vector<Pixels> result;
	geometry(s, 0);
	result.push_back(capture(s));
	geometry(s, 1);
	result.push_back(capture(s));

	// The retained list also reproduces snapshots and clips each poisoned strip.
	std::array<std::uint16_t, 64 * 64> snapshot;
	assert(renderRetainedSnapshotRgb565(snapshot.data(), 64, 64));
	for (int i = 0; i < 64 * 64; ++i)
		assert(snapshot[i] == pixel::toRgb565(result.back()[i]));
	setNativeDisplaySize(64, 64); // fixture snapshot rebind hook is a stub
	const auto poison = pixel::nativeColor(255, 0, 255);
	Pixels strip;
	for (int row = 0; row < 64; row += 16)
	{
		strip.fill(poison);
		auto *canvas = gea::platform::display::Display::canvas();
		canvas->bindPixels(strip.data(), 64, row + 16, 64);
		DisplayList::instance().replayDirectDirtyRegion(7, row, 39, row + 15);
		for (int y = 0; y < 64; ++y)
			for (int x = 0; x < 64; ++x)
				assert(strip[y * 64 + x] == (y >= row && y < row + 16 && x >= 7 && x <= 39
												 ? result.back()[y * 64 + x]
												 : poison));
	}
	setNativeDisplaySize(64, 64);
	DisplayList::instance().replayDirectDirtyRegion(0, 0, 63, 63);

	// Incremental batches preserve the previous complete frame, then fall back.
	begin(s);
	s.context.setFillStyleRgb565(pixel::nativeColor(255, 255, 255));
	s.context.fillRect(1, 1, 4, 5);
	s.context.endBatch();
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	result.push_back(capture(s));

	begin(s);
	s.context.setGlobalAlpha(0.5);
	s.context.setFillStyleRgb565(pixel::nativeColor(100, 50, 190));
	s.context.fillRect(0, 0, 36, 40);
	s.context.endBatch();
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	result.push_back(capture(s));

	geometry(s, 2);
	// A draw outside a batch must materialize before applying its mutation.
	s.context.setFillStyleRgb565(pixel::nativeColor(99, 20, 160));
	s.context.fillRect(4, 4, 6, 8);
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	result.push_back(capture(s));

	geometry(s, 3);
	// Unsupported path drawing materializes prior AND currently recorded draws.
	begin(s);
	s.context.setFillStyleRgb565(pixel::nativeColor(255, 255, 255));
	s.context.fillRect(2, 2, 6, 5);
	s.context.beginPath();
	s.context.arc(16, 17, 5, 0, 6.283);
	s.context.fill();
	s.context.endBatch();
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	result.push_back(capture(s));

	geometry(s, 4);
	s.node.style().setProperty("opacity", "0.5");
	result.push_back(capture(s));
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	s.node.style().setProperty("opacity", "1");
	capture(s);

	geometry(s, 5);
	begin(s);
	s.context.setFillStyleRgb565(pixel::nativeColor(255, 255, 255));
	s.context.fillRect(0, 0, 36, 40);
	std::array<pixel::native_t, 4> imagePixels{
		pixel::nativeColor(255, 0, 0), pixel::nativeColor(0, 255, 0), pixel::nativeColor(0, 0, 255),
		pixel::nativeColor(255, 255, 0)};
	auto &images = gea::framework::graphics::ImageStore::instance();
	const int imageId = images.registerBuffer(imagePixels.data(), 2, 2, -1, false);
	s.context.drawImage(imageId, 2, 3);
	s.context.endBatch();
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	images.dispose(imageId);
	imagePixels.fill(0);
	result.push_back(capture(s)); // image lifetime ends after the synchronous draw

	geometry(s, 6);
	s.node.style().setProperty("transform", "rotate(15deg)");
	result.push_back(capture(s));
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	s.node.style().removeProperty("transform");
	capture(s);

	geometry(s, 7);
	s.node.style().width(30);
	result.push_back(capture(s));
	assert(!CanvasRenderingContext2D::hasRetainedCanvas(s.node.id()));
	return result;
}

static void tinyGeometry(Scene &s, int x, int background = 31)
{
	begin(s);
	s.context.setGlobalAlpha(1);
	s.context.setFillStyleRgb565(pixel::nativeColor(background, 21, 40));
	s.context.fillRect(0, 0, 36, 40);
	const std::vector<float> x0{float(x - 2)}, y0{10}, x1{float(x + 3)}, y1{12}, x2{float(x)},
		y2{19};
	const std::vector<std::uint32_t> colors{0xf19a53ffu};
	const std::vector<std::uint16_t> order{0};
	s.context.fillTrianglesRgb565Sorted(x0, y0, x1, y1, x2, y2, colors, order, 1);
	s.context.setGlobalAlpha(0.5);
	const std::vector<std::uint16_t> xs{std::uint16_t(x), std::uint16_t(x + 2)}, ys{15, 17};
	const std::vector<pixel::native_t> circleColors{pixel::nativeColor(30, 240, 20),
													pixel::nativeColor(80, 20, 220)};
	s.context.fillCirclesRgb565(xs, ys, 3, circleColors);
	s.context.setGlobalAlpha(1);
	s.context.endBatch();
}

static std::vector<Pixels> damageExercise(bool owned)
{
	auto s = scene(owned);
	std::vector<Pixels> result;
	tinyGeometry(s, 6);
	result.push_back(capture(s));
	gea::platform::display::Display::flushStatsReset();
	tinyGeometry(s, 9);
	int x0, y0, x1, y1;
	if (!owned)
	{
		assert(CanvasRenderingContext2D::retainedCanvasDamage(s.node.id(), x0, y0, x1, y1));
		assert(x1 - x0 < 24 && y1 - y0 < 24);
		// A snapshot sees the committed frame without consuming display damage.
		auto *canvas = gea::platform::display::Display::canvas();
		auto *displayPixels = canvas->pixels();
		std::array<std::uint16_t, 64 * 64> snapshot;
		assert(renderRetainedSnapshotRgb565(snapshot.data(), 64, 64));
		int sx0, sy0, sx1, sy1;
		assert(CanvasRenderingContext2D::retainedCanvasDamage(s.node.id(), sx0, sy0, sx1, sy1));
		assert(x0 == sx0 && y0 == sy0 && x1 == sx1 && y1 == sy1);
		// Restore the fixture canvas binding without clearing the displayed pixels.
		canvas->bindPixels(displayPixels, 64, 64);
	}
	// Two further batches arrive before the document frame. The final identical
	// batch must preserve the earlier movement's old+new dirty union.
	tinyGeometry(s, 21);
	tinyGeometry(s, 21);
	if (!owned)
	{
		assert(CanvasRenderingContext2D::retainedCanvasDamage(s.node.id(), x0, y0, x1, y1));
		assert(x0 <= 3 && x1 >= 24);
		// A controller may expand the local damage window by one pixel. Replay
		// the resulting fringes into poisoned 16-row strips in normal painter
		// order.
		auto *canvas = gea::platform::display::Display::canvas();
		auto *displayPixels = canvas->pixels();
		std::array<std::uint16_t, 64 * 64> reference;
		assert(renderRetainedSnapshotRgb565(reference.data(), 64, 64));
		const auto poison = pixel::nativeColor(255, 0, 255);
		Pixels strip;
		const auto &node = Tree::instance().node(s.node.id());
		const int left = std::max(0, node.layout.x + x0 - 1),
				  right = std::min(63, node.layout.x + x1 + 1);
		const int top = std::max(0, node.layout.y + y0 - 1),
				  bottom = std::min(63, node.layout.y + y1 + 1);
		for (int row = top; row <= bottom; row += 16)
		{
			strip.fill(poison);
			const int end = std::min(bottom, row + 15);
			canvas->bindPixels(strip.data(), 64, end + 1, 64);
			DisplayList::instance().replayDirectDirtyRegion(left, row, right, end);
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < 64; ++x)
				{
					const auto expected = x >= left && x <= right && y >= row && y <= end
											  ? pixel::fromRgb565(reference[y * 64 + x])
											  : poison;
					assert(strip[y * 64 + x] == expected);
				}
		}
		canvas->bindPixels(displayPixels, 64, 64);
	}
	result.push_back(capture(s));
	if (!owned)
	{
		assert(flushPixelCount() < 36 * 40);
		if (std::getenv("GEA_CANVAS_BENCHMARK"))
			std::printf("retained multi-batch pending damage: flushed=%dpx canvas=%dpx\n",
						flushPixelCount(), 36 * 40);
		assert(!CanvasRenderingContext2D::retainedCanvasDamage(s.node.id(), x0, y0, x1, y1));
		gea::platform::display::Display::flushStatsReset();
		tinyGeometry(s, 21);
		result.push_back(capture(s));
		assert(flushPixelCount() == 0);
	}
	else
	{
		tinyGeometry(s, 21);
		result.push_back(capture(s));
	}
	// A changed opaque background covers the whole canvas, even if the small
	// foreground geometry is identical to the last frame.
	tinyGeometry(s, 21, 78);
	if (!owned)
	{
		assert(CanvasRenderingContext2D::retainedCanvasDamage(s.node.id(), x0, y0, x1, y1));
		assert(x0 == 0 && y0 == 0 && x1 == 35 && y1 == 39);
	}
	result.push_back(capture(s));

	// Pixel changes mixed with geometry changes must collect full old/new boxes.
	tinyGeometry(s, 12, 78);
	s.node.style().left(6);
	result.push_back(capture(s));
	tinyGeometry(s, 14, 78);
	s.node.style().setProperty("opacity", "0.5");
	result.push_back(capture(s));
	s.node.style().setProperty("opacity", "1");
	capture(s);
	tinyGeometry(s, 11, 78);
	s.node.style().setProperty("transform", "scale(1.1)");
	result.push_back(capture(s));
	s.node.style().removeProperty("transform");
	capture(s);
	// The opposite mutation order also invalidates partial damage safely.
	s.node.style().left(3);
	tinyGeometry(s, 18, 78);
	result.push_back(capture(s));
	return result;
}

// Independent legacy scanlines catch rounding changes when all production
// triangle entry points share the optimized helper.
static void uncachedTriangle(gea::framework::graphics::Canvas &canvas, std::array<int, 6> t,
							 pixel::native_t color)
{
	if (t[1] > t[3])
	{
		std::swap(t[0], t[2]);
		std::swap(t[1], t[3]);
	}
	if (t[1] > t[5])
	{
		std::swap(t[0], t[4]);
		std::swap(t[1], t[5]);
	}
	if (t[3] > t[5])
	{
		std::swap(t[2], t[4]);
		std::swap(t[3], t[5]);
	}
	if (t[1] == t[5])
	{
		const int left = std::min({t[0], t[2], t[4]});
		canvas.fillRect(left, t[1], std::max({t[0], t[2], t[4]}) - left + 1, 1, color);
		return;
	}
	for (int y = std::max(0, t[1]); y <= std::min(canvas.height() - 1, t[5]); ++y)
	{
		const bool second = y >= t[3];
		const float alpha = float(y - t[1]) / (t[5] - t[1]);
		const float beta =
			float(y - (second ? t[3] : t[1])) / std::max(1, second ? t[5] - t[3] : t[3] - t[1]);
		int ax = t[0] + int((t[4] - t[0]) * alpha);
		int bx = second ? t[2] + int((t[4] - t[2]) * beta) : t[0] + int((t[2] - t[0]) * beta);
		if (ax > bx)
			std::swap(ax, bx);
		canvas.fillRect(ax, y, bx - ax + 1, 1, color);
	}
}

static void concurrentTriangleRatios()
{
	std::atomic<int> ready{0};
	std::atomic<bool> start{false};
	std::array<std::thread, 4> workers;
	for (int worker = 0; worker < 4; ++worker)
	{
		workers[worker] = std::thread(
			[&, worker]
			{
				ready.fetch_add(1);
				while (!start.load())
					std::this_thread::yield();
				unsigned seed = 1 + worker;
				for (int test = 0; test < 256; ++test)
				{
					std::array<int, 6> triangle;
					for (auto &coordinate : triangle)
					{
						seed = seed * 1664525u + 1013904223u;
						coordinate = int(seed % 401) - 160;
					}
					// Cache boundary, oversized fallback, and flat degeneracy.
					if (test < 4)
						triangle = {-31, -20, 51, 21, 16, -20 + test + 127};
					if (test == 4)
						triangle = {-17, 21, 58, 21, 7, 21};
					Pixels expected{}, actual{};
					gea::framework::graphics::Canvas reference, cached;
					reference.bindPixels(expected.data(), 64, 64);
					cached.bindPixels(actual.data(), 64, 64);
					reference.pushClip(2, 7, 31, 24);
					cached.pushClip(2, 7, 31, 24);
					const auto color = pixel::nativeColor(215, 48, 123);
					uncachedTriangle(reference, triangle, color);
					cached.fillTriangleOpaque(triangle[0], triangle[1], triangle[2], triangle[3],
											  triangle[4], triangle[5], color);
					assert(expected == actual);
				}
			});
	}
	while (ready.load() != int(workers.size()))
		std::this_thread::yield();
	start.store(true);
	for (auto &worker : workers)
		worker.join();
}

static void triangleEdges()
{
	const std::array<std::array<int, 6>, 6> triangles{{{-6, 6, 25, 8, 19, 31},
													   {22, -9, -14, 8, 3, 45},
													   {-12, -4, 46, 2, 9, 37},
													   {2, 8, 12, 8, 31, 8},
													   {5, 5, 5, 5, 5, 5},
													   {23, 35, -4, 13, 9, 3}}};
	for (const auto &t : triangles)
	{
		Pixels normal{}, opaque{}, occluded{};
		gea::framework::graphics::Canvas a, b, c;
		a.bindPixels(normal.data(), 64, 64);
		b.bindPixels(opaque.data(), 64, 64);
		c.bindPixels(occluded.data(), 64, 64);
		a.pushClip(2, 7, 31, 24);
		b.pushClip(2, 7, 31, 24);
		c.pushClip(2, 7, 31, 24);
		const auto color = pixel::nativeColor(215, 48, 123);
		a.fillTriangle(t[0], t[1], t[2], t[3], t[4], t[5], color);
		b.fillTriangleOpaque(t[0], t[1], t[2], t[3], t[4], t[5], color);
		const gea::framework::graphics::TriangleEntry entry{short(t[0]),
															short(t[1]),
															short(t[2]),
															short(t[3]),
															short(t[4]),
															short(t[5]),
															color,
															short(std::min({t[1], t[3], t[5]})),
															short(std::max({t[1], t[3], t[5]}))};
		c.fillTrianglesOpaqueOccluded(&entry, 1, 0, 0);
		assert(normal == opaque);
		assert(normal == occluded);
	}
}

static void numericBatchBenchmark()
{
	using namespace gea::framework::display_present;
	using gea::framework::graphics::TriangleEntry;
	using gea::platform::display::DisplayPresentCommand;
	using gea::platform::display::DisplayPresentCommandType;
	constexpr int size = 332, stride = 334, rows = 16;
	std::array<TriangleEntry, 80> triangles{};
	for (int sector = 0; sector < 20; ++sector)
	{
		const double a = (sector * 9 + 91) * 3.141592653589793 / 180;
		const double b = (sector * 9 + 98) * 3.141592653589793 / 180;
		const float inner = 42;
		const float outer = 75 + (sector * 23) % 88;
		for (int mirror = -1; mirror <= 1; mirror += 2)
		{
			const int index = sector * 4 + (mirror == -1 ? 0 : 2);
			auto point = [&](double angle, float radius)
			{
				return std::array<short, 2>{
					short(std::round(166 + float(std::cos(angle)) * radius * mirror)),
					short(std::round(166 + float(std::sin(angle)) * radius))};
			};
			const auto ai = point(a, inner), ao = point(a, outer), bo = point(b, outer),
					   bi = point(b, inner);
			const auto color = pixel::nativeColor(100 + sector * 7, 30 + sector * 9, 80);
			triangles[index] = {ai[0], ai[1], ao[0], ao[1], bo[0], bo[1], color, 0, 0};
			triangles[index + 1] = {ai[0], ai[1], bo[0], bo[1], bi[0], bi[1], color, 0, 0};
		}
	}
	for (auto &triangle : triangles)
	{
		triangle.rowY0 = std::min({triangle.y0, triangle.y1, triangle.y2});
		triangle.rowY1 = std::max({triangle.y0, triangle.y1, triangle.y2});
	}
	int maxTriangleHeight = 0;
	for (const auto &triangle : triangles)
		maxTriangleHeight = std::max(maxTriangleHeight, int(triangle.rowY1 - triangle.rowY0));
	assert(maxTriangleHeight <= 128);
	if (std::getenv("GEA_CANVAS_BENCHMARK"))
		std::printf("80-triangle maximum denominator=%d\n", maxTriangleHeight);
	std::array<DisplayPresentCommand, 2> commands{};
	commands[0].type = DisplayPresentCommandType::FillRectRgb565;
	commands[0].fillRectRgb565 = {0, 0, size, size, pixel::nativeColor(31, 21, 40), 255};
	commands[1].type = DisplayPresentCommandType::FillTrianglesRgb565;
	commands[1].fillTrianglesRgb565 = {triangles.data(), int(triangles.size()), 255};
	Frame occluded, retained;
	assert(extractFrame(commands.data(), commands.size(), occluded));
	assert(extractFrame(commands.data(), commands.size(), retained, true));
	std::array<pixel::native_t, stride * rows> buffer;
	std::vector<pixel::native_t> expected(size * size), actual(size * size);
	const auto poison = pixel::nativeColor(255, 0, 255);
	auto render = [&](const Frame &frame, std::vector<pixel::native_t> *capture)
	{
		for (int row = 0; row < size; row += rows)
		{
			buffer.fill(poison);
			const int chunkRows = std::min(rows, size - row);
			rasterFrameRowsStrided(buffer.data(), stride, 0, size, row, chunkRows, size, frame);
			if (capture)
			{
				for (int y = 0; y < chunkRows; ++y)
				{
					assert(buffer[y * stride + size] == poison &&
						   buffer[y * stride + size + 1] == poison);
					std::copy_n(buffer.data() + y * stride, size,
								capture->data() + (row + y) * size);
				}
			}
		}
	};
	render(occluded, &expected);
	render(retained, &actual);
	assert(expected == actual);
	std::vector<pixel::native_t> reference(size * size);
	gea::framework::graphics::Canvas referenceCanvas;
	referenceCanvas.bindPixels(reference.data(), size, size);
	referenceCanvas.fillRect(0, 0, size, size, commands[0].fillRectRgb565.color);
	for (const auto &triangle : triangles)
		uncachedTriangle(
			referenceCanvas,
			{triangle.x0, triangle.y0, triangle.x1, triangle.y1, triangle.x2, triangle.y2},
			triangle.color);
	assert(reference == actual);
	if (!std::getenv("GEA_CANVAS_BENCHMARK"))
		return;
	auto measure = [&](const Frame &frame)
	{
		constexpr int iterations = 100;
		double best = 1e30;
		for (int run = 0; run < 3; ++run)
		{
			const auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < iterations; ++i)
				render(frame, nullptr);
			const auto elapsed =
				std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
					.count();
			best = std::min(best, elapsed / iterations);
		}
		return best;
	};
	const double oldUs = measure(occluded), retainedUs = measure(retained);
	std::printf("80-triangle sparse numeric batch, 332x332, 16-row strips: "
				"occluded=%.1fus/frame "
				"retained=%.1fus/frame ratio=%.2f\n",
				oldUs, retainedUs, retainedUs / oldUs);
}

int main()
{
	concurrentTriangleRatios();
	triangleEdges();
	numericBatchBenchmark();
	const auto ownedDamage = damageExercise(true);
	const auto retainedDamage = damageExercise(false);
	for (std::size_t frame = 0; frame < ownedDamage.size(); ++frame)
	{
		int count = 0;
		for (int i = 0; i < 64 * 64; ++i)
			if (ownedDamage[frame][i] != retainedDamage[frame][i])
			{
				if (count++ == 0)
					std::fprintf(stderr,
								 "damage frame=%zu first x=%d y=%d owned=%04x retained=%04x\n",
								 frame, i % 64, i / 64, unsigned(ownedDamage[frame][i]),
								 unsigned(retainedDamage[frame][i]));
			}
		if (count)
			std::fprintf(stderr, "damage frame=%zu changed=%d\n", frame, count);
	}
	assert(ownedDamage == retainedDamage);
	const auto owned = exercise(true);
	const auto retained = exercise(false);
	for (std::size_t frame = 0; frame < owned.size(); ++frame)
	{
		int changed = 0;
		for (int i = 0; i < 64 * 64; ++i)
		{
			if (owned[frame][i] != retained[frame][i])
			{
				if (changed++ == 0)
					std::fprintf(stderr,
								 "frame=%zu first mismatch x=%d y=%d owned=%04x retained=%04x\n",
								 frame, i % 64, i / 64, unsigned(owned[frame][i]),
								 unsigned(retained[frame][i]));
			}
		}
		if (changed)
			std::fprintf(stderr, "frame=%zu mismatched pixels=%d\n", frame, changed);
		assert(changed == 0);
	}
}
