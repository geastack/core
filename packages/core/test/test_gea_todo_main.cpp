#include "native_test_harness.h"

#include "events.h"
#include "ui/style.h"
#include "ui/tree_internal.h"
#include "ui/virtual_keyboard.h"

#include <cstdio>
#include <string>
#include <vector>

extern void __gea_top_level();

int main()
{
	using namespace gea::embedded::test;
	resetNativeHost();
	gea::embedded::ui::setViewportMetrics(410, 502, 1.5);
	__gea_top_level();
	refresh();

	auto text = rootTextContent();
	if (!expectContains(text, "Todos", "initial text", "test_gea_todo_main")) return 1;
	if (!expectContains(text, "Tap a task to complete it", "initial text", "test_gea_todo_main")) return 1;
	if (!expectContains(text, "Build counter", "initial text", "test_gea_todo_main")) return 1;
	if (!expectContains(text, "Try stopwatch laps", "initial text", "test_gea_todo_main")) return 1;
	if (!expectContains(text, "Add a todo", "initial text", "test_gea_todo_main")) return 1;

	auto &tree = gea::embedded::ui::Tree::instance();
	const auto apps = nodesWithClass("todo-app");
	const auto titles = nodesWithClass("todo-title");
	if (apps.empty() || titles.empty()) {
		std::fprintf(stderr, "[test_gea_todo_main] expected styled todo app and title nodes\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	const auto &appStyle = tree.node(apps[0]).style;
	const auto &titleStyle = tree.node(titles[0]).style;
	if (appStyle.width != 410 || appStyle.height != 502 || appStyle.has_bg == 0 ||
	    titleStyle.font_size != 36 || titleStyle.font_id < 0) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] expected todo CSS styles, app=%dx%d bg=%d titleFont=%d titleSize=%d\n",
		             appStyle.width,
		             appStyle.height,
		             static_cast<int>(appStyle.has_bg),
		             titleStyle.font_id,
		             titleStyle.font_size);
		dumpTree("test_gea_todo_main");
		return 1;
	}

	if (nodesWithClass("todo-row").size() != 3) {
		std::fprintf(stderr, "[test_gea_todo_main] expected 3 visible todo rows\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}

	const auto checks = nodesWithClass("todo-check");
	if (checks.size() < 2 || !dispatchPress(checks[1])) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Todo updated", "toggle text", "test_gea_todo_main")) return 1;

	const auto deletes = nodesWithClass("todo-delete-button");
	if (deletes.empty() || !dispatchPress(deletes[0])) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Todo removed", "delete text", "test_gea_todo_main")) return 1;
	if (!expectContains(text, "Try stopwatch laps", "delete text", "test_gea_todo_main")) return 1;
	if (text.find("Build counter") != std::string::npos) {
		std::fprintf(stderr, "[test_gea_todo_main] expected deleted first todo to be gone, got:\n%s\n", text.c_str());
		return 1;
	}

	auto expectButtonTextInset = [&](const std::vector<int> &buttons, const char *buttonClass, int minTotalInset) {
		for (const int buttonNode : buttons) {
			const auto &button = tree.node(buttonNode);
			for (int child = button.first_child; child >= 0; child = tree.node(child).next_sibling) {
				const auto &label = tree.node(child);
				if (label.type != gea::embedded::ui::NodeType::Text || label.style.display == 1) continue;
				const int totalInset = button.layout.width - label.layout.width;
				if (totalInset < minTotalInset) {
					std::fprintf(stderr,
					             "[test_gea_todo_main] expected %s label to have horizontal inset, button=%d label=%d buttonWidth=%d labelWidth=%d inset=%d min=%d text=%s\n",
					             buttonClass,
					             buttonNode,
					             child,
					             button.layout.width,
					             label.layout.width,
					             totalInset,
					             minTotalInset,
					             label.text.c_str());
					dumpTree("test_gea_todo_main");
					return false;
				}
			}
		}
		return true;
	};
	const int minButtonTextInset = 24;
	if (!expectButtonTextInset(nodesWithClass("todo-add-button"), "todo-add-button", minButtonTextInset)) return 1;
	if (!expectButtonTextInset(nodesWithClass("todo-delete-button"), "todo-delete-button", minButtonTextInset)) return 1;
	auto expectContentSizedButton = [&](const std::vector<int> &buttons, const char *buttonClass) {
		for (const int buttonNode : buttons) {
			const auto &button = tree.node(buttonNode);
			const int horizontalPadding = button.style.padding[1] + button.style.padding[3];
			const int verticalPadding = button.style.padding[0] + button.style.padding[2];
			if (button.style.width != gea::embedded::ui::kUnset ||
			    button.style.height != gea::embedded::ui::kUnset ||
			    (button.style.min_width != 0 && button.style.min_width != gea::embedded::ui::kUnset) ||
			    (button.style.min_height != 0 && button.style.min_height != gea::embedded::ui::kUnset) ||
			    horizontalPadding <= 0 ||
			    verticalPadding <= 0) {
				std::fprintf(stderr,
				             "[test_gea_todo_main] expected %s to content-size from label padding, node=%d width=%d height=%d minWidth=%d minHeight=%d paddingX=%d paddingY=%d\n",
				             buttonClass,
				             buttonNode,
				             button.style.width,
				             button.style.height,
				             button.style.min_width,
				             button.style.min_height,
				             horizontalPadding,
				             verticalPadding);
				dumpTree("test_gea_todo_main");
				return false;
			}
			for (int child = button.first_child; child >= 0; child = tree.node(child).next_sibling) {
				const auto &label = tree.node(child);
				if (label.type != gea::embedded::ui::NodeType::Text || label.style.display == 1) continue;
				// Content-box button: padding plus any border (the reset button has a 0.5px one).
				const int expectedHeight = label.layout.height + gea::embedded::ui::boxInsets(button.computedStyle(), false);
				if (button.layout.height != expectedHeight) {
					std::fprintf(stderr,
					             "[test_gea_todo_main] expected %s to keep content-sized height, button=%d label=%d buttonHeight=%d labelHeight=%d paddingY=%d expected=%d\n",
					             buttonClass,
					             buttonNode,
					             child,
					             button.layout.height,
					             label.layout.height,
					             verticalPadding,
					             expectedHeight);
					dumpTree("test_gea_todo_main");
					return false;
				}
			}
		}
		return true;
	};
	if (!expectContentSizedButton(nodesWithClass("todo-add-button"), "todo-add-button")) return 1;
	if (!expectContentSizedButton(nodesWithClass("todo-delete-button"), "todo-delete-button")) return 1;
	if (!expectContentSizedButton(nodesWithClass("todo-reset-button"), "todo-reset-button")) return 1;

	const auto inputWraps = nodesWithClass("todo-input-wrap");
	if (inputWraps.empty()) {
		std::fprintf(stderr, "[test_gea_todo_main] expected todo input wrapper\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	const auto &inputWrap = tree.node(inputWraps[0]);
	if (inputWrap.style.height != gea::embedded::ui::kUnset ||
	    (inputWrap.style.padding[0] + inputWrap.style.padding[2]) <= 0) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] expected todo input wrapper to content-size from vertical padding, height=%d paddingY=%d\n",
		             inputWrap.style.height,
		             inputWrap.style.padding[0] + inputWrap.style.padding[2]);
		dumpTree("test_gea_todo_main");
		return 1;
	}

	if (!pressFirstText("Reset", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Tap a task to complete it", "reset text", "test_gea_todo_main")) return 1;
	if (!expectContains(text, "Build counter", "reset text", "test_gea_todo_main")) return 1;

	const auto inputs = nodesWithClass("todo-input");
	if (inputs.empty()) {
		std::fprintf(stderr, "[test_gea_todo_main] expected todo input\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	if (tree.node(inputs[0]).style.height != gea::embedded::ui::kUnset ||
	    tree.node(inputs[0]).style.flex != 0 ||
	    tree.node(inputs[0]).layout.height <= 0) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] expected todo input to keep intrinsic height, height=%d flex=%d layoutHeight=%d\n",
		             tree.node(inputs[0]).style.height,
		             tree.node(inputs[0]).style.flex,
		             tree.node(inputs[0]).layout.height);
		dumpTree("test_gea_todo_main");
		return 1;
	}
	tree.setAttribute(inputs[0], "value", "Fourth item");
	gea::framework::events::PointerEvent inputEvent{gea::framework::events::PointerEventType::Input};
	inputEvent.targetId = inputs[0];
	if (!tree.dispatchEvent(inputEvent)) {
		std::fprintf(stderr, "[test_gea_todo_main] expected input event to dispatch\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	refresh();

	if (!pressFirstText("Add", true)) return 1;
	refresh();
	text = rootTextContent();
	if (!expectContains(text, "Fourth item", "fourth todo text", "test_gea_todo_main")) return 1;
	if (nodesWithClass("todo-row").size() != 4) {
		std::fprintf(stderr, "[test_gea_todo_main] expected 4 visible todo rows after add\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	for (const int rowNode : nodesWithClass("todo-row")) {
		const auto &row = tree.node(rowNode);
		for (int child = row.first_child; child >= 0; child = tree.node(child).next_sibling) {
			const auto &childNode = tree.node(child);
			if (childNode.style.display == 1) continue;
			if (childNode.layout.x < row.layout.x || childNode.layout.x + childNode.layout.width > row.layout.x + row.layout.width) {
				std::fprintf(stderr,
				             "[test_gea_todo_main] expected todo row children to stay inside row bounds, row=%d child=%d row=(%d,%d %dx%d) child=(%d,%d %dx%d)\n",
				             rowNode,
				             child,
				             row.layout.x,
				             row.layout.y,
				             row.layout.width,
				             row.layout.height,
				             childNode.layout.x,
				             childNode.layout.y,
				             childNode.layout.width,
				             childNode.layout.height);
				dumpTree("test_gea_todo_main");
				return 1;
			}
		}
	}

	const auto lists = nodesWithClass("todo-list");
	if (lists.empty()) {
		std::fprintf(stderr, "[test_gea_todo_main] expected todo list node\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	const int listNode = lists[0];
	const auto &listBeforeDrag = tree.node(listNode);
	const int listHeightBeforeKeyboard = listBeforeDrag.layout.height;
	if (listBeforeDrag.style.overflow != 2) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] expected todo list to be overflow:auto, overflow=%d content=%d height=%d\n",
		             static_cast<int>(listBeforeDrag.style.overflow),
		             listBeforeDrag.layout.scroll_content_height,
		             listBeforeDrag.layout.height);
		dumpTree("test_gea_todo_main");
		return 1;
	}
	if (listBeforeDrag.layout.scroll_content_height > listBeforeDrag.layout.height) {
		const int dragX = listBeforeDrag.layout.x + listBeforeDrag.layout.width / 2;
		const int dragY = listBeforeDrag.layout.y + listBeforeDrag.layout.height / 2;
		tree.pointerDown(dragX, dragY);
		if (!tree.pointerMove(dragX, dragY - 48)) {
			std::fprintf(stderr, "[test_gea_todo_main] expected todo list drag to scroll\n");
			dumpTree("test_gea_todo_main");
			return 1;
		}
		tree.pointerUp();
		if (tree.node(listNode).layout.scroll_y <= 0) {
			std::fprintf(stderr, "[test_gea_todo_main] expected todo list scroll_y to increase\n");
			dumpTree("test_gea_todo_main");
			return 1;
		}
	}

	tree.setScrollTop(listNode, 0);
	refresh();
	// Showing the keyboard shrinks and absolutely pins the app's content node.
	// Hiding it has to undo that in the INLINE-STYLE RECORD as well as in the
	// computed fields, because a class recompute rebuilds computed style out of
	// class rules plus that record -- so a stale record survives every frame in
	// which nothing looks wrong and reinstates the keyboard's geometry the next
	// time any class on the subtree changes. Snapshot the position styles of
	// every root child here, and re-check them after hide + a forced recompute.
	struct RootChildPosition {
		int node;
		int position;
		int top;
		int left;
	};
	// The keyboard resizes the mounted root's last non-keyboard child. Author an
	// inline `top` on it first: `top` is inert on a statically positioned node,
	// so this changes no layout, but it is exactly the case a blanket "remove
	// the property on hide" would get wrong -- an app's own inline value must
	// come back, not vanish with the keyboard's.
	const int mountedRootNode = tree.mountedRoot();
	int scrollableChild = -1;
	for (int child = mountedRootNode >= 0 ? tree.node(mountedRootNode).first_child : -1;
	     child >= 0 && child < tree.nodeCount();
	     child = tree.node(child).next_sibling) {
		scrollableChild = child;
	}
	static constexpr int kAuthoredInlineTop = 7;
	if (scrollableChild >= 0) {
		tree.setStyle(scrollableChild, gea::embedded::ui::Property::Top, kAuthoredInlineTop);
		refresh();
	}

	std::vector<RootChildPosition> positionsBeforeKeyboard;
	for (int child = mountedRootNode >= 0 ? tree.node(mountedRootNode).first_child : -1;
	     child >= 0 && child < tree.nodeCount();
	     child = tree.node(child).next_sibling) {
		const auto &c = tree.node(child);
		positionsBeforeKeyboard.push_back({child, c.style.position, c.style.pos_offsets[0], c.style.pos_offsets[3]});
	}

	const auto &inputNode = tree.node(inputs[0]);
	const int inputX = inputNode.layout.x + inputNode.layout.width / 2;
	const int inputY = inputNode.layout.y + inputNode.layout.height / 2;
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, inputX, inputY);
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, inputX, inputY);
	refresh();
	if (tree.activeInputId() != inputs[0] || !gea::embedded::ui::VirtualKeyboard::instance().active()) {
		std::fprintf(stderr, "[test_gea_todo_main] expected touch focus to open virtual keyboard\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	const auto &listWithKeyboard = tree.node(listNode);
	if (listWithKeyboard.style.overflow != 2 ||
	    listWithKeyboard.layout.scroll_content_height <= listWithKeyboard.layout.height) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] expected todo list to remain scrollable with keyboard open, overflow=%d content=%d height=%d\n",
		             static_cast<int>(listWithKeyboard.style.overflow),
		             listWithKeyboard.layout.scroll_content_height,
		             listWithKeyboard.layout.height);
		dumpTree("test_gea_todo_main");
		return 1;
	}
	const int keyboardDragX = listWithKeyboard.layout.x + listWithKeyboard.layout.width / 2;
	const int keyboardDragY = listWithKeyboard.layout.y + listWithKeyboard.layout.height / 2;
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, keyboardDragX, keyboardDragY);
	dispatchTouch(gea::framework::events::TouchPhase::Move, true, keyboardDragX, keyboardDragY - 48);
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, keyboardDragX, keyboardDragY - 48);
	if (tree.node(listNode).layout.scroll_y <= 0) {
		std::fprintf(stderr, "[test_gea_todo_main] expected todo list touch drag to scroll with keyboard open\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}

	const auto resetButtons = nodesWithClass("todo-reset-button");
	if (resetButtons.empty()) {
		std::fprintf(stderr, "[test_gea_todo_main] expected reset button\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}
	const auto &resetButton = tree.node(resetButtons[0]);
	const int resetX = resetButton.layout.x + resetButton.layout.width / 2;
	const int resetY = resetButton.layout.y + resetButton.layout.height / 2;
	dispatchTouch(gea::framework::events::TouchPhase::Down, true, resetX, resetY);
	dispatchTouch(gea::framework::events::TouchPhase::Up, false, resetX, resetY);
	refresh();
	if (gea::embedded::ui::VirtualKeyboard::instance().active() ||
	    tree.node(listNode).layout.height != listHeightBeforeKeyboard) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] expected keyboard hide to restore list height, active=%d height=%d before=%d\n",
		             gea::embedded::ui::VirtualKeyboard::instance().active() ? 1 : 0,
		             tree.node(listNode).layout.height,
		             listHeightBeforeKeyboard);
		dumpTree("test_gea_todo_main");
		return 1;
	}
	gea::embedded::ui::StyleSheet::instance().recomputeSubtree(mountedRootNode);
	refresh();
	if (scrollableChild >= 0 && tree.node(scrollableChild).style.pos_offsets[0] != kAuthoredInlineTop) {
		std::fprintf(stderr,
		             "[test_gea_todo_main] keyboard hide dropped the app's own inline top on node %d, "
		             "expected %d got %d\n",
		             scrollableChild, kAuthoredInlineTop,
		             static_cast<int>(tree.node(scrollableChild).style.pos_offsets[0]));
		dumpTree("test_gea_todo_main");
		return 1;
	}
	for (const auto &before : positionsBeforeKeyboard) {
		const auto &after = tree.node(before.node);
		if (after.style.position != before.position || after.style.pos_offsets[0] != before.top ||
		    after.style.pos_offsets[3] != before.left) {
			std::fprintf(stderr,
			             "[test_gea_todo_main] keyboard hide left node %d pinned across a class recompute, "
			             "position %d->%d top %d->%d left %d->%d\n",
			             before.node, before.position, static_cast<int>(after.style.position), before.top,
			             static_cast<int>(after.style.pos_offsets[0]), before.left,
			             static_cast<int>(after.style.pos_offsets[3]));
			dumpTree("test_gea_todo_main");
			return 1;
		}
	}

	text = rootTextContent();
	if (!expectContains(text, "Fourth item", "keyboard dismiss text", "test_gea_todo_main")) return 1;
	if (nodesWithClass("todo-row").size() != 4) {
		std::fprintf(stderr, "[test_gea_todo_main] expected dismiss tap not to activate reset button\n");
		dumpTree("test_gea_todo_main");
		return 1;
	}

	return 0;
}
