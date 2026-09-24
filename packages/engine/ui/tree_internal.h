// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "canvas.h"
#include "events.h"
#include "node_model.h"
#include "style.h"

#include <string>
#include <string_view>

namespace gea::embedded::ui {

class Tree {
public:
	static Tree &instance();

	Node *nodes();
	const Node *nodes() const;
	Node &node(int id);
	const Node &node(int id) const;
	int nodeCount() const;
	int mountedRoot() const;
	int mountedWidth() const;
	int mountedHeight() const;

	int createNode(NodeType type);
	int createView();
	int createButton();
	int createText();
	int createImage();
	int createCanvas();
	int createCamera();
	int createAudio();
	int createVirtualList();
	// Deep/shallow duplicate of a node subtree (DOM cloneNode semantics: copies
	// tag, text, attributes, classes, styles; NOT event listeners; the clone is
	// detached until the caller parents it).
	int cloneNode(int sourceId, bool deep);

	// DOM-style child traversal for compiled templates (firstChild/childNodes[i]).
	int firstChildOf(int node);
	int childAt(int node, int index);

	void setParent(int child, int parent);
	void insertBefore(int child, int parent, int reference);
	void setStyle(int node, Property prop, int value);
	void setDefaultStyle(int node, Property prop, int value);
	void setStyleFromClass(int node, Property prop, int value);
	void resetStyleForClassRecompute(int node);
	void setText(int node, const char *text);
	void setAttribute(int node, const char *name, const char *value);
	void removeAttribute(int node, const char *name);
	bool toggleAttribute(int node, const char *name, bool force);
	const char *getAttribute(int node, const char *name) const;
	bool hasAttribute(int node, const char *name) const;
	void setTagName(int node, const char *tagName);
	const char *tagName(int node) const;
	void setClassName(int node, const char *className);
	void setClassName(int node, const std::string &className);
	bool addClass(int node, const std::string &token);
	bool removeClass(int node, const std::string &token);
	bool toggleClass(int node, const std::string &token);
	bool toggleClass(int node, const std::string &token, bool force);
	void clearClasses(int node);
	bool hasClass(int node, const std::string &token) const;
	std::string className(int node) const;
	void setPressId(int node, int pressId);
	int pressId(int node) const;
	void setPressValue(int node, int pressValue);
	int pressValue(int node) const;
	gea::framework::events::EventListenerId setEventListener(int node, const char *type, gea::framework::events::EventListener listener);
	bool removeEventListener(int node, const char *type, gea::framework::events::EventListenerId listenerId);
	bool hasEventListener(int node) const;
	// True if any node currently has a listener for `type` (a DOM event-type
	// string, e.g. "touchmove"). Dispatchers query this to skip building and
	// walking an event nobody listens to. Conservatively returns true for
	// event types not tracked by the listener tables.
	bool hasListenersForType(const char *type) const;
	bool dispatchEvent(gea::framework::events::PointerEvent &event);
	bool containsNode(int ancestor, int node) const;
	void removeNode(int node);
	gea::framework::graphics::Canvas *ensureCanvas(int node, int width = 0, int height = 0);
	const gea::framework::graphics::Canvas *canvas(int node) const;
	bool canUseDisplayBackedCanvas(int node, int width = 0, int height = 0);
	bool markDisplayBackedCanvas(int node, int width = 0, int height = 0);
	bool isDisplayBackedCanvas(int node) const;
	void displayBackedFailDebug(int *reason, int *extra, int *count) const;
	void canvasSlotDebug(int id, int *mode, int *rebinds) const;
	void markCanvasDirty(int node);

	// A <camera> leaf reuses the same per-node RGB565 surface machinery as a
	// canvas, but the pixels are produced by the platform camera backend each
	// frame (framebuffer preview mode) rather than by app drawing calls.
	gea::framework::graphics::Canvas *ensureCameraSurface(int node, int width = 0, int height = 0);

	void mount(int root, int width, int height);
	void refresh(int root, int width, int height);
	// Native targets (macOS) that bypass the display-list pipeline and drive
	// platform widgets directly need just the flex-layout pass without any
	// rendering / dirty-region / present side effects. computeLayout is that
	// pass in isolation; other targets keep calling refresh() unchanged.
	void computeLayout(int root, int width, int height);
	void frame(int timestampMs);
	void clear();

	int hitTest(int x, int y);
	int hitTestNode(int x, int y);
	// Hover-capable pointing devices only; touch dragging remains pointerMove.
	// Negative coordinates clear hover when the pointer leaves the surface.
	int pointerHover(int x, int y);
	bool isHovered(int node) const;
	void pointerDown(int x, int y);
	int pointerMove(int x, int y);
	int pointerUp();
	// Programmatic vertical scroll of the first scrollable container by `dy`
	// layout px (positive = scroll toward the end of the content). Drives the
	// default Up/Down-button scroll on touchless hardware. Returns true if the
	// scroll offset changed.
	bool scrollByKeyStep(int dy);
	void tickInput(int timestampMs);
	void resetInput();
	int scrollLeft(int node) const;
	void setScrollLeft(int node, int scrollLeft);
	int scrollTop(int node) const;
	void setScrollTop(int node, int scrollTop);
	void scrollIntoView(int node);
	void applyPendingScrollIntoView();

	// Active focus for `<input>` elements. Pixel-only targets can paint the
	// caret + open the virtual keyboard based on this; native text targets
	// can use their platform responder/focus chain instead.
	// setActiveInput(-1) clears focus.
	int activeInputId() const;
	void setActiveInput(int nodeId);
	bool activeInputCaretVisible() const;
	void tickInputCaret(int timestampMs);

	void markDisplayListDirty();
	void markDisplayListContentDirty();
	void markNodeDisplayCommandsDirty(int node);
	void markScrollDirty(int node);
	bool displayListRebuildRequired() const;
	bool refreshRequired() const;
	// Bumped on every mount()/refresh(); native renderers diff against the last
	// value they synced to decide whether a re-sync is needed at all.
	uint64_t refreshSerial() const;
	void setDisplayListRebuildRequired(bool required);
	bool nodeDisplayCommandsDirty(int node) const;
	void clearNodeDisplayCommandDirty(int node);

	int lastFrameMs() const;
	void setLastFrameMs(int timestampMs);

private:
	Tree() = default;
};

// Document-level (off-tree) events: `rotary` always, and `keydown` bound on a
// non-input element. These belong to `document`, not to any node, so they are
// stored as singletons (not per-node listener slots) and dispatched directly by
// the runtime rather than through Tree::dispatchEvent's tree walk.
void setDocumentEventListener(const char *type, gea::framework::events::EventListener listener);
void dispatchDocumentRotary(int delta);
bool dispatchDocumentKeyDown(gea::framework::events::PointerEvent &event);
void resetDocumentEventListeners();
void resetDocumentDelegatedEventListeners();

// Deliver a pointer/touch event straight to document-level listeners with no
// tree walk. Immediate-mode apps (Display.ctx games) mount no UI tree, so there
// is no root node to bubble to or to bind document listeners onto; without this
// path, `document.addEventListener('pointerdown', …)` would never fire on-device
// (it does on web, where the DOM document sees every pointer event). Returns true
// if any listener ran. Caller invokes this only when no UI tree is mounted.
bool dispatchDocumentPointer(gea::framework::events::PointerEvent &event);

// Tag interning: the per-node tag is stored as a small int16 id (Node::tag_id)
// indexing a global table of unique tag strings, instead of a 16-byte inline
// buffer. internTag returns the id for a tag string (creating it on first use);
// tagFromId returns the stable interned string (id 0 = "").
int16_t internTag(const char *tag);
const char *tagFromId(int16_t id);

inline bool isDocumentCanvasRoot(const Node &node)
{
	return node.parent < 0 && std::string_view(tagFromId(node.tag_id)) == "html";
}


// HTML importers mark DOM text separately from Gea's styleable TextElement.
// The former inherits text properties but is never an element selector target.
inline bool isAnonymousTextNode(const Node &node)
{
	if (node.type != NodeType::Text) return false;
	return std::string_view(tagFromId(node.tag_id)) == "#text";
}

// Blink-style rare-data pool accessors. A node's cold/optional state lives in a
// pooled NodeRareData block referenced by Node::rare_data (-1 = none).
struct NodeRareData;
NodeRareData &ensureRareData(int node);  // allocates a block on first use
NodeRareData *rareDataFor(int node);     // nullptr when the node has none
void releaseRareData(int node);          // frees the block back to the pool

// Re-replay the live retained display list full-screen into `dst` (rgb565) for a
// screenshot, working around the fused flush leaving the PSRAM framebuffer stale.
// Caller must hold AppState::lock(). Returns false for canvas/present apps.
bool renderRetainedSnapshotRgb565(std::uint16_t *dst, int width, int height);

}  // namespace gea::embedded::ui
