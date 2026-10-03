// SPDX-License-Identifier: Apache-2.0
#include "document.h"

#include "internal.h"

#include "display.h"
#include "display_underlay.h"
#include "tree_internal.h"

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

bool matchesSelector(Tree &tree, int nodeId, const std::string &selector)
{
	if (selector.empty()) return false;
	if (selector == "body") {
		const int mountedRoot = tree.mountedRoot();
		return (mountedRoot >= 0 && nodeId == mountedRoot) || (mountedRoot < 0 && nodeId == 0);
	}
	if (selector[0] == '#') {
		if (selector.size() <= 1) return false;
		return std::strcmp(tree.getAttribute(nodeId, "id"), selector.c_str() + 1) == 0;
	}
	if (selector[0] == '.') {
		if (selector.size() <= 1) return false;
		return tree.hasClass(nodeId, selector.substr(1));
	}
	return lowerAscii(tree.tagName(nodeId)) == lowerAscii(selector);
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
	return NodeHandle(Tree::instance().createView());
}

ViewElement Document::createVirtualList() const
{
	return ViewElement(Tree::instance().createVirtualList());
}

NodeHandle Document::createElement(const char *tag) const
{
	std::string lowered = tag ? tag : "";
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return std::tolower(ch); });
	NodeHandle node;
	if (lowered == "button") node = createButton();
	else if (lowered == "canvas") node = createCanvas();
	else if (lowered == "audio") node = createAudio();
	else if (lowered == "img" || lowered == "image" || lowered == "video") node = createImage();
	else if (lowered == "virtual-list") node = createVirtualList();
	else node = createView();
	if (!lowered.empty()) node.setTagName(lowered.c_str());
	return node;
}

NodeHandle Document::getElementById(const char *id) const
{
	if (!id || id[0] == '\0') return NodeHandle();
	Tree &tree = Tree::instance();
	NodeHandle existing = findElementById(tree, id);
	if (existing) return existing;
	return std::strcmp(id, "app") == 0 ? ensureAppRoot("app") : NodeHandle();
}

NodeHandle Document::querySelector(const char *selector) const
{
	const std::string normalized = normalizeSelector(selector);
	if (normalized == "body" || normalized == "#app") return ensureAppRoot("app");
	const std::vector<NodeHandle> matches = querySelectorAll(normalized.c_str());
	return matches.empty() ? NodeHandle() : matches.front();
}

std::vector<NodeHandle> Document::querySelectorAll(const char *selector) const
{
	std::vector<NodeHandle> matches;
	const std::string normalized = normalizeSelector(selector);
	if (normalized.empty()) return matches;
	if (normalized == "body" || normalized == "#app") {
		matches.push_back(ensureAppRoot("app"));
		return matches;
	}

	Tree &tree = Tree::instance();
	for (int nodeId = 0; nodeId < tree.nodeCount(); nodeId++) {
		if (matchesSelector(tree, nodeId, normalized)) matches.push_back(NodeHandle(nodeId));
	}
	return matches;
}

namespace {
// Preferred mount size, set by platform targets via Application::init.
// ensureAppRoot reads this when creating the implicit #app root so
// fullscreen targets get the panel-sized root instead of kWidth × kHeight.
int g_preferred_mount_w = 0;
int g_preferred_mount_h = 0;

struct DocumentEventListener {
	std::string type;
	gea::framework::events::EventListener listener;
	int boundRoot = -1;
};

std::vector<DocumentEventListener> &documentEventListeners()
{
	static std::vector<DocumentEventListener> listeners;
	return listeners;
}

void bindDocumentEventListeners()
{
	const int root = Tree::instance().mountedRoot();
	if (root < 0) return;
	for (auto &entry : documentEventListeners()) {
		if (entry.boundRoot == root) continue;
		Tree::instance().setEventListener(root, entry.type.c_str(), entry.listener);
		entry.boundRoot = root;
	}
}

// Map a pointer/touch event type to a slot index, aliasing pointer* onto its
// touch* sibling (the web binds onPointerDown/Move/Up; the device synthesizes
// touch*). Mirrors eventTypeIndex's pointer/touch grouping in tree_events.cpp.
int pointerTypeIndex(const char *type)
{
	if (!type) return -1;
	if (std::strcmp(type, "click") == 0) return 0;
	if (std::strcmp(type, "touchstart") == 0 || std::strcmp(type, "pointerdown") == 0) return 1;
	if (std::strcmp(type, "touchmove") == 0 || std::strcmp(type, "pointermove") == 0) return 2;
	if (std::strcmp(type, "touchend") == 0 || std::strcmp(type, "pointerup") == 0 ||
	    std::strcmp(type, "pointercancel") == 0)
		return 3;
	return -1;
}
}  // namespace

bool dispatchDocumentPointer(gea::framework::events::PointerEvent &event)
{
	const int wantIdx = pointerTypeIndex(event.typeName());
	if (wantIdx < 0) return false;
	bool fired = false;
	for (auto &entry : documentEventListeners()) {
		if (pointerTypeIndex(entry.type.c_str()) != wantIdx) continue;
		if (!static_cast<bool>(entry.listener)) continue;
		entry.listener(event);
		fired = true;
		if (event.propagationStopped) break;
	}
	return fired;
}

void resetDocumentDelegatedEventListeners()
{
	documentEventListeners().clear();
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
	bindDocumentEventListeners();
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
	if (!type || type[0] == '\0' || !listener) return;
	// `rotary` (and `keydown` bound on a non-input) is a genuine document-level
	// event — it has no element target, so it is stored off-tree as a singleton
	// and dispatched directly by the runtime, never bound to a node.
	if (std::strcmp(type, "rotary") == 0 || std::strcmp(type, "keydown") == 0) {
		setDocumentEventListener(type, std::move(listener));
		return;
	}
	// Everything else delegates through the native body node (event bubbles up
	// the tree to the mounted root, where this listener filters by target).
	documentEventListeners().push_back(DocumentEventListener{type, std::move(listener), -1});
	bindDocumentEventListeners();
}

}  // namespace gea::embedded::ui

namespace
{
	gea::framework::display::Underlay::Paint underlayPaint = nullptr;
	void *underlayUser = nullptr;
	int underlayX0 = 0, underlayY0 = 0, underlayX1 = -1, underlayY1 = -1;
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
}

void gea::framework::display::Underlay::clear()
{
	underlayPaint = nullptr;
	underlayUser = nullptr;
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
}
