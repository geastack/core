// SPDX-License-Identifier: Apache-2.0
// Host allocator census, including nested containers, text buffers and spare
// pool capacity. This is a repeatable fixture, not the pedal's device heap.
#include "display.h"
#include "graphics/font.h"
#include "native_test_harness.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "heap_census.h"

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated {
void ensureLinked() {}
const RasterizedFontData *lookupFontForFamily(int, int) { return nullptr; }
}
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
using gea::platform::display::Display;
struct Snapshot {
    const char *phase;
    HeapCensus heap{};
    std::uint64_t pixels = 0;
};
static Snapshot capture(const char *phase)
{
    Snapshot result{phase};
    captureHeapCensus(&result.heap);
    result.pixels = 1469598103934665603ull;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 120; ++x)
            result.pixels = (result.pixels ^ displayPixelAt(x, y)) * 1099511628211ull;
    return result;
}
int main(int argc, char **argv)
{
    const bool rounded = argc > 1 && std::strcmp(argv[1], "--rounded") == 0;
    // Keep reporting buffers out of the measured phase transitions.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    resetNativeHost();
    setNativeDisplaySize(120, 240);
    setViewportMetrics(120, 240, 1.0);
    Display::clearNoFlush();
    auto &sheet = StyleSheet::instance();
    sheet.clear();
    Tree::instance().nodeCount();
    std::array<Snapshot, 6> samples{};
    samples[0] = capture("empty-tree");
    sheet.registerRule("root", "width", "120px");
    sheet.registerRule("root", "height", "240px");
    sheet.registerRule("root", "background", "#123456");
    sheet.registerRule("root", "display", "flex");
    sheet.registerRule("root", "flex-direction", "column");
    sheet.registerRule("row", "height", "10px");
    sheet.registerRule("row", "width", "110px");
    sheet.registerRule("row", "border", "1px solid #aabbcc");
    sheet.registerRule("row", "font-size", "8px");
    if (rounded) sheet.registerRule("row", "border-radius", "3px");
    auto root = Document::instance().createView();
    root.classList().set("root");
    std::array<NodeHandle, 24> rows{}, labels{};
    for (int i = 0; i < 24; ++i) {
        rows[i] = Document::instance().createView();
        rows[i].classList().set("row");
        labels[i] = Document::instance().createText("X X");
        rows[i].appendChild(labels[i]);
        root.appendChild(rows[i]);
    }
    Document::instance().mount(root, 120, 240);
    DisplayList::instance().replay();
    assert(Tree::instance().node(rows[0].id()).style.font_size == 8);
    assert(Tree::instance().node(rows[0].id()).style.line_height == 0);
    samples[1] = capture("49-nodes-short-labels");
    for (int i = 0; i < 24; ++i) {
        char id[24];
        std::snprintf(id, sizeof(id), "row-%d", i);
        rows[i].setAttribute("id", id);
        if (i % 3 == 0) rows[i].addEventListener("click", [](gea::framework::events::PointerEvent &) {});
        rows[i].style().setProperty("background", "#223344");
        if (rounded) rows[i].style().setProperty("border-radius", "4px");
    }
    Document::instance().refresh(root, 120, 240);
    DisplayList::instance().replay();
    samples[2] = capture("attributes-listeners-inline-styles");
    char longLabel[513];
    std::memset(longLabel, 'X', sizeof(longLabel) - 1);
    longLabel[sizeof(longLabel) - 1] = 0;
    for (auto label : labels) label.setText(longLabel);
    Document::instance().refresh(root, 120, 240);
    DisplayList::instance().replay();
    samples[3] = capture("long-labels");
    for (auto label : labels) label.setText("");
    Document::instance().refresh(root, 120, 240);
    DisplayList::instance().replay();
    samples[4] = capture("cleared-labels");
    for (auto label : labels) label.setText("X X");
    Document::instance().refresh(root, 120, 240);
    DisplayList::instance().replay();
    samples[5] = capture("reused-labels");
    if (argc > 1 && !rounded) {
        FILE *out = std::fopen(argv[1], "wb");
        assert(out);
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 120; ++x) {
            const std::uint32_t color = displayPixelAt(x, y);
            std::fwrite(&color, sizeof(color), 1, out);
        }
        std::fclose(out);
        for (int i = 0; i < 7; ++i) {
            const auto &node = Tree::instance().node(i);
            std::fprintf(stderr, "node %d xywh=%d,%d,%d,%d font=%d,%d,%d line=%d indent=%d baseline=%d transform=%d white=%d text=%s\n",
                         i, node.layout.x, node.layout.y, node.layout.width, node.layout.height,
                         node.style.font_id, node.style.font_size, node.style.font_weight,
                         node.style.line_height, node.layout.inline_indent, node.render.inline_baseline,
                         node.style.text_transform, node.style.white_space, node.text.c_str());
        }
    }
    assert(Tree::instance().nodeCount() == 49);
    for (const auto &sample : samples)
        std::printf("%s,%zu,%u,%zu,%016llx\n", sample.phase, sample.heap.size_in_use,
                    sample.heap.blocks_in_use, sizeof(TreeState),
                    static_cast<unsigned long long>(sample.pixels));
}
