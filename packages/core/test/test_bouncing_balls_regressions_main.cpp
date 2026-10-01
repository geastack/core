// Regression cases from the original Bouncing Balls JSX 60 FPS restoration.
#include "native_test_harness.h"
#include "display.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/refresh_perf.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/tree_state.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
using namespace gea::embedded::ui;
using namespace gea::embedded::test;
using gea::platform::display::Display;

static int workerSubmissions;
static std::uint64_t pixelHash = 1469598103934665603ULL;
// A real worker has its own canvas and cannot inherit a rebound DMA buffer or clip.
extern "C" bool gea_render_parallel_submit(void (*)(void *, int, int), void *, int, int)
{
    ++workerSubmissions;
    return false;
}
static void require(bool ok, const char *why)
{
    if (!ok) { std::fprintf(stderr, "FAIL balls regression: %s\n", why); std::exit(1); }
}
// Work-count assertions require instrumentation; geometry, ownership and pixel
// assertions run unchanged in both builds.
#if GEA_EMBEDDED_UI_REFRESH_PERF
#define requirePerf(...) require(__VA_ARGS__)
#else
#define requirePerf(...) do {} while (false)
#endif
static int root(int width, int height, double dpr = 1)
{
    resetNativeHost();
    setNativeDisplaySize(width, height);
    setViewportMetrics(width, height, dpr);
    int id = Tree::instance().createView();
    NodeHandle(id).style().width(width);
    NodeHandle(id).style().height(height);
    NodeHandle(id).style().backgroundColor(0);
    return id;
}
static int ball(int parent, int x, int y)
{
    int id = Tree::instance().createView();
    NodeHandle(parent).appendChild(NodeHandle(id));
    auto style = NodeHandle(id).style();
    style.position(1); style.left(x); style.top(y);
    style.width(12); style.height(12); style.backgroundColor(0xffff);
    return id;
}
static void nodeAllocationCensus()
{
    NodeText original;
    original = std::string(512, 'x');
    NodeText copied(original);
    const auto both = NodeText::storageUsage();
    NodeText moved(std::move(copied));
    require(copied.empty() && moved.size() == 512 && NodeText::storageUsage().heap == both.heap,
            "moving text ownership must not create or hide allocations");
    original.clear();
    const auto one = NodeText::storageUsage();
    require(both.allocations == one.allocations + 1 && both.payload >= one.payload + 513,
            "text census must include each independent long character buffer and release it");
    moved.clear();
    const auto empty = NodeText::storageUsage();
    require(one.allocations == empty.allocations + 1 && one.payload >= empty.payload + 513 && empty.allocations >= 3,
            "cleared text retains its pool pages and pointer table but no dead string buffer");
    require(NodeText::storageUsage().heap == empty.heap, "reading text census must not allocate");
    NodeStyleOverrideStore overrides;
    overrides.set(Property::Left, 7); overrides.set(Property::Top, 8);
    const auto numeric = overrides.storageUsage();
    require(numeric.allocations == 1 && numeric.payload > 0, "numeric overrides must count their owning block");
    overrides.setCssPixels(Property::Left, 7.5f);
    const auto css = overrides.storageUsage();
    require(css.allocations == numeric.allocations + 2 && css.payload > numeric.payload,
            "CSS pixel metadata must count both vector owner and entry allocation");
    overrides.clear();
    require(overrides.storageUsage().heap == 0 && overrides.storageUsage().allocations == 0,
            "clearing overrides must release their whole allocation chain");
    resetNativeHost();
    const int r = Tree::instance().createView();
    auto &rare = ensureRareData(r);
    const auto before = nodeAuxiliaryStorageUsage();
    rare.inlineStyles.set(Property::Left, 7);
    rare.attributes.set("data-census", "allocated");
    const auto populated = nodeAuxiliaryStorageUsage();
    require(populated.tree.payload == sizeof(TreeState) && populated.tree.allocations == 1,
            "tree census includes the owning allocation");
    require(populated.rare.allocations == before.rare.allocations + 1 &&
            populated.overrides.allocations == before.overrides.allocations + 1,
            "rare-node census must include linked attributes and override blocks");
    rare.clear();
    const auto cleared = nodeAuxiliaryStorageUsage();
    require(cleared.rare.heap == before.rare.heap && cleared.overrides.heap == before.overrides.heap,
            "clearing a rare record retains its pool storage but releases owned children");
    std::puts("PASS: node-owned census includes text, pool slack, attributes and override lifetimes");
}

static void overrideCapacityAndOwnership()
{
    NodeStyleOverrideStore small;
    small.set(Property::Left, 1); small.set(Property::Top, 2);
    const auto twoBytes = small.storageUsage().payload;
    small.set(Property::Width, 3);
    require(small.storageUsage().payload <= twoBytes + sizeof(NodeStyleOverride),
            "three overrides must not retain an unused fourth entry");
    small.set(Property::Height, 4);
    for (std::size_t i = 0; i < small.size(); ++i)
        require(small.at(i).value == static_cast<int>(i + 1), "small override growth must preserve values");
    NodeStyleOverrideStore original;
    const int count = static_cast<int>(Property::Count);
    for (int i = 0; i < count; ++i) original.set(static_cast<Property>(i), i * 3);
    original.setCssPixels(Property::Left, 7.25f);
    require(original.size() == static_cast<std::size_t>(count), "all distinct properties must survive override growth");
    NodeStyleOverrideStore copy(original);
    original.set(Property::Left, 999);
    float pixels = 0;
    require(copy.getCssPixels(Property::Left, pixels) && pixels == 7.25f && !original.getCssPixels(Property::Left, pixels),
            "copied CSS unit metadata must remain independently owned after growth");
    for (int i = 0; i < count; ++i)
        require(copy.at(i).property == static_cast<Property>(i) && copy.at(i).value == i * 3,
                "narrow override counts must preserve every property and value");
    for (int i = 0; i < count; i += 2) require(copy.remove(static_cast<Property>(i)), "removing alternating override entries must succeed");
    NodeStyleOverrideStore moved(std::move(copy));
    require(copy.empty() && moved.size() == static_cast<std::size_t>(count / 2), "moving a grown override block must transfer its count");
    for (int i = 1; i < count; i += 2)
        require(moved.at(i / 2).property == static_cast<Property>(i) && moved.at(i / 2).value == i * 3,
                "compaction must preserve remaining override order and values");
    std::puts("PASS: compact override headers retain every Property through growth, copy, erase and move");
}

static void classDependencyStorage()
{
    auto &sheet = StyleSheet::instance();
    auto &tree = Tree::instance();
    int r = root(120, 80);
    int child = tree.createView(); NodeHandle(r).appendChild(NodeHandle(child));
    sheet.registerRule("small", "font-size", "12px");
    sheet.registerRule("large", "font-size", "18px");
    sheet.registerRule("narrow", "width", "20px");
    sheet.registerRule("wide", "width", "30px");
    sheet.setClassName(NodeHandle(r), "small"); sheet.setClassName(NodeHandle(child), "narrow");
    tree.mount(r, 120, 80);
    require(tree.node(child).computedStyle().font_size == 12 && tree.node(child).layout.width == 20,
            "initial classes and inherited font must apply with pruned dependency records");
    beginStyleMountBatch(); sheet.setClassName(NodeHandle(r), "large"); endStyleMountBatch();
    tree.refresh(r, 120, 80);
    require(tree.node(child).computedStyle().font_size == 18 && tree.node(child).layout.width == 20,
            "pruned dependency records must still propagate inherited class changes");
    beginStyleMountBatch(); sheet.setClassName(NodeHandle(child), "wide"); endStyleMountBatch();
    tree.refresh(r, 120, 80);
    require(tree.node(child).layout.width == 30, "pruned dependency records must still recompute directly changed classes");
#if GEA_CSS_CUSTOM_PROPERTIES
    r = root(120, 80); child = tree.createView(); NodeHandle(r).appendChild(NodeHandle(child));
    sheet.registerRule("theme-a", "--edge", "21px");
    sheet.registerRule("theme-b", "--edge", "31px");
    sheet.registerRule("uses-var", "width", "var(--edge, 11px)");
    sheet.setClassName(NodeHandle(r), "theme-a"); sheet.setClassName(NodeHandle(child), "uses-var");
    tree.mount(r, 120, 80);
    require(tree.node(child).layout.width == 21, "enabled variables must inherit from ancestor classes");
    beginStyleMountBatch(); sheet.setClassName(NodeHandle(r), "theme-b"); endStyleMountBatch(); tree.refresh(r, 120, 80);
    require(tree.node(child).layout.width == 31, "enabled dependency tracking must invalidate changed inherited variables");
    NodeHandle(r).style().setProperty("--edge", "41px"); tree.refresh(r, 120, 80);
    require(tree.node(child).layout.width == 41, "inline custom properties must override class values");
    require(NodeHandle(r).style().removeProperty("--edge"), "inline variable removal must succeed"); tree.refresh(r, 120, 80);
    require(tree.node(child).layout.width == 31, "variable removal must restore the inherited class value");
    sheet.setClassName(NodeHandle(r), ""); tree.refresh(r, 120, 80);
    require(tree.node(child).layout.width == 11, "missing variables must resolve their authored fallback");
    require(styleDependencyStorageUsage().staticBytes > static_cast<std::size_t>(kMaxNodes), "enabled variable tracking must be included in the census");
#else
    static_assert(std::is_empty_v<NodeCustomPropertyStore>);
#if GEA_EMBEDDED_SHARED_STYLES
    require(tree.node(child).class_style_tracked, "class recomputation must set its inline tracking flag");
    tree.removeNode(child);
    const int reused = tree.createView();
    require(reused == child && !tree.node(reused).class_style_tracked,
            "a reused node slot must not inherit its previous occupant's dependency flag");
    static_assert(offsetof(Node, class_style_tracked) == offsetof(Node, type) + sizeof(NodeType));
    static_assert(offsetof(Node, render) == (offsetof(Node, type) + sizeof(NodeType) + alignof(RenderState) - 1) / alignof(RenderState) * alignof(RenderState));
#endif
    require(styleDependencyStorageUsage().staticBytes == (GEA_EMBEDDED_SHARED_STYLES ? 0u : static_cast<std::size_t>(kMaxNodes)) &&
            styleDependencyStorageUsage().allocations == 0,
            "unused variable tracking must occupy node padding or one direct byte per inline node");
#endif
    std::printf("DEPENDENCIES custom_properties=%d static=%zu rare_record=%zu\n", GEA_CSS_CUSTOM_PROPERTIES,
                styleDependencyStorageUsage().staticBytes, sizeof(NodeRareData));
    resetNativeHost();
}

// Cache keys must stay distinct beyond both inline cache entry capacities,
// then remain correct when those keys are revisited in a different order.
static void classRuleCacheReuse()
{
    const int r = root(512, 512);
    auto &tree = Tree::instance();
    auto &sheet = StyleSheet::instance();
    std::array<int, 32> ids;
    for (int i = 0; i < 32; ++i) {
        const std::string name = "cache" + std::to_string(i);
        // More than 24 matching declarations exercises candidate/plan spill
        // before shadowed declarations collapse to the winning width.
        for (int width = 1; width <= 32; ++width)
            sheet.registerRule(name, "width", std::to_string(40 + i + width) + "px");
        sheet.registerRule(name, "height", std::to_string(10 + i) + "px");
        ids[i] = tree.createView(); NodeHandle(r).appendChild(NodeHandle(ids[i]));
        sheet.setClassName(NodeHandle(ids[i]), name);
    }
    tree.mount(r, 512, 512);
    for (int round = 0; round < 5; ++round) {
        beginStyleMountBatch();
        for (int i = 0; i < 32; ++i)
            sheet.setClassName(NodeHandle(ids[i]), "cache" + std::to_string((31 - i + round) % 32));
        endStyleMountBatch();
        tree.refresh(r, 512, 512);
        for (int i = 0; i < 32; ++i) {
            const int key = (31 - i + round) % 32;
            const auto &style = tree.node(ids[i]).computedStyle();
            require(style.width == 72 + key && style.height == 10 + key,
                    "class cache reuse and spills must preserve each key and its last declaration");
        }
    }
#if GEA_UI_CLASS_OVERFLOW
    for (int i = 0; i < 7; ++i)
        sheet.registerRule("multi" + std::to_string(i), "width", std::to_string(100 + i) + "px");
    for (const auto &classes : {"multi0 multi1 multi2 multi3 multi4 multi5", "multi5 multi4 multi3 multi2 multi1 multi0",
                               "multi0 multi1 multi2 multi3 multi4 multi5 multi6", "multi6 multi0 multi1 multi2 multi3 multi4 multi5"}) {
        sheet.setClassName(NodeHandle(ids[0]), classes);
        tree.refresh(r, 512, 512);
        require(tree.node(ids[0]).computedStyle().width == (std::strstr(classes, "multi6") ? 106 : 105),
                "six-class cached and seven-class uncached rules must preserve stylesheet order");
    }
#endif
#if GEA_CSS_PSEUDO_ELEMENTS
    const int panel = tree.createView(); NodeHandle(r).appendChild(NodeHandle(panel));
    for (const char *side : {"before", "after"}) {
        const std::string selector = std::string(".pseudo-cache::") + side;
        sheet.registerSelectorRule(selector, "content", "\"\"");
        sheet.registerSelectorRule(selector, "width", side[0] == 'b' ? "17px" : "23px");
        sheet.registerSelectorRule(selector, "height", "5px");
    }
    sheet.setClassName(NodeHandle(panel), "pseudo-cache");
    tree.refresh(r, 512, 512);
    int seen = 0;
    for (int child = tree.node(panel).first_child; child >= 0; child = tree.node(child).next_sibling) {
        const char *tag = tree.tagName(child);
        if (std::strcmp(tag, "::before") == 0 || std::strcmp(tag, "::after") == 0) {
            require(tree.node(child).computedStyle().width == (tag[2] == 'b' ? 17 : 23),
                    "ordinary pseudo-element buckets must work with or without variable buckets");
            ++seen;
        }
    }
    require(seen == 2, "both ordinary pseudo-element buckets must materialize");
#endif
    resetNativeHost();
    std::puts("PASS: class cache spills, key reuse, class-count fallback and pseudo-rule buckets");
}

static void dimensionPercentageStorage()
{
    const int r = root(200, 100), id = ball(r, 0, 0);
    auto &tree = Tree::instance();
    const auto set = [&](const char *name, const char *value) { NodeHandle(id).style().setProperty(name, value); };
    require(tree.node(id).computedStyle().width_percent == kUnset && tree.node(id).computedStyle().height_percent == kUnset,
            "percentage-free dimensions must begin unset");
#if GEA_CSS_WIDTH_EXPRESSIONS && GEA_CSS_HEIGHT_EXPRESSIONS
    set("width", "25vw"); set("height", "10vh"); tree.mount(r, 200, 100);
    require(tree.node(id).layout.width == 50 && tree.node(id).layout.height == 10,
            "viewport units must preserve geometry without percentage fields");
    set("font-size", "20px"); set("width", "2em"); set("height", "3em"); tree.refresh(r, 200, 100);
    require(tree.node(id).layout.width == 40 && tree.node(id).layout.height == 60,
            "font-relative lengths must preserve geometry without percentage fields");
#endif
    set("width", "30px"); set("height", "40px"); tree.mount(r, 200, 100);
    require(tree.node(id).layout.width == 30 && tree.node(id).layout.height == 40,
            "fixed pixel setters must clear previous relative expressions");
#if GEA_CSS_WIDTH_PERCENT
    refreshPerfStatsReset();
    set("width", "50%"); tree.refresh(r, 200, 100);
    requirePerf(refreshPerfStatsRead().treeAbsModeFull > 0, "percentage size changes must honor structural layout invalidation");
    require(tree.node(id).layout.width == 100, "enabled percentage width must resolve against its containing block");
    set("width", "30px"); tree.refresh(r, 200, 100);
    require(tree.node(id).layout.width == 30 && tree.node(id).computedStyle().width_percent == kUnset,
            "pixel width must clear a previous percentage");
#endif
#if GEA_CSS_HEIGHT_PERCENT
    set("height", "25%"); tree.refresh(r, 200, 100);
    require(tree.node(id).layout.height == 25, "enabled percentage height must resolve against its containing block");
    set("height", "40px"); tree.refresh(r, 200, 100);
    require(tree.node(id).layout.height == 40 && tree.node(id).computedStyle().height_percent == kUnset,
            "pixel height must clear a previous percentage");
#endif
    set("width", "auto"); set("height", "auto");
    require(tree.node(id).computedStyle().width == kUnset && tree.node(id).computedStyle().height == kUnset,
            "auto dimensions must reset the pixel values in both layouts");
#if GEA_CSS_WIDTH_EXPRESSIONS && GEA_CSS_HEIGHT_EXPRESSIONS
    const int classNode = tree.createView(); NodeHandle(r).appendChild(NodeHandle(classNode));
    auto &sheet = StyleSheet::instance();
    sheet.registerRule("relative-size", "width", "25vw"); sheet.registerRule("relative-size", "height", "10vh");
    sheet.registerRule("fixed-size", "width", "30px"); sheet.registerRule("fixed-size", "height", "40px");
    for (const char *name : {"relative-size", "fixed-size", "relative-size"}) {
        sheet.setClassName(NodeHandle(classNode), name); tree.refresh(r, 200, 100);
        const bool relative = name[0] == 'r';
        require(tree.node(classNode).layout.width == (relative ? 50 : 30) && tree.node(classNode).layout.height == (relative ? 10 : 40),
                "cached class rules must switch between relative and pixel dimensions without percentage storage");
    }
#endif
    resetNativeHost();
    std::printf("PASS: dimension units and percentage storage width=%d height=%d\n", GEA_CSS_WIDTH_PERCENT, GEA_CSS_HEIGHT_PERCENT);
}

static void batchedClassEpochs()
{
    const int r = root(160, 120);
    auto &tree = Tree::instance();
    auto &sheet = StyleSheet::instance();
    std::array<int, 3> parents{}, children{}, expected{};
    sheet.registerRule("target", "width", "11px");
    sheet.registerRule("alternate", "width", "21px");
    sheet.registerSelectorRule(".on .target", "width", "31px");
    sheet.registerSelectorRule(".on .alternate", "width", "41px");
    for (int i = 0; i < 3; ++i) {
        parents[i] = tree.createView(); children[i] = tree.createView();
        NodeHandle(r).appendChild(NodeHandle(parents[i]));
        NodeHandle(parents[i]).appendChild(NodeHandle(children[i]));
        sheet.setClassName(NodeHandle(parents[i]), "off");
        sheet.setClassName(NodeHandle(children[i]), "target");
        expected[i] = 11;
    }
    tree.mount(r, 160, 120);
    for (int batch = 0; batch < 800; ++batch) {
        const int i = batch % 3;
        const bool on = (batch / 3) % 2 != 0, alternate = (batch / 6) % 2 != 0;
        beginStyleMountBatch();
        // Ancestors previously marked in old batches must not suppress this
        // batch's child roots when the byte generation counter wraps.
        if (batch % 300 == 0) sheet.setClassName(NodeHandle(r), batch % 600 ? "root-b" : "root-a");
        sheet.setClassName(NodeHandle(parents[i]), on ? "off" : "on");
        sheet.setClassName(NodeHandle(children[i]), alternate ? "alternate" : "target");
        sheet.setClassName(NodeHandle(parents[i]), on ? "on" : "off");
        endStyleMountBatch();
        expected[i] = (alternate ? 21 : 11) + (on ? 20 : 0);
        for (int j = 0; j < 3; ++j)
            require(tree.node(children[j]).computedStyle().width == expected[j],
                    "batched class deduplication and descendant invalidation must survive epoch reuse");
    }
    tree.refresh(r, 160, 120);
    for (int i = 0; i < 3; ++i)
        require(tree.node(children[i]).layout.width == expected[i], "batched class geometry must match the final cascade");
    resetNativeHost();
    std::puts("PASS: 800 class batches preserve pending roots and descendant invalidation across epoch reuse");
}

static void lengthCacheGeometry()
{
#if GEA_CSS_WIDTH_EXPRESSIONS && GEA_CSS_HEIGHT_EXPRESSIONS
    const int r = root(240, 160);
    auto &tree = Tree::instance();
    std::array<int, 70> ids{}; // Exceed the 64-entry dynamic cache in both axes.
    for (int &id : ids) {
        id = ball(r, 0, 0);
        auto style = NodeHandle(id).style();
        style.setProperty("width", "calc(50% + 3px)");
        style.setProperty("height", "calc(25% + 2px)");
    }
    tree.mount(r, 240, 160);
    for (int phase = 0; phase < 3; ++phase) {
        const int width = phase == 1 ? 280 : 240;
        const int height = phase == 1 ? 200 : 160;
        NodeHandle(r).style().width(width); NodeHandle(r).style().height(height);
        tree.refresh(r, width, height);
        for (int id : ids) {
            require(tree.node(id).layout.width == width / 2 + 3 && tree.node(id).layout.height == height / 4 + 2,
                    "length cache reuse and eviction must preserve node, axis and percentage basis");
        }
    }
    const int id = ids[0];
    NodeHandle(id).style().setProperty("width", "calc(7px + 11px)");
    NodeHandle(id).style().setProperty("height", "calc(5px + 9px)");
    for (int phase = 0; phase < 3; ++phase) {
        NodeHandle(id).style().left(phase);
        tree.refresh(r, 240, 160);
        require(tree.node(id).layout.width == 18 && tree.node(id).layout.height == 14,
                "static expression cache must preserve values and validity across repeated layout");
    }
#if GEA_CSS_CUSTOM_PROPERTIES
    for (int phase = 0; phase < 3; ++phase) {
        NodeHandle(id).style().setProperty("--length", phase == 1 ? "21px" : "9px");
        NodeHandle(id).style().setProperty("width", "calc(var(--length) + 1px)");
        NodeHandle(id).style().setProperty("height", "calc(var(--length) + 2px)");
        tree.refresh(r, 240, 160);
        require(tree.node(id).layout.width == (phase == 1 ? 22 : 10) && tree.node(id).layout.height == (phase == 1 ? 23 : 11),
                "dynamic expression cache must invalidate changed custom-property inputs");
    }
#endif
    resetNativeHost();
    std::puts("PASS: length-cache values, axes, basis changes, eviction and variable invalidation");
#endif
}

static void baseStyleFields()
{
    const int r = root(120, 80), id = ball(r, 0, 0);
    auto &tree = Tree::instance();
    auto set = [&](const char *key, const char *value) { NodeHandle(id).style().setProperty(key, value); };
    auto currentStyle = [&]() -> const ComputedStyle & { return tree.node(id).computedStyle(); };
    require(currentStyle().box_sizing == 0 && currentStyle().margin_auto == 0 && currentStyle().line_height_multiplier == -1 &&
            currentStyle().width_expression == -1 && currentStyle().min_height == kUnset && currentStyle().max_width == kUnset &&
            currentStyle().has_active_bg == 0 && currentStyle().active_bg_color == 0, "pruned common fields must retain their exact initial values");
    set("display", "flex");
    require(usesRowLayout(currentStyle()), "unmodified flex-direction must retain the CSS initial row behavior");
#if GEA_CSS_FLEX_DIRECTION
    set("flex-direction", "column");
    require(!usesRowLayout(currentStyle()) && currentStyle().flex_direction_explicit, "authored flex-direction must override the initial row");
#endif
#if GEA_CSS_JUSTIFY_CONTENT
    set("justify-content", "center"); require(currentStyle().justify_content == 1, "retained main-axis alignment must apply");
#endif
#if GEA_CSS_ALIGN_ITEMS
    set("align-items", "center"); require(currentStyle().align_items == 1, "retained cross-axis alignment must apply");
#endif
#if GEA_CSS_ACTIVE_BACKGROUND
    set("active-background", "#f00");
    require(currentStyle().has_active_bg && currentStyle().active_bg_color != 0, "active shorthand must retain color and presence together");
#endif
#if GEA_CSS_MARGIN_AUTO
    set("margin", "0 auto"); require(currentStyle().margin_auto == 10, "auto margins must retain both horizontal bits");
    set("margin-left", "3px"); require(currentStyle().margin_auto == 2, "numeric margin resets only its own auto bit");
#endif
    set("margin", "0"); set("padding", "4px"); set("border-width", "2px"); set("width", "40px"); set("height", "10px");
#if GEA_CSS_BOX_SIZING
    set("box-sizing", "border-box");
#endif
    const int insets = 2 * ((GEA_CSS_PADDING ? 4 : 0) + (GEA_CSS_BORDER_WIDTHS ? 2 : 0));
    tree.mount(r, 120, 80);
    require(tree.node(id).layout.width == (40 + (GEA_CSS_BOX_SIZING ? 0 : insets)), "border/content sizing must preserve padding and borders");
#if GEA_CSS_MIN_HEIGHT
    set("min-height", "30px"); tree.refresh(r, 120, 80);
    require(tree.node(id).layout.height == (30 + (GEA_CSS_BOX_SIZING ? 0 : insets)), "retained minimum height must clamp the box");
#endif
#if GEA_CSS_MAX_WIDTH
    set("max-width", "25px"); tree.refresh(r, 120, 80);
    require(tree.node(id).layout.width == (25 + (GEA_CSS_BOX_SIZING ? 0 : insets)), "retained maximum width must clamp the box");
#endif
#if GEA_CSS_WIDTH_EXPRESSIONS
    set("max-width", "none"); set("padding", "0"); set("border-width", "0"); set("width", "calc(100% - 10px)");
    tree.refresh(r, 120, 80); require(currentStyle().width_expression >= 0 && tree.node(id).layout.width == 110, "retained deferred width must resolve against its parent");
    set("width", "20px"); require(currentStyle().width_expression == -1, "ordinary widths must clear their previous expression");
#endif
#if GEA_CSS_LINE_HEIGHT_MULTIPLIER
    set("font-size", "10px"); set("line-height", "1.5");
    const int child = tree.createText(); NodeHandle(id).appendChild(NodeHandle(child));
    NodeHandle(child).style().setProperty("font-size", "20px"); NodeHandle(child).setText("X");
    tree.refresh(r, 120, 80);
    require(tree.node(child).computedStyle().line_height == 30, "inherited unitless line-height must resolve at the child's font size");
    set("line-height", "18px"); tree.refresh(r, 120, 80);
    require(currentStyle().line_height_multiplier == -1 && tree.node(child).computedStyle().line_height == 18, "fixed line-height must clear inherited multiplier state");
#endif
    std::printf("STYLE bytes=%zu base_fields=%d\n", sizeof(ComputedStyle), GEA_CSS_ACTIVE_BACKGROUND);
    resetNativeHost();
}
static void defaultStyleFields()
{
    const int r = root(160, 100), id = ball(r, 0, 0);
    auto &tree = Tree::instance();
    auto set = [&](const char *key, const char *value) { NodeHandle(id).style().setProperty(key, value); };
    auto current = [&]() -> const ComputedStyle & { return tree.node(id).computedStyle(); };
    for (int side = 0; side < 4; ++side)
        require(current().margin[side] == 0 && current().padding[side] == 0, "pruned edges must retain initial zero values");
    require(current().flex == 0 && current().flex_shrink == 1 && current().gap == 0 && current().border_width == 0 &&
            current().border_color_flags == 0 && current().font_weight == 400 && current().text_align == 0 &&
            current().white_space == 0 && current().text_overflow == 0, "unused defaults must retain their exact computed values");
#if GEA_CSS_MARGINS
    set("margin", "-2px 3px 4px 5px");
    require(current().margin[0] == -2 && current().margin[1] == 3 && current().margin[2] == 4 && current().margin[3] == 5,
            "authored margins must preserve signed physical edges");
#endif
#if GEA_CSS_PADDING
    set("padding", "1px 2px 3px 4px");
    for (int side = 0; side < 4; ++side) require(current().padding[side] == side + 1, "authored padding must preserve all edges");
#endif
#if GEA_CSS_FLEX_FACTORS
    set("flex", "2 0 auto");
    require(current().flex == 2 && current().flex_shrink == 0, "flex shorthand must retain both factors");
#endif
#if GEA_CSS_GAP
    set("gap", "7px"); require(current().gap == 7, "authored common gap must apply");
#endif
#if GEA_CSS_BORDER_WIDTHS
    set("border-width", "3px"); require(current().border_width == 3, "authored common border width must apply");
#endif
#if GEA_CSS_BORDER_COLORS
    set("border-color", "#ff0000");
    require(!borderColorIsCurrent(current()) && borderPaintColor(current()) == gea::framework::graphics::pixel::nativeColor(255, 0, 0),
                                       "literal border color must retain its binding flag");
    set("border-color", "currentColor"); require(borderColorIsCurrent(current()) && borderPaintColor(current()) == current().text_color,
                                                 "currentColor reset must follow inherited text color");
#endif
#if GEA_CSS_FONT_WEIGHT
    set("font-weight", "700"); require(current().font_weight == 700, "authored font weight must apply");
#endif
#if GEA_CSS_TEXT_ALIGN
    set("text-align", "center"); require(current().text_align == 1, "authored text alignment must apply");
#endif
#if GEA_CSS_WHITE_SPACE
    set("white-space", "pre-wrap"); require(current().white_space == 3, "authored whitespace mode must apply");
#endif
#if GEA_CSS_TEXT_OVERFLOW
    set("text-overflow", "ellipsis"); require(current().text_overflow == 1, "authored ellipsis mode must apply");
#endif
    const int child = tree.createText(); NodeHandle(id).appendChild(NodeHandle(child)); NodeHandle(child).setText("child");
    tree.mount(r, 160, 100);
    const auto &inherited = tree.node(child).computedStyle();
    require(inherited.font_weight == current().font_weight && inherited.text_align == current().text_align &&
            inherited.white_space == current().white_space && inherited.text_overflow == 0,
            "inheritance must preserve retained defaults without inheriting ellipsis");
    resetNativeHost();
}
static void retainedBalls()
{
    // Both default unset minima and explicit zero must keep the fast layout path.
    for (bool zero : {false, true}) {
        int r = root(512, 512);
        std::array<int, 64> ids{};
        for (int i = 0; i < 64; ++i) {
            ids[i] = ball(r, 12 + (i % 8) * 58, 12 + (i / 8) * 58);
            if (zero) {
                NodeHandle(ids[i]).style().set(Property::MinWidth, 0);
                NodeHandle(ids[i]).style().set(Property::MinHeight, 0);
            }
        }
        Tree::instance().mount(r, 512, 512);
        const int commands = DisplayList::instance().commandCount();
        for (int frame = 1; frame <= 12; ++frame) {
            refreshPerfStatsReset();
            for (int i = 0; i < 64; ++i) {
                auto &sheet = StyleSheet::instance();
                sheet.applyProperty(NodeHandle(ids[i]), "left", std::to_string(12 + (i % 8) * 58 + frame));
                sheet.applyProperty(NodeHandle(ids[i]), "top", std::to_string(12 + (i / 8) * 58 + frame));
            }
            Tree::instance().refresh(r, 512, 512);
            const auto stats = refreshPerfStatsRead();
            require(LayoutEngine::memoStorageBytes() == 0, "retained frames must not retain layout scratch");
            requirePerf(stats.treeIntegerPositionFastCalls == 128, "128 integer position updates must bypass generic CSS parsing");
            requirePerf(stats.treeAbsModeFast > 0 && stats.treeAbsModeFull == 0, "moving fixed-size leaves must use retained absolute layout");
            requirePerf(stats.treeAbsContainingAreaCalls == 1 && stats.treeAbsContainingAreaHits == 63,
                    "absolute siblings must calculate their shared containing area once per frame");
            requirePerf(stats.treeLayoutNodeCalls == 0 && stats.treeRecordCalls == 0, "ball motion must not relayout or rerecord the tree");
            requirePerf(stats.treeTranslateClipChecks == 0, "leaf motion must not scan the display list for descendant clips");
            require(DisplayList::instance().commandCount() == commands, "ball motion must keep the display list");
            for (int i = 0; i < 64; ++i) {
                const auto &layout = Tree::instance().node(ids[i]).layout;
                require(layout.x == 12 + (i % 8) * 58 + frame && layout.y == 12 + (i / 8) * 58 + frame, "retained geometry must follow each ball");
            }
        }
        require(displayPixelAt(13, 13) == 0 && displayPixelAt(25, 25) == 0xffff, "retained motion must clear old pixels and paint new ones");
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
            pixelHash ^= displayPixelAt(x, y);
            pixelHash *= 1099511628211ULL;
        }
    }
    require(pixelHash == 18157712955437833091ULL, "all retained pixels must match the captured inline baseline");
    for (Property constraint : {Property::MinWidth, Property::MinHeight, Property::MaxWidth, Property::MaxHeight}) {
        if ((!GEA_CSS_MIN_HEIGHT && constraint == Property::MinHeight) ||
            (!GEA_CSS_MAX_WIDTH && constraint == Property::MaxWidth)) continue;
        int r = root(96, 96), id = ball(r, 10, 10);
        NodeHandle(id).style().set(constraint, 20);
        Tree::instance().mount(r, 96, 96);
        refreshPerfStatsReset();
        NodeHandle(id).style().left(20);
        Tree::instance().refresh(r, 96, 96);
        requirePerf(refreshPerfStatsRead().treeAbsModeFull > 0, "dimension constraints must preserve the general layout fallback");
    }
}
static void retainedContainingAreas()
{
    auto &tree = Tree::instance();
    const int r = root(512, 256);
    const int p = ball(r, 10, 10), q = ball(r, 260, 20);
    NodeHandle(p).style().width(200); NodeHandle(p).style().height(180);
    NodeHandle(q).style().width(160); NodeHandle(q).style().height(140);
    NodeHandle(p).style().setProperty("border-width", "3px");
    NodeHandle(q).style().setProperty("border-width", "5px");
    // Deliberately return to the first parent after visiting a different one.
    std::array<int, 5> ids{ball(p, 10, 10), ball(p, 30, 10), ball(q, 10, 10), ball(q, 30, 10), ball(p, 50, 10)};
    tree.mount(r, 512, 256);
    for (int frame = 0; frame < 3; ++frame) {
        refreshPerfStatsReset();
        for (int id : ids) {
            auto style = NodeHandle(id).style();
            style.setProperty("left", "auto"); style.setProperty("top", "auto");
            style.setProperty("right", std::to_string(10 + frame) + "%");
            style.setProperty("bottom", std::to_string(20 + frame) + "%");
        }
        tree.refresh(r, 512, 256);
        const auto stats = refreshPerfStatsRead();
        requirePerf(stats.treeAbsModeFast > 0 && stats.treeAbsModeFull == 0, "percentage anchors must retain layout");
        requirePerf(stats.treeAbsContainingAreaCalls == 3 && stats.treeAbsContainingAreaHits == 2,
                "changing parents must invalidate containing-area reuse");
        std::array<std::array<int, 4>, 5> boxes{};
        for (int i = 0; i < 5; ++i) {
            const auto &b = tree.node(ids[i]).layout;
            boxes[i] = {b.x, b.y, b.width, b.height};
        }
        {
            LayoutEngine::Pass pass;
            LayoutEngine::instance().layoutNode(r, 512, 256);
            LayoutEngine::instance().resolveAbsoluteCoords(r, 0, 0);
        }
        for (int i = 0; i < 5; ++i) {
            const auto &b = tree.node(ids[i]).layout;
            require(boxes[i] == std::array<int, 4>{b.x, b.y, b.width, b.height},
                    "cached parent borders and percentage anchors must match full layout");
        }
    }
}
static void boundedClassStorage()
{
    NodeClassList original;
    for (int i = 0; i < NodeClassList::kInlineTokenCount; ++i)
        require(original.add("class" + std::to_string(i)), "all proven inline class slots must remain usable");
    require(!original.add("class0"), "duplicates must not overflow a full class list");
    NodeClassList copied(original);
    NodeClassList moved(std::move(copied));
    require(copied.empty() && moved.value() == original.value(), "class copy/move must preserve all inline tokens");
    require(moved.remove("class0") && !moved.contains("class0"), "class removal must release its inline slot");
    require(original.contains("class0"), "class copies must mutate independently");
    require(moved.add("replacement"), "class removal must permit slot reuse");
    moved = original;
    require(moved.value() == original.value(), "class assignment must replace the reused slot");
#if GEA_UI_CLASS_OVERFLOW
    require(original.add("overflow"), "unknown class programs must retain overflow support");
    NodeClassList overflowCopy(original), overflowMove(std::move(overflowCopy));
    require(overflowCopy.empty() && overflowMove.contains("overflow"), "overflow ownership must survive copy and move");
    require(overflowMove.remove("class0") && overflowMove.contains("overflow"), "removal must promote overflow tokens");
#else
    static_assert(sizeof(NodeClassList) == (NodeClassList::kInlineTokenCount + 1) * sizeof(CssAtomId));
#endif
    original.clear(); moved.clear();
    require(original.empty() && moved.empty(), "class storage must clear completely");
}
static void integerPositions()
{
    int r = root(200, 100, 2), id = ball(r, 0, 0);
    Tree::instance().mount(r, 200, 100);
    const char *names[] = {"top", "right", "bottom", "left"};
    auto position = [&](int side) {
        const auto &s = Tree::instance().node(id).computedStyle();
        switch (side) {
        case 0: return int(GEA_CSS_POSITION_PX(s, 0));
        case 1: return int(GEA_CSS_POSITION_PX(s, 1));
        case 2: return int(GEA_CSS_POSITION_PX(s, 2));
        default: return int(GEA_CSS_POSITION_PX(s, 3));
        }
    };
    for (int side = 0; side < 4; ++side) {
        for (const char *value : {"0", "-0", "+17", "-17", "00042", "32767", "-32767"}) {
            refreshPerfStatsReset();
            StyleSheet::instance().applyProperty(NodeHandle(id), names[side], value);
            requirePerf(refreshPerfStatsRead().treeIntegerPositionFastCalls == 1, "complete small signed integers must use the fast parser");
            require(position(side) == std::stoi(value), "unitless integers must not be scaled by DPR");
        }
        for (const char *value : {"", "+", "-", "12px", "1.5", "20%", "calc(2px + 3px)", "32768", "-32768", "999999999999999999999999", " 12 ", "12oops"}) {
            refreshPerfStatsReset();
            StyleSheet::instance().applyProperty(NodeHandle(id), names[side], value);
            requirePerf(refreshPerfStatsRead().treeIntegerPositionFastCalls == 0, "units, expressions and invalid/range overflow values must use the general parser");
        }
        StyleSheet::instance().applyProperty(NodeHandle(id), names[side], "12px");
        require(position(side) == 24, "px lengths must retain DPR scaling");
    }
    refreshPerfStatsReset();
    StyleSheet::instance().applyProperty(NodeHandle(id), "width", "12");
    requirePerf(refreshPerfStatsRead().treeIntegerPositionFastCalls == 0, "non-position declarations must retain their own semantics");
}
static void clippedReplay()
{
    constexpr int width = 96, height = 96, stride = 101;
    int r = root(width, height), id = ball(r, 20, 20);
    Tree::instance().mount(r, width, height);
    require(DisplayList::instance().canReplaySimpleDirtyRegions(width, height), "fixture must exercise simple replay");
    // Rebind to a guarded DMA-like destination, distinct from the normal framebuffer.
    constexpr std::uint16_t guard = 0x1234;
    std::vector<std::uint16_t> chunk(stride * height + 32, guard);
    auto *canvas = Display::canvas();
    canvas->bindPixels(chunk.data() + 16, width, height, stride);
    canvas->pushClip(18, 18, 20, 20);
    workerSubmissions = 0;
    DisplayList::instance().replaySimpleClippedDirtyRegion(0, 0, width - 1, height - 1, id);
    require(workerSubmissions == 0, "already-clipped DMA replay must not submit to a differently bound worker canvas");
    for (int y = 0; y < height; ++y) for (int x = 0; x < stride; ++x) {
        auto expected = guard;
        if (x >= 18 && x < 38 && y >= 18 && y < 38)
            expected = x >= 20 && x < 32 && y >= 20 && y < 32 ? 0xffff : 0;
        require(chunk[16 + y * stride + x] == expected, "replay must honor the active clip, destination and stride");
    }
    require(std::all_of(chunk.begin(), chunk.begin() + 16, [](auto p) { return p == guard; }) &&
            std::all_of(chunk.end() - 16, chunk.end(), [](auto p) { return p == guard; }), "DMA buffer guards must survive replay");
    // Detach before the backing vector goes out of scope.
    resetNativeHost();
}
static void layoutMemoLifetime()
{
    auto &tree = Tree::instance();
    auto &engine = LayoutEngine::instance();
    const int r = root(120, 80);
    const int id = tree.createView();
    NodeHandle(r).appendChild(NodeHandle(id));
    NodeHandle(id).style().width(20); NodeHandle(id).style().height(10);
    NodeHandle(id).style().setProperty("display", "flex");
    tree.mount(r, 120, 80);
    require(engine.memoStorageBytes() == 0, "mount must release its layout scratch before returning");
    require(engine.memoPeakHeapBytes() >= 10 * tree.nodeCount(), "memo peak must count the complete scratch array");
    {
        LayoutEngine::Pass pass;
        require(engine.memoStorageBytes() == 10 * tree.nodeCount(), "layout pass must own its scratch array");
        engine.layoutNode(id, 100, 100); engine.layoutNode(id, 120, 100);
        const auto calls = refreshPerfStatsRead().treeLayoutNodeCalls;
        const auto hits = refreshPerfStatsRead().treeLayoutMemoHits;
        for (int i = 0; i < 32; ++i) engine.layoutNode(id, i % 2 ? 120 : 100, 100);
        requirePerf(refreshPerfStatsRead().treeLayoutNodeCalls == calls && refreshPerfStatsRead().treeLayoutMemoHits == hits + 32,
                "both memo slots must survive repeated promotion during flex measurement");
        tree.nodes()[id].layout.width = 99;
        engine.layoutNode(id, 100, 100);
        require(tree.node(id).layout.width == 20, "layout dimensions must remain correct with profiling disabled");
        requirePerf(refreshPerfStatsRead().treeLayoutNodeCalls == calls + 1,
                "external resize must invalidate memoized results");
        engine.invalidateMemo(id);
        engine.layoutNode(id, 120, 100);
        requirePerf(refreshPerfStatsRead().treeLayoutNodeCalls == calls + 2, "explicit invalidation must clear the secondary slot too");
        const auto scratchBytes = engine.memoStorageBytes();
        const int added = tree.createView();
        NodeHandle(added).style().width(37); NodeHandle(added).style().height(9);
        engine.layoutNode(added, 100, 100);
        require(tree.node(added).layout.width == 37 && engine.memoStorageBytes() == scratchBytes,
                "nodes added during layout must work without moving the active scratch array");
        const auto afterGrowth = refreshPerfStatsRead().treeLayoutNodeCalls;
        engine.layoutNode(id, 120, 100);
        requirePerf(refreshPerfStatsRead().treeLayoutNodeCalls == afterGrowth, "node growth must preserve existing memo entries");
        tree.removeNode(added);
        // An out-of-range measurement cannot be memoized, but may leave the
        // earlier valid measurement in slot two. Promoting it must restore
        // the persistent available box used by the scoped pass below.
        engine.layoutNode(id, 100, 100);
        engine.layoutNode(id, 40000, 100);
        const auto beforePromotion = refreshPerfStatsRead().treeLayoutNodeCalls;
        engine.layoutNode(id, 100, 100);
        requirePerf(refreshPerfStatsRead().treeLayoutNodeCalls == beforePromotion,
                "an uncacheable measurement must preserve the previous secondary memo hit");
    }
    require(engine.memoStorageBytes() == 0, "layout scope must release its allocation");
    // The available box remains inline after scratch is freed, so a scoped
    // relayout must still work and clean up on both success and rejection.
    require(engine.layoutNodeScoped(id, r), "scoped layout must retain its cross-frame available box");
    require(engine.memoStorageBytes() == 0, "scoped layout must release scratch");
    NodeHandle(id).style().setProperty("width", "auto");
    NodeHandle(id).style().setProperty("align-self", "flex-start");
    const int text = tree.createText();
    NodeHandle(text).setText("a longer intrinsic box");
    NodeHandle(id).appendChild(NodeHandle(text));
    require(!engine.layoutNodeScoped(id, r), "changed intrinsic dimensions must reject the old scope");
    require(engine.memoStorageBytes() == 0, "rejected scoped layout must release scratch");
    // Repeated pass allocation must not revive a first-slot hit from an earlier
    // allocation. Deliberately bypass style invalidation to expose that bug.
    for (int width : {40, 60}) {
        tree.nodes()[id].mutableStyle().width = width;
        for (int i = 0; i < 65535; ++i) engine.beginLayoutPass();
        engine.layoutNode(id, 120, 100);
        require(tree.node(id).layout.width == width, "new passes must never revive an old allocation's result");
        engine.endLayoutPass();
    }
    engine.beginLayoutPass();
    engine.layoutNode(id, 100, 100); engine.layoutNode(id, 120, 100);
    tree.removeNode(id);
    const int reused = tree.createView();
    require(reused == id, "memo lifetime fixture must reuse the released slot");
    NodeHandle(reused).style().width(25); NodeHandle(reused).style().height(10);
    const auto beforeReuse = refreshPerfStatsRead().treeLayoutNodeCalls;
    engine.layoutNode(reused, 100, 100);
    require(tree.node(reused).layout.width == 25, "layout dimensions must remain correct with profiling disabled");
        requirePerf(refreshPerfStatsRead().treeLayoutNodeCalls == beforeReuse + 1,
            "reused node slots must not inherit either memo entry");
    engine.endLayoutPass();
    tree.nodes()[reused].mutableStyle().width = 35;
    engine.layoutNode(reused, 100, 100);
    require(tree.node(reused).layout.width == 35, "layout without scratch must still compute correct dimensions");
    engine.beginLayoutPass();
    tree.clear();
    require(engine.memoStorageBytes() == 0, "clearing the tree must release a manually opened pass");
    // A zero-sized previous result matches fresh zeroed scratch. Persistent
    // available-box validity must never be mistaken for a hit in a new pass.
    const int zero = root(100, 100);
    NodeHandle(zero).style().width(0); NodeHandle(zero).style().height(0);
    {
        LayoutEngine::Pass pass;
        engine.layoutNode(zero, 100, 100);
    }
    require(tree.node(zero).layout.width == 0 && tree.node(zero).layout.height == 0,
            "zero-size memo fixture must retain its empty geometry");
    tree.nodes()[zero].mutableStyle().width = 31;
    {
        LayoutEngine::Pass pass;
        engine.layoutNode(zero, 100, 100);
        require(tree.node(zero).layout.width == 31,
                "fresh layout scratch must not reuse a prior zero-size result");
    }
    resetNativeHost();
    std::puts("PASS: layout memo lifetime, MRU, invalidation, scoped cleanup and fresh zero-size geometry");
}
static void persistentLayoutPages()
{
    auto &tree = Tree::instance();
    auto &engine = LayoutEngine::instance();
    int r = root(120, 80);
    NodeHandle(r).style().setProperty("display", "flex");
    tree.mount(r, 120, 80);
#if GEA_EMBEDDED_SHARED_STYLES
    require(engine.persistentLayoutStorageBytes() > 0, "shared nodes must account for their cold layout state");
    const auto initial = engine.persistentLayoutStorageBytes();
    // Cross several page boundaries while the old root remains laid out.
    // A pointer table resize must never move the referenced entries.
    for (int i = 0; i < 70; ++i) tree.createView();
    require(engine.persistentLayoutStorageBytes() > initial, "new pages must be included in the allocation census");
    require(engine.persistentLayoutPeakBytes() >= engine.persistentLayoutHeapBytes(), "page growth must account for overlapping pointer tables");
    require(engine.persistentLayoutPeakAllocations() >= engine.persistentLayoutAllocationCount(), "growth headers must be counted too");
    require(engine.layoutNodeScoped(r, r), "page growth must preserve the earlier node's available box");
#endif
    resetNativeHost();
    require(engine.persistentLayoutStorageBytes() == 0 && engine.persistentLayoutAllocationCount() == 0,
            "tree reset must reclaim every cold layout page and its pointer table");
    r = root(120, 80);
    // The auto-positioned absolute box must follow its preceding block, then
    // keep the same static anchor during a retained position-only change.
    const int before = tree.createView(), absolute = tree.createView();
    NodeHandle(r).appendChild(NodeHandle(before)); NodeHandle(r).appendChild(NodeHandle(absolute));
    NodeHandle(before).style().width(20); NodeHandle(before).style().height(25);
    NodeHandle(absolute).style().position(1); NodeHandle(absolute).style().width(10); NodeHandle(absolute).style().height(10);
    tree.mount(r, 120, 80);
    require(tree.node(absolute).layout.y == 25, "absolute auto position must retain its preceding block anchor");
    NodeHandle(absolute).style().left(30);
    tree.refresh(r, 120, 80);
    require(tree.node(absolute).layout.x == 30 && tree.node(absolute).layout.y == 25,
            "retained absolute movement must keep the cold static-position anchor");
    NodeHandle(before).style().height(35);
    tree.refresh(r, 120, 80);
    require(tree.node(absolute).layout.y == 35, "reflow must update the cold static-position anchor");
    resetNativeHost();
}
#if GEA_EMBEDDED_SHARED_STYLES
static void sharedOwnership()
{
    resetNativeHost();
    require(NodeStyleStorage::allocatedRecords() == 0, "tree reset must reclaim every style record");
    {
        NodeStyleStorage first;
        first.write().width = 20;
        NodeStyleStorage second(first);
        require(&first.read() == &second.read(), "copy must share initially");
        second.write().width = 30;
        require(first.read().width == 20 && second.read().width == 30, "copy-on-write must isolate changes");
        NodeStyleStorage moved(std::move(second));
        require(moved.read().width == 30 && second.read().width == kUnset, "move must transfer ownership and reset its source");
        first = first; moved = std::move(moved);
        moved.write().width = 20; moved.intern();
        require(&first.read() == &moved.read(), "equal semantic styles must intern");
        for (int i = 0; i < 2000; ++i) {
            NodeStyleStorage temporary(first);
            temporary.write().height = i;
            require(first.read().height == kUnset, "repeated detach must leave the shared source intact");
        }
    }
    require(NodeStyleStorage::allocatedRecords() == 0, "owners must reclaim all records");
    {
        NodeStyleStorage tail, middle, head;
        tail.write().width = 11; middle.write().width = 22; head.write().width = 33;
        middle.reset();
        require(NodeStyleStorage::allocatedRecords() == 2 && tail.read().width == 11 && head.read().width == 33,
                "reclaiming a middle style must preserve both neighbours");
        tail.reset();
        require(NodeStyleStorage::allocatedRecords() == 1 && head.read().width == 33,
                "reclaiming the tail style must preserve the head");
        head.reset();
        require(NodeStyleStorage::allocatedRecords() == 0, "reclaiming the head must empty the style pool");
    }
    const int r = root(120, 80);
    auto &sheet = StyleSheet::instance();
    sheet.clear();
    sheet.registerRule("shared-card", "width", "20");
    sheet.registerRule("shared-card", "height", "10");
    sheet.registerRule("shared-card", "background-color", "#ffffff");
    int a = Tree::instance().createView(), b = Tree::instance().createView();
    NodeHandle(r).appendChild(NodeHandle(a)); NodeHandle(r).appendChild(NodeHandle(b));
    NodeHandle(a).classList().set("shared-card"); NodeHandle(b).classList().set("shared-card");
    Tree::instance().mount(r, 120, 80);
    require(&Tree::instance().node(a).computedStyle() == &Tree::instance().node(b).computedStyle(), "identical class styles must share after recompute");
    NodeHandle(a).style().width(30);
    require(Tree::instance().node(b).computedStyle().width == 20, "inline override must not alter its sibling");
    NodeHandle(a).style().setProperty("transform", "translateX(3px)");
    int clone = Tree::instance().cloneNode(a, false);
    require(clone >= 0, "clone must succeed");
    NodeHandle(clone).style().setProperty("transform", "translateX(7px)");
    require(rstyle(Tree::instance().node(a).computedStyle()).transform_translate_x == 3 &&
            rstyle(Tree::instance().node(clone).computedStyle()).transform_translate_x == 7,
            "cloning must preserve independent rare-style ownership");
    for (int frame = 0; frame < 30; ++frame) {
        NodeHandle(r).style().setProperty("color", frame % 2 ? "#ff0000" : "#00ff00");
        Tree::instance().refresh(r, 120, 80);
        require(Tree::instance().node(a).computedStyle().text_color == Tree::instance().node(b).computedStyle().text_color,
                "inheritance recompute must reach both shared and detached children");
        require(Tree::instance().node(b).computedStyle().width == 20, "inheritance must preserve the sibling's own class style");
    }
    resetNativeHost();
    require(NodeStyleStorage::allocatedRecords() == 0, "class/clone/rare-style reset must reclaim all style records");
}
#endif
// Exercise adjacent bitfield writes and signed sentinel values, including
// copy-on-write isolation; byte counts alone cannot catch their corruption.
static void packedStyleFields()
{
    for (int display = 0; display < 4; ++display)
    for (int position = 0; position < 4; ++position)
    for (int overflow = -1; overflow <= 3; ++overflow)
    for (int flags = 0; flags < 8; ++flags) {
        ComputedStyle s{};
        s.display = display; s.position = position; s.overflow = overflow;
        s.has_bg = flags & 1;
#if GEA_CSS_DISPLAY_EXPLICIT
        s.display_explicit = (flags >> 1) & 1;
#endif
#if GEA_CSS_Z_INDEX
        s.z_index_auto = (flags >> 2) & 1;
#endif
        require(s.display == display && s.position == position && s.overflow == overflow && s.has_bg == (flags & 1),
                "packed modes must preserve every value and the signed overflow sentinel");
#if GEA_CSS_DISPLAY_EXPLICIT
        require(s.display_explicit == ((flags >> 1) & 1), "background and z-index flags must not overwrite display explicitness");
#endif
#if GEA_CSS_Z_INDEX
        require(s.z_index_auto == ((flags >> 2) & 1), "mode writes must not overwrite z-index auto");
#endif
#if GEA_EMBEDDED_SHARED_STYLES
        NodeStyleStorage a;
        a.write() = s;
        NodeStyleStorage b(a);
        b.write().display = (display + 1) % 4;
        b.write().overflow = overflow == -1 ? 3 : -1;
        require(a.read().display == display && a.read().overflow == overflow && a.read().position == position,
                "packed shared-style mutation must detach without changing its sibling");
        require(b.read().position == position && b.read().has_bg == (flags & 1),
                "detached packed-style writes must preserve adjacent fields");
#endif
    }
    for (int bits = 0; bits < 512; ++bits) {
        RenderState r{};
        r.text_dirty = {-32000, 32000};
        r.dirty = bits & 1; r.layout_dirty = (bits >> 1) & 1;
        r.bg_recolor_pending = (bits >> 2) & 1; r.text_layout_stable = (bits >> 3) & 1;
        r.text_partial_dirty = (bits >> 4) & 1; r.inline_baseline = (bits >> 5) & 1;
#if GEA_CSS_SCROLLING
        r.scroll_dirty = (bits >> 6) & 1; r.non_scroll_dirty = (bits >> 7) & 1;
#endif
#if GEA_CSS_TRANSFORMS
        r.transform_dirty = (bits >> 8) & 1;
#endif
        require(r.dirty == (bits & 1) && r.layout_dirty == ((bits >> 1) & 1) &&
                r.bg_recolor_pending == ((bits >> 2) & 1) && r.text_layout_stable == ((bits >> 3) & 1) &&
                r.text_partial_dirty == ((bits >> 4) & 1) && r.inline_baseline == ((bits >> 5) & 1) &&
                r.text_dirty.x0 == -32000 && r.text_dirty.x1 == 32000,
                "packed render writes must preserve neighbouring flags and scratch coordinates");
#if GEA_CSS_SCROLLING
        require(r.scroll_dirty == ((bits >> 6) & 1) && r.non_scroll_dirty == ((bits >> 7) & 1), "scroll flags must remain independent");
#endif
#if GEA_CSS_TRANSFORMS
        require(r.transform_dirty == ((bits >> 8) & 1), "transform dirty must retain its own bit");
#endif
    }
#if !GEA_UI_NODE_LISTENERS
    static_assert(std::is_empty_v<NodeEventListeners>);
#endif
#if !GEA_UI_NODE_ATTRIBUTES
    static_assert(std::is_empty_v<NodeAttributeStore>);
#endif
#if !GEA_UI_DEFAULT_STYLES
    static_assert(std::is_empty_v<EmptyNodeStyleOverrideStore>);
#endif
    resetNativeHost();
    auto &document = Document::instance();
    const auto mounted = document.getElementById("app");
    require(mounted && document.getElementById("app").id() == mounted.id() &&
            document.querySelector("#app").id() == mounted.id() && document.ensureAppRoot().id() == mounted.id(),
            "implicit app root must initialize and keep its identity without allocating attribute owners");
    document.clear();
    require(document.getElementById("app").id() >= 0, "clearing the app must allow a fresh implicit root");
    resetNativeHost();
    std::puts("PASS: packed style/render modes preserve neighbours, sentinels and shared ownership");
}
int main(int argc, char **argv)
{
#if !GEA_EMBEDDED_UI_REFRESH_PERF
    // Check the actual backing bytes, not the public off-mode read (which returns
    // defaults). A discarded diagnostic write is still unwanted runtime work.
    std::array<unsigned char, sizeof(RefreshPerfStats)> untouchedStats;
    untouchedStats.fill(0x5a);
    std::memcpy(&gRefreshPerfStats, untouchedStats.data(), untouchedStats.size());
    int evaluated = 0;
    GEA_REFRESH_PERF(++evaluated);
    require(evaluated == 0, "disabled profiling must not evaluate diagnostic arguments");
#endif
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "classes-overflow") {
        NodeClassList list;
        for (int i = 0; i <= NodeClassList::kInlineTokenCount; ++i) list.add("class" + std::to_string(i));
        return 0;
    }
    if (selected == "all" || selected == "compact-storage") packedStyleFields();
    if (selected == "compact-storage") retainedBalls();
    if (selected == "all" || selected == "storage") { nodeAllocationCensus(); overrideCapacityAndOwnership(); }
    if (selected == "all" || selected == "dependencies") classDependencyStorage();
    if (selected == "all" || selected == "base-style") { baseStyleFields(); defaultStyleFields(); }
    if (selected == "all" || selected == "dimension-percent") dimensionPercentageStorage();
    if (selected == "all" || selected == "length-cache") lengthCacheGeometry();
    if (selected == "all" || selected == "class-epochs") batchedClassEpochs();
    if (selected == "all" || selected == "classes") boundedClassStorage();
    if (selected == "all" || selected == "rule-cache") classRuleCacheReuse();
    if (selected == "all" || selected == "retained") { retainedBalls(); retainedContainingAreas(); }
    if (selected == "all" || selected == "integer") integerPositions();
    if (selected == "all" || selected == "replay") clippedReplay();
    if (selected == "all" || selected == "memo") { layoutMemoLifetime(); persistentLayoutPages(); }
#if GEA_EMBEDDED_SHARED_STYLES
    if (selected == "all") sharedOwnership();
#endif
#if !GEA_EMBEDDED_UI_REFRESH_PERF
    require(std::memcmp(&gRefreshPerfStats, untouchedStats.data(), untouchedStats.size()) == 0,
            "profiling-off rendering must not write any diagnostic counter or sample");
    std::puts("PASS: profiling disabled leaves all diagnostic storage untouched");
#endif
    std::printf("PIXELS hash=%llu\n", static_cast<unsigned long long>(pixelHash));
    std::puts("PASS: 64-ball retained work, constraints, integer CSS semantics, clipped DMA replay");
}
