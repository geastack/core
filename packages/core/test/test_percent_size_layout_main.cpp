#include "native_test_harness.h"

#include "ui/document.h"
#include "ui/node.h"
#include "ui/style.h"
#include "ui/tree_internal.h"

#include <cstdio>


namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {

bool expectEqual(int actual, int expected, const char *label)
{
	if (actual == expected) return true;
	std::fprintf(stderr, "[test_percent_size_layout] %s expected %d, got %d\n", label, expected, actual);
	return false;
}

}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	StyleSheet::instance().clear();
	gea::embedded::ui::setViewportMetrics(300, 180, 1.0);

	const int parentId = Tree::instance().createView();
	const int childId = Tree::instance().createView();
	NodeHandle parent(parentId);
	parent.appendChild(NodeHandle(childId));
	Tree::instance().mount(parentId, 300, 180);

	StyleSheet::instance().registerRule("stale-percent-parent", "width", "180");
	StyleSheet::instance().registerRule("stale-percent-parent", "height", "120");
	StyleSheet::instance().registerRule("stale-percent-parent", "box-sizing", "border-box");
	StyleSheet::instance().registerRule("stale-percent-parent", "padding", "10");
	StyleSheet::instance().registerRule("stale-percent-child", "width", "100%");
	StyleSheet::instance().registerRule("stale-percent-child", "height", "100%");
	parent.classList().set("stale-percent-parent");

	auto &parentNode = Tree::instance().node(parentId);
	parentNode.layout.width = 451;
	parentNode.layout.height = 2151;
	NodeHandle(childId).classList().set("stale-percent-child");
	Tree::instance().computeLayout(parentId, 300, 180);

	const auto &child = Tree::instance().node(childId);
	if (!expectEqual(child.layout.x, 10, "child x")) return 1;
	if (!expectEqual(child.layout.y, 10, "child y")) return 1;
	if (!expectEqual(child.layout.width, 160, "child width")) return 1;
	if (!expectEqual(child.layout.height, 100, "child height")) return 1;

	parent.style().setProperty("box-sizing", "content-box");
	Tree::instance().computeLayout(parentId, 300, 180);
	if (!expectEqual(child.layout.width, 180, "content-box child width")) return 1;
	if (!expectEqual(child.layout.height, 120, "content-box child height")) return 1;

	return 0;
}
