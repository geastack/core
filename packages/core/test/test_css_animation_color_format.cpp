#include "css/animation.h"
#include "pixel.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// A color keyframe holds the native style value parseColorStyleValue produces,
// so a color track must interpolate in the build's pixel format: RGB565 on
// 16-bit boards, RGBA8888/ARGB8888 on macOS and iOS. Built once per format.
namespace {

using namespace gea::css;
using gea::embedded::ui::Property;
namespace pixel = gea::framework::graphics::pixel;

struct Rgba {
	int r, g, b, a;
};

int styleValue(int r, int g, int b)
{
	return static_cast<int>(pixel::nativeStyleValue(r, g, b));
}

Rgba unpack(int value)
{
	Rgba c{};
#if GEA_PIXEL_FORMAT_IS_8888
	pixel::unpackNative8(static_cast<pixel::native_t>(static_cast<std::uint32_t>(value)), &c.r, &c.g, &c.b, &c.a);
#else
	int r5 = 0, g6 = 0, b5 = 0;
	pixel::unpackRgb565(static_cast<std::uint16_t>(value), &r5, &g6, &b5);
	c = {(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2), 255};
#endif
	return c;
}

bool near(int actual, int expected)
{
	// RGB565 keeps 5 or 6 bits per channel.
	const int tolerance = GEA_PIXEL_FORMAT_IS_8888 ? 1 : 8;
	return std::abs(actual - expected) <= tolerance;
}

}  // namespace

int main()
{
	const Rgba from{245, 158, 11, 255};
	const Rgba to{37, 99, 235, 255};
	const auto track = Animation::transition(1, Property::BackgroundColor, styleValue(from.r, from.g, from.b),
	                                         styleValue(to.r, to.g, to.b), 1000, Easing::linear());
	bool ok = true;
	for (const double p : {0.0, 0.25, 0.5, 0.75, 1.0}) {
		const Rgba c = unpack(static_cast<int>(std::llround(sampleTrack(track, p))));
		const Rgba e{static_cast<int>(std::lround(from.r + (to.r - from.r) * p)),
		             static_cast<int>(std::lround(from.g + (to.g - from.g) * p)),
		             static_cast<int>(std::lround(from.b + (to.b - from.b) * p)), 255};
		if (near(c.r, e.r) && near(c.g, e.g) && near(c.b, e.b) && c.a == 255) continue;
		std::fprintf(stderr, "[test_css_animation_color_format] format %d p=%.2f expected %d,%d,%d,%d got %d,%d,%d,%d\n",
		             GEA_EMBEDDED_PIXEL_FORMAT, p, e.r, e.g, e.b, e.a, c.r, c.g, c.b, c.a);
		ok = false;
	}
	if (!ok) return 1;
	std::printf("css animation color format %d test passed\n", GEA_EMBEDDED_PIXEL_FORMAT);
	return 0;
}
