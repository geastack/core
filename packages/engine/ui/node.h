// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/dom_rect.h"
#include "events.h"
#include "style.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gea::embedded::ui {

class ChildNodeList;

namespace DocumentPosition {
inline constexpr int disconnected = 1;
inline constexpr int preceding = 2;
inline constexpr int following = 4;
inline constexpr int contains = 8;
inline constexpr int containedBy = 16;
inline constexpr int implementationSpecific = 32;
}

class ClassList {
public:
	explicit ClassList(int nodeId = -1)
	  : nodeId_(nodeId), identity_(gea::framework::identity::node(nodeId)) {}

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
	bool valid() const { return gea::framework::identity::current(nodeId_, identity_); }
	int nodeId_;
	gea::framework::identity::Owner identity_;
};

class NodeHandle {
protected:
	int id_;
	gea::framework::identity::Owner identity_;

public:
	// DOM Node.nodeType, resolved from the backing tree node. geatsc types a
	// compiled component template as `Node` (not a dynamic value), so the
	// runtime's `CompiledStaticComponent::render` element/fragment check
	// (`node.nodeType === 1` / `=== 11`) lowers to a typed member read on this
	// handle rather than a dynamic record lookup — the handle has to answer it.
	// Structural comments/fragments report 8/11 and never become visual boxes.
	// A retired handle reports 0, distinct from a live empty fragment.
	// Resolved lazily on read (cheap, rare) rather than at every handle
	// construction (hot path). Inside a handle the property borrows the
	// handle's identity token instead of owning a second reference: handles
	// are copied constantly, and each shared_ptr copy is an atomic
	// read-modify-write (a critical section on targets without hardware
	// atomics). The handle rebuilds it on copy and move. A property copied
	// out of its handle takes its own owner, so it can outlive the handle.
	class NodeTypeProperty {
	public:
		operator int() const;
		bool operator==(int other) const { return static_cast<int>(*this) == other; }
		bool operator!=(int other) const { return static_cast<int>(*this) != other; }

		// A standalone copy owns the node's current token when it is still
		// the same allocation; a retired one stays retired (reports 0).
		NodeTypeProperty(const NodeTypeProperty &other) : id_(other.id_), bits_(0) { adopt(other.token()); }
		NodeTypeProperty &operator=(const NodeTypeProperty &other)
		{
			if (this != &other) {
				const auto *token = other.token();
				release();
				id_ = other.id_;
				adopt(token);
			}
			return *this;
		}
		~NodeTypeProperty() { release(); }

	private:
		friend class NodeHandle;
		using Token = gea::framework::identity::Token;
		using Owner = gea::framework::identity::Owner;
		NodeTypeProperty(int id, const Token *token) : id_(id), bits_(reinterpret_cast<std::uintptr_t>(token)) {}
		const Token *token() const
		{
			if (bits_ & 1) return reinterpret_cast<const Owner *>(bits_ & ~std::uintptr_t{1})->get();
			return reinterpret_cast<const Token *>(bits_);
		}
		void borrow(int id, const Token *token)
		{
			release();
			id_ = id;
			bits_ = reinterpret_cast<std::uintptr_t>(token);
		}
		void release()
		{
			if (bits_ & 1) delete reinterpret_cast<Owner *>(bits_ & ~std::uintptr_t{1});
			bits_ = 0;
		}
		void adopt(const Token *token)
		{
			Owner owner = gea::framework::identity::node(id_);
			bits_ = token && owner.get() == token ? reinterpret_cast<std::uintptr_t>(new Owner(std::move(owner))) | 1 : 0;
		}
		int id_;
		// The handle's borrowed Token*, or (low bit set) a standalone copy's own
		// heap Owner*: the handle stays as small as one pointer here.
		std::uintptr_t bits_;
	};

	// A real default constructor (not a defaulted-arg explicit ctor): records
	// holding a NodeHandle field aggregate-initialize with `{}`, which is
	// copy-list-initialization — that's not allowed to call an explicit
	// constructor, so `explicit NodeHandle(int = -1)` alone breaks every
	// generated `__gea_type_X __gea_out{};`.
	NodeHandle() : NodeHandle(-1) {}
	explicit NodeHandle(int id) : id_(id), identity_(gea::framework::identity::node(id)), nodeType{id, identity_.get()} {}
	// A handle for an owner already in hand (a Style's, another handle's),
	// without a registry lookup. Validity is still checked on use.
	NodeHandle(int id, gea::framework::identity::Owner owner)
	  : id_(id), identity_(std::move(owner)), nodeType{id, identity_.get()} {}
	// Copies and moves rebuild nodeType from this handle's own owner.
	NodeHandle(const NodeHandle &other) : id_(other.id_), identity_(other.identity_), nodeType{other.id_, identity_.get()} {}
	NodeHandle(NodeHandle &&other) noexcept
	  : id_(other.id_), identity_(std::move(other.identity_)), nodeType{other.id_, identity_.get()}
	{
		other.nodeType.borrow(other.id_, nullptr);
	}
	NodeHandle &operator=(const NodeHandle &other)
	{
		if (this != &other) {
			id_ = other.id_;
			identity_ = other.identity_;
			nodeType.borrow(id_, identity_.get());
		}
		return *this;
	}
	NodeHandle &operator=(NodeHandle &&other) noexcept
	{
		if (this != &other) {
			id_ = other.id_;
			identity_ = std::move(other.identity_);
			nodeType.borrow(id_, identity_.get());
			other.nodeType.borrow(other.id_, nullptr);
		}
		return *this;
	}
	const void *identity() const { return identity_.get(); }
	std::shared_ptr<const void> weakIdentityOwner() const { return identity_; }
	std::shared_ptr<void> propertyStorage(const std::shared_ptr<void> &initial = {}) const
	{
		return gea::framework::identity::nodePropertyStorage(id_, identity_, initial);
	}
	// Event targets and DOM elements are two views of the same tree identity.
	// Pointer/touch event targets declared as Element therefore project to a
	// full node handle without boxing or losing the source node id.
	NodeHandle(const gea::framework::events::EventTarget &target)
	  : id_(target.id()), identity_(target.identityOwner()), nodeType{target.id(), identity_.get()} {}
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

	int id() const { return valid() ? id_ : -1; }
	bool valid() const { return gea::framework::identity::current(id_, identity_); }
	// Current allocation is in the explicitly mounted retained-tree subtree.
	bool isConnected() const;
	bool disabled() const;
	void setDisabled(bool value) const;
	static const char *selectorText(const std::string &s);
	bool matches(const char *selector) const;
	NodeHandle closest(const char *selector) const;
	NodeHandle querySelector(const char *selector) const;
	std::vector<NodeHandle> querySelectorAll(const char *selector) const;
	bool matches(const std::string &s) const { return matches(selectorText(s)); }
	NodeHandle closest(const std::string &s) const { return closest(selectorText(s)); }
	NodeHandle querySelector(const std::string &s) const { return querySelector(selectorText(s)); }
	std::vector<NodeHandle> querySelectorAll(const std::string &s) const { return querySelectorAll(selectorText(s)); }
	explicit operator bool() const { return valid(); }
	// Shares this handle's owner instead of looking the id up again; the
	// Style validates it on every use, as it validated its own lookup.
	Style style() const { return Style(id_, identity_); }
	// `style().setNumberProperty(...)` without the Style: it would take a
	// second owner of this handle's identity just to forward the write.
	bool setStyleNumberProperty(const std::string &property, double value) const;
	ClassList classList() const { return ClassList(id()); }

	NodeTypeProperty nodeType;

private:
	friend class Style;
	struct Borrowed {};
	// Shares `owner`'s token without a reference count, for a handle that
	// lives only inside one call while the caller (a Style) holds `owner`.
	// Copies of it do not keep the token alive, so it is never stored.
	NodeHandle(int id, const gea::framework::identity::Owner &owner, Borrowed)
	  : id_(id), identity_(gea::framework::identity::Owner{}, owner.get()), nodeType{id, identity_.get()} {}

public:

	NodeHandle cloneNode(bool deep = true) const;
	// Named *Handle to avoid colliding with the DOM `firstChild` PROPERTY
	// lowering (gea_ir::domFirstChild), which geatsc resolves on Node-typed
	// receivers; a same-named member function would shadow it.
	NodeHandle firstChildHandle() const;
	ChildNodeList childNodesHandle() const;
	NodeHandle nextSiblingHandle() const;
	NodeHandle previousSiblingHandle() const;
	int compareDocumentPosition(const NodeHandle &other) const;
	NodeHandle parentHandle() const;
	std::vector<NodeHandle> elementChildren() const;
	NodeHandle lastElementChildHandle() const;
	// Replace children without retiring allocations. Retained children move to
	// their final order; removed children remain live and may be reinserted.
	void replaceChildren(const std::vector<NodeHandle> &children) const;
	NodeHandle childAt(int index) const;
	void appendChild(NodeHandle child) const;
	NodeHandle insertBefore(NodeHandle child, NodeHandle reference) const;
	// A literal JS null appends through the same ownerless native sentinel.
	NodeHandle insertBefore(NodeHandle child, std::nullptr_t) const { return insertBefore(child, NodeHandle()); }
	NodeHandle removeChild(NodeHandle child) const;
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
	gea::host::DOMRect getBoundingClientRect() const;
	void scrollIntoView() const;
	// Text-input focus, the same state `Tree::setActiveInput` already holds for
	// the virtual keyboard: focusing an `<input>` is what summons the on-screen
	// keyboard and starts the caret blink, and blurring clears both. The state
	// existed and had no handle-level spelling, so an app that wanted it (maps'
	// hidden search input) had nothing typed to call.
	void focus() const;
 // Native responder focus never scrolls the Tree viewport. The supported
 // options record honors preventScroll without fabricating viewport movement.
 template<class Options> requires requires(const Options& options){options->preventScroll;}
 void focus(const Options& options) const {(void)options;focus();}
	void blur() const;
	bool play() const;
	void pause() const;
	void setTagName(const char *tagName) const;
	const char *tagName() const;
	std::string domNodeName() const;
	const std::string *characterData() const;
	void setCharacterData(const char *text) const;
	void setCharacterData(const std::string &text) const { setCharacterData(text.c_str()); }
	const std::string *namespaceURI() const;
	const std::string *namespacePrefix() const;
	void setPressId(int pressId) const;
	void setPressValue(int pressValue) const;
	gea::framework::events::EventListenerId addEventListener(const char *type, gea::framework::events::EventListener listener) const;
	bool removeEventListener(const char *type, gea::framework::events::EventListenerId listenerId) const;
};

// Reads the current native cascade each time, including after stylesheet changes.
class NativeComputedStyle {
	enum class Target { element, before, after, absent };
	NodeHandle owner_;
	Target target_ = Target::absent;
	NodeHandle selectedNode() const;
	std::string resolvedDimension(bool horizontal) const;
	NativeComputedStyle(NodeHandle owner, Target target) : owner_(owner), target_(target) {}
	friend NativeComputedStyle getComputedStyle(NodeHandle, std::string_view);
public:
	NativeComputedStyle() = default;
	explicit operator bool() const { return owner_.identity() != nullptr; }
	std::string display() const;
	std::string position() const;
	std::string overflow() const;
	std::string borderRadius() const;
	std::string filter() const;
	std::string width() const;
	std::string height() const;
	std::string paddingTop() const;
	std::string paddingBottom() const;
};

NativeComputedStyle getComputedStyle(NodeHandle node, std::string_view pseudoElement = {});

// Platform focus owns the actual responder transition. Pixel targets use the
// unhandled default; native targets must explicitly accept or refuse.
enum class NativeFocusResult { unhandled, accepted, refused };
struct NativeFocusQuery { bool handled=false; NodeHandle node; };
NativeFocusResult requestNativeFocus(NodeHandle node,bool focus);
NativeFocusQuery queryNativeFocus();

}  // namespace gea::embedded::ui
