// SPDX-License-Identifier: Apache-2.0
// A full node pool refuses new nodes with -1 and says so once on stdout
// (run-node-pool-exhausted.sh checks the message), and a freed slot is reused.
#include "native_test_harness.h"

#include "ui/node.h"
#include "ui/tree_internal.h"

#include <cstdio>
#include <cstdlib>

namespace gea::framework::app::generated {
void drainMicrotasks() {}
}  // namespace gea::framework::app::generated

namespace gea::framework::graphics::generated {
void ensureLinked() {}
}  // namespace gea::framework::graphics::generated

namespace {
void expect(bool ok, const char *what)
{
	if (ok) return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	std::exit(1);
}
}  // namespace

int main()
{
	using namespace gea::embedded::test;
	using namespace gea::embedded::ui;

	resetNativeHost();
	Tree &tree = Tree::instance();
	tree.clear();

	int created = 0;
	int last = -1;
	for (int i = 0; i < kMaxNodes; i++) {
		const int id = tree.createView();
		expect(id >= 0, "a node fits while the pool has room");
		last = id;
		created++;
	}
	expect(created == kMaxNodes, "the pool holds kMaxNodes nodes");
	expect(tree.createView() == -1, "a full pool refuses a node");
	expect(tree.createText() == -1, "a full pool refuses every node type");

	tree.removeNode(last);
	expect(tree.createView() == last, "a freed slot is reused");
	expect(tree.createView() == -1, "the pool is full again");

	std::fprintf(stderr, "node pool exhausted: ok (kMaxNodes=%d)\n", kMaxNodes);
	return 0;
}
