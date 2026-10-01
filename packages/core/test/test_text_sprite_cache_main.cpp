// Include the implementation to inspect its existing diagnostic counters without
// adding a testing API or instrumentation to production firmware.
#define GEA_EMBEDDED_TEXT_SPRITE_DIAG 1
#ifndef GEA_TEXT_CACHE_SOURCE
#define GEA_TEXT_CACHE_SOURCE "../../engine/canvas.cpp"
#endif
#include GEA_TEXT_CACHE_SOURCE
#include <array>
#include <cstdlib>

using namespace gea::framework::graphics;
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int family, int size)
{
    static const std::uint8_t atlas[] = {0, 85, 170, 255, 255, 170, 85, 0};
    static const std::uint8_t alternate[] = {255, 0, 255, 0, 0, 255, 0, 255};
    static const Glyph glyphs[] = {
        {32,0,0,0,0,3,0,0}, {48,0,0,2,2,3,0,2}, {49,2,0,2,2,3,0,2},
        {53,0,0,2,2,3,0,2}, {54,2,0,2,2,3,0,2}, {55,0,0,2,2,3,0,2},
        {56,2,0,2,2,3,0,2}, {57,0,0,2,2,3,0,2}, {58,2,0,2,2,3,0,2},
        {70,0,0,2,2,3,0,2}, {80,2,0,2,2,3,0,2}, {83,0,0,2,2,3,0,2},
    };
    static const RasterizedFontData normal{1, 48, 4, 2, -2, 12, glyphs, 4, 2, atlas};
    static const RasterizedFontData other{2, 48, 4, 2, -2, 12, glyphs, 4, 2, alternate};
    static const RasterizedFontData larger{3, 60, 6, 4, -2, 12, glyphs, 4, 2, atlas};
    return family == 2 ? &other : size == 60 ? &larger : &normal;
}
}
static void require(bool ok, const char *why)
{
    if (!ok) { std::fprintf(stderr, "FAIL text cache: %s\n", why); std::exit(1); }
}
int main()
{
    std::array<pixel::native_t, 80 * 16> pixels{}, reference{};
    Canvas canvas;
    canvas.bindPixels(pixels.data(), 80, 16);
    auto draw = [&](const char *text, bool cache, int family = 1, int size = 48, int alpha = 255, bool clip = false) {
        Canvas::setTextRasterCacheEnabled(cache);
        canvas.resetClip(); canvas.setGlobalAlpha(255); canvas.clear(0x18e3);
        canvas.setGlobalAlpha(alpha);
        if (clip) canvas.pushClip(4, 1, 12, 3);
        canvas.drawTextFontFamily(text, 1, 1, 0xffff, family, size);
    };
    // These exact two labels collided in the old four-slot FNV mapping.
    const char *labels[] = {"FPS: 59", "FPS: 60"};
    for (int i = 0; i < 4; ++i) draw(labels[i % 2], true);
    require(gTsDiag.built == 2, "alternating 59/60 must both warm up instead of evicting each other");
    for (int i = 0; i < 120; ++i) {
        const auto before = gTsDiag;
        draw(labels[i % 2], true);
        require(gTsDiag.blit == before.blit + 1 && gTsDiag.built == before.built && gTsDiag.seen == before.seen && gTsDiag.raster == before.raster,
                "every warmed alternating label must hit, with no raster or cache rebuild");
    }
    for (auto label : labels) for (int alpha : {128, 255}) for (bool clip : {false, true}) {
        draw(label, false, 1, 48, alpha, clip); reference = pixels;
        draw(label, true, 1, 48, alpha, clip);
        require(pixels == reference, "cached text must be pixel-identical to raster text, including alpha and clipping");
    }
    // Four independent keys, including identical strings in distinct font/size keys.
    for (int i = 0; i < 2; ++i) { draw(labels[0], true, 2); draw(labels[0], true, 1, 60); }
    for (int i = 0; i < 4; ++i) {
        const int family = i == 2 ? 2 : 1, size = i == 3 ? 60 : 48;
        const char *label = i == 1 ? labels[1] : labels[0];
        draw(label, false, family, size); reference = pixels;
        auto before = gTsDiag;
        draw(label, true, family, size);
        require(gTsDiag.blit == before.blit + 1 && pixels == reference, "all four full text/font/size keys must coexist with correct output");
    }
    auto before = gTsDiag;
    draw("FPS: 58", true); draw("FPS: 58", true); draw("FPS: 58", true);
    require(gTsDiag.seen == before.seen + 1 && gTsDiag.built == before.built + 1 && gTsDiag.blit == before.blit + 1,
            "a fifth key must warm once, then hit within the same bounded cache");
    require(std::size(gTextSprites) == 4, "fix must not grow the four-slot cache");
    std::puts("PASS: text collision, zero steady-state rebuilds, bounded capacity, full cache keys, pixel parity");
}
