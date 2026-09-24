// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "css_atom.h"
#include "events.h"
#include "node_model.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace gea::embedded::ui {

inline constexpr int kMaxNodeAttributes = 8;
inline constexpr int kNodeAttributeNameMax = 32;
inline constexpr int kNodeAttributeValueMax = 64;

struct NodeAttribute {
	char name[kNodeAttributeNameMax]{};
	char value[kNodeAttributeValueMax]{};
};

struct NodeAttributeStore {
	int16_t pressId = -1;
	int16_t pressValue = -1;
	CssAtomId idAtom = kInvalidCssAtom;
	uint8_t count = 0;
	NodeAttribute values[kMaxNodeAttributes]{};

	void clear();
	void set(const char *name, const char *value);
	bool remove(const char *name);
	const char *get(const char *name) const;
	bool has(const char *name) const;
};

using gea::framework::events::EventListenerId;
using gea::framework::events::kInvalidEventListenerId;

struct NodeEventListenerEntry {
	EventListenerId id = kInvalidEventListenerId;
	std::shared_ptr<gea::framework::events::EventListener> listener;
};

struct NodeEventListenerList {
	std::vector<NodeEventListenerEntry> entries;

	bool empty() const { return entries.empty(); }
};

struct NodeEventListeners {
	NodeEventListenerList click;
	NodeEventListenerList touchstart;
	NodeEventListenerList touchmove;
	NodeEventListenerList touchend;
	NodeEventListenerList input;
	NodeEventListenerList keydown;
	NodeEventListenerList scroll;

	void clear();
	NodeEventListenerList *listenersFor(const char *type);
	const NodeEventListenerList *listenersFor(const char *type) const;
	bool hasAny() const;
};

// NOTE: the NodeRareData block (which bundles attributes + listeners + other
// cold per-node state) is defined in tree_state.h, after the remaining
// cold-state structs (NodeCustomPropertyStore, VirtualListNodeState) it embeds.

}  // namespace gea::embedded::ui
