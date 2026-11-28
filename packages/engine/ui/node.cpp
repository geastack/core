#include "selector.h"
// SPDX-License-Identifier: Apache-2.0
#include "node.h"
#include "child_node_list.h"
#include "tree_state.h"
#include "internal.h"

#include "host/audio.h"
#include "tree_internal.h"
#include "input_retirement.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>
#include <stdexcept>

namespace gea::embedded::ui {

bool ClassList::add(const std::string &token) const
{
	if (!valid()) return false;
	return Tree::instance().addClass(nodeId_, token);
}

bool ClassList::remove(const std::string &token) const
{
	if (!valid()) return false;
	return Tree::instance().removeClass(nodeId_, token);
}

bool ClassList::toggle(const std::string &token) const
{
	if (!valid()) return false;
	return Tree::instance().toggleClass(nodeId_, token);
}

bool ClassList::toggle(const std::string &token, bool force) const
{
	if (!valid()) return false;
	return Tree::instance().toggleClass(nodeId_, token, force);
}

void ClassList::set(const std::string &className) const
{
	if (!valid()) return;
	Tree::instance().setClassName(nodeId_, className);
}

void ClassList::set(const char *className) const
{
	if (!valid()) return;
	Tree::instance().setClassName(nodeId_, className);
}

void ClassList::clear() const
{
	if (!valid()) return;
	Tree::instance().clearClasses(nodeId_);
}

bool ClassList::contains(const std::string &token) const
{
	if (!valid()) return false;
	return Tree::instance().hasClass(nodeId_, token);
}

std::string ClassList::value() const
{
	if (!valid()) return std::string();
	return Tree::instance().className(nodeId_);
}

NodeHandle NodeHandle::cloneNode(bool deep) const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().cloneNode(id(), deep));
}

NodeHandle NodeHandle::firstChildHandle() const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().firstChildOf(id()));
}

NodeHandle NodeHandle::nextSiblingHandle() const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().node(id()).next_sibling);
}

NodeHandle NodeHandle::previousSiblingHandle() const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().node(id()).prev_sibling);
}

int NodeHandle::compareDocumentPosition(const NodeHandle &other) const
{
	using namespace DocumentPosition;
	if (identity() == other.identity()) return 0;
	const auto disconnectedPosition = [&]() {
		const bool otherPrecedes = std::less<const void *>{}(other.identity(), identity());
		return disconnected | implementationSpecific | (otherPrecedes ? preceding : following);
	};
	if (!valid() || !other.valid()) return disconnectedPosition();

	const auto &tree = Tree::instance();
	const auto ancestors = [&](int current) {
		std::vector<int> path;
		for (; current >= 0; current = tree.node(current).parent) path.push_back(current);
		return path;
	};
	auto selfPath = ancestors(id());
	auto otherPath = ancestors(other.id());
	if (selfPath.back() != otherPath.back()) return disconnectedPosition();
	while (!selfPath.empty() && !otherPath.empty() && selfPath.back() == otherPath.back()) {
		selfPath.pop_back();
		otherPath.pop_back();
	}
	if (selfPath.empty()) return following | containedBy;
	if (otherPath.empty()) return preceding | contains;
	for (int sibling = tree.node(selfPath.back()).next_sibling; sibling >= 0; sibling = tree.node(sibling).next_sibling) {
		if (sibling == otherPath.back()) return following;
	}
	return preceding;
}

ChildNodeList NodeHandle::childNodesHandle() const
{
	if (!valid()) return ChildNodeList();
	auto &rare = ensureRareData(id());
	if (!rare.childNodeListIdentity) rare.childNodeListIdentity = std::make_shared<ChildNodeListIdentity>();
	return ChildNodeList(*this, rare.childNodeListIdentity);
}

NodeHandle NodeHandle::parentHandle() const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().node(id()).parent);
}

NodeHandle NodeHandle::lastElementChildHandle() const
{
	if (!valid()) return NodeHandle();
	auto &tree = Tree::instance();
	for (int child = tree.node(id()).last_child; child >= 0; child = tree.node(child).prev_sibling) {
		NodeHandle candidate(child);
		if (candidate.valid() && candidate.nodeType == 1) return candidate;
	}
	return NodeHandle();
}

std::vector<NodeHandle> NodeHandle::elementChildren() const
{
	std::vector<NodeHandle> result;
	if (!valid()) return result;
	auto &tree = Tree::instance();
	for (int child = tree.node(id()).first_child; child >= 0; child = tree.node(child).next_sibling) {
		NodeHandle candidate(child);
		if (candidate.valid() && candidate.nodeType == 1) result.push_back(candidate);
	}
	return result;
}

void NodeHandle::replaceChildren(const std::vector<NodeHandle> &children) const
{
	if (!valid()) return;
	auto &tree = Tree::instance();
	std::vector<int> ids;
	ids.reserve(children.size());
	for (const auto &child : children) {
		if (!child.valid()) throw std::invalid_argument("replaceChildren requires live nodes");
		ids.push_back(child.id());
	}
	tree.replaceChildren(id(), ids);
}

NodeHandle NodeHandle::childAt(int index) const
{
	if (!valid()) return NodeHandle();
	return NodeHandle(Tree::instance().childAt(id(), index));
}

void NodeHandle::appendChild(NodeHandle child) const
{
	if (!valid() || !child.valid()) return;
	Tree::instance().setParent(child.id(), id());
}

NodeHandle NodeHandle::insertBefore(NodeHandle child, NodeHandle reference) const
{
	if (!valid() || !child.valid()) return NodeHandle();
	if (!reference.valid() && reference.identity()) throw std::invalid_argument("insertBefore reference is retired");
	Tree::instance().insertBefore(child.id(), id(), reference.valid() ? reference.id() : -1);
	return child;
}

NodeHandle NodeHandle::removeChild(NodeHandle child) const
{
	if (!valid() || !child.valid() || child.parentHandle().identity() != identity())
		throw std::invalid_argument("removeChild requires a direct live child");
	Tree::instance().detachNode(child.id());
	return child;
}

void NodeHandle::setParent(NodeHandle parent) const
{
	parent.appendChild(*this);
}

void NodeHandle::remove() const
{
	if (!valid()) return;
	Tree::instance().removeNode(id());
}

void NodeHandle::setText(const char *text) const
{
	if (!valid()) return;
	Tree::instance().setText(id(), text);
}

void NodeHandle::setAttribute(const char *name, const char *value) const
{
	if (!valid()) return;
	Tree::instance().setAttribute(id(), name, value);
}

void NodeHandle::removeAttribute(const char *name) const
{
	if (!valid()) return;
	Tree::instance().removeAttribute(id(), name);
}

bool NodeHandle::toggleAttribute(const char *name, bool force) const
{
	if (!valid()) return false;
	return Tree::instance().toggleAttribute(id(), name, force);
}

const char *NodeHandle::getAttribute(const char *name) const
{
	if (!valid()) return "";
	return Tree::instance().getAttribute(id(), name);
}

bool NodeHandle::hasAttribute(const char *name) const
{
	if (!valid()) return false;
	return Tree::instance().hasAttribute(id(), name);
}

void NodeHandle::scrollIntoView() const
{
	if (!valid()) return;
	Tree::instance().scrollIntoView(id());
}

bool NodeHandle::isConnected() const
{
	if (!valid()) return false;
	const Tree &tree = Tree::instance();
	const int root = tree.mountedRoot();
	if (root < 0 || !NodeHandle(root).valid()) return false;
	// Bound the walk even if trusted raw Tree clients violate ancestry invariants.
	int current = id_;
	for (int remaining = tree.nodeCount(); remaining > 0; --remaining) {
		if (current < 0 || current >= tree.nodeCount()) return false;
		if (current == root) return true;
		current = tree.node(current).parent;
	}
	return false;
}

__attribute__((weak)) void retirePlatformInputForSubtrees(const InputRetirementRoots &,const InputRetirementRoots &) {}
__attribute__((weak)) NativeFocusResult requestNativeFocus(NodeHandle,bool){return NativeFocusResult::unhandled;}
__attribute__((weak)) NativeFocusQuery queryNativeFocus(){return {};}
void NodeHandle::focus() const
{
 if(!valid()||!isConnected()||selector::disabled(Tree::instance(),id()))return;
 if(requestNativeFocus(*this,true)==NativeFocusResult::refused)return;
 // A native responder callback may retire this allocation synchronously.
 if(valid()&&isConnected())Tree::instance().setActiveInput(id());
}

void NodeHandle::blur() const
{
 if(!valid())return;
 const auto native=queryNativeFocus();
 if(native.handled){
  if(native.node.valid()&&native.node.identity()!=identity())return;
  if(native.node.valid()&&requestNativeFocus(*this,false)==NativeFocusResult::refused)return;
 }
 if(valid()&&Tree::instance().activeInputId()==id())Tree::instance().setActiveInput(-1);
}

bool NodeHandle::play() const
{
	if (!valid() || std::strcmp(tagName(), "audio") != 0) return false;
	return gea::host::HTMLAudioElement(*this).play();
}

void NodeHandle::pause() const
{
	if (!valid() || std::strcmp(tagName(), "audio") != 0) return;
	gea::host::HTMLAudioElement(*this).pause();
}

void NodeHandle::setTagName(const char *tagName) const
{
	if (!valid()) return;
	Tree::instance().setTagName(id(), tagName);
}

const char *NodeHandle::tagName() const
{
	if (!valid()) return "";
	return Tree::instance().tagName(id());
}

NativeComputedStyle getComputedStyle(NodeHandle node, std::string_view pseudoElement)
{
	if (static_cast<int>(node.nodeType) != 1) throw std::invalid_argument("getComputedStyle requires a live element");
	using Target = NativeComputedStyle::Target;
	if (!pseudoElement.starts_with(':')) return NativeComputedStyle(node, Target::element);
	pseudoElement.remove_prefix(1);
	if (pseudoElement.starts_with(':')) pseudoElement.remove_prefix(1);
	std::string name(pseudoElement);
	for (char &ch : name)
		if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
	if (name == "before") return NativeComputedStyle(node, Target::before);
	if (name == "after") return NativeComputedStyle(node, Target::after);
	return NativeComputedStyle(node, Target::absent);
}

NodeHandle NativeComputedStyle::selectedNode() const
{
	if (!owner_.valid()) return {};
	if (target_ == Target::element) return owner_;
	if (target_ == Target::absent) return {};
	return NodeHandle(Tree::instance().generatedPseudoChild(owner_.id(), target_ == Target::before));
}

std::string NativeComputedStyle::display() const
{
	const auto selected = selectedNode();
	if (!selected.valid()) return {};
	const auto &node = Tree::instance().node(selected.id());
	switch (node.computedStyle().display) {
	case kDisplayNone: return "none";
	case kDisplayFlex: return "flex";
	case kDisplayGrid: return "grid";
	default: return LayoutEngine::isCssInlineLevelBox(node) ? "inline" : "block";
	}
}

std::string NativeComputedStyle::position() const
{
	const auto selected = selectedNode();
	if (!selected.valid()) return {};
	const auto &style = Tree::instance().node(selected.id()).computedStyle();
	switch (style.position) {
	case 1: return "absolute";
	case 2: return "relative";
	case kPositionFixed: return "fixed";
	default: return "static";
	}
}

namespace {
std::string computedPercentText(int permille)
{
	std::string text = std::to_string(permille / 10);
	if (permille % 10 != 0) text += '.' + std::to_string(permille % 10);
	return text + '%';
}

const char *computedOverflowText(int value)
{
	switch (value) {
	case 1: return "hidden";
	case 2: return "auto"; // The native engine uses one scrolling mode.
	case 3: return "clip";
	default: return "visible";
	}
}
}

std::string NativeComputedStyle::overflow() const
{
	const auto selected = selectedNode();
	if (!selected.valid()) return {};
	const auto &style = Tree::instance().node(selected.id()).computedStyle();
	const int x = overflowX(style), y = overflowY(style);
	std::string text = computedOverflowText(x);
	if (x != y) text += std::string(" ") + computedOverflowText(y);
	return text;
}

std::string NativeComputedStyle::borderRadius() const
{
	const auto selected = selectedNode();
	if (!selected.valid()) return {};
	const auto &style = Tree::instance().node(selected.id()).computedStyle();
	std::string cornerText[4];
	for (int corner = 0; corner < 4; ++corner) {
		const auto index = GEA_CSS_RADIUS_INDEX(corner);
		const int percent = style.border_radius_percent[index];
		cornerText[corner] = percent != kUnset ? computedPercentText(percent) : std::to_string(style.border_radius[index]) + "px";
	}
	int count = 4;
	if (cornerText[3] == cornerText[1]) {
		--count;
		if (cornerText[2] == cornerText[0]) {
			--count;
			if (cornerText[1] == cornerText[0]) --count;
		}
	}
	std::string result = std::move(cornerText[0]);
	for (int corner = 1; corner < count; ++corner) result += ' ' + cornerText[corner];
	return result;
}

std::string NativeComputedStyle::filter() const
{
	const auto selected = selectedNode();
	if (!selected.valid()) return {};
	const auto &style = rstyle(Tree::instance().node(selected.id()).computedStyle());
	return style.filter_present ? "blur(" + std::to_string(style.filter_blur_radius) + "px)" : "none";
}

std::string NativeComputedStyle::resolvedDimension(bool horizontal) const
{
	const auto selected = selectedNode();
	if (!selected.valid()) return {};
	auto &tree = Tree::instance();
	const int id = selected.id();
	const auto &node = tree.node(id);
	const auto &style = node.computedStyle();
	const bool inlineBox = LayoutEngine::isCssInlineLevelBox(node) && node.type != NodeType::Image;
	if (selected.isConnected() && style.display != kDisplayNone && !inlineBox) {
		tree.flushLayoutFor(id);
		if (!selected.valid()) return {};
		const auto &laidOut = tree.node(id);
		const auto &laidOutStyle = laidOut.computedStyle();
		const int borderBox = horizontal ? laidOut.layout.width : laidOut.layout.height;
		const int insets = laidOutStyle.box_sizing == 0 ? boxInsets(laidOutStyle, horizontal) : 0;
		return std::to_string(std::max(0, borderBox - insets)) + "px";
	}
	const auto expression = horizontal ? style.width_expression : style.height_expression;
	if (expression >= 0) return {}; // This view cannot serialize unresolved CSS expressions.
	const int percent = horizontal ? style.width_percent : style.height_percent;
	if (percent != kUnset) return computedPercentText(percent);
	const int pixels = horizontal ? style.width : style.height;
	return pixels == kUnset ? "auto" : std::to_string(pixels) + "px";
}

std::string NativeComputedStyle::width() const { return resolvedDimension(true); }
std::string NativeComputedStyle::height() const { return resolvedDimension(false); }

std::string NativeComputedStyle::paddingTop() const
{
	const auto selected = selectedNode();
	return selected.valid() ? std::to_string(Tree::instance().node(selected.id()).computedStyle().padding[0]) + "px" : std::string{};
}

std::string NativeComputedStyle::paddingBottom() const
{
	const auto selected = selectedNode();
	return selected.valid() ? std::to_string(Tree::instance().node(selected.id()).computedStyle().padding[2]) + "px" : std::string{};
}

std::string NodeHandle::domNodeName() const
{
	switch (static_cast<int>(nodeType)) {
	case 3: return "#text";
	case 8: return "#comment";
	case 11: return "#document-fragment";
	case 1: break;
	default: return {};
	}
	std::string qualified;
	if (const auto *prefix = namespacePrefix()) qualified = *prefix + ':';
	qualified += tagName();
	const auto *uri = namespaceURI();
	if (uri && *uri == "http://www.w3.org/1999/xhtml") {
		for (auto &ch : qualified)
			if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - ('a' - 'A'));
	}
	return qualified;
}

const std::string *NodeHandle::characterData() const
{
	const int type = static_cast<int>(nodeType);
	if (type != 3 && type != 8) return nullptr;
	return &Tree::instance().node(id()).text.str();
}

void NodeHandle::setCharacterData(const char *text) const
{
	if (characterData()) setText(text ? text : "");
}

const std::string *NodeHandle::namespaceURI() const
{
	if (static_cast<int>(nodeType) != 1) return nullptr;
	const auto *metadata = rareDataFor(id());
	static const std::string html = "http://www.w3.org/1999/xhtml";
	if (!metadata || !metadata->hasExplicitNamespace) return &html;
	return metadata->namespaceUri == kInvalidCssAtom ? nullptr : &cssAtomString(metadata->namespaceUri);
}

const std::string *NodeHandle::namespacePrefix() const
{
	const auto *metadata = valid() ? rareDataFor(id()) : nullptr;
	return metadata && metadata->namespacePrefix != kInvalidCssAtom ? &cssAtomString(metadata->namespacePrefix) : nullptr;
}

// DOM Node.nodeType for a handle: ELEMENT_NODE (1) for any element-like tree
// node, TEXT_NODE (3), COMMENT_NODE (8), or DOCUMENT_FRAGMENT_NODE (11).
NodeHandle::NodeTypeProperty::operator int() const
{
	if (!gea::framework::identity::current(id_, token())) return 0;
	const int id = id_;
	const auto &tree=Tree::instance();
	// The template renderer's list/conditional anchors are hidden nodes tagged
	// "#comment"; DOM queries must see them as the comments they stand for.
	if (tree.node(id).type == NodeType::Comment || std::strcmp(tree.tagName(id), "#comment") == 0) return 8;
	if (tree.node(id).type == NodeType::DocumentFragment) return 11;
	// Native JSX text leaves retain their element tag (p/h1/etc.). Only an
	// anonymous Text allocation projects a DOM text node.
	return tree.node(id).type==NodeType::Text && !*tree.tagName(id)?3:1;
}

void NodeHandle::setPressId(int pressId) const
{
	if (!valid()) return;
	Tree::instance().setPressId(id(), pressId);
}

void NodeHandle::setPressValue(int pressValue) const
{
	if (!valid()) return;
	Tree::instance().setPressValue(id(), pressValue);
}

gea::framework::events::EventListenerId NodeHandle::addEventListener(const char *type, gea::framework::events::EventListener listener) const
{
	if (!valid()) return gea::framework::events::kInvalidEventListenerId;
	return Tree::instance().setEventListener(id(), type, std::move(listener));
}

bool NodeHandle::removeEventListener(const char *type, gea::framework::events::EventListenerId listenerId) const
{
	if (!valid()) return false;
	return Tree::instance().removeEventListener(id(), type, listenerId);
}

}  // namespace gea::embedded::ui

namespace gea::embedded::ui {
const char *NodeHandle::selectorText(const std::string &s) { if(s.find('\0')!=std::string::npos)selector::parse("").requireValid();return s.c_str(); }
bool NodeHandle::disabled() const { return valid()&&hasAttribute("disabled"); }
void NodeHandle::setDisabled(bool value) const { if(value)setAttribute("disabled","");else removeAttribute("disabled"); }
bool NodeHandle::matches(const char *s) const { auto q=selector::parse(s); q.requireValid(); return valid()&&q.matches(Tree::instance(),id(),id()); }
NodeHandle NodeHandle::closest(const char *s) const { auto q=selector::parse(s);q.requireValid();if(!valid())return {};auto&t=Tree::instance();for(int n=id(),left=t.nodeCount();left>0&&NodeHandle(n).valid();--left,n=t.node(n).parent)if(q.matches(t,n,id()))return NodeHandle(n);return {}; }
std::vector<NodeHandle> NodeHandle::querySelectorAll(const char *s) const {
 auto q=selector::parse(s);q.requireValid();std::vector<NodeHandle> out;if(!valid())return out;auto&t=Tree::instance();
 std::vector<int> pending; for(int c=t.node(id()).last_child,left=t.nodeCount();left>0&&NodeHandle(c).valid();--left,c=t.node(c).prev_sibling)pending.push_back(c);
 for(int left=t.nodeCount();left>0&&!pending.empty();--left){int n=pending.back();pending.pop_back();if(!NodeHandle(n).valid())continue;if(q.matches(t,n,id()))out.emplace_back(n);for(int c=t.node(n).last_child,b=t.nodeCount();b>0&&NodeHandle(c).valid();--b,c=t.node(c).prev_sibling)pending.push_back(c);}return out;
}
NodeHandle NodeHandle::querySelector(const char*s) const {auto all=querySelectorAll(s);return all.empty()?NodeHandle():all.front();}
}
