// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "display.h"
#include "document.h"
#include "refresh_perf.h"
#include "tree_state.h"

namespace gea::embedded::ui {

Tree &Tree::instance()
{
	static Tree tree;
	return tree;
}

void Tree::markDisplayListDirty()
{
	refreshPerfStatsMutable().treeMarkDisplayListDirtyCalls++;
	auto &state = treeState();
	state.displayListDirty = true;
	// Structural: the pending rebuild may change draw order / which commands
	// exist, so refresh must repaint the whole viewport (see displayListRebuildStructural).
	state.displayListRebuildStructural = true;
}

void Tree::markDisplayListContentDirty()
{
	refreshPerfStatsMutable().treeMarkDisplayListContentDirtyCalls++;
	// Appearance-only rebuild on existing nodes (no draw-order change): request
	// the rebuild but leave displayListRebuildStructural alone, so refresh can
	// replay only the dirty-node regions. A structural change elsewhere in the
	// same frame still sets the flag and forces the full-viewport repaint.
	treeState().displayListDirty = true;
}

void Tree::markNodeDisplayCommandsDirty(int node)
{
	if (node < 0 || node >= kMaxNodes) return;
	refreshPerfStatsMutable().treeMarkNodeCommandDirtyCalls++;
	auto &state = treeState();
	state.nodeCommandDirty[node] = 1;
	const int root = state.mountedRoot;
	if (root >= 0 && root < state.nodeCount && node < state.nodeCount && isDocumentCanvasRoot(state.nodes[root]) &&
	    (node == root || (state.nodes[node].parent == root && std::string_view(tagFromId(state.nodes[node].tag_id)) == "body"))) {
		// The affected canvas pixels extend beyond the root/body layout boxes.
		// A body mutation can also move background ownership to or from html.
		markDisplayListDirty();
	}
}

void Tree::markScrollDirty(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= kMaxNodes) return;
	state.nodes[node].render.scroll_dirty = 1;
	state.scrollDirtyNodes[node / 64] |= (1ull << (node % 64));
	state.scrollDirtyAny = true;
}

Node *Tree::nodes()
{
	return treeState().nodes;
}

const Node *Tree::nodes() const
{
	return treeState().nodes;
}

Node &Tree::node(int id)
{
	return treeState().nodes[id];
}

const Node &Tree::node(int id) const
{
	return treeState().nodes[id];
}

int Tree::nodeCount() const { return treeState().nodeCount; }
int Tree::mountedRoot() const { return treeState().mountedRoot; }
int Tree::mountedWidth() const { return treeState().mountedWidth; }
int Tree::mountedHeight() const { return treeState().mountedHeight; }

bool Tree::displayListRebuildRequired() const { return treeState().displayListDirty; }
bool Tree::refreshRequired() const
{
	const auto &state = treeState();
	int reason = 0;
	if (state.displayListDirty) {
		reason = 1;
	} else if (state.scrollDirtyAny) {
		reason = 2;
	}
	if (!reason) {
		for (int i = 0; i < state.nodeCount; i++) {
			if (state.nodes[i].render.dirty) {
				reason = 4;
				break;
			}
			if (state.nodeCommandDirty[i]) {
				reason = 5;
				break;
			}
		}
	}
#if !defined(GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT) || !GEA_EMBEDDED_DIRECT_CANVAS_CONTEXT
	if (!reason && !Document::directCanvasContextUsed()) {
		const int root = state.mountedRoot;
		const bool displayBackedCanvasRoot =
			root >= 0 &&
			root < state.nodeCount &&
			state.nodes[root].type == NodeType::Canvas &&
			isDisplayBackedCanvas(root);
		if (!displayBackedCanvasRoot) {
			if (auto *canvas = gea::platform::display::Display::canvas()) {
				if (canvas->dirty(nullptr, nullptr, nullptr, nullptr)) reason = 3;
			}
		}
	}
#endif
	if (!reason) return false;
	return true;
}

uint64_t Tree::refreshSerial() const { return treeState().refreshSerial; }

void Tree::setDisplayListRebuildRequired(bool required) { treeState().displayListDirty = required; }
bool Tree::nodeDisplayCommandsDirty(int node) const
{
	auto &state = treeState();
	return node >= 0 && node < kMaxNodes && state.nodeCommandDirty[node] != 0;
}

void Tree::clearNodeDisplayCommandDirty(int node)
{
	if (node >= 0 && node < kMaxNodes) treeState().nodeCommandDirty[node] = 0;
}

int Tree::lastFrameMs() const { return treeState().lastFrameMs; }
void Tree::setLastFrameMs(int timestampMs) { treeState().lastFrameMs = timestampMs; }

}  // namespace gea::embedded::ui
