// SPDX-License-Identifier: Apache-2.0
#include "tree_events.h"

#include "audio.h"
#include "internal.h"
#include "tree_internal.h"
#include "tree_state.h"

#include <algorithm>
#include <new>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

extern "C" double gea_host_image_load_asset_path(const char *path);

namespace gea::embedded::ui {

namespace {

void copyBounded(char *dest, int capacity, const char *value)
{
	if (capacity <= 0) return;
	if (!value) value = "";
	std::snprintf(dest, static_cast<std::size_t>(capacity), "%s", value);
}

bool sameName(const char *a, const char *b)
{
	return a && b && std::strcmp(a, b) == 0;
}

bool audioBooleanAttributeEnabled(const char *value)
{
	if (!value) return false;
	if (value[0] == '\0') return true;
	return std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0;
}

void applyAudioAttribute(Tree &tree, int node, const char *name, const char *value)
{
	if (tree.node(node).type != NodeType::Audio) return;
	const bool srcChanged = sameName(name, "src");
	const bool autoplayChanged = sameName(name, "autoplay");
	if (!srcChanged && !autoplayChanged) return;
	const char *src = srcChanged ? value : tree.getAttribute(node, "src");
	const bool hasAutoplay = autoplayChanged || tree.hasAttribute(node, "autoplay");
	const char *autoplay = autoplayChanged ? value : (hasAutoplay ? tree.getAttribute(node, "autoplay") : nullptr);
	if (!src || src[0] == '\0' || !hasAutoplay || !audioBooleanAttributeEnabled(autoplay)) return;
	gea::platform::audio::AudioSystem::playFile(src);
}

// Per-event-type count of nodes that currently have a listener for that type.
// Lets the dispatcher skip building the event payload and walking the tree for
// a type nobody listens to (e.g. `touchmove` when an app binds only `click`).
// Index order mirrors NodeEventListeners' slots and is kept in sync by
// setEventListener (on the empty->set transition) and NodeEventListeners::clear()
// (which every removal path calls).
constexpr int kEventTypeCount = 7;
#if GEA_UI_NODE_LISTENERS
int g_listenerTypeCounts[kEventTypeCount] = {};
EventListenerId g_nextEventListenerId = 1;
#endif

int eventTypeIndex(const char *type)
{
	if (sameName(type, "click")) return 0;
	// Pointer events are the cross-platform input API (the web build binds the
	// same onPointerDown/Move/Up handlers, where the browser raises them for both
	// mouse and touch). On device the controller only synthesizes touch*, so a
	// pointer* listener shares its touch* sibling's slot and is fired by the same
	// touch dispatch — otherwise addEventListener() would drop it as an unknown
	// type and the handler would never run (maps' pan/zoom did exactly that).
	if (sameName(type, "touchstart") || sameName(type, "pointerdown")) return 1;
	if (sameName(type, "touchmove") || sameName(type, "pointermove")) return 2;
	if (sameName(type, "touchend") || sameName(type, "pointerup")) return 3;
	if (sameName(type, "input")) return 4;
	if (sameName(type, "keydown")) return 5;
	if (sameName(type, "scroll")) return 6;
	return -1;
}

// Document-level (off-tree) event handlers. `rotary` (encoder dial) belongs to
// the document, not to any element — it is never stored per-node. `keydown`
// bound on a non-input element is treated the same way (hardware buttons /
// app-level shortcuts); keydown on an <input> stays a per-node listener.
// Dispatched directly by the runtime (dispatch_rotary_input / dispatch_key_input),
// never by the tree walk. Reset on app switch via resetDocumentEventListeners().
gea::framework::events::EventListener g_documentRotaryListener;
gea::framework::events::EventListener g_documentKeyDownListener;

void chainDocumentListener(gea::framework::events::EventListener &slot,
                           gea::framework::events::EventListener listener)
{
	if (!listener) return;
	if (static_cast<bool>(slot)) {
		slot = [previous = std::move(slot), next = std::move(listener)](
		           gea::framework::events::PointerEvent &event) mutable {
			previous(event);
			if (event.propagationStopped) return;
			next(event);
		};
		return;
	}
	slot = std::move(listener);
}

}  // namespace

void resetDocumentEventListeners()
{
	g_documentRotaryListener = {};
	g_documentKeyDownListener = {};
}

// Register a document-level handler. Only "rotary" and "keydown" are
// document-native; anything else is ignored (those route through per-node
// setEventListener instead). Called by Document::addEventListener.
void setDocumentEventListener(const char *type, gea::framework::events::EventListener listener)
{
	if (!type || !listener) return;
	if (sameName(type, "rotary")) {
		chainDocumentListener(g_documentRotaryListener, std::move(listener));
		return;
	}
	if (sameName(type, "keydown")) {
		chainDocumentListener(g_documentKeyDownListener, std::move(listener));
	}
}

// Fire the document-level rotary handler with a fresh Rotary event carrying the
// encoder delta. No tree walk, no node target.
void dispatchDocumentRotary(int delta)
{
	if (!static_cast<bool>(g_documentRotaryListener)) return;
	gea::framework::events::PointerEvent event{};
	event.type = gea::framework::events::PointerEventType::Rotary;
	event.delta = delta;
	event.bubbles = true;
	event.cancelable = true;
	g_documentRotaryListener(event);
}

// Fire the document-level keydown handler (hardware buttons / app-level
// shortcuts bound on a non-input element). Returns true if a handler ran.
bool dispatchDocumentKeyDown(gea::framework::events::PointerEvent &event)
{
	if (!static_cast<bool>(g_documentKeyDownListener)) return false;
	g_documentKeyDownListener(event);
	return true;
}

// ---- NodeRareData pool ------------------------------------------------------
//
// Cold/optional per-node state (currently event listeners) lives here instead
// of a dense [kMaxNodes] array. A node holds an int16 handle (Node::rare_data,
// -1 = none) into this pool; a block is allocated lazily the first time a node
// needs one and returned to a free list when the node is reset. Owned records
// keep their addresses STABLE while the pointer table grows: the dispatcher
// retains NodeRareData* across re-entrant handlers. A direct pointer table also
// avoids deque's block arithmetic changing when record compaction crosses its
// block-size thresholds. The free list keeps allocations bounded and reused.
namespace {
std::vector<std::unique_ptr<NodeRareData>> g_rareDataPool;
std::vector<int16_t> g_rareDataFreeList;
}  // namespace

NodeAuxiliaryStorageUsage nodeAuxiliaryStorageUsage()
{
    NodeAuxiliaryStorageUsage result;
    result.tree = treeStorageUsage();
    result.text = NodeText::storageUsage();
    result.dependencies = styleDependencyStorageUsage();
    auto &usage = result.rare;
    usage.staticBytes = sizeof(g_rareDataPool) + sizeof(g_rareDataFreeList) + sizeof(g_documentKeyDownListener) + sizeof(g_documentRotaryListener);
#if GEA_UI_NODE_LISTENERS
    usage.staticBytes += sizeof(g_listenerTypeCounts) + sizeof(g_nextEventListenerId);
#endif
    if (g_documentKeyDownListener) ++usage.untrackedOwners;
    if (g_documentRotaryListener) ++usage.untrackedOwners;
    usage.addVector(g_rareDataPool);
    usage.addVector(g_rareDataFreeList);
    for (const auto &record : g_rareDataPool) {
        usage.addAllocation(record.get(), sizeof(NodeRareData));
#if GEA_UI_NODE_LISTENERS
        usage.addVector(record->listeners.entries);
        for (const auto &listener : record->listeners.entries)
            if (listener.listener.use_count()) ++usage.untrackedOwners;
#endif
#if GEA_UI_NODE_ATTRIBUTES
        for (auto *entry = record->attributes.values.get(); entry; entry = entry->next.get())
            usage.addAllocation(entry, sizeof(NodeAttributeEntry) + entry->nameBytes + entry->valueCapacity);
#endif
#if GEA_CSS_CUSTOM_PROPERTIES
        for (const auto *properties : {&record->customProperties, &record->inlineCustomProperties}) {
            usage.addVector(properties->values);
            for (const auto &property : properties->values)
                if (property.value.use_count()) ++usage.untrackedOwners;
        }
#endif
        result.overrides += record->defaultStyles.storageUsage();
        result.overrides += record->inlineStyles.storageUsage();
#if GEA_CSS_GRID
        if (record->gridLayout) {
            usage.addAllocation(record->gridLayout.get(), sizeof(GridTrackLayout));
            usage.addVector(record->gridLayout->columnStart); usage.addVector(record->gridLayout->columnEnd);
            usage.addVector(record->gridLayout->rowStart); usage.addVector(record->gridLayout->rowEnd);
        }
#endif
#if GEA_CSS_SCROLLING
        usage.addAllocation(record->virtualList.get(), sizeof(VirtualListNodeState));
#endif
    }
    return result;
}

NodeRareData &ensureRareData(int node)
{
	auto &state = treeState();
	int16_t handle = state.nodes[node].rare_data;
	if (handle >= 0) return *g_rareDataPool[static_cast<std::size_t>(handle)];
	if (!g_rareDataFreeList.empty()) {
		handle = g_rareDataFreeList.back();
		g_rareDataFreeList.pop_back();
	} else {
		handle = static_cast<int16_t>(g_rareDataPool.size());
		g_rareDataPool.emplace_back(std::make_unique<NodeRareData>());
	}
	state.nodes[node].rare_data = handle;
	return *g_rareDataPool[static_cast<std::size_t>(handle)];
}

NodeRareData *rareDataFor(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= kMaxNodes) return nullptr;
	const int16_t handle = state.nodes[node].rare_data;
	return handle >= 0 ? g_rareDataPool[static_cast<std::size_t>(handle)].get() : nullptr;
}

void releaseRareData(int node)
{
	auto &state = treeState();
	if (node < 0 || node >= kMaxNodes) return;
	const int16_t handle = state.nodes[node].rare_data;
	if (handle < 0) return;
	g_rareDataPool[static_cast<std::size_t>(handle)]->clear();
	g_rareDataFreeList.push_back(handle);
	state.nodes[node].rare_data = -1;
}

void NodeAttributeDeleter::operator()(NodeAttributeEntry *entry) const
{
	entry->~NodeAttributeEntry();
	::operator delete(entry);
}
NodeAttributePtr NodeAttributeEntry::create(const char *name, const char *value)
{
	if (!value) value = "";
	const auto nameBytes = std::min(std::strlen(name), std::size_t(kNodeAttributeNameMax - 1)) + 1;
	const auto valueBytes = std::min(std::strlen(value), std::size_t(kNodeAttributeValueMax - 1)) + 1;
	auto *entry = new (::operator new(sizeof(NodeAttributeEntry) + nameBytes + valueBytes)) NodeAttributeEntry;
	entry->nameBytes = nameBytes;
	entry->valueCapacity = valueBytes;
	copyBounded(reinterpret_cast<char *>(entry + 1), nameBytes, name);
	copyBounded(entry->value(), valueBytes, value);
	return NodeAttributePtr(entry);
}

#if GEA_UI_NODE_ATTRIBUTES
NodeAttributeStore::NodeAttributeStore(const NodeAttributeStore &other)
    : pressId(other.pressId), pressValue(other.pressValue), idAtom(other.idAtom), count(other.count)
{
	auto *tail = &values;
	for (auto *entry = other.values.get(); entry; entry = entry->next.get()) {
		*tail = NodeAttributeEntry::create(entry->name(), entry->value());
		tail = &(*tail)->next;
	}
}

NodeAttributeStore &NodeAttributeStore::operator=(const NodeAttributeStore &other)
{
	if (this != &other) {
		NodeAttributeStore copy(other);
		*this = std::move(copy);
	}
	return *this;
}

NodeAttributeStore::NodeAttributeStore(NodeAttributeStore &&other) noexcept
{
	*this = std::move(other);
}

NodeAttributeStore &NodeAttributeStore::operator=(NodeAttributeStore &&other) noexcept
{
	if (this == &other) return *this;
	values = std::move(other.values);
	pressId = other.pressId;
	pressValue = other.pressValue;
	idAtom = other.idAtom;
	count = other.count;
	other.clear();
	return *this;
}

void NodeAttributeStore::clear()
{
	pressId = -1;
	pressValue = -1;
	idAtom = kInvalidCssAtom;
	count = 0;
	values.reset();
}

void NodeAttributeStore::set(const char *name, const char *value)
{
	if (!name || !*name) return;
	if (!value) value = "";
	const bool isId = sameName(name, "id");
	auto *tail = &values;
	while (*tail) {
		auto &entry = **tail;
		if (sameName(entry.name(), name)) {
			const auto bytes = std::min(std::strlen(value), std::size_t(kNodeAttributeValueMax - 1)) + 1;
			if (bytes > entry.valueCapacity) {
				// Allocate/copy first: value may point into the existing entry.
				auto replacement = NodeAttributeEntry::create(name, value);
				replacement->next = std::move(entry.next);
				*tail = std::move(replacement);
			} else {
				std::memmove(entry.value(), value, bytes - 1);
				entry.value()[bytes - 1] = 0;
			}
			if (isId) idAtom = internCssAtom((*tail)->value());
			return;
		}
		tail = &entry.next;
	}
	if (count >= kMaxNodeAttributes) return;
	auto entry = NodeAttributeEntry::create(name, value);
	if (isId) idAtom = internCssAtom(entry->value());
	*tail = std::move(entry);
	++count;
}

bool NodeAttributeStore::remove(const char *name)
{
	if (!name) return false;
	for (auto *link = &values; *link; link = &(*link)->next) {
		if (!sameName((*link)->name(), name)) continue;
		if (sameName(name, "id")) idAtom = kInvalidCssAtom;
		// Keep the removed owner alive while moving its next link.
		auto removed = std::move(*link);
		*link = std::move(removed->next);
		count--;
		return true;
	}
	return false;
}

const char *NodeAttributeStore::get(const char *name) const
{
	if (!name) return "";
	for (auto *entry = values.get(); entry; entry = entry->next.get()) {
		if (sameName(entry->name(), name)) return entry->value();
	}
	return "";
}

bool NodeAttributeStore::has(const char *name) const
{
	if (!name) return false;
	for (auto *entry = values.get(); entry; entry = entry->next.get()) {
		if (sameName(entry->name(), name)) return true;
	}
	return false;
}

#else
void NodeAttributeStore::set(const char *name, const char *)
{
	if (name && *name) std::abort(); // A source proof must never discard a live attribute.
}
#endif

void NodeEventListeners::clear()
{
#if GEA_UI_NODE_LISTENERS
	for (int type = 0; type < 7; ++type)
		if (hasType(type) && g_listenerTypeCounts[type] > 0) --g_listenerTypeCounts[type];
	entries.clear();
	types = 0;
#else

#endif
}

EventListenerId Tree::setEventListener(int node, const char *type, gea::framework::events::EventListener listener)
{
#if GEA_UI_NODE_LISTENERS
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return kInvalidEventListenerId;
	if (!listener) return kInvalidEventListenerId;
	// Don't allocate a rare-data block for an unrecognized event type (the set
	// listenerFor knows == the set eventTypeIndex tracks).
	if (eventTypeIndex(type) < 0) return kInvalidEventListenerId;
	auto &list = ensureRareData(node).listeners;
	const int typeIndex = eventTypeIndex(type);
	if (!list.hasType(typeIndex)) {
		++g_listenerTypeCounts[typeIndex];
		list.types |= 1u << typeIndex;
	}
	EventListenerId id = g_nextEventListenerId++;
	if (id == kInvalidEventListenerId) id = g_nextEventListenerId++;
	list.entries.push_back(NodeEventListenerEntry{
		id, static_cast<std::uint8_t>(typeIndex),
		std::make_shared<gea::framework::events::EventListener>(std::move(listener))});
	return id;
#else
	if (node >= 0 && node < treeState().nodeCount && listener && eventTypeIndex(type) >= 0) std::abort();
	return kInvalidEventListenerId;
#endif
}

bool Tree::removeEventListener(int node, const char *type, EventListenerId listenerId)
{
#if GEA_UI_NODE_LISTENERS
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || listenerId == kInvalidEventListenerId) return false;
	NodeRareData *rd = rareDataFor(node);
	if (!rd) return false;
	auto &list = rd->listeners;
	const int typeIndex = eventTypeIndex(type);
	if (!list.hasType(typeIndex)) return false;
	for (auto it = list.entries.begin(); it != list.entries.end(); ++it) {
		if (it->id != listenerId || it->type != typeIndex) continue;
		list.entries.erase(it);
		const bool remains = std::any_of(list.entries.begin(), list.entries.end(),
		    [typeIndex](const auto &entry) { return entry.type == typeIndex; });
		if (!remains) {
			list.types &= ~(1u << typeIndex);
			if (g_listenerTypeCounts[typeIndex] > 0) --g_listenerTypeCounts[typeIndex];
		}
		return true;
	}
	return false;
#else
	return false;
#endif
}

bool Tree::hasEventListener(int node) const
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	const NodeRareData *rd = rareDataFor(node);
	return rd && rd->listeners.hasAny();
}

bool Tree::hasListenersForType(const char *type) const
{
#if GEA_UI_NODE_LISTENERS
	const int index = eventTypeIndex(type);
	// Unknown/untracked types fall through to dispatch — never silently drop an
	// event we don't account for.
	return index < 0 || g_listenerTypeCounts[index] > 0;
#else
	return false;
#endif
}

bool Tree::dispatchEvent(gea::framework::events::PointerEvent &event)
{
#if GEA_UI_NODE_LISTENERS
	auto &state = treeState();
	if (event.targetId < 0 || event.targetId >= state.nodeCount) return false;

	event.target = gea::framework::events::EventTarget(event.targetId);
	bool dispatched = false;
	for (int node = event.targetId; node >= 0 && node < state.nodeCount; node = state.nodes[node].parent) {
		if (!event.bubbles && node != event.targetId) break;
		NodeRareData *rd = rareDataFor(node);
		if (!rd) continue;
		const int typeIndex = eventTypeIndex(event.typeName());
		if (!rd->listeners.hasType(typeIndex)) continue;

		event.currentTargetId = node;
		event.currentTarget = gea::framework::events::EventTarget(node);
		event.eventPhase = node == event.targetId ? gea::framework::events::EventPhase::AtTarget
		                                           : gea::framework::events::EventPhase::Bubbling;
		// pressId/pressValue ride on the event from the first ancestor that has one
		// (the listener-bearing node here, `rd`), else the original target's. Both
		// now live in the rare-data block, so read them through it.
		const NodeRareData *targetRd = rareDataFor(event.targetId);
		const int currentPressId = rd->attributes.pressId;
		const int currentPressValue = rd->attributes.pressValue;
		event.pressId = currentPressId >= 0 ? currentPressId : (targetRd ? targetRd->attributes.pressId : -1);
		event.pressValue = currentPressValue >= 0 ? currentPressValue : (targetRd ? targetRd->attributes.pressValue : -1);
		// A delegated handler may dispose its own conditional branch, removing
		// listeners from this same body list during dispatch. The shared callback
		// keeps the in-flight function alive after its entry is erased. Reacquire the
		// list after every callback and advance by monotonic listener id; listeners
		// added during this dispatch are above maxId and do not receive this event.
		const EventListenerId maxId = rd->listeners.entries.back().id;
		EventListenerId cursor = kInvalidEventListenerId;
		for (;;) {
			rd = rareDataFor(node);
			if (!rd) break;
			if (!rd->listeners.hasType(typeIndex)) break;
			EventListenerId nextId = kInvalidEventListenerId;
			std::shared_ptr<gea::framework::events::EventListener> callback;
			for (const auto &entry : rd->listeners.entries) {
				if (entry.type != typeIndex) continue;
				if (entry.id <= cursor || entry.id > maxId) continue;
				nextId = entry.id;
				callback = entry.listener;
				break;
			}
			if (!callback) break;
			cursor = nextId;
			(*callback)(event);
			dispatched = true;
		}
		if (event.propagationStopped || !event.bubbles) break;
	}
	return dispatched;
#else
	return false;
#endif
}

bool Tree::containsNode(int ancestor, int node) const
{
	const auto &state = treeState();
	bool result = false;
	if (ancestor >= 0 && ancestor < state.nodeCount && node >= 0 && node < state.nodeCount) {
		for (int current = node; current >= 0 && current < state.nodeCount; current = state.nodes[current].parent) {
			if (current == ancestor) {
				result = true;
				break;
			}
		}
	}

	return result;
}

namespace {

// True when changing this (node, attribute) pair actually affects what
// pixels get painted, so the framework can skip dirty-region invalidation
// for the common case (data-* attributes, internal bookkeeping, ARIA
// labels, etc.) and only repaint when the change really matters.
//
// Currently the only "renderable attribute" in the framework is on
// `<input>`: InputRenderer reads `value` and `placeholder` straight from
// the attribute store to paint the text. Other tags use the style /
// className / setText paths for visual updates, which already mark
// their nodes dirty in the right spot. Add new entries here if you wire
// up another tag where an attribute name drives rendering directly.
bool attributeAffectsRendering(const Node &node, const char *attrName)
{
	if (std::strcmp(tagFromId(node.tag_id), "input") == 0 &&
	    (sameName(attrName, "value") || sameName(attrName, "placeholder"))) {
		return true;
	}
	// A runtime `src`/`fit` change on an <img> loads a new image id (see the
	// Image branch of setAttribute above) but paints nothing unless the node is
	// invalidated: the first render drew the original src, so a later reactive /
	// imperative src swap (e.g. an EPUB cover extracted to the SD card and
	// applied via setAttribute) needs an explicit repaint to reach the panel.
	if (node.type == NodeType::Image && (sameName(attrName, "src") || sameName(attrName, "fit"))) {
		return true;
	}
	return false;
}

int imageFitAttributeValue(const char *value)
{
	if (!value) return 0;
	if (sameName(value, "contain")) return 1;
	if (sameName(value, "cover")) return 2;
	if (sameName(value, "none")) return 3;
	if (sameName(value, "scale-down")) return 4;
	return 0;
}

}  // namespace

void Tree::setAttribute(int node, const char *name, const char *value)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	if (sameName(name, "class")) {
		setClassName(node, value ? value : "");
		return;
	}
	ensureRareData(node).attributes.set(name, value);
	if (state.nodes[node].type == NodeType::Image && sameName(name, "src")) {
		const int imageId = static_cast<int>(gea_host_image_load_asset_path(value ? value : ""));
		if (imageId >= 0) {
			setStyleFromClass(node, Property::ImageId, imageId);
			// A runtime src swap makes the image node emit a blit command it
			// didn't have before (its initial record, with no image, produced
			// nothing) — a structural change to the display list. Mark it
			// structural so refresh does a full recordNode() rebuild that records
			// the new blit, not a partial per-node re-record that keeps the empty
			// command range. Without this a reactively/imperatively loaded image
			// (e.g. an EPUB cover applied via setAttribute) never paints.
			markDisplayListDirty();
			state.nodes[node].render.dirty = 1;
#if GEA_CSS_SCROLLING
			state.nodes[node].render.non_scroll_dirty = 1;
#endif
		}
	}
	if (state.nodes[node].type == NodeType::Image && sameName(name, "fit")) {
		Style(node).imageFit(imageFitAttributeValue(value));
	}
	applyAudioAttribute(*this, node, name, value ? value : "");
	if (state.nodes[node].type == NodeType::VirtualList) VirtualListRenderer::configureAttribute(node, name, value);
	// Tree::setText marks render.dirty + non_scroll_dirty so the
	// dirty-region tracker pushes the new pixels to the panel. We do
	// the same here for the small set of attributes that drive
	// per-node painting (see attributeAffectsRendering above) so
	// reactive bindings like `value={todo.draft}` actually repaint the
	// input. Other attribute writes — data-*, ARIA, internal flags —
	// stay free.
	if (attributeAffectsRendering(state.nodes[node], name)) {
		markNodeDisplayCommandsDirty(node);
		state.displayListDirty = true;
		state.nodes[node].render.dirty = 1;
		state.nodes[node].render.layout_dirty = 1;
#if GEA_CSS_SCROLLING
		state.nodes[node].render.non_scroll_dirty = 1;
#endif
	}
}

void Tree::removeAttribute(int node, const char *name)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || !name) return;
	if (sameName(name, "class")) {
		setClassName(node, "");
		return;
	}
	NodeRareData *rd = rareDataFor(node);
	if (!rd) return;  // no rare-data block → no attributes to remove
	const bool hadAttribute = rd->attributes.remove(name);
	if (!hadAttribute) return;
#if GEA_UI_NODE_ATTRIBUTES
	if (sameName(name, "data-press-id")) rd->attributes.pressId = -1;
	if (sameName(name, "data-press-value")) rd->attributes.pressValue = -1;
#endif
	if (attributeAffectsRendering(state.nodes[node], name)) {
		markNodeDisplayCommandsDirty(node);
		state.displayListDirty = true;
		state.nodes[node].render.dirty = 1;
		state.nodes[node].render.layout_dirty = 1;
#if GEA_CSS_SCROLLING
		state.nodes[node].render.non_scroll_dirty = 1;
#endif
	}
}

bool Tree::toggleAttribute(int node, const char *name, bool force)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount || !name || name[0] == '\0') return false;
	if (force) {
		setAttribute(node, name, "");
		return true;
	}
	removeAttribute(node, name);
	return false;
}

const char *Tree::getAttribute(int node, const char *name) const
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return "";
	// `class` is not stored in the attribute table (that would force a rare-data
	// block onto every styled node); it lives in the dense NodeClassList. Serve
	// reads from there via a stable static buffer (the runtime is single-threaded
	// and getAttribute's pointer is consumed before the next call, like the DOM).
	if (sameName(name, "class")) {
		static std::string classBuf;
		classBuf = className(node);
		return classBuf.c_str();
	}
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->attributes.get(name) : "";
}

bool Tree::hasAttribute(int node, const char *name) const
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return false;
	if (sameName(name, "class")) return !className(node).empty();
	const NodeRareData *rd = rareDataFor(node);
	return rd && rd->attributes.has(name);
}

void Tree::setPressId(int node, int pressId)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	auto &attrs = ensureRareData(node).attributes;
#if GEA_UI_NODE_ATTRIBUTES
	attrs.pressId = static_cast<int16_t>(pressId);
#endif
	char value[16];
	std::snprintf(value, sizeof(value), "%d", pressId);
	attrs.set("data-press-id", value);
}

int Tree::pressId(int node) const
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return -1;
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->attributes.pressId : -1;
}

void Tree::setPressValue(int node, int pressValue)
{
	auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return;
	auto &attrs = ensureRareData(node).attributes;
#if GEA_UI_NODE_ATTRIBUTES
	attrs.pressValue = static_cast<int16_t>(pressValue);
#endif
	char value[16];
	std::snprintf(value, sizeof(value), "%d", pressValue);
	attrs.set("data-press-value", value);
}

int Tree::pressValue(int node) const
{
	const auto &state = treeState();
	if (node < 0 || node >= state.nodeCount) return -1;
	const NodeRareData *rd = rareDataFor(node);
	return rd ? rd->attributes.pressValue : -1;
}

}  // namespace gea::embedded::ui
