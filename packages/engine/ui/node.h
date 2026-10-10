// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "events.h"
#include "style.h"

#include <string>
#include <utility>
#include <vector>

namespace gea::embedded::ui {

class ClassList {
public:
	explicit ClassList(int nodeId = -1) : nodeId_(nodeId) {}

	bool add(const std::string &token) const;
	bool remove(const std::string &token) const;
	bool toggle(const std::string &token) const;
	bool toggle(const std::string &token, bool force) const;
	void set(const char *className) const;
	void set(const std::string &className) const;
	void clear() const;
	bool contains(const std::string &token) const;
	std::string value() const;

private:
	int nodeId_;
};

class NodeHandle {
protected:
	int id_;

public:
	// DOM Node.nodeType, resolved from the backing tree node. geatsc types a
	// compiled component template as `Node` (not a dynamic value), so the
	// runtime's `CompiledStaticComponent::render` element/fragment check
	// (`node.nodeType === 1` / `=== 11`) lowers to a typed member read on this
	// handle rather than a dynamic record lookup — the handle has to answer it.
	// The embedded tree has no document-fragment node kind (every tree node is
	// element-like or Text), so a handle to a real node reports 1 (element) or 3
	// (text); 11 (document fragment) only arises for an invalid handle, e.g. the
	// base `createDocumentFragment()` template no mounted component ever uses.
	// Resolved lazily on read (cheap, rare) rather than at every handle
	// construction (hot path), so it stays a thin id view.
	struct NodeTypeProperty {
		int id;
		operator int() const;
		bool operator==(int other) const { return static_cast<int>(*this) == other; }
		bool operator!=(int other) const { return static_cast<int>(*this) != other; }
	};

	// A real default constructor (not a defaulted-arg explicit ctor): records
	// holding a NodeHandle field aggregate-initialize with `{}`, which is
	// copy-list-initialization — that's not allowed to call an explicit
	// constructor, so `explicit NodeHandle(int = -1)` alone breaks every
	// generated `__gea_type_X __gea_out{};`.
	NodeHandle() : NodeHandle(-1) {}
	explicit NodeHandle(int id) : id_(id), nodeType{id} {}
	// Event targets and DOM elements are two views of the same tree identity.
	// Pointer/touch event targets declared as Element therefore project to a
	// full node handle without boxing or losing the source node id.
	NodeHandle(const gea::framework::events::EventTarget &target) : NodeHandle(target.id()) {}
	// Implicit build from a boxed node value (record carrying "__gea_node_id"),
	// so the compiled-component emitters' `NodeHandle x = <gea_cpp_value>` copy-
	// inits and NodeHandle-typed args accept a boxed node. SFINAE'd on
	// record_get_literal so it only matches the runtime value type.
	template <typename V, typename = decltype(std::declval<const V &>().record_get_literal(""))>
	NodeHandle(const V &value) : NodeHandle(__gea_native_from_value(value)) {}

	// geatsc boxed-node protocol: rebuild a typed handle from a boxed node
	// value (a record carrying "__gea_node_id"). The emitter's native-type
	// cast prefers this over `static_cast<NodeHandle>(value)`, which would
	// numerically coerce the record (NaN) into a garbage id. Templated so
	// this header stays independent of the geatsc runtime's value type; a
	// missing/non-numeric id yields the invalid handle (NaN != NaN).
	template <typename V>
	static NodeHandle __gea_native_from_value(const V &value)
	{
		const double raw = static_cast<double>(value.record_get_literal("__gea_node_id"));
		return NodeHandle(raw == raw ? static_cast<int>(raw) : -1);
	}

	// Assign a boxed node value (a record carrying "__gea_node_id") straight
	// into a typed handle. The DOM-walk emitters do `n = gea_cpp_key(domParentNode(n))`
	// where `n` is a NodeHandle and the right-hand side is a gea_cpp_value.
	// SFINAE'd on record_get_literal so it only matches the boxed value type and
	// never shadows the compiler-generated NodeHandle copy/move assignment.
	template <typename V>
	auto operator=(const V &value) -> decltype((void)value.record_get_literal(""), *this)
	{
		return *this = __gea_native_from_value(value);
	}

	int id() const { return id_; }
	bool valid() const { return id_ >= 0; }
	explicit operator bool() const { return valid(); }
	Style style() const { return Style(id_); }
	ClassList classList() const { return ClassList(id_); }

	NodeTypeProperty nodeType;

	NodeHandle cloneNode(bool deep = true) const;
	// Named *Handle to avoid colliding with the DOM `firstChild` PROPERTY
	// lowering (gea_ir::domFirstChild), which geatsc resolves on Node-typed
	// receivers; a same-named member function would shadow it.
	NodeHandle firstChildHandle() const;
	NodeHandle nextSiblingHandle() const;
	// Replace ALL children with `children` (the compiled-component keyed-list
	// reconciler's replaceChildren): detach every current child, then append the
	// new set in order.
	void replaceChildren(const std::vector<NodeHandle> &children) const;
	NodeHandle childAt(int index) const;
	void appendChild(NodeHandle child) const;
	void insertBefore(NodeHandle child, NodeHandle reference) const;
	void setParent(NodeHandle parent) const;
	void remove() const;
	void setText(const char *text) const;
	void setAttribute(const char *name, const char *value) const;
	void removeAttribute(const char *name) const;
	// std::string overloads: geatsc emits std::string arguments when a typed
	// NodeHandle method is called from app code (e.g. el.setAttribute('src', pathVar))
	// and does not coerce to const char*, so accept std::string directly. String
	// literals still bind the const char* overloads above (exact match preferred).
	void setAttribute(const std::string &name, const std::string &value) const { setAttribute(name.c_str(), value.c_str()); }
	void removeAttribute(const std::string &name) const { removeAttribute(name.c_str()); }
	bool toggleAttribute(const char *name, bool force) const;
	// std::string overload, matching setAttribute/removeAttribute above: the patch
	// reconciler carries a dynamic attribute name (`gea_cpp_value extra`), which
	// implicitly converts to std::string but not to const char*.
	bool toggleAttribute(const std::string &name, bool force) const { return toggleAttribute(name.c_str(), force); }
	const char *getAttribute(const char *name) const;
	bool hasAttribute(const char *name) const;
	// std::string overloads, matching set/remove/toggleAttribute above: an app
	// that reads an attribute by a computed name holds a `std::string`, which
	// binds neither `const char *` parameter.
	const char *getAttribute(const std::string &name) const { return getAttribute(name.c_str()); }
	bool hasAttribute(const std::string &name) const { return hasAttribute(name.c_str()); }
	void scrollIntoView() const;
	// Text-input focus, the same state `Tree::setActiveInput` already holds for
	// the virtual keyboard: focusing an `<input>` is what summons the on-screen
	// keyboard and starts the caret blink, and blurring clears both. The state
	// existed and had no handle-level spelling, so an app that wanted it (maps'
	// hidden search input) had nothing typed to call.
	void focus() const;
	void blur() const;
	bool play() const;
	void pause() const;
	void setTagName(const char *tagName) const;
	const char *tagName() const;
	void setPressId(int pressId) const;
	void setPressValue(int pressValue) const;
	gea::framework::events::EventListenerId addEventListener(const char *type, gea::framework::events::EventListener listener) const;
	bool removeEventListener(const char *type, gea::framework::events::EventListenerId listenerId) const;
};

}  // namespace gea::embedded::ui
