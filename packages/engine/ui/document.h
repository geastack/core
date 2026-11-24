// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "events.h"
#include "event_listener_options.h"
#include "tree_events.h"
#include <type_traits>
#include <cstdlib>
#include "image.h"
#include "canvas_element.h"
#include "camera_element.h"
#include "node.h"
#include "text.h"
#include "view.h"

#include <string>
#include <vector>

namespace gea { template <typename Signature> struct CallableObject; }

namespace gea::embedded::ui {

class Document {
public:
	static Document &instance();

	ViewElement body() const;
	// No guessed body fallback: null handle when no actual connected focus.
	NodeHandle activeElement() const;
	ViewElement createView() const;
	ViewElement createButton() const;
	TextElement createText(const char *text = "") const;
	// geatsc emits std::string arguments at typed call sites (a string literal
	// arrives as `std::string("...")`), so the factories the plugin binds take
	// both forms — the binding emit passes the argument through untouched
	// instead of adapting it with .c_str().
	TextElement createText(const std::string &text) const { return createText(text.c_str()); }
	ImageElement createImage() const;
	CanvasElement createCanvas() const;
	// Node id of the mounted root when it is a <canvas> (the shape a previous
	// Display.ctx() call mounts), else -1. Lets Display.ctx() behave as an
	// idempotent getter instead of rebuilding the document on every call.
	int mountedCanvasNode() const;
	CameraElement createCamera() const;
	NodeHandle createAudio() const;
	NodeHandle createComment(const char *text = "") const;
	NodeHandle createComment(const std::string &text) const { return createComment(text.c_str()); }
	// A detached structural owner whose children move into an insertion parent.
	NodeHandle createDocumentFragment() const;
	ViewElement createVirtualList() const;
	// Generic tag-string factory (the DOM document.createElement shape): maps
	// the known embedded tags to their typed creators and any other tag to a
	// plain view, stamping the tag via setTagName so tag-driven behaviours
	// (e.g. the <input> VirtualKeyboard focus hook) still see it. Returns the
	// TYPED handle — boxing happens only if the caller stores it dynamically.
	NodeHandle createElement(const char *tag) const;
	NodeHandle createElement(const std::string &tag) const { return createElement(tag.c_str()); }
	NodeHandle getElementById(const char *id) const;
	// geatsc's `nativeDocumentMethods` binding for `document.getElementById(...)`
	// passes its argument through untouched (see `createElement`'s identical
	// comment above) — the compiler's `host-document-calls.ts` only ever plans a
	// template of the exact shape `symbol({arg0}, ...)`, never one with a
	// `.c_str()` (or any other) adaptation baked in, so the `const char *`
	// overload alone left every typed `document.getElementById(id)` with no call
	// plan at all. Both overloads exist so whichever argument form geatsc emits
	// (string literal or `std::string`) resolves without the plugin needing to
	// adapt it.
	NodeHandle getElementById(const std::string &id) const { return getElementById(id.c_str()); }
	NodeHandle querySelector(const char *selector) const;
	NodeHandle querySelector(const std::string &selector) const { return querySelector(NodeHandle::selectorText(selector)); }
	std::vector<NodeHandle> querySelectorAll(const char *selector) const;
	std::vector<NodeHandle> querySelectorAll(const std::string &selector) const { return querySelectorAll(NodeHandle::selectorText(selector)); }
	ViewElement ensureAppRoot(const char *id = "app") const;

	// Platforms set this before Application::init to control the size of
	// the implicit #app root created by ensureAppRoot — e.g. geaos passes
	// the /dev/fb0 dimensions so apps that use `width: 100vw` get the full
	// panel instead of the framework's default 410×502.
	static void setPreferredMountSize(int width, int height);
	static int preferredMountWidth();
	static int preferredMountHeight();
	static void markDirectCanvasContextUsed();
	static bool directCanvasContextUsed();

	void clear() const;
	void mount(NodeHandle root, int width, int height) const;
	void refresh(NodeHandle root, int width, int height) const;
	void refreshMountedIfDirty() const;
	void frame(int timestampMs) const;
	void addEventListener(const char *type, gea::framework::events::EventListener listener) const;
 // Exact typed literal overload prevents a cross-conversion ambiguity with
 // the legacy const-char*/std::function C++ entry point.
 template <typename Handler> requires requires(const Handler &handler) { handler.functionObjectIdentity(); }
 void addEventListener(const char *type, const Handler &handler, bool capture=false, bool once=false) const {
  addEventListener(std::string(type?type:""),handler,capture,once);
 }
 template <typename Result, typename Argument>
 void addEventListener(const std::string &type,
                       const gea::CallableObject<Result(Argument)> &handler,
                       bool capture = false, bool once = false) const {
  if (documentEventListenersRetiring()) return;
  static_assert(std::is_constructible_v<std::decay_t<Argument>,
                const gea::framework::events::PointerEvent &>,
                "native document listener needs an installed event carrier");
  const auto held = handler;
  if (!addDocumentEventListener(type.c_str(), handler.functionObjectIdentity().get(),
      gea::framework::events::explicitEventCallback([](auto& held, gea::framework::events::PointerEvent &event) {
        held.call(static_cast<std::decay_t<Argument>>(event));
      }, held), capture, once)) std::abort();
 }
 template <typename Result>
 void addEventListener(const std::string &type,
                       const gea::CallableObject<Result()> &handler,
                       bool capture = false, bool once = false) const {
  if (documentEventListenersRetiring()) return;
  const auto held = handler;
  if (!addDocumentEventListener(type.c_str(), handler.functionObjectIdentity().get(),
      gea::framework::events::explicitEventCallback([](auto& held, gea::framework::events::PointerEvent &) { held.call(); }, held), capture, once)) std::abort();
 }
 template <typename Handler>
 void removeEventListener(const std::string &type, const Handler &handler,
                          bool capture = false) const {
  removeDocumentEventListener(type.c_str(), handler.functionObjectIdentity().get(), capture);
 }

 template <typename Handler, typename Options>
  requires (!std::is_same_v<std::decay_t<Options>,bool>)
 void addEventListener(const std::string &type, const Handler &handler, const Options &options) const {
  const auto parsed=gea::framework::events::listenerOptions(options);
  addEventListener(type,handler,parsed.capture,parsed.once);
 }
 template <typename Handler, typename Options>
  requires (!std::is_same_v<std::decay_t<Options>,bool>)
 void removeEventListener(const std::string &type, const Handler &handler, const Options &options) const {
  const auto parsed=gea::framework::events::listenerOptions(options);
  removeEventListener(type,handler,parsed.capture);
 }


private:
	Document() = default;
};

// Bracket the initial app mount. Between these calls, per-node class-style
// recomputes are deferred so a large tree + large stylesheet don't mount in
// O(nodes^2 x rules); endStyleMountBatch() runs one recomputeAllClassStyles()
// pass with identical results. Defined in style.cpp.
void beginStyleMountBatch();
void endStyleMountBatch();
// True while an initial-mount style batch is open (between begin/endStyleMountBatch).
// During this window per-node class styles are NOT yet applied, so anything that
// presents to the panel would show unstyled content; the render path uses this to
// suppress that flush until styles land. Defined in style.cpp.
bool styleMountBatchActive();
// Queues the class styles of a newly mounted root. The mounted root is what
// `:root` matches and where a stylesheet's custom properties live, but nothing
// in building it (createView + an id attribute) queues its own recompute, so
// `:root { --x: ... }` never landed and every var(--x) below resolved to
// nothing. Deferred and coalesced under an open batch, where it becomes the
// one top-most root; immediate otherwise. Defined in style.cpp.
void noteMountedRootStyle(int root);

}  // namespace gea::embedded::ui
