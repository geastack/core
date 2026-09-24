// The engine's per-node RARE-DATA pool: event listeners, attributes, pressId.
//
// PROVENANCE. This is the salvaged half of `test_gea_host_document_gea_main.cpp`
// / `run-gea-host-document-gea.sh`, which were deleted. That test was a ~430-line
// probe against v1's `gea::runtime::host::document()` -- the boxed `gea_cpp_value`
// DOM shim that `packages/geatsc/src/targets/cpp/runtime/host_document_gea.cpp`
// provided. Neither that file nor the `gea::runtime::host` namespace exists any
// more: geatsc's runtime is the header-only `gea_runtime.h`, which has no such
// namespace and reaches the tree through `gea::jsx::*` and `NodeHandle` instead.
// A test whose subject was deleted is dead, not failing, so the probe went with
// it.
//
// But roughly 90 of its lines never touched the DOM shim at all: they drove
// `gea::embedded::ui::Tree` directly, and that API is entirely intact. Those
// assertions are the ONLY coverage of the rare-data pool, so they are ported
// here verbatim in substance -- same operations, same order, same expectations,
// with the boxed spellings (`record_get_literal("appendChild")`,
// `record_get_literal("__gea_node_id")`) replaced by the native ones
// (`NodeHandle::appendChild`, `NodeHandle::id`) that mean the same thing.
//
// WHAT IT COVERS. "Rare data" is the side table the engine allocates lazily,
// per node, only for nodes that need more than the hot `Node` record holds. The
// pattern that depends on it is the keyed-list ROW: a handler bound on the row
// NODE itself, with no `containsNode` guard, relying on `dispatchEvent`'s
// tree-walk and bubbling, and reclaimed when the row node is removed. So:
//
//   * per-node listener storage and target->ancestor bubbling ORDER,
//   * `stopPropagation` actually keeping the event off the parent,
//   * block RECLAMATION on `removeNode` (a leak here is a stale handler firing
//     against a node id the tree has since handed to somebody else),
//   * attribute set/get/has/remove through the same block, including the
//     empty-node read that must not allocate one,
//   * `setPressId` -> dispatch -> `event.pressId`, the path the virtual
//     keyboard relies on (`ui/virtual_keyboard.cpp` sets a pressId per key).
#include "native_test_harness.h"

#include "display.h"
#include "ui/document.h"
#include "ui/internal.h"
#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <string>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {

constexpr const char *kTestName = "test_gea_engine_rare_data";

// The probe keeps the original's numbered return codes (30-38) rather than
// renumbering from 1: the numbers are what its failure messages were reported
// as, and preserving them keeps any older log or note that cites one readable.
int probe()
{
	using namespace gea::embedded::ui;
	auto &document = Document::instance();
	auto &tree = Tree::instance();

	auto perParent = document.createElement("div");
	auto perChild = document.createElement("span");
	perParent.appendChild(perChild);
	const int perParentId = perParent.id();
	const int perChildId = perChild.id();
	if (perParentId < 0 || perChildId < 0 || tree.node(perChildId).parent != perParentId) {
		std::fprintf(stderr, "[%s] per-node listener setup failed\n", kTestName);
		return 30;
	}

	int seq = 0, childOrder = -1, parentOrder = -1;
	tree.setEventListener(perChildId, "click", [&](gea::framework::events::PointerEvent &) { childOrder = seq++; });
	tree.setEventListener(perParentId, "click", [&](gea::framework::events::PointerEvent &) { parentOrder = seq++; });
	if (!tree.hasEventListener(perChildId) || !tree.hasEventListener(perParentId)) {
		std::fprintf(stderr, "[%s] per-node listeners not stored in rare-data pool\n", kTestName);
		return 31;
	}
	{
		gea::framework::events::PointerEvent ev{};
		ev.type = gea::framework::events::PointerEventType::Click;
		ev.targetId = perChildId;
		tree.dispatchEvent(ev);
	}
	if (childOrder != 0 || parentOrder != 1) {
		std::fprintf(stderr, "[%s] per-node bubbling wrong (child=%d parent=%d)\n", kTestName, childOrder, parentOrder);
		return 32;
	}

	// stopPropagation on a fresh child must keep the event off its parent.
	auto stopParent = document.createElement("div");
	auto stopChild = document.createElement("span");
	stopParent.appendChild(stopChild);
	const int stopParentId = stopParent.id();
	const int stopChildId = stopChild.id();
	int stopChildHits = 0, stopParentHits = 0;
	tree.setEventListener(stopChildId, "click", [&](gea::framework::events::PointerEvent &e) {
		stopChildHits++;
		e.stopPropagation();
	});
	tree.setEventListener(stopParentId, "click", [&](gea::framework::events::PointerEvent &) { stopParentHits++; });
	{
		gea::framework::events::PointerEvent ev{};
		ev.type = gea::framework::events::PointerEventType::Click;
		ev.targetId = stopChildId;
		tree.dispatchEvent(ev);
	}
	if (stopChildHits != 1 || stopParentHits != 0) {
		std::fprintf(stderr, "[%s] stopPropagation leaked to parent (child=%d parent=%d)\n",
		             kTestName, stopChildHits, stopParentHits);
		return 33;
	}
	int nextChildHits = 0;
	tree.setEventListener(stopChildId, "click", [&](gea::framework::events::PointerEvent &) { nextChildHits++; });
	{
		gea::framework::events::PointerEvent ev{};
		ev.type = gea::framework::events::PointerEventType::Click;
		ev.targetId = stopChildId;
		tree.dispatchEvent(ev);
	}
	if (stopChildHits != 2 || nextChildHits != 1 || stopParentHits != 0) {
		std::fprintf(stderr, "[%s] stopPropagation skipped a target listener or reached parent\n", kTestName);
		return 39;
	}
	int parentScrollHits = 0;
	tree.setEventListener(perParentId, "scroll", [&](gea::framework::events::PointerEvent &) { parentScrollHits++; });
	{
		gea::framework::events::PointerEvent ev{};
		ev.type = gea::framework::events::PointerEventType::Scroll;
		ev.targetId = perChildId;
		ev.bubbles = false;
		if (tree.dispatchEvent(ev) || parentScrollHits != 0) {
			std::fprintf(stderr, "[%s] non-bubbling scroll reached parent without target listener\n", kTestName);
			return 40;
		}
	}

	// Removing a node must reclaim its rare-data block (no leak, no stale fire).
	tree.removeNode(perChildId);
	if (tree.hasEventListener(perChildId)) {
		std::fprintf(stderr, "[%s] removed node still reports a listener (rare-data block leaked)\n", kTestName);
		return 34;
	}

	// Attribute storage through the rare-data block: set/get/has/remove, the
	// empty-node case (no block allocated), and the pressId path the keyboard
	// relies on (setPressId -> dispatch -> event.pressId).
	auto attrParent = document.createElement("div");
	auto attrChild = document.createElement("span");
	attrParent.appendChild(attrChild);
	const int attrParentId = attrParent.id();
	const int attrChildId = attrChild.id();
	tree.setAttribute(attrChildId, "data-foo", "bar");
	if (!tree.hasAttribute(attrChildId, "data-foo") ||
	    std::string(tree.getAttribute(attrChildId, "data-foo")) != "bar") {
		std::fprintf(stderr, "[%s] attribute set/get via rare-data failed\n", kTestName);
		return 35;
	}
	if (tree.hasAttribute(attrParentId, "data-foo") ||
	    std::string(tree.getAttribute(attrParentId, "data-foo")) != "") {
		std::fprintf(stderr, "[%s] empty-node attribute read should be empty (no block allocated)\n", kTestName);
		return 36;
	}
	tree.removeAttribute(attrChildId, "data-foo");
	if (tree.hasAttribute(attrChildId, "data-foo")) {
		std::fprintf(stderr, "[%s] attribute remove via rare-data failed\n", kTestName);
		return 37;
	}

	tree.setPressId(attrChildId, 4242);
	int seenPressId = -1;
	tree.setEventListener(attrChildId, "click", [&](gea::framework::events::PointerEvent &e) { seenPressId = e.pressId; });
	{
		gea::framework::events::PointerEvent ev{};
		ev.type = gea::framework::events::PointerEventType::Click;
		ev.targetId = attrChildId;
		tree.dispatchEvent(ev);
	}
	if (seenPressId != 4242) {
		std::fprintf(stderr, "[%s] pressId via rare-data not delivered (got %d)\n", kTestName, seenPressId);
		return 38;
	}

	return 0;
}

}  // namespace

int main()
{
	gea::embedded::test::resetNativeHost();
	const int result = probe();
	if (result != 0) {
		std::fprintf(stderr, "[%s] probe failed with %d\n", kTestName, result);
		gea::embedded::test::dumpTree(kTestName);
		return result;
	}
	std::fprintf(stdout, "[%s] PASS\n", kTestName);
	return 0;
}
