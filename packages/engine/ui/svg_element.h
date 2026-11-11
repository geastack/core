// SPDX-License-Identifier: Apache-2.0
#pragma once

// Inline <svg> in the node tree.
//
// Every SVG element is an ordinary node: it has the NodeHandle interface,
// classes, inline style, listeners and :active/:hover state. What differs is
// how it paints:
//
//   - <svg> is a view (its CSS box, background and border paint as usual) that
//     records ONE DrawSvg display command. That command rasterizes the svg's
//     cached scene, anti-aliased, straight into the framebuffer at replay.
//   - Shapes and groups (path polyline polygon line circle ellipse rect g) are
//     out-of-flow, zero-size nodes that never paint a box of their own; they
//     are the scene's source. The hit test descends into them by geometry, so
//     a tap on a <circle> targets that circle and bubbles through <g> and <svg>.
//
// Paint comes from presentation attributes and from CSS (fill, stroke,
// stroke-width, fill-opacity, stroke-opacity, stroke-linecap,
// stroke-linejoin, fill-rule, opacity, color/currentColor, visibility,
// display, pointer-events); CSS wins over attributes, and the inheritable
// properties inherit through the svg subtree.
//
// A change anywhere in an svg subtree (attribute, style, class, structure)
// marks that svg's scene dirty; it rebuilds once, when the svg next records,
// and repaints only the svg's box. Everything here compiles out with
// GEA_UI_SVG=0, which the plugin proves for apps without <svg>.

#include "node_features.h"

#if GEA_UI_SVG

#include "style.h"

#include <cstdint>

namespace gea::embedded::ui {
struct Node;
struct DisplayCommand;
}

namespace gea::embedded::ui::svg {

// True for the SVG paint properties (stored in RareStyle).
inline bool isPaintProperty(Property p)
{
	return p >= Property::SvgFill && p <= Property::SvgFillRule;
}

// Parses a CSS declaration value for one of the SVG paint declarations into
// its style property and encoded value. False leaves the property untouched.
bool cssDeclarationValue(StyleDeclaration declaration, const char *value, Property &property, int &encoded);

// The <svg> root test: one int compare against the interned "svg" tag id.
bool isRoot(const Node &node);

// True for an SVG shape, group or metadata element (anything in an svg
// subtree except the <svg> root). Such nodes paint nothing themselves, so
// inserting, removing or restyling one only repaints its svg's scene.
bool isShape(int node);

// The pixels (relative to the svg's box) that its last scene rebuild changed,
// consumed once per rebuild. False means "repaint the whole box": no rebuild
// since the last call, a first build or resize, a z-order change, or a change
// to the svg box's own style or position. An empty rect (x1 < x0) means the
// rebuild changed nothing on screen.
bool damage(int root, int &x0, int &y0, int &x1, int &y1);

// Tree hooks.
void noteTagChanged(int node);
// Records an attribute on an SVG element (value == nullptr removes it).
// Returns true when the node is an SVG element.
bool noteAttribute(int node, const char *name, const char *value);
// The full attribute value for an SVG element (unbounded, unlike the generic
// attribute store), or nullptr when the node holds no such SVG attribute.
const char *attribute(int node, const char *name);
void noteNodeChanged(int node);
void releaseNode(int node);
void clearAll();

// Refresh hook: detects changes made by any path (class recompute, :active,
// structure) by comparing each svg subtree's signature with its last scene.
void validateScenes();

// Rendering.
void record(const Node &svgNode);
void paint(const DisplayCommand &command);

// Hit test inside an svg root whose box contains (x, y): the topmost shape
// under the point, or -1 when no shape paints there.
int hitTest(int rootId, int x, int y);

}  // namespace gea::embedded::ui::svg

#endif  // GEA_UI_SVG
