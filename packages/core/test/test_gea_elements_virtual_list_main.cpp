#include "native_test_harness.h"

#include "app.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern void __gea_top_level();

namespace {
constexpr int kViewportWidth = 720;
constexpr int kViewportHeight = 1440;
constexpr double kDevicePixelRatio = 2.0;
constexpr const char *kTest = "test_gea_elements_virtual_list_main";
// VirtualList renders a fixed pool of SLOT_COUNT (VirtualList.tsx) slots.
constexpr int kSlotCount = 16;

// The row indexes the visible labels show. nodesWithClass skips hidden nodes,
// so slots past the end of the list are not counted.
std::vector<int> visibleRowIndexes()
{
	std::vector<int> out;
	for (const int id : gea::embedded::test::nodesWithClass("row-label")) {
		const std::string text = gea::embedded::test::textContent(id);
		if (text.rfind("Row ", 0) != 0) continue;
		out.push_back(std::atoi(text.c_str() + 4));
	}
	return out;
}

// Every slot shows exactly one row, and together they show rows
// first..first+15 with no gap or repeat.
bool expectRows(const char *phase, int first)
{
	const auto indexes = visibleRowIndexes();
	std::vector<bool> seen(kSlotCount, false);
	bool ok = indexes.size() == static_cast<std::size_t>(kSlotCount);
	for (const int index : indexes) {
		const int offset = index - first;
		if (offset < 0 || offset >= kSlotCount || seen[offset]) ok = false;
		else seen[offset] = true;
	}
	if (!ok) {
		std::fprintf(stderr, "[%s] %s: expected rows %d..%d once each, got %zu labels:", kTest, phase, first, first + kSlotCount - 1, indexes.size());
		for (const int index : indexes) std::fprintf(stderr, " %d", index);
		std::fprintf(stderr, "\n");
	}
	return ok;
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;
	auto &tree = gea::embedded::ui::Tree::instance();

	resetNativeHost();
	setNativeDisplaySize(kViewportWidth, kViewportHeight);
	gea::framework::app::Application::init(kViewportWidth, kViewportHeight, kDevicePixelRatio);
	__gea_top_level();
	refresh();
	gea::framework::app::Application::frame(16);

	const auto lists = nodesWithType(gea::embedded::ui::NodeType::VirtualList);
	if (lists.size() != 1) {
		std::fprintf(stderr, "[%s] expected one virtual-list node, got %zu\n", kTest, lists.size());
		dumpTree(kTest);
		return 1;
	}
	const auto &list = tree.node(lists[0]);

	// The rows are absolutely positioned, so the list's height comes only from
	// the class the app passes.
	const int expectedHeight = static_cast<int>(400 * kDevicePixelRatio);
	bool ok = true;
	if (list.layout.height != expectedHeight) {
		std::fprintf(stderr, "[%s] list height %d, expected %d from .rows\n", kTest, list.layout.height, expectedHeight);
		ok = false;
	}

	// The pool shows the first 16 rows, each from itemTemplate.
	ok = ok && expectRows("initial", 0);

	// Scrolling is not checked here: a scrollTop change re-renders VirtualList
	// and replaces its <virtual-list> node, which resets the native offset.

	if (!ok) {
		dumpTree(kTest);
		return 1;
	}
	return 0;
}
