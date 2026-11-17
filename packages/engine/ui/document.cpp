// SPDX-License-Identifier: Apache-2.0
#include "document.h"
#include "selector.h"
#include "tree_state.h"

#include "internal.h"

#include "display.h"
#include "display_underlay.h"
#include "tree_internal.h"
#include "tree_events.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <string>

namespace gea::embedded::ui {

namespace {

bool g_direct_canvas_context_used = false;

std::string normalizeSelector(const char *raw)
{
	if (!raw) return std::string();
	std::string selector(raw);
	auto first = std::find_if_not(selector.begin(), selector.end(), [](unsigned char ch) { return std::isspace(ch); });
	auto last = std::find_if_not(selector.rbegin(), selector.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
	if (first >= last) return std::string();
	return std::string(first, last);
}

std::string lowerAscii(std::string value)
{
	for (char &ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return value;
}

NodeHandle findElementById(Tree &tree, const char *id)
{
	if (!id || id[0] == '\0') return NodeHandle();
	for (int nodeId = 0; nodeId < tree.nodeCount(); nodeId++) {
		if (std::strcmp(tree.getAttribute(nodeId, "id"), id) == 0) return NodeHandle(nodeId);
	}
	return NodeHandle();
}

}  // namespace

Document &Document::instance()
{
	static Document document;
	return document;
}

ViewElement Document::body() const
{
	Tree &tree = Tree::instance();
	if (tree.mountedRoot() >= 0) return ViewElement(tree.mountedRoot());
	if (tree.nodeCount() > 0) return ViewElement(0);
	return ViewElement();
}

ViewElement Document::createView() const
{
	return ViewElement::create();
}

ViewElement Document::createButton() const
{
	return ViewElement(Tree::instance().createButton());
}

TextElement Document::createText(const char *text) const
{
	return TextElement::create(text);
}

ImageElement Document::createImage() const
{
	return ImageElement::create();
}

CanvasElement Document::createCanvas() const
{
	return CanvasElement::create();
}

int Document::mountedCanvasNode() const
{
	auto &tree = Tree::instance();
	const int root = tree.mountedRoot();
	if (root < 0) return -1;
	return tree.node(root).type == NodeType::Canvas ? root : -1;
}

void Document::markDirectCanvasContextUsed()
{
	g_direct_canvas_context_used = true;
}

bool Document::directCanvasContextUsed()
{
	return g_direct_canvas_context_used;
}

CameraElement Document::createCamera() const
{
	return CameraElement::create();
}

NodeHandle Document::createAudio() const
{
	return NodeHandle(Tree::instance().createAudio());
}

NodeHandle Document::createDocumentFragment() const
{
	return NodeHandle(Tree::instance().createDocumentFragment());
}

NodeHandle Document::createComment(const char *text) const
{
	auto &tree = Tree::instance();
	const int id = tree.createComment();
	if (id >= 0) tree.setText(id, text ? text : "");
	return NodeHandle(id);
}

ViewElement Document::createVirtualList() const
{
	return ViewElement(Tree::instance().createVirtualList());
}

NodeHandle Document::activeElement() const {
 const auto native=queryNativeFocus();
 auto node=native.handled?native.node:NodeHandle(Tree::instance().activeInputId());
 return node.valid()&&node.isConnected()?node:NodeHandle();
}

NodeHandle Document::createElement(const char *tag) const
{
	std::string lowered = tag ? tag : "";
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return std::tolower(ch); });
	NodeHandle node;
	if (lowered == "button") node = createButton();
	else if (lowered == "canvas") node = createCanvas();
	else if (lowered == "camera") node = createCamera();
	else if (lowered == "audio") node = createAudio();
	else if (lowered == "img" || lowered == "image" || lowered == "video") node = createImage();
	else if (lowered == "virtual-list") node = createVirtualList();
	else node = createView();
	if (!lowered.empty()) node.setTagName(lowered.c_str());
	return node;
}

NodeHandle Document::getElementById(const char *id) const
{
 if (!id || !*id) return {};
 auto &tree=Tree::instance();
 if(tree.mountedRoot()<0) return std::strcmp(id,"app")==0?ensureAppRoot("app"):NodeHandle();
 NodeHandle root(tree.mountedRoot());
 if(root.valid()&&std::strcmp(root.getAttribute("id"),id)==0)return root;
 for(auto node:root.querySelectorAll("*"))if(std::strcmp(node.getAttribute("id"),id)==0)return node;
 return {};
}
NodeHandle Document::querySelector(const char *s) const
{
 auto all=querySelectorAll(s);return all.empty()?NodeHandle():all.front();
}
std::vector<NodeHandle> Document::querySelectorAll(const char *s) const
{
 auto q=selector::parse(s);q.requireValid();auto &tree=Tree::instance();
 // Generated application bootstrap queries its single native root before
 // any node is mounted. Ordinary queries never manufacture nodes.
 const auto normalized=normalizeSelector(s);
 if(tree.mountedRoot()<0&&(normalized=="body"||normalized=="#app"))ensureAppRoot("app");
 NodeHandle root(tree.mountedRoot());std::vector<NodeHandle> out;if(!root.valid())return out;
 if(q.matches(tree,root.id(),root.id()))out.push_back(root);
 auto descendants=root.querySelectorAll(s);out.insert(out.end(),descendants.begin(),descendants.end());return out;
}

namespace {
// Preferred mount size, set by platform targets via Application::init.
// ensureAppRoot reads this when creating the implicit #app root so
// fullscreen targets get the panel-sized root instead of kWidth × kHeight.
int g_preferred_mount_w = 0;
int g_preferred_mount_h = 0;

// Map a pointer/touch event type to a slot index, aliasing pointer* onto its
// touch* sibling (the web binds onPointerDown/Move/Up; the device synthesizes
// touch*). Mirrors eventTypeIndex's pointer/touch grouping in tree_events.cpp.
int pointerTypeIndex(const char *type)
{
	if (!type) return -1;
	if (std::strcmp(type, "click") == 0) return 0;
	if (std::strcmp(type, "touchstart") == 0 || std::strcmp(type, "pointerdown") == 0) return 1;
	if (std::strcmp(type, "touchmove") == 0 || std::strcmp(type, "pointermove") == 0) return 2;
	if (std::strcmp(type, "touchend") == 0 || std::strcmp(type, "pointerup") == 0) return 3;
 if (std::strcmp(type, "pointercancel") == 0 || std::strcmp(type, "touchcancel") == 0) return 4;
	return -1;
}
}  // namespace

bool dispatchDocumentPointer(gea::framework::events::PointerEvent &event)
{
	const int wantIdx = pointerTypeIndex(event.typeName());
	if (wantIdx < 0) return false;
 bool fired = dispatchWindowEvent(event, true);
 if (!event.propagationStopped) fired = dispatchDocumentEvent(event, true) || fired;
 if (!event.propagationStopped) fired = dispatchDocumentEvent(event, false) || fired;
 if (!event.propagationStopped && event.bubbles) fired = dispatchWindowEvent(event, false) || fired;
	return fired;
}


void Document::setPreferredMountSize(int width, int height)
{
	g_preferred_mount_w = width;
	g_preferred_mount_h = height;
}

int Document::preferredMountWidth()  { return g_preferred_mount_w; }
int Document::preferredMountHeight() { return g_preferred_mount_h; }

ViewElement Document::ensureAppRoot(const char *id) const
{
	const char *rootId = (id && id[0] != '\0') ? id : "app";
#if GEA_UI_NODE_ATTRIBUTES
	NodeHandle existing = findElementById(Tree::instance(), rootId);
#else
	// With no authored attributes or ID selectors, the framework's implicit
	// #app is just the mounted root. Its built-in identity needs no per-node
	// attribute owner. Authored DOM lookups retain the ordinary path above.
	if (std::strcmp(rootId, "app") != 0) std::abort();
	NodeHandle existing(Tree::instance().mountedRoot());
#endif
	ViewElement root = existing ? ViewElement(existing.id()) : createView();
	Tree &tree = Tree::instance();
	const int rootW = g_preferred_mount_w  > 0 ? g_preferred_mount_w
	                : tree.mountedWidth()  > 0 ? tree.mountedWidth()
	                : gea::platform::display::kWidth;
	const int rootH = g_preferred_mount_h  > 0 ? g_preferred_mount_h
	                : tree.mountedHeight() > 0 ? tree.mountedHeight()
	                : gea::platform::display::kHeight;
	if (!existing) {
#if GEA_UI_NODE_ATTRIBUTES
		root.setAttribute("id", rootId);
#endif
		root.style().width(rootW);
		root.style().height(rootH);
	}
	if (tree.mountedRoot() < 0) mount(root, rootW, rootH);
	return root;
}

void Document::clear() const
{
	g_direct_canvas_context_used = false;
	Tree::instance().clear();
}

void Document::mount(NodeHandle root, int width, int height) const
{
	// Override caller-supplied dimensions with the platform's preferred
	// mount size if it set one. The JSX → C++ bundler emits hardcoded
	// `kWidth, kHeight` here, which is the right thing for fixed-size
	// embedded panels but wrong for fullscreen platforms (geaos passes
	// /dev/fb0's dimensions). Targets that don't call
	// Document::setPreferredMountSize keep the existing behavior.
	if (g_preferred_mount_w > 0) {
		width = g_preferred_mount_w;
		// Also override the root's inline width so app CSS (`width: 100vw`)
		// has a parent the right size.
		root.style().width(width);
	}
	if (g_preferred_mount_h > 0) {
		height = g_preferred_mount_h;
		root.style().height(height);
	}
	Tree::instance().mount(root.id(), width, height);
}

void Document::refresh(NodeHandle root, int width, int height) const
{
	Tree::instance().refresh(root.id(), width, height);
}

void Document::refreshMountedIfDirty() const
{
	if (directCanvasContextUsed()) return;
	// Post-fling image repaint: the first quiet frame after scroll-held image
	// blits marks the tree dirty so this very refresh repaints the covers.
	rootScrollImageHoldTick();
	auto &tree = Tree::instance();
	const int root = tree.mountedRoot();
	if (root < 0) return;
	if (!tree.refreshRequired()) return;
	tree.refresh(root, tree.mountedWidth(), tree.mountedHeight());
}

void Document::frame(int timestampMs) const
{
	Tree::instance().frame(timestampMs);
}

void Document::addEventListener(const char *type, gea::framework::events::EventListener listener) const
{
 setDocumentEventListener(type, std::move(listener));
}

}  // namespace gea::embedded::ui

namespace
{
	gea::framework::display::Underlay::Paint underlayPaint = nullptr;
	void *underlayUser = nullptr;
	int underlayX0 = 0, underlayY0 = 0, underlayX1 = -1, underlayY1 = -1;
	bool underlayOpaque = false;
}  // namespace

bool gea::framework::display::Underlay::composited()
{
	return !gea::embedded::ui::Document::directCanvasContextUsed() && gea::embedded::ui::Tree::instance().mountedRoot() >= 0;
}

void gea::framework::display::Underlay::set(Paint paint, void *user, int x0, int y0, int x1, int y1)
{
	underlayPaint = paint;
	underlayUser = user;
	underlayX0 = x0;
	underlayY0 = y0;
	underlayX1 = x1;
	underlayY1 = y1;
	underlayOpaque = false;
}

void gea::framework::display::Underlay::setOpaque(bool opaque)
{
	underlayOpaque = opaque;
}

bool gea::framework::display::Underlay::covers(int x0, int y0, int x1, int y1)
{
	return underlayPaint && underlayOpaque && x0 >= underlayX0 && y0 >= underlayY0 && x1 <= underlayX1 && y1 <= underlayY1;
}

void gea::framework::display::Underlay::clear()
{
	underlayPaint = nullptr;
	underlayUser = nullptr;
	underlayOpaque = false;
}

bool gea::framework::display::Underlay::active()
{
	return underlayPaint != nullptr;
}

void gea::framework::display::Underlay::paint(int x0, int y0, int x1, int y1)
{
	if (!underlayPaint) return;
	x0 = std::max(x0, underlayX0);
	y0 = std::max(y0, underlayY0);
	x1 = std::min(x1, underlayX1);
	y1 = std::min(y1, underlayY1);
	if (x0 > x1 || y0 > y1) return;
	underlayPaint(x0, y0, x1, y1, underlayUser);
	// The painter writes pixels directly, so the canvas has no record of them.
	// A display that stages the replay in a strip copies back only the canvas's
	// dirty box, which left the frame black under everything but the HUD text.
	if (auto *canvas = gea::platform::display::Display::canvas()) {
		int cx0 = 0, cy0 = 0, cx1 = -1, cy1 = -1;
		canvas->currentClip(&cx0, &cy0, &cx1, &cy1);
		x0 = std::max(x0, cx0);
		y0 = std::max(y0, cy0);
		x1 = std::min(x1, cx1);
		y1 = std::min(y1, cy1);
		if (x0 <= x1 && y0 <= y1) canvas->markDirty(x0, y0, x1, y1);
	}
}
