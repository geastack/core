#include "native_test_harness.h"

#include "ui/document.h"
#include "ui/node.h"
#include "ui/tree_state.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/internal.h"
#include "ui/flex_balance.h"
#include "graphics/font.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
// Deliberately different ascenders/line advances expose baseline alignment
// that accidentally aligns border edges. No platform font installation needed.
const RasterizedFontData *lookupFontForFamily(int familyId, int)
{
	static const std::uint8_t atlas[] = {255};
	static const Glyph glyphs[] = {{32, 0, 0, 0, 0, 5, 0, 0}, {88, 0, 0, 1, 1, 5, 0, 1}};
	static const RasterizedFontData small{9101, 10, 10, 6, -4, 2, glyphs, 1, 1, atlas};
	static const RasterizedFontData large{9102, 20, 20, 18, -2, 2, glyphs, 1, 1, atlas};
	return familyId == 9101 ? &small : familyId == 9102 ? &large : nullptr;
}
}  // namespace gea::framework::graphics::generated

namespace {

using namespace gea::embedded::ui;
using namespace gea::embedded::test;

int gFailures = 0;

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[css_block_flexbasis] FAIL %s: expected %d, got %d\n", label, expected, actual);
	gFailures++;
	return false;
}

bool expectTrue(bool cond, const char *label)
{
	if (cond) return true;
	std::fprintf(stderr, "[css_block_flexbasis] FAIL %s\n", label);
	gFailures++;
	return false;
}

int nativeRgb565(std::uint16_t color)
{
	return static_cast<int>(gea::framework::graphics::pixel::fromRgb565(color));
}

const NodeCustomProperty *customPropertyForNode(int nodeId, const char *name)
{
	const NodeRareData *rare = rareDataFor(nodeId);
	if (!rare) return nullptr;
	return rare->customProperties.getEntry(internCssAtom(name));
}

int makeSpan(int parent, const char *cls)
{
	const int id = Tree::instance().createView();
	Tree::instance().setTagName(id, "span");
	NodeHandle(parent).appendChild(NodeHandle(id));
	NodeHandle(id).classList().set(cls);
	return id;
}

int makeDiv(int parent, const char *cls)
{
	const int id = Tree::instance().createView();
	Tree::instance().setTagName(id, "div");
	if (parent >= 0) NodeHandle(parent).appendChild(NodeHandle(id));
	if (cls) NodeHandle(id).classList().set(cls);
	return id;
}

int findDirectChildByTag(int parent, const char *tag)
{
	Tree &tree = Tree::instance();
	if (parent < 0 || parent >= tree.nodeCount()) return -1;
	for (int child = tree.node(parent).first_child; child >= 0; child = tree.node(child).next_sibling) {
		if (std::strcmp(tree.tagName(child), tag) == 0) return child;
	}
	return -1;
}

// BUG #1: two `display:block` <span>s inside a plain-block <button> must STACK
// vertically (block-level boxes), not flow side-by-side as an inline row. This is
// the weather city list's .city-name / .city-detail.
void testDisplayBlockStacks()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int sel = Tree::instance().createView();
	Tree::instance().setTagName(sel, "button");  // plain block, no display rule
	NodeHandle(sel).classList().set("sel");
	const int name = makeSpan(sel, "name");
	const int detail = makeSpan(sel, "detail");
	Tree::instance().mount(sel, 200, 100);

	StyleSheet::instance().registerRule("name", "display", "block");
	StyleSheet::instance().registerRule("name", "width", "40");
	StyleSheet::instance().registerRule("name", "height", "12");
	StyleSheet::instance().registerRule("detail", "display", "block");
	StyleSheet::instance().registerRule("detail", "width", "60");
	StyleSheet::instance().registerRule("detail", "height", "10");
	// re-set classes so the registered rules apply
	NodeHandle(name).classList().set("name");
	NodeHandle(detail).classList().set("detail");
	Tree::instance().computeLayout(sel, 200, 100);

	const auto &n = Tree::instance().node(name);
	const auto &d = Tree::instance().node(detail);
	std::fprintf(stderr, "[css_block_flexbasis] block: name=(%d,%d %dx%d) detail=(%d,%d %dx%d)\n",
	             n.layout.x, n.layout.y, n.layout.width, n.layout.height,
	             d.layout.x, d.layout.y, d.layout.width, d.layout.height);
	expectEqual(n.layout.x, d.layout.x, "block: spans share x (stacked)");
	expectTrue(d.layout.y >= n.layout.y + n.layout.height, "block: detail below name (stacked, not row)");
}

// REGRESSION: two DEFAULT <span>s (no display rule) inside a plain block must
// still flow as an inline ROW (gea's inline-emulation). Guards that the explicit
// flag does not change default behavior.
void testDefaultSpansStillRow()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = Tree::instance().createView();
	Tree::instance().setTagName(box, "div");
	NodeHandle(box).classList().set("ibox");
	const int s1 = makeSpan(box, "s1");
	const int s2 = makeSpan(box, "s2");
	Tree::instance().mount(box, 200, 100);

	StyleSheet::instance().registerRule("s1", "width", "40");
	StyleSheet::instance().registerRule("s1", "height", "12");
	StyleSheet::instance().registerRule("s2", "width", "60");
	StyleSheet::instance().registerRule("s2", "height", "10");
	NodeHandle(s1).classList().set("s1");
	NodeHandle(s2).classList().set("s2");
	Tree::instance().computeLayout(box, 200, 100);

	const auto &a = Tree::instance().node(s1);
	const auto &b = Tree::instance().node(s2);
	std::fprintf(stderr, "[css_block_flexbasis] inline: s1=(%d,%d %dx%d) s2=(%d,%d %dx%d)\n",
	             a.layout.x, a.layout.y, a.layout.width, a.layout.height,
	             b.layout.x, b.layout.y, b.layout.width, b.layout.height);
	// They share a BASELINE, not a top edge. An inline-block with no in-flow line
	// box takes its baseline from its bottom margin edge, so two empty spans of
	// different heights align along their bottoms — Chrome puts these same two at
	// s1=(0,3 40x12) s2=(40,5 60x10), a +2 offset the engine reproduces exactly.
	expectEqual(a.layout.y + a.layout.height, b.layout.y + b.layout.height,
	            "inline: default spans share a baseline (row)");
	expectTrue(b.layout.x >= a.layout.x + a.layout.width, "inline: s2 to the right of s1 (row)");
}

// Native Gea uses simple block/inline heuristics for non-flex containers. A
// vertical margin on a span means the author expects a stacked label, as in Sky
// Hop's win overlay: "Course Clear" above "Coins 2/8".
void testVerticalMarginSpanStacks()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(320, 160, 1.0);

	const int panel = Tree::instance().createView();
	Tree::instance().setTagName(panel, "div");
	NodeHandle(panel).classList().set("panel");
	const int title = makeSpan(panel, "title");
	const int score = makeSpan(panel, "score");
	Tree::instance().mount(panel, 320, 160);

	StyleSheet::instance().registerRule("panel", "width", "316");
	StyleSheet::instance().registerRule("panel", "height", "116");
	StyleSheet::instance().registerRule("panel", "align-items", "center");
	StyleSheet::instance().registerRule("panel", "justify-content", "center");
	StyleSheet::instance().registerRule("title", "width", "120");
	StyleSheet::instance().registerRule("title", "height", "30");
	StyleSheet::instance().registerRule("title", "margin-bottom", "14");
	StyleSheet::instance().registerRule("score", "width", "64");
	StyleSheet::instance().registerRule("score", "height", "16");
	NodeHandle(panel).classList().set("panel");
	NodeHandle(title).classList().set("title");
	NodeHandle(score).classList().set("score");
	Tree::instance().computeLayout(panel, 320, 160);

	const auto &t = Tree::instance().node(title);
	const auto &s = Tree::instance().node(score);
	std::fprintf(stderr, "[css_block_flexbasis] vertical-margin: title=(%d,%d %dx%d) score=(%d,%d %dx%d)\n",
	             t.layout.x, t.layout.y, t.layout.width, t.layout.height,
	             s.layout.x, s.layout.y, s.layout.width, s.layout.height);
	expectEqual(t.layout.x + t.layout.width / 2, s.layout.x + s.layout.width / 2,
	            "vertical-margin: stacked spans share horizontal center");
	expectTrue(s.layout.y >= t.layout.y + t.layout.height + 14,
	           "vertical-margin: score below title with margin gap");
}

// BUG #2: `flex: 0 0 26px` on a display:grid flex item must size the item to 26px
// (its flex-basis), not stretch it to fill the row. This is the weather forecast's
// .hour / .day cards.
void testFlexBasisFixedWidth()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(400, 120, 1.0);

	const int row = makeDiv(-1, "frow");
	const int card1 = makeDiv(row, "fcard");
	makeDiv(card1, "finner");
	const int card2 = makeDiv(row, "fcard");
	makeDiv(card2, "finner");
	Tree::instance().mount(row, 400, 120);

	StyleSheet::instance().registerRule("frow", "display", "flex");
	StyleSheet::instance().registerRule("frow", "width", "300");
	StyleSheet::instance().registerRule("frow", "height", "80");
	StyleSheet::instance().registerRule("fcard", "display", "grid");
	StyleSheet::instance().registerRule("fcard", "flex", "0 0 26px");
	StyleSheet::instance().registerRule("finner", "width", "20");
	StyleSheet::instance().registerRule("finner", "height", "40");
	NodeHandle(row).classList().set("frow");
	NodeHandle(card1).classList().set("fcard");
	NodeHandle(card2).classList().set("fcard");
	Tree::instance().computeLayout(row, 400, 120);

	const auto &c1 = Tree::instance().node(card1);
	const auto &c2 = Tree::instance().node(card2);
	std::fprintf(stderr, "[css_block_flexbasis] flexbasis: card1=(%d,%d %dx%d) card2=(%d,%d %dx%d)\n",
	             c1.layout.x, c1.layout.y, c1.layout.width, c1.layout.height,
	             c2.layout.x, c2.layout.y, c2.layout.width, c2.layout.height);
	expectEqual(c1.layout.width, 26, "flexbasis: card1 width == basis 26px");
	expectEqual(c2.layout.width, 26, "flexbasis: card2 width == basis 26px");
	expectEqual(c2.layout.x, 26, "flexbasis: card2 sits right after card1 (row of fixed cards)");
}

void testDeferredFlexBasis()
{
	for (bool column : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(500, 500, 1.0);
		auto &tree = Tree::instance();
		StyleSheet::instance().registerRule("basis", "flex-basis", "50%");
		StyleSheet::instance().registerRule("basis", "flex-shrink", "0");
		const int root = makeDiv(-1, nullptr), parent = makeDiv(root, nullptr);
		const int item = makeDiv(-1, "basis"); // Styled before attachment.
		NodeHandle(parent).appendChild(NodeHandle(item));
		auto container = NodeHandle(parent).style(), child = NodeHandle(item).style();
		container.setProperty("display", "flex"); container.setProperty("flex-direction", column ? "column" : "row");
		container.setProperty("width", "240px"); container.setProperty("height", "340px");
		container.setProperty("box-sizing", "border-box"); container.setProperty("padding", "20px");
		child.setProperty(column ? "height" : "width", "17px");
		child.setProperty(column ? "min-height" : "min-width", "0");
		child.setProperty("padding", "5px");
		tree.mount(root, 500, 500);
		auto measure = [&]() { tree.computeLayout(root, 500, 500); return column ? tree.node(item).layout.height : tree.node(item).layout.width; };
		expectEqual(measure(), column ? 160 : 110, "percent basis uses inner physical main size and overrides preferred size");
		container.setProperty(column ? "height" : "width", "440px");
		expectEqual(measure(), 210, "cached percent basis follows resize");
		child.setProperty("flex-basis", "calc(25% + 10px)");
		expectEqual(measure(), 120, "inline calc basis uses container content size");
		child.setProperty("box-sizing", "border-box");
		expectEqual(measure(), 110, "border-box flex basis includes padding");
		child.setProperty("flex", "0 0 calc(50% - 10px)");
		expectEqual(measure(), 190, "function-aware flex shorthand resolves percentage on main axis");
		child.setProperty("flex-basis", "-10%");
		expectEqual(measure(), 190, "negative percentage basis declaration is ignored");
		child.setProperty("flex-basis", "40px");
		expectEqual(measure(), 40, "pixel basis replaces deferred inline basis");
		child.removeProperty("flex-basis");
		expectEqual(measure(), 200, "removing inline basis restores cached percentage class");
		child.setProperty("flex-basis", "auto");
		expectEqual(measure(), 17, "auto basis restores preferred main size");
	}
	for (bool shorthand : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance(); auto &sheet = StyleSheet::instance();
		if (shorthand) sheet.registerStaticFlexRule(StaticStyleSelectorKind::Class, "static-basis", 0, {StaticStyleLengthUnit::Percent, 75}, true);
		else sheet.registerStaticLengthRule(StaticStyleSelectorKind::Class, "static-basis", StaticStyleLengthProperty::FlexBasis, StaticStyleLengthUnit::Percent, 75);
		const int root = makeDiv(-1, nullptr), parent = makeDiv(root, nullptr), item = makeDiv(parent, "static-basis");
		NodeHandle(parent).style().setProperty("display", "flex");
		NodeHandle(parent).style().setProperty("writing-mode", "vertical-rl");
		NodeHandle(parent).style().setProperty("width", "100px");
		NodeHandle(parent).style().setProperty("height", "240px");
		NodeHandle(item).style().setProperty("height", "5px");
		tree.mount(root, 400, 400); tree.computeLayout(root, 400, 400);
		expectEqual(tree.node(item).layout.height, 180, "static percentage basis follows physical axis in vertical writing mode");
		NodeHandle(item).style().setProperty("flex-basis", "calc(var(--basis, 50%) - 10px)");
		tree.computeLayout(root, 400, 400);
		expectEqual(tree.node(item).layout.height, 110, "custom property fallback preserves percentage basis");
		NodeHandle(parent).style().setProperty("--basis", "25%");
		tree.computeLayout(root, 400, 400);
		expectEqual(tree.node(item).layout.height, 50, "inherited custom property updates percentage flex basis");
		NodeHandle(item).style().removeProperty("flex-basis");
		const int sibling = makeDiv(parent, "static-basis");
		tree.computeLayout(root, 400, 400);
		expectEqual(tree.node(item).layout.height, 120, "static flex basis retains default shrink when bases overflow");
		expectEqual(tree.node(sibling).layout.height, 120, "cached static flex basis shrinks sibling equally");
	}
	// Auto-height column: percentages (including zero percent and calc) use
	// content, not the viewport and not the item's authored height.
	for (const char *basis : {"50%", "0%", "calc(50% + 10px)"}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), parent = makeDiv(root, nullptr), item = makeDiv(parent, nullptr), inner = makeDiv(item, nullptr);
		NodeHandle(parent).style().setProperty("display", "flex");
		NodeHandle(parent).style().setProperty("flex-direction", "column");
		NodeHandle(parent).style().setProperty("width", "100px");
		NodeHandle(item).style().setProperty("height", "80px");
		NodeHandle(item).style().setProperty("flex-basis", basis);
		NodeHandle(inner).style().setProperty("height", "30px");
		tree.mount(root, 300, 300); tree.computeLayout(root, 300, 300);
		expectEqual(tree.node(item).layout.height, 30, "indefinite percentage basis uses content instead of preferred height");
		expectEqual(tree.node(parent).layout.height, 30, "auto column remains content sized");
	}
}

// BUG #3: CSS specificity. A more-specific descendant selector (`.box .row`,
// registered EARLY) must beat a less-specific base rule (`.row`, registered LATE)
// for the same property — real CSS resolves by specificity, not source order. This
// is what makes the weather forecast's `.forecast.show-days .hour-row { display:none }`
// actually hide the hour row when the base `.hour-row { display:flex }` comes later.
void testSpecificityDescendantBeatsBase()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = Tree::instance().createView();
	Tree::instance().setTagName(box, "div");
	NodeHandle(box).classList().set("box");
	const int row = makeDiv(box, "row");
	Tree::instance().mount(box, 200, 100);

	// Register the MORE-specific descendant rule FIRST, the base rule LAST — so a
	// pure source-order cascade would (wrongly) let the base win.
	StyleSheet::instance().registerSelectorRule(".box .row", "display", "none");
	StyleSheet::instance().registerRule("row", "display", "flex");
	StyleSheet::instance().registerRule("row", "width", "50");
	StyleSheet::instance().registerRule("row", "height", "20");
	NodeHandle(box).classList().set("box");
	NodeHandle(row).classList().set("row");
	Tree::instance().computeLayout(box, 200, 100);

	const int display = Tree::instance().node(row).style.display;
	std::fprintf(stderr, "[css_block_flexbasis] specificity: .row display=%d (expect %d=none)\n",
	             display, kDisplayNone);
	expectEqual(display, kDisplayNone, "specificity: descendant .box .row beats base .row");
}

// BUG #4: selectors with more than two descendant parts must walk the full
// ancestor chain. `.vn-menu .vn-menu-list div` in voice-notes used to register
// correctly but never match the row divs because the matcher skipped the middle
// selector part incorrectly.
void testThreePartDescendantSelector()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int outer = makeDiv(-1, "outer");
	const int middle = makeDiv(outer, "middle");
	const int row = Tree::instance().createView();
	NodeHandle(middle).appendChild(NodeHandle(row));
	Tree::instance().setTagName(row, "div");
	Tree::instance().mount(outer, 200, 100);

	StyleSheet::instance().registerSelectorRule(".outer .middle div", "display", "flex");
	StyleSheet::instance().registerSelectorRule(".outer .middle div", "width", "70");
	StyleSheet::instance().registerSelectorRule(".outer .middle div", "height", "12");
	NodeHandle(outer).classList().set("outer");
	NodeHandle(middle).classList().set("middle");
	Tree::instance().computeLayout(outer, 200, 100);

	const auto &r = Tree::instance().node(row);
	std::fprintf(stderr, "[css_block_flexbasis] descendant3: row display=%d box=(%d,%d %dx%d)\n",
	             r.style.display, r.layout.x, r.layout.y, r.layout.width, r.layout.height);
	expectEqual(r.style.display, kDisplayFlex, "descendant3: .outer .middle div display applies");
	expectEqual(r.layout.width, 70, "descendant3: .outer .middle div width applies");
	expectEqual(r.layout.height, 12, "descendant3: .outer .middle div height applies");
}

void testRootAndIdSelectors()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	Tree::instance().setAttribute(root, "id", "app");
	const int special = makeDiv(root, nullptr);
	Tree::instance().setAttribute(special, "id", "special");
	const int other = makeDiv(root, nullptr);
	Tree::instance().setAttribute(other, "id", "other");
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerSelectorRule(":root", "width", "123");
	StyleSheet::instance().registerSelectorRule(":root", "height", "80");
	StyleSheet::instance().registerSelectorRule("#special", "width", "45");
	StyleSheet::instance().registerSelectorRule("#special", "height", "20");
	StyleSheet::instance().registerSelectorRule("#other", "display", "none");
	StyleSheet::instance().recomputeSubtree(root);
	Tree::instance().computeLayout(root, 200, 100);

	const auto &rootNode = Tree::instance().node(root);
	const auto &specialNode = Tree::instance().node(special);
	const auto &otherNode = Tree::instance().node(other);
	std::fprintf(stderr,
	             "[css_block_flexbasis] root-id: root=%dx%d special=%dx%d otherDisplay=%d\n",
	             rootNode.style.width,
	             rootNode.style.height,
	             specialNode.style.width,
	             specialNode.style.height,
	             otherNode.style.display);
	expectEqual(rootNode.style.width, 123, "root-id: :root width applies to root");
	expectEqual(rootNode.style.height, 80, "root-id: :root height applies to root");
	expectEqual(specialNode.style.width, 45, "root-id: #special width applies");
	expectEqual(specialNode.style.height, 20, "root-id: #special height applies");
	expectEqual(otherNode.style.display, kDisplayNone, "root-id: #other display applies");
}

void testCandidateCacheKeepsAncestorSelectorsDynamic()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(160, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int activeGroup = makeDiv(root, "active");
	const int activeItem = makeDiv(activeGroup, "item");
	const int plainGroup = makeDiv(root, nullptr);
	const int plainItem = makeDiv(plainGroup, "item");
	Tree::instance().mount(root, 160, 100);

	StyleSheet::instance().registerRule("item", "width", "30");
	StyleSheet::instance().registerRule("item", "height", "10");
	StyleSheet::instance().registerSelectorRule(".active .item", "width", "90");
	StyleSheet::instance().recomputeSubtree(root);

	expectEqual(Tree::instance().node(activeItem).style.width, 90, "candidate-cache: ancestor selector applies under .active");
	expectEqual(Tree::instance().node(plainItem).style.width, 30, "candidate-cache: ancestor selector rejected outside .active");

	NodeHandle(activeGroup).classList().set("");
	NodeHandle(plainGroup).classList().set("active");

	expectEqual(Tree::instance().node(activeItem).style.width, 30, "candidate-cache: removed ancestor stops matching");
	expectEqual(Tree::instance().node(plainItem).style.width, 90, "candidate-cache: added ancestor starts matching");
}

void testCachedGridTemplateRulesApply()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int first = makeDiv(root, nullptr);
	const int second = makeDiv(root, nullptr);
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerRule("gridy", "display", "grid");
	StyleSheet::instance().registerRule("gridy", "grid-template-columns", "repeat(2, 20px) 1fr auto");
	StyleSheet::instance().registerRule("gridy", "grid-template-rows", "12px minmax(4px, 2fr)");
	NodeHandle(first).classList().set("gridy");
	NodeHandle(second).classList().set("gridy");

	const RareStyle &rs = rstyle(Tree::instance().node(second).style);
	expectEqual(Tree::instance().node(second).style.display, kDisplayGrid, "grid-template-cache: display applies");
	expectEqual(rs.grid_column_count, 4, "grid-template-cache: column count");
	expectEqual(rs.grid_column_type[0], 1, "grid-template-cache: column 0 fixed");
	expectEqual(rs.grid_column_type[1], 1, "grid-template-cache: column 1 fixed");
	expectEqual(rs.grid_column_type[2], 2, "grid-template-cache: column 2 fr");
	expectEqual(rs.grid_column_type[3], 0, "grid-template-cache: column 3 auto");
	expectEqual(rs.grid_column_value[0], 20, "grid-template-cache: column 0 px");
	expectEqual(rs.grid_column_value[1], 20, "grid-template-cache: column 1 px");
	expectEqual(rs.grid_column_value[2], 1, "grid-template-cache: column 2 fr value");
	expectEqual(rs.grid_row_count, 2, "grid-template-cache: row count");
	expectEqual(rs.grid_row_type[0], 1, "grid-template-cache: row 0 fixed");
	expectEqual(rs.grid_row_type[1], 2, "grid-template-cache: row 1 fr");
	expectEqual(rs.grid_row_value[0], 12, "grid-template-cache: row 0 px");
	expectEqual(rs.grid_row_value[1], 2, "grid-template-cache: row 1 fr value");
}

void testStaticCustomLengthExpressionPreResolves()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = makeDiv(-1, "cube-wrap");
	Tree::instance().mount(box, 200, 100);

	StyleSheet::instance().registerRule("cube-wrap", "--cube-size", "clamp(20px, 50vw, 80px)");
	StyleSheet::instance().registerRule("cube-wrap", "width", "var(--cube-size)");
	StyleSheet::instance().registerRule("cube-wrap", "height", "calc(var(--cube-size) / 2)");
	NodeHandle(box).classList().set("cube-wrap");
	Tree::instance().computeLayout(box, 200, 100);

	const NodeCustomProperty *entry = customPropertyForNode(box, "--cube-size");
	expectTrue(entry && entry->hasLength(), "custom-length: clamp custom prop stored as length");
	if (entry && entry->hasLength()) {
		expectEqual(entry->lengthUnit, 1, "custom-length: clamp pre-resolved to raw internal px");
		expectEqual(static_cast<int>(entry->lengthValue), 80, "custom-length: initial resolved raw value");
	}
	expectEqual(Tree::instance().node(box).style.width, 80, "custom-length: var(width) uses pre-resolved clamp");
	expectEqual(Tree::instance().node(box).style.height, 40, "custom-length: calc(var / 2) uses pre-resolved clamp");

	setViewportMetrics(60, 100, 1.0);
	Tree::instance().computeLayout(box, 60, 100);
	entry = customPropertyForNode(box, "--cube-size");
	expectTrue(entry && entry->hasLength(), "custom-length: clamp survives viewport recompute");
	if (entry && entry->hasLength()) {
		expectEqual(entry->lengthUnit, 1, "custom-length: viewport recompute remains raw");
		expectEqual(static_cast<int>(entry->lengthValue), 30, "custom-length: viewport recompute updates raw value");
	}
	expectEqual(Tree::instance().node(box).style.width, 30, "custom-length: viewport recompute updates width");
	expectEqual(Tree::instance().node(box).style.height, 15, "custom-length: viewport recompute updates calc");
}

void testPercentCustomLengthStaysDynamic()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int parent = makeDiv(-1, "parent");
	const int child = makeDiv(parent, "pct");
	Tree::instance().mount(parent, 200, 100);

	StyleSheet::instance().registerRule("parent", "width", "120");
	StyleSheet::instance().registerRule("parent", "height", "40");
	StyleSheet::instance().registerRule("pct", "--w", "50%");
	StyleSheet::instance().registerRule("pct", "width", "var(--w)");
	StyleSheet::instance().registerRule("pct", "height", "10");
	NodeHandle(parent).classList().set("parent");
	NodeHandle(child).classList().set("pct");
	Tree::instance().computeLayout(parent, 200, 100);

	const NodeCustomProperty *entry = customPropertyForNode(child, "--w");
	expectTrue(entry && entry->hasLength(), "custom-length: percent custom prop stored as length");
	if (entry && entry->hasLength()) {
		expectEqual(entry->lengthUnit, 3, "custom-length: percent custom prop stays percent");
	}
	expectEqual(Tree::instance().node(child).style.width_percent, 500, "custom-length: percent var uses percent style slot");
	expectEqual(Tree::instance().node(child).layout.width, 60, "custom-length: percent var resolves against parent width in layout");
}

void testStaticLengthExpressionCacheInvalidatesWithViewport()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int first = makeDiv(root, "sized");
	const int second = makeDiv(root, "sized");
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerRule("sized", "width", "clamp(20px, 50vw, 80px)");
	StyleSheet::instance().registerRule("sized", "height", "10");
	NodeHandle(first).classList().set("sized");
	NodeHandle(second).classList().set("sized");
	Tree::instance().computeLayout(root, 200, 100);

	expectEqual(Tree::instance().node(first).style.width, 80, "static-length-cache: initial first width");
	expectEqual(Tree::instance().node(second).style.width, 80, "static-length-cache: initial sibling width");

	setViewportMetrics(60, 100, 1.0);
	Tree::instance().computeLayout(root, 60, 100);

	expectEqual(Tree::instance().node(first).style.width, 30, "static-length-cache: viewport updates first width");
	expectEqual(Tree::instance().node(second).style.width, 30, "static-length-cache: viewport updates sibling width");
}

void testActiveRuleCollapseKeepsCascadeSemantics()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int root = makeDiv(-1, nullptr);
	const int box = makeDiv(root, "box override scrollx");
	Tree::instance().mount(root, 200, 100);

	StyleSheet::instance().registerRule("box", "width", "10");
	StyleSheet::instance().registerRule("box", "height", "12");
	StyleSheet::instance().registerRule("box", "overflow", "hidden");
	StyleSheet::instance().registerRule("box", "transform", "rotateX(30deg) translateZ(8px) scale(2)");
	StyleSheet::instance().registerSelectorRule(".box.override", "width", "50%");
	StyleSheet::instance().registerSelectorRule(".box.override", "transform", "translateZ(4px)");
	StyleSheet::instance().registerSelectorRule(".box.scrollx", "overflow-x", "scroll");
	NodeHandle(box).classList().set("box override scrollx");
	Tree::instance().computeLayout(root, 200, 100);

	const auto &node = Tree::instance().node(box);
	expectEqual(node.style.width, kUnset, "rule-collapse: later percent width clears earlier px width");
	expectEqual(node.style.width_percent, 500, "rule-collapse: later percent width wins");
	expectEqual(node.style.height, 12, "rule-collapse: unrelated earlier height remains");
	expectEqual(node.style.overflow_x, 2, "rule-collapse: later overflow-x wins");
	expectEqual(node.style.overflow_y, 1, "rule-collapse: earlier overflow-y from shorthand remains");
	expectEqual(node.style.overflow, 2, "rule-collapse: aggregate overflow sees both axes");
	expectEqual(rstyle(node.style).transform_rotate_x, 0, "rule-collapse: later transform resets earlier rotateX");
	expectEqual(rstyle(node.style).transform_translate_z, 4, "rule-collapse: later transform translateZ wins");
	expectEqual(rstyle(node.style).transform_scale_x, 1000, "rule-collapse: later transform resets earlier scaleX");
	expectEqual(rstyle(node.style).transform_scale_y, 1000, "rule-collapse: later transform resets earlier scaleY");
}

void testCustomPropertyLookupCacheInvalidates()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int parent = makeDiv(-1, "theme small");
	const int child = makeDiv(parent, "uses");
	const int sibling = makeDiv(parent, "uses");
	const int overrideBranch = makeDiv(parent, "override");
	const int overrideChild = makeDiv(overrideBranch, "uses");
	const int missingFallback = makeDiv(parent, "missing-fallback");
	const int missingNoFallback = makeDiv(parent, "missing-no-fallback");
	const int invalidBranch = makeDiv(parent, "invalid");
	const int invalidChild = makeDiv(invalidBranch, "invalid-color");
	Tree::instance().mount(parent, 200, 100);

	StyleSheet::instance().registerRule("small", "--s", "30px");
	StyleSheet::instance().registerRule("small", "--c", "#ff0000");
	StyleSheet::instance().registerRule("large", "--s", "70px");
	StyleSheet::instance().registerRule("large", "--c", "#0000ff");
	StyleSheet::instance().registerRule("override", "--s", "45px");
	StyleSheet::instance().registerRule("override", "--c", "#00ff00");
	StyleSheet::instance().registerRule("uses", "width", "var(--s)");
	StyleSheet::instance().registerRule("uses", "height", "var(--s)");
	StyleSheet::instance().registerRule("uses", "color", "var(--c, #000000)");
	StyleSheet::instance().registerRule("missing-fallback", "color", "var(--missing-c, #00ff00)");
	StyleSheet::instance().registerRule("missing-no-fallback", "color", "var(--missing-c)");
	StyleSheet::instance().registerRule("invalid", "--bad-c", "not-a-color");
	StyleSheet::instance().registerRule("invalid-color", "color", "var(--bad-c, #ff0000)");
	NodeHandle(parent).classList().set("theme small");
	NodeHandle(child).classList().set("uses");
	NodeHandle(sibling).classList().set("uses");
	NodeHandle(overrideBranch).classList().set("override");
	NodeHandle(overrideChild).classList().set("uses");
	NodeHandle(missingFallback).classList().set("missing-fallback");
	NodeHandle(missingNoFallback).classList().set("missing-no-fallback");
	NodeHandle(invalidBranch).classList().set("invalid");
	NodeHandle(invalidChild).classList().set("invalid-color");
	Tree::instance().computeLayout(parent, 200, 100);

	expectEqual(Tree::instance().node(child).style.width, 30, "custom-cache: first inherited width");
	expectEqual(Tree::instance().node(child).style.height, 30, "custom-cache: first inherited height");
	expectEqual(Tree::instance().node(sibling).style.width, 30, "custom-cache: sibling inherited width");
	expectEqual(Tree::instance().node(overrideChild).style.width, 45, "custom-cache: child override shadows inherited width");
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0xf800), "custom-cache: inherited color var resolves red");
	expectEqual(Tree::instance().node(sibling).style.text_color, nativeRgb565(0xf800), "custom-cache: sibling inherited color var resolves red");
	expectEqual(Tree::instance().node(overrideChild).style.text_color, nativeRgb565(0x07e0), "custom-cache: child override color resolves green");
	expectEqual(Tree::instance().node(missingFallback).style.text_color, nativeRgb565(0x07e0), "custom-cache: missing color var uses fallback");
	expectEqual(Tree::instance().node(missingNoFallback).style.text_color, nativeRgb565(0xffff), "custom-cache: missing color var without fallback resolves white");
	expectEqual(Tree::instance().node(invalidChild).style.text_color, nativeRgb565(0xffff), "custom-cache: invalid inherited color var resolves white");

	NodeHandle(parent).classList().set("theme large");
	Tree::instance().computeLayout(parent, 200, 100);

	expectEqual(Tree::instance().node(child).style.width, 70, "custom-cache: inherited width updates after parent custom change");
	expectEqual(Tree::instance().node(child).style.height, 70, "custom-cache: inherited height updates after parent custom change");
	expectEqual(Tree::instance().node(sibling).style.width, 70, "custom-cache: sibling inherited width updates");
	expectEqual(Tree::instance().node(overrideChild).style.width, 45, "custom-cache: override still shadows parent change");
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0x001f), "custom-cache: inherited color updates blue");
	expectEqual(Tree::instance().node(sibling).style.text_color, nativeRgb565(0x001f), "custom-cache: sibling inherited color updates blue");
	expectEqual(Tree::instance().node(overrideChild).style.text_color, nativeRgb565(0x07e0), "custom-cache: override still shadows parent color");
	expectEqual(Tree::instance().node(missingFallback).style.text_color, nativeRgb565(0x07e0), "custom-cache: missing fallback color remains green");
	expectEqual(Tree::instance().node(missingNoFallback).style.text_color, nativeRgb565(0xffff), "custom-cache: missing no-fallback color remains white");
	expectEqual(Tree::instance().node(invalidChild).style.text_color, nativeRgb565(0xffff), "custom-cache: invalid color remains white");
}

void testStaticCustomColorPrecompiles()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int parent = makeDiv(-1, "theme-red");
	const int child = makeDiv(parent, "uses-color");
	Tree::instance().mount(parent, 200, 100);

	StyleSheet::instance().registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
	                                                     "theme-red",
	                                                     "--ink",
	                                                     255,
	                                                     0,
	                                                     0,
	                                                     255);
	StyleSheet::instance().registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
	                                                     "theme-blue",
	                                                     "--ink",
	                                                     0,
	                                                     0,
	                                                     255,
	                                                     255);
	StyleSheet::instance().registerRule("uses-color", "color", "var(--ink)");
	NodeHandle(parent).classList().set("theme-red");
	NodeHandle(child).classList().set("uses-color");
	Tree::instance().computeLayout(parent, 200, 100);

	const NodeCustomProperty *entry = customPropertyForNode(parent, "--ink");
	expectTrue(entry && entry->hasColor(), "static-custom-color: custom prop stored as color");
	if (entry && entry->hasColor()) {
		expectEqual(static_cast<int>(entry->colorNative), nativeRgb565(0xf800), "static-custom-color: stored red native color");
		expectEqual(entry->colorAlpha, 255, "static-custom-color: stored alpha");
	}
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0xf800), "static-custom-color: var resolves red");

	NodeHandle(parent).classList().set("theme-blue");
	Tree::instance().computeLayout(parent, 200, 100);
	entry = customPropertyForNode(parent, "--ink");
	expectTrue(entry && entry->hasColor(), "static-custom-color: color survives class update");
	if (entry && entry->hasColor()) {
		expectEqual(static_cast<int>(entry->colorNative), nativeRgb565(0x001f), "static-custom-color: stored blue native color");
	}
	expectEqual(Tree::instance().node(child).style.text_color, nativeRgb565(0x001f), "static-custom-color: var resolves blue");
}

void testBackgroundColorClipping()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(100, 100, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().backgroundColor(0xffff);
	NodeHandle(root).style().width(100); NodeHandle(root).style().height(100);
	const int child = makeDiv(root, nullptr);
	auto style = NodeHandle(child).style();
	style.setProperty("position", "absolute");
	style.setProperty("left", "20px"); style.setProperty("top", "20px");
	style.width(20); style.height(20);
	style.setProperty("padding", "10px");
	style.setProperty("border", "5px solid transparent");
	style.setProperty("background-color", "#ff0000");
	tree.mount(root, 100, 100);
	auto rectangle = [&](int inset, const char *message, int dx = 0) {
		tree.refresh(root, 100, 100);
		int wrong = 0;
		for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) {
			const bool inside = x >= 20 + inset + dx && x < 70 - inset + dx && y >= 20 + inset && y < 70 - inset;
			if (displayPixelAt(x, y) != nativeRgb565(inside ? 0xf800 : 0xffff)) ++wrong;
		}
		expectEqual(wrong, 0, message);
	};
	rectangle(0, "default color clip is the border box");
	style.setProperty("background-clip", "padding-box");
	rectangle(5, "padding-box excludes transparent border");
	style.setProperty("background-clip", "content-box");
	rectangle(15, "content-box excludes padding and border");
	for (const char *invalid : {"padding-box banana", "content-box,", "none", "padding-box content-box"}) {
		style.setProperty("background-clip", invalid);
		rectangle(15, "invalid clip list leaves the earlier value intact");
	}
	style.setProperty("background-clip", "border-box, content-box, padding-box");
	style.setProperty("background-image", "none, none");
	rectangle(15, "color uses bottom image clip and ignores excess clip values");
	style.setProperty("background-image", "none");
	rectangle(0, "image layer mutation changes the color clip");
	style.setProperty("background-clip", "border-box, content-box");
	style.setProperty("background-image", "none, none, none, none");
	rectangle(15, "short clip lists repeat to the bottom image layer");
	style.setProperty("background", "#ff0000");
	rectangle(0, "color shorthand resets the clip list");
	style.setProperty("background", "#ff0000 padding-box content-box");
	rectangle(15, "shorthand second box controls clipping");
	style.setProperty("background", "#ff0000 padding-box");
	rectangle(5, "shorthand single box also controls clipping");
	style.removeProperty("background");
	style.setProperty("background-color", "#ff0000");
	StyleSheet::instance().registerRule("clipped", "background-clip", "content-box");
	NodeHandle(child).classList().set("clipped");
	rectangle(15, "cached class clip applies after inline shorthand removal");
	style.setProperty("background-clip", "initial");
	rectangle(0, "initial overrides cached clip");
	style.removeProperty("background-clip");
	rectangle(15, "removing inline clip restores cached rule");
	style.setProperty("transform", "translateX(5px)");
	rectangle(15, "clipped background follows transformed geometry", 5);
	style.setProperty("transform", "none");
	style.setProperty("border-radius", "25px");
	tree.refresh(root, 100, 100);
	expectEqual(displayPixelAt(35, 35), nativeRgb565(0xffff), "content curve excludes its corner");
	expectEqual(displayPixelAt(45, 45), nativeRgb565(0xf800), "content curve retains its center");
	expectEqual(displayPixelAt(45, 35), nativeRgb565(0xf800), "content radius subtracts border plus padding");
	style.setProperty("border-radius", "0");
	style.setProperty("background-image", "linear-gradient(transparent, transparent), none");
	style.setProperty("background-clip", "border-box, content-box");
	rectangle(15, "transparent gradient preserves bottom-layer color clipping");
	style.setProperty("background-image", "none");
	style.setProperty("background-clip", "content-box");
	style.setProperty("left", "0"); style.setProperty("top", "0");
	style.width(50); style.height(50);
	style.setProperty("padding", "0");
	style.setProperty("border", "25px solid transparent");
	style.setProperty("border-radius", "100% 0 0 0");
	tree.refresh(root, 100, 100);
	expectEqual(displayPixelAt(50, 30), nativeRgb565(0xffff), "inner radius is not reclamped to half the content box");
	expectEqual(displayPixelAt(70, 70), nativeRgb565(0xf800), "large inner radius retains its curved interior");
	style.setProperty("border-color", "#0000ff");
	tree.refresh(root, 100, 100);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0xffff), "percentage border curve excludes outer corner");
	expectEqual(displayPixelAt(90, 90), nativeRgb565(0x001f), "percentage border paints outside the content box");
	expectEqual(displayPixelAt(70, 70), nativeRgb565(0xf800), "percentage border preserves content background");
	style.setProperty("background-clip", "border-box");
	tree.refresh(root, 100, 100);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0xffff), "large outer radius is not reclamped by the canvas primitive");
	style.setProperty("background-color", "#00ff00");
	tree.refresh(root, 100, 100);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0xffff), "recolor preserves the excluded outer curve");
	expectEqual(displayPixelAt(90, 90), nativeRgb565(0x001f), "recolor preserves a different-colored CSS border");
	expectEqual(displayPixelAt(70, 70), nativeRgb565(0x07e0), "recolor changes only the background within the border");


}

void testDocumentCanvasBackground()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(80, 80, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).setTagName("html");
	const int body = makeDiv(root, nullptr);
	NodeHandle(body).setTagName("body");
	auto html = NodeHandle(root).style();
	auto content = NodeHandle(body).style();
	html.setProperty("display", "block");
	html.width(20); html.height(20); html.setProperty("margin", "20px");
	html.setProperty("background-color", "#00ff00");
	content.width(10); content.height(10); content.setProperty("background-color", "#ff0000");
	tree.mount(root, 80, 80);
	auto refresh = [&]() { tree.refresh(root, 80, 80); };
	auto corners = [&](int rgb, const char *message) {
		for (const auto &point : {std::pair<int,int>{0, 0}, {79, 0}, {0, 79}, {79, 79}})
			expectEqual(displayPixelAt(point.first, point.second), nativeRgb565(rgb), message);
	};
	corners(0x07e0, "root background covers canvas beyond root margins and size");
	expectEqual(displayPixelAt(tree.node(body).layout.x + 2, tree.node(body).layout.y + 2), nativeRgb565(0xf800), "opaque root leaves body background local");
	html.setProperty("background-color", "transparent");
	refresh();
	corners(0xf800, "transparent root propagates body color to canvas on refresh");
	content.setProperty("background-color", "#0000ff");
	refresh();
	corners(0x001f, "body recolor invalidates the entire canvas");
	content.setProperty("background-color", "rgba(0,255,0,0.5)");
	refresh();
	expectEqual(displayPixelAt(tree.node(body).layout.x + 2, tree.node(body).layout.y + 2), displayPixelAt(79, 79), "propagated translucent body color is painted only once");
	content.setProperty("background-color", "#ff0000");
	content.setProperty("opacity", "0");
	refresh();
	corners(0xf800, "body opacity does not apply to its propagated canvas background");
	content.setProperty("display", "none");
	refresh();
	corners(0xffff, "display-none background source reveals the UA canvas");
	content.setProperty("display", "block");
	content.setProperty("opacity", "1");
	content.height(0); html.height(0);
	refresh();
	corners(0xf800, "zero-height document still paints the canvas");
	html.setProperty("background-image", "linear-gradient(transparent, transparent)");
	refresh();
	corners(0xffff, "root image prevents body background propagation even if transparent");
	html.setProperty("background", "#0000ff");
	html.setProperty("opacity", "0");
	refresh();
	corners(0xffff, "hidden root reveals the UA canvas without stale paint");
	html.setProperty("opacity", "0.5");
	refresh();
	expectTrue(displayPixelAt(79, 79) != nativeRgb565(0xffff) && displayPixelAt(79, 79) != nativeRgb565(0x001f), "root opacity applies to canvas background");
	html.setProperty("opacity", "1");
	html.setProperty("position", "relative");
	html.setProperty("left", "15px");
	refresh();
	corners(0x001f, "moving the root box does not move uniform canvas paint");
	html.setProperty("background", "none");
	content.setProperty("background", "none");
	const int child = makeDiv(body, nullptr);
	auto childStyle = NodeHandle(child).style();
	childStyle.setProperty("position", "fixed");
	childStyle.setProperty("left", "5px"); childStyle.setProperty("top", "5px");
	childStyle.width(10); childStyle.height(10); childStyle.setProperty("background", "#ff0000");
	refresh();
	expectEqual(displayPixelAt(8, 8), nativeRgb565(0xf800), "child paints above UA canvas");
	childStyle.setProperty("background-color", "transparent");
	refresh();
	expectEqual(displayPixelAt(8, 8), nativeRgb565(0xffff), "retained child removal repaints UA canvas below it");
	html.height(20); content.height(10);
	content.setProperty("background", "#0000ff");
	for (auto target : {html, content}) {
		for (const char *value : {"paint", "layout", "style", "size", "inline-size", "content", "strict", "layout paint"}) {
			target.setProperty("contain", value);
			refresh();
			corners(0xffff, "containment on html or body prevents propagation");
			expectEqual(displayPixelAt(tree.node(body).layout.x + 2, tree.node(body).layout.y + 2), nativeRgb565(0x001f), "contained body keeps local color");
		}
		for (const char *invalid : {"paint none", "paint paint", "size inline-size", "banana"}) {
			target.setProperty("contain", invalid); refresh();
			corners(0xffff, "invalid containment preserves previous declaration");
		}
		target.removeProperty("contain"); refresh();
		corners(0x001f, "removing containment restores canvas propagation");
	}
	StyleSheet::instance().registerRule("contained", "contain", "paint");
	NodeHandle(body).classList().set("contained"); refresh();
	corners(0xffff, "cached containment rule prevents propagation");
	content.setProperty("contain", "none"); refresh();
	corners(0x001f, "inline none overrides cached containment");
	content.removeProperty("contain"); refresh();
	corners(0xffff, "removing inline containment restores cached rule");
}

void testHoverCascadeAndScale()
{
	auto &tree = Tree::instance();
	tree.clear();
	auto &sheet = StyleSheet::instance();
	sheet.registerRule("hover-square", "position", "absolute");
	sheet.registerRule("hover-square", "width", "40px");
	sheet.registerRule("hover-square", "height", "40px");
	sheet.registerRule("hover-square", "top", "20px");
	sheet.registerRule("hover-square", "background-color", "#000000");
	sheet.registerSelectorRule(".hover-square:hover", "transform-origin", "0 0");
	sheet.registerSelectorRule(".hover-square:hover", "transform", "scale(2)");
	sheet.registerSelectorRule(".hover-parent:hover .hover-indicator", "background-color", "#00ff00");
	const int root = makeDiv(-1, "hover-parent");
	NodeHandle(root).style().setProperty("background-color", "#ffffff");
	NodeHandle(root).style().setProperty("width", "240px");
	NodeHandle(root).style().setProperty("height", "200px");
	const int first = makeDiv(root, "hover-square");
	const int second = makeDiv(root, "hover-square");
	NodeHandle(first).style().setProperty("left", "20px");
	NodeHandle(second).style().setProperty("left", "140px");
	const int indicator = makeDiv(root, "hover-indicator");
	NodeHandle(indicator).style().setProperty("position", "absolute");
	NodeHandle(indicator).style().setProperty("top", "150px");
	NodeHandle(indicator).style().setProperty("width", "10px");
	NodeHandle(indicator).style().setProperty("height", "10px");
	tree.mount(root, 240, 200);
	expectEqual(displayPixelAt(90, 90), 0xffff, "scale is inactive before hover");
	expectEqual(tree.pointerHover(30, 30), first, "hover uses geometric hit testing");
	tree.refresh(root, 240, 200);
	expectTrue(tree.isHovered(root), "hover includes ancestors");
	expectEqual(displayPixelAt(90, 90), 0, "hover scales around explicit zero origin");
	expectEqual(displayPixelAt(210, 90), 0xffff, "hover cache distinguishes identical class signatures");
	expectEqual(displayPixelAt(5, 155), nativeRgb565(0x07e0), "hover ancestor selector recomputes descendants");
	expectEqual(tree.pointerHover(90, 90), first, "hover hit testing follows expanded transform");
	tree.pointerHover(150, 30);
	tree.refresh(root, 240, 200);
	expectEqual(displayPixelAt(90, 90), 0xffff, "old hover transform is removed");
	expectEqual(displayPixelAt(210, 90), 0, "hover moves to same-class sibling");
	tree.pointerHover(-1, -1);
	tree.refresh(root, 240, 200);
	expectEqual(displayPixelAt(210, 90), 0xffff, "pointer exit restores base transform");
	expectEqual(displayPixelAt(5, 155), 0xffff, "pointer exit removes ancestor hover");
	tree.pointerHover(30, 30);
	tree.removeNode(first);
	expectTrue(!tree.isHovered(root), "removed hover target does not leave stale state");
	tree.clear();
	expectTrue(!tree.isHovered(root), "tree reset clears hover");
}

void testUnbreakableTextBesideFloats()
{
	for (bool br : {false,true}) for (bool leadingSpace : {false,true}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(200,200,1.0);
		auto &tree=Tree::instance();
		const int root=makeDiv(-1,nullptr), floated=makeDiv(root,nullptr);
		auto rs=NodeHandle(root).style(); rs.width(30); rs.backgroundColor(0xffff); rs.setProperty("color","#000000"); rs.set(Property::FontId,9101); rs.set(Property::FontSize,10); rs.setProperty("line-height","10px");
		auto fs=NodeHandle(floated).style(); fs.width(15); fs.backgroundColor(0xf800); fs.height(30); fs.setProperty("float","left");
		if(br) { const int id=makeDiv(root,nullptr); NodeHandle(id).setTagName("br"); }
		auto text=Document::instance().createText(leadingSpace?" XXXXXXXXXX":"XXXXXXXXXX"); NodeHandle(root).appendChild(text);
		tree.mount(root,200,200);
		expectEqual(displayPixelAt(0,35),nativeRgb565(0),"inline text paints at its font baseline");
		expectEqual(displayPixelAt(0,34),nativeRgb565(0xffff),"ink centring does not shift an inline baseline");
		expectEqual(tree.node(text.id()).layout.y,30,"unbreakable word moves below float");
		expectEqual(tree.node(text.id()).layout.height,10,"normal wrapping does not split an unbreakable word");
		expectEqual(tree.node(root).layout.height,40,"float avoidance contributes to normal flow height");
		expectEqual(TextRenderer::firstUnbreakableWidth(tree.node(text.id())),50,"first unit excludes collapsed leading whitespace");
		fs.height(50); tree.refresh(root,200,200);
		expectEqual(tree.node(text.id()).layout.y,50,"float resize moves following line on retained refresh");
		fs.setProperty("float","none"); tree.refresh(root,200,200);
		expectEqual(tree.node(text.id()).layout.height,10,"unbreakable text remains one line without float");
	}
}

void testBlockMarginTrim()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(200,200,1.0);
	auto &tree = Tree::instance();
	const int root=makeDiv(-1,nullptr), container=makeDiv(root,"trim"), a=makeDiv(container,nullptr), b=makeDiv(container,nullptr), after=makeDiv(root,nullptr);
	auto parent=NodeHandle(container).style(), first=NodeHandle(a).style(), last=NodeHandle(b).style();
	NodeHandle(root).style().width(200); NodeHandle(root).style().height(200);
	parent.width(100); parent.setProperty("margin","7px"); parent.setProperty("padding","3px 0 5px");
	first.height(20); first.setProperty("margin","10px 0 15px");
	last.height(20); last.setProperty("margin","25px 0 30px");
	NodeHandle(after).style().height(10);
	StyleSheet::instance().registerRule("trim","margin-trim","block");
	tree.mount(root,200,200);
	auto check=[&](int height,int firstY,int lastY) {
		expectEqual(tree.node(container).layout.y,7,"margin trim preserves container margin");
		expectEqual(tree.node(container).layout.height,height,"margin trim contributes correct auto height");
		expectEqual(tree.node(a).layout.y,firstY,"margin trim first child edge");
		expectEqual(tree.node(b).layout.y,lastY,"margin trim keeps internal collapsed margins");
		expectEqual(tree.node(after).layout.y,height+14,"margin trim preserves following sibling gap");
	};
	check(73,10,55);
	parent.setProperty("margin-trim","none"); tree.refresh(root,200,200); check(113,20,65);
	parent.setProperty("margin-trim","block-start"); tree.refresh(root,200,200); check(103,10,55);
	parent.setProperty("margin-trim","block-end"); tree.refresh(root,200,200); check(83,20,65);
	parent.removeProperty("margin-trim"); tree.refresh(root,200,200); check(73,10,55);
	for(const char *invalid : {"block block", "block block-end", "none inline", "banana"}) {
		parent.setProperty("margin-trim",invalid); tree.refresh(root,200,200); check(73,10,55);
	}
	parent.setProperty("padding","0"); first.setProperty("height","auto");
	const int nested=makeDiv(a,nullptr); NodeHandle(nested).style().height(20); NodeHandle(nested).style().setProperty("margin","40px 0 45px");
	tree.refresh(root,200,200); check(85,7,72);
	expectEqual(tree.node(nested).layout.y,7,"trim removes collapsed descendant margin");
	expectEqual(tree.node(nested).style.margin[0],40,"trim leaves computed descendant margin intact");
	NodeHandle(container).classList().set(""); tree.refresh(root,200,200);
	expectEqual(tree.node(container).layout.y,40,"class removal restores collapsed leading margin");
	NodeHandle(container).classList().set("trim"); tree.refresh(root,200,200); check(85,7,72);

	// Empty siblings join the same collapsed group at each boundary.
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(200,200,1.0);
	const int er=makeDiv(-1,nullptr), ec=makeDiv(er,nullptr), emptyStart=makeDiv(ec,nullptr), middle=makeDiv(ec,nullptr), emptyEnd=makeDiv(ec,nullptr);
	NodeHandle(er).style().width(200); NodeHandle(er).style().height(200);
	auto es=NodeHandle(ec).style(); es.width(100); es.setProperty("padding","1px 0 2px"); es.setProperty("margin-trim","block");
	for(int empty : {emptyStart,emptyEnd}) NodeHandle(empty).style().setProperty("margin","80px 0 90px");
	NodeHandle(middle).style().height(20); NodeHandle(middle).style().setProperty("margin","10px 0 15px");
	tree.mount(er,200,200);
	expectEqual(tree.node(ec).layout.height,23,"trim removes whole collapsed groups through empty siblings");
	expectEqual(tree.node(middle).layout.y,1,"trimmed empty leading sibling contributes no gap");
	es.setProperty("margin-trim","inline"); tree.refresh(er,200,200);
	expectEqual(tree.node(ec).layout.height,203,"inline trim does not remove block-axis margins");
	es.setProperty("margin-trim","block inline"); tree.refresh(er,200,200);
	expectEqual(tree.node(ec).layout.height,23,"combined logical axes retain block trimming");
	for(int empty : {emptyStart,emptyEnd}) NodeHandle(empty).style().setProperty("margin","-80px 0 -90px");
	NodeHandle(middle).style().setProperty("margin","-10px 0 -15px"); tree.refresh(er,200,200);
	expectEqual(tree.node(ec).layout.height,23,"negative collapsed margins are trimmed to zero");
	expectEqual(tree.node(middle).layout.y,1,"negative margins cannot escape trimmed boundary");

	for (const char *mode : {"vertical-lr","vertical-rl","sideways-lr","sideways-rl"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(200,200,1.0);
		const int r=makeDiv(-1,nullptr), c=makeDiv(r,nullptr), one=makeDiv(c,nullptr), two=makeDiv(c,nullptr);
		NodeHandle(r).style().width(200); NodeHandle(r).style().height(200); NodeHandle(r).style().setProperty("writing-mode",mode);
		auto cs=NodeHandle(c).style(); cs.height(100); cs.setProperty("padding","0 4px"); cs.setProperty("margin-trim","block");
		NodeHandle(one).style().width(20); NodeHandle(one).style().height(40); NodeHandle(one).style().setProperty("margin","0 10px");
		NodeHandle(two).style().width(20); NodeHandle(two).style().height(40); NodeHandle(two).style().setProperty("margin","0 15px");
		tree.mount(r,200,200);
		expectEqual(tree.node(c).layout.width,63,"logical block trim sizes vertical container");
		const bool reverse=std::strcmp(mode,"vertical-rl")==0 || std::strcmp(mode,"sideways-rl")==0;
		expectEqual(tree.node(one).layout.x-tree.node(c).layout.x,reverse?39:4,"logical block-start trim respects writing mode");
		expectEqual(tree.node(two).layout.x-tree.node(c).layout.x,reverse?4:39,"logical block-end trim respects writing mode");
	}
}

void testCanvasBackgroundImages()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(80, 80, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), body = makeDiv(root, nullptr);
	NodeHandle(root).setTagName("html"); NodeHandle(body).setTagName("body");
	auto html = NodeHandle(root).style(), content = NodeHandle(body).style();
	html.setProperty("display", "block"); html.width(40); html.height(20); html.setProperty("margin", "10px");
	content.width(5); content.height(5); content.setProperty("margin", "3px");
	const char *stripes = "linear-gradient(#000000 0%, #000000 50%, #ffffff 50%)";
	html.setProperty("background-image", stripes);
	tree.mount(root, 80, 80);
	auto refresh = [&]() { tree.refresh(root,80,80); };
	auto bands = [&](int origin, int period, const char *label) {
		int wrong = 0;
		for (int y=0;y<80;++y) for (int x=0;x<80;++x) {
			const int local = ((y-origin)%period+period)%period;
			wrong += displayPixelAt(x,y) != nativeRgb565(local < period/2 ? 0 : 0xffff);
		}
		expectEqual(wrong,0,label);
	};
	bands(10,20,"root gradient repeats over canvas while retaining root image size and origin");
	html.setProperty("background-size", "40px 10px"); html.setProperty("background-position", "0px 5px"); refresh();
	bands(15,10,"background size and position use the root box without clipping at its margin");
	html.setProperty("background-attachment", "fixed"); refresh();
	bands(5,10,"fixed background placement uses viewport origin");
	html.setProperty("background-attachment", "scroll"); html.setProperty("background-size", "50% 50%"); refresh();
	bands(15,10,"percentage background size uses its own positioning area");
	html.setProperty("background", "none"); content.setProperty("background-image", stripes); refresh();
	bands(10,20,"propagated body images use root geometry and paint once");
	content.setProperty("opacity", "0"); refresh();
	bands(10,20,"body opacity does not hide a propagated canvas image");
	content.setProperty("background-image", "none"); refresh();
	int wrong=0; for(int y=0;y<80;++y) for(int x=0;x<80;++x) wrong += displayPixelAt(x,y) != nativeRgb565(0xffff);
	expectEqual(wrong,0,"removing a propagated image clears the entire canvas");
	const int absolute = makeDiv(body, nullptr);
	auto a = NodeHandle(absolute).style(); a.setProperty("position", "absolute"); a.setProperty("inset", "0"); refresh();
	expectEqual(tree.node(absolute).layout.x,0,"initial containing block ignores HTML margin");
	expectEqual(tree.node(absolute).layout.y,0,"initial containing block starts at viewport top");
	expectEqual(tree.node(absolute).layout.width,80,"initial containing block width is viewport width");
	expectEqual(tree.node(absolute).layout.height,80,"initial containing block height ignores short HTML box");
	html.setProperty("rotate", "0deg"); refresh();
	expectEqual(tree.node(absolute).layout.x,10,"identity rotation establishes absolute containing block");
	expectEqual(tree.node(absolute).layout.y,10,"transformed root keeps its box origin");
	expectEqual(tree.node(absolute).layout.width,40,"transformed root establishes its own containing width");
	expectEqual(tree.node(absolute).layout.height,20,"transformed root establishes its own containing height");
}

// Viewports can be wider than the embedded scanline buffer. Repeated replays
// exercise both the rasterizer and its stable bitmap caches.
void testWideBackgroundScanlines()
{
	resetNativeHost(); StyleSheet::instance().clear();
	setNativeDisplaySize(800, 80); setViewportMetrics(800, 80, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), box = makeDiv(root, nullptr);
	auto rootStyle = NodeHandle(root).style(), style = NodeHandle(box).style();
	rootStyle.width(800); rootStyle.height(80); rootStyle.backgroundColor(0xffff);
	style.setProperty("position", "absolute"); style.left(20); style.top(10);
	style.width(700); style.height(60);
	style.setProperty("background-image", "linear-gradient(#ff0000 0%, #ff0000 50%, #00ff00 50%)");
	tree.mount(root,800,80);
	auto check = [&](int top, int height, bool radial, bool translucent) {
		for (int frame=0; frame<4; ++frame) {
			DisplayList::instance().replayDirectDirtyRegion(0,0,799,79);
			int wrong=0;
			for(int y=0;y<80;++y) for(int x=0;x<800;++x) {
				const bool inside = x>=20 && x<720 && y>=top && y<top+height;
				const auto expected = inside ? (translucent ? displayPixelAt(25,y) : nativeRgb565(radial || y<top+height/2 ? 0xf800 : 0x07e0)) : nativeRgb565(0xffff);
				wrong += displayPixelAt(x,y) != expected;
			}
			expectEqual(wrong,0,"wide gradient covers every scanline on direct and cached replay");
			if(translucent) expectTrue(displayPixelAt(25,top+5) != nativeRgb565(0xffff),"translucent gradient paints visible color");
		}
	};
	check(10,60,false,false);
	style.top(20); style.height(40); tree.refresh(root,800,80);
	check(20,40,false,false);
	for (const char *image : {"linear-gradient(rgba(255,0,0,0.5),rgba(255,0,0,0.5))", "radial-gradient(#ff0000,#ff0000)", "radial-gradient(rgba(255,0,0,0.5),rgba(255,0,0,0.5))"}) {
		style.setProperty("background-image",image); tree.refresh(root,800,80);
		check(20,40,true,std::strstr(image,"rgba") != nullptr);
	}
}

void testIndependentBackgroundLayers()
{
	for (bool cached : {false, true}) {
		resetNativeHost();
		auto &sheet = StyleSheet::instance();
		sheet.clear();
		setViewportMetrics(80, 80, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto rootStyle = NodeHandle(root).style();
		rootStyle.width(80); rootStyle.height(80); rootStyle.setProperty("background", "#ffffff");
		const int box = makeDiv(root, nullptr);
		auto style = NodeHandle(box).style();
		style.width(40); style.height(40);
		if (cached) {
			sheet.registerRule("layers", "background-color", "#00ff00");
			sheet.registerRule("layers", "background-image", "none");
			NodeHandle(box).classList().set("layers");
		} else {
			style.setProperty("background-color", "#00ff00");
			style.setProperty("background-image", "none");
		}
		tree.mount(root, 80, 80);
		auto refresh = [&]() { tree.refresh(root, 80, 80); };
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0x07e0), "background-image none preserves base color");
		style.setProperty("background-image", "linear-gradient(#0000ff, #0000ff)");
		style.setProperty("background-color", "#ff0000");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0x001f), "background-color does not clear image");
		NodeHandle(root).classList().set("recompute-child-background");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0x001f), "inline image survives ancestor class recompute");
		style.setProperty("background-image", "none, none");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0xf800), "clearing image reveals latest color");
		style.setProperty("background-color", "#00ff00");
		style.setProperty("background-image", "linear-gradient(transparent, transparent)");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0x07e0), "transparent gradient reveals bottom color layer");
		style.setProperty("background-image", "linear-gradient(rgba(0,0,255,0.5), rgba(0,0,255,0.5))");
		refresh();
		const int blended = displayPixelAt(10, 10);
		expectTrue(blended != nativeRgb565(0x07e0) && blended != nativeRgb565(0x001f) && blended != nativeRgb565(0xffff), "translucent image composites over color");
		style.setProperty("background", "#ff0000");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0xf800), "background shorthand clears image");
		style.setProperty("background", "none");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0xffff), "background none clears color and image");
		style.setProperty("background", "linear-gradient(transparent, transparent) #00ff00");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0x07e0), "shorthand combines independent color and image");
		style.setProperty("background", "linear-gradient(transparent, transparent)");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(0xffff), "image-only shorthand resets previous color to transparent");
		style.setProperty("background-color", "rgba(255,0,0,0.5)");
		refresh();
		const int translucent = displayPixelAt(10, 10);
		NodeHandle(root).classList().set("recompute-alpha");
		refresh();
		expectEqual(displayPixelAt(10, 10), translucent, "color alpha persists through inline replay");
		style.removeProperty("background-image");
		refresh();
		expectEqual(displayPixelAt(10, 10), translucent, "removing image retains independent inline color");
		style.removeProperty("background-color");
		refresh();
		expectEqual(displayPixelAt(10, 10), nativeRgb565(cached ? 0x07e0 : 0xffff), "removing color restores cascade");
	}
	resetNativeHost();
	auto &sheet = StyleSheet::instance();
	sheet.clear();
	setViewportMetrics(80, 80, 1.0);
	sheet.registerRule("cached-layers", "background-image", "linear-gradient(transparent, transparent)");
	sheet.registerRule("cached-layers", "background-color", "#00ff00");
	const int root = makeDiv(-1, "cached-layers");
	NodeHandle(root).style().width(80); NodeHandle(root).style().height(80);
	Tree::instance().mount(root, 80, 80);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0x07e0), "cached image followed by color paints independent layers");
	expectEqual(Tree::instance().node(root).style.bg_fill, 1, "cached color preserves gradient");
	sheet.registerRule("replacement", "background-image", "linear-gradient(#0000ff, #0000ff)");
	NodeHandle(root).classList().set("cached-layers replacement");
	Tree::instance().refresh(root, 80, 80);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0x001f), "cached image class changes repaint");
	expectEqual(Tree::instance().node(root).style.bg_color, nativeRgb565(0x07e0), "cached image preserves base color");
	NodeHandle(root).style().setProperty("background-image", "none");
	Tree::instance().refresh(root, 80, 80);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0x07e0), "inline none overrides cached image");
	NodeHandle(root).style().removeProperty("background-image");
	Tree::instance().refresh(root, 80, 80);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0x001f), "removing inline none restores cached image");

	sheet.registerStaticColorRule(StaticStyleSelectorKind::Class, "static-layers", StaticStyleColorProperty::BackgroundColor, 0, 255, 0, 255);
	sheet.registerStaticBackgroundRule(StaticStyleSelectorKind::Class, "static-layers",
	    {1800, {0, 0, 0, 0}, {}, {0, 0, 0, 0}, 500, 1000, false}, {}, false, {}, {}, nullptr, true);
	NodeHandle(root).classList().set("static-layers");
	Tree::instance().refresh(root, 80, 80);
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0x07e0), "static image-only API preserves static base color");
	expectEqual(Tree::instance().node(root).style.bg_fill, 1, "static image-only API applies gradient");

}

void testCompiledGradientColorVarFallbacks()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);

	const int box = makeDiv(-1, "gradient-theme gradient-var");
	const int noFallback = makeDiv(box, "gradient-theme gradient-no-fallback");
	const int missingNoFallback = makeDiv(box, "gradient-missing-no-fallback");
	Tree::instance().mount(box, 200, 100);

	StyleSheet::instance().registerStaticCustomColorRule(StaticStyleSelectorKind::Class,
	                                                     "gradient-theme",
	                                                     "--to",
	                                                     255,
	                                                     0,
	                                                     0,
	                                                     255);
	StyleSheet::instance().registerRule("gradient-var",
	                                    "background",
	                                    "linear-gradient(90deg, var(--missing, #00ff00), var(--to, #0000ff))");
	StyleSheet::instance().registerRule("gradient-no-fallback",
	                                    "background",
	                                    "linear-gradient(90deg, #000000, var(--to))");
	StyleSheet::instance().registerRule("gradient-missing-no-fallback",
	                                    "background",
	                                    "linear-gradient(90deg, #000000, var(--missing-gradient-stop))");
	NodeHandle(box).classList().set("gradient-theme gradient-var");
	NodeHandle(noFallback).classList().set("gradient-theme gradient-no-fallback");
	NodeHandle(missingNoFallback).classList().set("gradient-missing-no-fallback");
	Tree::instance().computeLayout(box, 200, 100);

	const auto &style = Tree::instance().node(box).style;
	expectEqual(style.has_bg, 1, "gradient-var: compiled background applies");
	expectEqual(style.bg_fill, 1, "gradient-var: compiled background fill");
	expectEqual(static_cast<int>(rstyle(style).bg_gradient_from_color), nativeRgb565(0x07e0), "gradient-var: missing from stop uses fallback");
	expectEqual(static_cast<int>(rstyle(style).bg_gradient_to_color), nativeRgb565(0xf800), "gradient-var: custom to stop resolves red");
	expectEqual(static_cast<int>(rstyle(Tree::instance().node(noFallback).style).bg_gradient_to_color),
	            nativeRgb565(0xf800),
	            "gradient-var: no-fallback custom stop resolves red");
	expectEqual(Tree::instance().node(missingNoFallback).style.has_bg,
	            0,
	            "gradient-var: missing no-fallback stop leaves background unapplied");

	NodeHandle(box).classList().set("gradient-var");
	Tree::instance().computeLayout(box, 200, 100);
	expectEqual(static_cast<int>(rstyle(Tree::instance().node(box).style).bg_gradient_to_color),
	            nativeRgb565(0x001f),
	            "gradient-var: missing to stop uses fallback");
}

void testPseudoSelectorMaterializeAndRemove()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(120, 80, 1.0);

	const int chipId = makeDiv(-1, "chip active");
	Tree::instance().mount(chipId, 120, 80);
	StyleSheet::instance().registerRule("chip", "width", "60");
	StyleSheet::instance().registerRule("chip", "height", "20");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "content", "\"\"");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "position", "absolute");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "left", "0");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "right", "0");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "bottom", "0");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "height", "2");
	StyleSheet::instance().registerSelectorRule(".chip.active::after", "background", "#ffffff");
	NodeHandle(chipId).classList().set("chip active");

	const int afterId = findDirectChildByTag(chipId, "::after");
	expectTrue(afterId >= 0, "pseudo: active chip materializes ::after");
	if (afterId >= 0) {
		const auto &after = Tree::instance().node(afterId);
		expectEqual(after.style.height, 2, "pseudo: ::after height");
		expectEqual(after.style.has_bg, 1, "pseudo: ::after background");
	}

	NodeHandle(chipId).classList().set("chip");
	expectEqual(findDirectChildByTag(chipId, "::after") >= 0 ? 1 : 0, 0, "pseudo: inactive chip removes ::after");
}

void testFlexLineAlignment()
{
	for (bool column : {false, true}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setViewportMetrics(200, 200, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.setProperty("display", "flex");
		style.setProperty("flex-direction", column ? "column" : "row");
		style.setProperty("flex-wrap", "wrap");
		style.width(column ? 100 : 80);
		style.height(column ? 80 : 100);
		int items[4];
		for (int &id : items) {
			id = makeDiv(root, nullptr);
			NodeHandle(id).style().width(column ? 20 : 40);
			NodeHandle(id).style().height(column ? 40 : 20);
		}
		tree.mount(root, 200, 200);
		struct Alignment { const char *value; int first; int second; };
		for (const Alignment &a : {Alignment{"flex-start", 0, 20}, {"center", 30, 50},
		                          {"flex-end", 60, 80}, {"space-between", 0, 80},
		                          {"space-around", 15, 65}, {"stretch", 0, 50}}) {
			style.setProperty("align-content", a.value);
			tree.refresh(root, 200, 200);
			const auto &first = tree.node(items[0]).layout;
			const auto &second = tree.node(items[2]).layout;
			expectEqual(column ? first.x : first.y, a.first, a.value);
			expectEqual(column ? second.x : second.y, a.second, a.value);
		}
		// Wrap still allows align-content with a single occupied line.
		if (column) style.height(160); else style.width(160);
		style.setProperty("align-content", "center");
		tree.refresh(root, 200, 200);
		expectEqual(column ? tree.node(items[0]).layout.x : tree.node(items[0]).layout.y,
		            40, "align-content: single occupied wrapping line");
		style.setProperty("flex-wrap", "nowrap");
		tree.refresh(root, 200, 200);
		expectEqual(column ? tree.node(items[0]).layout.x : tree.node(items[0]).layout.y,
		            0, "align-content: does not move nowrap line");
	}
}

void testOrderLayoutAndInvalidation()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 100, 1.0);
	auto &tree = Tree::instance();
	auto &sheet = StyleSheet::instance();
	sheet.registerRule("item", "width", "20px");
	sheet.registerRule("item", "height", "20px");
	sheet.registerRule("late", "order", "3");
	sheet.registerRule("early", "order", "-2");
	sheet.registerRule("early", "order", "1px"); // invalid must not mask -2
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("display", "flex");
	NodeHandle(root).style().width(200);
	NodeHandle(root).style().height(100);
	const int a = makeDiv(root, "item late");
	const int b = makeDiv(root, "item early");
	const int c = makeDiv(root, "item early");
	tree.mount(root, 200, 100);
	expectEqual(tree.node(b).layout.x, 0, "order: negative items first");
	expectEqual(tree.node(c).layout.x, 20, "order: equal values keep document order");
	expectEqual(tree.node(a).layout.x, 40, "order: positive item last");
	expectEqual(tree.node(root).first_child, a, "order: tree order unchanged");

	NodeHandle(a).style().setProperty("order", "-4");
	tree.refresh(root, 200, 100);
	expectEqual(tree.node(a).layout.x, 0, "order: inline mutation relayouts siblings");
	NodeHandle(a).style().setProperty("order", "2.5");
	expectEqual(tree.node(a).style.order, -4, "order: fractional token ignored");
	NodeHandle(a).style().removeProperty("order");
	tree.refresh(root, 200, 100);
	expectEqual(tree.node(a).layout.x, 40, "order: removal restores class value");
	NodeHandle(a).classList().set("item early");
	tree.refresh(root, 200, 100);
	expectEqual(tree.node(a).layout.x, 0, "order: class mutation invalidates cached geometry");
	NodeHandle(a).style().setProperty("order", "var(--rank, -7)");
	expectEqual(tree.node(a).style.order, -7, "order: variable fallback uses runtime parser");

	NodeHandle(root).style().setProperty("display", "block");
	NodeHandle(a).style().setProperty("order", "9");
	tree.refresh(root, 200, 100);
	expectEqual(tree.node(a).layout.y, 0, "order: no reordering in block layout");
	expectEqual(tree.node(b).layout.y, 20, "order: block siblings follow document order");

	NodeHandle(root).style().setProperty("display", "grid");
	NodeHandle(root).style().setProperty("grid-template-columns", "20px 20px 20px");
	tree.refresh(root, 200, 100);
	expectEqual(tree.node(b).layout.x, 0, "order: grid auto-placement uses order");
	expectEqual(tree.node(c).layout.x, 20, "order: grid ties retain tree order");
	expectEqual(tree.node(a).layout.x, 40, "order: grid positive item last");
}

void testOrderPaintingAndHitTesting()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setNativeDisplaySize(120, 80);
	setViewportMetrics(120, 80, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("display", "flex");
	NodeHandle(root).style().width(120);
	NodeHandle(root).style().height(80);
	const int a = makeDiv(root, nullptr);
	const int b = makeDiv(root, nullptr);
	for (int id : {a, b}) {
		NodeHandle(id).style().width(40);
		NodeHandle(id).style().height(40);
		NodeHandle(id).style().setProperty("margin-right", "-40px");
	}
	NodeHandle(a).style().setProperty("background", "#ff0000");
	NodeHandle(b).style().setProperty("background", "#0000ff");
	NodeHandle(a).style().setProperty("order", "1");
	tree.mount(root, 120, 80);
	expectEqual(tree.hitTestNode(10, 10), a, "order: hit testing follows paint order");
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0xf800), "order: higher order paints last");
	NodeHandle(a).style().setProperty("order", "-1");
	tree.refresh(root, 120, 80);
	expectEqual(tree.hitTestNode(10, 10), b, "order: mutation changes hit order");
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0x001f), "order: mutation repaints overlap");
	NodeHandle(a).style().setProperty("z-index", "1");
	tree.refresh(root, 120, 80);
	expectEqual(tree.hitTestNode(10, 10), a, "order: z-index takes precedence");
	expectEqual(displayPixelAt(10, 10), nativeRgb565(0xf800), "order: z-index takes precedence in paint");
}

void testBoxSizingAndDeferredCalc()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(400, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto parent = NodeHandle(root).style();
	parent.setProperty("display", "block");
	parent.width(200);
	parent.height(200);
	parent.setProperty("padding", "10px");
	parent.setProperty("border", "2px solid #000000");
	const int child = makeDiv(root, nullptr);
	auto style = NodeHandle(child).style();
	style.setProperty("width", "calc(50% - 10px)");
	style.height(20);
	tree.mount(root, 400, 300);
	expectEqual(tree.node(root).layout.width, 224, "content-box: padding and borders outside width");
	expectEqual(tree.node(child).layout.x, 12, "content-box: child starts inside border and padding");
	expectEqual(tree.node(child).layout.width, 90, "calc: uses parent content width at layout");
	parent.width(300);
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 140, "calc: parent resize invalidates percentage basis");
	parent.setProperty("box-sizing", "border-box");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(root).layout.width, 300, "border-box: edges included in width");
	expectEqual(tree.node(child).layout.width, 128, "calc: border-box excludes parent edges");
	style.width(40);
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 40, "calc: numeric width replaces expression");
}

void testInheritedFlexDirectionAndSidewaysModes()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 200, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(200);
	NodeHandle(root).style().height(200);
	NodeHandle(root).style().setProperty("direction", "rtl");
	const int flex = makeDiv(root, nullptr);
	auto style = NodeHandle(flex).style();
	style.setProperty("display", "flex");
	style.setProperty("align-items", "flex-start");
	style.width(100);
	style.height(100);
	const int item = makeDiv(flex, nullptr);
	NodeHandle(item).style().width(20);
	NodeHandle(item).style().height(20);
	tree.mount(root, 200, 200);
	expectEqual(tree.node(flex).layout.x, 100, "direction: RTL block parent right-aligns the narrower flex container");
	expectEqual(tree.node(item).layout.x - tree.node(flex).layout.x, 80, "direction: flex inherits rtl from parent");
	style.setProperty("direction", "ltr");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(flex).layout.x, 100, "direction: child's LTR override does not change parent block alignment");
	expectEqual(tree.node(item).layout.x - tree.node(flex).layout.x, 0, "direction: explicit child overrides inherited rtl");
	style.removeProperty("direction");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(item).layout.x - tree.node(flex).layout.x, 80, "direction: removal restores inherited rtl");
	NodeHandle(root).style().setProperty("direction", "ltr");
	style.setProperty("writing-mode", "sideways-lr");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(item).layout.y, 80, "sideways-lr: ltr inline axis runs bottom to top");
	NodeHandle(root).style().setProperty("direction", "rtl");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(item).layout.y, 0, "sideways-lr: rtl reverses inline axis");
	style.setProperty("writing-mode", "sideways-rl");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(item).layout.x - tree.node(flex).layout.x, 80, "sideways-rl: block axis runs right to left");
	expectEqual(tree.node(item).layout.y, 80, "sideways-rl: rtl inline axis runs bottom to top");
}

void testGridAbsoluteStaticAlignment()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	NodeHandle(root).style().height(300);
	const int grid = makeDiv(root, nullptr);
	auto style = NodeHandle(grid).style();
	style.setProperty("display", "grid");
	style.setProperty("align-items", "center");
	style.setProperty("padding", "10px");
	style.setProperty("border", "2px solid #000000");
	style.width(100);
	style.height(100);
	const int item = makeDiv(grid, nullptr);
	auto child = NodeHandle(item).style();
	child.setProperty("position", "absolute");
	child.width(20);
	child.height(20);
	tree.mount(root, 300, 300);
	expectEqual(tree.node(item).layout.x, 12, "grid abspos: inline origin uses content edge");
	expectEqual(tree.node(item).layout.y, 52, "grid abspos: align-items centers in content box");
	child.setProperty("align-self", "flex-end");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.x, 12, "grid abspos: align-self does not change inline alignment");
	expectEqual(tree.node(item).layout.y, 92, "grid abspos: align-self overrides align-items");
	style.setProperty("justify-items", "center");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.x, 52, "grid abspos: justify-items controls inline alignment");
}

void testUnitlessLineHeightInheritance()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance(); auto &sheet = StyleSheet::instance();
	const int root = makeDiv(-1, nullptr), child = makeDiv(root, nullptr), grandchild = makeDiv(child, nullptr);
	auto p = NodeHandle(root).style(), c = NodeHandle(child).style(), g = NodeHandle(grandchild).style();
	p.width(300); p.setProperty("font-size", "20px"); p.setProperty("line-height", "1.5");
	c.setProperty("font-size", "40px"); g.setProperty("font-size", "10px");
	g.setProperty("white-space", "pre");
	const auto text = Document::instance().createText("X\nX"); NodeHandle(grandchild).appendChild(text);
	tree.mount(root, 300, 300);
	auto heights = [&](int parentHeight, int childHeight, int grandHeight, const char *message) {
		expectEqual(tree.node(root).style.line_height, parentHeight, message);
		expectEqual(tree.node(child).style.line_height, childHeight, message);
		expectEqual(tree.node(grandchild).style.line_height, grandHeight, message);
		expectEqual(tree.node(text.id()).layout.height, 2 * grandHeight, "dynamic inherited line height reaches multiline text layout");
	};
	heights(30, 60, 15, "unitless line height is inherited as a multiplier");
	for (const char *length : {"30px", "150%", "1.5em"}) {
		p.setProperty("line-height", length); tree.refresh(root, 300, 300);
		heights(30, 30, 30, "line-height lengths and percentages inherit computed pixels");
	}
	p.setProperty("line-height", "150%"); p.setProperty("font-size", "30px"); tree.refresh(root, 300, 300);
	heights(45, 45, 45, "authored percentage recomputes with own font before length inheritance");
	p.setProperty("font-size", "20px");
	sheet.registerRule("leading-number", "line-height", "1.5");
	sheet.registerRule("leading-length", "line-height", "30px");
	p.removeProperty("line-height");
	for (int i = 0; i < 6; ++i) {
		tree.setClassName(root, i % 2 ? "leading-length" : "leading-number"); tree.refresh(root, 300, 300);
		heights(30, i % 2 ? 30 : 60, i % 2 ? 30 : 15, "cached rules distinguish equal used heights with different inheritance");
	}
	p.setProperty("line-height", "1.5"); c.setProperty("line-height", "2"); tree.refresh(root, 300, 300);
	heights(30, 80, 20, "descendant multiplier overrides inherited multiplier");
	c.removeProperty("line-height"); tree.refresh(root, 300, 300);
	heights(30, 60, 15, "removing multiplier restores inherited number");
	c.setProperty("font-size", "50px"); p.setProperty("font-size", "10px"); tree.refresh(root, 300, 300);
	heights(15, 75, 15, "font mutations recompute each element's inherited multiplier");
	sheet.applyNumberProperty(NodeHandle(root), "line-height", 2.0); tree.refresh(root, 300, 300);
	heights(20, 100, 20, "numeric CSS line-height preserves multiplier semantics");
	p.set(Property::LineHeight, 12); tree.refresh(root, 300, 300);
	heights(12, 12, 12, "native pixel line-height API keeps pixel semantics");
	p.setProperty("--leading", "1.25"); p.setProperty("line-height", "var(--leading)"); tree.refresh(root, 300, 300);
	heights(13, 63, 13, "custom-property number retains multiplier semantics");
	p.setProperty("font", "20px/1.5 serif"); tree.refresh(root, 300, 300);
	heights(30, 75, 15, "font shorthand retains unitless line height");
	sheet.registerStaticLineHeightRule(StaticStyleSelectorKind::Class, "static-leading", StaticStyleLineHeightKind::Scalar, {StaticStyleLengthUnit::Raw, 1.75f});
	p.removeProperty("font"); p.setProperty("font-size", "20px"); tree.setClassName(root, "static-leading"); tree.refresh(root, 300, 300);
	heights(35, 88, 18, "generated static CSS scalar rules retain multiplier inheritance");
}

void testFontShorthandAndRelativeSize()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("font-size", "20px");
	const int child = makeDiv(root, "font-probe");
	StyleSheet::instance().registerRule("font-probe", "font", "bold 150% / 1.5 serif");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(child).style.font_size, 30, "font shorthand: percentage refers to parent font");
	expectEqual(tree.node(child).style.line_height, 45, "font shorthand: line height refers to resulting font size");
	expectEqual(tree.node(child).style.font_weight, 700, "font shorthand: weight parsed");
	NodeHandle(root).style().setProperty("font-size", "24px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(child).style.font_size, 36, "font shorthand: inherited basis mutation recomputes size");
	expectEqual(tree.node(child).style.line_height, 54, "font shorthand: inherited basis mutation recomputes line height");
	auto style = NodeHandle(child).style();
	style.setProperty("font", "18px/2em \"Test Family\", serif");
	expectEqual(tree.node(child).style.font_size, 18, "font shorthand: inline size overrides class shorthand");
	expectEqual(tree.node(child).style.line_height, 36, "font shorthand: em line height uses own font");
	expectEqual(tree.node(child).style.font_weight, 400, "font shorthand: omitted weight resets to normal");
	style.setProperty("font", "22px serif");
	expectEqual(tree.node(child).style.line_height, 0, "font shorthand: omitted line height resets normal");
	style.setProperty("font", "garbage");
	expectEqual(tree.node(child).style.font_size, 22, "font shorthand: invalid input does not partially overwrite");
	style.removeProperty("font");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(child).style.font_size, 36, "font shorthand: removal restores stylesheet");
	StyleSheet::instance().registerRule("font-probe", "font-size", "200%");
	StyleSheet::instance().recomputeSubtree(root);
	expectEqual(tree.node(child).style.font_size, 48, "compiled font-size: percentage uses parent font, not box height");
	style.setProperty("font", "1.5rem/2rem serif");
	expectEqual(tree.node(child).style.font_size, 36, "font shorthand: rem size uses root font");
	expectEqual(tree.node(child).style.line_height, 48, "font shorthand: rem line-height uses root font");
}

void testFontRelativeDimensions()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	const int small = makeDiv(root, "ch-box");
	const int big = makeDiv(root, "ch-box");
	// The bitmap test font has an 8px zero advance at 16px font size.
	StyleSheet::instance().registerRule("ch-box", "width", "calc(5ch + 2px)");
	StyleSheet::instance().registerRule("ch-box", "height", "2ch");
	StyleSheet::instance().registerRule("ch-box", "padding-left", "1ch");
	StyleSheet::instance().registerRule("ch-box", "font-size", "16px");
	NodeHandle(big).style().setProperty("font-size", "32px");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(small).layout.width, 50, "ch width and padding use zero advance");
	expectEqual(tree.node(big).layout.width, 98, "ch expression is not cached across font sizes");
	expectEqual(tree.node(big).style.padding[3], 16, "ch padding uses final inline font");
	expectEqual(tree.node(big).layout.height, 32, "ch height uses inline glyph advance too");
	NodeHandle(big).style().setProperty("font-size", "24px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(big).layout.width, 74, "ch width follows inline font mutation");
	expectEqual(tree.node(big).style.padding[3], 12, "ch padding follows inline font mutation");
	expectEqual(tree.node(big).layout.height, 24, "ch height follows inline font mutation");
	auto style = NodeHandle(small).style();
	style.setProperty("width", "calc(50% - 2ch)");
	style.setProperty("font-size", "20px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(small).layout.width, 140, "ch calc resolves percentage and current font");
	style.setProperty("width", "3CH");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(small).layout.width, 40, "ch unit is case insensitive");
	style.setProperty("width", "40px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(small).layout.width, 50, "px override clears ch dependency");
	style.removeProperty("width");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(small).layout.width, 62, "removal restores class ch expression");
	const int sized = makeDiv(big, nullptr);
	NodeHandle(sized).style().setProperty("font-size", "2ch");
	expectEqual(tree.node(sized).style.font_size, 24, "font-size ch uses parent metrics");
	StyleSheet::instance().registerRule("ch-box", "--ch-size", "calc(2ch + 1px)");
	StyleSheet::instance().registerRule("ch-box", "width", "var(--ch-size)");
	StyleSheet::instance().recomputeSubtree(root);
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(small).layout.width, 31, "custom ch expression uses consumer font");
	expectEqual(tree.node(big).layout.width, 37, "custom ch expression is not pre-resolved across consumers");
	const int ordered = makeDiv(root, "font-order");
	StyleSheet::instance().registerRule("font-order", "font-size", "12px");
	StyleSheet::instance().registerRule("font-order", "padding-left", "1ch");
	StyleSheet::instance().registerRule("font-order", "font-size", "24px");
	StyleSheet::instance().recomputeSubtree(root);
	expectEqual(tree.node(ordered).style.padding[3], 12, "ch padding uses winning font, not preceding declaration");
}

void testLineHeightRelativeUnits()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(500, 500, 1.0);
	auto &tree = Tree::instance(); auto &sheet = StyleSheet::instance();
	const int root = makeDiv(-1, nullptr), parent = makeDiv(root, nullptr), child = makeDiv(parent, "lh-box");
	auto rootStyle = NodeHandle(root).style(), parentStyle = NodeHandle(parent).style(), style = NodeHandle(child).style();
	rootStyle.setProperty("line-height", "30px"); parentStyle.setProperty("line-height", "24px");
	// Deliberately put the metric declaration after its consumers.
	sheet.registerRule("lh-box", "width", "calc(2lh + 1rlh)");
	sheet.registerRule("lh-box", "height", "1lh");
	sheet.registerRule("lh-box", "padding-left", "0.5lh");
	sheet.registerRule("lh-box", "line-height", "20px");
	tree.mount(root, 500, 500);
	expectEqual(tree.node(child).layout.width, 80, "lh/rlh calc and padding use final element/root line heights");
	expectEqual(tree.node(child).layout.height, 20, "lh height uses final class line-height");
	style.setProperty("line-height", "40px"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).layout.width, 130, "lh dimensions follow inline line-height mutation");
	rootStyle.setProperty("line-height", "50px"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).layout.width, 150, "rlh follows root line-height mutation despite local override");
	style.setProperty("line-height", "calc(1lh + 2px)"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).style.line_height, 26, "line-height lh uses parent metrics rather than previous self value");
	parentStyle.setProperty("line-height", "32px"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).style.line_height, 34, "inline relative line-height follows parent mutation");
	style.setProperty("font-size", "1lh");
	expectEqual(tree.node(child).style.font_size, 32, "font-size lh uses parent line-height");
	style.setProperty("line-height", "1rlh");
	expectEqual(tree.node(child).style.line_height, 50, "non-root line-height rlh uses root metrics");
	style.setProperty("width", "2LH"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).layout.width, 125, "lh units are case insensitive");
	style.removeProperty("line-height"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).layout.width, 50, "removing relative line-height restores class metric");
	style.set(Property::FontId, 9102); style.setProperty("font-size", "20px"); style.setProperty("line-height", "normal");
	tree.refresh(root, 500, 500);
	expectEqual(tree.node(child).layout.width, 50, "normal lh uses first available font line metrics");
	sheet.registerRule("ordered-lh", "line-height", "10px");
	sheet.registerRule("ordered-lh", "min-width", "2lh");
	sheet.registerRule("ordered-lh", "line-height", "30px");
	const int ordered = makeDiv(parent, "ordered-lh"); tree.refresh(root, 500, 500);
	expectEqual(tree.node(ordered).style.min_width, 60, "eager constraints use winning line-height regardless of declaration order");
	NodeHandle(ordered).style().setProperty("line-height", "40px");
	expectEqual(tree.node(ordered).style.min_width, 80, "inline line-height update keeps parent fallback scoped to line-height resolution");
	rootStyle.setProperty("font-size", "1lh");
	expectEqual(tree.node(root).style.font_size, 16, "root font-size lh uses initial font/line-height metrics");
	rootStyle.setProperty("line-height", "2rlh");
	expectEqual(tree.node(root).style.line_height, 32, "root line-height rlh uses initial metrics without accumulating");
	sheet.recomputeSubtree(root);
	expectEqual(tree.node(root).style.line_height, 32, "root relative line-height stays stable on cascade replay");
	style.setProperty("font", "20px/1lh serif");
	expectEqual(tree.node(child).style.line_height, 32, "font shorthand lh line-height uses parent metrics");
	style.setProperty("font", "20px/15pt serif");
	expectEqual(tree.node(child).style.line_height, 20, "font shorthand retains absolute point line-height");
}

void testUserAgentDefaultsAndFontUnits()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	auto &sheet = StyleSheet::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	NodeHandle(root).style().setProperty("font-size", "24px");
	const int paragraph = makeDiv(root, "paragraph");
	tree.setTagName(paragraph, "p");
	sheet.registerUserAgentElementRule("p", "margin-top", "1em");
	sheet.registerUserAgentElementRule("p", "margin-bottom", "1em");
	sheet.registerRule("paragraph", "width", "5em");
	sheet.registerRule("paragraph", "height", "2rem");
	sheet.registerRule("paragraph", "font-size", "20px");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(paragraph).style.margin[0], 20, "UA em margin uses winning font");
	expectEqual(tree.node(paragraph).layout.width, 100, "em width uses own font");
	expectEqual(tree.node(paragraph).layout.height, 48, "rem height uses root font");
	NodeHandle(root).style().setProperty("font-size", "30px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(paragraph).layout.height, 60, "rem dimension follows root font mutation");
	sheet.registerSelectorRule("*", "margin", "0");
	// Registering another default later must not beat an earlier author rule.
	sheet.registerUserAgentElementRule("p", "margin-top", "2em");
	sheet.recomputeSubtree(root);
	expectEqual(tree.node(paragraph).style.margin[0], 0, "author universal outranks UA element rule regardless of order");
	expectEqual(tree.node(paragraph).style.margin[2], 0, "author shorthand overrides UA bottom margin");
	auto style = NodeHandle(paragraph).style();
	style.setProperty("font-size", "calc(50% + 1em)");
	expectEqual(tree.node(paragraph).style.font_size, 45, "font-size calc em and percent use parent metrics");
	NodeHandle(root).style().setProperty("font-size", "2rem");
	expectEqual(tree.node(root).style.font_size, 32, "root rem font-size refers to initial font size");
}

void testBlockMarginsAndIntrinsicWidth()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	const int group = makeDiv(root, nullptr);
	NodeHandle(group).style().setProperty("padding", "1px");
	const int first = makeDiv(group, nullptr);
	const int empty = makeDiv(group, nullptr);
	const int last = makeDiv(group, nullptr);
	NodeHandle(first).style().height(10);
	NodeHandle(first).style().setProperty("margin-bottom", "20px");
	NodeHandle(empty).style().setProperty("margin-top", "30px");
	NodeHandle(empty).style().setProperty("margin-bottom", "-15px");
	NodeHandle(last).style().height(10);
	NodeHandle(last).style().setProperty("margin-top", "-10px");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(last).layout.y, 26, "margin group preserves largest positive and most negative across empty box");
	expectEqual(tree.node(empty).layout.y, 41, "empty box position excludes its own negative bottom margin");
	expectEqual(tree.node(group).layout.height, 37, "collapsed margin group contributes once to auto height");
	expectEqual(tree.node(first).layout.width, 298, "normal block auto width fills containing content box");
	NodeHandle(group).style().setProperty("padding", "0");
	NodeHandle(first).style().setProperty("margin-top", "25px");
	NodeHandle(last).style().setProperty("margin-bottom", "40px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(group).layout.y, 25, "first child top margin escapes unpadded parent");
	expectEqual(tree.node(first).layout.y, 25, "escaped top margin is not counted twice");
	expectEqual(tree.node(group).layout.height, 35, "escaped bottom margin does not enlarge parent");
	expectEqual(tree.node(root).layout.height, 100, "root contains collapsed descendant edge margins");
	NodeHandle(group).style().setProperty("overflow", "hidden");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(group).layout.y, 0, "new block context prevents child top margin escaping");
	expectEqual(tree.node(group).layout.height, 100, "new block context contains both child edge margins");

	const int flex = makeDiv(root, nullptr);
	NodeHandle(flex).style().setProperty("display", "flex");
	NodeHandle(flex).style().setProperty("align-items", "flex-start");
	const int item = makeDiv(flex, nullptr);
	const int inner = makeDiv(item, nullptr);
	const int fixed = makeDiv(inner, nullptr);
	NodeHandle(fixed).style().width(40);
	NodeHandle(fixed).style().height(10);
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.width, 40, "auto flex item measures nested block content before stretching");
	expectEqual(tree.node(inner).layout.width, 40, "nested normal block fills final intrinsic flex item width");
	NodeHandle(inner).style().setProperty("overflow", "auto");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.width, 40, "scrollable descendant still contributes intrinsic auto width");
	expectEqual(tree.node(inner).layout.height, 10, "auto height scrollable block encloses content before parent sizing");
	NodeHandle(inner).style().setProperty("overflow", "visible");
	NodeHandle(flex).style().setProperty("flex-direction", "column");
	NodeHandle(flex).style().setProperty("align-items", "stretch");
	NodeHandle(flex).style().setProperty("max-width", "250px");
	NodeHandle(flex).style().height(500);
	const int wideItem = makeDiv(flex, nullptr);
	const int wideContent = makeDiv(wideItem, nullptr);
	NodeHandle(wideContent).style().width(500);
	NodeHandle(wideContent).style().height(100);
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.width, 250, "single flex line clamps intrinsic cross size to container maximum");
	expectEqual(tree.node(wideItem).layout.width, 250, "all stretch items use clamped single-line cross size");
	expectEqual(tree.node(wideContent).layout.width, 500, "fixed block content may overflow a clamped flex item");
	for (const char *display : {"flex", "grid"}) {
		const int vertical = makeDiv(root, nullptr);
		NodeHandle(vertical).style().setProperty("display", display);
		NodeHandle(vertical).style().setProperty("writing-mode", "vertical-lr");
		const int horizontal = makeDiv(vertical, nullptr);
		NodeHandle(horizontal).style().setProperty("writing-mode", "horizontal-tb");
		const int content = makeDiv(horizontal, nullptr);
		NodeHandle(content).style().width(10);
		NodeHandle(content).style().height(79);
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(vertical).layout.width, 10, "orthogonal auto block size uses content width, not parent inline width");
		expectEqual(tree.node(horizontal).layout.width, 10, "orthogonal child retains its intrinsic horizontal width");
	}
}

void testDeferredPercentageBoxEdges()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(100);
	NodeHandle(root).style().height(200);
	NodeHandle(root).style().setProperty("position", "relative");
	const int absolute = makeDiv(root, nullptr);
	NodeHandle(absolute).style().setProperty("position", "absolute");
	NodeHandle(absolute).style().setProperty("left", "0");
	NodeHandle(absolute).style().setProperty("top", "0");
	const int child = makeDiv(absolute, nullptr);
	NodeHandle(child).style().width(100);
	NodeHandle(child).style().height(100);
	auto childStyle = NodeHandle(child).style();
	childStyle.setProperty("margin-left", "-50%");
	tree.mount(root, 100, 200);
	expectEqual(tree.node(absolute).layout.width, 100, "cyclic percentage margin contributes zero to intrinsic width");
	expectEqual(tree.node(child).layout.x, -50, "percentage margin resolves after shrink-to-fit width is determined");
	childStyle.setProperty("margin-left", "0");
	childStyle.setProperty("padding-left", "50%");
	tree.refresh(root, 100, 200);
	expectEqual(tree.node(absolute).layout.width, 100, "cyclic percentage padding contributes zero to intrinsic width");
	expectEqual(tree.node(child).layout.width, 150, "child padding resolves against final width and may overflow");
	childStyle.setProperty("padding-left", "calc(10px + 50%)");
	tree.refresh(root, 100, 200);
	expectEqual(tree.node(absolute).layout.width, 110, "calc fixed term remains in intrinsic padding contribution");
	expectEqual(tree.node(child).layout.width, 165, "calc percentage resolves against final content width");
	childStyle.setProperty("padding-left", "0");
	StyleSheet::instance().recomputeSubtree(root);
	tree.refresh(root, 100, 200);
	expectEqual(tree.node(child).layout.width, 100, "numeric override clears authored expression through cascade replay");
	childStyle.setProperty("padding-left", "var(--edge, 0px)");
	tree.refresh(root, 100, 200);
	NodeHandle(root).style().setProperty("--edge", "10%");
	tree.refresh(root, 100, 200);
	expectTrue(rstyle(tree.node(child).style).padding_expression[3] >= 0, "inline variable edge retains expression handle");
	expectEqual(tree.node(absolute).layout.width, 100, "late custom percentage still has zero intrinsic contribution");
	expectEqual(tree.node(child).layout.width, 110, "constant fallback retains variable dependency for later percentage");
	tree.computeLayout(root, 100, 200);
	expectEqual(tree.node(child).layout.width, 110, "explicit layout also resolves late custom percentage");
	StyleSheet::instance().registerRule("inline-variable-owner", "--edge", "20%");
	NodeHandle(root).classList().set("inline-variable-owner");
	tree.refresh(root, 100, 200);
	expectEqual(tree.node(child).layout.width, 110, "inline custom property outranks class value after recomputation");
	NodeHandle(root).style().removeProperty("--edge");
	tree.refresh(root, 100, 200);
	expectEqual(tree.node(child).layout.width, 120, "removing inline custom property restores class value");
	childStyle.setProperty("padding-left", "0");

	const int normal = makeDiv(root, "percent-box-edges");
	NodeHandle(normal).style().height(10);
	StyleSheet::instance().registerRule("percent-box-edges", "margin", "10%");
	StyleSheet::instance().registerRule("percent-box-edges", "padding", "10%");
	StyleSheet::instance().recomputeSubtree(root);
	tree.refresh(root, 100, 200);
	expectEqual(tree.node(normal).style.margin[0], 10, "vertical percentage margin uses containing inline size");
	expectEqual(tree.node(normal).style.padding[0], 10, "vertical percentage padding uses containing inline size");
	expectEqual(tree.node(normal).layout.width, 80, "normal block fills available width after percentage margins");
	NodeHandle(root).style().width(200);
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(normal).style.margin[0], 20, "class percentage margin follows containing block resize");
	expectEqual(tree.node(normal).style.padding[0], 20, "class percentage padding follows containing block resize");
	NodeHandle(normal).style().setProperty("margin", "5% auto");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(normal).style.margin[0], 10, "mixed percentage/auto shorthand retains percentage");
	expectEqual(tree.node(normal).style.margin_auto, 10, "mixed percentage/auto shorthand retains auto side bits");
	NodeHandle(normal).style().removeProperty("margin");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(normal).style.margin[0], 20, "removing inline shorthand restores class expression");
}

void testIntrinsicHeightKeywords()
{
	for (const char *keyword : {"min-content", "max-content", "fit-content"}) {
		for (const char *display : {"block", "flex"}) {
			resetNativeHost();
			StyleSheet::instance().clear();
			auto &tree = Tree::instance();
			const int root = makeDiv(-1, nullptr);
			NodeHandle(root).style().width(100);
			NodeHandle(root).style().height(50);
			NodeHandle(root).style().setProperty("position", "relative");
			StyleSheet::instance().registerRule("intrinsic-height", "height", keyword);
			const int absolute = makeDiv(root, "intrinsic-height");
			auto style = NodeHandle(absolute).style();
			style.setProperty("position", "absolute");
			style.setProperty("display", display);
			style.setProperty("top", "0");
			style.setProperty("bottom", "0");
			style.width(100);
			const int child = makeDiv(absolute, nullptr);
			NodeHandle(child).style().setProperty("height", "100%");
			const int content = makeDiv(child, nullptr);
			NodeHandle(content).style().height(100);
			tree.mount(root, 100, 200);
			expectEqual(tree.node(absolute).layout.height, 100, "intrinsic abspos height sizes to content despite both insets");
			expectEqual(tree.node(child).layout.height, 100, "percentage child of intrinsic height behaves as auto");
			NodeHandle(child).style().setProperty("height", "calc(50% + 10px)");
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(child).layout.height, 100, "mixed percentage calculation is auto in intrinsic height");
			NodeHandle(child).style().setProperty("height", "100%");
			style.height(40);
			StyleSheet::instance().recomputeSubtree(root);
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(absolute).layout.height, 40, "numeric inline height overrides class intrinsic keyword");
			expectEqual(tree.node(child).layout.height, 40, "percentage child resolves against definite height");
			style.setProperty("height", keyword);
			StyleSheet::instance().recomputeSubtree(root);
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(absolute).layout.height, 100, "inline intrinsic height survives cascade replay");
			style.height(60);
			StyleSheet::instance().recomputeSubtree(root);
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(absolute).layout.height, 60, "numeric mutation clears inline intrinsic expression");
			style.removeProperty("height");
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(absolute).layout.height, 100, "removing inline height restores class intrinsic keyword");
			NodeHandle(child).style().setProperty("height", "auto");
			NodeHandle(content).style().setProperty("height", "50%");
			const int leaf = makeDiv(content, nullptr);
			NodeHandle(leaf).style().height(80);
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(absolute).layout.height, 80, "auto wrapper does not make intrinsic percentage basis definite");
			expectEqual(tree.node(content).layout.height, 80, "nested percentage under intrinsic size follows its content");
			NodeHandle(child).style().height(40);
			tree.refresh(root, 100, 200);
			expectEqual(tree.node(content).layout.height, 20, "fixed ancestor supplies percentage basis inside intrinsic box");
		}
	}
	for (const char *display : {"flex", "grid"}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().width(200);
		NodeHandle(root).style().height(200);
		NodeHandle(root).style().setProperty("display", display);
		const int child = makeDiv(root, nullptr);
		NodeHandle(child).style().width(20);
		NodeHandle(child).style().setProperty("height", "max-content");
		const int content = makeDiv(child, nullptr);
		NodeHandle(content).style().height(40);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(child).layout.height, 40, "stretch alignment preserves authored intrinsic height");
	}
}

void testSafeOverflowAlignment()
{
	for (const char *display : {"flex", "grid"}) {
		for (const char *direction : {"row", "column", "row-reverse", "column-reverse"}) {
			resetNativeHost();
			StyleSheet::instance().clear();
			auto &tree = Tree::instance();
			const int root = makeDiv(-1, "safe-align");
			auto rootStyle = NodeHandle(root).style();
			rootStyle.width(100); rootStyle.height(80);
			rootStyle.setProperty("display", display);
			rootStyle.setProperty("flex-direction", direction);
			rootStyle.setProperty("grid-template-columns", "100px");
			rootStyle.setProperty("grid-template-rows", "80px");
			for (const char *property : {"justify-content", "align-items", "justify-items"})
				StyleSheet::instance().registerRule("safe-align", property, "safe center");
			const int child = makeDiv(root, nullptr);
			auto childStyle = NodeHandle(child).style();
			childStyle.width(140); childStyle.height(120);
			childStyle.setProperty("flex-shrink", "0");
			tree.mount(root, 200, 200);
			expectEqual(tree.node(child).layout.x, 0, "safe overflow alignment uses start on horizontal axis");
			expectEqual(tree.node(child).layout.y, 0, "safe overflow alignment uses start on vertical axis");
			for (const char *property : {"justify-content", "align-items", "justify-items"})
				rootStyle.setProperty(property, "unsafe center");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.x, -20, "unsafe overflow alignment centers horizontally");
			expectEqual(tree.node(child).layout.y, -20, "unsafe overflow alignment centers vertically");
			for (const char *property : {"justify-content", "align-items", "justify-items"})
				rootStyle.setProperty(property, "safe center");
			childStyle.width(20); childStyle.height(40);
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.x, 40, "safe alignment still centers when horizontal space is sufficient");
			expectEqual(tree.node(child).layout.y, 20, "safe alignment still centers when vertical space is sufficient");
			rootStyle.setProperty("align-items", "safe stretch");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(root).style.align_items, kAlignSafe | 1, "invalid safety modifier does not overwrite alignment");
		}
	}
}

void testInlineGridTrackPersistence()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	StyleSheet::instance().registerRule("grid-track-defaults", "grid-template-columns", "20px");
	StyleSheet::instance().registerRule("grid-track-defaults", "grid-template-rows", "20px");
	const int root = makeDiv(-1, "grid-track-defaults");
	auto style = NodeHandle(root).style();
	style.setProperty("display", "grid");
	style.width(100); style.height(100);
	style.setProperty("--track", "40px");
	style.setProperty("grid-template-columns", "var(--track)");
	style.setProperty("grid-template-rows", "50px");
	style.setProperty("justify-items", "center");
	style.setProperty("align-items", "center");
	const int child = makeDiv(root, nullptr);
	NodeHandle(child).style().width(20);
	NodeHandle(child).style().height(20);
	tree.mount(root, 100, 100);
	expectEqual(tree.node(child).layout.x, 10, "inline grid column overrides class track after alignment changes");
	expectEqual(tree.node(child).layout.y, 15, "inline grid row survives alignment changes");
	style.setProperty("--track", "60px");
	tree.refresh(root, 100, 100);
	expectEqual(tree.node(child).layout.x, 20, "inline grid template resolves updated custom property");
	style.removeProperty("grid-template-columns");
	style.removeProperty("grid-template-rows");
	tree.refresh(root, 100, 100);
	expectEqual(tree.node(child).layout.x, 0, "removing inline grid columns restores class definition");
	expectEqual(tree.node(child).layout.y, 0, "removing inline grid rows restores class definition");
}

void testIntrinsicWidths()
{
	for (const char *keyword : {"min-content", "max-content", "fit-content"}) {
		for (int available : {10, 30, 100}) for (bool edges : {false, true}) {
			resetNativeHost(); StyleSheet::instance().clear();
			auto &tree = Tree::instance();
			StyleSheet::instance().registerRule("intrinsic-width", "width", keyword);
			const int root = makeDiv(-1, nullptr); NodeHandle(root).style().width(available);
			const int box = makeDiv(root, "intrinsic-width");
			auto style = NodeHandle(box).style();
			if (edges) { style.setProperty("padding", "5px"); style.setProperty("border", "2px solid #000000"); }
			auto text = Document::instance().createText("XXXX XXXX"); NodeHandle(box).appendChild(text);
			text.style().set(Property::FontId, 9101); text.style().set(Property::FontSize, 10);
			tree.mount(root, 200, 200);
			const int minimum = 20 + (edges ? 14 : 0), maximum = 45 + (edges ? 14 : 0);
			const int expected = std::strcmp(keyword, "min-content") == 0 ? minimum : std::strcmp(keyword, "max-content") == 0 ? maximum : std::min(maximum, std::max(minimum, available));
			expectEqual(tree.node(box).layout.width, expected, "intrinsic width uses word minimum, unwrapped maximum, or fit clamp including edges");
			style.setProperty("box-sizing", "border-box"); tree.refresh(root, 200, 200);
			expectEqual(tree.node(box).layout.width, expected, "intrinsic content measurements are independent of box-sizing");
			style.setProperty("width", "max-content"); tree.refresh(root, 200, 200);
			expectEqual(tree.node(box).layout.width, maximum, "inline intrinsic width overrides class on refresh");
			style.removeProperty("width"); tree.refresh(root, 200, 200);
			expectEqual(tree.node(box).layout.width, expected, "removing inline width restores intrinsic class width");
		}
	}
	for (bool space : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear(); auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr); auto style = NodeHandle(root).style();
		style.setProperty("width", "min-content");
		style.set(Property::FontId, 9101); style.set(Property::FontSize, 10);
		const int a = makeSpan(root, ""), b = makeSpan(root, "");
		NodeHandle(a).appendChild(Document::instance().createText("XX"));
		NodeHandle(b).appendChild(Document::instance().createText(space ? " XX" : "XX"));
		tree.mount(root, 100, 100);
		expectEqual(tree.node(root).layout.width, space ? 10 : 20, "min-content preserves words across adjacent transparent spans");
	}
	for (const char *bound : {"none", "minimum", "maximum"}) {
		resetNativeHost(); StyleSheet::instance().clear(); auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr); NodeHandle(root).style().width(200);
		const int box = makeDiv(root, nullptr); auto style = NodeHandle(box).style();
		style.setProperty("width", "fit-content"); style.setProperty("padding", "10%");
		style.setProperty("box-sizing", "border-box");
		if (std::strcmp(bound, "minimum") == 0) style.setProperty("min-width", "80px");
		if (std::strcmp(bound, "maximum") == 0) style.setProperty("max-width", "50px");
		auto text = Document::instance().createText("XXXX"); NodeHandle(box).appendChild(text);
		text.style().set(Property::FontId, 9101); text.style().set(Property::FontSize, 10);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(box).layout.width, std::strcmp(bound, "minimum") == 0 ? 80 : std::strcmp(bound, "maximum") == 0 ? 50 : 60,
		            "intrinsic width resolves percentage edges before final min/max constraints");
	}
	for (bool percent : {false, true}) for (int depth : {1, 3}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr); NodeHandle(root).style().width(200);
		int parent = root, outer = -1;
		for (int i = 0; i < depth; ++i) {
			parent = makeDiv(parent, nullptr); if (outer < 0) outer = parent;
			NodeHandle(parent).style().setProperty("width", "fit-content");
		}
		const int ratio = makeDiv(parent, nullptr);
		NodeHandle(ratio).style().setProperty("aspect-ratio", "1/2"); NodeHandle(ratio).style().height(100);
		if (percent) NodeHandle(ratio).style().setProperty("width", "100%");
		const int child = makeDiv(ratio, nullptr); NodeHandle(child).style().width(100);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(outer).layout.width, 100, "nested fit-content measures ratio content minimum before percentage resolution");
		expectEqual(tree.node(ratio).layout.width, 100, "ratio auto minimum retains intrinsic descendant width");
		NodeHandle(child).style().width(120); tree.refresh(root, 200, 200);
		expectEqual(tree.node(outer).layout.width, 120, "intrinsic width refresh invalidates measured descendants");
	}
	for (const char *display : {"block", "flex", "grid"}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr); auto rs = NodeHandle(root).style(); rs.width(200); rs.height(100);
		rs.setProperty("display", display); rs.setProperty("flex-direction", "column");
		rs.setProperty("grid-template-columns", "200px");
		const int box = makeDiv(root, nullptr); auto style = NodeHandle(box).style(); style.setProperty("width", "max-content");
		const int child = makeDiv(box, nullptr); NodeHandle(child).style().width(40); NodeHandle(child).style().height(10);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(box).layout.width, 40, "parent stretch does not override intrinsic width");
		style.setProperty("position", "absolute"); style.setProperty("left", "10px"); style.setProperty("right", "10px");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(box).layout.width, 40, "opposing absolute insets do not stretch intrinsic width");
	}
}

void testOverflowClip()
{
	for (int mode = 0; mode < 4; ++mode) {
		resetNativeHost(); StyleSheet::instance().clear();
		setNativeDisplaySize(160, 160); setViewportMetrics(160, 160, 1.0);
		auto &tree = Tree::instance();
		StyleSheet::instance().registerRule("clip-test", "overflow", "clip");
		const int root = makeDiv(-1, nullptr);
		auto rootStyle = NodeHandle(root).style();
		rootStyle.width(160); rootStyle.height(160); rootStyle.setProperty("background", "#ffffff");
		const int outer = makeDiv(root, mode == 0 ? "clip-test" : nullptr);
		auto style = NodeHandle(outer).style();
		style.setProperty("position", "absolute"); style.setProperty("left", "40px"); style.setProperty("top", "40px");
		style.setProperty("box-sizing", "border-box"); style.width(60); style.height(60);
		style.setProperty("padding", "5px"); style.setProperty("border", "4px solid #00ff00");
		if (mode == 1) style.setProperty("overflow", "clip visible");
		if (mode == 2) { style.setProperty("--clip", "clip"); style.setProperty("overflow-y", "var(--clip)"); }
		if (mode == 3) style.setProperty("overflow", "visible");
		const int inner = makeDiv(outer, nullptr);
		auto child = NodeHandle(inner).style();
		child.setProperty("position", "relative"); child.setProperty("left", "-20px"); child.setProperty("top", "-20px");
		child.width(100); child.height(100); child.setProperty("background", "#0000ff");
		tree.mount(root, 160, 160);
		for (int frame = 0; frame < 3; ++frame) {
			const int dx = frame == 1 ? 20 : 0;
			if (frame) {
				style.setProperty("left", frame == 1 ? "60px" : "40px");
				child.setProperty("background", frame == 1 ? "#ff0000" : "#0000ff");
				tree.refresh(root, 160, 160);
			}
			const int paint = nativeRgb565(frame == 1 ? 0xf800 : 0x001f);
			const bool clipX = mode == 0 || mode == 1, clipY = mode == 0 || mode == 2;
			expectEqual(displayPixelAt(35 + dx, 60), clipX ? nativeRgb565(0xffff) : paint, "overflow clip respects only its horizontal axis on refresh");
			expectEqual(displayPixelAt(60 + dx, 35), clipY ? nativeRgb565(0xffff) : paint, "overflow clip respects only its vertical axis on refresh");
			expectEqual(displayPixelAt(41 + dx, 60), clipX ? nativeRgb565(0x07e0) : paint, "overflow clips at padding edge and leaves own border visible");
			expectEqual(displayPixelAt(60 + dx, 60), paint, "overflow retains interior pixels");
			expectEqual(tree.hitTestNode(35 + dx, 60) == inner, !clipX, "hit testing follows horizontal overflow clip");
			expectEqual(tree.hitTestNode(41 + dx, 60), clipX ? outer : inner, "own border remains hit-testable outside content clip");
			expectEqual(tree.hitTestNode(60 + dx, 35) == inner, !clipY, "hit testing follows vertical overflow clip");
		}
		tree.setScrollTop(outer, 20); tree.setScrollLeft(outer, 20);
		tree.refresh(root, 160, 160);
		expectEqual(tree.node(outer).layout.scroll_x, 0, "clip does not permit horizontal scrolling");
		expectEqual(tree.node(outer).layout.scroll_y, 0, "clip does not permit vertical scrolling");
		if (mode == 1 || mode == 2) {
			style.setProperty(mode == 1 ? "top" : "left", "180px");
			child.setProperty(mode == 1 ? "top" : "left", "-180px");
			tree.refresh(root, 160, 160);
			expectEqual(displayPixelAt(mode == 1 ? 60 : 15, mode == 1 ? 15 : 60), nativeRgb565(0x001f),
			            "offscreen parent does not cull descendants on its visible overflow axis");
		}
		style.setProperty("overflow", "clip"); style.setProperty("overflow-y", "auto");
		tree.refresh(root, 160, 160);
		expectEqual(overflowX(tree.node(outer).style), 1, "clip computes to hidden beside auto");
		style.setProperty("overflow-y", "visible"); tree.refresh(root, 160, 160);
		expectEqual(overflowX(tree.node(outer).style), 3, "clip recovers after other axis changes to visible");
		style.setProperty("overflow", "invalid"); tree.refresh(root, 160, 160);
		expectEqual(overflowX(tree.node(outer).style), 3, "invalid overflow is ignored");
	}
	for (bool clip : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().setProperty("padding", "1px");
		const int outer = makeDiv(root, nullptr);
		NodeHandle(outer).style().setProperty("overflow", clip ? "clip" : "hidden");
		const int child = makeDiv(outer, nullptr);
		NodeHandle(child).style().height(10); NodeHandle(child).style().setProperty("margin-top", "20px");
		tree.mount(root, 160, 160);
		expectEqual(tree.node(child).layout.y - tree.node(outer).layout.y, clip ? 0 : 20, "clip preserves parent-child margin collapsing");
		NodeHandle(root).style().setProperty("display", "flex"); NodeHandle(root).style().width(100);
		NodeHandle(root).style().setProperty("padding", "0");
		NodeHandle(child).style().width(200);
		tree.refresh(root, 160, 160);
		expectEqual(tree.node(outer).layout.width, clip ? 200 : 100, "clip retains the flex item's content-based automatic minimum");
	}
	for (int mode = 0; mode < 4; ++mode) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().width(100); NodeHandle(root).style().height(100);
		NodeHandle(root).style().setProperty("overflow", "auto");
		const int wrapper = makeDiv(root, nullptr);
		auto style = NodeHandle(wrapper).style(); style.width(20); style.height(20);
		style.setProperty("overflow", mode == 0 ? "clip" : mode == 1 ? "clip visible" : mode == 2 ? "visible clip" : "visible");
		const int child = makeDiv(wrapper, nullptr);
		NodeHandle(child).style().width(300); NodeHandle(child).style().height(300);
		tree.mount(root, 160, 160);
		expectEqual(tree.node(root).layout.scroll_content_width, mode == 0 || mode == 1 ? 100 : 300,
		            "only visible horizontal descendant overflow propagates to ancestor scroll range");
		expectEqual(tree.node(root).layout.scroll_content_height, mode == 0 || mode == 2 ? 100 : 300,
		            "only visible vertical descendant overflow propagates to ancestor scroll range");
	}
}

void testFixedPositioning()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setNativeDisplaySize(300, 250);
	setViewportMetrics(300, 250, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto rootStyle = NodeHandle(root).style();
	rootStyle.setProperty("display", "block");
	rootStyle.width(200); rootStyle.height(180);
	rootStyle.setProperty("margin", "20px");
	rootStyle.setProperty("background", "#ffffff");
	const int scroller = makeDiv(root, nullptr);
	auto scrollStyle = NodeHandle(scroller).style();
	scrollStyle.setProperty("display", "block");
	scrollStyle.setProperty("position", "relative");
	scrollStyle.setProperty("overflow", "auto");
	scrollStyle.setProperty("margin", "10px");
	scrollStyle.setProperty("padding", "5px");
	scrollStyle.width(80); scrollStyle.height(60);
	const int fixed = makeDiv(scroller, nullptr);
	auto style = NodeHandle(fixed).style();
	style.setProperty("position", "fixed");
	style.setProperty("left", "120px"); style.setProperty("top", "20px");
	style.width(25); style.height(25);
	style.setProperty("background", "#ff0000");
	const int following = makeDiv(scroller, nullptr);
	NodeHandle(following).style().width(20); NodeHandle(following).style().height(200);
	tree.mount(root, 300, 250);
	expectEqual(tree.node(fixed).layout.x, 120, "fixed position ignores positioned ancestor and root margins");
	expectEqual(tree.node(fixed).layout.y, 20, "fixed top uses viewport coordinates");
	expectEqual(tree.node(following).layout.y, tree.node(scroller).layout.y + 5, "fixed box does not consume normal-flow space");
	expectEqual(displayPixelAt(125, 25), nativeRgb565(0xf800), "fixed box escapes ancestor overflow clip");
	expectEqual(tree.hitTestNode(125, 25), fixed, "fixed box outside ancestor clip remains hit-testable");
	tree.setScrollTop(scroller, 40);
	tree.refresh(root, 300, 250);
	expectEqual(tree.node(fixed).layout.y, 20, "fixed box remains stationary during ancestor scroll");
	expectEqual(displayPixelAt(125, 25), nativeRgb565(0xf800), "scroll refresh keeps fixed pixels stationary");
	style.setProperty("width", "50%"); style.setProperty("height", "20%");
	style.setProperty("left", "auto"); style.setProperty("right", "0");
	tree.refresh(root, 300, 250);
	expectEqual(tree.node(fixed).layout.width, 150, "fixed percentage width uses viewport instead of root style width");
	expectEqual(tree.node(fixed).layout.height, 50, "fixed percentage height uses viewport");
	expectEqual(tree.node(fixed).layout.x, 150, "fixed right offset uses viewport width");
	setNativeDisplaySize(400, 300);
	setViewportMetrics(400, 300, 1.0);
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 200, "fixed percentage width updates with viewport resize");
	expectEqual(tree.node(fixed).layout.x, 200, "fixed right position updates with viewport resize");
	for (const auto &effect : {std::pair<const char *, const char *>{"transform", "translateX(0px)"},
	                          {"rotate", "0deg"}, {"scale", "1"}, {"filter", "blur(0px)"}}) {
		scrollStyle.setProperty(effect.first, effect.second);
		tree.setScrollTop(scroller, 0);
		tree.refresh(root, 400, 300);
		expectEqual(tree.node(fixed).layout.width, 45, "identity effect establishes fixed percentage containing block");
		expectEqual(tree.node(fixed).layout.x, tree.node(scroller).layout.x + 45, "fixed right inset uses identity effect containing block");
		scrollStyle.setProperty(effect.first, "none");
		tree.refresh(root, 400, 300);
		expectEqual(tree.node(fixed).layout.width, 200, "effect none returns fixed containing block to viewport");
		scrollStyle.removeProperty(effect.first);
	}
	scrollStyle.setProperty("transform", "translateX(20px)");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 45, "adding visible transform relayouts fixed percentage child");
	scrollStyle.setProperty("transform", "none");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 200, "removing visible transform restores viewport percentage basis");
	scrollStyle.removeProperty("transform");
	StyleSheet::instance().registerRule("fixed-containing-block", "transform", "translateX(0px)");
	tree.setClassName(scroller, "fixed-containing-block");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 45, "cached class identity transform establishes containing block");
	scrollStyle.setProperty("transform", "none");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 200, "inline transform none overrides class identity transform");
	scrollStyle.removeProperty("transform");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 45, "removing inline transform restores class containing block");
	tree.setClassName(scroller, "");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(fixed).layout.width, 200, "removing class identity transform restores viewport containing block");
	scrollStyle.setProperty("margin-top", "400px");
	tree.refresh(root, 400, 300);
	expectEqual(displayPixelAt(210, 25), nativeRgb565(0xf800), "fixed descendant paints when overflow ancestor is outside viewport");
	expectEqual(tree.hitTestNode(210, 25), fixed, "fixed descendant hit testing survives culled ancestor");
	auto followingStyle = NodeHandle(following).style();
	followingStyle.setProperty("position", "relative");
	followingStyle.setProperty("top", std::to_string(20 - tree.node(scroller).layout.y - 5) + "px");
	followingStyle.setProperty("background", "#0000ff");
	tree.refresh(root, 400, 300);
	expectEqual(displayPixelAt(40, 25), nativeRgb565(0xffff), "culled overflow ancestor still clips ordinary descendants");
	expectEqual(tree.hitTestNode(40, 25), root, "ordinary descendant stays outside hit testing clip");
	style.setProperty("background", "#0000ff");
	tree.refresh(root, 400, 300);
	expectEqual(displayPixelAt(210, 25), nativeRgb565(0x001f), "fixed paint-only update escapes culled ancestor clip");
	scrollStyle.setProperty("display", "none");
	tree.refresh(root, 400, 300);
	expectEqual(tree.hitTestNode(210, 25), root, "display none ancestor hides fixed subtree");
	resetNativeHost();
	StyleSheet::instance().clear();
	const int gridRoot = makeDiv(-1, nullptr);
	auto grid = NodeHandle(gridRoot).style();
	grid.setProperty("display", "grid"); grid.setProperty("position", "relative");
	grid.setProperty("grid", "40px 40px / 50px 50px");
	grid.setProperty("margin", "20px 30px");
	grid.setProperty("padding", "5px"); grid.setProperty("border", "2px solid #000000");
	grid.width(100); grid.height(80);
	const int item = makeDiv(gridRoot, nullptr);
	NodeHandle(item).style().setProperty("position", "fixed");
	NodeHandle(item).style().setProperty("grid-area", "2 / 2");
	NodeHandle(item).style().width(10); NodeHandle(item).style().height(10);
	tree.mount(gridRoot, 300, 250);
	expectEqual(tree.node(item).layout.x, 37, "fixed static position uses grid content edge when grid is not containing block");
	expectEqual(tree.node(item).layout.y, 27, "fixed static position ignores grid-area when attached to viewport");
	grid.setProperty("position", "static");
	grid.setProperty("transform", "translateX(0px)");
	tree.refresh(gridRoot, 300, 250);
	expectEqual(tree.node(item).layout.x, 87, "transformed static grid supplies fixed child's grid area");
	expectEqual(tree.node(item).layout.y, 67, "fixed static grid area uses transformed containing block");
}

void testStackingContextDescendants()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(120, 100, 1.0);
	setNativeDisplaySize(120, 100);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), red = makeDiv(root, nullptr), wrapper = makeDiv(root, nullptr);
	const int green = makeDiv(wrapper, nullptr);
	auto rs = NodeHandle(root).style(), r = NodeHandle(red).style(), w = NodeHandle(wrapper).style(), g = NodeHandle(green).style();
	rs.width(120); rs.height(100); rs.setProperty("position", "relative"); rs.setProperty("background", "#ffffff");
	r.setProperty("position", "absolute"); r.left(20); r.top(20); r.width(60); r.height(60); r.setProperty("background", "#ff0000");
	w.width(100); w.height(100); w.setProperty("background", "#0000ff");
	g.setProperty("position", "absolute"); g.left(30); g.top(30); g.width(20); g.height(20); g.setProperty("background", "#00ff00");
	tree.mount(root, 120, 100);
	expectEqual(displayPixelAt(25, 25), nativeRgb565(0xf800), "non-stacking wrapper background stays below positioned sibling");
	expectEqual(displayPixelAt(35, 35), nativeRgb565(0x07e0), "positioned descendant joins ancestor stacking context");
	expectEqual(tree.hitTestNode(35, 35), green, "hoisted positioned descendant receives the visible hit");
	w.setProperty("position", "relative"); w.setProperty("z-index", "0"); tree.refresh(root, 120, 100);
	expectEqual(displayPixelAt(25, 25), nativeRgb565(0x001f), "explicit zero isolates wrapper paint as a stacking context");
	w.setProperty("z-index", "-1"); tree.refresh(root, 120, 100);
	expectEqual(displayPixelAt(35, 35), nativeRgb565(0xf800), "negative context keeps its positioned descendants below outer sibling");
	w.setProperty("position", "static"); w.setProperty("z-index", "auto");
	w.setProperty("overflow", "hidden"); w.height(40); tree.refresh(root, 120, 100);
	expectEqual(displayPixelAt(35, 35), nativeRgb565(0x07e0), "hoisted descendant paints inside ancestor overflow clip");
	expectEqual(displayPixelAt(35, 45), nativeRgb565(0xf800), "hoisted descendant retains ancestor overflow clip");
	expectEqual(tree.hitTestNode(35, 45), red, "hit testing retains the hoisted descendant's ancestor clip");
	w.setProperty("pointer-events", "none"); tree.refresh(root, 120, 100);
	expectEqual(tree.hitTestNode(35, 35), red, "hoisted descendants retain ancestor pointer exclusion");
}

void testPositionedPaintOrder()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(100, 100, 1.0);
	setNativeDisplaySize(100, 100);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto parent = NodeHandle(root).style();
	parent.width(100); parent.height(100); parent.setProperty("position", "relative");
	parent.setProperty("display", "grid"); parent.setProperty("grid", "100px / 100px");
	parent.setProperty("background", "#ffffff");
	const int first = makeDiv(root, nullptr), later = makeDiv(root, nullptr);
	auto a = NodeHandle(first).style(), b = NodeHandle(later).style();
	for (int id : {first, later}) {
		NodeHandle(id).style().width(60); NodeHandle(id).style().height(60);
		NodeHandle(id).style().setProperty("grid-area", "1 / 1 / 2 / 2");
	}
	a.setProperty("position", "absolute"); a.setProperty("background", "#ff0000");
	b.setProperty("background", "#00ff00");
	tree.mount(root, 100, 100);
	auto top = [&](int id, const char *message) {
		expectEqual(displayPixelAt(20, 20), nativeRgb565(id == first ? 0xf800 : 0x07e0), message);
		expectEqual(tree.hitTestNode(20, 20), id, message);
	};
	top(first, "positioned auto paints and hits above later normal-flow grid item");
	a.setProperty("z-index", "-1"); tree.refresh(root, 100, 100);
	top(later, "negative positioned stack level paints below normal-flow item");
	a.setProperty("z-index", "auto"); tree.refresh(root, 100, 100);
	top(first, "auto restores positioned painting phase");
	b.setProperty("z-index", "0"); tree.refresh(root, 100, 100);
	top(later, "explicit zero on a grid item joins positioned phase");
	b.setProperty("z-index", "auto"); tree.refresh(root, 100, 100);
	top(first, "auto grid item returns to in-flow painting phase");
	StyleSheet::instance().registerRule("stack-zero", "z-index", "0");
	StyleSheet::instance().registerRule("stack-auto", "z-index", "auto");
	b.removeProperty("z-index");
	for (int frame = 0; frame < 4; ++frame) {
		tree.setClassName(later, frame % 2 ? "stack-auto" : "stack-zero");
		tree.refresh(root, 100, 100);
		top(frame % 2 ? first : later, "cached classes preserve zero versus auto stacking");
	}
	tree.setClassName(later, "");
	a.setProperty("position", "static"); a.setProperty("z-index", "2");
	tree.refresh(root, 100, 100);
	top(first, "positive z-index applies to static grid items");
	parent.setProperty("display", "block"); b.setProperty("margin-top", "-60px");
	tree.refresh(root, 100, 100);
	top(later, "z-index does not apply to an ordinary static block");
	a.setProperty("position", "relative"); tree.refresh(root, 100, 100);
	top(first, "relative positioning enables the authored stack level");
	a.setProperty("position", "static"); a.setProperty("z-index", "auto");
	a.setProperty("transform", "translateX(0px)"); tree.refresh(root, 100, 100);
	top(first, "identity transform creates a zero-level painting group");
	a.setProperty("transform", "none"); tree.refresh(root, 100, 100);
	top(later, "removing identity transform restores normal-flow paint order");
	a.setProperty("float", "left"); b.removeProperty("margin-top");
	tree.refresh(root, 100, 100);
	top(first, "float paints and hits above overlapping normal block background");
}

void testAbsoluteDescendantContainingBlockSizing()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(400, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto grid = NodeHandle(root).style();
	grid.setProperty("position", "relative");
	grid.setProperty("display", "grid");
	grid.setProperty("grid-template", "100px 100px / 100px 100px");
	grid.width(200); grid.height(200);
	const int wrapper = makeDiv(root, nullptr);
	NodeHandle(wrapper).style().setProperty("grid-area", "1 / 1 / 2 / 2");
	const int child = makeDiv(wrapper, nullptr);
	auto absolute = NodeHandle(child).style();
	absolute.setProperty("position", "absolute");
	absolute.setProperty("grid-area", "1 / 1 / 3 / 3");
	absolute.setProperty("width", "50%"); absolute.setProperty("height", "50%");
	const int inner = makeDiv(child, nullptr);
	NodeHandle(inner).style().setProperty("width", "100%");
	NodeHandle(inner).style().setProperty("height", "50%");
	tree.mount(root, 400, 300);
	expectEqual(tree.node(wrapper).layout.width, 100, "static grid item keeps its own track width");
	expectEqual(tree.node(child).layout.width, 100, "nested abspos percentage width uses grid containing area");
	expectEqual(tree.node(child).layout.height, 100, "nested abspos percentage height uses grid containing area");
	expectEqual(tree.node(inner).layout.width, 100, "nested abspos content relayout uses corrected width");
	expectEqual(tree.node(inner).layout.height, 50, "nested abspos content relayout uses corrected height");
	absolute.setProperty("grid-column", "span 2 / span 3");
	absolute.setProperty("grid-row", "1 / 99");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 100, "span-only abspos placement uses full padding width");
	expectEqual(tree.node(child).layout.height, 100, "invalid grid line becomes padding edge before sizing");
	grid.setProperty("padding", "20px");
	absolute.setProperty("grid-area", "auto");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 120, "auto grid area percentages include containing padding");
	expectEqual(tree.node(child).layout.height, 120, "auto grid area percentage height includes padding");
	expectEqual(tree.node(child).layout.x, 20, "nested automatic inset retains wrapper static x");
	expectEqual(tree.node(child).layout.y, 20, "nested automatic inset retains wrapper static y");
	absolute.setProperty("left", "10%"); absolute.setProperty("top", "10%");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x, 24, "percentage left and width use the same containing block");
	expectEqual(tree.node(child).layout.y, 24, "percentage top and height use the same containing block");
	NodeHandle(wrapper).style().setProperty("position", "relative");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 50, "new positioned wrapper becomes percentage containing block");
	expectEqual(tree.node(child).layout.height, 50, "positioned wrapper overrides outer grid area height");
	expectEqual(tree.node(child).layout.x, 30, "positioned wrapper offsets use its local basis");
	NodeHandle(wrapper).style().setProperty("position", "static");
	grid.setProperty("display", "block");
	NodeHandle(wrapper).style().width(40); NodeHandle(wrapper).style().height(30);
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 120, "non-grid absolute percentage bypasses static wrapper width");
	expectEqual(tree.node(child).layout.height, 120, "non-grid absolute percentage bypasses static wrapper height");
	NodeHandle(wrapper).style().setProperty("display", "grid");
	NodeHandle(wrapper).style().setProperty("justify-items", "center");
	NodeHandle(wrapper).style().setProperty("align-items", "center");
	NodeHandle(wrapper).style().width(160); NodeHandle(wrapper).style().height(140);
	absolute.setProperty("left", "auto"); absolute.setProperty("top", "auto");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 120, "outer containing block supplies size for grid static alignment");
	expectEqual(tree.node(child).layout.x, 40, "grid static horizontal alignment uses final percentage width");
	expectEqual(tree.node(child).layout.y, 30, "grid static vertical alignment uses final percentage height");
	NodeHandle(wrapper).style().setProperty("display", "block");
	NodeHandle(wrapper).style().setProperty("padding", "0 7px 0 3px");
	NodeHandle(wrapper).style().width(40);
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x - tree.node(wrapper).layout.x, 3, "block static position starts at the content edge");
	NodeHandle(wrapper).style().setProperty("direction", "rtl");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(wrapper).layout.x, 20, "block flow alignment follows the parent's LTR direction");
	expectEqual(tree.node(child).layout.x - tree.node(wrapper).layout.x, -77, "RTL static position anchors final width to the content right edge");
	absolute.setProperty("margin-right", "5px");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x - tree.node(wrapper).layout.x, -82, "RTL static position includes its right margin");
	grid.setProperty("direction", "rtl");
	NodeHandle(wrapper).style().removeProperty("direction");
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(wrapper).layout.x, 170, "block flow uses the parent's inherited RTL direction");
	expectEqual(tree.node(child).layout.x - tree.node(wrapper).layout.x, -82, "RTL static alignment inherits direction through the wrapper");
}

void testRotationFunctionLists()
{
	struct Case { const char *css; int x,y,w,h; };
	const Case cases[] = {
		{"rotate3d(2,0,0,60deg)",100,100,40,10},
		{"rotate3d(0,-3,0,60deg)",100,100,20,20},
		{"rotate3d(0,0,-4,90deg)",100,60,20,40},
		{"rotate3d(1,1,0,180deg)",100,100,20,40},
		{"rotate3d(1e200,1e200,0,180deg)",100,100,20,40},
		{"rotate(90deg) rotate3d(1,0,0,60deg)",90,100,10,40},
		{"rotate3d(1,0,0,60deg) rotate(90deg)",80,100,20,20},
		{"rotate(45deg) rotate(45deg)",80,100,20,40},
		{"rotate(1980deg) rotate(1980deg)",100,100,40,20},
		{"rotate(-1980deg) rotate(-1980deg)",100,100,40,20},
		{"rotate3d(0,0,0,60deg)",100,100,40,20},
		{"rotateX(90deg) rotateY(90deg)",100,100,0,40},
	};
	for (bool inlineStyle : {false, true}) for (const auto &c : cases) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(240,240,1);
		auto &tree = Tree::instance();
		if (!inlineStyle) StyleSheet::instance().registerRule("rotation", "transform", c.css);
		const int root = makeDiv(-1, nullptr), box = makeDiv(root,"rotation");
		NodeHandle(root).style().width(240); NodeHandle(root).style().height(240); NodeHandle(root).style().backgroundColor(0xffff);
		auto style = NodeHandle(box).style(); style.setProperty("position","absolute");
		style.left(100); style.top(100); style.width(40); style.height(20); style.backgroundColor(0);
		style.setProperty("transform-origin","0 0");
		if (inlineStyle) style.setProperty("transform",c.css);
		tree.mount(root,240,240);
		int different = 0;
		for (int y=0; y<240; ++y) for (int x=0; x<240; ++x) {
			const bool inside = x>=c.x && x<c.x+c.w && y>=c.y && y<c.y+c.h;
			if (displayPixelAt(x,y) != (inside ? 0 : 0xffff)) ++different;
		}
		expectEqual(different,0,c.css);
		if (c.w > 0 && c.h > 0) expectEqual(tree.hitTestNode(c.x+c.w/2,c.y+c.h/2),box,"rotation list hit testing follows painted rectangle");
		style.setProperty("transform","none"); tree.refresh(root,240,240);
		expectEqual(displayPixelAt(130,110),0,"removing rotation restores base box");
	}
}

void testIndividualRotationAndScale()
{
	for (bool reverse : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(240, 240, 1.0);
		auto &sheet = StyleSheet::instance();
		if (reverse) sheet.registerRule("composed", "transform", "translate(10px,20px)");
		sheet.registerRule("composed", "rotate", "90deg");
		sheet.registerRule("composed", "scale", "2 3");
		if (!reverse) sheet.registerRule("composed", "transform", "translate(10px,20px)");
		auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr), box = makeDiv(root, "composed");
		NodeHandle(root).style().width(240); NodeHandle(root).style().height(240); NodeHandle(root).style().backgroundColor(0xffff);
		auto s = NodeHandle(box).style(); s.setProperty("position", "absolute"); s.left(60); s.top(60); s.width(20); s.height(10);
		s.backgroundColor(0); s.setProperty("transform-origin", "0 0"); s.setProperty("translate", "100px 20px");
		tree.mount(root, 240, 240); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(80, 120), box, "independent rotate and scale compose with list translation in either declaration order");
		expectEqual(displayPixelAt(80, 120), 0, "composed transform paints at independently calculated position");
		s.setProperty("transform", "none"); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(140, 90), box, "transform none retains individual rotation and nonuniform scale");
		expectEqual(displayPixelAt(80, 120), 0xffff, "removing list translation clears old rotated paint");
		s.setProperty("rotate", "none"); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(190, 100), box, "rotate none retains individual scale");
		s.setProperty("scale", "none"); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(175, 85), box, "scale none restores identity scaling");
		s.removeProperty("rotate"); s.removeProperty("scale"); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(140, 90), box, "removing inline individual overrides restores class composition");
		s.setProperty("translate", "none"); s.setProperty("scale", "none"); s.setProperty("rotate", "x 180deg"); tree.refresh(root, 240, 240);
		expectEqual(displayPixelAt(65, 55), 0, "axis rotation reflects the painted plane");
		s.setProperty("backface-visibility", "hidden"); tree.refresh(root, 240, 240);
		expectEqual(displayPixelAt(65, 55), 0xffff, "independent rotation participates in backface culling");
		s.setProperty("backface-visibility", "visible"); s.setProperty("rotate", "1 1 0 180deg"); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(65, 75), box, "vector-axis half turn swaps the plane's x and y axes");
		s.setProperty("rotate", "none"); s.setProperty("scale", "-100% 200%"); tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(45, 75), box, "negative percentage scales reflect and resize independently");
		s.setProperty("transform", "translateX(10px)"); s.cssRotateDegrees(90); s.cssScale(2);
		tree.refresh(root, 240, 240);
		expectEqual(tree.hitTestNode(50, 100), box, "generated numeric CSS helpers compose outside the transform list");
		expectEqual(displayPixelAt(45, 75), 0xffff, "numeric CSS helper update clears previous reflected paint");
	}
}

void testIndividualTranslate()
{
	for (bool translateFirst : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &sheet = StyleSheet::instance();
		if (translateFirst) sheet.registerRule("shift", "translate", "30px 20px");
		sheet.registerRule("shift", "transform", "translateX(10px) scale(2)");
		if (!translateFirst) sheet.registerRule("shift", "translate", "30px 20px");
		sheet.registerRule("shift", "transform-origin", "0 0");
		auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr), box = makeDiv(root, "shift");
		NodeHandle(root).style().width(300); NodeHandle(root).style().height(300); NodeHandle(root).style().backgroundColor(0xffff);
		auto s = NodeHandle(box).style(); s.width(20); s.height(10); s.setProperty("position", "absolute"); s.left(20); s.top(30); s.backgroundColor(0);
		tree.mount(root, 300, 300); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(65, 55), box, "individual translation composes outside transform scale regardless of declaration order");
		expectEqual(displayPixelAt(65, 55), 0, "composed translation paints at the hit-tested location");
		s.setProperty("translate", "80px 20px"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(115, 55), box, "translation mutation invalidates transform cache");
		expectEqual(displayPixelAt(65, 55), 0xffff, "translation mutation clears old painted pixels");
		s.removeProperty("translate"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(65, 55), box, "removing inline translation restores independent class translation");
		s.setProperty("translate", "50% 100%"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(45, 45), box, "percent translation uses own untransformed box");
		s.width(40); s.height(20); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(55, 55), box, "percent translation follows own box resize");
		s.setProperty("--shift", "40px 30px"); s.setProperty("translate", "var(--shift)"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(75, 65), box, "custom property translation uses runtime parser");
		s.setProperty("translate", "none"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(35, 35), box, "translate none preserves transform-list translation");
		s.setProperty("translate", "30px 20px"); s.setProperty("transform", "none"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(55, 55), box, "transform none preserves independent translation");
		s.setProperty("width", "20px"); s.setProperty("height", "10px"); s.setProperty("left", "100px"); s.setProperty("top", "100px");
		s.setProperty("translate", "40px 20px"); s.setProperty("transform", "rotate(90deg) scale(2)"); tree.refresh(root, 300, 300);

		expectEqual(tree.hitTestNode(125, 125), box, "rotation does not rotate independent translation");
		expectEqual(displayPixelAt(125, 125), 0, "rotated individual translation matches literal paint coordinates");
		s.setProperty("translate", "none"); s.setProperty("transform", "translate(40px,20px) rotate(90deg) scale(2)"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(125, 125), box, "leading transform-list translation remains outside rotation");
		s.setProperty("transform", "rotate(90deg) translate(40px,20px) scale(2)"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(65, 145), box, "trailing transform-list translation rotates with local axes");
		expectEqual(displayPixelAt(125, 125), 0xffff, "changing transform order clears previous transformed bounds");
		s.setProperty("--move", "40px"); s.setProperty("transform", "translate(var(--move),20px) rotate(90deg) scale(2)"); tree.refresh(root, 300, 300);
		expectEqual(tree.hitTestNode(125, 125), box, "runtime transform parser preserves translation order");
	}

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr), parent = makeDiv(root, nullptr), fixed = makeDiv(parent, nullptr);
	NodeHandle(root).style().width(300); NodeHandle(root).style().height(300);
	auto p = NodeHandle(parent).style(); p.setProperty("position", "absolute"); p.left(40); p.top(60); p.width(100); p.height(100);
	auto f = NodeHandle(fixed).style(); f.setProperty("position", "fixed"); f.left(5); f.top(7); f.width(10); f.height(10);
	p.setProperty("translate", "0"); tree.mount(root, 300, 300);
	expectEqual(tree.node(fixed).layout.x, 45, "identity translate establishes a fixed containing block");
	expectEqual(tree.node(fixed).layout.y, 67, "identity translate containing block preserves vertical offset");
	p.setProperty("translate", "none"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(fixed).layout.x, 5, "translate none releases fixed containing block");
	p.setProperty("translate", "0 0"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(fixed).layout.x, 45, "restoring identity translate invalidates layout");
	p.setProperty("translate", "none"); p.setProperty("perspective", "100px"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(fixed).layout.x, 45, "perspective mutation still establishes fixed containing block");
	p.setProperty("perspective", "none");
	tree.refresh(root, 300, 300);
	expectEqual(rstyle(tree.node(parent).style).perspective, 0, "perspective none removes the perspective value");
	expectEqual(LayoutEngine::fixedContainingBlock(tree.node(fixed)), -1, "perspective none releases the containing block");
	expectEqual(tree.node(fixed).layout.x, 5, "removing perspective recomputes fixed geometry");
}

void testZeroTransformOrigins()
{
	for (const char *zero : {"0", "-0", "0px", "0em", "0%"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		const std::string origin = std::string(zero) + " " + zero;
		StyleSheet::instance().registerRule("origin", "transform-origin", origin.c_str());
		StyleSheet::instance().registerRule("origin", "perspective-origin", origin.c_str());
		auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr), box = makeDiv(root, "origin");
		NodeHandle(root).style().width(100); NodeHandle(box).style().width(20); NodeHandle(box).style().height(20);
		tree.mount(root, 300, 300);
		expectEqual(rstyle(tree.node(box).style).transform_origin_x, 0, "cached zero transform origin resolves to start");
		expectEqual(rstyle(tree.node(box).style).transform_origin_y, 0, "cached zero transform origin resolves vertical start");
		expectEqual(rstyle(tree.node(box).style).perspective_origin_x, 0, "zero perspective origin shares length handling");
		auto s = NodeHandle(box).style(); s.setProperty("transform-origin", "center"); tree.refresh(root, 300, 300);
		expectEqual(rstyle(tree.node(box).style).transform_origin_x, 500, "center origin remains percentage based");
		s.setProperty("--origin", zero); s.setProperty("transform-origin", "var(--origin) var(--origin)"); tree.refresh(root, 300, 300);
		expectEqual(rstyle(tree.node(box).style).transform_origin_x, 0, "dynamic origin zero matches compiled origin");
		s.removeProperty("transform-origin"); tree.refresh(root, 300, 300);
		expectEqual(rstyle(tree.node(box).style).transform_origin_x, 0, "removing origin override restores cached zero");
	}
}

void testAbsoluteBlockStaticPosition()
{
	for (const char *mode : {"horizontal-tb", "vertical-lr", "vertical-rl"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), before = makeDiv(root, nullptr), absolute = makeDiv(root, nullptr), after = makeDiv(root, nullptr);
		const bool vertical = mode[0] == 'v', reversed = std::strcmp(mode, "vertical-rl") == 0;
		auto parent = NodeHandle(root).style(); parent.width(100); parent.height(100); parent.setProperty("padding", "10px");
		parent.setProperty("position", "relative"); parent.setProperty("writing-mode", mode);
		auto preceding = NodeHandle(before).style(); preceding.width(20); preceding.height(20);
		preceding.setProperty(vertical ? (reversed ? "margin-left" : "margin-right") : "margin-bottom", "15px");
		preceding.setProperty("position", "relative"); preceding.top(40); preceding.left(30);
		auto box = NodeHandle(absolute).style(); box.setProperty("position", "absolute"); box.width(10); box.height(10);
		box.setProperty(vertical ? (reversed ? "margin-right" : "margin-left") : "margin-top", "3px");
		NodeHandle(after).style().width(20); NodeHandle(after).style().height(20);
		tree.mount(root, 300, 300);
		expectEqual(vertical ? tree.node(absolute).layout.x : tree.node(absolute).layout.y, reversed ? 62 : 48,
		            "automatic block inset follows preceding sibling and its margin before relative translation");
		expectEqual(vertical ? tree.node(absolute).layout.y : tree.node(absolute).layout.x, 10,
		            "automatic inline inset starts at content edge in every writing mode");
		preceding.setProperty(vertical ? (reversed ? "margin-left" : "margin-right") : "margin-bottom", "25px");
		tree.refresh(root, 300, 300);
		expectEqual(vertical ? tree.node(absolute).layout.x : tree.node(absolute).layout.y, reversed ? 52 : 58,
		            "preceding margin mutation recomputes static anchor");
		preceding.setProperty("display", "none"); tree.refresh(root, 300, 300);
		expectEqual(vertical ? tree.node(absolute).layout.x : tree.node(absolute).layout.y, reversed ? 97 : 13,
		            "hidden preceding sibling leaves static position at content start");
	}

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), before = makeDiv(root, nullptr), absolute = makeDiv(root, nullptr);
	auto parent = NodeHandle(root).style(); parent.width(200); parent.height(100); parent.setProperty("position", "relative"); parent.setProperty("padding", "10px");
	auto preceding = NodeHandle(before).style(); preceding.height(20); preceding.setProperty("margin-bottom", "15px");
	auto box = NodeHandle(absolute).style(); box.setProperty("position", "absolute"); box.width(10); box.height(10);
	tree.mount(root, 300, 300);
	expectEqual(tree.node(absolute).layout.y, 45, "static anchor excludes following absent content");
	box.width(30); tree.refresh(root, 300, 300);
	expectEqual(tree.node(absolute).layout.y, 45, "retained absolute size update preserves parent-relative static anchor");
	box.top(2); tree.refresh(root, 300, 300);
	expectEqual(tree.node(absolute).layout.y, 2, "definite inset overrides static anchor");
	box.setProperty("top", "auto"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(absolute).layout.y, 45, "removing inset restores block static anchor");
}

void testAbsoluteInlineStaticPosition()
{
	auto prepare = [](int width) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.width(width); style.setProperty("position", "relative");
		style.set(Property::FontId, 9101); style.set(Property::FontSize, 10); style.setProperty("line-height", "10px");
		return root;
	};

	for (int precedingKind = 0; precedingKind < 3; ++precedingKind) {
		auto &tree = Tree::instance(); const int root = prepare(300);
		if (precedingKind == 1) NodeHandle(root).appendChild(Document::instance().createText("X"));
		if (precedingKind == 2) {
			const int block = makeDiv(root, nullptr);
			NodeHandle(block).appendChild(Document::instance().createText("X"));
		}
		const int wrapper = makeSpan(root, nullptr);
		NodeHandle(wrapper).style().setProperty("padding-left", "100px");
		const int nested = makeSpan(wrapper, nullptr);
		NodeHandle(nested).style().setProperty("padding-right", "1px");
		auto text = Document::instance().createText(" X"); NodeHandle(nested).appendChild(text);
		tree.mount(root, 300, 300);
		expectEqual(tree.node(text.id()).layout.inline_indent, precedingKind == 1 ? 0 : -5,
		            "nested padded inline trims leading space at a block boundary but preserves a mid-line separator");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(300);
		NodeHandle(root).appendChild(Document::instance().createText("X"));
		const int wrapper = makeSpan(root, nullptr);
		NodeHandle(wrapper).style().setProperty("margin-top", "1px");
		auto text = Document::instance().createText(" X"); NodeHandle(wrapper).appendChild(text);
		tree.mount(root, 300, 300);
		expectEqual(tree.node(text.id()).layout.inline_indent, -5,
		            "legacy vertical-margin block wrappers retain line-start whitespace trimming");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(10);
		auto first = Document::instance().createText("XX"); NodeHandle(root).appendChild(first);
		const int absolute = makeSpan(root, nullptr); NodeHandle(absolute).style().setProperty("position", "absolute");
		auto following = Document::instance().createText("X"); NodeHandle(root).appendChild(following);
		tree.mount(root, 300, 300);
		expectEqual(tree.node(following.id()).layout.y, 10, "following run wraps after preceding inline run fills the line");
		expectEqual(tree.node(absolute).layout.x, 10, "inline static x uses source-order pen before following run wraps");
		expectEqual(tree.node(absolute).layout.y, 0, "inline static y stays on current line when following run wraps");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(100);
		auto first = Document::instance().createText("X"); NodeHandle(root).appendChild(first);
		NodeHandle(root).appendChild(Document::instance().createElement("br"));
		const int absolute = makeSpan(root, nullptr); NodeHandle(absolute).style().setProperty("position", "absolute");
		tree.mount(root, 300, 300);
		expectEqual(tree.node(absolute).layout.x, 0, "inline static position after BR starts at content inline edge");
		expectEqual(tree.node(absolute).layout.y, 10, "inline static position after trailing BR advances to next line");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(100);
		auto first = Document::instance().createText("XX"); NodeHandle(root).appendChild(first);
		const int absolute = makeSpan(root, nullptr); NodeHandle(absolute).style().setProperty("position", "absolute");
		tree.mount(root, 300, 300);
		expectEqual(tree.node(absolute).layout.x, 10, "inline static position at ordinary line end follows previous run");
		expectEqual(tree.node(absolute).layout.y, 0, "inline static position at ordinary line end remains on that line");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(100);
		auto first = Document::instance().createText("XX"); NodeHandle(root).appendChild(first);
		const int inlineAbsolute = makeSpan(root, nullptr);
		auto inlineStyle = NodeHandle(inlineAbsolute).style();
		inlineStyle.setProperty("position", "absolute"); inlineStyle.width(10); inlineStyle.height(10);
		inlineStyle.setProperty("margin-top", "2px");
		const int blockAbsolute = makeDiv(root, nullptr);
		auto blockStyle = NodeHandle(blockAbsolute).style();
		blockStyle.setProperty("position", "absolute"); blockStyle.width(10); blockStyle.height(10);
		auto following = Document::instance().createText("X"); NodeHandle(root).appendChild(following);
		tree.mount(root, 300, 300);
		expectEqual(tree.node(inlineAbsolute).layout.x, 10, "inline absolute anchor follows preceding text");
		expectEqual(tree.node(inlineAbsolute).layout.y, 2, "inline absolute with vertical margin keeps inline static position");
		expectEqual(tree.node(blockAbsolute).layout.x, 0, "block absolute static position uses the block inline-start edge");
		expectEqual(tree.node(blockAbsolute).layout.y, 10, "block absolute static position follows the preceding line");
		expectEqual(tree.node(following.id()).layout.y, 0, "block absolute hypothetical position does not break following inline flow");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(300);
		auto preceding = Document::instance().createText("X"); NodeHandle(root).appendChild(preceding);
		const int inlineParent = makeSpan(root, nullptr);
		NodeHandle(inlineParent).style().setProperty("padding-left", "100px");
		NodeHandle(inlineParent).appendChild(Document::instance().createText("Y"));
		NodeHandle(inlineParent).appendChild(Document::instance().createElement("br"));
		const int absolute = makeSpan(inlineParent, nullptr);
		auto absoluteStyle = NodeHandle(absolute).style();
		absoluteStyle.setProperty("position", "absolute");
		absoluteStyle.setProperty("padding-left", "100px");
		auto absoluteText = Document::instance().createText("Line 2");
		NodeHandle(absolute).appendChild(absoluteText);
		tree.mount(root, 300, 300);
		expectEqual(tree.node(absolute).layout.x, 0,
		            "continuation static position returns to the block line start after inline padding");
		expectEqual(tree.node(absoluteText.id()).layout.x, 100,
		            "absolute inline's own padding begins at the continuation line start");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(10);
		NodeHandle(root).appendChild(Document::instance().createText("XX XX"));
		const int absolute = makeSpan(root, nullptr);
		NodeHandle(absolute).style().setProperty("position", "absolute");
		tree.mount(root, 300, 300);
		expectEqual(tree.node(absolute).layout.x, 10, "static x after wrapped text follows the final line pen");
		expectEqual(tree.node(absolute).layout.y, 10, "static y after wrapped text uses the final line");
	}

	{
		auto &tree = Tree::instance(); const int root = prepare(100);
		auto first = Document::instance().createText("XX"); NodeHandle(root).appendChild(first);
		const int absolute = makeSpan(root, nullptr);
		auto positioned = NodeHandle(absolute).style();
		positioned.setProperty("position", "absolute"); positioned.width(10); positioned.height(10);
		tree.mount(root, 300, 300);
		expectEqual(tree.node(absolute).layout.x, 10, "initial inline static position follows preceding text");
		positioned.setProperty("position", "static");
		auto parent = NodeHandle(root).style();
		parent.setProperty("display", "flex"); parent.setProperty("justify-content", "flex-start"); parent.setProperty("align-items", "flex-start");
		tree.refresh(root, 300, 300);
		positioned.setProperty("position", "absolute"); tree.refresh(root, 300, 300);
		expectEqual(tree.node(absolute).layout.x, 0, "inline static anchor is cleared when the parent changes to flex");
	}
}

void testAbsoluteShrinkToFitStaticPosition()
{
	for (const char *direction : {"ltr", "rtl"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), wrapper = makeDiv(root, nullptr), child = makeDiv(wrapper, nullptr);
		auto grid = NodeHandle(root).style(), wrap = NodeHandle(wrapper).style(), abs = NodeHandle(child).style();
		grid.setProperty("position", "relative"); grid.setProperty("display", "grid");
		grid.setProperty("direction", direction); grid.setProperty("grid-template", "40px / 10px 33px");
		grid.width(20); grid.setProperty("padding", "2px 8px"); grid.setProperty("border", "3px solid #000000");
		wrap.setProperty(direction[0] == 'l' ? "padding-left" : "padding-right", "3px");
		abs.setProperty("position", "absolute"); abs.setProperty("grid-column", "2 / 3");
		for (int i = 0; i < 2; ++i) {
			const int f = makeDiv(child, nullptr); auto fs = NodeHandle(f).style();
			fs.setProperty("float", direction[0] == 'l' ? "left" : "right"); fs.width(20); fs.height(40);
		}
		tree.mount(root, 400, 300);
		expectEqual(tree.node(child).layout.width, 40, "absolute shrink-to-fit includes space before its grid area");
		expectEqual(tree.node(child).layout.height, 40, "absolute floats do not wrap at the narrower grid area width");
		abs.setProperty("left", "0"); tree.refresh(root, 400, 300);
		expectEqual(tree.node(child).layout.width, 33, "definite inset restricts shrink-to-fit to the grid area");
		expectEqual(tree.node(child).layout.height, 80, "floats wrap when the resolved inset leaves insufficient space");
		abs.setProperty("left", "auto"); tree.refresh(root, 400, 300);
		expectEqual(tree.node(child).layout.width, 40, "removing inset restores static-position available width");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
	auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr), child = makeDiv(root, nullptr);
	auto grid = NodeHandle(root).style(), abs = NodeHandle(child).style();
	grid.setProperty("display", "grid"); grid.setProperty("position", "relative");
	grid.width(100); grid.height(100); grid.setProperty("padding", "10px"); grid.setProperty("border", "3px solid #000000");
	abs.setProperty("position", "absolute"); abs.width(20); abs.height(20); abs.setProperty("margin-top", "2px");
	tree.mount(root, 400, 300);
	expectEqual(tree.node(child).layout.x, 3, "positioned grid automatic area starts at padding edge");
	expectEqual(tree.node(child).layout.y, 5, "grid static position applies margin from padding edge");
	grid.setProperty("position", "static"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x, 13, "non-containing grid static rectangle retains content edge");
	expectEqual(tree.node(child).layout.y, 15, "non-containing grid static rectangle includes padding before margin");
	grid.setProperty("position", "relative"); grid.setProperty("display", "block");
	grid.setProperty("padding", "0"); grid.setProperty("border", "0"); grid.width(200);
	abs.setProperty("width", "auto"); abs.setProperty("left", "10px"); abs.setProperty("padding", "0 10%");
	const int content = makeDiv(child, nullptr);
	NodeHandle(content).style().width(60); NodeHandle(content).style().height(20);
	tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 100, "shrink-to-fit resolves percentage padding against the containing block");
	expectEqual(tree.node(content).layout.width, 60, "intrinsic content survives the final used-width pass");
	abs.setProperty("min-width", "90px"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 130, "absolute minimum width constrains final content size");
	abs.setProperty("min-width", "0"); abs.setProperty("max-width", "40px"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 80, "absolute maximum width constrains final content size");
}

void testAbsolutePaddingBoxContainingBlock()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), wrapper = makeDiv(root, nullptr), child = makeDiv(wrapper, nullptr);
	auto parent = NodeHandle(root).style(), abs = NodeHandle(child).style();
	parent.setProperty("position", "relative"); parent.width(100); parent.height(80);
	parent.setProperty("padding", "10px"); parent.setProperty("border-style", "solid"); parent.setProperty("border-width", "3px 5px 7px 11px");
	abs.setProperty("position", "absolute"); abs.setProperty("width", "50%"); abs.setProperty("height", "50%");
	abs.setProperty("left", "10%"); abs.setProperty("top", "10%"); abs.setProperty("margin", "2px 4px 6px 8px");
	tree.mount(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 60, "absolute percentage width excludes containing borders");
	expectEqual(tree.node(child).layout.height, 50, "absolute percentage height excludes containing borders");
	expectEqual(tree.node(child).layout.x, 31, "absolute left percentage and margin start at padding edge");
	expectEqual(tree.node(child).layout.y, 15, "absolute top percentage and margin start at padding edge");
	abs.setProperty("left", "auto"); abs.setProperty("top", "auto");
	abs.setProperty("right", "10%"); abs.setProperty("bottom", "10%"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x, 55, "absolute right percentage includes right margin inside padding edge");
	expectEqual(tree.node(child).layout.y, 37, "absolute bottom percentage includes bottom margin inside padding edge");
	abs.setProperty("width", "auto"); abs.setProperty("height", "auto");
	abs.setProperty("left", "10px"); abs.setProperty("right", "20px");
	abs.setProperty("top", "5px"); abs.setProperty("bottom", "15px"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.width, 78, "absolute stretched width subtracts insets and margins from padding box");
	expectEqual(tree.node(child).layout.height, 72, "absolute stretched height subtracts insets and margins from padding box");
	expectEqual(tree.node(child).layout.x, 29, "stretched absolute left includes border and margin");
	expectEqual(tree.node(child).layout.y, 10, "stretched absolute top includes border and margin");
}

void testVerticalBlockFlow()
{
	for (const char *mode : {"vertical-lr", "vertical-rl", "sideways-lr", "sideways-rl"}) {
		for (const char *direction : {"ltr", "rtl"}) {
			resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
			auto &tree = Tree::instance();
			const int root = makeDiv(-1, nullptr), first = makeDiv(root, nullptr), second = makeDiv(root, nullptr);
			auto parent = NodeHandle(root).style(), a = NodeHandle(first).style(), b = NodeHandle(second).style();
			parent.setProperty("writing-mode", mode); parent.setProperty("direction", direction);
			parent.width(200); parent.height(100); parent.setProperty("padding", "5px"); parent.setProperty("border", "3px solid #000000");
			a.width(30); a.height(20); a.setProperty("margin", "4px 7px 9px 11px");
			b.width(40); b.height(25); b.setProperty("margin", "6px 17px 10px 13px");
			const bool reverseBlock = std::strstr(mode, "-rl") != nullptr;
			const bool reverseInline = (direction[0] == 'r') != (std::strcmp(mode, "sideways-lr") == 0);
			tree.mount(root, 400, 300);
			expectEqual(tree.node(first).layout.x, reverseBlock ? 171 : 19, "vertical block starts at the logical block-start edge");
			expectEqual(tree.node(second).layout.x, reverseBlock ? 114 : 62, "vertical block siblings collapse adjoining horizontal margins");
			expectEqual(tree.node(first).layout.y, reverseInline ? 79 : 12, "vertical block inline alignment follows direction and sideways mode");
			expectEqual(tree.node(second).layout.y, reverseInline ? 73 : 14, "vertical block inline end alignment includes margins");
			parent.setProperty("width", "auto"); tree.refresh(root, 400, 300);
			expectEqual(tree.node(root).layout.width, reverseBlock ? 123 : 127, "vertical automatic block size sums collapsed sibling extents");
			expectEqual(tree.node(first).layout.x, reverseBlock ? 78 : 19, "vertical reverse flow uses the final automatic width");
			b.setProperty("margin-top", "auto"); b.setProperty("margin-bottom", "auto"); tree.refresh(root, 400, 300);
			expectEqual(tree.node(second).layout.y, 45, "vertical inline auto margins center the child");
			b.setProperty("height", "auto"); tree.refresh(root, 400, 300);
			expectEqual(tree.node(second).layout.height, 100, "vertical automatic inline size fills the containing content height");
		}
	}
}

void testAbsoluteDistributedFallback()
{
	for (bool vertical : {false, true}) for (bool rtl : {false, true}) {
		for (const char *direction : {"row", "row-reverse", "column", "column-reverse"}) {
			resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
			auto &tree = Tree::instance();
			const int root = makeDiv(-1, nullptr), child = makeDiv(root, nullptr);
			auto parent = NodeHandle(root).style(), abs = NodeHandle(child).style();
			parent.width(100); parent.height(80); parent.setProperty("display", "flex"); parent.setProperty("position", "relative");
			parent.setProperty("padding", "2px"); parent.setProperty("border", "1px solid #000000");
			parent.setProperty("writing-mode", vertical ? "vertical-lr" : "horizontal-tb");
			parent.setProperty("direction", rtl ? "rtl" : "ltr"); parent.setProperty("flex-direction", direction);
			abs.setProperty("position", "absolute"); abs.width(20); abs.height(10);
			const bool row = std::strncmp(direction, "row", 3) == 0;
			const bool horizontal = row != vertical;
			const bool reverse = (std::strstr(direction, "reverse") != nullptr) != (row && rtl);
			const int free = horizontal ? 80 : 70;
			for (const char *alignment : {"space-around", "space-evenly", "space-between"}) {
				parent.setProperty("justify-content", alignment); tree.mount(root, 400, 300);
				const bool between = std::strcmp(alignment, "space-between") == 0;
				expectEqual(horizontal ? tree.node(child).layout.x : tree.node(child).layout.y,
				    3 + (between ? (reverse ? free : 0) : free / 2), "single absolute flex item uses distributed alignment fallback");
			}
			parent.setProperty("justify-content", "space-around"); parent.setProperty(horizontal ? "width" : "height", horizontal ? "10px" : "6px");
			tree.refresh(root, 400, 300);
			expectEqual(horizontal ? tree.node(child).layout.x : tree.node(child).layout.y, horizontal ? -2 : 1,
			    "distributed static center remains centered when the absolute child overflows");
		}
	}
}

void testComputedBorderInheritance()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 250, 1.0); setNativeDisplaySize(300, 250);
	auto &tree = Tree::instance(); auto &sheet = StyleSheet::instance();
	sheet.registerRule("parent-border", "font-size", "10px");
	sheet.registerRule("parent-border", "border-width", "1em");
	sheet.registerRule("different-parent", "border-width", "3px 5px 7px 9px");
	sheet.registerRule("inherit-border", "font-size", "40px");
	sheet.registerRule("inherit-border", "border-width", "inherit");
	const int root = makeDiv(-1, "parent-border"), first = makeDiv(root, "inherit-border"), second = makeDiv(root, "inherit-border");
	auto r = NodeHandle(root).style(), a = NodeHandle(first).style(), b = NodeHandle(second).style();
	sheet.registerRule("side-overrides", "border-width", "12px");
	sheet.registerRule("side-overrides", "border-left-width", "2px");
	sheet.registerRule("side-overrides", "border-top-width", "0");
	for (int repeat = 0; repeat < 2; ++repeat) {
		const int probe = makeDiv(root, "side-overrides");
		expectEqual(computedBorderWidth(tree.node(probe).style, 0), 0, "cached cascade retains a zero side override");
		expectEqual(computedBorderWidth(tree.node(probe).style, 1), 12, "cached cascade preserves untouched shorthand edges");
		expectEqual(computedBorderWidth(tree.node(probe).style, 3), 2, "cached cascade retains a narrower side override");
		NodeHandle(probe).style().setProperty("display", "none");
	}
	r.width(200); r.setProperty("background", "#ffffff");
	for (auto style : {a, b}) { style.width(40); style.height(20); style.setProperty("border-color", "#000000"); }
	b.setProperty("border-width", "inherit");
	tree.mount(root, 300, 250);
	for (int child : {first, second}) {
		expectEqual(tree.node(child).layout.width, 60, "inherit copies computed border width rather than resolving parent em in child font");
		expectEqual(tree.node(child).layout.height, 40, "inherited computed borders affect both axes");
	}
	expectEqual(displayPixelAt(15, 15), nativeRgb565(0), "inherited border paints its full computed width");
	expectEqual(displayPixelAt(25, 25), nativeRgb565(0xffff), "inherited border stops at the content edge");
	r.setProperty("border-width", "2em"); tree.refresh(root, 300, 250);
	for (int child : {first, second}) expectEqual(tree.node(child).layout.width, 80, "parent inline border mutation updates class and inline inheritance");
	r.removeProperty("border-width"); tree.setClassName(root, "different-parent"); tree.refresh(root, 300, 250);
	for (int child : {first, second}) {
		expectEqual(tree.node(child).layout.width, 54, "parent class mutation preserves all four inherited side widths");
		expectEqual(tree.node(child).layout.height, 30, "parent class mutation updates inherited vertical sides");
	}
	a.setProperty("border-width", "12px"); a.setProperty("border-left-width", "inherit");
	a.setProperty("border-top-width", "0"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.width, 61, "a narrower inherited side overrides the common border");
	expectEqual(tree.node(first).layout.height, 32, "zero side width overrides a common border");
	r.setProperty("border-left-width", "4px"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.width, 56, "individual inheritance remains live after parent side mutation");
	expectEqual(tree.node(first).layout.height, 32, "parent mutation preserves the explicit zero side override");
	a.removeProperty("border-left-width"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.width, 64, "removing side inheritance reveals the authored uniform border");
	a.setProperty("border-width", "6px"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.height, 32, "later shorthand resets every earlier side override");
	a.setProperty("border-width", "inherit 2px"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.width, 52, "mixed inherit shorthand is invalid and leaves prior declaration intact");
	a.removeProperty("border-width"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.width, 49, "removing shorthand restores class inheritance");
	r.set(Property::BorderWidth, 8); tree.refresh(root, 300, 250);
	for (int child : {first, second}) expectEqual(tree.node(child).layout.width, 56, "direct property mutation updates explicit inheritance and resets parent sides");
	a.setProperty("font-size", "80px"); tree.refresh(root, 300, 250);
	expectEqual(tree.node(first).layout.width, 56, "child font mutation never reinterprets inherited computed pixels");
}

void testVisibilityInheritanceAndPainting()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(120, 100, 1.0); setNativeDisplaySize(120, 100);
	auto &tree = Tree::instance();
	StyleSheet::instance().registerRule("hidden-box", "visibility", "hidden");
	const int root = makeDiv(-1, nullptr), hidden = makeDiv(root, "hidden-box");
	const int inherited = makeDiv(hidden, nullptr), visible = makeDiv(hidden, nullptr);
	auto r = NodeHandle(root).style(), h = NodeHandle(hidden).style(), i = NodeHandle(inherited).style(), v = NodeHandle(visible).style();
	r.width(120); r.height(100); r.setProperty("background", "#ffffff");
	h.width(100); h.height(80); h.setProperty("background", "#ff0000");
	i.width(40); i.height(20); i.setProperty("background", "#0000ff");
	v.width(20); v.height(20); v.setProperty("background", "#00ff00"); v.setProperty("visibility", "visible");
	tree.mount(root, 120, 100);
	expectEqual(tree.node(inherited).style.visibility, 1, "visibility inherits through cached class styles");
	expectEqual(tree.node(visible).layout.y, 20, "hidden child retains its layout space");
	expectEqual(displayPixelAt(5, 5), nativeRgb565(0xffff), "hidden backgrounds and descendants do not paint");
	expectEqual(tree.hitTestNode(5, 5), root, "hidden boxes do not intercept input");
	expectEqual(displayPixelAt(5, 25), nativeRgb565(0x07e0), "explicit visible descendant paints through hidden ancestor");
	expectEqual(tree.hitTestNode(5, 25), visible, "explicit visible descendant accepts input");
	v.setProperty("position", "absolute"); v.left(30); v.top(30); tree.refresh(root, 120, 100);
	expectEqual(displayPixelAt(35, 35), nativeRgb565(0x07e0), "hoisted visible descendant paints through hidden ancestor");
	v.removeProperty("visibility"); tree.refresh(root, 120, 100);
	expectEqual(tree.node(visible).style.visibility, 1, "removing visibility restores inheritance");
	expectEqual(displayPixelAt(35, 35), nativeRgb565(0xffff), "visibility mutation clears old descendant pixels");
	h.setProperty("visibility", "visible"); tree.refresh(root, 120, 100);
	expectEqual(tree.node(inherited).style.visibility, 0, "parent visibility mutation recomputes descendants");
	expectEqual(displayPixelAt(5, 5), nativeRgb565(0x001f), "visible mutation repaints inherited descendants");
	h.setProperty("visibility", "invalid"); tree.refresh(root, 120, 100);
	expectEqual(tree.node(hidden).style.visibility, 0, "invalid visibility does not replace valid declaration");
	h.removeProperty("visibility"); tree.refresh(root, 120, 100);
	expectEqual(tree.node(hidden).style.visibility, 1, "removing inline visibility restores class declaration");
	tree.setClassName(hidden, ""); tree.refresh(root, 120, 100);
	expectEqual(tree.node(inherited).style.visibility, 0, "class removal restores visible inheritance");
	h.setProperty("visibility", "collapse"); tree.refresh(root, 120, 100);
	expectEqual(tree.node(hidden).layout.height, 80, "collapse outside flex preserves block layout");
	expectEqual(displayPixelAt(5, 5), nativeRgb565(0xffff), "collapse outside flex behaves as hidden");
}

void testCollapsedFlexItems()
{
	for (bool column : {false, true}) for (int collapsedIndex : {0, 1, 3}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), flex = makeDiv(root, nullptr);
		NodeHandle(root).style().width(300); NodeHandle(root).style().height(300);
		auto parent = NodeHandle(flex).style(); parent.setProperty("display", "flex");
		parent.setProperty("flex-direction", column ? "column" : "row");
		parent.setProperty("width", "max-content"); parent.setProperty("gap", "10px");
		int items[4];
		for (int n = 0; n < 4; ++n) {
			items[n] = makeDiv(flex, nullptr); auto item = NodeHandle(items[n]).style();
			item.width(column ? (n == collapsedIndex ? 40 : 20) : 20);
			item.height(column ? 20 : (n == collapsedIndex ? 40 : 20)); item.setProperty("flex", "none");
		}
		auto collapsed = NodeHandle(items[collapsedIndex]).style(); collapsed.setProperty("visibility", "collapse");
		tree.mount(root, 300, 300);
		expectEqual(column ? tree.node(flex).layout.height : tree.node(flex).layout.width, 80, "collapsed flex items add neither main size nor gap to intrinsic size");
		expectEqual(column ? tree.node(flex).layout.width : tree.node(flex).layout.height, 40, "collapsed flex line retains its original cross size");
		int position = 0;
		for (int n = 0; n < 4; ++n) if (n != collapsedIndex) {
			expectEqual(column ? tree.node(items[n]).layout.y : tree.node(items[n]).layout.x, position, "collapsed item contributes no adjacent gap"); position += 30;
		}
		collapsed.setProperty("visibility", "visible"); tree.refresh(root, 300, 300);
		expectEqual(column ? tree.node(flex).layout.height : tree.node(flex).layout.width, 110, "uncollapsing restores main size and gaps");
		collapsed.setProperty("visibility", "collapse"); tree.refresh(root, 300, 300);
		expectEqual(column ? tree.node(flex).layout.height : tree.node(flex).layout.width, 80, "recollapsing remeasures original strut");
	}
	// A stretched strut migrates from the original second line to the first.
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(100, 100, 1.0); setNativeDisplaySize(100, 100);
	auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr), flex = makeDiv(root, nullptr);
	auto r = NodeHandle(root).style(); r.width(100); r.height(100); r.setProperty("background", "#ffffff");
	auto f = NodeHandle(flex).style(); f.width(25); f.height(60); f.setProperty("display", "flex"); f.setProperty("flex-wrap", "wrap");
	const int first = makeDiv(flex, nullptr), collapsed = makeDiv(flex, nullptr), last = makeDiv(flex, nullptr);
	NodeHandle(first).style().width(25); NodeHandle(first).style().height(10);
	auto c = NodeHandle(collapsed).style(); c.width(10); c.setProperty("visibility", "collapse");
	NodeHandle(last).style().width(10); NodeHandle(last).style().height(30);
	const int overrideChild = makeDiv(collapsed, nullptr); auto v = NodeHandle(overrideChild).style();
	v.setProperty("position", "absolute"); v.left(60); v.top(60); v.width(20); v.height(20);
	v.setProperty("background", "#ff0000"); v.setProperty("visibility", "visible");
	tree.mount(root, 100, 100);
	expectEqual(tree.node(last).layout.y, 40, "collapsed strut retains original stretched line size after migration");
	expectEqual(displayPixelAt(65, 65), nativeRgb565(0xffff), "collapsed flex subtree suppresses even visible positioned descendants");
	expectEqual(tree.hitTestNode(65, 65), root, "collapsed flex descendants cannot be hit through paint-order hoisting");
	c.setProperty("visibility", "hidden"); tree.refresh(root, 100, 100);
	expectEqual(tree.node(last).layout.y, 20, "hidden flex item keeps main size and original line wrapping");
	expectEqual(displayPixelAt(65, 65), nativeRgb565(0xf800), "hidden flex ancestor permits visible descendant override");
}

void testClearingBreakWhitespace()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), floated = makeDiv(root, nullptr);
	NodeHandle(root).style().width(200); NodeHandle(root).style().setProperty("line-height", "20px");
	auto f = NodeHandle(floated).style(); f.width(30); f.height(30); f.setProperty("margin-bottom", "5px"); f.setProperty("float", "left");
	auto br1 = Document::instance().createElement("br"); br1.style().setProperty("clear", "both"); NodeHandle(root).appendChild(br1);
	auto whitespace = [&](const char *value) {
		auto text = Document::instance().createText(value); tree.setTagName(text.id(), "#text"); NodeHandle(root).appendChild(text);
	};
	whitespace(" \n ");
	const int hidden = makeDiv(root, nullptr); NodeHandle(hidden).style().setProperty("display", "none");
	whitespace(" \t ");
	auto br2 = Document::instance().createElement("br"); NodeHandle(root).appendChild(br2);
	tree.mount(root, 300, 300);
	expectEqual(tree.node(br1.id()).layout.x, 30, "clearing BR remains on the current line beside the float");
	expectEqual(tree.node(br1.id()).layout.y, 0, "clear moves the next line rather than the BR itself");
	expectEqual(tree.node(br2.id()).layout.y, 35, "clearing BR advances below the float margin edge");
	expectEqual(tree.node(root).layout.height, 55, "hidden content and collapsed whitespace add no lines between BRs");
	f.height(10); tree.refresh(root, 300, 300);
	expectEqual(tree.node(br2.id()).layout.y, 20, "short float clearance does not reduce the current line height");
	expectEqual(tree.node(root).layout.height, 40, "empty second line contributes one line height");
}

void testOrthogonalFlexPercentageGaps()
{
	for (bool vertical : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), flex = makeDiv(root, nullptr);
		const int first = makeDiv(flex, nullptr), second = makeDiv(flex, nullptr);
		auto outer = NodeHandle(root).style(), parent = NodeHandle(flex).style();
		outer.width(400); outer.height(300); outer.setProperty("writing-mode", vertical ? "horizontal-tb" : "vertical-lr");
		parent.setProperty("writing-mode", vertical ? "vertical-lr" : "horizontal-tb");
		parent.setProperty("display", "flex"); parent.setProperty("flex-flow", "row wrap");
		parent.setProperty("border", "2px solid #000000"); parent.setProperty("row-gap", "20%"); parent.setProperty("column-gap", "10%");
		parent.setProperty("align-content", "start");
		for (int id : {first, second}) { auto s = NodeHandle(id).style(); s.width(50); s.height(50); s.setProperty("flex", "none"); }
		tree.mount(root, 400, 300);
		expectEqual(tree.node(flex).layout.width, 104, "orthogonal fit-content resolves percentage gap after measuring intrinsic width");
		expectEqual(tree.node(flex).layout.height, 104, "orthogonal fit-content resolves percentage gap after measuring intrinsic height");
		expectEqual(tree.node(second).layout.x - tree.node(first).layout.x, vertical ? 50 : 0, "orthogonal percentage gap wraps second item onto a new line x");
		expectEqual(tree.node(second).layout.y - tree.node(first).layout.y, vertical ? 0 : 50, "orthogonal percentage gap wraps second item onto a new line y");
		parent.setProperty("column-gap", "0"); tree.refresh(root, 400, 300);
		expectEqual(tree.node(flex).layout.width, vertical ? 54 : 104, "zero-gap mutation restores one flex line width");
		expectEqual(tree.node(flex).layout.height, vertical ? 104 : 54, "zero-gap mutation restores one flex line height");
		parent.setProperty("column-gap", "10%"); parent.setProperty(vertical ? "height" : "width", "150px"); tree.refresh(root, 400, 300);
		expectEqual(vertical ? tree.node(flex).layout.height : tree.node(flex).layout.width, 154, "explicit inline size replaces orthogonal fit-content size");
		expectEqual(vertical ? tree.node(second).layout.y - tree.node(first).layout.y : tree.node(second).layout.x - tree.node(first).layout.x, 65, "explicit inline size supplies percentage gap basis");
		parent.setProperty(vertical ? "height" : "width", "auto"); tree.refresh(root, 400, 300);
		expectEqual(tree.node(flex).layout.width, 104, "restoring auto inline size remeasures width");
		expectEqual(tree.node(flex).layout.height, 104, "restoring auto inline size remeasures height");
		parent.setProperty(vertical ? "max-height" : "max-width", "70px"); tree.refresh(root, 400, 300);
		expectEqual(vertical ? tree.node(flex).layout.height : tree.node(flex).layout.width, 74, "maximum constrains final orthogonal inline size");
		parent.setProperty(vertical ? "max-height" : "max-width", "none");
		parent.setProperty(vertical ? "min-height" : "min-width", "140px"); tree.refresh(root, 400, 300);
		expectEqual(vertical ? tree.node(flex).layout.height : tree.node(flex).layout.width, 144, "minimum expands final orthogonal inline size");
		expectEqual(vertical ? tree.node(second).layout.y - tree.node(first).layout.y : tree.node(second).layout.x - tree.node(first).layout.x, 64, "minimum-constrained inline size supplies percentage gap basis");
	}
}

void testAbsoluteAutoInsetAlignment()
{
	for (bool vertical : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), grid = makeDiv(root, nullptr), child = makeDiv(grid, nullptr);
		auto cb = NodeHandle(root).style(), parent = NodeHandle(grid).style(), abs = NodeHandle(child).style();
		cb.setProperty("position", "relative"); cb.width(200); cb.height(25); cb.setProperty("border", "3px solid #000000");
		cb.setProperty("writing-mode", vertical ? "vertical-rl" : "horizontal-tb");
		parent.setProperty("display", "grid"); parent.width(30); parent.height(25);
		parent.setProperty("padding", "2px 1px"); parent.setProperty("border", "1px solid #000000");
		parent.setProperty("margin-right", "5px"); parent.setProperty("grid-template", "3px 14px 3px / 2px 20px 2px");
		abs.setProperty("position", "absolute"); abs.width(45); abs.height(35);
		abs.setProperty("grid-area", "2 / 2 / 3 / 3"); abs.setProperty("align-self", "safe center"); abs.setProperty("justify-self", "safe end");
		tree.mount(root, 400, 300);
		expectEqual(tree.node(child).layout.x, vertical ? 158 : 3, "safe static alignment resolves auto insets against the actual containing block");
		expectEqual(tree.node(child).layout.y, vertical ? 3 : 9, "safe center uses the symmetric inset-modified containing block");
		abs.setProperty("justify-self", "unsafe end"); tree.refresh(root, 400, 300);
		expectEqual(vertical ? tree.node(child).layout.y : tree.node(child).layout.x, vertical ? -4 : -10, "unsafe static alignment preserves overflow at the specified edge");
		abs.setProperty("justify-self", "safe end"); abs.setProperty(vertical ? "left" : "top", "20px"); tree.refresh(root, 400, 300);
		expectEqual(vertical ? tree.node(child).layout.x : tree.node(child).layout.y, 23, "one definite inset overrides self-alignment on its axis");
	}
	{
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), child = makeDiv(root, nullptr);
		auto parent = NodeHandle(root).style(), abs = NodeHandle(child).style();
		parent.setProperty("position", "relative"); parent.setProperty("display", "grid");
		parent.width(4); parent.height(2); parent.setProperty("padding", "1px 2px");
		parent.setProperty("border", "1px solid #000000"); parent.setProperty("grid-template", "0px 2px 0px / 3px 2px 3px");
		abs.setProperty("position", "absolute"); abs.width(8); abs.height(6);
		abs.setProperty("grid-area", "2 / 2 / 3 / 3"); abs.setProperty("align-self", "center");
		tree.mount(root, 400, 300);
		expectEqual(tree.node(child).layout.y, 0, "unqualified grid center preserves overflow");
		abs.setProperty("align-self", "end"); tree.refresh(root, 400, 300);
		expectEqual(tree.node(child).layout.y, -2, "unqualified grid end preserves overflow");
		abs.setProperty("align-self", "safe center"); tree.refresh(root, 400, 300);
		expectEqual(tree.node(child).layout.y, 2, "explicit safe grid center falls back to start");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(400, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), flex = makeDiv(root, nullptr), child = makeDiv(flex, nullptr);
	auto cb = NodeHandle(root).style(), parent = NodeHandle(flex).style(), abs = NodeHandle(child).style();
	cb.setProperty("position", "relative"); cb.width(200); cb.height(100); cb.setProperty("border", "3px solid #000000");
	parent.setProperty("display", "flex"); parent.setProperty("flex-direction", "column"); parent.setProperty("writing-mode", "vertical-rl");
	parent.width(50); parent.height(50); parent.setProperty("margin-left", "125px"); parent.setProperty("border", "3px solid #000000");
	abs.setProperty("position", "absolute"); abs.width(229); abs.height(79); abs.setProperty("align-self", "safe center");
	tree.mount(root, 400, 300);
	expectEqual(tree.node(child).layout.x, 3, "default flex main alignment keeps overflowing start reachable in outer containing block");
	abs.setProperty("margin", "5px"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x, 8, "default flex main alignment keeps the margin edge reachable");
	parent.setProperty("justify-content", "unsafe flex-start"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x, -53, "explicit unsafe flex main alignment permits start overflow");
	parent.setProperty("flex-direction", "row"); parent.setProperty("justify-content", "normal");
	abs.setProperty("align-self", "center"); abs.setProperty("margin", "0"); tree.refresh(root, 400, 300);
	expectEqual(tree.node(child).layout.x, -26, "default flex cross alignment limits overflow in the actual containing block");

}

void testGridAreasAndShorthand()
{
	for (const char *mode : {"horizontal-tb", "vertical-rl", "vertical-lr"}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto parent = NodeHandle(root).style();
		parent.setProperty("display", "grid");
		parent.setProperty("position", "relative");
		parent.setProperty("writing-mode", mode);
		parent.setProperty("grid", "10px 40px 10px / 20px 60px 20px");
		parent.setProperty("gap", "5px 10px");
		parent.setProperty("padding", "4px");
		parent.setProperty("border", "2px solid #000000");
		parent.width(120); parent.height(120);
		const int child = makeDiv(root, nullptr);
		auto style = NodeHandle(child).style();
		style.setProperty("grid-area", "2 / 2 / 3 / 3");
		style.setProperty("position", "absolute");
		style.setProperty("justify-self", "center");
		style.setProperty("align-self", "center");
		style.width(20); style.height(20);
		tree.mount(root, 300, 300);
		const bool vertical = std::strcmp(mode, "horizontal-tb") != 0;
		const bool reversed = std::strcmp(mode, "vertical-rl") == 0;
		const int x = !vertical ? 56 : reversed ? 81 : 31;
		const int y = vertical ? 56 : 31;
		expectEqual(tree.node(child).layout.x, x, "grid area: centered absolute x follows tracks and writing mode");
		expectEqual(tree.node(child).layout.y, y, "grid area: centered absolute y follows tracks and writing mode");
		style.setProperty("grid-area", "-3 / -3 / -2 / -2");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(child).layout.x, x, "grid area: negative lines count from explicit end");
		expectEqual(tree.node(child).layout.y, y, "grid area: negative rows count from explicit end");
		style.setProperty("grid-area", "2 / 2 / span 1 / span 1");
		style.setProperty("left", "0"); style.setProperty("top", "0");
		style.setProperty("width", "50%"); style.setProperty("height", "50%");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(child).layout.width, vertical ? 20 : 30, "grid area: percentage width uses area width");
		expectEqual(tree.node(child).layout.height, vertical ? 30 : 20, "grid area: percentage height uses area height");
		expectEqual(tree.node(child).layout.x, !vertical ? 36 : reversed ? 71 : 21, "grid area: explicit inset uses physical area edge");
		expectEqual(tree.node(child).layout.y, vertical ? 36 : 21, "grid area: top inset uses physical area edge");
		style.setProperty("position", "relative");
		style.setProperty("left", "auto"); style.setProperty("top", "auto");
		style.width(20); style.height(20);
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(child).layout.x, x, "grid area: normal-flow placement uses the same area geometry");
		expectEqual(tree.node(child).layout.y, y, "grid area: normal-flow placement honors writing mode");
	}
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto parent = NodeHandle(root).style();
	parent.setProperty("display", "grid");
	parent.width(100);
	parent.setProperty("--tracks", "20px 30px / 40px 60px");
	parent.setProperty("grid-template", "var(--tracks)");
	const int placed = makeDiv(root, nullptr);
	NodeHandle(placed).style().setProperty("grid-area", "1 / 2 / 2 / 3");
	const int automatic = makeDiv(root, nullptr);
	const int spanning = makeDiv(root, nullptr);
	NodeHandle(spanning).style().setProperty("grid-column", "1 / span 2");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(placed).layout.x, 40, "grid explicit placement reserves its column");
	expectEqual(tree.node(automatic).layout.x, 0, "grid auto placement fills the unoccupied first cell");
	expectEqual(tree.node(spanning).layout.y, 20, "grid spanning item advances to a free row");
	expectEqual(tree.node(spanning).layout.width, 100, "grid span covers both tracks");
	expectEqual(tree.node(root).layout.height, 50, "grid auto height includes explicit rows");
	parent.setProperty("--tracks", "10px 50px / 30px 70px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(placed).layout.x, 30, "grid shorthand variable re-evaluates on mutation");
	expectEqual(tree.node(spanning).layout.y, 10, "grid shorthand variable updates both axes");
	parent.setProperty("grid-template-columns", "25px 75px");
	parent.setProperty("--tracks", "15px 45px / 20px 80px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(placed).layout.x, 25, "grid longhand overrides only its shorthand axis");
	expectEqual(tree.node(spanning).layout.y, 15, "grid other shorthand axis remains live");
	parent.setProperty("grid", "nonsense / 99px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(placed).layout.x, 25, "invalid grid shorthand does not partially apply");
	NodeHandle(placed).style().setProperty("grid-area", "0 / 1 / 2 / 2");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(placed).layout.x, 25, "invalid grid line zero preserves the earlier area");
	parent.setProperty("grid", "none");
	tree.refresh(root, 300, 300);
	expectEqual(rstyle(tree.node(root).style).grid_row_count, 0, "grid none resets explicit rows");
	expectEqual(rstyle(tree.node(root).style).grid_column_count, 0, "grid none resets explicit columns");
	resetNativeHost();
	StyleSheet::instance().clear();
	StyleSheet::instance().registerRule("area-parent", "display", "grid");
	StyleSheet::instance().registerRule("area-parent", "grid", "20px 30px / 40px 60px");
	StyleSheet::instance().registerRule("area-child", "grid-area", "2 / 2 / 3 / 3");
	const int styledRoot = makeDiv(-1, "area-parent");
	NodeHandle(styledRoot).style().width(100);
	const int styledChild = makeDiv(styledRoot, "area-child");
	tree.mount(styledRoot, 300, 300);
	expectEqual(tree.node(styledChild).layout.x, 40, "class grid shorthand and area apply together");
	expectEqual(tree.node(styledChild).layout.y, 20, "class grid area selects explicit row");
	NodeHandle(styledChild).style().setProperty("grid-area", "1 / 1 / 2 / 2");
	tree.refresh(styledRoot, 300, 300);
	expectEqual(tree.node(styledChild).layout.x, 0, "inline grid area changes placement");
	NodeHandle(styledChild).style().removeProperty("grid-area");
	tree.refresh(styledRoot, 300, 300);
	expectEqual(tree.node(styledChild).layout.x, 40, "removing inline grid area restores class placement");
	NodeHandle(styledRoot).style().setProperty("position", "relative");
	NodeHandle(styledChild).style().setProperty("position", "absolute");
	NodeHandle(styledChild).style().setProperty("grid-area", "99 / 99 / 100 / 100");
	NodeHandle(styledChild).style().width(20);
	NodeHandle(styledChild).style().height(200);
	NodeHandle(styledChild).style().setProperty("justify-self", "center");
	tree.refresh(styledRoot, 300, 300);
	expectEqual(tree.node(styledChild).layout.x, 40, "out-of-range abspos lines become auto without adding tracks");
	expectEqual(tree.node(styledRoot).layout.height, 50, "abspos contents do not size explicit grid rows");
	NodeHandle(styledRoot).style().setProperty("grid-template", "repeat(2, 10px 15px) / repeat(2, 50px)");
	tree.refresh(styledRoot, 300, 300);
	expectEqual(tree.node(styledRoot).layout.height, 50, "repeat track lists expand before sizing empty grid");
	NodeHandle(styledRoot).style().setProperty("grid-template", "repeat(0, 99px) / 99px");
	tree.refresh(styledRoot, 300, 300);
	expectEqual(tree.node(styledRoot).layout.height, 50, "invalid repeat count preserves previous template");
	NodeHandle(styledRoot).style().removeProperty("grid-template");
	tree.refresh(styledRoot, 300, 300);
	expectEqual(rstyle(tree.node(styledRoot).style).grid_row_count, 2, "removing grid shorthand restores class rows");
	NodeHandle(styledRoot).style().setProperty("grid", "none / 20px");
	NodeHandle(styledRoot).style().setProperty("padding", "10px");
	NodeHandle(styledRoot).style().height(50);
	NodeHandle(styledChild).style().setProperty("grid-area", "2 / span 2 / 3 / 3");
	NodeHandle(styledChild).style().setProperty("align-self", "center");
	NodeHandle(styledChild).style().width(10);
	NodeHandle(styledChild).style().height(10);
	tree.refresh(styledRoot, 300, 300);
	expectEqual(tree.node(styledChild).layout.y, 30, "abspos does not create a phantom row in a columns-only grid");
	expectEqual(tree.node(styledChild).layout.x, 55, "abspos span from a missing line becomes auto");
	resetNativeHost();
	StyleSheet::instance().clear();
	const int autoRoot = makeDiv(-1, nullptr);
	auto autoStyle = NodeHandle(autoRoot).style();
	autoStyle.setProperty("display", "grid");
	autoStyle.setProperty("grid-template-columns", "auto auto");
	autoStyle.setProperty("justify-content", "start");
	autoStyle.width(100);
	const int wide = makeDiv(autoRoot, nullptr);
	NodeHandle(wide).style().setProperty("grid-area", "1 / 1 / 2 / span 2");
	NodeHandle(wide).style().width(100);
	const int narrow = makeDiv(autoRoot, nullptr);
	NodeHandle(narrow).style().setProperty("grid-area", "2 / 1 / 3 / 2");
	NodeHandle(narrow).style().width(80);
	const int last = makeDiv(autoRoot, nullptr);
	NodeHandle(last).style().setProperty("grid-area", "3 / 2 / 4 / 3");
	tree.mount(autoRoot, 300, 300);
	expectEqual(tree.node(last).layout.x, 90, "auto grid tracks size non-spanning contributions before spans");
	expectEqual(tree.node(last).layout.width, 10, "auto grid spanning deficit is shared across eligible tracks");
	autoStyle.setProperty("grid-template-columns", "auto auto auto");
	autoStyle.width(150);
	NodeHandle(narrow).style().setProperty("grid-area", "2 / 2 / 3 / span 2");
	NodeHandle(narrow).style().width(100);
	NodeHandle(last).style().setProperty("grid-area", "3 / 3 / 4 / 4");
	tree.refresh(autoRoot, 300, 300);
	expectEqual(tree.node(last).layout.x, 100, "equal grid spans plan growth from shared base sizes");
	expectEqual(tree.node(last).layout.width, 50, "overlapping equal spans do not bias track growth by source order");
}

void testGridJustifySelf()
{
	for (bool absolute : {false, true}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setViewportMetrics(200, 200, 1.0);
		auto &tree = Tree::instance();
		StyleSheet::instance().registerRule("self-end", "justify-self", "end");
		const int root = makeDiv(-1, nullptr);
		auto parent = NodeHandle(root).style();
		parent.setProperty("display", "grid");
		parent.setProperty("position", "relative");
		parent.setProperty("justify-items", "center");
		parent.width(100); parent.height(80);
		const int child = makeDiv(root, "self-end");
		auto style = NodeHandle(child).style();
		if (absolute) style.setProperty("position", "absolute");
		style.width(20); style.height(20);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 80, "grid justify-self: class overrides parent alignment");
		style.setProperty("justify-self", "start");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 0, "grid justify-self: inline overrides class after mount");
		style.setProperty("justify-self", "space-between");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 0, "grid justify-self: invalid distribution preserves earlier value");
		style.removeProperty("justify-self");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 80, "grid justify-self: removing inline restores class");
		style.setProperty("justify-self", "auto");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 40, "grid justify-self: auto uses parent justify-items");
		parent.setProperty("justify-items", "start");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 0, "grid justify-self: auto responds to parent mutation");
		style.setProperty("justify-self", "unsafe center");
		style.width(140);
		// Keep the normal-flow track definite so the oversized child overflows.
		parent.setProperty("grid-template-columns", "100px");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, -20, "grid justify-self: unsafe center permits negative free space");
		style.setProperty("justify-self", "safe end");
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, 0, "grid justify-self: safe overflow falls back to start");
		const int refreshed = tree.node(child).layout.x;
		tree.computeLayout(root, 200, 200);
		expectEqual(tree.node(child).layout.x, refreshed, "grid justify-self: refresh matches full layout");
		if (!absolute) {
			const int content = makeDiv(child, nullptr);
			NodeHandle(content).style().width(20);
			NodeHandle(content).style().height(10);
			style.setProperty("width", "auto");
			style.setProperty("justify-self", "center");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.width, 20, "grid justify-self: centered auto width uses content");
			expectEqual(tree.node(child).layout.x, 40, "grid justify-self: centered intrinsic item position");
			style.setProperty("justify-self", "stretch");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.width, 100, "grid justify-self: stretch assigns the track width");
			style.setProperty("margin-left", "auto");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.width, 20, "grid auto margins disable stretching");
			expectEqual(tree.node(child).layout.x, 80, "grid auto margin consumes positive free space");
			style.setProperty("justify-self", "center");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.x, 80, "grid auto margin takes precedence over justify-self");
			style.setProperty("margin-right", "auto");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.x, 40, "grid two auto margins share positive free space");
			style.width(140);
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.x, 0, "grid overflowing auto margins are zero and suppress self alignment");
			parent.setProperty("grid-template-rows", "80px");
			style.setProperty("align-self", "center");
			style.setProperty("margin-top", "auto");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.y, 60, "grid top auto margin overrides align-self");
			style.setProperty("margin-bottom", "auto");
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.y, 30, "grid vertical auto margins share free space");
			style.height(100);
			tree.refresh(root, 200, 200);
			expectEqual(tree.node(child).layout.y, 0, "grid vertical overflow suppresses self alignment with auto margins");
		}
	}
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("display", "flex");
	NodeHandle(root).style().setProperty("flex-direction", "row");
	NodeHandle(root).style().width(100);
	const int child = makeDiv(root, nullptr);
	NodeHandle(child).style().width(20);
	NodeHandle(child).style().height(20);
	NodeHandle(child).style().setProperty("justify-self", "end");
	tree.mount(root, 200, 200);
	expectEqual(tree.node(child).layout.x, 0, "justify-self does not align flex items on their main axis");
}

void testAbsoluteOverflowAlignmentMutation()
{
	for (const char *display : {"flex", "grid"}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.width(100); style.height(80);
		style.setProperty("position", "relative");
		style.setProperty("display", display);
		style.setProperty("justify-content", "unsafe center");
		style.setProperty("justify-items", "unsafe center");
		const int child = makeDiv(root, nullptr);
		auto childStyle = NodeHandle(child).style();
		childStyle.setProperty("position", "absolute");
		childStyle.setProperty("align-self", "unsafe center");
		childStyle.width(140); childStyle.height(120);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(child).layout.x, -20, "absolute unsafe alignment permits horizontal overflow before start");
		expectEqual(tree.node(child).layout.y, -20, "absolute unsafe alignment permits vertical overflow before start");
		childStyle.setProperty("align-self", "safe center");
		childStyle.width(160);
		tree.refresh(root, 200, 200);
		expectEqual(tree.node(child).layout.x, -30, "retained absolute width mutation keeps unsafe horizontal alignment");
		expectEqual(tree.node(child).layout.y, 0, "retained absolute alignment mutation applies safe fallback");
		tree.computeLayout(root, 200, 200);
		expectEqual(tree.node(child).layout.x, -30, "full layout agrees with retained absolute horizontal alignment");
		expectEqual(tree.node(child).layout.y, 0, "full layout agrees with retained absolute vertical alignment");
	}
}

void testFloatingRootGeometry()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("float", "right");
	const int child = makeDiv(root, nullptr);
	NodeHandle(child).style().width(40);
	NodeHandle(child).style().height(20);
	NodeHandle(child).style().setProperty("margin", "8px");
	tree.mount(root, 100, 100);
	expectEqual(tree.node(root).layout.width, 56, "floating root uses shrink-to-fit content width");
	expectEqual(tree.node(root).layout.x, 44, "right-floating root aligns to initial containing block");
	expectEqual(tree.node(child).layout.x, 52, "floating root offset reaches descendants once");
	expectEqual(tree.node(child).layout.y, 8, "floating root contains child edge margins");
	NodeHandle(root).style().setProperty("float", "none");
	tree.refresh(root, 100, 100);
	expectEqual(tree.node(root).layout.width, 100, "clearing float restores normal root auto width");
	expectEqual(tree.node(root).layout.x, 0, "clearing float removes previous root position");
	expectEqual(tree.node(child).layout.x, 8, "descendant position is restored after root float mutation");
}

void testCssWhitespaceModes()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	auto &tree = Tree::instance();
	const int id = tree.createText();
	auto style = NodeHandle(id).style();
	style.setProperty("font-size", "16px");
	style.setProperty("line-height", "16px");
	struct Case { const char *mode; int value; const char *text; int available; int width; int height; };
	for (const auto &c : {
		Case{"normal", 0, "X \r\n\t  X", 100, 24, 16},
		Case{"nowrap", 1, "X \r\n\t  X", 8, 24, 16},
		Case{"pre", 2, "X  X\nX", 8, 32, 32},
		Case{"pre-line", 4, "X  X \r\n  X", 100, 24, 32},
		Case{"pre-wrap", 3, "X  X\nX", 100, 32, 32},
		Case{"break-spaces", 5, "X  X\nX", 100, 32, 32},
		Case{"normal", 0, "X X", 8, 8, 32},
	}) {
		style.setProperty("white-space", c.mode);
		NodeHandle(id).setText(c.text);
		TextRenderer::layout(id, c.available);
		expectEqual(tree.node(id).style.white_space, c.value, c.mode);
		expectEqual(tree.node(id).layout.width, c.width, c.mode);
		expectEqual(tree.node(id).layout.height, c.height, c.mode);
		expectTrue(tree.node(id).text == c.text, "white-space: authored text survives normalization");
	}
	// A style mutation must reprocess the original content, not a collapsed copy.
	NodeHandle(id).setText("X  X\nX");
	style.setProperty("white-space", "normal");
	TextRenderer::layout(id, 100);
	expectEqual(tree.node(id).layout.width, 40, "normal mutation: collapse spaces and newline");
	style.setProperty("white-space", "pre");
	TextRenderer::layout(id, 8);
	expectEqual(tree.node(id).layout.width, 32, "pre mutation: restore authored spaces");
	expectEqual(tree.node(id).layout.height, 32, "pre mutation: restore authored break");
}

void testAnonymousTextSemantics()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto text = [&](const char *value) {
		const int id = tree.createText();
		tree.setTagName(id, "#text");
		NodeHandle(id).setText(value);
		NodeHandle(root).appendChild(NodeHandle(id));
		return id;
	};
	text(" \n ");
	text("\t\n"); // Comments can separate adjacent DOM whitespace nodes.
	text("  ");
	const int first = makeSpan(root, "first");
	const int middle = text(" ");
	const int last = makeSpan(root, "last");
	text("\n");
	StyleSheet::instance().registerSelectorRule("*", "width", "30px");
	StyleSheet::instance().registerSelectorRule("*", "padding", "5px");
	StyleSheet::instance().registerSelectorRule("span:first-child", "height", "10px");
	StyleSheet::instance().registerSelectorRule("span:last-child", "height", "20px");
	NodeHandle(root).style().setProperty("font-size", "20px");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(middle).style.width, kUnset, "anonymous text: universal width does not apply");
	expectEqual(tree.node(middle).style.padding[0], 0, "anonymous text: universal padding does not apply");
	expectEqual(tree.node(middle).style.font_size, 20, "anonymous text: inherits font size");
	expectEqual(tree.node(first).style.height, 10, "anonymous text: first-child skips leading text");
	expectEqual(tree.node(last).style.height, 20, "anonymous text: last-child skips trailing text");
	int children[8];
	expectEqual(LayoutEngine::instance().collectChildren(root, children, 8, true), 3, "anonymous spaces: retain between inline elements only");
	NodeHandle(root).style().setProperty("display", "flex");
	tree.refresh(root, 300, 300);
	expectEqual(LayoutEngine::instance().collectChildren(root, children, 8, true), 2, "anonymous spaces: no flex whitespace items");
	NodeHandle(root).style().setProperty("display", "grid");
	tree.refresh(root, 300, 300);
	expectEqual(LayoutEngine::instance().collectChildren(root, children, 8, true), 2, "anonymous spaces: no grid whitespace items");
	NodeHandle(root).style().setProperty("display", "block");
	NodeHandle(first).style().setProperty("display", "block");
	tree.refresh(root, 300, 300);
	expectEqual(LayoutEngine::instance().collectChildren(root, children, 8, true), 2, "anonymous spaces: adjacent block boundary collapses space");
	NodeHandle(middle).setText("word");
	tree.refresh(root, 300, 300);
	expectEqual(LayoutEngine::instance().collectChildren(root, children, 8, true), 3, "anonymous text: content mutation restores text run");
	NodeHandle(root).style().setProperty("font-size", "24px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(middle).style.font_size, 24, "anonymous text: inherited font updates after mutation");
	const int label = tree.createText();
	NodeHandle(root).appendChild(NodeHandle(label));
	NodeHandle(label).setText("label");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(label).style.width, 30, "Gea TextElement remains an element selector target");
}

void testGridAutoTrackStretch()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto grid = NodeHandle(root).style();
	grid.setProperty("display", "grid");
	grid.setProperty("padding", "10px");
	grid.setProperty("border", "2px solid #000000");
	grid.width(100);
	grid.height(100);
	const int item = makeDiv(root, nullptr);
	auto child = NodeHandle(item).style();
	child.width(20);
	child.height(20);
	child.setProperty("align-self", "center");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(item).layout.y, 52, "grid auto row: align-self centers in stretched track");
	child.setProperty("align-self", "end");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.y, 92, "grid auto row: align-self end reaches content edge");
	grid.setProperty("align-content", "start");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.y, 12, "grid start: implicit track remains intrinsic");
	grid.setProperty("align-content", "center");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.y, 52, "grid center: positions track independently of align-self");
	grid.setProperty("align-content", "normal");
	grid.setProperty("grid-template-rows", "20px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.y, 12, "grid fixed row: normal does not stretch fixed tracks");
	grid.setProperty("grid-template-rows", "auto auto");
	grid.setProperty("row-gap", "10px");
	const int second = makeDiv(root, nullptr);
	NodeHandle(second).style().height(30);
	NodeHandle(second).style().setProperty("align-self", "end");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.y, 32, "grid auto rows: first receives equal free-space share");
	expectEqual(tree.node(second).layout.y, 82, "grid auto rows: second track includes gap and intrinsic height");
	grid.setProperty("grid-template-columns", "auto auto");
	grid.setProperty("grid-template-rows", "auto");
	grid.setProperty("column-gap", "10px");
	NodeHandle(second).style().width(30);
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(second).layout.x, 62, "grid auto columns: distribute free space equally");
	grid.setProperty("justify-content", "center");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(item).layout.x, 32, "grid centered columns: do not stretch auto tracks");
	expectEqual(tree.node(second).layout.x, 62, "grid centered columns: preserve intrinsic widths and gap");
	grid.setProperty("height", "auto");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(root).layout.height, 54, "grid auto height: intrinsic row plus padding and border");
	grid.setProperty("min-height", "100px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(root).layout.height, 124, "grid auto height: definite min-height supplies free space");
	expectEqual(tree.node(item).layout.y, 92, "grid min-height: stretches auto track to used minimum");
}

void testLeafBoxSizing()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	NodeHandle(root).style().height(300);
	const int text = tree.createText();
	NodeHandle(text).setText("test");
	NodeHandle(root).appendChild(NodeHandle(text));
	const int input = makeDiv(root, nullptr);
	tree.setTagName(input, "input");
	const int image = tree.createImage();
	NodeHandle(root).appendChild(NodeHandle(image));
	for (int id : {text, input, image}) {
		auto style = NodeHandle(id).style();
		style.width(100);
		style.height(20);
		style.setProperty("padding", "5px");
		style.setProperty("border", "2px solid #000000");
	}
	tree.mount(root, 300, 300);
	for (int id : {text, input, image}) {
		expectEqual(tree.node(id).layout.width, 114, "leaf content-box width includes edges");
		expectEqual(tree.node(id).layout.height, 34, "leaf content-box height includes edges");
		NodeHandle(id).style().setProperty("box-sizing", "border-box");
	}
	tree.refresh(root, 300, 300);
	for (int id : {text, input, image}) {
		expectEqual(tree.node(id).layout.width, 100, "leaf border-box width includes edges");
		expectEqual(tree.node(id).layout.height, 20, "leaf border-box height includes edges");
	}
}

void testGapAxesAndAutoMargins()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto style = NodeHandle(root).style();
	style.setProperty("display", "flex");
	style.setProperty("flex-flow", "row wrap");
	style.setProperty("align-content", "flex-start");
	style.setProperty("gap", "10px 20px");
	style.width(100);
	style.height(100);
	int children[3];
	for (int &child : children) {
		child = makeDiv(root, nullptr);
		NodeHandle(child).style().width(40);
		NodeHandle(child).style().height(20);
	}
	tree.mount(root, 300, 300);
	expectEqual(tree.node(children[1]).layout.x, 60, "column-gap separates row items");
	expectEqual(tree.node(children[2]).layout.y, 30, "row-gap separates flex lines");
	style.setProperty("writing-mode", "vertical-lr");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(children[1]).layout.y, 40, "vertical writing: column-gap follows inline axis");
	expectEqual(tree.node(children[2]).layout.y, 80, "vertical writing: three items fit inline axis");
	style.setProperty("writing-mode", "horizontal-tb");
	style.setProperty("flex-flow", "row nowrap");
	style.width(200);
	NodeHandle(children[0]).style().setProperty("margin-left", "auto");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(children[0]).layout.x, 40, "auto margin consumes free space before justification");
	NodeHandle(children[0]).style().setProperty("margin-left", "0px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(children[0]).layout.x, 0, "numeric margin clears auto state");
}

void testUniversalSelectorAndAutomaticMinimum()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 200, 1.0);
	auto &tree = Tree::instance();
	StyleSheet::instance().registerSelectorRule("*", "box-sizing", "border-box");
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("display", "flex");
	NodeHandle(root).style().width(40);
	NodeHandle(root).style().height(30);
	const int item = makeDiv(root, nullptr);
	NodeHandle(item).style().setProperty("flex-basis", "10px");
	const int content = makeDiv(item, nullptr);
	NodeHandle(content).style().width(80);
	NodeHandle(content).style().height(20);
	tree.mount(root, 200, 200);
	expectEqual(tree.node(content).style.box_sizing, 1, "universal selector matches descendants");
	expectEqual(tree.node(item).layout.width, 80, "automatic minimum floors flex basis at content size");
	NodeHandle(item).style().setProperty("min-width", "0px");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(item).layout.width, 10, "explicit zero minimum permits smaller flex basis");
}

void testGridGapAxes()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 200, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	auto style = NodeHandle(root).style();
	style.setProperty("display", "grid");
	style.setProperty("grid-template-columns", "40px 40px");
	style.setProperty("grid-template-rows", "20px 20px");
	style.setProperty("gap", "10px 20px");
	style.width(100);
	style.height(50);
	int children[3];
	for (int &child : children) child = makeDiv(root, nullptr);
	tree.mount(root, 200, 200);
	expectEqual(tree.node(children[1]).layout.x, 60, "grid: column gap separates tracks");
	expectEqual(tree.node(children[2]).layout.y, 30, "grid: row gap separates tracks");
}

void testSelfRelativeAlignment()
{
	for (bool absolute : {false, true}) {
		for (const char *parentMode : {"horizontal-tb", "vertical-rl", "vertical-lr"}) {
			for (const char *childMode : {"horizontal-tb", "vertical-rl", "vertical-lr"}) {
				for (bool rtl : {false, true}) for (bool end : {false, true}) {
					resetNativeHost(); StyleSheet::instance().clear();
					auto &tree = Tree::instance();
					const int root = makeDiv(-1, nullptr);
					auto container = NodeHandle(root).style();
					container.width(100); container.height(80); container.setProperty("display", "grid");
					container.setProperty("position", "relative"); container.setProperty("writing-mode", parentMode);
					container.setProperty("grid-template", "1fr / 1fr");
					const int child = makeDiv(root, "self-relative");
					StyleSheet::instance().registerRule("self-relative", "align-self", end ? "self-end" : "self-start");
					StyleSheet::instance().registerRule("self-relative", "justify-self", end ? "self-end" : "self-start");
					auto style = NodeHandle(child).style(); style.width(20); style.height(10);
					style.setProperty("writing-mode", childMode); style.setProperty("direction", rtl ? "rtl" : "ltr");
					if (absolute) style.setProperty("position", "absolute");
					tree.mount(root, 300, 300);
					const bool vertical = std::strcmp(childMode, "horizontal-tb") != 0;
					const bool reverseX = vertical ? std::strcmp(childMode, "vertical-rl") == 0 : rtl;
					const bool reverseY = vertical && rtl;
					expectEqual(tree.node(child).layout.x, reverseX != end ? 80 : 0, "grid self alignment follows subject's horizontal edge");
					expectEqual(tree.node(child).layout.y, reverseY != end ? 70 : 0, "grid self alignment follows subject's vertical edge");
					style.setProperty("align-self", "safe self-end"); style.setProperty("justify-self", "safe self-end");
					style.width(140); style.height(100); tree.refresh(root, 300, 300);
					expectEqual(tree.node(child).layout.x, std::strcmp(parentMode, "vertical-rl") == 0 ? -40 : 0,
					            "safe self alignment overflows toward the container's end");
					expectEqual(tree.node(child).layout.y, 0, "safe self alignment uses the container's vertical start");
				}
			}
		}
	}
	for (bool column : {false, true}) for (bool reverse : {false, true}) for (bool rtl : {false, true}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr);
		auto container = NodeHandle(root).style(); container.width(100); container.height(80);
		container.setProperty("display", "flex"); container.setProperty("flex-direction", column ? "column" : "row");
		container.setProperty("flex-wrap", reverse ? "wrap-reverse" : "nowrap");
		container.setProperty("align-items", "self-end");
		const int child = makeDiv(root, nullptr); auto style = NodeHandle(child).style();
		style.width(20); style.height(10); style.setProperty("direction", rtl ? "rtl" : "ltr");
		tree.mount(root, 300, 300);
		expectEqual(column ? tree.node(child).layout.x : tree.node(child).layout.y, column ? (rtl ? 0 : 80) : 70,
		            "flex self edge remains physical when wrap reverses the cross axis");
	}
	for (const char *value : {"left", "right"}) {
		for (const char *mode : {"horizontal-tb", "vertical-rl", "vertical-lr", "sideways-rl", "sideways-lr"}) {
			for (bool rtl : {false, true}) {
				resetNativeHost(); StyleSheet::instance().clear();
				auto &tree = Tree::instance(); const int root = makeDiv(-1, nullptr);
				auto container = NodeHandle(root).style(); container.width(100); container.height(80);
				container.setProperty("display", "grid"); container.setProperty("direction", rtl ? "rtl" : "ltr");
				container.setProperty("writing-mode", mode); container.setProperty("grid-template", "1fr / 1fr");
				const int child = makeDiv(root, nullptr); auto style = NodeHandle(child).style(); style.width(20); style.height(10);
				style.setProperty("justify-self", value); tree.mount(root, 300, 300);
				const bool vertical = std::strcmp(mode, "horizontal-tb") != 0;
				const bool atEnd = (std::strcmp(mode, "sideways-lr") == 0) != (std::strcmp(value, "right") == 0);
				expectEqual(vertical ? tree.node(child).layout.y : tree.node(child).layout.x, atEnd ? (vertical ? 70 : 80) : 0,
				            "justify-self left/right follow physical or line-relative edges independently of direction");
			}
		}
	}
}

void testForcedLineBreaks()
{
	for (const char *mode : {"normal", "nowrap", "pre", "pre-line"}) {
		for (int leading = 0; leading < 2; ++leading) {
			for (int breaks = 1; breaks <= 3; ++breaks) {
				resetNativeHost(); StyleSheet::instance().clear();
				auto &tree = Tree::instance();
				const int root = makeDiv(-1, nullptr);
				auto style = NodeHandle(root).style(); style.width(100);
				style.set(Property::FontId, 9101); style.set(Property::FontSize, 10);
				style.setProperty("line-height", "10px"); style.setProperty("white-space", mode);
				if (!leading) NodeHandle(root).appendChild(Document::instance().createText("X"));
				int lastBreak = -1;
				for (int i = 0; i < breaks; ++i) {
					auto br = Document::instance().createElement("br"); lastBreak = br.id(); NodeHandle(root).appendChild(br);
				}
				auto after = Document::instance().createText("X"); NodeHandle(root).appendChild(after);
				tree.mount(root, 200, 200);
				expectEqual(tree.node(after.id()).layout.y, breaks * 10, "BR creates forced and empty lines in every whitespace mode");
				expectEqual(tree.node(after.id()).layout.x, 0, "text after BR starts at inline start");
				expectEqual(tree.node(root).layout.height, (breaks + 1) * 10, "BR line boxes contribute to automatic block height");
				expectEqual(tree.node(lastBreak).layout.width, 0, "BR has zero inline advance");
				NodeHandle(lastBreak).style().setProperty("display", "none");
				tree.refresh(root, 200, 200);
				expectEqual(tree.node(after.id()).layout.y, (breaks - 1) * 10, "hiding a BR removes the forced break on refresh");
			}
		}
	}
	for (int count = 1; count <= 3; ++count) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style(); style.width(100);
		style.set(Property::FontId, 9101); style.set(Property::FontSize, 10); style.setProperty("line-height", "10px");
		NodeHandle(root).appendChild(Document::instance().createText("X"));
		for (int i = 0; i < count; ++i) NodeHandle(root).appendChild(Document::instance().createElement("br"));
		tree.mount(root, 200, 200);
		expectEqual(tree.node(root).layout.height, count * 10, "trailing BR does not manufacture a phantom line");
	}
}

void testBalancedFlex()
{
	// Independent exhaustive partition oracle for small cases. Checks optimum,
	// nonempty lines, oversized items, zero sizes, gaps and lexicographic ties.
	std::uint32_t random = 7321;
	for (int trial = 0; trial < 600; ++trial) {
		auto next = [&]() { random = random * 1664525u + 1013904223u; return random >> 8; };
		const int n = 1 + next() % 8, gap = next() % 5, available = next() % 31;
		const int minimum = 1 + next() % (n + 3);
		std::vector<int> sizes(n);
		for (int &size : sizes) size = static_cast<int>(next() % 41) - 5;
		int count = 1, used = std::max(0, sizes[0]);
		for (int i = 1; i < n; ++i) {
			const int item = std::max(0, sizes[i]);
			if (used + gap + item > available) { ++count; used = item; }
			else used += gap + item;
		}
		count = std::max(count, std::min(n, minimum));
		std::vector<int> best, candidate;
		std::int64_t bestCost = INT64_MAX;
		auto enumerate = [&](auto &&self, int start, int remaining, std::int64_t cost) -> void {
			if (!remaining) {
				if (start == n && (cost < bestCost || (cost == bestCost && candidate > best))) {
					best = candidate; bestCost = cost;
				}
				return;
			}
			int length = 0;
			for (int end = start + 1; end <= n - remaining + 1; ++end) {
				length += std::max(0, sizes[end - 1]) + (end > start + 1 ? gap : 0);
				if (length > available && end > start + 1) break;
				const std::int64_t error = available - length;
				candidate.push_back(end);
				self(self, end, remaining - 1, cost + error * error);
				candidate.pop_back();
			}
		};
		enumerate(enumerate, 0, count, 0);
		expectTrue(balancedFlexLineEnds(sizes, available, gap, minimum) == best, "balanced partition matches exhaustive optimum and start bias");
	}
	for (const char *wrap : {"balance", "wrap", "nowrap"}) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, "balanced");
		StyleSheet::instance().registerRule("balanced", "display", "flex");
		StyleSheet::instance().registerRule("balanced", "flex-direction", "column");
		StyleSheet::instance().registerRule("balanced", "flex-wrap", wrap);
		StyleSheet::instance().registerRule("balanced", "flex-line-count", "2");
		auto style = NodeHandle(root).style(); style.width(50); style.setProperty("gap", "10px");
		int children[3];
		for (int &child : children) {
			auto text = Document::instance().createText("XXXX XXXX"); child = text.id();
			NodeHandle(root).appendChild(text);
			text.style().set(Property::FontId, 9101); text.style().set(Property::FontSize, 10);
			text.style().setProperty("line-height", "10px");
		}
		tree.mount(root, 300, 300);
		expectEqual(rstyle(tree.node(root).style).flex_line_count, 2, "compiled class rule retains minimum line count");
		expectEqual(tree.node(children[0]).layout.height, std::strcmp(wrap, "nowrap") == 0 ? 10 : 20,
		            "line count restricts available cross space for text measurement only when wrapping");
	}
	{
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.setProperty("display", "flex"); style.setProperty("flex-flow", "row balance");
		style.width(100); style.setProperty("flex-line-count", "10000");
		int last = -1;
		for (int i = 0; i < 40; ++i) {
			last = makeDiv(root, nullptr); NodeHandle(last).style().width(1); NodeHandle(last).style().height(1);
		}
		tree.mount(root, 300, 300);
		expectEqual(tree.node(last).layout.y, 39, "minimum line count exceeds shared line storage without losing items");
		expectEqual(tree.node(root).layout.height, 40, "minimum line count is capped by item count");
	}
	for (int direction = 0; direction < 4; ++direction) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.setProperty("display", "flex");
		style.setProperty("flex-flow", direction & 1 ? "column balance" : "row balance");
		if (direction & 2) style.setProperty("flex-wrap", "balance wrap-reverse");
		style.setProperty("gap", "10px");
		if (direction & 1) style.height(100); else style.width(100);
		int children[4];
		for (int &child : children) { child = makeDiv(root, nullptr); NodeHandle(child).style().width(25); NodeHandle(child).style().height(25); }
		tree.mount(root, 200, 200);
		for (int i = 0; i < 4; ++i) {
			const auto &box = tree.node(children[i]).layout;
			const int main = i % 2 * 35;
			const int cross = ((direction & 2) ? 1 - i / 2 : i / 2) * 35;
			expectEqual(direction & 1 ? box.y : box.x, main, "balance puts two items on each line");
			expectEqual(direction & 1 ? box.x : box.y, cross, "balance honors cross-axis reversal");
		}
		style.setProperty("flex-line-count", "4");
		tree.refresh(root, 200, 200);
		expectEqual(direction & 1 ? tree.node(children[1]).layout.y : tree.node(children[1]).layout.x, 0, "line count mutation creates four nonempty lines");
		style.setProperty("flex-line-count", "0");
		style.setProperty("flex-wrap", "nowrap balance");
		expectEqual(rstyle(tree.node(root).style).flex_line_count, 4, "invalid line count preserves earlier declaration");
		expectEqual(tree.node(root).style.flex_wrap, direction & 2 ? 6 : 5, "invalid wrap combination preserves earlier declaration");
		style.removeProperty("flex-line-count");
		tree.refresh(root, 200, 200);
		expectEqual(rstyle(tree.node(root).style).flex_line_count, 1, "removing minimum line count restores initial value");
	}
	for (int column = 0; column < 2; ++column) {
		resetNativeHost(); StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.setProperty("display", "flex"); style.setProperty("flex-flow", column ? "column balance" : "row balance");
		style.setProperty("flex-line-count", "2"); style.setProperty("gap", "20px");
		style.width(column ? 100 : 220); if (!column) style.height(100);
		int children[4];
		for (int i = 0; i < (column ? 4 : 3); ++i) {
			children[i] = makeDiv(root, nullptr);
			auto child = NodeHandle(children[i]).style();
			child.setProperty(column ? "width" : "height", "60%"); child.setProperty("aspect-ratio", "1");
		}
		tree.mount(root, 300, 300);
		expectEqual(tree.node(children[0]).layout.width, 60, "balanced percentage ratio width uses container basis");
		expectEqual(tree.node(children[0]).layout.height, 60, "balanced percentage ratio height uses container basis");
		expectEqual(column ? tree.node(children[2]).layout.x : tree.node(children[2]).layout.y, 80, "minimum line count distributes percentage items");
	}
}

void testAspectRatio()
{
	{
		resetNativeHost();
		StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto container = NodeHandle(root).style();
		container.setProperty("display", "flex"); container.setProperty("flex-direction", "row"); container.width(100);
		const int child = makeDiv(root, nullptr);
		auto style = NodeHandle(child).style();
		style.setProperty("aspect-ratio", "1 / 2"); style.setProperty("max-height", "100px"); style.setProperty("flex", "1");
		tree.mount(root, 200, 200);
		expectEqual(tree.node(child).layout.width, 100, "flex grow determines width independently of ratio cross maximum");
		expectEqual(tree.node(child).layout.height, 100, "flex grow recomputes ratio height then applies cross maximum");
		expectEqual(tree.node(root).layout.height, 100, "flex line includes the ratio height after main-axis growth");
	}
	{
		resetNativeHost();
		StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().width(300);
		const int outer = makeDiv(root, nullptr), inner = makeDiv(outer, nullptr);
		auto style = NodeHandle(outer).style();
		style.width(50); style.setProperty("aspect-ratio", "5 / 3");
		NodeHandle(inner).style().setProperty("width", "100%");
		NodeHandle(inner).style().setProperty("height", "100%");
		tree.mount(root, 300, 300);
		expectEqual(tree.node(outer).layout.height, 30, "ratio creates a definite percentage height basis");
		expectEqual(tree.node(inner).layout.height, 30, "percentage descendant resolves against ratio-derived height");
		NodeHandle(inner).style().height(90);
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(outer).layout.height, 90, "automatic minimum accommodates content taller than preferred ratio");
		style.setProperty("overflow", "hidden");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(outer).layout.height, 30, "scrollable overflow disables ratio content minimum");
		style.setProperty("min-height", "40px");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(outer).layout.height, 40, "minimum height can override the preferred ratio");
		style.setProperty("min-height", "0"); style.setProperty("max-width", "25px");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(outer).layout.width, 25, "ratio respects determining-axis maximum width");
		expectEqual(tree.node(outer).layout.height, 15, "ratio transfers the constrained determining size");
	}
	for (bool borderBox : {false, true}) {
		for (bool horizontal : {false, true}) {
			resetNativeHost();
			StyleSheet::instance().clear();
			auto &tree = Tree::instance();
			const int root = makeDiv(-1, nullptr);
			NodeHandle(root).style().width(400);
			const int child = makeDiv(root, "ratio");
			auto style = NodeHandle(child).style();
			StyleSheet::instance().registerRule("ratio", "aspect-ratio", "2 / 1");
			style.setProperty("box-sizing", borderBox ? "border-box" : "content-box");
			style.setProperty("padding", "10px");
			style.setProperty("border", "2px solid #000000");
			style.setProperty(horizontal ? "height" : "width", "100px");
			tree.mount(root, 400, 400);
			expectEqual(tree.node(child).layout.width, horizontal ? (borderBox ? 200 : 224) : (borderBox ? 100 : 124), "ratio derives width in the selected sizing box");
			expectEqual(tree.node(child).layout.height, horizontal ? (borderBox ? 100 : 124) : (borderBox ? 50 : 74), "ratio derives height in the selected sizing box");
			style.setProperty("aspect-ratio", "1");
			tree.refresh(root, 400, 400);
			expectEqual(tree.node(child).layout.width, borderBox ? 100 : 124, "live ratio change updates width");
			expectEqual(tree.node(child).layout.height, borderBox ? 100 : 124, "live ratio change updates height");
			style.setProperty("aspect-ratio", "-2 / 1");
			tree.refresh(root, 400, 400);
			expectEqual(tree.node(child).layout.height, borderBox ? 100 : 124, "invalid negative ratio preserves prior inline value");
			style.removeProperty("aspect-ratio");
			tree.refresh(root, 400, 400);
			expectEqual(tree.node(child).layout.width, horizontal ? (borderBox ? 200 : 224) : (borderBox ? 100 : 124), "removing inline ratio restores cached class rule");
			style.setProperty("aspect-ratio", "auto");
			tree.refresh(root, 400, 400);
			expectEqual(tree.node(child).layout.height, horizontal ? (borderBox ? 100 : 124) : 24, "auto removes the preferred ratio");
		}
	}
	for (bool row : {false, true}) {
		for (int crossMode : {0, 1, 2}) {
			resetNativeHost();
			StyleSheet::instance().clear();
			auto &tree = Tree::instance();
			const int root = makeDiv(-1, nullptr);
			auto container = NodeHandle(root).style();
			container.setProperty("display", "flex"); container.setProperty("flex-direction", row ? "row" : "column");
			container.width(row ? 0 : 100); container.height(row ? 100 : 0);
			const int child = makeDiv(root, nullptr);
			auto style = NodeHandle(child).style();
			style.setProperty("aspect-ratio", "1");
			if (crossMode == 1) style.setProperty(row ? "height" : "width", "100%");
			if (crossMode == 2) { style.width(100); style.height(100); }
			tree.mount(root, 400, 400);
			expectEqual(tree.node(child).layout.width, 100, "ratio transferred minimum prevents flex width collapsing to zero");
			expectEqual(tree.node(child).layout.height, 100, "ratio transferred minimum prevents flex height collapsing to zero");
			style.setProperty(row ? "min-width" : "min-height", "0");
			tree.refresh(root, 400, 400);
			expectEqual(row ? tree.node(child).layout.width : tree.node(child).layout.height, 0, "explicit zero minimum permits flex shrink");
		}
	}
	{
		resetNativeHost();
		StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto container = NodeHandle(root).style();
		container.setProperty("display", "flex"); container.setProperty("flex-direction", "column"); container.width(500);
		const int child = makeDiv(root, nullptr);
		auto style = NodeHandle(child).style();
		style.width(200); style.setProperty("padding", "100px"); style.setProperty("aspect-ratio", "auto 1 / 1");
		style.setProperty("overflow", "hidden");
		tree.mount(root, 500, 600);
		expectEqual(tree.node(child).layout.width, 400, "ratio flex item preserves content-box width plus padding");
		expectEqual(tree.node(child).layout.height, 400, "ratio flex item transfers content width before adding vertical padding");
	}
}

void testBaselineAlignment()
{
	{
		resetNativeHost();
		StyleSheet::instance().clear();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		auto style = NodeHandle(root).style();
		style.setProperty("display", "flex"); style.setProperty("flex-direction", "row");
		style.setProperty("align-items", "baseline"); style.width(100);
		const int a = makeDiv(root, nullptr), b = makeDiv(root, nullptr);
		NodeHandle(a).style().width(20); NodeHandle(a).style().height(20);
		NodeHandle(a).style().setProperty("margin-bottom", "9px");
		NodeHandle(b).style().width(20); NodeHandle(b).style().height(30);
		tree.mount(root, 200, 200);
		expectEqual(tree.node(a).layout.y, 10, "empty flex item synthesizes baseline at border edge, excluding margin");
		expectEqual(tree.node(b).layout.y, 0, "empty flex item border baselines align");
		expectEqual(tree.node(root).layout.height, 39, "baseline descent includes bottom margin once");
	}
	for (const char *alignment : {"baseline", "first baseline", "baseline first", "last baseline", "baseline last"}) {
		for (bool explicitHeight : {false, true}) {
			for (bool reverseWrap : {false, true}) {
				resetNativeHost();
				StyleSheet::instance().clear();
				auto &tree = Tree::instance();
				const int root = makeDiv(-1, "baseline-container");
				NodeHandle(root).style().setProperty("display", "flex");
				NodeHandle(root).style().setProperty("flex-direction", "row");
				NodeHandle(root).style().setProperty("flex-wrap", reverseWrap ? "wrap-reverse" : "nowrap");
				NodeHandle(root).style().width(100);
				if (explicitHeight) NodeHandle(root).style().height(60);
				StyleSheet::instance().registerRule("baseline-container", "align-items", alignment);
				auto a = Document::instance().createText("X\nX");
				auto b = Document::instance().createText("X");
				NodeHandle(root).appendChild(a); NodeHandle(root).appendChild(b);
				for (auto child : {a, b}) {
					child.style().width(20);
					child.style().setProperty("white-space", "pre");
					child.style().setProperty("display", "block");
				}
				a.style().set(Property::FontId, 9101); a.style().set(Property::FontSize, 10); a.style().height(30);
				b.style().set(Property::FontId, 9102); b.style().set(Property::FontSize, 20); b.style().height(20);
				tree.mount(root, 200, 200);
				const bool last = std::strstr(alignment, "last") != nullptr;
				expectEqual(tree.node(root).style.align_items, last ? kAlignLastBaseline : 5, "baseline parser preserves first/last distinction");
				expectEqual(TextRenderer::baselineOffset(tree.node(a.id()), last), last ? 16 : 6, "multiline text uses requested baseline");
				expectEqual(tree.node(root).layout.height, explicitHeight ? 60 : last ? 32 : 42, "baseline group contributes ascent plus descent to line height");
				expectEqual(tree.node(a.id()).layout.y, last ? (explicitHeight ? 30 : 2) : 12, "first item aligns its text baseline");
				expectEqual(tree.node(b.id()).layout.y, last && explicitHeight ? 28 : 0, "second item aligns its text baseline");
			}
		}
	}
	for (bool positioned : {false, true}) {
		for (const char *mode : {"ltr", "rtl", "vertical-rl"}) {
			for (bool small : {false, true}) {
				for (bool ownWritingMode : {false, true}) {
					resetNativeHost();
					StyleSheet::instance().clear();
					auto &tree = Tree::instance();
					const int root = makeDiv(-1, nullptr);
					NodeHandle(root).style().width(300);
					const int grid = makeDiv(root, nullptr);
					auto style = NodeHandle(grid).style();
					style.setProperty("display", "grid");
					style.width(small ? 2 : 40); style.height(small ? 2 : 40);
					style.setProperty("padding", "2px"); style.setProperty("border", "1px solid #000000");
					style.setProperty("grid-template-columns", small ? "0px 2px 0px" : "2px 20px 2px");
					style.setProperty("grid-template-rows", small ? "0px 2px 0px" : "2px 20px 2px");
					if (positioned) style.setProperty("position", "relative");
					style.setProperty(std::strcmp(mode, "vertical-rl") == 0 ? "writing-mode" : "direction", mode);
					const int child = makeDiv(grid, "baseline-child");
					auto childStyle = NodeHandle(child).style();
					childStyle.setProperty("position", "absolute"); childStyle.width(8); childStyle.height(6);
					if (ownWritingMode) {
						childStyle.setProperty("direction", "ltr");
						childStyle.setProperty("writing-mode", "horizontal-tb");
					}
					childStyle.setProperty("grid-area", "2 / 2 / 3 / 3");
					StyleSheet::instance().registerRule("baseline-child", "align-self", "last baseline");
					StyleSheet::instance().registerRule("baseline-child", "justify-self", "baseline last");
					tree.mount(root, 300, 200);
					const bool reversed = std::strcmp(mode, "ltr") != 0;
					const bool endAtLeft = reversed && !ownWritingMode;
					const int areaLeft = positioned && !small ? (reversed ? 21 : 5) : 3;
					const int areaWidth = small ? 2 : positioned ? 20 : 40;
					const int x = areaLeft + (endAtLeft ? 0 : areaWidth - 8);
					const int y = small ? -1 : positioned ? 19 : 37;
					expectEqual(tree.node(child).layout.x - tree.node(grid).layout.x, x, "absolute baseline fallback uses logical horizontal end without overflow clamp");
					expectEqual(tree.node(child).layout.y - tree.node(grid).layout.y, y, "absolute baseline fallback uses logical vertical end without overflow clamp");
				}
			}
		}
	}
}

void testBorderWidthSnapping()
{
	for (double dpr : {1.0, 2.0}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setViewportMetrics(200, 100, dpr);
		auto &sheet = StyleSheet::instance();
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		struct Case { const char *css; int at1; int at2; };
		for (const Case &test : {
			Case{"0px", 0, 0}, {"0.1px", 1, 1}, {"0.9px", 1, 1},
			{"1px", 1, 2}, {"1.9px", 1, 3}, {"2.1px", 2, 4},
			{"calc(1.4px + 0.6px)", 2, 4}, {"calc(0.6px + 0.6px)", 1, 2}, {"calc(1.9px * 2)", 3, 7},
			{"calc(3.8px / 2)", 1, 3}, {"calc(2.4px - 0.5px)", 1, 3},
			{"min(2.5px, 1.9px)", 1, 3}, {"max(0.1px, 1.9px)", 1, 3},
			{"clamp(0.5px, 1.9px, 3px)", 1, 3}, {"calc(1px - 3px)", 0, 0},
			{"0.95vw", 1, 1}, {"1.9vh", 1, 1}, {"0.19em", 1, 3},
			{"var(--fraction)", 1, 3}, {"var(--missing, 1.9px)", 1, 3},
		}) {
			const int expected = dpr == 1.0 ? test.at1 : test.at2;
			sheet.registerRule("fraction", "--fraction", "calc(1.4px + 0.5px)");
			sheet.registerRule("fraction", "font-size", "10px");
			sheet.registerRule("fraction", "border-width", test.css);
			sheet.registerRule("fraction-border", "--fraction", "calc(1.4px + 0.5px)");
			sheet.registerRule("fraction-border", "font-size", "10px");
			sheet.registerRule("fraction-border", "border", std::string(test.css) + " solid #000000");
			for (const char *side : {"border-top", "border-right", "border-bottom", "border-left"})
				sheet.registerRule("fraction-border", side, std::string(test.css) + " solid #000000");

			// Repeat to exercise cached class applications as well as first parse.
			for (int repeat = 0; repeat < 2; ++repeat) {
				const int cached = makeDiv(root, "fraction-border");
				expectEqual(tree.node(cached).style.border_width, expected, "cached border shorthand snapping");
				for (int side = 0; side < 4; ++side)
					expectEqual(rstyle(tree.node(cached).style).border_side_width[side], expected, "cached side shorthand snapping");
				const int box = makeDiv(root, "fraction");
				expectEqual(tree.node(box).style.border_width, expected, test.css);
				NodeHandle(box).classList().set("");
				NodeHandle(box).style().setProperty("font-size", "10px");
				NodeHandle(box).style().setProperty("--fraction", "calc(1.4px + 0.5px)");
				for (const char *property : {"border-width", "border"}) {
					const std::string value = std::string(test.css) + (std::strcmp(property, "border") == 0 ? " solid #000000" : "");
					NodeHandle(box).style().setProperty(property, value);
					expectEqual(tree.node(box).style.border_width, expected, test.css);
				}
				int side = 0;
				for (const char *property : {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"}) {
					NodeHandle(box).style().setProperty(property, test.css);
					expectEqual(rstyle(tree.node(box).style).border_side_width[side++], expected, property);
				}
			}
		}
		const int box = makeDiv(root, nullptr);
		for (StyleDeclaration declaration : {StyleDeclaration::BorderWidth, StyleDeclaration::BorderTopWidth,
		     StyleDeclaration::BorderRightWidth, StyleDeclaration::BorderBottomWidth, StyleDeclaration::BorderLeftWidth}) {
			sheet.applyNumberProperty(NodeHandle(box), declaration, 1.9);
		}
		expectEqual(tree.node(box).style.border_width, 1, "numeric border width snaps raw device pixels");
		for (int side = 0; side < 4; ++side)
			expectEqual(rstyle(tree.node(box).style).border_side_width[side], 1, "numeric border side snapping");
		sheet.applyPixelLengthProperty(NodeHandle(box), StyleDeclaration::BorderWidth, 1.9);
		expectEqual(tree.node(box).style.border_width, dpr == 1 ? 1 : 3, "compiled CSS px width scales before snapping");
		NodeHandle(box).style().setProperty("border-width", "2.1px");
		NodeHandle(box).style().setProperty("border-width", "-1px");
		NodeHandle(box).style().setProperty("border-width", "-0.1em");
		NodeHandle(box).style().setProperty("border", "-0.5ch solid #ffffff");
		sheet.applyNumberProperty(NodeHandle(box), StyleDeclaration::BorderWidth, -1.0);
		expectEqual(tree.node(box).style.border_width, dpr == 1 ? 2 : 4, "negative border literals ignored");
		// Ordinary lengths round once after arithmetic, rather than snapping as borders.
		NodeHandle(box).style().setProperty("width", "calc(0.6px + 0.6px)");
		expectEqual(tree.node(box).style.width, dpr == 1 ? 1 : 2, "calc preserves fractional operands");
	}
	resetNativeHost();
	StyleSheet::instance().clear();
	setNativeDisplaySize(80, 80);
	setViewportMetrics(80, 80, 1.0);
	const int root = makeDiv(-1, nullptr);
	const int box = makeDiv(root, nullptr);
	NodeHandle(root).style().setProperty("background", "#ffffff");
	auto style = NodeHandle(box).style();
	style.setProperty("width", "20px");
	style.setProperty("height", "20px");
	style.setProperty("background", "#ff0000");
	style.setProperty("border", "1.9px solid #000000");
	Tree::instance().mount(root, 80, 80);
	expectEqual(Tree::instance().node(box).layout.width, 22, "snapped border contributes to outer width");
	expectEqual(displayPixelAt(0, 10), 0, "one-pixel black left border");
	expectEqual(displayPixelAt(1, 10), nativeRgb565(0xf800), "content begins after one border pixel");
	expectEqual(displayPixelAt(21, 10), 0, "one-pixel black right border");
	style.setProperty("border-width", "2.1px");
	Tree::instance().refresh(root, 80, 80);
	expectEqual(Tree::instance().node(box).layout.width, 24, "border mutation updates outer width");
	expectEqual(displayPixelAt(1, 10), 0, "border mutation repaints second pixel");
	expectEqual(displayPixelAt(2, 10), nativeRgb565(0xf800), "border mutation shifts content paint");
}

void testBorderWidthShorthandPixels()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setNativeDisplaySize(80, 80);
	setViewportMetrics(80, 80, 1.0);
	auto &tree = Tree::instance();
	auto &sheet = StyleSheet::instance();
	sheet.registerRule("sides", "border", "solid #0000ff");
	sheet.registerRule("sides", "border-width", "1.9px 2.9px 3.9px 4.9px");
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("background", "#ffffff");
	const int box = makeDiv(root, "sides");
	auto style = NodeHandle(box).style();
	style.setProperty("width", "20px");
	style.setProperty("height", "20px");
	style.setProperty("background", "#ff0000");
	tree.mount(root, 80, 80);
	auto checkAsymmetric = [&]() {
		expectEqual(tree.node(box).layout.width, 26, "four border widths add horizontal edges");
		expectEqual(tree.node(box).layout.height, 24, "four border widths add vertical edges");
		for (const auto &point : {std::pair<int,int>{3,10}, {24,10}, {10,0}, {10,21}})
			expectEqual(displayPixelAt(point.first, point.second), nativeRgb565(0x001f), "asymmetric edges use common blue border color");
		for (const auto &point : {std::pair<int,int>{4,10}, {23,10}, {10,1}, {10,20}})
			expectEqual(displayPixelAt(point.first, point.second), nativeRgb565(0xf800), "content begins at each snapped edge");
	};
	checkAsymmetric();
	style.setProperty("border-width", "2.1px");
	tree.refresh(root, 80, 80);
	expectEqual(tree.node(box).layout.width, 24, "uniform shorthand replaces side widths");
	expectEqual(tree.node(box).layout.height, 24, "uniform shorthand replaces top and bottom");
	style.removeProperty("border-width");
	tree.refresh(root, 80, 80);
	checkAsymmetric();
	style.setProperty("border-width", "5.9px 2.9px");
	tree.refresh(root, 80, 80);
	expectEqual(tree.node(box).layout.width, 24, "two border widths repeat horizontally");
	expectEqual(tree.node(box).layout.height, 30, "two border widths repeat vertically");
	style.setProperty("border-width", "1.9px 2.9px 3.9px");
	tree.refresh(root, 80, 80);
	expectEqual(tree.node(box).layout.width, 24, "three border widths repeat the second value");
	expectEqual(tree.node(box).layout.height, 24, "three border widths retain distinct vertical sides");
	style.setProperty("border-right-color", "#00ff00");
	tree.refresh(root, 80, 80);
	expectEqual(displayPixelAt(23,10), nativeRgb565(0x07e0), "explicit side color overrides common color");
	expectEqual(displayPixelAt(0,10), nativeRgb565(0x001f), "other sides retain common color");
	style.removeProperty("border-right-color");
	tree.refresh(root, 80, 80);
	expectEqual(displayPixelAt(23,10), nativeRgb565(0x001f), "removing side color restores common color");
	style.setProperty("border-color", "rgba(0,0,255,0.5)");
	tree.refresh(root, 80, 80);
	expectEqual(displayPixelAt(0,0), displayPixelAt(10,0), "translucent border corners paint exactly once");
	expectTrue(displayPixelAt(0,0) != nativeRgb565(0xf800), "translucent border has visible paint");

	style.setProperty("border", "transparent solid 1.9px");
	tree.refresh(root, 80, 80);
	expectEqual(tree.node(box).layout.width, 22, "reordered transparent border retains snapped width");
	expectEqual(tree.node(box).style.border_alpha, 0, "transparent shorthand retains zero alpha");
	expectEqual(displayPixelAt(0,10), nativeRgb565(0xf800), "transparent border reveals box background");

}

void testBorderCurrentColor()
{
	// Check pixels against literal colors, including after retained-display updates.
	// A test/reference pair using currentColor on both sides could hide missing paint.
	for (bool cached : {false, true}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setNativeDisplaySize(80, 80);
		setViewportMetrics(80, 80, 1.0);
		auto &tree = Tree::instance();
		auto &sheet = StyleSheet::instance();
		sheet.registerRule("green", "color", "#00ff00");
		sheet.registerRule("blue", "color", "#0000ff");
		sheet.registerRule("half-blue", "color", "rgba(0,0,255,0.5)");
		sheet.registerRule("literal-blue", "border-color", "#0000ff");
		sheet.registerRule("border-current", "border", "4px solid");
		sheet.registerRule("border-explicit-current", "border", "4px solid currentColor");
		const int root = makeDiv(-1, "green");
		NodeHandle(root).style().setProperty("background", "#ffffff");
		const int box = makeDiv(root, cached ? "border-current" : nullptr);
		auto style = NodeHandle(box).style();
		style.setProperty("width", "20px");
		style.setProperty("height", "20px");
		if (!cached) style.setProperty("border", "4px solid");
		tree.mount(root, 80, 80);
		auto refresh = [&]() { tree.refresh(root, 80, 80); };
		auto edges = [&](int top, int right, int bottom, int left, const char *label) {
			expectEqual(displayPixelAt(10, 1), nativeRgb565(top), label);
			expectEqual(displayPixelAt(26, 10), nativeRgb565(right), label);
			expectEqual(displayPixelAt(10, 26), nativeRgb565(bottom), label);
			expectEqual(displayPixelAt(1, 10), nativeRgb565(left), label);
			expectEqual(tree.node(box).layout.width, 28, "color changes preserve geometry");
		};
		edges(0x07e0, 0x07e0, 0x07e0, 0x07e0, "omitted border color inherits green text color");
		NodeHandle(root).classList().set("blue");
		refresh();
		edges(0x001f, 0x001f, 0x001f, 0x001f, "inherited class color repaints currentColor borders");
		style.setProperty("color", "#ff0000");
		refresh();
		edges(0xf800, 0xf800, 0xf800, 0xf800, "inline text color repaints currentColor borders");
		style.setProperty("border-color", "#ff0000");
		style.setProperty("color", "#00ff00");
		refresh();
		edges(0xf800, 0xf800, 0xf800, 0xf800, "literal border matching old text color stays literal");
		style.setProperty("border-right-color", "currentColor");
		refresh();
		edges(0xf800, 0x07e0, 0xf800, 0xf800, "side currentColor overrides uniform literal color");
		style.setProperty("border-color", "#ff0000");
		refresh();
		edges(0xf800, 0xf800, 0xf800, 0xf800, "same common literal resets side binding");
		style.setProperty("border-color", "currentColor");
		refresh();
		edges(0x07e0, 0x07e0, 0x07e0, 0x07e0, "explicit currentColor resets literal binding");
		style.setProperty("border-left-color", "#0000ff");
		style.setProperty("border-top", "4px solid #ff0000");
		refresh();
		edges(0xf800, 0x07e0, 0x07e0, 0x001f, "literal side overrides remain independent");
		style.setProperty("border-top", "4px solid");
		refresh();
		edges(0x07e0, 0x07e0, 0x07e0, 0x001f, "omitted side shorthand color resets to currentColor");
		style.removeProperty("border-left-color");
		refresh();
		edges(0x07e0, 0x07e0, 0x07e0, 0x07e0, "removing side color restores currentColor");
		style.setProperty("border", "4px solid #ff0000");
		style.setProperty("border", "4px solid currentColor");
		style.removeProperty("color");
		refresh();
		edges(0x001f, 0x001f, 0x001f, 0x001f, "explicit shorthand currentColor follows restored inheritance");
		style.setProperty("color", "rgba(0,0,255,0.5)");
		refresh();
		const auto halfBlue = displayPixelAt(10, 1);
		expectTrue(halfBlue != nativeRgb565(0x001f) && halfBlue != nativeRgb565(0xffff), "currentColor preserves text alpha");
		style.setProperty("border-color", "rgba(0,0,255,0.5)");
		refresh();
		expectEqual(displayPixelAt(10, 1), halfBlue, "currentColor alpha matches literal rgba compositing");
		style.setProperty("border-right-color", "currentColor");
		style.setProperty("color", "rgba(255,0,0,0.5)");
		refresh();
		expectEqual(displayPixelAt(0, 0), displayPixelAt(10, 0), "mixed alpha border corners composite once");
		expectTrue(displayPixelAt(26, 10) != halfBlue, "alpha side follows later text color change");
		style.removeProperty("border");
		style.removeProperty("color");
		NodeHandle(box).classList().set("border-explicit-current");
		refresh();
		edges(0x001f, 0x001f, 0x001f, 0x001f, "removal restores explicit cached currentColor shorthand");
		NodeHandle(root).classList().set("half-blue");
		refresh();
		expectEqual(displayPixelAt(10, 1), halfBlue, "class alpha-only change propagates to inherited borders");
		NodeHandle(root).style().setProperty("color", "rgba(0,0,255,0.5)");
		NodeHandle(root).classList().set("green");
		refresh();
		expectEqual(displayPixelAt(10, 1), halfBlue, "equal-valued inline color retains alpha across class changes");
		NodeHandle(box).classList().set("border-current literal-blue");
		style.setProperty("border-color", "#0000ff");
		NodeHandle(box).classList().set("border-current");
		refresh();
		edges(0x001f, 0x001f, 0x001f, 0x001f, "equal-valued inline border survives removing class literal");
		style.setProperty("border-color", "rgba(0,0,255,0.5)");
		style.setProperty("border-right-color", "rgba(255,0,0,0.5)");
		refresh();
		const auto halfRed = displayPixelAt(26, 10);
		sheet.recomputeSubtree(root);
		refresh();
		expectEqual(displayPixelAt(10, 1), halfBlue, "literal border alpha survives cascade replay");
		expectEqual(displayPixelAt(26, 10), halfRed, "literal side alpha survives cascade replay");
	}
}

void testSharpInsetShadow()
{
	for (bool cached : {false, true}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setNativeDisplaySize(80, 80);
		setViewportMetrics(80, 80, 1.0);
		auto &tree = Tree::instance();
		auto &sheet = StyleSheet::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().setProperty("background", "#ffffff");
		NodeHandle(root).style().width(80);
		NodeHandle(root).style().height(80);
		const int box = makeDiv(root, "shadow-case");
		auto style = NodeHandle(box).style();
		style.setProperty("position", "absolute");
		style.setProperty("left", "10px");
		style.setProperty("top", "12px");
		style.setProperty("width", "20px");
		style.setProperty("height", "16px");
		style.setProperty("padding", "2px");
		style.setProperty("border", "solid transparent");
		style.setProperty("border-width", "2px 3px 4px 5px");
		tree.mount(root, 80, 80);
		struct Case { int spread, x, y, alpha; };
		for (const auto c : {Case{0, 0, 0, 255}, {3, 0, 0, 255}, {3, 0, 0, 128},
		     {0, 7, 5, 255}, {0, -7, -5, 255}, {2, 7, -5, 128},
		     {-3, 0, 0, 255}, {-3, 7, -5, 128}, {99, 0, 0, 255}, {0, 40, 0, 128}}) {
			char value[128];
			std::snprintf(value, sizeof(value), "inset %dpx %dpx 0 %dpx rgba(0,0,0,%s)", c.x, c.y, c.spread, c.alpha == 255 ? "1" : "0.5");
			if (cached) sheet.registerRule("shadow-case", "box-shadow", value);
			else style.setProperty("box-shadow", value);
			tree.refresh(root, 80, 80);
			int different = 0;
			// Padding edge: [15,39) × [14,34). The shadow is the part of
			// this rectangle outside its translated, spread-adjusted hole.
			const int hx = 15 + c.spread + c.x, hy = 14 + c.spread + c.y;
			const int hw = std::max(0, 24 - 2 * c.spread), hh = std::max(0, 20 - 2 * c.spread);
			const int white = nativeRgb565(0xffff);
			const int shadow = static_cast<int>(gea::framework::graphics::pixel::blendNative(
			    gea::framework::graphics::pixel::fromRgb565(0), gea::framework::graphics::pixel::fromRgb565(0xffff), c.alpha));
			for (int y = 0; y < 80; ++y) for (int x = 0; x < 80; ++x) {
				const bool clip = x >= 15 && x < 39 && y >= 14 && y < 34;
				const bool hole = x >= hx && x < hx + hw && y >= hy && y < hy + hh;
				if (displayPixelAt(x, y) != (clip && !hole ? shadow : white)) ++different;
			}
			expectEqual(different, 0, value);
			expectEqual(tree.node(box).layout.width, 32, "inset shadow does not change layout width");
			expectEqual(tree.node(box).layout.height, 26, "inset shadow does not change layout height");
		}
		style.setProperty("border", "5px solid transparent");
		style.setProperty("padding", "0");
		style.setProperty("width", "40px");
		style.setProperty("height", "40px");
		style.setProperty("border-radius", "20px");
		style.setProperty("box-shadow", "inset 0 0 0 5px #000000");
		tree.refresh(root, 80, 80);
		expectEqual(displayPixelAt(30, 12), nativeRgb565(0xffff), "shadow excludes transparent border area");
		expectEqual(displayPixelAt(15, 17), nativeRgb565(0xffff), "shadow clips to rounded padding corner");
		expectEqual(displayPixelAt(30, 18), nativeRgb565(0), "shadow starts at padding edge");
		expectEqual(displayPixelAt(22, 24), nativeRgb565(0), "rounded inner hole retains corner shadow");
		expectEqual(displayPixelAt(30, 32), nativeRgb565(0xffff), "shadow hole remains clear");
		style.setProperty("box-shadow", "inset 0 0 0 5px rgba(0,0,0,0.5)");
		tree.refresh(root, 80, 80);
		expectEqual(displayPixelAt(22, 24), displayPixelAt(30, 18), "rounded translucent corner composites once");
		// A percentage radius on a non-square box is an ellipse. The border
		// thickness is subtracted independently from its horizontal/vertical radii.
		style.setProperty("height", "20px");
		style.setProperty("border-radius", "50%");
		style.setProperty("box-shadow", "inset 0 0 0 50px #000000");
		tree.refresh(root, 80, 80);
		expectEqual(displayPixelAt(15, 17), nativeRgb565(0xffff), "ellipse shadow excludes padding corner");
		expectEqual(displayPixelAt(35, 18), nativeRgb565(0), "ellipse shadow reaches top center");
		expectEqual(displayPixelAt(16, 27), nativeRgb565(0), "ellipse shadow reaches left center");
		expectEqual(displayPixelAt(35, 27), nativeRgb565(0), "collapsed ellipse hole fills padding box");
		style.setProperty("box-shadow", "none");
		tree.refresh(root, 80, 80);
		expectEqual(displayPixelAt(35, 27), nativeRgb565(0xffff), "removing shadow clears retained pixels");
		style.setProperty("border-radius", "0");
		style.setProperty("box-shadow", "inset 0 0 0 50px #000000");
		style.setProperty("transform", "translate(5px, 3px)");
		tree.refresh(root, 80, 80);
		int transformedDifferences = 0;
		for (int y = 0; y < 80; ++y) for (int x = 0; x < 80; ++x) {
			const bool inside = x >= 20 && x < 60 && y >= 20 && y < 40;
			if (displayPixelAt(x, y) != nativeRgb565(inside ? 0 : 0xffff)) ++transformedDifferences;
		}
		expectEqual(transformedDifferences, 0, "transformed shadow uses the same padding contour");
	}
}

void testAlignmentShorthands()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(500, 500, 1.0);
	auto &tree = Tree::instance();
	auto &sheet = StyleSheet::instance();
	sheet.registerRule("placed", "place-content", "end space-evenly");
	sheet.registerRule("placed", "place-items", "center");
	const int root = makeDiv(-1, nullptr);
	const int grid = makeDiv(root, "placed");
	auto style = NodeHandle(grid).style();
	style.setProperty("display", "grid");
	style.setProperty("grid", "200px / 200px");
	style.width(400); style.height(400);
	const int child = makeDiv(grid, nullptr);
	auto item = NodeHandle(child).style();
	item.width(40); item.height(20);
	tree.mount(root, 500, 500);
	auto check = [&](int x, int y, const char *label) {
		tree.refresh(root, 500, 500);
		expectEqual(tree.node(child).layout.x - tree.node(grid).layout.x, x, label);
		expectEqual(tree.node(child).layout.y - tree.node(grid).layout.y, y, label);
	};
	check(180, 290, "cached place-items preserves place-content track distribution");
	style.setProperty("place-items", "end center");
	check(180, 380, "place-items accepts independent axis values");
	style.setProperty("place-items", "safe center unsafe end");
	check(260, 290, "place-items keeps overflow modifiers attached to each axis");
	style.setProperty("place-items", "center bogus");
	check(260, 290, "invalid second shorthand component does not partially apply");
	style.removeProperty("place-items");
	check(180, 290, "removing place-items restores cached rule without changing content alignment");
	item.setProperty("place-self", "end start");
	check(100, 380, "place-self overrides each inherited item-alignment default");
	item.setProperty("place-self", "auto center");
	check(180, 290, "place-self auto retains container alignment on that axis");
	item.removeProperty("place-self");
	check(180, 290, "removing place-self restores both item defaults");
	style.setProperty("place-content", "center start");
	check(80, 190, "place-content mutates track positions on both axes");
	style.removeProperty("place-content");
	check(180, 290, "removing place-content restores both cached longhands");

	for (const char *property : {"place-items", "place-content", "place-self"}) {
		const int target = std::strcmp(property, "place-self") == 0 ? child : grid;
		auto targetStyle = NodeHandle(target).style();
		targetStyle.setProperty(property, "first baseline");
		const auto &computed = tree.node(target).style;
		const int first = std::strcmp(property, "place-self") == 0 ? computed.align_self : std::strcmp(property, "place-content") == 0 ? computed.align_content : computed.align_items;
		const int second = std::strcmp(property, "place-self") == 0 ? rstyle(computed).justify_self : std::strcmp(property, "place-content") == 0 ? computed.justify_content : computed.justify_items;
		expectEqual(first, 5, "baseline pair parses as one shorthand component");
		expectEqual(second, std::strcmp(property, "place-content") == 0 ? kAlignStart : 5, "omitted second component uses the shorthand-specific baseline fallback");
		targetStyle.setProperty(property, "last baseline safe center");
		for (const char *invalid : {"safe baseline", "center banana", "center start end", "safe center unsafe", "initial center"})
			targetStyle.setProperty(property, invalid);
		const auto &after = tree.node(target).style;
		expectEqual(std::strcmp(property, "place-self") == 0 ? after.align_self : std::strcmp(property, "place-content") == 0 ? after.align_content : after.align_items,
		            kAlignLastBaseline, "invalid shorthand preserves the previous first component");
		expectEqual(std::strcmp(property, "place-self") == 0 ? rstyle(after).justify_self : std::strcmp(property, "place-content") == 0 ? after.justify_content : after.justify_items,
		            1 | kAlignSafe, "invalid shorthand preserves the previous second component");
		targetStyle.setProperty(property, "unset");
		expectEqual(std::strcmp(property, "place-self") == 0 ? tree.node(target).style.align_self : std::strcmp(property, "place-content") == 0 ? tree.node(target).style.align_content : tree.node(target).style.align_items,
		            std::strcmp(property, "place-self") == 0 ? -1 : 0, "unset restores shorthand initial values");
	}
	sheet.registerRule("variable-place", "place-content", "var(--placement)");
	style.setProperty("--placement", "end space-evenly");
	style.removeProperty("place-content");
	style.removeProperty("place-items");
	NodeHandle(grid).classList().set("placed variable-place");
	item.removeProperty("place-self");
	check(180, 290, "variable shorthand uses the same parser as compiled rules");
	style.setProperty("--placement", "start end");
	check(280, 90, "variable shorthand updates both used axes");
}

void testDistributedAlignment()
{
	for (const char *display : {"grid", "flex"}) for (const char *distribution : {"space-between", "space-around", "space-evenly"}) {
		resetNativeHost();
		StyleSheet::instance().clear();
		setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		const int container = makeDiv(root, nullptr);
		auto style = NodeHandle(container).style();
		style.setProperty("display", display);
		style.setProperty("flex-direction", "row");
		style.width(140); style.height(140);
		style.setProperty("column-gap", "10px");
		style.setProperty("grid-template-columns", "20px 20px 20px");
		style.setProperty("justify-content", distribution);
		int children[3];
		for (int &child : children) {
			child = makeDiv(container, nullptr);
			NodeHandle(child).style().width(20); NodeHandle(child).style().height(20);
			NodeHandle(child).style().setProperty("flex-shrink", "0");
		}
		tree.mount(root, 300, 300);
		const int first = std::strcmp(distribution, "space-between") == 0 ? 0 : std::strcmp(distribution, "space-around") == 0 ? 10 : 15;
		const int last = 120 - first;
		expectEqual(tree.node(children[0]).layout.x, first, "distributed leading space");
		expectEqual(tree.node(children[1]).layout.x, 60, "distributed middle item includes authored gap");
		expectEqual(tree.node(children[2]).layout.x, last, "distributed trailing space");
		style.setProperty("direction", "rtl");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(children[0]).layout.x - tree.node(container).layout.x, last, "RTL mirrors distributed first item");
		expectEqual(tree.node(children[2]).layout.x - tree.node(container).layout.x, first, "RTL mirrors distributed last item");
		style.setProperty("direction", "ltr");
		style.setProperty("grid-template-columns", "60px 60px 60px");
		for (int child : children) NodeHandle(child).style().width(60);
		tree.refresh(root, 300, 300);
		for (int i = 0; i < 3; ++i) expectEqual(tree.node(children[i]).layout.x, i * 70, "overflow uses safe distributed fallback without negative gaps");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	const int flex = makeDiv(root, nullptr);
	auto style = NodeHandle(flex).style();
	style.setProperty("display", "flex"); style.setProperty("flex-flow", "row wrap");
	style.width(100); style.height(140); style.setProperty("row-gap", "10px");
	style.setProperty("align-content", "space-evenly");
	int children[3];
	for (int &child : children) { child = makeDiv(flex, nullptr); NodeHandle(child).style().width(100); NodeHandle(child).style().height(20); }
	tree.mount(root, 300, 300);
	for (int i = 0; i < 3; ++i) expectEqual(tree.node(children[i]).layout.y, 15 + i * 45, "flex lines receive even space in addition to row gap");
}

void testFloatFormattingContextHeight()
{
	for (const char *kind : {"normal", "relative", "absolute", "fixed", "float", "overflow", "flex-item", "grid-item", "root"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().width(300);
		const bool rootCase = std::strcmp(kind, "root") == 0;
		const int box = rootCase ? root : makeDiv(root, nullptr);
		auto style = NodeHandle(box).style(); style.width(100);
		style.setProperty("padding", "3px"); style.setProperty("border", "2px solid #000000");
		if (std::strcmp(kind, "absolute") == 0 || std::strcmp(kind, "fixed") == 0 || std::strcmp(kind, "relative") == 0)
			style.setProperty("position", kind);
		if (std::strcmp(kind, "float") == 0) style.setProperty("float", "left");
		if (std::strcmp(kind, "overflow") == 0) style.setProperty("overflow", "hidden");
		if (std::strcmp(kind, "flex-item") == 0) { NodeHandle(root).style().setProperty("display", "flex"); NodeHandle(root).style().setProperty("align-items", "start"); }
		if (std::strcmp(kind, "grid-item") == 0) { NodeHandle(root).style().setProperty("display", "grid"); NodeHandle(root).style().setProperty("align-items", "start"); }
		const int child = makeDiv(box, nullptr);
		auto cs = NodeHandle(child).style(); cs.width(30); cs.height(40);
		cs.setProperty("float", "left"); cs.setProperty("margin", "7px 0 11px");
		tree.mount(root, 300, 300);
		const bool encloses = std::strcmp(kind, "normal") && std::strcmp(kind, "relative");
		if (tree.node(box).layout.height != (encloses ? 68 : 10)) std::fprintf(stderr, "float-height case: %s\n", kind);
		expectEqual(tree.node(box).layout.height, encloses ? 68 : 10, "BFC auto height encloses float margin box; ordinary block does not");
		cs.height(60); tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.height, encloses ? 88 : 10, "float height mutation recomputes formatting-context extent");
		style.height(20); tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.height, 30, "explicit height does not expand to enclose floats");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), box = makeDiv(root, nullptr), wrapper = makeDiv(box, nullptr), floated = makeDiv(wrapper, nullptr);
	NodeHandle(root).style().width(300);
	auto s = NodeHandle(box).style(), w = NodeHandle(wrapper).style(), f = NodeHandle(floated).style();
	s.setProperty("position", "absolute"); s.width(100); s.setProperty("padding", "5px");
	w.setProperty("margin-top", "10px"); w.setProperty("position", "relative"); w.top(30);
	f.setProperty("float", "left"); f.width(20); f.height(40); f.setProperty("margin-bottom", "7px");
	f.setProperty("position", "relative"); f.top(20);
	tree.mount(root, 300, 300);
	expectEqual(tree.node(box).layout.height, 67, "BFC encloses nested floats without relative offsets");
	w.setProperty("overflow", "hidden"); w.height(15); tree.refresh(root, 300, 300);
	expectEqual(tree.node(box).layout.height, 35, "outer BFC does not enclose floats in an inner formatting context");
	w.setProperty("overflow", "visible"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(box).layout.height, 67, "fixed-height ordinary wrapper does not isolate its floats");
	s.setProperty("max-height", "25px"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(box).layout.height, 35, "max-height constrains automatic float enclosure");
}

void testFloatShrinkToFit()
{
	for (const char *display : {"block", "flex"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr);
		NodeHandle(root).style().width(300);
		const int box = makeDiv(root, nullptr);
		auto style = NodeHandle(box).style();
		style.setProperty("float", "left"); style.setProperty("display", display);
		style.setProperty("flex-flow", "row wrap");
		style.setProperty("border", "1px solid #000000");
		style.setProperty("min-width", "100px");
		int children[5];
		for (int &child : children) {
			child = makeDiv(box, nullptr);
			auto cs = NodeHandle(child).style();
			cs.width(30); cs.height(10); cs.setProperty("border", "1px solid #0000ff");
			if (std::strcmp(display, "block") == 0) cs.setProperty("float", "left");
		}
		tree.mount(root, 300, 300);
		expectEqual(tree.node(box).layout.width, 162, "automatic float uses preferred width including floated contents");
		expectEqual(tree.node(box).layout.height, 14, "float encloses a single content line");
		style.setProperty("max-width", "120px");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.width, 122, "max-width clamps preferred float size without shrinking to the wrapped line");
		expectEqual(tree.node(box).layout.height, 26, "float recomputes height after constrained width wraps its children");
		expectEqual(tree.node(children[3]).layout.y - tree.node(box).layout.y, 13, "fourth float item wraps at the selected used width");
		style.removeProperty("max-width"); style.removeProperty("min-width");
		NodeHandle(root).style().width(80);
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.width, 80, "float width clamps to available containing-block space");
		expectEqual(tree.node(box).layout.height, 38, "available width produces three content lines");
		style.setProperty("margin", "0 5px");
		style.setProperty("padding", "2px");
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.width, 70, "shrink-to-fit accounts for margins and padding");
		NodeHandle(root).style().width(20);
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.width, 38, "min-content floor can overflow the available float width");
		style.width(90);
		tree.refresh(root, 300, 300);
		expectEqual(tree.node(box).layout.width, 96, "an explicit width leaves shrink-to-fit sizing");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	const int box = makeDiv(root, nullptr);
	auto style = NodeHandle(box).style(); style.setProperty("float", "right");
	const int left = makeDiv(box, nullptr), right = makeDiv(box, nullptr);
	NodeHandle(left).style().setProperty("float", "left");
	NodeHandle(right).style().setProperty("float", "right");
	for (int child : {left, right}) { NodeHandle(child).style().width(40); NodeHandle(child).style().height(20); }
	tree.mount(root, 300, 300);
	expectEqual(tree.node(box).layout.width, 80, "opposing floats both contribute to preferred width");
	expectEqual(tree.node(box).layout.x, 220, "right float positions using its shrink-to-fit width");
	expectEqual(tree.node(right).layout.x - tree.node(box).layout.x, 40, "right child positions against final float width");
	NodeHandle(right).style().setProperty("clear", "both");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(box).layout.width, 40, "clear separates float intrinsic contributions");
	expectEqual(tree.node(box).layout.height, 40, "cleared float remains inside automatic height");
}

void testEmptyBoxFloatPosition()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	const int group = makeDiv(root, nullptr);
	const int first = makeDiv(group, nullptr), empty = makeDiv(group, nullptr);
	NodeHandle(first).style().height(20);
	NodeHandle(first).style().setProperty("margin-bottom", "16px");
	const int floated = makeDiv(empty, nullptr);
	NodeHandle(floated).style().setProperty("float", "left");
	NodeHandle(floated).style().width(30); NodeHandle(floated).style().height(40);
	tree.mount(root, 300, 300);
	expectEqual(tree.node(group).layout.height, 20, "trailing collapsed margin stays outside parent height");
	expectEqual(tree.node(empty).layout.y, 36, "trailing empty box follows preceding collapsed margin");
	expectEqual(tree.node(floated).layout.y, 36, "float retains its empty parent's border edge");
	NodeHandle(empty).style().setProperty("margin", "10px 0 40px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(empty).layout.y, 36, "empty box bottom margin does not affect its own top edge");
	expectEqual(tree.node(root).layout.height, 76, "root encloses the nested float beyond collapsed bottom margins");
	const int last = makeDiv(group, nullptr);
	NodeHandle(last).style().height(10); NodeHandle(last).style().setProperty("margin-top", "60px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(empty).layout.y, 36, "following sibling margin does not move earlier empty box");
	expectEqual(tree.node(last).layout.y, 80, "following sibling uses the complete collapsed margin group");
	NodeHandle(first).style().height(0);
	NodeHandle(first).style().setProperty("margin-top", "25px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(empty).layout.y, tree.node(group).layout.y, "empty box sharing parent top margin uses parent's border edge");
}

void testFloatClearanceMargins()
{
	struct Case { const char *bottom, *top, *clear; int height, expectedFloat, expectedBlock, expectedNext; };
	const Case cases[] = {
		{"0px", "20px", "left", 10, 10, 50, 65},
		{"0px", "70px", "left", 10, 10, 80, 95},
		{"0px", "-20px", "left", 10, 10, 50, 65},
		{"40px", "60px", "left", 10, 50, 90, 105},
		{"30px", "20px", "none", 10, 40, 40, 55},
		{"0px", "10px", "left", 0, 10, 50, 140},
	};
	for (const auto &test : cases) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), previous = makeDiv(root, nullptr), floated = makeDiv(root, nullptr);
		const int block = makeDiv(root, nullptr), next = makeDiv(root, nullptr);
		NodeHandle(root).style().width(300);
		NodeHandle(previous).style().height(10);
		NodeHandle(previous).style().setProperty("margin-bottom", test.bottom);
		auto f = NodeHandle(floated).style(); f.width(30); f.height(40); f.setProperty("float", "left");
		auto b = NodeHandle(block).style(); b.height(test.height); b.setProperty("margin-top", test.top); b.setProperty("clear", test.clear);
		NodeHandle(next).style().height(10);
		NodeHandle(next).style().setProperty("margin-top", test.height ? "5px" : "100px");
		tree.mount(root, 300, 300);
		expectEqual(tree.node(floated).layout.y, test.expectedFloat, "float follows preceding collapsed margin");
		expectEqual(tree.node(block).layout.y, test.expectedBlock, "clearance aligns border edge after hypothetical margin collapse");
		expectEqual(tree.node(block).layout.width, 300, "ordinary block fills containing width beside float");
		expectEqual(tree.node(next).layout.y, test.expectedNext, "following margin collapses from cleared block");
		b.setProperty("margin-top", "120px"); tree.refresh(root, 300, 300);
		expectEqual(tree.node(block).layout.y, 130, "margin mutation removes unnecessary clearance");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), floated = makeDiv(root, nullptr), wrapper = makeDiv(root, nullptr), child = makeDiv(wrapper, nullptr);
	NodeHandle(root).style().width(100);
	auto f = NodeHandle(floated).style(); f.width(30); f.height(50); f.setProperty("float", "left");
	auto w = NodeHandle(wrapper).style(); w.setProperty("clear", "left"); w.setProperty("margin-top", "25px");
	auto c = NodeHandle(child).style(); c.height(50); c.setProperty("margin-top", "150px");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(wrapper).layout.y, 150, "descendant margin participates in hypothetical position before clearance");
	expectEqual(tree.node(child).layout.y, 150, "collapsed child margin does not add internal space");
	expectEqual(tree.node(child).layout.width, 100, "float-path fill width reaches nested blocks");
}

void testNestedFloatClearance()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), first = makeDiv(root, nullptr), wrapper = makeDiv(root, nullptr), child = makeDiv(wrapper, nullptr);
	NodeHandle(root).style().width(100);
	auto f = NodeHandle(first).style(); f.setProperty("float", "left"); f.width(50); f.height(50);
	auto c = NodeHandle(child).style(); c.setProperty("clear", "left"); c.setProperty("margin-top", "20px"); c.height(20);
	tree.mount(root, 300, 300);
	expectEqual(tree.node(wrapper).layout.y, 0, "nested clearance prevents child margin escaping through wrapper top");
	expectEqual(tree.node(child).layout.y, 50, "nested clear sees float in same formatting context");
	expectEqual(tree.node(wrapper).layout.height, 70, "nested clearance contributes to wrapper automatic height");
	c.setProperty("margin-top", "80px"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(wrapper).layout.y, 80, "unnecessary nested clear preserves parent-child margin collapse");
	expectEqual(tree.node(child).layout.y, 80, "large collapsed margin already clears float");
	expectEqual(tree.node(wrapper).layout.height, 20, "collapsed margin is outside wrapper height");
	c.setProperty("margin-top", "20px"); f.height(100); tree.refresh(root, 300, 300);
	expectEqual(tree.node(child).layout.y, 100, "float mutation updates nested clearance");
	NodeHandle(wrapper).style().setProperty("position", "absolute"); NodeHandle(wrapper).style().top(0);
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(child).layout.y, 20, "absolute formatting context isolates exterior floats");
	NodeHandle(wrapper).style().setProperty("position", "static"); f.setProperty("display", "none");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(wrapper).layout.y, 20, "clear without any floats does not prevent parent-child margin collapse");
	expectEqual(tree.node(child).layout.y, 20, "authored clear alone does not introduce clearance");

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	const int negativeRoot = makeDiv(-1, nullptr), tallFloat = makeDiv(negativeRoot, nullptr), padded = makeDiv(negativeRoot, nullptr);
	const int group = makeDiv(padded, nullptr), empty = makeDiv(group, nullptr), cleared = makeDiv(group, nullptr), after = makeDiv(padded, nullptr);
	NodeHandle(negativeRoot).style().width(100);
	auto tf = NodeHandle(tallFloat).style(); tf.setProperty("float", "left"); tf.width(50); tf.height(100);
	NodeHandle(padded).style().setProperty("padding-top", "1px");
	NodeHandle(empty).style().setProperty("margin-bottom", "49px");
	auto clearStyle = NodeHandle(cleared).style(); clearStyle.setProperty("clear", "left"); clearStyle.setProperty("margin-top", "98px");
	NodeHandle(after).style().height(50);
	tree.mount(negativeRoot, 300, 300);
	expectEqual(tree.node(group).layout.y, 50, "only margins before actual clearance escape through parent top");
	expectEqual(tree.node(group).layout.height, 50, "negative clearance retains height between collapsed margins and float bottom");
	expectEqual(tree.node(cleared).layout.y, 100, "negative clearance places empty cleared border at float bottom");
	expectEqual(tree.node(after).layout.y, 100, "following sibling starts after cleared margin group");

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	const int avoidRoot = makeDiv(-1, nullptr), left = makeDiv(avoidRoot, nullptr), right = makeDiv(avoidRoot, nullptr);
	const int lower = makeDiv(avoidRoot, nullptr), context = makeDiv(avoidRoot, nullptr);
	NodeHandle(avoidRoot).style().width(100);
	for (int floated : {left, right, lower}) {
		auto style = NodeHandle(floated).style(); style.width(25); style.height(floated == left ? 10 : 20);
		style.setProperty("float", floated == right ? "right" : "left");
	}
	NodeHandle(lower).style().setProperty("clear", "left");
	auto isolated = NodeHandle(context).style(); isolated.width(60); isolated.height(80); isolated.setProperty("overflow", "hidden");
	tree.mount(avoidRoot, 300, 300);
	expectEqual(tree.node(lower).layout.y, 10, "cleared float starts below preceding same-side float");
	expectEqual(tree.node(context).layout.x, 25, "new formatting context avoids left float");
	expectEqual(tree.node(context).layout.y, 20, "new formatting context moves below narrow float exclusion band");

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	const int zeroRoot = makeDiv(-1, nullptr), zero = makeDiv(zeroRoot, nullptr), opposite = makeDiv(zeroRoot, nullptr), fill = makeDiv(zeroRoot, nullptr);
	NodeHandle(zeroRoot).style().width(125);
	auto z = NodeHandle(zero).style(); z.setProperty("float", "left"); z.width(0); z.height(50);
	auto o = NodeHandle(opposite).style(); o.setProperty("float", "right"); o.setProperty("clear", "left"); o.width(25); o.height(50);
	auto bfc = NodeHandle(fill).style(); bfc.setProperty("overflow", "hidden"); bfc.setProperty("margin-left", "-50px"); bfc.height(100);
	tree.mount(zeroRoot, 300, 300);
	expectEqual(tree.node(fill).layout.x, 0, "zero-width float constrains negative margin at border edge");
	expectEqual(tree.node(fill).layout.y, 0, "automatic BFC width fits beside floats without moving below them");
	expectEqual(tree.node(fill).layout.width, 100, "automatic BFC fills interval bounded by both float sides");
}

void testBlockInInlineFormatting()
{
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr), firstSpan = makeSpan(root, nullptr), first = makeDiv(firstSpan, nullptr);
	const int secondSpan = makeSpan(root, nullptr), nested = makeSpan(secondSpan, nullptr), second = makeDiv(nested, nullptr);
	NodeHandle(root).style().width(100);
	NodeHandle(firstSpan).style().setProperty("margin", "50px 0");
	NodeHandle(nested).style().setProperty("margin", "70px 0");
	auto a = NodeHandle(first).style(); a.height(20); a.setProperty("margin-bottom", "40px"); a.setProperty("background", "#000000");
	auto b = NodeHandle(second).style(); b.height(20); b.setProperty("margin-top", "30px"); b.setProperty("background", "#000000");
	tree.mount(root, 300, 300);
	expectEqual(tree.node(first).layout.y, 0, "inline ancestor vertical margins do not shift nested block");
	expectEqual(tree.node(second).layout.y, 60, "block margins collapse across separate inline ancestors");
	expectEqual(tree.node(root).layout.height, 80, "projected blocks determine containing block height");
	NodeHandle(firstSpan).style().setProperty("margin-bottom", "500px"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(root).layout.scroll_content_height, 80, "ignored inline margin does not inflate scroll content");
	expectEqual(tree.node(second).parent, nested, "formatting projection preserves DOM ancestry");
	expectEqual(tree.hitTestNode(5, 65), second, "projected block remains hittable through inline ancestry");
	a.setProperty("margin-bottom", "60px"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(second).layout.y, 80, "margin mutation updates sibling across inline wrappers");
	a.setProperty("margin-bottom", "40px");
	auto spanStyle = NodeHandle(firstSpan).style(); spanStyle.setProperty("position", "relative"); spanStyle.left(7); spanStyle.top(5);
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(first).layout.x, 7, "relative inline ancestor translates projected block horizontally");
	expectEqual(tree.node(first).layout.y, 5, "relative inline ancestor translates projected block vertically");
	expectEqual(tree.node(second).layout.y, 60, "relative inline translation does not shift following flow");

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	const int textRoot = makeDiv(-1, nullptr);
	auto rs = NodeHandle(textRoot).style(); rs.width(20); rs.set(Property::FontId, 9101); rs.set(Property::FontSize, 10); rs.setProperty("line-height", "10px");
	auto prefix = Document::instance().createText("X"); NodeHandle(textRoot).appendChild(prefix);
	const int span = makeSpan(textRoot, nullptr);
	auto leading = Document::instance().createText("X"); NodeHandle(span).appendChild(leading);
	const int block = makeDiv(span, nullptr); NodeHandle(block).style().height(10); NodeHandle(block).style().setProperty("margin-bottom", "20px");
	auto trailing = Document::instance().createText("XX"); NodeHandle(span).appendChild(trailing);
	tree.mount(textRoot, 300, 300);
	expectEqual(tree.node(leading.id()).layout.x, 5, "inline run continues across split wrapper start");
	expectEqual(tree.node(block).layout.y, 10, "nested block starts after preceding anonymous line");
	expectEqual(tree.node(trailing.id()).layout.y, 40, "following anonymous line starts after block bottom margin");
	expectEqual(tree.node(textRoot).layout.height, 50, "anonymous lines and block all contribute to height");

	// A BR belongs to the preceding anonymous line, including when it clears
	// a float. It must not add a second line after that line was already laid out.
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	const int breaksRoot = makeDiv(-1, nullptr);
	auto bs = NodeHandle(breaksRoot).style(); bs.width(100); bs.set(Property::FontId, 9101); bs.set(Property::FontSize, 10); bs.setProperty("line-height", "10px");
	const int initialBlock = makeDiv(breaksRoot, nullptr); NodeHandle(initialBlock).style().height(10);
	auto line = Document::instance().createText("XX"); NodeHandle(breaksRoot).appendChild(line);
	auto br = Document::instance().createElement("br"); NodeHandle(breaksRoot).appendChild(br);
	auto nextLine = Document::instance().createText("XX"); NodeHandle(breaksRoot).appendChild(nextLine);
	tree.mount(breaksRoot, 300, 300);
	expectEqual(tree.node(br.id()).layout.y, 10, "BR terminates preceding anonymous line");
	expectEqual(tree.node(nextLine.id()).layout.y, 20, "BR advances one line in mixed block and inline flow");
	expectEqual(tree.node(breaksRoot).layout.height, 30, "mixed flow has no extra line for BR");
	const int floating = makeDiv(breaksRoot, nullptr); auto fs = NodeHandle(floating).style(); fs.setProperty("float", "left"); fs.width(10); fs.height(60);
	NodeHandle(breaksRoot).insertBefore(NodeHandle(floating), line);
	br.style().setProperty("clear", "both"); tree.refresh(breaksRoot, 300, 300);
	expectEqual(tree.node(br.id()).layout.y, 10, "clearing BR stays on preceding anonymous line");
	expectEqual(tree.node(nextLine.id()).layout.y, 70, "clearing BR moves next anonymous line to float bottom");

	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	const int sizingRoot = makeDiv(-1, nullptr), fit = makeDiv(sizingRoot, nullptr);
	NodeHandle(sizingRoot).style().width(100);
	auto fitStyle = NodeHandle(fit).style(); fitStyle.setProperty("float", "left");
	fitStyle.set(Property::FontId, 9101); fitStyle.set(Property::FontSize, 10); fitStyle.setProperty("line-height", "10px");
	const int sizedBlock = makeDiv(fit, nullptr); NodeHandle(sizedBlock).style().width(15); NodeHandle(sizedBlock).style().height(10);
	NodeHandle(fit).appendChild(Document::instance().createText("XX"));
	NodeHandle(fit).appendChild(Document::instance().createElement("br"));
	NodeHandle(fit).appendChild(Document::instance().createText("XXXX"));
	tree.mount(sizingRoot, 300, 300);
	expectEqual(tree.node(fit).layout.width, 20, "mixed flow shrink-to-fit measures longest line and block");
	expectEqual(tree.node(fit).layout.height, 30, "shrink-to-fit preserves anonymous line breaks");
}

void testAutomaticFlexWrapBlockSize()
{
	for (const char *maximum : {"none", "120px"}) {
		resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
		auto &tree = Tree::instance();
		const int root = makeDiv(-1, nullptr), wrapper = makeDiv(root, nullptr), flex = makeDiv(wrapper, nullptr);
		NodeHandle(root).style().width(300);
		auto s = NodeHandle(flex).style(); s.setProperty("float", "left"); s.setProperty("display", "flex");
		s.setProperty("flex-direction", "column"); s.setProperty("flex-wrap", "wrap");
		s.width(12); s.setProperty("min-height", "100px"); s.setProperty("max-height", maximum);
		int items[5];
		for (int &item : items) { item = makeDiv(flex, nullptr); NodeHandle(item).style().height(32); }
		tree.mount(root, 300, 300);
		const bool bounded = std::strcmp(maximum, "none") != 0;
		expectEqual(tree.node(flex).layout.height, bounded ? 100 : 160, "automatic flex block size uses maximum, never minimum, for line packing");
		expectEqual(tree.node(items[3]).layout.x - tree.node(flex).layout.x, bounded ? 6 : 0, "only maximum creates a second flex column");
		expectEqual(tree.node(items[3]).layout.y - tree.node(flex).layout.y, bounded ? 0 : 96, "unbounded automatic flex column remains continuous");
	}
	resetNativeHost(); StyleSheet::instance().clear(); setViewportMetrics(300, 300, 1.0);
	StyleSheet::instance().registerRule("uncapped", "max-width", "none");
	StyleSheet::instance().registerRule("uncapped", "max-height", "none");
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().width(300);
	int boxes[2];
	for (int &box : boxes) {
		box = makeDiv(root, "uncapped"); NodeHandle(box).style().width(120); NodeHandle(box).style().height(30);
	}
	tree.mount(root, 300, 300);
	for (int box : boxes) {
		expectEqual(tree.node(box).layout.width, 120, "cached max-width none preserves unbounded width");
		expectEqual(tree.node(box).layout.height, 30, "cached max-height none preserves unbounded height");
	}
	auto s = NodeHandle(boxes[0]).style(); s.setProperty("max-width", "20px"); s.setProperty("max-height", "10px");
	tree.refresh(root, 300, 300);
	expectEqual(tree.node(boxes[0]).layout.width, 20, "maximum mutation constrains width");
	s.setProperty("max-width", "none"); s.setProperty("max-height", "none"); tree.refresh(root, 300, 300);
	expectEqual(tree.node(boxes[0]).layout.width, 120, "inline max-width none removes previous constraint");
	expectEqual(tree.node(boxes[0]).layout.height, 30, "inline max-height none removes previous constraint");
}

void testFloatReferenceGeometry()
{
	resetNativeHost();
	StyleSheet::instance().clear();
	setViewportMetrics(200, 200, 1.0);
	auto &tree = Tree::instance();
	const int root = makeDiv(-1, nullptr);
	NodeHandle(root).style().setProperty("display", "block");
	NodeHandle(root).style().width(100);
	int children[3];
	for (int &child : children) {
		child = makeDiv(root, nullptr);
		NodeHandle(child).style().setProperty("float", "left");
		NodeHandle(child).style().width(40);
		NodeHandle(child).style().height(20);
	}
	tree.mount(root, 200, 200);
	expectEqual(tree.node(children[1]).layout.x, 40, "floats: second box fits beside first");
	expectEqual(tree.node(children[2]).layout.y, 20, "floats: third box wraps below occupied band");
	NodeHandle(children[1]).style().setProperty("clear", "left");
	tree.refresh(root, 200, 200);
	expectEqual(tree.node(children[1]).layout.y, 20, "clear: left float starts below preceding float");
}

}  // namespace

int main()
{
	testBoxSizingAndDeferredCalc();
	testBaselineAlignment();
	testSelfRelativeAlignment();
	testForcedLineBreaks();
	testBalancedFlex();
	testAspectRatio();
	testLeafBoxSizing();
	testGridAbsoluteStaticAlignment();
	testGridAutoTrackStretch();
	testAnonymousTextSemantics();
	testFontShorthandAndRelativeSize();
	testUnitlessLineHeightInheritance();
	testCssWhitespaceModes();
	testFontRelativeDimensions();
	testLineHeightRelativeUnits();
	testUserAgentDefaultsAndFontUnits();
	testBlockMarginsAndIntrinsicWidth();
	testDeferredPercentageBoxEdges();
	testFloatingRootGeometry();
	testIntrinsicHeightKeywords();
	testSafeOverflowAlignment();
	testInlineGridTrackPersistence();
	testGridJustifySelf();
	testAbsoluteDescendantContainingBlockSizing();
	testAbsoluteShrinkToFitStaticPosition();
	testAbsoluteBlockStaticPosition();
	testAbsoluteInlineStaticPosition();
	testRotationFunctionLists();
	testIndividualRotationAndScale();
	testIndividualTranslate();
	testZeroTransformOrigins();
	testAbsolutePaddingBoxContainingBlock();
	testVerticalBlockFlow();
	testAbsoluteAutoInsetAlignment();
	testOrthogonalFlexPercentageGaps();
	testAbsoluteDistributedFallback();
	testClearingBreakWhitespace();
	testVisibilityInheritanceAndPainting();
	testComputedBorderInheritance();
	testCollapsedFlexItems();
	testGridAreasAndShorthand();
	testIntrinsicWidths();
	testOverflowClip();
	testFixedPositioning();
	testAbsoluteOverflowAlignmentMutation();
	testInheritedFlexDirectionAndSidewaysModes();
	testGapAxesAndAutoMargins();
	testBorderWidthSnapping();
	testBorderWidthShorthandPixels();
	testBorderCurrentColor();
	testSharpInsetShadow();
	testAlignmentShorthands();
	testDistributedAlignment();
	testFloatReferenceGeometry();
	testFloatClearanceMargins();
	testNestedFloatClearance();
	testBlockInInlineFormatting();
	testAutomaticFlexWrapBlockSize();
	testFloatShrinkToFit();
	testFloatFormattingContextHeight();
	testEmptyBoxFloatPosition();
	testUniversalSelectorAndAutomaticMinimum();
	testGridGapAxes();
	testFlexLineAlignment();
	testOrderLayoutAndInvalidation();
	testOrderPaintingAndHitTesting();
	testPositionedPaintOrder();
	testStackingContextDescendants();
	testDisplayBlockStacks();
	testDefaultSpansStillRow();
	testVerticalMarginSpanStacks();
	testFlexBasisFixedWidth();
	testDeferredFlexBasis();
	testSpecificityDescendantBeatsBase();
	testThreePartDescendantSelector();
	testRootAndIdSelectors();
	testCandidateCacheKeepsAncestorSelectorsDynamic();
	testCachedGridTemplateRulesApply();
	testStaticCustomLengthExpressionPreResolves();
	testPercentCustomLengthStaysDynamic();
	testStaticLengthExpressionCacheInvalidatesWithViewport();
	testActiveRuleCollapseKeepsCascadeSemantics();
	testCustomPropertyLookupCacheInvalidates();
	testStaticCustomColorPrecompiles();
	testCompiledGradientColorVarFallbacks();
	testBackgroundColorClipping();
	testIndependentBackgroundLayers();
	testDocumentCanvasBackground();
	testHoverCascadeAndScale();
	testUnbreakableTextBesideFloats();
	testBlockMarginTrim();
	testCanvasBackgroundImages();
	testWideBackgroundScanlines();
	testPseudoSelectorMaterializeAndRemove();
	if (gFailures == 0) {
		std::fprintf(stderr, "[css_block_flexbasis] ALL PASS\n");
		return 0;
	}
	std::fprintf(stderr, "[css_block_flexbasis] %d FAILURE(S)\n", gFailures);
	return 1;
}
