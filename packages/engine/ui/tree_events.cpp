// SPDX-License-Identifier: Apache-2.0
#include "tree_events.h"

#include "audio.h"
#include <image.h>
#include "internal.h"
#include "tree_internal.h"
#include "tree_state.h"
#include <stdexcept>

#include <algorithm>
#include <new>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

extern "C" double gea_host_image_acquire_asset_path(const char *path, bool *owned);

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
constexpr int kEventTypeCount = 12;
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
	if (sameName(type, "change")) return 9;
 if (sameName(type,"focusin")) return 10;
 if (sameName(type,"focusout")) return 11;
	if (sameName(type, "keydown")) return 5;
	if (sameName(type, "scroll")) return 6;
	if (sameName(type, "keyup")) return 7;
	if (sameName(type, "pointercancel") || sameName(type, "touchcancel")) return 8;
	return -1;
}

}  // namespace

void setDocumentEventListener(const char *type, gea::framework::events::EventListener listener)
{
 if (!listener) return;
 // Legacy C++ registrations have no externally removable callback identity.
 // Retain their distinct callable allocation for the whole registration.
 auto held = std::make_shared<gea::framework::events::EventListener>(std::move(listener));
 addDocumentEventListener(type, held.get(), [held](auto &event) { (*held)(event); });
}
void dispatchDocumentRotary(int delta)
{
 gea::framework::events::PointerEvent event{};
 event.type = gea::framework::events::PointerEventType::Rotary;
 event.delta = delta;
 event.bubbles = true;
 event.cancelable = true;
 dispatchWindowEvent(event, true);
 if (!event.propagationStopped) dispatchDocumentEvent(event, true);
 if (!event.propagationStopped) dispatchDocumentEvent(event, false);
 if (!event.propagationStopped) dispatchWindowEvent(event, false);
}
bool dispatchDocumentKeyDown(gea::framework::events::PointerEvent &event)
{ return dispatchDocumentEvent(event, false); }
bool dispatchDocumentKeyUp(gea::framework::events::PointerEvent &event)
{ return dispatchDocumentEvent(event, false); }

namespace {
struct SubscriberTypeScope {
 gea::framework::events::PointerEvent &event;
 std::string previous;
 SubscriberTypeScope(gea::framework::events::PointerEvent &event, const std::string &name)
  : event(event), previous(std::move(event.subscriberTypeName)) { event.subscriberTypeName = name; }
 ~SubscriberTypeScope() { event.subscriberTypeName = std::move(previous); }
};
struct WindowListenerEntry {
 std::uint64_t id;
 std::string type;
 const void *identity;
 bool capture;
 bool once;
 std::shared_ptr<gea::framework::events::EventListener> listener;
};
// UI-owner-thread realm identity fences callbacks still on the stack when
// Tree.clear retires both global targets. Neither target is a fabricated node.
bool g_nativeEventRealmRetired = false;
std::uint64_t g_rootEventRealm = 1;
std::uint64_t g_activeListenerRealm = 0;
struct ListenerRealmScope {
 std::uint64_t previous;
 explicit ListenerRealmScope(std::uint64_t realm): previous(g_activeListenerRealm) { g_activeListenerRealm = realm; }
 ~ListenerRealmScope() { g_activeListenerRealm = previous; }
};
bool staleListenerRealm() { return g_activeListenerRealm != 0 && g_activeListenerRealm != g_rootEventRealm; }
std::vector<WindowListenerEntry> g_documentListeners;
std::uint64_t g_nextDocumentListenerId = 1;
bool g_resettingDocumentListeners = false;
bool supportedDocumentType(const char *type) {
 return eventTypeIndex(type) >= 0 || sameName(type, "rotary");
}
std::vector<WindowListenerEntry> g_windowListeners;
std::uint64_t g_nextWindowListenerId = 1;
bool g_resettingWindowListeners = false;
std::string windowTypeKey(const char *type) {
 const int index = eventTypeIndex(type);
 return index >= 0 ? std::to_string(index) : std::string(type ? type : "");
}
std::vector<gea::framework::events::EventTarget> captureEventPath(Tree &tree, int target) {
 std::vector<gea::framework::events::EventTarget> path;
 for (int node = target, left = tree.nodeCount(); left > 0 && node >= 0 && node < tree.nodeCount(); --left) {
  gea::framework::events::EventTarget owner(node);
  if (!owner.valid()) break;
  path.push_back(owner);
  node = tree.node(node).parent;
 }
 return path;
}
bool supportedWindowType(const char *type) {
 return eventTypeIndex(type) >= 0 || sameName(type, "rotary") || sameName(type, "blur") || sameName(type, "resize");
}
}

bool addWindowEventListener(const char *type, const void *identity,
                           gea::framework::events::EventListener listener,
                           bool capture, bool once) {
 if (g_nativeEventRealmRetired || g_resettingWindowListeners || staleListenerRealm() || !supportedWindowType(type) || !identity || !listener) return false;
 const std::string key(type);
 for (const auto &entry : g_windowListeners)
  if (entry.type == key && entry.identity == identity && entry.capture == capture) return true;
 if (g_windowListeners.size() >= 256 || g_nextWindowListenerId == 0) return false;
 g_windowListeners.push_back({g_nextWindowListenerId++, key, identity, capture, once,
   std::make_shared<gea::framework::events::EventListener>(std::move(listener))});
 return true;
}
bool removeWindowEventListener(const char *type, const void *identity, bool capture) {
 if (g_nativeEventRealmRetired || staleListenerRealm() || !type || !identity) return false;
 const std::string key(type);
 for (auto it = g_windowListeners.begin(); it != g_windowListeners.end(); ++it) {
  if (it->type == key && it->identity == identity && it->capture == capture) {
   auto held = std::move(it->listener);
   g_windowListeners.erase(it);
   return true;
  }
 }
 return false;
}
bool windowEventListenersRetiring() { return g_nativeEventRealmRetired || g_resettingWindowListeners || staleListenerRealm(); }
void resetWindowEventListeners() {
 // Retire the registry before releasing captures: their destructors may reenter.
 if (g_resettingWindowListeners) return;
 g_resettingWindowListeners = true;
 { std::vector<WindowListenerEntry> retired; retired.swap(g_windowListeners); }
 g_resettingWindowListeners = false;
}
bool dispatchWindowEvent(gea::framework::events::PointerEvent &event, bool capture) {
 if (g_nativeEventRealmRetired || staleListenerRealm()) return false;
 const auto maximum = g_nextWindowListenerId - 1;
 const auto dispatchRealm = g_rootEventRealm;
 std::uint64_t cursor = 0;
 bool dispatched = false;
 struct Restore {
  gea::framework::events::PointerEvent &event;
  gea::framework::events::EventTarget target;
  int id;
  gea::framework::events::EventPhase phase;
  ~Restore() { event.currentTarget = target; event.currentTargetId = id; event.eventPhase = phase; }
 } restore{event, event.currentTarget, event.currentTargetId, event.eventPhase};
 const auto key = windowTypeKey(event.typeName());
 event.currentTarget = gea::framework::events::EventTarget();
 event.currentTargetId = -1;
 event.eventPhase = capture ? gea::framework::events::EventPhase::Capturing
                            : gea::framework::events::EventPhase::Bubbling;
 for (;;) {
  if (dispatchRealm != g_rootEventRealm) break;
  ListenerRealmScope realm(dispatchRealm);
  std::shared_ptr<gea::framework::events::EventListener> callback;
  std::string subscribedType;
  for (auto it = g_windowListeners.begin(); it != g_windowListeners.end(); ++it) {
   if (it->id <= cursor || it->id > maximum || it->capture != capture || windowTypeKey(it->type.c_str()) != key) continue;
   cursor = it->id;
   callback = it->listener;
   subscribedType = it->type;
   if (it->once) g_windowListeners.erase(it); // Before invocation, including reentrant dispatch.
   break;
  }
  if (!callback) break;
  SubscriberTypeScope projection(event, subscribedType);
  (*callback)(event);
  dispatched = true;
 }
 return dispatched;
}

bool addDocumentEventListener(const char *type, const void *identity,
                           gea::framework::events::EventListener listener,
                           bool capture, bool once) {
 if (g_nativeEventRealmRetired || g_resettingDocumentListeners || staleListenerRealm() || !supportedDocumentType(type) || !identity || !listener) return false;
 const std::string key(type);
 for (const auto &entry : g_documentListeners)
  if (entry.type == key && entry.identity == identity && entry.capture == capture) return true;
 if (g_documentListeners.size() >= 256 || g_nextDocumentListenerId == 0) return false;
 g_documentListeners.push_back({g_nextDocumentListenerId++, key, identity, capture, once,
   std::make_shared<gea::framework::events::EventListener>(std::move(listener))});
 return true;
}
bool removeDocumentEventListener(const char *type, const void *identity, bool capture) {
 if (g_nativeEventRealmRetired || staleListenerRealm() || !type || !identity) return false;
 const std::string key(type);
 for (auto it = g_documentListeners.begin(); it != g_documentListeners.end(); ++it) {
  if (it->type == key && it->identity == identity && it->capture == capture) {
   auto held = std::move(it->listener);
   g_documentListeners.erase(it);
   return true;
  }
 }
 return false;
}
bool documentEventListenersRetiring() { return g_nativeEventRealmRetired || g_resettingDocumentListeners || staleListenerRealm(); }
void resetDocumentEventListeners() {
 // Retire the registry before releasing captures: their destructors may reenter.
 if (g_resettingDocumentListeners) return;
 g_resettingDocumentListeners = true;
 { std::vector<WindowListenerEntry> retired; retired.swap(g_documentListeners); }
 g_resettingDocumentListeners = false;
}
bool dispatchDocumentEvent(gea::framework::events::PointerEvent &event, bool capture) {
 if (g_nativeEventRealmRetired || staleListenerRealm()) return false;
 const auto maximum = g_nextDocumentListenerId - 1;
 const auto dispatchRealm = g_rootEventRealm;
 std::uint64_t cursor = 0;
 bool dispatched = false;
 struct Restore {
  gea::framework::events::PointerEvent &event;
  gea::framework::events::EventTarget target;
  int id;
  gea::framework::events::EventPhase phase;
  ~Restore() { event.currentTarget = target; event.currentTargetId = id; event.eventPhase = phase; }
 } restore{event, event.currentTarget, event.currentTargetId, event.eventPhase};
 const auto key = windowTypeKey(event.typeName());
 event.currentTarget = gea::framework::events::EventTarget();
 event.currentTargetId = -1;
 event.eventPhase = capture ? gea::framework::events::EventPhase::Capturing
                            : gea::framework::events::EventPhase::Bubbling;
 for (;;) {
  if (dispatchRealm != g_rootEventRealm) break;
  ListenerRealmScope realm(dispatchRealm);
  std::shared_ptr<gea::framework::events::EventListener> callback;
  std::string subscribedType;
  for (auto it = g_documentListeners.begin(); it != g_documentListeners.end(); ++it) {
   if (it->id <= cursor || it->id > maximum || it->capture != capture || windowTypeKey(it->type.c_str()) != key) continue;
   cursor = it->id;
   callback = it->listener;
   subscribedType = it->type;
   if (it->once) g_documentListeners.erase(it); // Before invocation, including reentrant dispatch.
   break;
  }
  if (!callback) break;
  SubscriberTypeScope projection(event, subscribedType);
  (*callback)(event);
  dispatched = true;
 }
 return dispatched;
}

void resetRootEventListeners() noexcept {
 if (g_resettingDocumentListeners || g_resettingWindowListeners) return;
 g_resettingDocumentListeners = g_resettingWindowListeners = true;
 if (++g_rootEventRealm == 0) std::abort();
 {
  std::vector<WindowListenerEntry> document, window;
  document.swap(g_documentListeners);
  window.swap(g_windowListeners);
  // Both admission guards remain closed while either set releases captures.
 }
 g_resettingDocumentListeners = g_resettingWindowListeners = false;
}

void retireNativeEventRealm() noexcept {
 if (g_nativeEventRealmRetired) return;
 g_nativeEventRealmRetired = true;
 resetRootEventListeners();
#if GEA_UI_NODE_LISTENERS
 // Move every node capture away before destruction: destructors may reenter
 // Tree APIs, but terminal admission/dispatch is already permanently closed.
 for (auto &count:g_listenerTypeCounts) count=0;
 for (int node=0;node<treeState().nodeCount;++node) {
  // Fresh lookup each iteration: capture destruction may clear or grow Tree.
  // No allocation and no cached rare-data reference survives capture release.
  std::vector<NodeEventListenerEntry> retired;
  {
   auto *rare=rareDataFor(node);
   if (!rare) continue;
   retired.swap(rare->listeners.entries);
   rare->listeners.types=0;
  }
 }
#endif
}

bool dispatchWindowLifecycleEvent(gea::framework::events::PointerEventType type) {
 using gea::framework::events::PointerEventType;
 if (type != PointerEventType::WindowBlur && type != PointerEventType::WindowResize) return false;
 gea::framework::events::PointerEvent event{};
 event.type = type;
 event.bubbles = false;
 event.cancelable = false;
 bool dispatched = dispatchWindowEvent(event, true);
 // Both registrations are on the actual lifecycle target; stopPropagation
 // does not suppress other listeners on that same target.
 return dispatchWindowEvent(event, false) || dispatched;
}

bool dispatchRootKeyEvent(gea::framework::events::PointerEvent &event, bool legacyDocumentFirst) {
 using gea::framework::events::PointerEventType;
 if (event.type != PointerEventType::KeyDown && event.type != PointerEventType::KeyUp) return false;
 auto &tree = Tree::instance();
 // With no focused input a hardware key belongs to the app: target its top
 // element (the mount container's first child), so the event bubbles through
 // the app root -- where apps bind onKeyDown -- and then the container. Aiming
 // at the container itself skipped every app-level key handler.
 if (event.targetId < 0) {
  const int root = tree.mountedRoot();
  const int app = tree.firstChildOf(root);
  event.targetId = app >= 0 ? app : root;
 }
 event.target = gea::framework::events::EventTarget(event.targetId);
 const auto path = captureEventPath(tree, event.targetId);
 bool dispatched = dispatchWindowEvent(event, true);
 if (!event.propagationStopped) dispatched = dispatchDocumentEvent(event, true) || dispatched;
 const auto document = [&]() {
  return event.type == PointerEventType::KeyUp ? dispatchDocumentKeyUp(event) : dispatchDocumentKeyDown(event);
 };
 if (event.targetId >= 0 && !event.target.valid()) return dispatched;
 if (!event.propagationStopped) {
  if (legacyDocumentFirst) {
   const bool present = document();
   dispatched = present || dispatched;
   if (!present && !event.propagationStopped) dispatched = tree.dispatchEventAlongPath(event, path) || dispatched;
  } else {
   dispatched = tree.dispatchEventAlongPath(event, path) || dispatched;
   if (!event.propagationStopped) dispatched = document() || dispatched;
  }
 }
 if (!event.propagationStopped && event.bubbles) dispatched = dispatchWindowEvent(event, false) || dispatched;
 return dispatched;
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
    usage.staticBytes = sizeof(g_rareDataPool) + sizeof(g_rareDataFreeList) + sizeof(g_documentListeners) + sizeof(g_nextDocumentListenerId) + sizeof(g_resettingDocumentListeners) + sizeof(g_windowListeners) + sizeof(g_nextWindowListenerId) + sizeof(g_resettingWindowListeners) + sizeof(g_rootEventRealm) + sizeof(g_activeListenerRealm) + sizeof(g_nativeEventRealmRetired);
#if GEA_UI_NODE_LISTENERS
    usage.staticBytes += sizeof(g_listenerTypeCounts) + sizeof(g_nextEventListenerId);
#endif
    usage.addVector(g_documentListeners);
    usage.addVector(g_windowListeners);
    usage.untrackedOwners += g_documentListeners.size() + g_windowListeners.size();
    usage.addVector(g_rareDataPool);
    usage.addVector(g_rareDataFreeList);
    for (const auto &record : g_rareDataPool) {
        usage.addAllocation(record.get(), sizeof(NodeRareData));
        // Shared token/control-block allocation is not exposed by shared_ptr.
        if (record->childNodeListIdentity) ++usage.untrackedOwners;
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
	auto &record = *g_rareDataPool[static_cast<std::size_t>(handle)];
#if GEA_UI_NODE_LISTENERS
	std::vector<NodeEventListenerEntry> retired;
	retired.swap(record.listeners.entries);
#endif
	// Publish the detached, reusable record before any capture destructor runs.
	state.nodes[node].rare_data = -1;
	record.clear();
	g_rareDataFreeList.push_back(handle);
}

// Called only after Tree::clear has published its entire empty structural state.
// Retire all node/global registration ownership before releasing any captures.
void resetTreeEventStorage()
{
	std::vector<std::unique_ptr<NodeRareData>> retired;
	std::vector<int16_t> retiredFreeList;
	retired.swap(g_rareDataPool);
	retiredFreeList.swap(g_rareDataFreeList);
#if GEA_UI_NODE_LISTENERS
	for (auto &count : g_listenerTypeCounts) count = 0;
	for (const auto &record : retired) if (record) record->listeners.types = 0;
#endif
	// This swaps BOTH global registries before either releases captures. Any
	// successor Tree created by those destructors uses the already-new pool.
	resetRootEventListeners();
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
	for (int type = 0; type < kEventTypeCount; ++type)
		if (hasType(type) && g_listenerTypeCounts[type] > 0) --g_listenerTypeCounts[type];
	entries.clear();
	types = 0;
#else

#endif
}

EventListenerId Tree::setEventListener(int node, const char *type, gea::framework::events::EventListener listener)
{
 if (g_nativeEventRealmRetired) return kInvalidEventListenerId;
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
		id, static_cast<std::uint8_t>(typeIndex), std::string(type),
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
		if (it->id != listenerId || it->type != typeIndex || it->subscribedType != type) continue;
		// Detach physical callback ownership before vector mutation. Its final
		// destructor may register/remove handlers and invalidate this storage.
		auto retired = std::move(it->listener);
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
 if (g_nativeEventRealmRetired) return false;
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
 if (g_nativeEventRealmRetired) return false;
 return dispatchEventAlongPath(event, captureEventPath(*this, event.targetId));
}

bool Tree::dispatchEventAlongPath(gea::framework::events::PointerEvent &event,
    const std::vector<gea::framework::events::EventTarget> &path)
{
 if (g_nativeEventRealmRetired) return false;
#if GEA_UI_NODE_LISTENERS
	auto &state = treeState();
	if (event.targetId < 0 || event.targetId >= state.nodeCount) return false;

 if (path.empty() || !path.front().valid() || path.front().id() != event.targetId) return false;
 struct RestoreCurrentTarget {
  gea::framework::events::PointerEvent &event;
  gea::framework::events::EventTarget target;
  int id;
  gea::framework::events::EventPhase phase;
  ~RestoreCurrentTarget() {
   event.currentTarget = target;
   event.currentTargetId = id;
   event.eventPhase = phase;
  }
 } restore{event, event.currentTarget, event.currentTargetId, event.eventPhase};
 event.target = path.front();
 const NodeRareData *originalTarget = rareDataFor(event.targetId);
 const int targetPressId = originalTarget ? originalTarget->attributes.pressId : -1;
 const int targetPressValue = originalTarget ? originalTarget->attributes.pressValue : -1;
 const bool windowPhases = event.type != gea::framework::events::PointerEventType::KeyDown &&
                           event.type != gea::framework::events::PointerEventType::KeyUp;
 bool dispatched = windowPhases && dispatchWindowEvent(event, true);
 if (event.propagationStopped || !event.target.valid()) return dispatched;
 if (windowPhases) dispatched = dispatchDocumentEvent(event, true) || dispatched;
 if (event.propagationStopped || !event.target.valid()) return dispatched;
	for (const auto &owner : path) {
  if (g_nativeEventRealmRetired) break;
  if (!owner.valid()) continue;
  const int node = owner.id();
		if (!event.bubbles && node != event.targetId) break;
		NodeRareData *rd = rareDataFor(node);
		if (!rd) continue;
		const int typeIndex = eventTypeIndex(event.typeName());
		if (!rd->listeners.hasType(typeIndex)) continue;

		event.currentTargetId = node;
		event.currentTarget = owner;
		event.eventPhase = node == event.targetId ? gea::framework::events::EventPhase::AtTarget
		                                           : gea::framework::events::EventPhase::Bubbling;
		// pressId/pressValue ride on the event from the first ancestor that has one
		// (the listener-bearing node here, `rd`), else the original target's. Both
		// now live in the rare-data block, so read them through it.
		const int currentPressId = rd->attributes.pressId;
		const int currentPressValue = rd->attributes.pressValue;
		event.pressId = currentPressId >= 0 ? currentPressId : targetPressId;
		event.pressValue = currentPressValue >= 0 ? currentPressValue : targetPressValue;
		// A delegated handler may dispose its own conditional branch, removing
		// listeners from this same body list during dispatch. The shared callback
		// keeps the in-flight function alive after its entry is erased. Reacquire the
		// list after every callback and advance by monotonic listener id; listeners
		// added during this dispatch are above maxId and do not receive this event.
		const EventListenerId maxId = rd->listeners.entries.back().id;
		EventListenerId cursor = kInvalidEventListenerId;
		for (;;) {
   if (g_nativeEventRealmRetired || !owner.valid()) break;
			rd = rareDataFor(node);
			if (!rd) break;
			if (!rd->listeners.hasType(typeIndex)) break;
			EventListenerId nextId = kInvalidEventListenerId;
			std::shared_ptr<gea::framework::events::EventListener> callback;
   std::string subscribedType;
			for (const auto &entry : rd->listeners.entries) {
				if (entry.type != typeIndex) continue;
				if (entry.id <= cursor || entry.id > maxId) continue;
				nextId = entry.id;
				callback = entry.listener;
    subscribedType = entry.subscribedType;
				break;
			}
			if (!callback) break;
			cursor = nextId;
   SubscriberTypeScope projection(event, subscribedType);
			(*callback)(event);
			dispatched = true;
		}
		if (event.propagationStopped || !event.bubbles) break;
	}
 if (windowPhases && event.bubbles && !event.propagationStopped)
  dispatched = dispatchDocumentEvent(event, false) || dispatched;
 if (windowPhases && event.bubbles && !event.propagationStopped)
  dispatched = dispatchWindowEvent(event, false) || dispatched;
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
		const auto &style = state.nodes[node].computedStyle();
		const bool fixedBox = (style.width != kUnset || style.width_percent != kUnset) &&
		                      (style.height != kUnset || style.height_percent != kUnset);
		bool hadBlit = false;
		for (int i = 0; i < DisplayList::instance().nodeCommandCount(node); ++i) {
			const auto *command = DisplayList::instance().nodeCommandAt(node, i);
			if (command && (command->type == DisplayCommandType::BlitImage ||
				                command->type == DisplayCommandType::BlitImageScaled ||
				                command->type == DisplayCommandType::BlitImageProjected))
				hadBlit = true;
		}
		bool owned = false;
		const int imageId = static_cast<int>(gea_host_image_acquire_asset_path(value ? value : "", &owned));
		{
			setStyleFromClass(node, Property::ImageId, imageId);
			if (owned) gea::framework::graphics::ImageStore::instance().release(imageId);
			if (fixedBox) {
				// ImageId already owns fixed-box paint invalidation. Replacing
				// an existing blit changes this image, not the scene's structure.
				// A previously empty range needs a fresh recording, but still
				// only damages this fixed box; siblings need no full repaint.
				if (!hadBlit) markDisplayListContentDirty();
				// Mark even if a runtime file reused the same decoded slot id.
				markNodeDisplayCommandsDirty(node);
				state.nodes[node].render.dirty = 1;
#if GEA_CSS_SCROLLING
				state.nodes[node].render.non_scroll_dirty = 1;
#endif
				return;
			}
			// An intrinsic-size image can change surrounding layout and may
			// introduce its first blit. Keep the full layout/recording fallback
			// for this case so newly loaded images and displaced siblings paint.
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
	if (state.nodes[node].type == NodeType::Image && sameName(name, "src")) {
		setStyleFromClass(node, Property::ImageId, -1);
		markDisplayListDirty();
	}
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
