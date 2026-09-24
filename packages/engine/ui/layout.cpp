// SPDX-License-Identifier: Apache-2.0
#include "internal.h"
#include "flex_balance.h"
#include "memory.h"
#include "tree_state.h"
#include "refresh_perf.h"

#include "graphics/font.h"

#include <cstdio>
#include <climits>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>

namespace gea::embedded::ui {

bool isCollapsedFlexItem(const Node &node)
{
	return node.style.visibility == 2 && node.parent >= 0 &&
	       !isOutOfFlowPosition(node.style.position) && Tree::instance().nodes()[node.parent].style.display == kDisplayFlex;
}

bool isCollapsedFlexSubtree(const Node &node)
{
	const Node *nodes = Tree::instance().nodes();
	for (const Node *current = &node; current; current = current->parent >= 0 ? &nodes[current->parent] : nullptr)
		if (isCollapsedFlexItem(*current)) return true;
	return false;
}

namespace {

constexpr int kScratchDepth = 16;

int formattingEdgeNode(const Node *nodes, int id, bool last);

bool isLineBreak(const Node &node)
{
	return node.type == NodeType::View && std::strcmp(tagFromId(node.tag_id), "br") == 0;
}


bool suppressAnonymousWhitespace(const Node *nodes, int parent, int child)
{
	const Node &text = nodes[child];
	if (!isAnonymousTextNode(text)) return false;
	if (text.text.empty()) return true;
	if ((text.style.white_space != 0 && text.style.white_space != 1) || text.text.find_first_not_of(" \t\r\n\f") != std::string::npos) return false;
	if (nodes[parent].style.display == kDisplayFlex || isDisplayGrid(nodes[parent].style)) return true;
	// Whitespace at a line boundary or between block boxes creates no line.
	// Preserve the text node itself and spaces between inline siblings.
	auto adjacentInline = [&](int sibling, bool forward) {
		while (sibling >= 0 && (isDisplayNone(nodes[sibling].style) || isOutOfFlowPosition(nodes[sibling].style.position) ||
		       (isAnonymousTextNode(nodes[sibling]) && (nodes[sibling].style.white_space == 0 || nodes[sibling].style.white_space == 1) &&
		        nodes[sibling].text.find_first_not_of(" \t\r\n\f") == std::string::npos)))
			sibling = forward ? nodes[sibling].next_sibling : nodes[sibling].prev_sibling;
		if (sibling >= 0) sibling = formattingEdgeNode(nodes, sibling, !forward);
		return sibling >= 0 && !isLineBreak(nodes[sibling]) && LayoutEngine::isInlineLevelNode(nodes[sibling]);
	};
	return !adjacentInline(text.prev_sibling, false) || !adjacentInline(text.next_sibling, true);
}

// An inline wrapper shares its parent's line. Find a definite line boundary
// through preceding siblings and inline ancestors before measuring its first
// text run; padding does not turn leading collapsible whitespace into content.
bool startsFormattingLine(const Node *nodes, int id)
{
	while (LayoutEngine::isCssInlineLevelBox(nodes[id]) && LayoutEngine::isInlineLevelNode(nodes[id])) {
		for (int sibling = nodes[id].prev_sibling; sibling >= 0; sibling = nodes[sibling].prev_sibling) {
			const Node &previous = nodes[sibling];
			if (isDisplayNone(previous.style) || isOutOfFlowPosition(previous.style.position) || previous.style.float_side)
				continue;
			if (isAnonymousTextNode(previous) &&
			    (previous.style.white_space == 0 || previous.style.white_space == 1 || previous.style.white_space == 4) &&
			    previous.text.find_first_not_of(" \t\r\n\f") == std::string::npos) continue;
			const int edge = formattingEdgeNode(nodes, sibling, true);
			if (edge < 0) continue;
			return isLineBreak(nodes[edge]) || !LayoutEngine::isInlineLevelNode(nodes[edge]);
		}
		id = nodes[id].parent;
		if (id < 0) return true;
	}
	return true;
}

struct FlexLine {
	int start = 0;
	int count = 0;
	int mainSize = 0;
	int crossSize = 0;
	int strutSize = 0;
};

struct InlineFlowExtent { int width, height; int firstLineHeight = 0; };

struct FirstLineFragmentCandidate {
	int node = -1;
	int x = 0;
	int width = 0;
	int height = 0;
	int itemIndex = -1;
	int trailingSpace = 0;
};

class LayoutScratch {
public:
	static LayoutScratch &instance()
	{
		static LayoutScratch scratch;
		return scratch;
	}

	int enter()
	{
		const int current = depth_;
		depth_++;
		return current;
	}

	void leave()
	{
		if (depth_ > 0) depth_--;
	}

	int *childrenForDepth(int depth)
	{
		if (depth < 0 || depth >= kScratchDepth) return nullptr;
		if (!children_) children_ = static_cast<int *>(allocate(sizeof(int) * kScratchDepth * kMaxChildren));
		if (!children_) return nullptr;
		return children_ + depth * kMaxChildren;
	}

	FlexLine *linesForDepth(int depth)
	{
		if (depth < 0 || depth >= kScratchDepth) return nullptr;
		if (!lines_) lines_ = static_cast<FlexLine *>(allocate(sizeof(FlexLine) * kScratchDepth * kMaxFlexLines));
		if (!lines_) return nullptr;
		return lines_ + depth * kMaxFlexLines;
	}

private:
	static void *allocate(size_t size)
	{
		return gea::framework::memory::Allocator::allocatePreferSpiram(size);
	}

	int depth_ = 0;
	int *children_ = nullptr;
	FlexLine *lines_ = nullptr;
};

class ScratchFrame {
public:
	ScratchFrame()
		: scratch_(LayoutScratch::instance()), depth_(scratch_.enter())
	{
		children_ = scratch_.childrenForDepth(depth_);
		lines_ = scratch_.linesForDepth(depth_);
	}

	~ScratchFrame()
	{
		scratch_.leave();
	}

	bool valid() const
	{
		return depth_ >= 0 && depth_ < kScratchDepth && children_ && lines_;
	}

	int *children() const { return children_; }
	FlexLine *lines() const { return lines_; }

private:
	LayoutScratch &scratch_;
	int depth_ = 0;
	int *children_ = nullptr;
	FlexLine *lines_ = nullptr;
};

int resolvePercentSize(int basis, int percent)
{
	const long long numerator = static_cast<long long>(basis) * percent;
	return static_cast<int>((numerator + (numerator >= 0 ? 500 : -500)) / 1000);
}

// Probe the existing formatting algorithms without rewriting authored style.
// A stack of constraints keeps nested measurements finite and allocates no
// per-node storage; a final phase supplies the selected used border-box size.
struct IntrinsicSizeScope;
IntrinsicSizeScope *gIntrinsicSizeScope = nullptr;
struct IntrinsicSizeScope {
	const Node &node;
	bool horizontal;
	int constraint; // 1=min-content, 2=max-content, 0=final used size
	int borderSize = 0;
	IntrinsicSizeScope *previous;
	IntrinsicSizeScope(const Node &n, int mode, bool axis = true) : node(n), horizontal(axis), constraint(mode), previous(gIntrinsicSizeScope) { gIntrinsicSizeScope = this; }
	~IntrinsicSizeScope() { gIntrinsicSizeScope = previous; }
};
IntrinsicSizeScope *intrinsicSizeScope(const Node &node, bool horizontal)
{
	for (auto *scope = gIntrinsicSizeScope; scope; scope = scope->previous)
		if (&scope->node == &node && scope->horizontal == horizontal) return scope;
	return nullptr;
}
int intrinsicSizeConstraint(const Node &node, bool horizontal)
{
	if (!gIntrinsicSizeScope) return 0;
	const Node *current = &node;
	for (;;) {
		if (auto *scope = intrinsicSizeScope(*current, horizontal)) return scope->constraint;
		const int expression = horizontal ? current->style.width_expression : current->style.height_expression;
		if (current->parent < 0 || (horizontal ? current->style.width : current->style.height) != kUnset ||
		    (expression >= 0 && !layoutSizeExpressionUsesPercentage(
		        static_cast<int>(current - Tree::instance().nodes()), expression))) return 0;
		current = &Tree::instance().nodes()[current->parent];
	}
}
int intrinsicWidthConstraint(const Node &node) { return intrinsicSizeConstraint(node, true); }
// The flex basis replaces the preferred main size for item measurement without
// changing authored style. An unresolved percentage measures content, even when
// the item also has a preferred width/height.
struct FlexBasisScope;
FlexBasisScope *gFlexBasisScope = nullptr;
struct FlexBasisScope {
	const Node &node;
	bool horizontal;
	int value;
	FlexBasisScope *previous;
	FlexBasisScope(const Node &n, bool h, int v) : node(n), horizontal(h), value(v), previous(gFlexBasisScope) { gFlexBasisScope = this; }
	~FlexBasisScope() { gFlexBasisScope = previous; }
};
const FlexBasisScope *flexBasisScope(const Node &node, bool horizontal)
{
	for (auto *scope = gFlexBasisScope; scope; scope = scope->previous)
		if (&scope->node == &node && scope->horizontal == horizontal) return scope;
	return nullptr;
}
bool hasFlexBasis(const Node &node)
{
	return node.style.flex_basis != kUnset || rstyle(node.style).flex_basis_expression >= 0;
}

bool hasExplicitWidth(const Node &node)
{
	if (const auto *basis = flexBasisScope(node, true)) return basis->value != kUnset;
	if (const auto *scope = intrinsicSizeScope(node, true); scope && scope->constraint == 0) return true;
	if (isIntrinsicSizeExpression(node.style.width_expression)) {
		const auto *scope = intrinsicSizeScope(node, true);
		return scope && scope->constraint == 0;
	}
	if (node.style.width != kUnset) return true;
	if (intrinsicWidthConstraint(node)) return false;
	return node.style.width_expression >= 0 || node.style.width_percent != kUnset;
}

int clampLayoutSize(const Node &node, int size, bool horizontal)
{
	const auto *scope = intrinsicSizeScope(node, horizontal);
	// The property's intrinsic size is measured first; its author's min/max
	// constraints apply to the final used size, after resolving box edges.
	if (scope && scope->constraint) return std::max(boxInsets(node.style, horizontal), size);
	return clampBorderBoxSize(node.style, size, horizontal);
}

int16_t clampInt16(int value)
{
	if (value > 32767) return 32767;
	if (value < -32768) return -32768;
	return static_cast<int16_t>(value);
}

bool percentageHeightDependsOnIntrinsicSize(const Node &node)
{
	if (isOutOfFlowPosition(node.style.position)) return false;
	const Node *current = &node;
	while (current->parent >= 0) {
		const Node &parent = Tree::instance().nodes()[current->parent];
		if (isIntrinsicSizeExpression(parent.style.height_expression)) return true;
		if (parent.style.height != kUnset || isOutOfFlowPosition(parent.style.position)) return false;
		if (parent.style.height_expression >= 0 &&
		    !layoutSizeExpressionUsesPercentage(current->parent, parent.style.height_expression)) return false;
		current = &parent;
	}
	return false;
}

bool hasExplicitHeight(const Node &node)
{
	if (const auto *basis = flexBasisScope(node, false)) return basis->value != kUnset;
	if (const auto *scope = intrinsicSizeScope(node, false); scope && scope->constraint == 0) return true;
	if (node.style.height != kUnset) return true;
	if (intrinsicSizeConstraint(node, false)) return false;
	if (node.style.height_expression < 0 && node.style.height_percent == kUnset) return false;
	if (!percentageHeightDependsOnIntrinsicSize(node)) return true;
	return node.style.height_percent == kUnset &&
	       !layoutSizeExpressionUsesPercentage(static_cast<int>(&node - Tree::instance().nodes()), node.style.height_expression);
}

int resolvedStyleWidth(const Node &node, int basis)
{
	if (const auto *scope = flexBasisScope(node, true)) return scope->value == kUnset ? basis : scope->value;
	if (const auto *scope = intrinsicSizeScope(node, true); scope && scope->constraint == 0)
		return std::max(0, scope->borderSize - (node.style.box_sizing == 0 ? boxInsets(node.style, true) : 0));
	if (!hasExplicitWidth(node)) return basis;
	if (node.style.width_expression >= 0) return resolveLayoutSizeExpression(static_cast<int>(&node - Tree::instance().nodes()), node.style.width_expression, true);
	if (node.style.width != kUnset) return node.style.width;
	if (node.style.width_percent != kUnset) return resolvePercentSize(basis, node.style.width_percent);
	return basis;
}

int resolvedStyleHeight(const Node &node, int basis)
{
	if (const auto *scope = flexBasisScope(node, false)) return scope->value == kUnset ? basis : scope->value;
	if (const auto *scope = intrinsicSizeScope(node, false); scope && scope->constraint == 0)
		return std::max(0, scope->borderSize - (node.style.box_sizing == 0 ? boxInsets(node.style, false) : 0));
	if (!hasExplicitHeight(node)) return basis;
	if (node.style.height_expression >= 0) return resolveLayoutSizeExpression(static_cast<int>(&node - Tree::instance().nodes()), node.style.height_expression, false);
	if (node.style.height != kUnset) return node.style.height;
	if (node.style.height_percent != kUnset && !percentageHeightDependsOnIntrinsicSize(node))
		return resolvePercentSize(basis, node.style.height_percent);
	return basis;
}

int writingMode(const Node &node)
{
	const Node *current = &node;
	while (current->style.writing_mode < 0 && current->parent >= 0)
		current = &Tree::instance().nodes()[current->parent];
	return current->style.writing_mode < 0 ? 0 : current->style.writing_mode;
}

bool rightToLeft(const Node &node)
{
	const Node *current = &node;
	while (current->style.direction < 0 && current->parent >= 0)
		current = &Tree::instance().nodes()[current->parent];
	return current->style.direction == 1;
}

struct GridAxisPlacement {
	int start = kUnset, end = kUnset, span = 1;
};

GridAxisPlacement gridAxisPlacement(int start, int end, int explicitTracks, bool absolute)
{
	GridAxisPlacement p;
	const bool startSpan = start >= kGridLineSpan, endSpan = end >= kGridLineSpan;
	p.span = std::min(kMaxGridLayoutTracks, startSpan ? start - kGridLineSpan : endSpan ? end - kGridLineSpan : 1);
	auto line = [explicitTracks](int n) { return n > 0 ? n - 1 : explicitTracks + 1 + n; };
	if (start && !startSpan) p.start = line(start);
	if (end && !endSpan) p.end = line(end);
	if (p.start != kUnset && p.end != kUnset) {
		if (p.start > p.end) std::swap(p.start, p.end);
		if (p.start == p.end) p.end = kUnset;
	}
	if (p.start != kUnset && (endSpan || !absolute) && p.end == kUnset) p.end = p.start + p.span;
	if (p.end != kUnset && (startSpan || !absolute) && p.start == kUnset) p.start = p.end - p.span;
	if (p.start != kUnset && p.end != kUnset) p.span = std::min(kMaxGridLayoutTracks, p.end - p.start);
	return p;
}

bool gridAxisReversed(const Node &parent, bool horizontal)
{
	const int mode = writingMode(parent);
	const bool inlineAxis = horizontal == (mode == 0);
	return inlineAxis ? rightToLeft(parent) != (mode == 4) : mode == 2 || mode == 3;
}

bool flexRowDirection(const ComputedStyle &style);

double preferredRatio(const Node &node)
{
	float ratio = 0;
	const int bits = rstyle(node.style).aspect_ratio;
	std::memcpy(&ratio, &bits, sizeof(ratio));
	if (!std::isfinite(ratio)) return 0;
	if (ratio < 0 && node.type == NodeType::Image && node.image_id >= 0) return 0;
	return std::abs(static_cast<double>(ratio));
}

int ratioTransferredSize(const Node &node, int crossBorderSize, bool horizontal)
{
	const double ratio = preferredRatio(node);
	if (ratio <= 0) return 0;
	const int crossEdges = node.style.box_sizing ? 0 : boxInsets(node.style, !horizontal);
	const int mainEdges = node.style.box_sizing ? 0 : boxInsets(node.style, horizontal);
	const double value = std::max(0, crossBorderSize - crossEdges) * (horizontal ? ratio : 1 / ratio) + mainEdges;
	return static_cast<int>(std::min(32767.0, std::round(value)));
}

bool definiteFlexCrossSize(const Node &parent, const Node &child, bool row)
{
	const int alignment = child.style.align_self >= 0 ? child.style.align_self : parent.style.align_items;
	const int margins = row ? 5 : 10;
	return parent.style.flex_wrap == 0 && alignment == 0 && !(child.style.margin_auto & margins) &&
	       (row ? hasExplicitHeight(parent) : hasExplicitWidth(parent));
}

void applyPreferredRatio(Node &node, int availableWidth, int availableHeight)
{
	if (!preferredRatio(node) || (node.type != NodeType::View && node.type != NodeType::Image)) return;
	if (node.type != NodeType::Image && LayoutEngine::isInlineLevelNode(node)) return;
	bool width = hasExplicitWidth(node), height = hasExplicitHeight(node);
	if (width && height) return;
	const Node *parent = node.parent >= 0 ? &Tree::instance().nodes()[node.parent] : nullptr;
	const bool flexItem = parent && parent->style.display == kDisplayFlex;
	if (flexItem && !width && !height) {
		const bool row = flexRowDirection(parent->style) != (writingMode(*parent) != 0);
		if (definiteFlexCrossSize(*parent, node, row)) {
			if (row) {
				node.layout.height = clampLayoutSize(node, availableHeight - node.style.margin[0] - node.style.margin[2], false);
				height = true;
			} else {
				node.layout.width = clampLayoutSize(node, availableWidth - node.style.margin[1] - node.style.margin[3], true);
				width = true;
			}
		}
	}
	if (!width && !height) {
		// Normal block flow supplies a definite inline size. Shrink-to-fit
		// contexts keep the intrinsic width measured from their contents.
		if (!flexItem && !node.style.float_side && !isOutOfFlowPosition(node.style.position))
			node.layout.width = clampLayoutSize(node, availableWidth - node.style.margin[1] - node.style.margin[3], true);
		width = true;
	}
	const bool horizontal = !width && height;
	int16_t &target = horizontal ? node.layout.width : node.layout.height;
	const int fromRatio = ratioTransferredSize(node, horizontal ? node.layout.height : node.layout.width, horizontal);
	// Non-scrollable boxes retain a content-based minimum on the automatic axis.
	const bool contentMinimum = node.type != NodeType::Image && (horizontal ? node.style.min_width : node.style.min_height) == kUnset &&
	                            !isScrollableOverflow(horizontal ? overflowX(node.style) : overflowY(node.style));
	const int result = contentMinimum ? std::max<int>(target, fromRatio) : fromRatio;
	target = clampInt16(clampLayoutSize(node, result, horizontal));
}

int baselineFallbackAlignment(const Node &parent, const Node &child, int alignment, int free, bool horizontal, bool staticPosition)
{
	// Baseline self-alignment follows the subject's writing mode. Resolving
	// automatic insets uses the fallback edge without overflow safety; actual
	// in-flow self-alignment uses the safe fallback instead (CSS Position 3.5.1).
	if (!staticPosition && free < 0)
		return gridAxisReversed(parent, horizontal) ? 2 : 6;
	const bool last = (alignment & 15) == kAlignLastBaseline;
	return gridAxisReversed(child, horizontal) != last ? 2 : 6;
}

// Self-relative edges use the subject's writing mode, even when it differs
// from the containing box. Return physical start/end; flex positioning may
// subsequently express it relative to its reversed cross axis.
int physicalSelfAlignment(const Node &parent, const Node &child, int alignment, int free, bool horizontal)
{
	const int keyword = alignment & 15;
	if (keyword != kAlignSelfStart && keyword != kAlignSelfEnd && keyword != kAlignLeft && keyword != kAlignRight) return -1;
	if ((alignment & kAlignSafe) && free < 0) return gridAxisReversed(parent, horizontal) ? 2 : 6;
	if (keyword == kAlignLeft || keyword == kAlignRight) {
		if (horizontal) return keyword == kAlignRight ? 2 : 6;
		// In vertical text, left/right are line-relative and independent of
		// bidi direction. Sideways-lr alone has line-left at physical bottom.
		const int mode = writingMode(parent);
		if (mode != 0) return (mode == 4) != (keyword == kAlignRight) ? 2 : 6;
		return 6;
	}
	return gridAxisReversed(child, horizontal) != (keyword == kAlignSelfEnd) ? 2 : 6;
}

int usedGridAlignment(const Node &parent, const Node &child, int alignment, int free, bool horizontal, bool staticPosition = false)
{
	const int self = physicalSelfAlignment(parent, child, alignment, free, horizontal);
	if (self >= 0) return self;
	const int keyword = alignment & 15;
	if (keyword == 5 || keyword == kAlignLastBaseline)
		return baselineFallbackAlignment(parent, child, alignment, free, horizontal, staticPosition);
	if (keyword == 0 || keyword == 6) alignment = (alignment & ~15) | kAlignStart;
	else if (keyword == 2) alignment = (alignment & ~15) | kAlignEnd;
	return usedAlignment(alignment, free, gridAxisReversed(parent, horizontal));
}

void gridPhysicalRange(const Node &parent, bool columns, int start, int end, int &offset, int &size)
{
	const bool horizontal = columns == (writingMode(parent) == 0);
	const int available = (horizontal ? parent.layout.width : parent.layout.height) - boxInsets(parent.style, horizontal);
	offset = boxInset(parent.style, horizontal ? 3 : 0) + (gridAxisReversed(parent, horizontal) ? available - end : start);
	size = std::max(0, end - start);
}

bool flexRowDirection(const ComputedStyle &style)
{
	return !style.flex_direction_explicit || (style.flex_direction & 1);
}

int resolvedGap(const Node &node, bool rowGap, int width, int height)
{
	const int length = rowGap ? node.style.row_gap : node.style.column_gap;
	const int percent = rowGap ? node.style.row_gap_percent : node.style.column_gap_percent;
	const bool vertical = writingMode(node) != 0;
	const bool horizontalAxis = rowGap == vertical;
	if (percent != kUnset) {
		// Cyclic percentage gaps have zero used size in an indefinite flex axis.
		if (horizontalAxis ? !hasExplicitWidth(node) : !hasExplicitHeight(node)) return 0;
		return std::max(0, resolvePercentSize(horizontalAxis ? width : height, percent));
	}
	return std::max(0, length == kUnset ? node.style.gap : length);
}

int clampBorderSize(LayoutEngine &engine, const Node &node, int size, bool horizontal)
{
	return clampLayoutSize(node, size, horizontal);
}

class FlexLayoutPass {
public:
	FlexLayoutPass(LayoutEngine &engine, Node &node, int *children, int childCount, bool isRow, int mainAvail, int padWidth, int padHeight, FlexLine *lines, bool inlineRow = false, bool assignedSize = false)
		: engine_(engine),
		  node_(node),
		  children_(children),
		  childCount_(childCount),
		  isRow_(isRow),
		  inlineRow_(inlineRow && isRow),
		  assignedSize_(assignedSize),
		  mainAvail_(mainAvail),
		  padWidth_(padWidth),
		  padHeight_(padHeight),
		  lines_(lines)
	{
		const bool logicalRow = node.style.display == kDisplayFlex ? flexRowDirection(node.style) : isRow;
		mainGap_ = resolvedGap(node, !logicalRow, padWidth, padHeight);
		crossGap_ = resolvedGap(node, logicalRow, padWidth, padHeight);
		// Most containers fit the shared 32-line storage. Allocate extra lines
		// only for a wrapping container that can actually exceed it.
		if (node.style.display == kDisplayFlex && node.style.flex_wrap && childCount > kMaxFlexLines) {
			extraLines_.resize(childCount);
			lines_ = extraLines_.data();
		}
	}

	void measureChildren()
	{
		for (int i = 0; i < childCount_; i++) {
			const int child = children_[i];
			int childAvailWidth = isRow_ ? mainAvail_ : padWidth_;
			int childAvailHeight = isRow_ ? padHeight_ : mainAvail_;
			const int count = rstyle(node_.style).flex_line_count;
			const Node &item = Tree::instance().nodes()[child];
			if (node_.style.display == kDisplayFlex && node_.style.flex_wrap && count > 1) {
				// The specified count limits available cross space even for normal
				// wrapping. Percentage cross sizes retain the container's basis.
				const auto available = [&](int size) {
					return static_cast<int>(std::max<std::int64_t>(0, (static_cast<std::int64_t>(size) -
					                        static_cast<std::int64_t>(count - 1) * crossGap_) / count));
				};
				if (isRow_ && hasExplicitHeight(node_) && !hasExplicitHeight(item)) childAvailHeight = available(childAvailHeight);
				if (!isRow_ && !hasExplicitWidth(item)) childAvailWidth = available(childAvailWidth);
			}
			if (node_.style.display == kDisplayFlex && hasFlexBasis(item)) {
				const int basis = resolveLayoutFlexBasis(child, flexPercentageBasis(), isRow_);
				FlexBasisScope scope(item, isRow_, basis);
				engine_.layoutNode(child, childAvailWidth, childAvailHeight);
			} else {
				engine_.layoutNode(child, childAvailWidth, childAvailHeight);
			}
			if (node_.style.display == kDisplayFlex) {
				applyFlexBasis(child);
				Node &item = Tree::instance().nodes()[child];
				const int floor = automaticMinimumMainSize(item);
				int16_t &main = isRow_ ? item.layout.width : item.layout.height;
				if (main < floor) { main = clampInt16(floor); engine_.repositionChildren(child); }
			}
		}
	}

	void layoutLines(bool autosize)
	{
		const int originalWidth = node_.layout.width, originalHeight = node_.layout.height;
		int lineCount = buildLines();
		expandCrossSizes(lineCount, autosize);
		if (autosize) autosizeParent(lineCount);
		positionLines(lineCount);
		if (node_.style.display != kDisplayFlex) return;
		Node *nodes = Tree::instance().nodes();
		bool collapsed = false;
		for (int i = 0; i < childCount_; ++i) collapsed |= isCollapsedFlexItem(nodes[children_[i]]);
		if (!collapsed) return;

		// Flexbox 9.4: remember the original (including stretched) line size,
		// then restart with zero-main-size struts. Compact the child list only
		// AFTER assigning struts to lines, so allocation, gaps, auto margins,
		// baselines and alignment all operate on the surviving items alone.
		std::vector<int> struts(childCount_, -1);
		for (int line = 0; line < lineCount; ++line)
			for (int i = lines_[line].start; i < lines_[line].start + lines_[line].count; ++i)
				if (isCollapsedFlexItem(nodes[children_[i]])) struts[i] = lines_[line].crossSize;
		node_.layout.width = originalWidth;
		node_.layout.height = originalHeight;
		measureChildren();
		lineCount = buildCollapsedLines(struts);
		expandCrossSizes(lineCount, autosize);
		if (autosize) autosizeParent(lineCount);
		positionLines(lineCount);
	}

	// Flex line packing. An inline formatting context does NOT come through here —
	// layoutInlineFlow owns it — because flex lines cannot express one: their items
	// are atomic, so a text run may neither share a line with its neighbours nor
	// split across two.
	int buildLines()
	{
		if (node_.style.display == kDisplayFlex && (node_.style.flex_wrap & 4)) return buildBalancedLines();
		int lineStart = 0;
		int lineMain = 0;
		int lineCross = 0;
		int gapCount = 0;
		int lineCount = 0;
		Node *nodes = Tree::instance().nodes();

		for (int i = 0; i < childCount_; i++) {
			const int child = children_[i];
			Node &childNode = nodes[child];
			const int childMain = isRow_
				? (childNode.layout.width + childNode.style.margin[1] + childNode.style.margin[3])
				: (childNode.layout.height + childNode.style.margin[0] + childNode.style.margin[2]);
			const int childCross = isRow_
				? (childNode.layout.height + childNode.style.margin[0] + childNode.style.margin[2])
				: (childNode.layout.width + childNode.style.margin[1] + childNode.style.margin[3]);

			const int withGap = gapCount > 0 ? mainGap_ : 0;
			if (node_.style.display == kDisplayFlex && node_.style.flex_wrap && lineMain + childMain + withGap > flexWrapLimit() && gapCount > 0) {
				appendLine(lineCount, lineStart, gapCount, lineMain, lineCross);
				lineStart = i;
				lineMain = childMain;
				lineCross = childCross;
				gapCount = 1;
			} else {
				lineMain += childMain + withGap;
				if (childCross > lineCross) lineCross = childCross;
				gapCount++;
			}
		}

		if (gapCount > 0) appendLine(lineCount, lineStart, gapCount, lineMain, lineCross);
		return lineCount;
	}

	// Lays this node's children out as a sequence of LINE BOXES — a real inline
	// formatting context. Flex line packing cannot express one: it treats every
	// item as ATOMIC, so a text run that overflows the space left on a line is
	// pushed whole onto the next one and the line before it ends far short of the
	// right edge (typography's mixed-composition paragraphs lost ~40% of every
	// line to that). CSS instead lets a run START on the line box it inherits,
	// wrap its remainder at the block's left edge, and hand the pen to the next
	// box on its LAST line.
	//
	// One forward walk. Per line box it first SCANS ahead to learn which items
	// share it — and therefore the line's ascent, and so its baseline — then
	// places them. The scans partition the item list, so the pass is linear.
	InlineFlowExtent layoutInlineFlow(bool autosize, bool anonymous = false)
	{
		Node *nodes = Tree::instance().nodes();
		const int parentId = static_cast<int>(&node_ - nodes);
		int firstLineStyleOwner = -1;
		for (int ancestor = parentId; ancestor >= 0; ancestor = nodes[ancestor].parent) {
			const NodeRareData *rare = rareDataFor(ancestor);
			if (rare && rare->firstLineBackground.hasColor) {
				firstLineStyleOwner = ancestor;
				break;
			}
			// Inline boxes carry the rule down their descendants. A block child
			// carries it only when it is the first in-flow formatted child of its
			// parent; following block siblings start after the owner's first line.
			if (!LayoutEngine::isCssInlineLevelBox(nodes[ancestor])) {
				if (isOutOfFlowPosition(nodes[ancestor].style.position) || nodes[ancestor].style.float_side) break;
				const int parent = nodes[ancestor].parent;
				if (parent < 0) break;
				if (nodes[parent].style.display == kDisplayFlex || isDisplayGrid(nodes[parent].style)) break;
				bool firstInFlow = true;
				for (int sibling = nodes[parent].first_child; sibling >= 0 && sibling != ancestor;
				     sibling = nodes[sibling].next_sibling) {
					if (!isDisplayNone(nodes[sibling].style) &&
					    !isOutOfFlowPosition(nodes[sibling].style.position) &&
					    !nodes[sibling].style.float_side &&
					    !suppressAnonymousWhitespace(nodes, parent, sibling)) {
						firstInFlow = false;
						break;
					}
				}
				if (!firstInFlow) break;
			}
		}
		const bool captureFirstLine = firstLineStyleOwner >= 0;
		// Runs that leave this formatting context must not keep an old fragment
		// rectangle. A nested inline flow may write its own first-line fragment
		// later in the same layout pass.
		for (int i = 0; i < childCount_; ++i) {
			const int child = children_[i];
			if (NodeRareData *rare = rareDataFor(child)) rare->firstLineFragment.valid = false;
			const int run = transparentInlineRun(nodes[child]);
			if (run >= 0) if (NodeRareData *rare = rareDataFor(run)) rare->firstLineFragment.valid = false;
		}
		std::vector<std::pair<int, int>> staticBoundaries;
		int flowIndex = 0;
		for (int child = node_.first_child; child >= 0; child = nodes[child].next_sibling) {
			if (isDisplayNone(nodes[child].style) || suppressAnonymousWhitespace(nodes, parentId, child)) continue;
			if (isOutOfFlowPosition(nodes[child].style.position)) {
				if (auto *rare = rareDataFor(child)) rare->inlineStaticPosition.valid = false;
				if (LayoutEngine::isCssInlineLevelBox(nodes[child], true))
					staticBoundaries.emplace_back(flowIndex, child);
				continue;
			}
			while (flowIndex < childCount_ && children_[flowIndex] != child) ++flowIndex;
			if (flowIndex < childCount_) ++flowIndex;
		}
		int contentW = mainAvail_ < 0 ? 0 : mainAvail_;
		if (intrinsicWidthConstraint(node_) == 1) {
			int pendingWord = 0;
			for (int i = 0; i < childCount_; ++i) {
				const Node &child = nodes[children_[i]];
				if (isLineBreak(child)) { pendingWord = 0; continue; }
				const int wrappedRun = transparentInlineRun(child);
				if (child.type == NodeType::Text || wrappedRun >= 0) {
					contentW = std::max(contentW, TextRenderer::minContentWidth(wrappedRun >= 0 ? nodes[wrappedRun] : child, &pendingWord));
				} else {
					contentW = std::max(contentW, child.layout.width + child.style.margin[1] + child.style.margin[3]);
					pendingWord = 0;
				}
			}
		}
		const int contentLeft = boxInset(node_.style, 3);
		const int contentTop = boxInset(node_.style, 0);
		const int gapStyle = mainGap_;

		const bool blockOpensItsFirstLine = startsFormattingLine(nodes, parentId);
		int i = 0;
		int lineTop = contentTop;
		int flowBottom = contentTop;
		int maxLineWidth = 0;
		int firstLineHeight = 0;
		// Carried across a wrapped run: the pen its last line ended at, that
		// line's already-fixed baseline, and the ascent/descent it contributes.
		int penStart = 0;
		bool continuing = false;
		int continuedBaseline = 0;
		int inheritedAscent = 0;
		int inheritedDescent = 0;
		std::vector<bool> staticCaptured(staticBoundaries.size(), false);
		std::vector<FirstLineFragmentCandidate> firstLineCandidates;
		auto captureStaticBoundary = [&](int boundary, int pen, std::vector<int> &captured,
		                                 bool continuationLine = false) {
			for (std::size_t k = 0; k < staticBoundaries.size(); ++k) {
				if (staticCaptured[k] || staticBoundaries[k].first != boundary) continue;
				staticCaptured[k] = true;
				captured.push_back(staticBoundaries[k].second);
				NodeRareData &rare = ensureRareData(staticBoundaries[k].second);
				rare.inlineStaticPosition.continuationLine =
				    continuationLine && LayoutEngine::isCssInlineLevelBox(node_);
				rare.inlineStaticPosition.x = clampInt16(
				    (rare.inlineStaticPosition.continuationLine ? 0 : contentLeft) + pen);
			}
		};

		while (i < childCount_) {
			// A line box that opens as the tail of a wrapped run already has content
			// even if no further item joins it.
			const bool lineIsContinuation = continuing;
			int pen = penStart;
			int lineAscent = inheritedAscent;
			int lineDescent = inheritedDescent;
			int j = i;
			int wrapIdx = -1;
			int wrapSpans = 0;
			int wrapAdvance = 0;
			int wrapLastWidth = 0;
			std::vector<int> lineStaticChildren;

			for (; j < childCount_; j++) {
				captureStaticBoundary(j, pen, lineStaticChildren, lineIsContinuation || i > 0);
				const int child = children_[j];
				Node &cn = nodes[child];
				const int marginL = cn.style.margin[3];
				const int marginR = cn.style.margin[1];
				const int gap = pen > 0 ? gapStyle : 0;

				if (isLineBreak(cn)) {
					// A BR closes this line even under white-space:nowrap/pre.
					// It contributes its inherited line metrics to an otherwise
					// empty line, but does not create a trailing phantom line.
					lineAscent = std::max(lineAscent, childBaseline(cn));
					lineDescent = std::max(lineDescent, cn.layout.height - childBaseline(cn));
					++j;
					break;
				}

				// The run this item contributes to the line: itself when it IS a text
				// run, or the one inside a transparent <span> wrapper.
				const int innerRun = isFragmentableRun(cn) ? -1 : transparentInlineRun(cn);
				Node &subject = innerRun >= 0 ? nodes[innerRun] : cn;
				if (isFragmentableRun(subject)) {
					const int padH = boxInset(subject.style, 1) + boxInset(subject.style, 3);
					const int padV = boxInset(subject.style, 0) + boxInset(subject.style, 2);
					// Budgets are TEXT widths: the run's own padding is part of its box,
					// not of the line it can fill.
					const int rest = contentW - marginL - marginR - padH;
					const int firstAvail = rest - pen - gap;
					const bool atLineStart = pen == 0 && (j > 0 || blockOpensItsFirstLine);
					const InlineFlowMeasure m =
					    TextRenderer::measureInlineFlow(subject, firstAvail, rest, atLineStart);
					// Without real line metrics childFirstBaseline reports the box's
					// bottom edge, which is only a baseline for a ONE-line box. Such a
					// run keeps flowing whole, exactly as before.
					const bool haveLineMetrics =
					    m.lineAdvance > 0 &&
					    childBaseline(subject) <= boxInset(subject.style, 0) + m.lineAdvance;
					if (m.lineCount > 0 && haveLineMetrics) {
						// Its first WORD does not fit what is left: the run starts the
						// next line box rather than overflowing it or being cut open.
						if (pen > 0 && m.firstLineWidth > firstAvail) break;
						const int boxH = m.lineCount * m.lineAdvance + padV;
						const bool wraps = m.lineCount > 1;
						const int boxW = wraps ? rest + padH : m.firstLineWidth + padH;
						const int indent =
						    (wraps ? pen + gap : 0) + m.firstLineIndentAdjust;
						const bool firstLineFragment = captureFirstLine && i == 0 && m.firstLineWidth > 0;
						const int fragmentItemX = wraps ? contentLeft + marginL : contentLeft + pen + gap + marginL;
						// The text-command indent compensates for collapsed leading
						// whitespace; that discarded space is not part of the fragment.
						const int firstLineFragmentX = fragmentItemX + (wraps ? pen + gap : 0);
						const int firstLineFragmentWidth = m.firstLineWidth + padH;
						subject.layout.height = clampInt16(boxH);
						subject.layout.width = clampInt16(boxW);
						subject.layout.inline_indent = clampInt16(indent);
						if (innerRun >= 0) {
							// The wrapper is transparent: it takes the run's box exactly, and
							// the run sits at its origin, so childFirstBaseline's recursion
							// through the wrapper still lands on the run's own baseline.
							subject.layout.x = 0;
							subject.layout.y = 0;
							cn.layout.height = clampInt16(boxH);
							cn.layout.width = clampInt16(boxW);
							cn.layout.inline_indent = clampInt16(indent);
						}
						if (wraps) {
							const int ascent = cn.style.margin[0] + childBaseline(cn);
							const int descent = runLineDescent(subject, m.lineAdvance);
							if (ascent > lineAscent) lineAscent = ascent;
							if (descent > lineDescent) lineDescent = descent;
							pen += gap + marginL + m.firstLineWidth + marginR + padH;
							if (m.maxLineWidth + padH > maxLineWidth) maxLineWidth = m.maxLineWidth + padH;
							wrapIdx = j;
							wrapSpans = m.lineCount;
							wrapAdvance = m.lineAdvance;
							wrapLastWidth = m.lastLineWidth + padH;
							if (firstLineFragment)
								firstLineCandidates.push_back({innerRun >= 0 ? innerRun : child,
								                              firstLineFragmentX, firstLineFragmentWidth,
								                              m.lineAdvance + padV, j, m.firstLineTrailingSpace});
							j++;
							break;  // the run closes this line box
						}
					}
				} else {
					cn.layout.inline_indent = 0;
				}

				const int itemMain = cn.layout.width + marginL + marginR;
				if (pen > 0 && pen + gap + itemMain > contentW) break;
				if (isFragmentableRun(subject) && captureFirstLine && i == 0) {
					const int padH = boxInset(subject.style, 1) + boxInset(subject.style, 3);
					const int rest = contentW - marginL - marginR - padH;
					const InlineFlowMeasure m = TextRenderer::measureInlineFlow(
					    subject, rest - pen - gap, rest, pen == 0 && (j > 0 || blockOpensItsFirstLine));
					if (m.lineCount > 0 && m.firstLineWidth > 0) {
							firstLineCandidates.push_back({innerRun >= 0 ? innerRun : child,
								                              contentLeft + pen + gap + marginL,
								                              m.firstLineWidth + padH,
								                              m.lineAdvance + boxInset(subject.style, 0) + boxInset(subject.style, 2),
								                              j, m.firstLineTrailingSpace});
					}
				}
				const int ascent = cn.style.margin[0] + childBaseline(cn);
				const int descent = cn.layout.height + cn.style.margin[2] - childBaseline(cn);
				if (ascent > lineAscent) lineAscent = ascent;
				if (descent > lineDescent) lineDescent = descent;
				pen += gap + itemMain;
			}
			const bool endedWithBreak = j > i && isLineBreak(nodes[children_[j - 1]]);
			const bool trailingWrappedStaticBoundary = j >= childCount_ && !endedWithBreak && wrapIdx >= 0;
			if (j >= childCount_ && !endedWithBreak && !trailingWrappedStaticBoundary)
				captureStaticBoundary(childCount_, pen, lineStaticChildren, lineIsContinuation || i > 0);

			if (pen > maxLineWidth) maxLineWidth = pen;

			const int baseline = continuing ? continuedBaseline : lineTop + lineAscent;
			lineTop = baseline - lineAscent;
			for (int id : lineStaticChildren) {
				auto &position = ensureRareData(id).inlineStaticPosition;
				position.y = clampInt16(lineTop);
				position.valid = true;
			}
			int lineCross = lineAscent + lineDescent;
			if (lineCross < 0) lineCross = 0;
			const int actualLineCross = lineCross;
			if (i == 0) firstLineHeight = lineCross;
			// The one case the flex path's expandCrossSizes is observable here: a
			// lone line box in a block with a definite height fills it, so
			// `align-self: center/end` on an inline child has room to work.
			if (!anonymous && i == 0 && j >= childCount_ && wrapIdx < 0 && hasExplicitHeight(node_) &&
			    padHeight_ > lineCross)
				lineCross = padHeight_;

			placeLineItems(nodes, i, j, contentLeft, contentW, lineTop, lineCross, baseline, penStart,
			               !lineIsContinuation);
			if (i == 0 && captureFirstLine) {
				int firstFormattedLineHeight = actualLineCross;
				if (!firstLineCandidates.empty()) firstFormattedLineHeight = 0;
				for (const auto &candidate : firstLineCandidates)
					firstFormattedLineHeight = std::max(firstFormattedLineHeight, candidate.height);
				if (firstLineStyleOwner == parentId) {
					auto &style = ensureRareData(parentId).firstLineBackground;
					if (!style.lineValid) {
						style.lineY = clampInt16(lineTop);
						style.lineHeight = clampInt16(firstFormattedLineHeight);
						style.lineContextNode = static_cast<std::int16_t>(parentId);
						style.lineValid = actualLineCross > 0;
					}
				} else if (firstLineStyleOwner >= 0 && !LayoutEngine::isCssInlineLevelBox(node_)) {
					// A nested inline's local first line may ultimately land on a
					// later owner line. The owner's inline pass chooses that anchor;
					// only an eligible first block child can supply it from below.
					auto &style = ensureRareData(firstLineStyleOwner).firstLineBackground;
					if (!style.lineValid) {
						style.lineY = clampInt16(lineTop);
						style.lineHeight = clampInt16(firstFormattedLineHeight);
						style.lineContextNode = static_cast<std::int16_t>(parentId);
						style.lineValid = actualLineCross > 0;
					}
				}
				for (const auto &candidate : firstLineCandidates) {
					if (candidate.node < 0 || candidate.node >= Tree::instance().nodeCount()) continue;
					auto &fragment = ensureRareData(candidate.node).firstLineFragment;
					fragment.x = clampInt16(candidate.x);
					fragment.y = clampInt16(lineTop);
					// Spaces between inline runs remain part of the background;
					// collapsible spaces at the end of the line do not.
					const bool endsLine = candidate.itemIndex == j - (endedWithBreak ? 2 : 1);
					fragment.width = clampInt16(std::max(0, candidate.width - (endsLine ? candidate.trailingSpace : 0)));
					fragment.height = clampInt16(candidate.height);
					fragment.contextNode = static_cast<std::int16_t>(parentId);
					fragment.valid = fragment.width > 0 && fragment.height > 0;
				}
			}

			if (baseline + lineDescent > flowBottom) flowBottom = baseline + lineDescent;
			if (lineTop + lineCross > flowBottom) flowBottom = lineTop + lineCross;

			if (wrapIdx >= 0) {
				Node &run = nodes[children_[wrapIdx]];
				const int runBaseline = run.layout.y + childBaseline(run);
				continuedBaseline = runBaseline + (wrapSpans - 1) * wrapAdvance;
				inheritedAscent = run.style.margin[0] + childBaseline(run);
				inheritedDescent = runLineDescent(run, wrapAdvance);
				penStart = run.style.margin[3] + wrapLastWidth + run.style.margin[1];
				continuing = true;
				if (trailingWrappedStaticBoundary) {
					std::vector<int> trailingStaticChildren;
					captureStaticBoundary(childCount_, penStart, trailingStaticChildren, true);
					for (int id : trailingStaticChildren) {
						auto &position = ensureRareData(id).inlineStaticPosition;
						position.y = clampInt16(continuedBaseline - inheritedAscent);
						position.valid = true;
					}
				}
				if (continuedBaseline + inheritedDescent > flowBottom)
					flowBottom = continuedBaseline + inheritedDescent;
				if (penStart > maxLineWidth) maxLineWidth = penStart;
			} else {
				lineTop = baseline + lineDescent + gapStyle;
				penStart = 0;
				continuing = false;
				inheritedAscent = 0;
				inheritedDescent = 0;
			}
			if (j >= childCount_ && endedWithBreak) {
				std::vector<int> trailingStaticChildren;
				captureStaticBoundary(childCount_, 0, trailingStaticChildren, true);
				for (int id : trailingStaticChildren) {
					auto &position = ensureRareData(id).inlineStaticPosition;
					position.y = clampInt16(lineTop);
					position.valid = true;
				}
			}

			// A line box that took no item and inherited no run tail would spin. The
			// scan only produces that for an item wider than the whole box, so place
			// it anyway. A continuation line legitimately ends empty — its content is
			// the run's last line — and the item that did not fit opens the next one.
			if (j == i && !lineIsContinuation) j = i + 1;
			i = j;
		}
		if (childCount_ == 0 && !staticBoundaries.empty()) {
			std::vector<int> emptyStaticChildren;
			captureStaticBoundary(0, 0, emptyStaticChildren);
			for (int id : emptyStaticChildren) {
				auto &position = ensureRareData(id).inlineStaticPosition;
				position.y = clampInt16(contentTop);
				position.valid = true;
			}
		}

		const InlineFlowExtent extent{maxLineWidth, std::max(0, flowBottom - contentTop), firstLineHeight};
		if (!autosize) return extent;
		// Same rules as autosizeParent: a scroll container keeps the size it was
		// given on its scrolling axis so its overflow becomes scroll range.
		const int flowHeight = flowBottom - contentTop;
		const bool keepScrollHeight = scrollsOverflowY(node_.style) && node_.layout.height > 0;
		const bool keepScrollWidth = !intrinsicWidthConstraint(node_) && scrollsOverflowX(node_.style) && node_.layout.width > 0;
		if (!hasExplicitHeight(node_) && !keepScrollHeight) {
			const int height = (flowHeight < 0 ? 0 : flowHeight) + boxInset(node_.style, 0) + boxInset(node_.style, 2);
			node_.layout.height = clampInt16(clampLayoutSize(node_, height, false));
		}
		if (!hasExplicitWidth(node_) && !keepScrollWidth) {
			const int width = maxLineWidth + boxInset(node_.style, 1) + boxInset(node_.style, 3);
			node_.layout.width = clampInt16(clampLayoutSize(node_, width, true));
		}
		return extent;
	}

	// A text run whose line breaking this engine owns, and which therefore flows
	// into the block's line boxes instead of occupying one whole. An author-sized
	// or nowrap run keeps its own box.
	static bool isFragmentableRun(const Node &n)
	{
		return n.type == NodeType::Text && !n.text.empty() && n.style.white_space != 1 && n.style.white_space != 2 &&
		       n.style.width == kUnset && n.style.width_percent == kUnset;
	}

	// True when nothing about `n`'s BOX is visible — no background, border, shadow,
	// transform or blur. Such a wrapper can take whatever geometry inline flow finds
	// convenient, because only its content is ever painted.
	static bool paintsNoBox(const Node &n)
	{
		if (n.style.has_bg || n.style.border_width > 0 || hasSideBorder(n.style)) return false;
#if GEA_EMBEDDED_RARE_STYLE_INLINE
		const RareStyle &r = rstyle(n.style);
		if (r.translate_present || r.transform_present || r.rotate_present || r.scale_present || r.filter_present ||
		    r.transform_rotate || r.transform_rotate_x || r.transform_rotate_y ||
		    composedTranslateX(r) || composedTranslateY(r) || composedTranslateZ(r) ||
		    composedTranslateXPercent(r) || composedTranslateYPercent(r) ||
		    r.transform_scale_x != 1000 || r.transform_scale_y != 1000 || r.transform_scale_z != 1000 || r.perspective ||
		    r.box_shadow_alpha || r.filter_blur_radius)
			return false;
#else
		// No rare-style record at all: no transform, shadow, gradient or filter was
		// ever set on this node.
		if (n.style.rare_style >= 0) return false;
#endif
		return true;
	}

	// A <span> that only wraps text is TRANSPARENT to inline flow: CSS breaks lines
	// inside it exactly as if its content sat directly in the paragraph, which is
	// why `<p>… <span>score value</span> …</p>` may put "score" on one line and
	// "value" on the next. Returns that inner run, or -1 when the wrapper has a box
	// a reader could see, or holds anything other than one plain text run.
	static int transparentInlineRun(const Node &wrapper)
	{
		if (wrapper.type != NodeType::View) return -1;
		if (!paintsNoBox(wrapper)) return -1;
		if (boxInset(wrapper.style, 0) || boxInset(wrapper.style, 1) || boxInset(wrapper.style, 2) ||
		    boxInset(wrapper.style, 3))
			return -1;
		if (hasExplicitWidth(wrapper) || hasExplicitHeight(wrapper)) return -1;
		if (wrapper.style.min_width > 0 || wrapper.style.min_height > 0) return -1;
		if (wrapper.style.max_width != kUnset || wrapper.style.max_height != kUnset) return -1;
		if (wrapper.style.overflow_x || wrapper.style.overflow_y) return -1;
		if (wrapper.style.align_self >= 0 || wrapper.style.flex > 0 ||
		    hasFlexBasis(wrapper))
			return -1;
		const Node *nodes = Tree::instance().nodes();
		int run = -1;
		for (int c = wrapper.first_child; c >= 0; c = nodes[c].next_sibling) {
			const Node &child = nodes[c];
			if (isDisplayNone(child.style) || isOutOfFlowPosition(child.style.position)) continue;
			if (run >= 0) return -1;  // more than one in-flow child
			if (child.type != NodeType::Text) return -1;
			if (child.style.margin[0] || child.style.margin[1] || child.style.margin[2] ||
			    child.style.margin[3])
				return -1;
			if (!isFragmentableRun(child)) return -1;
			run = c;
		}
		return run;
	}

	// Distance from ONE of a run's line baselines to the bottom of that line box,
	// including the run's own bottom padding/margin (which only the last line
	// actually carries — the others are flush, so this is the taller, safe value).
	int runLineDescent(const Node &n, int lineAdvance) const
	{
		const int ascender = childBaseline(n) - boxInset(n.style, 0);
		const int descent = lineAdvance - ascender + boxInset(n.style, 2) + n.style.margin[2];
		return descent < 0 ? 0 : descent;
	}

	void placeLineItems(Node *nodes, int from, int to, int contentLeft, int contentW, int lineTop,
	                    int lineCross, int baseline, int penStart, bool ownsLineStart)
	{
		const int contentEnd = to > from && isLineBreak(nodes[children_[to - 1]]) ? to - 1 : to;
		const bool soleRun =
		    ownsLineStart && (contentEnd - from) == 1 && nodes[children_[from]].type == NodeType::Text;
		// Inline layout establishes the text baseline even for a sole run.
		// Painting must not replace it with an ink-centred baseline depending
		// on whether the source happens to contain leading whitespace.
		int pen = penStart;
		for (int k = from; k < to; k++) {
			const int child = children_[k];
			Node &cn = nodes[child];
			const int marginL = cn.style.margin[3];
			const int marginR = cn.style.margin[1];
			const int gap = pen > 0 ? mainGap_ : 0;

			if (cn.layout.inline_indent > 0) {
				// A wrapped continuation keeps a full-width box at the block's left
				// edge; its indent is what carries the pen it started from.
				cn.layout.x = clampInt16(contentLeft + marginL);
			} else {
				cn.layout.x = clampInt16(contentLeft + pen + gap + marginL);
			}
			pen += gap + marginL + cn.layout.width + marginR;

			if (soleRun && cn.type == NodeType::Text && !hasExplicitWidth(cn)) {
				// text-align needs a box as wide as the line box to align inside.
				const int full = contentW - marginL - marginR;
				if (full > cn.layout.width) cn.layout.width = clampInt16(full);
			}

			int y = baseline - childBaseline(cn);
			const int align = usedAlignment(crossAlignFor(cn), lineCross - cn.layout.height - cn.style.margin[0] - cn.style.margin[2]);
			if (align != 5) {
				const int crossBefore = cn.style.margin[0];
				const int crossAfter = cn.style.margin[2];
				const int crossTotal = cn.layout.height + crossBefore + crossAfter;
				y = lineTop + crossBefore;
				if (align == 1) y += (lineCross - crossTotal) / 2;
				else if (align == 2) y += lineCross - crossTotal;
				else if (align == 0 && !hasExplicitHeight(cn) && !isIntrinsicSizeExpression(cn.style.height_expression)) {
					cn.layout.height = clampInt16(lineCross - crossBefore - crossAfter);
					engine_.repositionChildren(child);
				}
			}
			cn.layout.y = clampInt16(y);
			cn.render.inline_baseline = 1;
			// An inline wrapper (<span>text</span>) contributes its CHILD's glyphs to
			// this line box, so the flag has to reach the node that actually draws.
			if (cn.type == NodeType::View) {
				for (int c = cn.first_child; c >= 0; c = nodes[c].next_sibling)
					if (nodes[c].type == NodeType::Text) nodes[c].render.inline_baseline = 1;
			}
		}
	}

	void expandCrossSizes(int lineCount, bool onlyIfDefinite)
	{
		const bool hasDefiniteCross = isRow_ ? hasExplicitHeight(node_) : hasExplicitWidth(node_);
		const bool wrappingFlex = node_.style.display == kDisplayFlex && node_.style.flex_wrap;
		if (onlyIfDefinite && !hasDefiniteCross) {
			// A single flex line's intrinsic cross size still obeys the
			// container's min/max cross size (Flexbox 9.4, step 8).
			if (node_.style.display == kDisplayFlex && lineCount == 1 && !wrappingFlex) {
				const int insets = boxInsets(node_.style, !isRow_);
				lines_[0].crossSize = std::max(0, clampLayoutSize(node_, lines_[0].crossSize + insets, !isRow_) - insets);
			}
			return;
		}

		const int crossAvail = isRow_ ? padHeight_ : padWidth_;
		if (lineCount == 1 && !wrappingFlex) {
			lines_[0].crossSize = crossAvail;
		} else if (lineCount > 0 && (!wrappingFlex || node_.style.align_content == 0)) {
			int totalLineCross = 0;
			for (int i = 0; i < lineCount; i++) totalLineCross += lines_[i].crossSize;
			const int extra = crossAvail - totalLineCross - (lineCount - 1) * crossGap_;
			if (extra > 0) {
				// Distribute the integer remainder cumulatively so no pixel is lost.
				for (int i = 0; i < lineCount; i++)
					lines_[i].crossSize += extra * (i + 1) / lineCount - extra * i / lineCount;
			}
		}
	}

	void positionLines(int lineCount)
	{
		Node *nodes = Tree::instance().nodes();
		const bool wrappingFlex = node_.style.display == kDisplayFlex && node_.style.flex_wrap;
		const bool definiteCross = isRow_ ? hasExplicitHeight(node_) : hasExplicitWidth(node_);
		bool ratioCrossChanged = false;

		// Free main-axis space is measured against the node's FINAL padded main size
		// (set by autosizeParent before this runs), NOT the available space it was
		// offered. An auto-sized box shrinks to its content, so it has zero free
		// space — distributing `mainAvail_ - content` here would, e.g., center a
		// label using the parent's full width and then leave it stranded once the
		// box shrinks to fit. Reading layout.{width,height} also gives the correct
		// free space when a flex item was GROWN past its content by its parent.
		const int mainBox = isRow_
			? (node_.layout.width - boxInset(node_.style, 1) - boxInset(node_.style, 3))
			: (node_.layout.height - boxInset(node_.style, 0) - boxInset(node_.style, 2));
		const int mainContentBox = mainBox < 0 ? 0 : mainBox;

		for (int lineIndex = 0; lineIndex < lineCount; lineIndex++) {
			FlexLine &line = lines_[lineIndex];
			int remaining = mainContentBox - line.mainSize;

			const int totalGrow = totalFlexGrowForLine(line, nodes);
			if (remaining > 0 && totalGrow > 0) {
				growFlexChildren(line, nodes, totalGrow, remaining);
				remaining = 0;
			} else if (remaining < 0) {
				// A container that scrolls along the main axis lets overflowing content
				// stay at its natural size and become scroll range instead of being
				// flex-shrunk to fit. Without this, a scrollable column (typography
				// specimen page) compresses every section into the viewport —
				// scroll_content_height never exceeds height and the page can't scroll.
				// (CSS floors shrink at min-content, which for stacked text is its
				// natural size; this engine has no min-content plumbing, so the scroll
				// container skips main-axis shrink entirely — the pre-flex-shrink
				// behavior for scrollable boxes.)
				const bool mainAxisScrolls =
					isRow_ ? scrollsOverflowX(node_.style) : scrollsOverflowY(node_.style);
				if (!mainAxisScrolls) {
					const int totalShrink = totalFlexShrinkForLine(line, nodes);
					if (totalShrink > 0) {
						shrinkFlexChildren(line, nodes, totalShrink, remaining);
						remaining = 0;
					}
				}
			} else if (totalGrow > 0) {
				// Free space is exactly zero, so no resize call runs — but the
				// flex items were zeroed for measurement (applyFlexBasis) and no
				// longer reposition there; give the still-zeroed ones their one
				// subtree layout at the final (zero) size.
				for (int i = 0; i < line.count; i++) {
					const int child = children_[line.start + i];
					Node &childNode = nodes[child];
					if (childNode.style.flex <= 0) continue;
					const int mainSize = isRow_ ? childNode.layout.width : childNode.layout.height;
					if (mainSize == 0) engine_.repositionChildren(child);
				}
			}
			bool hasRatio = false;
			int measuredCross = 0;
			for (int i = 0; i < line.count; ++i) {
				const Node &child = nodes[children_[line.start + i]];
				hasRatio |= preferredRatio(child) > 0;
				measuredCross = std::max(measuredCross, isRow_ ? child.layout.height + child.style.margin[0] + child.style.margin[2]
				                                                    : child.layout.width + child.style.margin[1] + child.style.margin[3]);
			}
			if (node_.style.display == kDisplayFlex && hasRatio && !definiteCross) {
				line.crossSize = std::max(line.strutSize, baselineCrossSize(line.start, line.count, measuredCross));
				ratioCrossChanged = true;
			}
		}
		// Resolve all flexed sizes before cross-axis placement: a changed ratio
		// height affects following lines and reversed line order as well.
		if (ratioCrossChanged) {
			expandCrossSizes(lineCount, true);
			int16_t &cross = isRow_ ? node_.layout.height : node_.layout.width;
			cross = clampInt16(clampLayoutSize(node_, totalCrossSize(lineCount) + boxInsets(node_.style, !isRow_), !isRow_));
		}
		int crossOffset = boxInset(node_.style, isRow_ ? 0 : 3);
		const int crossBox = (isRow_ ? node_.layout.height : node_.layout.width) - boxInsets(node_.style, !isRow_);
		const int crossFree = crossBox - totalCrossSize(lineCount);
		for (int lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
			FlexLine &line = lines_[lineIndex];
			int occupied = std::max(0, line.count - 1) * mainGap_;
			for (int i = 0; i < line.count; ++i) {
				const Node &child = nodes[children_[line.start + i]];
				occupied += isRow_ ? child.layout.width + child.style.margin[1] + child.style.margin[3]
				                   : child.layout.height + child.style.margin[0] + child.style.margin[2];
			}
			int remaining = mainContentBox - occupied;
			const int mainAlignment = usedAlignment(node_.style.justify_content, remaining, node_.style.flex_direction_explicit && node_.style.flex_direction >= 2);
			if (remaining < 0 && mainAlignment != 1 && mainAlignment != 2) remaining = 0;

			int mainOffset = isRow_ ? boxInset(node_.style, 3) : boxInset(node_.style, 0);
			int autoCount = 0;
			for (int i = 0; i < line.count; ++i) {
				const auto mask = nodes[children_[line.start + i]].style.margin_auto;
				autoCount += isRow_ ? !!(mask & 2) + !!(mask & 8) : !!(mask & 1) + !!(mask & 4);
			}
			const int autoSpace = autoCount && remaining > 0 ? remaining : 0;
			if (!isDistributedAlignment(node_.style.justify_content))
				applyJustifyContent(autoSpace ? 0 : remaining, &mainOffset);
			const int lineOffset = wrappingFlex ? crossAlignmentOffset(lineIndex, lineCount, crossFree) : 0;
			positionLineChildren(line, nodes, mainOffset, crossOffset + lineOffset, autoSpace, autoCount, autoSpace ? 0 : remaining);
			crossOffset += line.crossSize + crossGap_;
		}
	}

	int crossAlignmentOffset(int index, int count, int free) const
	{
		// A wrapping container is multi-line even when only one line is occupied.
		// Distributed alignments fall back to start/center for overflowing lines.
		switch (usedAlignment(node_.style.align_content, free, (node_.style.flex_wrap & 3) == 2)) {
		case 1: return free / 2;
		case 2: return free;
		case 3:
		case 4:
		case kAlignSpaceEvenly: return distributedAlignmentOffset(node_.style.align_content, free, index, count);
		default: return 0;
		}
	}

	void autosizeParent(int lineCount)
	{
		// A scroll container keeps the size its parent already gave it (flex grow,
		// stretch, or a definite parent box in `layout.{height,width}` from
		// prepareOwnSize) instead of auto-growing to its content on the scrolling
		// axis. Otherwise scroll_content_{height,width} == layout.{height,width},
		// scrollMax is 0, and the container never scrolls. This matches CSS:
		// overflow:scroll/auto clips/scrolls its overflow, it does not expand to
		// fit content. Guarded on a positive assigned size so an unsized scroll
		// container still autosizes as before.
		const bool keepScrollHeight = scrollsOverflowY(node_.style) && node_.layout.height > 0;
		const bool keepScrollWidth = !intrinsicWidthConstraint(node_) && scrollsOverflowX(node_.style) && node_.layout.width > 0;

		if (!hasExplicitHeight(node_) && !keepScrollHeight) {
			const int height = (isRow_ ? totalCrossSize(lineCount) : maxMainSize(lineCount)) + boxInset(node_.style, 0) + boxInset(node_.style, 2);
			node_.layout.height = clampLayoutSize(node_, height, false);
		}

		if (!hasExplicitWidth(node_) && !keepScrollWidth) {
			const int width = (isRow_ ? maxMainSize(lineCount) : totalCrossSize(lineCount)) + boxInset(node_.style, 1) + boxInset(node_.style, 3);
			node_.layout.width = clampLayoutSize(node_, width, true);
		}
	}

private:
	int flexWrapLimit() const
	{
		const bool blockAxis = isRow_ == (writingMode(node_) != 0);
		if (!blockAxis || assignedSize_ || (isRow_ ? hasExplicitWidth(node_) : hasExplicitHeight(node_))) return mainAvail_;
		// An automatic block main size grows to fit its items. min-height is
		// a floor, not a wrapping constraint; only a maximum bounds packing.
		if ((isRow_ ? node_.style.max_width : node_.style.max_height) == kUnset) return INT_MAX;
		return std::max(0, clampLayoutSize(node_, INT16_MAX, isRow_) - boxInsets(node_.style, isRow_));
	}

	int buildCollapsedLines(const std::vector<int> &struts)
	{
		Node *nodes = Tree::instance().nodes();
		std::vector<int> balancedEnds;
		if (node_.style.flex_wrap & 4) {
			std::vector<int> sizes;
			int total = 0;
			for (int i = 0; i < childCount_; ++i) {
				if (struts[i] >= 0) continue;
				const Node &child = nodes[children_[i]];
				const int size = isRow_ ? child.layout.width + child.style.margin[1] + child.style.margin[3]
				                        : child.layout.height + child.style.margin[0] + child.style.margin[2];
				total += std::max(0, size) + (sizes.empty() ? 0 : mainGap_);
				sizes.push_back(size);
			}
			const int available = std::min(total, flexWrapLimit());
			balancedEnds = balancedFlexLineEnds(sizes, available, mainGap_, rstyle(node_.style).flex_line_count);
		}
		int lineCount = 0, start = 0, written = 0, main = 0, cross = 0, strut = 0;
		auto finish = [&] {
			appendLine(lineCount, start, written - start, main, std::max(cross, strut));
			lines_[lineCount - 1].strutSize = strut;
			start = written; main = cross = strut = 0;
		};
		for (int i = 0; i < childCount_; ++i) {
			const int id = children_[i];
			Node &child = nodes[id];
			if (struts[i] >= 0) {
				strut = std::max(strut, struts[i]);
				// The strut belongs to the line, not to a rendered child box.
				child.layout.x = child.layout.y = child.layout.width = child.layout.height = 0;
				continue;
			}
			const int childMain = isRow_ ? child.layout.width + child.style.margin[1] + child.style.margin[3]
			                              : child.layout.height + child.style.margin[0] + child.style.margin[2];
			const int childCross = isRow_ ? child.layout.height + child.style.margin[0] + child.style.margin[2]
			                               : child.layout.width + child.style.margin[1] + child.style.margin[3];
			const bool wrap = !balancedEnds.empty() ? written == balancedEnds[lineCount]
			                                        : node_.style.flex_wrap && main + mainGap_ + childMain > flexWrapLimit();
			if (written > start && wrap) finish();
			main += childMain + (written > start ? mainGap_ : 0);
			cross = std::max(cross, childCross);
			children_[written++] = id;
		}
		finish();
		childCount_ = written;
		return lineCount;
	}

	int buildBalancedLines()
	{
		Node *nodes = Tree::instance().nodes();
		std::vector<int> sizes(childCount_);
		int total = 0;
		for (int i = 0; i < childCount_; ++i) {
			const Node &child = nodes[children_[i]];
			sizes[i] = isRow_ ? child.layout.width + child.style.margin[1] + child.style.margin[3]
			                  : child.layout.height + child.style.margin[0] + child.style.margin[2];
			total += std::max(0, sizes[i]) + (i ? mainGap_ : 0);
		}
		// An unconstrained automatic block main size is content-based; the
		// viewport height must not manufacture additional columns.
		const int available = std::min(total, flexWrapLimit());
		const auto ends = balancedFlexLineEnds(sizes, available, mainGap_, rstyle(node_.style).flex_line_count);
		int start = 0, count = 0;
		for (const int end : ends) {
			int main = 0, cross = 0;
			for (int i = start; i < end; ++i) {
				const Node &child = nodes[children_[i]];
				main += sizes[i] + (i > start ? mainGap_ : 0);
				cross = std::max(cross, isRow_ ? child.layout.height + child.style.margin[0] + child.style.margin[2]
				                             : child.layout.width + child.style.margin[1] + child.style.margin[3]);
			}
			appendLine(count, start, end - start, main, cross);
			start = end;
		}
		return count;
	}

	int flexPercentageBasis() const
	{
		if (isRow_ ? hasExplicitWidth(node_) : hasExplicitHeight(node_)) return mainAvail_;
		// A normal block's automatic inline size fills its containing block.
		// Intrinsic measurements and automatic block sizes remain indefinite.
		if (isRow_ && !intrinsicWidthConstraint(node_) && !node_.style.float_side &&
		    !isIntrinsicSizeExpression(node_.style.width_expression) &&
		    (node_.parent < 0 || Tree::instance().nodes()[node_.parent].style.display != kDisplayFlex)) return mainAvail_;
		if (assignedSize_ && node_.parent >= 0) {
			const Node &parent = Tree::instance().nodes()[node_.parent];
			if (parent.style.display == kDisplayFlex) {
				const bool parentRow = flexRowDirection(parent.style) != (writingMode(parent) != 0);
				if (isRow_ != parentRow ? definiteFlexCrossSize(parent, node_, parentRow)
				                       : (isRow_ ? hasExplicitWidth(parent) : hasExplicitHeight(parent))) return mainAvail_;
			}
		}
		return -1;
	}

	void applyFlexBasis(int child)
	{
		Node &childNode = Tree::instance().nodes()[child];
		// Authored bases were measured on the physical main axis above. Preserve
		// that result, including content sizing for an indefinite percentage.
		if (hasFlexBasis(childNode)) return;
		if (childNode.style.flex <= 0) return;
		if (isRow_) {
			if (hasExplicitWidth(childNode)) return;
			if (childNode.layout.width == 0) return;
			childNode.layout.width = 0;
		} else {
			if (hasExplicitHeight(childNode)) return;
			if (childNode.layout.height == 0) return;
			childNode.layout.height = 0;
		}
		// No repositionChildren here: the zeroed main size exists only for
		// buildLines' measurement, and growFlexChildren repositions the item
		// at its grown size moments later — recursively re-laying the subtree
		// at width/height 0 in between was the biggest source of redundant
		// layout work on nested-flex trees. The one case where no grow follows
		// (free space exactly 0) is compensated in positionLines.
	}

	int baselineCrossSize(int start, int count, int crossSize) const
	{
		if (isRow_ && node_.style.display == kDisplayFlex) {
			for (int alignment : {5, kAlignLastBaseline}) {
				int ascent = 0, descent = 0;
				for (int i = start; i < start + count; ++i) {
					const Node &child = Tree::instance().nodes()[children_[i]];
					if (crossAlignFor(child) != alignment || (child.style.margin_auto & 5)) continue;
					const int baseline = childBaseline(child, alignment == kAlignLastBaseline);
					ascent = std::max(ascent, child.style.margin[0] + baseline);
					descent = std::max(descent, child.style.margin[2] + child.layout.height - baseline);
				}
				crossSize = std::max(crossSize, ascent + descent);
			}
		}
		return crossSize;
	}

	void appendLine(int &lineCount, int start, int count, int mainSize, int crossSize)
	{
		if (lineCount >= (extraLines_.empty() ? kMaxFlexLines : static_cast<int>(extraLines_.size()))) return;
		crossSize = baselineCrossSize(start, count, crossSize);
		lines_[lineCount].start = start;
		lines_[lineCount].count = count;
		lines_[lineCount].mainSize = mainSize;
		lines_[lineCount].crossSize = crossSize;
		lines_[lineCount].strutSize = 0;
		lineCount++;
	}

	int totalFlexGrowForLine(const FlexLine &line, Node *nodes) const
	{
		if (node_.style.display != kDisplayFlex) return 0;
		int totalFlex = 0;
		for (int i = 0; i < line.count; i++) {
			totalFlex += nodes[children_[line.start + i]].style.flex;
		}
		return totalFlex;
	}

	int childMainSize(const Node &childNode) const
	{
		return isRow_ ? childNode.layout.width : childNode.layout.height;
	}

	// A text leaf's height IS its wrapped content height: the renderer draws
	// every wrapped line regardless of layout.height, so compressing it on a
	// column's main axis just makes the following sibling overlap the glyphs
	// (CSS floors flex shrink at the item's automatic minimum size — for text,
	// its min-content height). Treat column-axis text as non-shrinkable; the
	// deficit redistributes to shrinkable siblings or becomes overflow.
	bool childMainSizeIsShrinkable(const Node &childNode) const
	{
		if (node_.style.display != kDisplayFlex) return false;
		if (childNode.style.flex_shrink <= 0) return false;
		if (!isRow_ && childNode.type == NodeType::Text) return false;
		return true;
	}

	int totalFlexShrinkForLine(const FlexLine &line, Node *nodes) const
	{
		int totalShrink = 0;
		for (int i = 0; i < line.count; i++) {
			const Node &childNode = nodes[children_[line.start + i]];
			if (!childMainSizeIsShrinkable(childNode)) continue;
			const int basis = childMainSize(childNode);
			if (basis <= 0) continue;
			totalShrink += childNode.style.flex_shrink * basis;
		}
		return totalShrink;
	}

	void growFlexChildren(const FlexLine &line, Node *nodes, int totalGrow, int delta)
	{
		if (totalGrow <= 0 || delta <= 0) return;
		int applied = 0;
		int lastGrowChild = -1;

		for (int i = 0; i < line.count; i++) {
			const int child = children_[line.start + i];
			Node &childNode = nodes[child];
			if (childNode.style.flex <= 0) continue;
			lastGrowChild = child;
			const int change = (delta * childNode.style.flex) / totalGrow;
			applied += applyFlexMainSizeDelta(child, childNode, change);
		}

		const int remainder = delta - applied;
		if (remainder > 0 && lastGrowChild >= 0) {
			Node &childNode = nodes[lastGrowChild];
			applyFlexMainSizeDelta(lastGrowChild, childNode, remainder);
		}
	}

	void shrinkFlexChildren(const FlexLine &line, Node *nodes, int totalShrink, int delta)
	{
		if (totalShrink <= 0 || delta >= 0) return;
		const int magnitude = -delta;
		int applied = 0;
		int lastShrinkChild = -1;

		for (int i = 0; i < line.count; i++) {
			const int child = children_[line.start + i];
			Node &childNode = nodes[child];
			if (!childMainSizeIsShrinkable(childNode)) continue;
			const int basis = childMainSize(childNode);
			if (basis <= 0) continue;
			lastShrinkChild = child;
			const int change = (magnitude * childNode.style.flex_shrink * basis) / totalShrink;
			applied += applyFlexMainSizeDelta(child, childNode, -change);
		}

		const int remainder = magnitude - applied;
		if (remainder > 0 && lastShrinkChild >= 0) {
			Node &childNode = nodes[lastShrinkChild];
			applyFlexMainSizeDelta(lastShrinkChild, childNode, -remainder);
		}
	}

	// CSS `min-width: auto` / `min-height: auto` — a flex item's AUTOMATIC MINIMUM
	// SIZE: an item may not be flex-shrunk below what its own in-flow content needs.
	// The engine already honours this for text leaves (see childMainSizeIsShrinkable);
	// a container has to be measured, because a line's whole deficit lands on however
	// few items are shrinkable. On the temperature dial the face column overflows by
	// ~49px and its fan pill (`height: 8.125vmin`) was the ONLY shrinkable item, so it
	// absorbed all of it, clamped to zero, and laid out 112x0 — the pill, its dot and
	// its label recorded no commands at all. CSS overflows the container instead.
	// Applies exactly where CSS applies it: no explicit min on that axis (an explicit
	// one already floors clampSize), and overflow visible on the main axis — a
	// scroll/hidden box really does have a zero automatic minimum.
	int automaticMinimumMainSize(const Node &childNode) const
	{
		const int explicitMin = isRow_ ? childNode.style.min_width : childNode.style.min_height;
		if (explicitMin != kUnset) return 0;
		if (isScrollableOverflow(isRow_ ? overflowX(childNode.style) : overflowY(childNode.style))) return 0;
		const Node *nodes = Tree::instance().nodes();
		int extent = 0;
		for (int c = childNode.first_child; c >= 0; c = nodes[c].next_sibling) {
			const Node &grand = nodes[c];
			if (isDisplayNone(grand.style) || isOutOfFlowPosition(grand.style.position)) continue;
			// layout.{x,y} are still parent-relative here: resolveAbsoluteCoords runs
			// after the whole layout pass.
			const int end = isRow_
				? grand.layout.x + grand.layout.width + grand.style.margin[1]
				: grand.layout.y + grand.layout.height + grand.style.margin[2];
			if (end > extent) extent = end;
		}
		if (extent > 0) extent += isRow_ ? boxInset(childNode.style, 1) : boxInset(childNode.style, 2);
		if (preferredRatio(childNode) > 0 &&
		    ((isRow_ ? hasExplicitHeight(childNode) : hasExplicitWidth(childNode)) || definiteFlexCrossSize(node_, childNode, isRow_))) {
			const int transferred = ratioTransferredSize(childNode, isRow_ ? childNode.layout.height : childNode.layout.width, isRow_);
			extent = childNode.type == NodeType::Image ? std::min(extent, transferred) : std::max(extent, transferred);
		}
		if (extent <= 0) return 0;
		const int max = isRow_ ? childNode.style.max_width : childNode.style.max_height;
		if (max != kUnset) extent = std::min(extent, contentSizeToBorderSize(childNode.style, max, isRow_));
		if (isRow_ ? hasExplicitWidth(childNode) : hasExplicitHeight(childNode)) {
			const int specified = isRow_ ? resolvedStyleWidth(childNode, mainAvail_) : resolvedStyleHeight(childNode, mainAvail_);
			extent = std::min(extent, contentSizeToBorderSize(childNode.style, specified, isRow_));
		}
		return extent;
	}

	int applyFlexMainSizeDelta(int child, Node &childNode, int delta)
	{
		if (delta == 0) return 0;
		const int before = isRow_ ? childNode.layout.width : childNode.layout.height;
		int after = before + delta;
		after = clampFlexMainSize(childNode, after);
		if (delta < 0) {
			// Floor the shrink at the automatic minimum size, and never let that floor
			// GROW an item that was already smaller than its content.
			const int autoMin = automaticMinimumMainSize(childNode);
			const int minAllowed = autoMin < before ? autoMin : before;
			if (after < minAllowed) after = minAllowed;
		}
		if (after == before) return 0;
		if (isRow_) childNode.layout.width = after;
		else childNode.layout.height = after;
		if (preferredRatio(childNode) > 0 && !(isRow_ ? hasExplicitHeight(childNode) : hasExplicitWidth(childNode)) &&
		    !definiteFlexCrossSize(node_, childNode, isRow_)) {
			int16_t &cross = isRow_ ? childNode.layout.height : childNode.layout.width;
			cross = clampInt16(clampLayoutSize(childNode, ratioTransferredSize(childNode, after, !isRow_), !isRow_));
		}
		engine_.repositionChildren(child);
		return delta > 0 ? after - before : before - after;
	}

	int clampFlexMainSize(const Node &childNode, int size) const
	{
		return clampBorderSize(engine_, childNode, size, isRow_);
	}

	void applyJustifyContent(int remaining, int *mainOffset) const
	{
		switch (usedAlignment(node_.style.justify_content, remaining, node_.style.flex_direction_explicit && node_.style.flex_direction >= 2)) {
		case 0:
			break;
		case 1:
			*mainOffset += remaining / 2;
			break;
		case 2:
			*mainOffset += remaining;
			break;
		}
	}

	void positionLineChildren(const FlexLine &line, Node *nodes, int mainOffset, int crossOffset, int autoSpace = 0, int autoCount = 0, int distributedFree = 0)
	{
		const bool flex = node_.style.display == kDisplayFlex;
		const bool logicalRow = flexRowDirection(node_.style);
		const int mode = writingMode(node_);
		const bool blockReversed = mode == 2 || mode == 3;
		const bool inlineReversed = rightToLeft(node_) != (mode == 4);
		const bool reverseMain = flex && ((node_.style.flex_direction_explicit && node_.style.flex_direction >= 2) != (logicalRow ? inlineReversed : blockReversed));
		const bool reverseCross = flex && (((node_.style.flex_wrap & 3) == 2) != (logicalRow ? blockReversed : inlineReversed));
		const int mainBefore = isRow_ ? (reverseMain ? 1 : 3) : (reverseMain ? 2 : 0);
		const int mainAfter = (mainBefore + 2) % 4;
		const int crossBefore = isRow_ ? (reverseCross ? 2 : 0) : (reverseCross ? 1 : 3);
		const int crossAfter = (crossBefore + 2) % 4;
		int autoUsed = 0;
		auto autoMargin = [&](const Node &child, int side) {
			if (!(child.style.margin_auto & (1 << side)) || !autoCount) return 0;
			const int before = autoSpace * autoUsed / autoCount;
			return autoSpace * ++autoUsed / autoCount - before;
		};
		// First baselines of this line's baseline-aligned children (rows only):
		// every such child's baseline lands on the line's max baseline, matching
		// CSS `align-items: baseline` (a 12px range label shares the 14px
		// condition's baseline instead of top-aligning).
		int lineMaxBaseline = 0;
		int lineMaxLastDescent = 0, lineMaxLastAscent = 0;
		if (isRow_) {
			for (int i = 0; i < line.count; i++) {
				Node &probe = nodes[children_[line.start + i]];
				if (probe.style.margin_auto & 5) continue;
				if (crossAlignFor(probe) == kAlignLastBaseline) {
					const int baseline = childBaseline(probe, true);
					lineMaxLastDescent = std::max(lineMaxLastDescent, probe.style.margin[2] + probe.layout.height - baseline);
					lineMaxLastAscent = std::max(lineMaxLastAscent, probe.style.margin[0] + baseline);
				}
				if (crossAlignFor(probe) != 5) continue;
				const int baseline = probe.style.margin[0] + childBaseline(probe);
				if (baseline > lineMaxBaseline) lineMaxBaseline = baseline;
			}
		}

		for (int i = 0; i < line.count; i++) {
			const int child = children_[line.start + i];
			Node &childNode = nodes[child];

			const int mainMarginBefore = childNode.style.margin[mainBefore] + autoMargin(childNode, mainBefore);
			const int mainMarginAfter = childNode.style.margin[mainAfter] + autoMargin(childNode, mainAfter);
			const int crossMarginBefore = childNode.style.margin[crossBefore];
			const int crossMarginAfter = childNode.style.margin[crossAfter];
			const int crossSize = isRow_ ? childNode.layout.height : childNode.layout.width;
			const int crossTotal = crossSize + crossMarginBefore + crossMarginAfter;

			mainOffset += mainMarginBefore;
			int crossPosition = crossOffset + crossMarginBefore;
			const bool crossAutoBefore = childNode.style.margin_auto & (1 << crossBefore);
			const bool crossAutoAfter = childNode.style.margin_auto & (1 << crossAfter);
			if (crossAutoBefore || crossAutoAfter) {
				const int free = std::max(0, line.crossSize - crossTotal);
				if (crossAutoBefore) crossPosition += crossAutoAfter ? free / 2 : free;
			} else {
				positionCrossAxisChild(child, childNode, line.crossSize, crossMarginBefore, crossMarginAfter, crossTotal, &crossPosition, lineMaxBaseline, std::max(lineMaxLastAscent, line.crossSize - lineMaxLastDescent));
				// Baselines are distances from physical top. Reversing the line
				// order must not mirror each item's baseline inside that line.
				const int alignment = crossAlignFor(childNode) & 15;
				if (isRow_ && reverseCross && (alignment == 5 || alignment == kAlignLastBaseline))
					crossPosition = crossOffset + line.crossSize - (crossPosition - crossOffset) - crossSize;
			}

			if (isRow_) {
				childNode.layout.x = mainOffset + distributedAlignmentOffset(node_.style.justify_content, distributedFree, i, line.count);
				childNode.layout.y = crossPosition;
			} else {
				childNode.layout.x = crossPosition;
				childNode.layout.y = mainOffset + distributedAlignmentOffset(node_.style.justify_content, distributedFree, i, line.count);
			}

			const bool reverseX = isRow_ ? reverseMain : reverseCross;
			const bool reverseY = isRow_ ? reverseCross : reverseMain;
			if (reverseX) childNode.layout.x = node_.layout.width - childNode.layout.x - childNode.layout.width + boxInset(node_.style, 3) - boxInset(node_.style, 1);
			if (reverseY) childNode.layout.y = node_.layout.height - childNode.layout.y - childNode.layout.height + boxInset(node_.style, 0) - boxInset(node_.style, 2);

			const int mainSize = isRow_ ? childNode.layout.width : childNode.layout.height;
			mainOffset += mainSize + mainMarginAfter + mainGap_;
			// Flex items own their boxes; nothing here shares a line box.
			childNode.render.inline_baseline = 0;
		}
	}

	// Cross-axis alignment in force for one item. `align-self` always wins.
	// Otherwise an INLINE formatting row aligns on the shared baseline — that is
	// what an inline formatting context does, and `align-items` has no effect on
	// a block container, so the parent's value is not consulted there. A real
	// flex container keeps reading `align-items`.
	int crossAlignFor(const Node &childNode) const
	{
		if (childNode.style.align_self >= 0) return childNode.style.align_self;
		return inlineRow_ ? 5 : node_.style.align_items;
	}

	void positionCrossAxisChild(int child, Node &childNode, int lineCrossSize, int crossMarginBefore, int crossMarginAfter, int crossTotal, int *crossPosition, int lineMaxBaseline, int lineLastBaseline)
	{
		const int alignment = crossAlignFor(childNode);
		int align = physicalSelfAlignment(node_, childNode, alignment, lineCrossSize - crossTotal, !isRow_);
		if (align >= 0) {
			const bool reversed = gridAxisReversed(node_, !isRow_) != ((node_.style.flex_wrap & 3) == 2);
			if (reversed) align = align == 2 ? 6 : 2;
		} else align = usedAlignment(alignment, lineCrossSize - crossTotal, (node_.style.flex_wrap & 3) == 2);
		switch (align) {
			case 0:
			if (isRow_) {
				if (!hasExplicitHeight(childNode) && !isIntrinsicSizeExpression(childNode.style.height_expression)) {
					childNode.layout.height = lineCrossSize - crossMarginBefore - crossMarginAfter;
					engine_.repositionChildren(child);
				}
			} else if (!hasExplicitWidth(childNode) && !isIntrinsicSizeExpression(childNode.style.width_expression)) {
				childNode.layout.width = lineCrossSize - crossMarginBefore - crossMarginAfter;
				engine_.repositionChildren(child);
			}
			break;
		case 1:
			*crossPosition += (lineCrossSize - crossTotal) / 2;
			break;
		case 2:
			*crossPosition += lineCrossSize - crossTotal;
			break;
		case kAlignLastBaseline:
			if (isRow_) {
				*crossPosition += lineLastBaseline - childBaseline(childNode, true) - crossMarginBefore;
			} else if (lineCrossSize >= crossTotal) {
				*crossPosition += lineCrossSize - crossTotal;
			}
			break;
		case 5:
			// align-items/align-self: baseline (rows). Columns fall back to start,
			// matching CSS, where baseline alignment only applies across an inline axis.
			if (isRow_) {
				const int shift = lineMaxBaseline - (crossMarginBefore + childBaseline(childNode));
				if (shift > 0) *crossPosition += shift;
			}
			break;
		}
	}

	// First/last baseline of a flex child, measured from its border-box top. Text
	// leaves use the resolved atlas font's ascender — the glyph rasterizer
	// places runs at `top + ascender - bearingY`, so this matches the drawn
	// pixels exactly. Flex items synthesize missing baselines from border edges;
	// inline boxes use margin edges.
	int childBaseline(const Node &childNode, bool last = false) const
	{
		if ((childNode.type == NodeType::Text || isLineBreak(childNode)) && childNode.style.font_id >= 0)
			return TextRenderer::baselineOffset(childNode, last);
		// An inline wrapper (a <span> around text) carries no text of its own, so
		// CSS takes the baseline of its FIRST in-flow line box. Its children were
		// already positioned by measureChildren, and layout.y is still parent-
		// relative at this point, so the child's own offset inside the wrapper is
		// exactly its baseline's distance from the wrapper's top. Without this a
		// span-wrapped run reported its bottom margin edge as its baseline and
		// dropped below every bare text run beside it.
		if (childNode.type == NodeType::View && childNode.first_child >= 0) {
			const Node *nodes = Tree::instance().nodes();
			for (int c = last ? childNode.last_child : childNode.first_child; c >= 0; c = last ? nodes[c].prev_sibling : nodes[c].next_sibling) {
				const Node &inner = nodes[c];
				if (isDisplayNone(inner.style) || isOutOfFlowPosition(inner.style.position)) continue;
				return inner.layout.y + childBaseline(inner, last);
			}
		}
		return childNode.layout.height + (inlineRow_ ? childNode.style.margin[2] : 0);
	}

	int totalCrossSize(int lineCount) const
	{
		int total = 0;
		for (int i = 0; i < lineCount; i++) total += lines_[i].crossSize + (i > 0 ? crossGap_ : 0);
		return total;
	}

	int maxMainSize(int lineCount) const
	{
		int max = 0;
		for (int i = 0; i < lineCount; i++) {
			if (lines_[i].mainSize > max) max = lines_[i].mainSize;
		}
		return max;
	}

	LayoutEngine &engine_;
	Node &node_;
	int *children_ = nullptr;
	int childCount_ = 0;
	bool isRow_ = false;
	// True when this row is a synthesized INLINE FORMATTING context (a block
	// whose in-flow children are all inline-level), not a declared flex row.
	bool inlineRow_ = false;
	bool assignedSize_ = false;
	int mainAvail_ = 0;
	int padWidth_ = 0;
	int padHeight_ = 0;
	FlexLine *lines_ = nullptr;
	std::vector<FlexLine> extraLines_;
	int mainGap_ = 0;
	int crossGap_ = 0;
};

// CSS inline-level classification. Browsers flow inline-level boxes (text,
// replaced <img>, and inline elements like <span>/<a>/<em>) horizontally on a
// line inside a block; gea has no separate inline formatting context, so a plain
// block whose in-flow children are ALL inline-level is laid out as a flex row
// (see LayoutNodePass::resolveRowDirection) to match that. A child the app gave
// display:flex/grid is its own formatting context, not inline.
static bool isInlineLevelTag(const char *tag)
{
	return std::strcmp(tag, "br") == 0 || std::strcmp(tag, "span") == 0 || std::strcmp(tag, "a") == 0 || std::strcmp(tag, "b") == 0 ||
	       std::strcmp(tag, "i") == 0 || std::strcmp(tag, "em") == 0 || std::strcmp(tag, "strong") == 0 ||
	       std::strcmp(tag, "small") == 0 || std::strcmp(tag, "label") == 0 || std::strcmp(tag, "code") == 0 ||
	       std::strcmp(tag, "u") == 0 || std::strcmp(tag, "sub") == 0 || std::strcmp(tag, "sup") == 0 ||
	       std::strcmp(tag, "mark") == 0;
}

// Box-tree projection for inline ancestors split by in-flow blocks. The DOM
// ancestry still owns inheritance, events, relative translation, and painting.
// Visible inline decorations require fragments; keep those on their own path.
bool splitInlineWrapper(const Node *nodes, int id)
{
	const Node &n = nodes[id];
	if (n.type != NodeType::View || n.style.display_explicit || n.style.display != kDisplayBlock ||
	    !isInlineLevelTag(tagFromId(n.tag_id)) || n.style.float_side || isOutOfFlowPosition(n.style.position) ||
	    !FlexLayoutPass::paintsNoBox(n) || boxInsets(n.style, true) || boxInsets(n.style, false) ||
	    n.style.margin[1] || n.style.margin[3] || n.style.opacity != 255 || overflowEstablishesContext(n.style)) return false;
	bool containsBlock = false;
	for (int c = n.first_child; c >= 0; c = nodes[c].next_sibling) {
		const Node &child = nodes[c];
		if (isDisplayNone(child.style)) continue;
		if (isOutOfFlowPosition(child.style.position)) return false;
		if (child.style.float_side) continue;
		if (child.type == NodeType::View && (child.style.display_explicit || !isInlineLevelTag(tagFromId(child.tag_id)))) containsBlock = true;
		if (splitInlineWrapper(nodes, c)) containsBlock = true;
	}
	return containsBlock;
}

int formattingEdgeNode(const Node *nodes, int id, bool last)
{
	if (!splitInlineWrapper(nodes, id)) return id;
	for (int c = last ? nodes[id].last_child : nodes[id].first_child; c >= 0; c = last ? nodes[c].prev_sibling : nodes[c].next_sibling) {
		const Node &child = nodes[c];
		if (isDisplayNone(child.style) || isOutOfFlowPosition(child.style.position) || child.style.float_side) continue;
		if (isAnonymousTextNode(child) && (child.style.white_space == 0 || child.style.white_space == 1) &&
		    child.text.find_first_not_of(" \t\r\n\f") == std::string::npos) continue;
		return formattingEdgeNode(nodes, c, last);
	}
	return -1;
}

template<class Visit>
void visitFormattingChildren(const Node *nodes, int parent, const Visit &visit, bool reverse = false)
{
	const bool project = nodes[parent].style.display == kDisplayBlock && !nodes[parent].style.flex_direction_explicit;
	for (int c = reverse ? nodes[parent].last_child : nodes[parent].first_child; c >= 0; c = reverse ? nodes[c].prev_sibling : nodes[c].next_sibling) {
		if (project && splitInlineWrapper(nodes, c)) visitFormattingChildren(nodes, c, visit, reverse);
		else visit(c);
	}
}

class LayoutNodePass {
public:
	LayoutNodePass(LayoutEngine &engine, int id, int availWidth, int availHeight)
		: engine_(engine), id_(id), availWidth_(availWidth), availHeight_(availHeight), nodes_(Tree::instance().nodes()), node_(nodes_[id])
	{
	}

	void run()
	{
		invalidateInlineChildMetadata();
		if (NodeRareData *rare = rareDataFor(id_)) rare->firstLineBackground.lineValid = false;
		if (isDisplayNone(node_.style)) return;
		prepareOwnSize();

		if (isLineBreak(node_) && LayoutEngine::isInlineLevelNode(node_) &&
		    (node_.parent < 0 || (nodes_[node_.parent].style.display != kDisplayFlex && !isDisplayGrid(nodes_[node_.parent].style)))) {
			node_.layout.width = 0;
			node_.layout.height = clampInt16(TextRenderer::measureHeight(" ", node_.style.font_id,
			    node_.style.font_size, 0, node_.style.line_height));
			return;
		}

		if (node_.type == NodeType::Text) {
			const std::int64_t __tmT0 = refreshPerfNowUs();
			TextRenderer::layout(id_, availWidth_);
			refreshPerfStatsMutable().treeLayoutTextUs += refreshPerfNowUs() - __tmT0;
			return;
		}

		if (node_.type == NodeType::Image && node_.image_id >= 0) {
			ImageRenderer::layout(id_);
			return;
		}

		if (node_.type == NodeType::View && std::strcmp(tagFromId(node_.tag_id), "input") == 0) {
			InputRenderer::layout(id_, availWidth_);
			return;
		}

		const int padWidth = paddedWidth();
		const int padHeight = paddedHeight();
		const bool inlineRow = resolveInlineFormattingRow();
		const bool isRow = inlineRow || resolveRowDirection();
		const int mainAvail = isRow ? padWidth : padHeight;

		ScratchFrame scratch;
		if (!scratch.valid()) return;

		const int childCount = collectFormattingChildren(scratch.children(), kMaxChildren);
		if (childCount == 0 && !isDisplayGrid(node_.style)) {
			autosizeEmptyNode();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			applyVirtualListContentHeight();
			return;
		}

		if (isDisplayGrid(node_.style)) {
			layoutGridChildren(scratch.children(), childCount, padWidth, padHeight, true);
			updateScrollContentSize();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			applyVirtualListContentHeight();
			return;
		}

		if (layoutFloatChildren(scratch.children(), childCount, padWidth, padHeight, true)) {
			encloseFloatDescendants();
			updateScrollContentSize();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			return;
		}
		if (layoutBlockChildren(scratch.children(), childCount, padWidth, padHeight, true)) {
			encloseFloatDescendants();
			updateScrollContentSize();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			applyVirtualListContentHeight();
			return;
		}

		FlexLayoutPass flex(engine_, node_, scratch.children(), childCount, isRow, mainAvail, padWidth, padHeight, scratch.lines(), inlineRow);
		flex.measureChildren();
		if (inlineRow) {
			// An inline formatting context flows line boxes, not flex lines — see
			// FlexLayoutPass::layoutInlineFlow.
			flex.layoutInlineFlow(true);
		} else {
			flex.layoutLines(true);
		}
		encloseFloatDescendants();
		updateScrollContentSize();
		positionAbsoluteChildren();
		applyRelativeOffsets();
		applyVirtualListContentHeight();
	}

	void repositionChildren()
	{
		if (node_.type == NodeType::Text || node_.type == NodeType::Image) return;
		invalidateInlineChildMetadata();
		if (NodeRareData *rare = rareDataFor(id_)) rare->firstLineBackground.lineValid = false;

		const int padWidth = paddedWidth();
		const int padHeight = paddedHeight();
		const bool inlineRow = resolveInlineFormattingRow();
		const bool isRow = inlineRow || resolveRowDirection();
		const int mainAvail = isRow ? padWidth : padHeight;

		ScratchFrame scratch;
		if (!scratch.valid()) return;

		const int childCount = collectFormattingChildren(scratch.children(), kMaxChildren);
		if (childCount == 0 && !isDisplayGrid(node_.style)) {
			positionAbsoluteChildren();
			return;
		}

		if (isDisplayGrid(node_.style)) {
			layoutGridChildren(scratch.children(), childCount, padWidth, padHeight, false);
			positionAbsoluteChildren();
			applyRelativeOffsets();
			return;
		}

		if (layoutFloatChildren(scratch.children(), childCount, padWidth, padHeight, false)) {
			updateScrollContentSize();
			positionAbsoluteChildren();
			applyRelativeOffsets();
			return;
		}
		if (layoutBlockChildren(scratch.children(), childCount, padWidth, padHeight, false)) {
			positionAbsoluteChildren();
			applyRelativeOffsets();
			return;
		}

		FlexLayoutPass flex(engine_, node_, scratch.children(), childCount, isRow, mainAvail, padWidth, padHeight, scratch.lines(), inlineRow, true);
		flex.measureChildren();
		if (inlineRow) {
			flex.layoutInlineFlow(false);
		} else {
			flex.layoutLines(false);
		}
		positionAbsoluteChildren();
		applyRelativeOffsets();
	}

	bool containsChildMargins() const
	{
		// A stable border box is not sufficient for isolated relayout: child
		// margins can change an ancestor's position/size through either edge.
		// Check the boundary itself, so removing the last collapsing child is
		// handled as well as introducing or resizing one.
		const bool horizontal = writingMode(node_) != 0;
		return !blockBox(node_) || establishesBlockContext(node_) ||
		    (boxInset(node_.style, blockMarginSide(false)) > 0 &&
		     (boxInset(node_.style, blockMarginSide(true)) > 0 ||
		      (horizontal ? hasExplicitWidth(node_) : hasExplicitHeight(node_)) ||
		      (horizontal ? node_.style.min_width : node_.style.min_height) > 0));
	}

private:
	void invalidateInlineChildMetadata()
	{
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			NodeRareData *rare = rareDataFor(child);
			if (!rare) continue;
			rare->inlineStaticPosition.valid = false;
			rare->firstLineFragment.valid = false;
		}
	}

	bool splitInlineChildren_ = false;
	int collectFormattingChildren(int *children, int capacity)
	{
		if (node_.style.display != kDisplayBlock || node_.style.flex_direction_explicit)
			return engine_.collectChildren(id_, children, capacity, true);
		int count = 0;
		auto collect = [&](auto &&self, int parent) -> void {
			for (int c = nodes_[parent].first_child; c >= 0; c = nodes_[c].next_sibling) {
				Node &child = nodes_[c];
				if (isDisplayNone(child.style) || isOutOfFlowPosition(child.style.position) || suppressAnonymousWhitespace(nodes_, parent, c)) continue;
				if (splitInlineWrapper(nodes_, c)) {
					splitInlineChildren_ = true;
					child.layout.x = child.layout.y = child.layout.width = child.layout.height = 0;
					child.layout.scroll_content_width = child.layout.scroll_content_height = 0;
					child.layout.memo_pass = child.layout.memo2_pass = 0;
					self(self, c);
				} else if (count < capacity) children[count++] = c;
			}
		};
		collect(collect, id_);
		return count;
	}

	struct FloatBox { int left, top, right, bottom, side; };
	// One exclusion space for the entire BFC, including ordinary wrappers.
	// Coordinates exclude relative-position offsets. Clearance is used layout
	// state, not a consequence of merely authoring a non-none clear value.
	struct FloatContext {
		std::vector<FloatBox> boxes;
		std::vector<int> cleared;
		int lastTop = 0;
		bool hasClearance(int id) const { return std::find(cleared.begin(), cleared.end(), id) != cleared.end(); }
	};
	FloatContext *floatContext_ = nullptr;

	struct CollapsedMargin {
		int positive = 0, negative = 0;
		bool trimmed = false;
		void add(int value) { positive = std::max(positive, value); negative = std::min(negative, value); }
		void add(CollapsedMargin other) { add(other.positive); add(other.negative); trimmed |= other.trimmed; }
		int value() const { return trimmed ? 0 : positive + negative; }
	};

	bool blockBox(const Node &n) const
	{
		return n.type == NodeType::View && !isDisplayNone(n.style) &&
		    (n.style.display_explicit || !isInlineLevelTag(tagFromId(n.tag_id)));
	}

	bool flowChild(int parent, int child) const
	{
		return !isDisplayNone(nodes_[child].style) && !isOutOfFlowPosition(nodes_[child].style.position) &&
		    !nodes_[child].style.float_side && !suppressAnonymousWhitespace(nodes_, parent, child);
	}

	bool establishesBlockContext(const Node &n) const
	{
		return n.parent < 0 || isOutOfFlowPosition(n.style.position) || n.style.float_side ||
		    n.style.display != kDisplayBlock || overflowEstablishesContext(n.style) ||
		    (n.parent >= 0 && (nodes_[n.parent].style.display == kDisplayFlex || isDisplayGrid(nodes_[n.parent].style) ||
		                      writingMode(n) != writingMode(nodes_[n.parent])));
	}

	void encloseFloatDescendants()
	{
		if (node_.style.display != kDisplayBlock || hasExplicitHeight(node_) || !establishesBlockContext(node_)) return;
		int bottom = node_.layout.height - boxInset(node_.style, 2);
		// CSS 2.2 10.6.7 includes floats through ordinary wrappers, but never
		// crosses another formatting context. Child layout is still relative;
		// offsets below the first level have already been applied by its parent.
		auto visit = [&](auto &&self, int parent, int y) -> void {
			for (int id = nodes_[parent].first_child; id >= 0; id = nodes_[id].next_sibling) {
				const Node &child = nodes_[id];
				if (isDisplayNone(child.style) || isOutOfFlowPosition(child.style.position)) continue;
				int top = y + child.layout.y;
				if (parent != id_ && child.style.position == 2) {
					LayoutNodePass owner(engine_, parent, 0, 0);
					if (hasPositionOffset(child, 0)) top -= owner.resolvedRelativePositionOffset(child, 0);
					else if (hasPositionOffset(child, 2)) top += owner.resolvedRelativePositionOffset(child, 2);
				}
				if (child.style.float_side) bottom = std::max(bottom, top + child.layout.height + child.style.margin[2]);
				else if (!establishesBlockContext(child)) self(self, id, top);
			}
		};
		visit(visit, id_, 0);
		node_.layout.height = clampBorderSize(engine_, node_, bottom + boxInset(node_.style, 2), false);
	}

	int blockMarginSide(bool end) const
	{
		if (writingMode(node_) == 0) return end ? 2 : 0;
		return gridAxisReversed(node_, true) != end ? 1 : 3;
	}

	bool collapsesThrough(int id) const
	{
		const Node &n = nodes_[id];
		const bool horizontal = writingMode(node_) != 0;
		if (!blockBox(n) || establishesBlockContext(n) || (horizontal ? n.layout.width : n.layout.height) != 0 ||
		    (horizontal ? n.style.min_width : n.style.min_height) > 0 || boxInsets(n.style, horizontal)) return false;
		bool through = true;
		visitFormattingChildren(nodes_, id, [&](int c) {
			if (flowChild(nodes_[c].parent, c) && ((floatContext_ && floatContext_->hasClearance(c)) || !collapsesThrough(c))) through = false;
		});
		return through;
	}

	bool collapsesWithChildren(int id, bool bottom) const
	{
		const Node &n = nodes_[id];
		const bool horizontal = writingMode(node_) != 0;
		if (!blockBox(n) || establishesBlockContext(n) || boxInset(n.style, blockMarginSide(bottom))) return false;
		if (bottom && ((horizontal ? hasExplicitWidth(n) : hasExplicitHeight(n)) ||
		    isIntrinsicSizeExpression(horizontal ? n.style.width_expression : n.style.height_expression) ||
		    (horizontal ? n.style.min_width : n.style.min_height) > 0)) return false;
		int edge = -1;
		visitFormattingChildren(nodes_, id, [&](int c) {
			if (!flowChild(nodes_[c].parent, c) || (edge >= 0 && !bottom)) return;
			edge = c;
		});
		return edge >= 0 && blockBox(nodes_[edge]) &&
		    !(floatContext_ && floatContext_->hasClearance(edge) && (!bottom || collapsesThrough(edge)));
	}

	CollapsedMargin edgeMargin(int id, bool bottom, bool separateBottom = false) const
	{
		const Node &n = nodes_[id];
		CollapsedMargin margin;
		margin.add(n.style.margin[blockMarginSide(bottom)]);
		CollapsedMargin descendants;
		if (!separateBottom && collapsesThrough(id)) {
			margin.add(n.style.margin[blockMarginSide(!bottom)]);
			visitFormattingChildren(nodes_, id, [&](int c) {
				if (flowChild(nodes_[c].parent, c)) descendants.add(childEdgeMargin(id, c, false));
			});
		} else if (collapsesWithChildren(id, bottom)) {
			bool stop = false;
			visitFormattingChildren(nodes_, id, [&](int c) {
				if (stop || !flowChild(nodes_[c].parent, c)) return;
				if (!blockBox(nodes_[c]) || (floatContext_ && floatContext_->hasClearance(c) && (!bottom || collapsesThrough(c)))) { stop = true; return; }
				descendants.add(childEdgeMargin(id, c, bottom));
				if (!collapsesThrough(c)) stop = true;
			}, bottom);
		}
		if (!descendants.trimmed) margin.add(descendants);
		return margin;
	}

	// Whether this edge belongs to the collapsed group adjoining a container
	// boundary. Floats and positioned children do not consume that boundary;
	// inline content and non-empty blocks do.
	bool trimmedBlockEdge(int owner, int id, bool bottom) const
	{
		const Node &container = nodes_[owner];
		const int trim = rstyle(container.style).margin_trim;
		if (!(trim & 3) || container.style.display != kDisplayBlock || container.style.flex_direction_explicit || !blockBox(nodes_[id])) return false;
		for (bool end : {false, true}) {
			if (!(trim & (end ? 2 : 1)) || (end != bottom && !collapsesThrough(id))) continue;
			bool adjacent = true, found = false;
			visitFormattingChildren(nodes_, owner, [&](int child) {
				if (found || !adjacent || !flowChild(nodes_[child].parent, child)) return;
				if (child == id) { found = true; return; }
				if (!collapsesThrough(child) || (floatContext_ && floatContext_->hasClearance(child))) adjacent = false;
			}, end);
			if (found && adjacent) return true;
		}
		return false;
	}

	CollapsedMargin childEdgeMargin(int owner, int id, bool bottom, bool separateBottom = false) const
	{
		auto margin = edgeMargin(id, bottom, separateBottom);
		margin.trimmed = trimmedBlockEdge(owner, id, bottom);
		return margin;
	}

	bool layoutBlockChildren(int *children, int count, int contentWidth, int contentHeight, bool autosize)
	{
		if (node_.style.display != kDisplayBlock || node_.style.flex_direction_explicit) return false;
		const bool vertical = writingMode(node_) != 0;
		for (int i = 0; i < count; ++i)
			if (!blockBox(nodes_[children[i]]) || nodes_[children[i]].style.float_side) return false;
		if (vertical) {
			const bool measureInline = autosize && !hasExplicitHeight(node_) && node_.parent >= 0;
			int intrinsicHeight = 0;
			for (int i = 0; i < count; ++i) {
				engine_.layoutNode(children[i], contentWidth, contentHeight);
				const Node &child = nodes_[children[i]];
				intrinsicHeight = std::max(intrinsicHeight, child.layout.height + child.style.margin[0] + child.style.margin[2]);
			}
			if (measureInline) {
				node_.layout.height = clampLayoutSize(node_, intrinsicHeight + boxInsets(node_.style, false), false);
				contentHeight = paddedHeight();
			}
			for (int i = 0; i < count; ++i) {
				const int id = children[i]; Node &child = nodes_[id];
				if (measureInline) engine_.layoutNode(id, contentWidth, contentHeight);
				if (!hasExplicitHeight(child) && !isIntrinsicSizeExpression(child.style.height_expression) && writingMode(child) != 0 &&
				    !(preferredRatio(child) > 0 && hasExplicitWidth(child))) {
					const int height = clampLayoutSize(child, std::max(0, contentHeight - child.style.margin[0] - child.style.margin[2]), false);
					if (child.layout.height != height) {
						child.layout.height = clampInt16(height); engine_.repositionChildren(id);
					}
				}
				const int slack = contentHeight - child.layout.height - child.style.margin[0] - child.style.margin[2];
				const bool autoTop = child.style.margin_auto & 1, autoBottom = child.style.margin_auto & 4;
				int offset = slack > 0 && autoTop ? (autoBottom ? slack / 2 : slack) : 0;
				if (!autoTop && !autoBottom && gridAxisReversed(node_, false)) offset = slack;
				child.layout.y = boxInset(node_.style, 0) + child.style.margin[0] + offset;
			}
		} else {
			// Measure content before assigning the normal-flow fill width. A flex
			// item, float, or absolute box can use this intrinsic width; its parent
			// assigns the final width and calls repositionChildren afterwards.
			const bool measureIntrinsic = autosize && (node_.parent >= 0 || node_.style.float_side || intrinsicWidthConstraint(node_)) && !hasExplicitWidth(node_);
			int intrinsicWidth = 0;
			for (int i = 0; i < count; ++i) {
				const int id = children[i];
				engine_.layoutNode(id, contentWidth, contentHeight, measureIntrinsic);
				const Node &child = nodes_[id];
				intrinsicWidth = std::max(intrinsicWidth, child.layout.width + child.style.margin[1] + child.style.margin[3]);
			}
			if (measureIntrinsic) {
				node_.layout.width = clampLayoutSize(node_, intrinsicWidth + boxInsets(node_.style, true), true);
				contentWidth = paddedWidth();
			}
			for (int i = 0; i < count; ++i) {
				const int id = children[i];
				Node &child = nodes_[id];
				if (measureIntrinsic) engine_.layoutNode(id, contentWidth, contentHeight);
				// This horizontal formatting context fills the inline size of
				// parallel children. An orthogonal child's width is its block size.
				if (!hasExplicitWidth(child) && !isIntrinsicSizeExpression(child.style.width_expression) && writingMode(child) == 0 && !(preferredRatio(child) > 0 && hasExplicitHeight(child))) {
					const int width = clampLayoutSize(child, std::max(0, contentWidth - child.style.margin[1] - child.style.margin[3]), true);
					if (child.layout.width != width) {
						child.layout.width = clampInt16(width);
						engine_.repositionChildren(id);
					}
				}
				const int slack = contentWidth - child.layout.width - child.style.margin[1] - child.style.margin[3];
				const bool autoLeft = child.style.margin_auto & (1 << 3);
				const bool autoRight = child.style.margin_auto & (1 << 1);
				int offset = slack > 0 && autoLeft ? (autoRight ? slack / 2 : slack) : 0;
				if (!autoLeft && !autoRight && rightToLeft(node_)) offset = slack;
				child.layout.x = boxInset(node_.style, 3) + child.style.margin[3] + offset;
			}
		}
		const bool mergeTop = collapsesWithChildren(id_, false);
		const bool mergeBottom = collapsesWithChildren(id_, true);
		int cursor = 0;
		bool first = true;
		CollapsedMargin pending;
		for (int i = 0; i < count; ++i) {
			const int id = children[i];
			Node &child = nodes_[id];
			if (collapsesThrough(id)) {
				// CSS 2.2 8.3.1: an empty box sharing the parent's top margin
				// starts at that edge. Otherwise position it as if it had a bottom
				// border, before its own bottom and following siblings' margins
				// join the group. Its position does not advance normal flow.
				auto before = pending;
				before.add(childEdgeMargin(id_, id, false, true));
				(vertical ? child.layout.x : child.layout.y) = boxInset(node_.style, blockMarginSide(false)) + cursor + (first && mergeTop ? 0 : before.value());
				pending.add(childEdgeMargin(id_, id, false));
				continue;
			}
			pending.add(childEdgeMargin(id_, id, false));
			cursor += first && mergeTop ? 0 : pending.value();
			(vertical ? child.layout.x : child.layout.y) = boxInset(node_.style, blockMarginSide(false)) + cursor;
			cursor += vertical ? child.layout.width : child.layout.height;
			pending = childEdgeMargin(id_, id, true);
			first = false;
		}
		cursor += mergeBottom || (first && mergeTop) ? 0 : pending.value();
		if (vertical) {
			if (autosize && !hasExplicitWidth(node_) && !(scrollsOverflowX(node_.style) && node_.layout.width > 0))
				node_.layout.width = clampLayoutSize(node_, std::max(0, cursor) + boxInsets(node_.style, true), true);
			if (gridAxisReversed(node_, true)) for (int i = 0; i < count; ++i) {
				Node &child = nodes_[children[i]];
				child.layout.x = node_.layout.width - child.layout.x - child.layout.width;
			}
		} else if (autosize && !hasExplicitHeight(node_) && !(scrollsOverflowY(node_.style) && node_.layout.height > 0)) {
			node_.layout.height = clampLayoutSize(node_, std::max(0, cursor) + boxInsets(node_.style, false), false);
		}
		return true;
	}

	bool layoutFloatChildren(int *children, int count, int contentWidth, int contentHeight, bool autosize)
	{
		if (node_.style.display != kDisplayBlock) return false;
		auto containsFloat = [&](auto &&self, int parent) -> bool {
			for (int c = nodes_[parent].first_child; c >= 0; c = nodes_[c].next_sibling) {
				const Node &child = nodes_[c];
				if (isDisplayNone(child.style) || isOutOfFlowPosition(child.style.position)) continue;
				if (child.style.float_side || (!establishesBlockContext(child) && self(self, c))) return true;
			}
			return false;
		};
		bool blocks = false, inlines = false;
		for (int i = 0; i < count; ++i) {
			if (blockBox(nodes_[children[i]])) blocks = true;
			else if (!nodes_[children[i]].style.float_side) inlines = true;
		}
		if (!splitInlineChildren_ && !(blocks && inlines) && !containsFloat(containsFloat, id_)) return false;
		FloatContext context;
		const bool measureWidth = autosize && !hasExplicitWidth(node_) &&
		    (node_.parent >= 0 || node_.style.float_side || intrinsicWidthConstraint(node_)) &&
		    !(scrollsOverflowX(node_.style) && node_.layout.width > 0 && !intrinsicWidthConstraint(node_));
		layoutFloatChildrenInContext(children, count, contentWidth, contentHeight, autosize, context, 0, 0, measureWidth);
		if (measureWidth && paddedWidth() != contentWidth) {
			// As in ordinary block layout, measure first, then assign the used
			// containing width. Descendants in an existing float context already
			// have an assigned width and must not shrink it during placement.
			context = {};
			layoutFloatChildrenInContext(children, count, paddedWidth(), contentHeight, autosize, context, 0, 0);
		}
		floatContext_ = nullptr;
		return true;
	}

	void layoutFloatChildrenInContext(int *children, int count, int contentWidth, int contentHeight, bool autosize,
	                                 FloatContext &context, int originX, int originY, bool measureWidth = false)
	{
		floatContext_ = &context;
		auto &floats = context.boxes;
		int flowY = 0;
		int intrinsicWidth = 0;
		CollapsedMargin pending;
		bool first = true, clearedThrough = false;
		for (int i = 0; i < count; ++i) {
			Node &child = nodes_[children[i]];
			if (!blockBox(child) && !child.style.float_side) {
				int end = i + 1;
				// Keep a BR with the line it terminates, then recompute the
				// float exclusions for the next line.
				while (!isLineBreak(nodes_[children[end - 1]]) &&
				       end < count && !blockBox(nodes_[children[end]]) && !nodes_[children[end]].style.float_side) ++end;
				flowY += pending.value();
				pending = {};
				ScratchFrame inlineScratch;
					if (inlineScratch.valid()) {
						auto layoutRun = [&](int width) {
							FlexLayoutPass line(engine_, node_, children + i, end - i, true, width, width, contentHeight, inlineScratch.lines(), true);
							line.measureChildren();
							return line.layoutInlineFlow(false, true);
						};
						NodeRareData *ownerRare = rareDataFor(id_);
						const bool ownerLineWasValid = ownerRare && ownerRare->firstLineBackground.lineValid;
						auto extent = layoutRun(contentWidth);
					// If even the first unbreakable unit cannot fit beside a float,
					// move this line down to the next float boundary and retry.
					// Once the whole containing width is available, the word may overflow.
					int minimum = 0;
					for (int j = i; j < end && minimum == 0; ++j) {
						const Node &item = nodes_[children[j]];
						if (isLineBreak(item)) break;
						const int inner = FlexLayoutPass::transparentInlineRun(item);
						minimum = item.type == NodeType::Text || inner >= 0
						    ? TextRenderer::firstUnbreakableWidth(inner >= 0 ? nodes_[inner] : item)
						    : item.layout.width;
						minimum += item.style.margin[1] + item.style.margin[3];
						if (item.type == NodeType::Text) minimum += boxInsets(item.style, true);
					}
					int left = 0, right = contentWidth;
					for (;;) {
						left = 0; right = contentWidth;
						int nextBottom = INT_MAX;
						for (const auto &f : floats) {
							if (originY + flowY >= f.bottom || originY + flowY + extent.firstLineHeight <= f.top) continue;
							if (f.side == 1) left = std::max(left, f.right - originX);
							else right = std::min(right, f.left - originX);
							nextBottom = std::min(nextBottom, f.bottom - originY);
						}
						if (minimum <= right-left || (left == 0 && right == contentWidth) || nextBottom == INT_MAX) break;
						flowY = nextBottom;
					}
					if (right - left != contentWidth) extent = layoutRun(std::max(0, right - left));
					if (!ownerLineWasValid && ownerRare && ownerRare->firstLineBackground.lineValid)
						ownerRare->firstLineBackground.lineY = clampInt16(ownerRare->firstLineBackground.lineY + flowY);
					for (int j = i; j < end; ++j) {
						const int childId = children[j];
						const int runId = FlexLayoutPass::transparentInlineRun(nodes_[childId]);
						for (int fragmentId : {childId, runId}) {
							if (fragmentId < 0) continue;
							if (NodeRareData *rare = rareDataFor(fragmentId)) {
								auto &fragment = rare->firstLineFragment;
								if (fragment.valid && fragment.contextNode == id_) {
									fragment.x = clampInt16(fragment.x + left);
									fragment.y = clampInt16(fragment.y + flowY);
								}
							}
						}
					}
					for (int j = i; j < end; ++j) {
						nodes_[children[j]].layout.x += left;
						nodes_[children[j]].layout.y += flowY;
					}
					flowY += extent.height;
					intrinsicWidth = std::max(intrinsicWidth, left + extent.width);
					const Node &last = nodes_[children[end - 1]];
					if (isLineBreak(last)) for (const auto &f : floats)
						if (last.style.clear_side & f.side) flowY = std::max(flowY, f.bottom - originY);
				}
				first = false; clearedThrough = false;
				i = end - 1;
				continue;
			}
			engine_.layoutNode(children[i], contentWidth, contentHeight, measureWidth);
			const bool block = !child.style.float_side && blockBox(child);
			const int measuredOuterW = child.layout.width + child.style.margin[3] + child.style.margin[1];
			const bool automaticWidth = block && !hasExplicitWidth(child) && !isIntrinsicSizeExpression(child.style.width_expression) &&
			    writingMode(child) == 0 && !(preferredRatio(child) > 0 && hasExplicitHeight(child));
			// Ordinary blocks still fill their containing block beside floats;
			// only their line boxes are shortened by float exclusions.
			if (automaticWidth) {
				const int width = clampLayoutSize(child, std::max(0, contentWidth - child.style.margin[1] - child.style.margin[3]), true);
				if (child.layout.width != width) {
					child.layout.width = clampInt16(width);
					engine_.repositionChildren(children[i]);
				}
			}
			int outerW = child.layout.width + child.style.margin[3] + child.style.margin[1];
			int outerH = child.layout.height + child.style.margin[0] + child.style.margin[2];
			const bool mergeTop = first && collapsesWithChildren(id_, false);
			if (!block && !child.style.float_side) {
				flowY += mergeTop ? 0 : pending.value();
				pending = {};
				first = false;
			}
			auto before = pending;
			if (block) before.add(childEdgeMargin(id_, children[i], false, true));
			// Normal blocks clear their border edge, after adjoining margins
			// collapse. Floats instead clear their outer edge (CSS 2.2 9.5.2).
			int y = block ? flowY + (mergeTop ? 0 : before.value()) :
			    child.style.float_side ? std::max(flowY + (mergeTop ? 0 : pending.value()), context.lastTop - originY) : flowY;
			const int hypotheticalY = y;
			int clearBottom = INT_MIN;
			for (const auto &f : floats)
				if (child.style.clear_side & f.side) clearBottom = std::max(clearBottom, f.bottom - originY);
			const bool clearance = block && clearBottom != INT_MIN && (clearBottom > hypotheticalY || context.hasClearance(children[i]));
			// Once clearance separated the margins, rebasing the wrapper must
			// not mistake the now non-collapsing margin for a new hypothetical
			// position. CSS permits aligning exactly to the float bottom, even
			// when that requires negative clearance above the top margin.
			y = clearance ? clearBottom : std::max(y, clearBottom);
			if (clearance && !context.hasClearance(children[i])) context.cleared.push_back(children[i]);
			int left = 0, right = contentWidth, contextX = 0;
			const bool independentBlock = block && establishesBlockContext(child);
			if (child.style.float_side || child.style.display != kDisplayBlock || establishesBlockContext(child)) {
				for (;;) {
					left = 0; right = contentWidth;
					int nextY = 32767;
					bool hasLeft = false, hasRight = false;
					for (const auto &global : floats) {
						const FloatBox f{global.left - originX, global.top - originY, global.right - originX, global.bottom - originY, global.side};
						if (y >= f.bottom || y + std::max(1, independentBlock ? int(child.layout.height) : outerH) <= f.top) continue;
						if (f.side == 1) { left = std::max(left, f.right); hasLeft = true; }
						else { right = std::min(right, f.left); hasRight = true; }
						nextY = std::min(nextY, f.bottom);
					}
					bool fits = outerW <= right - left;
					if (independentBlock) {
						// A BFC's border box cannot overlap float margin boxes.
						// Automatic width uses the remaining interval. Even a
						// zero-width float prevents a negative margin crossing it.
						const int start = hasLeft ? std::max<int>(child.style.margin[3], left) : child.style.margin[3];
						const int end = hasRight ? std::min<int>(contentWidth - child.style.margin[1], right) : contentWidth - child.style.margin[1];
						contextX = start - child.style.margin[3];
						if (automaticWidth) {
							const int width = clampLayoutSize(child, std::max(0, end - start), true);
							if (child.layout.width != width) {
								const int previousHeight = child.layout.height;
								engine_.layoutNode(children[i], width + child.style.margin[1] + child.style.margin[3], contentHeight);
								child.layout.width = clampInt16(width);
								engine_.repositionChildren(children[i]);
								outerW = width + child.style.margin[1] + child.style.margin[3];
								outerH = child.layout.height + child.style.margin[0] + child.style.margin[2];
								if (previousHeight != child.layout.height) continue;
							}
						}
						fits = child.layout.width <= end - start;
					}
					if (fits || nextY == 32767) break;
					y = nextY;
				}
			}
			const int x = child.style.float_side == 2 ? right - outerW : independentBlock ? contextX : left;
			intrinsicWidth = std::max(intrinsicWidth, child.style.float_side
			    ? left + outerW + contentWidth - right : measuredOuterW);
			child.layout.x = boxInset(node_.style, 3) + x + child.style.margin[3];
			child.layout.y = boxInset(node_.style, 0) + y + (block ? 0 : child.style.margin[0]);
			if (block && !establishesBlockContext(child) && child.first_child >= 0) {
				LayoutNodePass nested(engine_, children[i], contentWidth, contentHeight);
				if (!nested.resolveInlineFormattingRow()) {
					ScratchFrame nestedScratch;
					if (nestedScratch.valid()) {
						const int nestedCount = nested.collectFormattingChildren(nestedScratch.children(), kMaxChildren);
						const auto precedingFloats = floats.size();
						const int precedingTop = context.lastTop;
						auto placeDescendants = [&]() {
							nested.layoutFloatChildrenInContext(nestedScratch.children(), nestedCount, nested.paddedWidth(), nested.paddedHeight(), true,
							    context, originX + x + child.style.margin[3] + boxInset(child.style, 3), originY + y + boxInset(child.style, 0));
						};
						placeDescendants();
						// Actual descendant clearance can separate a margin that was
						// adjoining this wrapper's top during hypothetical placement.
						// Rebase the subtree with that separation recorded; discard
						// its provisional floats before placing them at the final edge.
						auto separated = pending;
						separated.add(childEdgeMargin(id_, children[i], false, true));
						int finalY = flowY + (first && collapsesWithChildren(id_, false) ? 0 : separated.value());
						if (clearance) finalY = clearBottom;
						for (std::size_t f = 0; f < precedingFloats; ++f)
							if (child.style.clear_side & floats[f].side) finalY = std::max(finalY, floats[f].bottom - originY);
						if (finalY != y) {
							y = finalY;
							child.layout.y = boxInset(node_.style, 0) + y;
							floats.resize(precedingFloats);
							context.lastTop = precedingTop;
							placeDescendants();
						}
						nested.updateScrollContentSize();
						nested.positionAbsoluteChildren();
						nested.applyRelativeOffsets();
						// This result depends on preceding floats, beyond the size-only memo key.
						child.layout.memo_pass = child.layout.memo2_pass = 0;
					}
				}
			}
			if (child.style.float_side) {
				floats.push_back({originX + x, originY + y, originX + x + outerW, originY + y + outerH, child.style.float_side});
				context.lastTop = originY + y;
			} else if (block && collapsesThrough(children[i])) {
				if (clearance) {
					// The cleared empty box's margins can still collapse forward,
					// but that group cannot escape through the parent's bottom.
					flowY = y - childEdgeMargin(id_, children[i], false, true).value();
					pending = {};
					first = false;
					clearedThrough = true;
				}
				pending.add(childEdgeMargin(id_, children[i], false));
			} else if (block) {
				flowY = y + child.layout.height;
				pending = childEdgeMargin(id_, children[i], true);
				first = false;
				clearedThrough = false;
			} else flowY = y + outerH;
		}
		if (clearedThrough || (!collapsesWithChildren(id_, true) && !(first && collapsesWithChildren(id_, false))))
			flowY += pending.value();
		if (measureWidth)
			node_.layout.width = clampInt16(clampLayoutSize(node_, intrinsicWidth + boxInsets(node_.style, true), true));
		if (autosize && !hasExplicitHeight(node_)) {
			// Float enclosure is finalized with all same-context descendants.
			node_.layout.height = clampBorderSize(engine_, node_, flowY + boxInsets(node_.style, false), false);
		}
	}

	// Direction for FlexLayoutPass. Explicit flex-direction / display:flex win
	// (usesRowLayout). Otherwise a plain block whose in-flow children are all
	// inline-level flows them on a row (CSS inline formatting); a block with any
	// block-level in-flow child stacks them (column), as a browser would.
	bool resolveRowDirection() const
	{
		if (node_.style.display == kDisplayFlex)
			return flexRowDirection(node_.style) != (writingMode(node_) != 0);
		return resolveInlineFormattingRow();
	}

	// True when the row direction is SYNTHESIZED from inline-level children —
	// gea's stand-in for an inline formatting context — rather than declared by
	// display:flex / flex-direction. Such a row behaves like a run of line boxes
	// (wraps, never flex-shrinks, aligns on the shared baseline), which is what
	// FlexLayoutPass's `inlineRow` flag switches on.
	bool resolveInlineFormattingRow() const
	{
		if (usesRowLayout(node_.style)) return false;
		if (node_.style.flex_direction_explicit || node_.style.display != kDisplayBlock) return false;
		bool anyInFlow = false, allInline = true;
		visitFormattingChildren(nodes_, id_, [&](int c) {
			const Node &child = nodes_[c];
			if (child.style.display == kDisplayNone || isOutOfFlowPosition(child.style.position)) return;
			if (suppressAnonymousWhitespace(nodes_, child.parent, c)) return;
			if (!LayoutEngine::isInlineLevelNode(child)) allInline = false;
			anyInFlow = true;
		});
		return anyInFlow && allInline;
	}

	void measureGridChildren(int *children, int childCount, int padWidth, int padHeight)
	{
		for (int i = 0; i < childCount; ++i) {
			(void)padHeight;
			engine_.layoutNode(children[i], padWidth, 0);
		}
	}

	void computeTrackSizes(int count,
	                       const int8_t *types,
	                       const int16_t *values,
	                       const int *autoSizes,
	                       int available,
	                       int gap,
	                       int *outSizes)
	{
		int fixedTotal = 0;
		int totalFr = 0;
		for (int i = 0; i < count; ++i) {
			outSizes[i] = 0;
			if (types[i] == 1) {
				outSizes[i] = values[i];
				fixedTotal += outSizes[i];
			} else if (types[i] == 2) {
				totalFr += values[i] > 0 ? values[i] : 1;
			} else {
				outSizes[i] = autoSizes ? autoSizes[i] : 0;
				fixedTotal += outSizes[i];
			}
		}

		const int gapTotal = count > 1 ? (count - 1) * gap : 0;
		int remaining = available - fixedTotal - gapTotal;
		if (remaining < 0) remaining = 0;

		int assignedFr = 0;
		for (int i = 0; i < count; ++i) {
			if (types[i] != 2) continue;
			const int fr = values[i] > 0 ? values[i] : 1;
			int size = totalFr > 0 ? remaining * fr / totalFr : 0;
			outSizes[i] = size;
			assignedFr += size;
		}
		if (totalFr > 0 && assignedFr < remaining) {
			for (int i = count - 1; i >= 0; --i) {
				if (types[i] == 2) {
					outSizes[i] += remaining - assignedFr;
					break;
				}
			}
		}
	}

	int gridTrackTotalSize(int count, const int *sizes, int gap) const
	{
		int total = 0;
		for (int i = 0; i < count; ++i) total += sizes[i] + (i > 0 ? gap : 0);
		return total;
	}

	void stretchAutoGridTracks(int count, const int8_t *types, int *sizes,
	                           int available, int gap, int alignment) const
	{
		// CSS Grid track sizing, "Stretch auto Tracks": normal/stretch
		// distributes remaining space equally to auto tracks, not to items.
		if (alignment != 0) return;
		int free = available - gridTrackTotalSize(count, sizes, gap);
		int autoCount = 0;
		for (int i = 0; i < count; ++i) if (types[i] == 0) ++autoCount;
		if (free <= 0 || !autoCount) return;
		for (int i = 0; i < count; ++i) {
			if (types[i] != 0) continue;
			const int extra = free / autoCount;
			sizes[i] += extra;
			free -= extra;
			--autoCount;
		}
	}

	int gridContentOffset(int available, int total, int alignment, int index, int count) const
	{
		int free = available - total;
		if (isDistributedAlignment(alignment)) return distributedAlignmentOffset(alignment, free, index, count);
		alignment = usedAlignment(alignment, free);
		if (free < 0 && alignment != 1 && alignment != 2) free = 0;
		if (alignment == 1) return free / 2;
		if (alignment == 2) return free;
		return 0;
	}

	void stretchGridChild(int child, Node &childNode, int cellWidth, int cellHeight, int alignX, int alignY)
	{
		bool resized = false;
		const int availableWidth = cellWidth - childNode.style.margin[1] - childNode.style.margin[3];
		const int availableHeight = cellHeight - childNode.style.margin[0] - childNode.style.margin[2];
		const int width = clampLayoutSize(childNode, availableWidth < 0 ? 0 : availableWidth, true);
		const int height = clampLayoutSize(childNode, availableHeight < 0 ? 0 : availableHeight, false);
		if (alignX == 0 && !(childNode.style.margin_auto & 10) && !hasExplicitWidth(childNode) && !isIntrinsicSizeExpression(childNode.style.width_expression) && childNode.layout.width != width) {
			childNode.layout.width = width;
			resized = true;
		}
		if (alignY == 0 && !(childNode.style.margin_auto & 5) && !hasExplicitHeight(childNode) && !isIntrinsicSizeExpression(childNode.style.height_expression) && childNode.layout.height != height) {
			childNode.layout.height = height;
			resized = true;
		}
		if (resized) engine_.repositionChildren(child);
	}

	void layoutGridChildren(int *children, int childCount, int padWidth, int padHeight, bool allowAutosizeParent)
	{
		if (!childCount && !rstyle(node_.style).grid_column_count && !rstyle(node_.style).grid_row_count) {
			if (auto *rare = rareDataFor(id_)) rare->gridLayout.reset();
			if (allowAutosizeParent) autosizeEmptyNode();
			return;
		}
		const bool vertical = writingMode(node_) != 0;
		int columnCount = std::max(childCount ? 1 : 0, int(rstyle(node_.style).grid_column_count));
		int rowCount = std::max(childCount ? 1 : 0, int(rstyle(node_.style).grid_row_count));
		struct Placement { GridAxisPlacement column, row; };
		std::vector<Placement> placements(childCount);
		int columnOrigin = 0, rowOrigin = 0;
		for (int i = 0; i < childCount; ++i) {
			const auto &rs = rstyle(nodes_[children[i]].style);
			auto &p = placements[i];
			p.column = gridAxisPlacement(rs.grid_line[1], rs.grid_line[3], rstyle(node_.style).grid_column_count, false);
			p.row = gridAxisPlacement(rs.grid_line[0], rs.grid_line[2], rstyle(node_.style).grid_row_count, false);
			if (p.column.start != kUnset) columnOrigin = std::max(columnOrigin, -p.column.start);
			if (p.row.start != kUnset) rowOrigin = std::max(rowOrigin, -p.row.start);
			columnCount = std::max(columnCount, p.column.end == kUnset ? p.column.span : p.column.end);
			if (p.row.end != kUnset) rowCount = std::max(rowCount, p.row.end);
		}
		columnOrigin = std::min(columnOrigin, kMaxGridLayoutTracks - 1);
		rowOrigin = std::min(rowOrigin, kMaxGridLayoutTracks - 1);
		columnCount = std::min(kMaxGridLayoutTracks, columnCount + columnOrigin);
		rowCount = std::min(kMaxGridLayoutTracks, rowCount + rowOrigin);
		uint64_t occupied[kMaxGridLayoutTracks][2] = {};
		auto fits = [&](int row, int column, int rows, int columns) {
			if (row + rows > kMaxGridLayoutTracks || column + columns > kMaxGridLayoutTracks) return false;
			for (int y = row; y < row + rows; ++y)
				for (int x = column; x < column + columns; ++x)
					if (occupied[y][x / 64] & (uint64_t{1} << (x % 64))) return false;
			return true;
		};
		auto occupy = [&](Placement &p) {
			p.row.start = std::clamp(p.row.start, 0, kMaxGridLayoutTracks - p.row.span);
			p.column.start = std::clamp(p.column.start, 0, kMaxGridLayoutTracks - p.column.span);
			p.row.end = p.row.start + p.row.span;
			p.column.end = p.column.start + p.column.span;
			rowCount = std::max(rowCount, p.row.end);
			columnCount = std::max(columnCount, p.column.end);
			for (int y = p.row.start; y < p.row.end; ++y)
				for (int x = p.column.start; x < p.column.end; ++x)
					occupied[y][x / 64] |= uint64_t{1} << (x % 64);
		};
		for (auto &p : placements) {
			if (p.row.start != kUnset) p.row.start = std::clamp(p.row.start + rowOrigin, 0, kMaxGridLayoutTracks - p.row.span);
			if (p.column.start != kUnset) p.column.start = std::clamp(p.column.start + columnOrigin, 0, kMaxGridLayoutTracks - p.column.span);
			if (p.row.start != kUnset && p.column.start != kUnset) occupy(p);
		}
		int rowCursor[kMaxGridLayoutTracks] = {};
		for (auto &p : placements) {
			if (p.row.start == kUnset || p.column.start != kUnset) continue;
			int x = rowCursor[p.row.start];
			while (x + p.column.span < kMaxGridLayoutTracks && !fits(p.row.start, x, p.row.span, p.column.span)) ++x;
			p.column.start = x;
			occupy(p);
			rowCursor[p.row.start] = p.column.end;
		}
		int cursorRow = 0, cursorColumn = 0;
		for (auto &p : placements) {
			if (p.row.start != kUnset) continue;
			if (p.column.start != kUnset) {
				if (p.column.start < cursorColumn) ++cursorRow;
				cursorColumn = p.column.start;
				while (cursorRow + p.row.span < kMaxGridLayoutTracks && !fits(cursorRow, cursorColumn, p.row.span, p.column.span)) ++cursorRow;
			} else {
				while (cursorRow + p.row.span <= kMaxGridLayoutTracks) {
					if (cursorColumn + p.column.span > columnCount) { ++cursorRow; cursorColumn = 0; continue; }
					if (fits(cursorRow, cursorColumn, p.row.span, p.column.span)) break;
					++cursorColumn;
				}
				p.column.start = cursorColumn;
			}
			p.row.start = cursorRow;
			occupy(p);
			cursorColumn = p.column.end;
		}

		const int columnGap = resolvedGap(node_, false, padWidth, padHeight);
		const int rowGap = resolvedGap(node_, true, padWidth, padHeight);
		measureGridChildren(children, childCount, padWidth, padHeight);

		int8_t columnTypes[kMaxGridLayoutTracks] = {0};
		int8_t rowTypes[kMaxGridLayoutTracks] = {0};
		int16_t columnValues[kMaxGridLayoutTracks] = {0};
		int16_t rowValues[kMaxGridLayoutTracks] = {0};
		for (int i = 0; i < columnCount; ++i) {
			const int track = i - columnOrigin;
			columnTypes[i] = track >= 0 && track < rstyle(node_.style).grid_column_count ? rstyle(node_.style).grid_column_type[track] : 0;
			columnValues[i] = track >= 0 && track < rstyle(node_.style).grid_column_count ? rstyle(node_.style).grid_column_value[track] : 0;
		}
		for (int i = 0; i < rowCount; ++i) {
			const int track = i - rowOrigin;
			rowTypes[i] = track >= 0 && track < rstyle(node_.style).grid_row_count ? rstyle(node_.style).grid_row_type[track] : 0;
			rowValues[i] = track >= 0 && track < rstyle(node_.style).grid_row_count ? rstyle(node_.style).grid_row_value[track] : 0;
		}

		int autoColumns[kMaxGridLayoutTracks] = {0};
		int autoRows[kMaxGridLayoutTracks] = {0};
		// Resolve increasing span lengths. Contributions with the same span
		// share one set of base sizes and commit their maximum planned growth
		// together, so source order cannot bias overlapping spans.
		for (bool columns : {true, false}) {
			int *sizes = columns ? autoColumns : autoRows;
			const int8_t *types = columns ? columnTypes : rowTypes;
			const int16_t *values = columns ? columnValues : rowValues;
			const int gap = columns ? columnGap : rowGap;
			const int count = columns ? columnCount : rowCount;
			int maxSpan = 0;
			for (const auto &p : placements) maxSpan = std::max(maxSpan, columns ? p.column.span : p.row.span);
			for (int span = 1; span <= maxSpan; ++span) {
				int planned[kMaxGridLayoutTracks] = {};
				std::copy(sizes, sizes + count, planned);
				for (int i = 0; i < childCount; ++i) {
					const auto &p = columns ? placements[i].column : placements[i].row;
					if (p.span != span) continue;
					const Node &child = nodes_[children[i]];
					const bool horizontal = columns != vertical;
					const int size = horizontal ? child.layout.width + child.style.margin[1] + child.style.margin[3]
					                            : child.layout.height + child.style.margin[0] + child.style.margin[2];
					int occupied = (span - 1) * gap, autos = 0;
					for (int t = p.start; t < p.end; ++t) {
						occupied += types[t] == 1 ? values[t] : sizes[t];
						if (types[t] == 0) ++autos;
					}
					int extra = size - occupied;
					for (int t = p.start; t < p.end && extra > 0 && autos; ++t) {
						if (types[t] != 0) continue;
						const int amount = extra / autos;
						planned[t] = std::max(planned[t], sizes[t] + amount);
						extra -= amount; --autos;
					}
				}
				std::copy(planned, planned + count, sizes);
			}
		}

		int columnSizes[kMaxGridLayoutTracks] = {0};
		int rowSizes[kMaxGridLayoutTracks] = {0};
		computeTrackSizes(columnCount, columnTypes, columnValues, autoColumns, vertical ? padHeight : padWidth, columnGap, columnSizes);
		computeTrackSizes(rowCount, rowTypes, rowValues, autoRows, vertical ? padWidth : padHeight, rowGap, rowSizes);
		if (allowAutosizeParent && !hasExplicitWidth(node_) &&
		    rstyle(node_.style).grid_column_count == 0 && columnCount == 1 &&
		    writingMode(node_) == 0 && node_.style.justify_content == 0) columnSizes[0] = padWidth;

		if (allowAutosizeParent && !hasExplicitWidth(node_)) {
			int width = boxInset(node_.style, 1) + boxInset(node_.style, 3);
			width += vertical ? gridTrackTotalSize(rowCount, rowSizes, rowGap) : gridTrackTotalSize(columnCount, columnSizes, columnGap);
			node_.layout.width = clampLayoutSize(node_, width, true);
		}
		if (allowAutosizeParent && !hasExplicitHeight(node_)) {
			int height = boxInset(node_.style, 0) + boxInset(node_.style, 2);
			height += vertical ? gridTrackTotalSize(columnCount, columnSizes, columnGap) : gridTrackTotalSize(rowCount, rowSizes, rowGap);
			node_.layout.height = clampLayoutSize(node_, height, false);
		}
		// Auto-height grids first derive their size from intrinsic tracks.
		// A definite size (including one assigned by the parent) can then
		// contribute free space without making an auto-sized grid fill its parent.
		padWidth = paddedWidth();
		padHeight = paddedHeight();
		stretchAutoGridTracks(columnCount, columnTypes, columnSizes, vertical ? padHeight : padWidth, columnGap, node_.style.justify_content);
		stretchAutoGridTracks(rowCount, rowTypes, rowSizes, vertical ? padWidth : padHeight, rowGap, node_.style.align_content);

		int rowY[kMaxGridLayoutTracks] = {0};
		int columnX[kMaxGridLayoutTracks] = {0};
		const int columnTotal = gridTrackTotalSize(columnCount, columnSizes, columnGap);
		const int rowTotal = gridTrackTotalSize(rowCount, rowSizes, rowGap);
		int columnPosition = 0, rowPosition = 0;
		for (int i = 0; i < columnCount; ++i) {
			columnX[i] = columnPosition + gridContentOffset(vertical ? padHeight : padWidth, columnTotal, node_.style.justify_content, i, columnCount);
			columnPosition += columnSizes[i] + columnGap;
		}
		for (int i = 0; i < rowCount; ++i) {
			rowY[i] = rowPosition + gridContentOffset(vertical ? padWidth : padHeight, rowTotal, node_.style.align_content, i, rowCount);
			rowPosition += rowSizes[i] + rowGap;
		}

		auto &rare = ensureRareData(id_);
		if (!rare.gridLayout) rare.gridLayout = std::make_unique<GridTrackLayout>();
		auto &tracks = *rare.gridLayout;
		tracks.columnOrigin = columnOrigin; tracks.rowOrigin = rowOrigin;
		tracks.columnStart.assign(columnX, columnX + columnCount);
		tracks.rowStart.assign(rowY, rowY + rowCount);
		tracks.columnEnd.resize(columnCount); tracks.rowEnd.resize(rowCount);
		for (int i = 0; i < columnCount; ++i) tracks.columnEnd[i] = columnX[i] + columnSizes[i];
		for (int i = 0; i < rowCount; ++i) tracks.rowEnd[i] = rowY[i] + rowSizes[i];
		for (int i = 0; i < childCount; ++i) {
			const auto &p = placements[i];
			const int child = children[i];
			Node &childNode = nodes_[child];
			int cellX, cellY, cellWidth, cellHeight;
			gridPhysicalRange(node_, true, columnX[p.column.start], tracks.columnEnd[p.column.end - 1], vertical ? cellY : cellX, vertical ? cellHeight : cellWidth);
			gridPhysicalRange(node_, false, rowY[p.row.start], tracks.rowEnd[p.row.end - 1], vertical ? cellX : cellY, vertical ? cellWidth : cellHeight);
			engine_.layoutNode(child, cellWidth, cellHeight);
			const int inlineAlign = rstyle(childNode.style).justify_self >= 0 ? rstyle(childNode.style).justify_self : node_.style.justify_items;
			const int blockAlign = childNode.style.align_self >= 0 ? childNode.style.align_self : node_.style.align_items;
			const int alignX = vertical ? blockAlign : inlineAlign;
			const int alignY = vertical ? inlineAlign : blockAlign;
			stretchGridChild(child, childNode, cellWidth, cellHeight, alignX, alignY);

			int x = cellX + childNode.style.margin[3];
			int y = cellY + childNode.style.margin[0];
			const int freeX = cellWidth - childNode.layout.width - childNode.style.margin[1] - childNode.style.margin[3];
			const int freeY = cellHeight - childNode.layout.height - childNode.style.margin[0] - childNode.style.margin[2];
			const int usedX = usedGridAlignment(node_, childNode, alignX, freeX, true);
			const int usedY = usedGridAlignment(node_, childNode, alignY, freeY, false);
			// Auto margins consume positive free space before self-alignment and
			// suppress it even when the item overflows the grid area.
			if (childNode.style.margin_auto & 10) {
				if (freeX > 0 && (childNode.style.margin_auto & 8))
					x += (childNode.style.margin_auto & 2) ? freeX / 2 : freeX;
			} else if (usedX == 1) x += freeX / 2;
			else if (usedX == 2) x += freeX;
			if (childNode.style.margin_auto & 5) {
				if (freeY > 0 && (childNode.style.margin_auto & 1))
					y += (childNode.style.margin_auto & 4) ? freeY / 2 : freeY;
			} else if (usedY == 1) y += freeY / 2;
			else if (usedY == 2) y += freeY;
			childNode.layout.x = x;
			childNode.layout.y = y;
		}
	}

	void prepareOwnSize()
	{
		int contentWidth = resolvedStyleWidth(node_, availWidth_);
		int contentHeight = resolvedStyleHeight(node_, availHeight_);
		if (!hasExplicitWidth(node_)) {
			contentWidth -= node_.style.margin[1] + node_.style.margin[3];
		}
		if (!hasExplicitHeight(node_)) {
			contentHeight -= node_.style.margin[0] + node_.style.margin[2];
		}

		if (hasExplicitWidth(node_)) contentWidth = contentSizeToBorderSize(node_.style, contentWidth, true);
		if (hasExplicitHeight(node_)) contentHeight = contentSizeToBorderSize(node_.style, contentHeight, false);
		node_.layout.width = clampLayoutSize(node_, contentWidth, true);
		node_.layout.height = clampLayoutSize(node_, contentHeight, false);
		// Resolve a definite ratio-derived axis before measuring percentage
		// descendants. Final sizing can still grow to the content-based minimum.
		if (preferredRatio(node_) > 0) {
			if (hasExplicitWidth(node_) && !hasExplicitHeight(node_))
				node_.layout.height = clampLayoutSize(node_, ratioTransferredSize(node_, node_.layout.width, false), false);
			else if (hasExplicitHeight(node_) && !hasExplicitWidth(node_))
				node_.layout.width = clampLayoutSize(node_, ratioTransferredSize(node_, node_.layout.height, true), true);
		}
	}

	int paddedWidth() const
	{
		int width = node_.layout.width - boxInset(node_.style, 1) - boxInset(node_.style, 3);
		return width < 0 ? 0 : width;
	}

	int paddedHeight() const
	{
		int height = node_.layout.height - boxInset(node_.style, 0) - boxInset(node_.style, 2);
		return height < 0 ? 0 : height;
	}

	void autosizeEmptyNode()
	{
		if (!hasExplicitWidth(node_)) node_.layout.width = boxInset(node_.style, 1) + boxInset(node_.style, 3);
		if (!hasExplicitHeight(node_)) node_.layout.height = boxInset(node_.style, 0) + boxInset(node_.style, 2);
		node_.layout.width = clampLayoutSize(node_, node_.layout.width, true);
		node_.layout.height = clampLayoutSize(node_, node_.layout.height, false);
		// A <virtual-list>'s scroll geometry is owned by
		// applyVirtualListContentHeight() (invoked right after this in the
		// empty-node branch). Its recycled slot pool is entirely
		// position:absolute, so collectChildren() returns 0 and a scrollable
		// virtual-list lands here — resetting scroll_y/scroll_content_height
		// would wipe the scroll position on every relayout the moment the store
		// rewrites its slots.
		if (node_.type != NodeType::VirtualList) {
			node_.layout.scroll_content_width = node_.layout.width;
			node_.layout.scroll_content_height = node_.layout.height;
			node_.layout.scroll_x = 0;
			node_.layout.scroll_y = 0;
		}
	}

	void updateScrollContentSize()
	{
		// A <virtual-list>'s scroll geometry is owned by
		// applyVirtualListContentHeight() (scroll_content_height = itemCount *
		// rowHeight). Its real children are the recycled, position:absolute slot
		// pool, which this children-bottom walk skips — so running here would
		// collapse scroll_content_height to the node height, force maxScroll to
		// 0, and clamp scroll_y back to 0 on every relayout (wiping the scroll
		// position the moment the store rewrites its slots).
		if (node_.type == NodeType::VirtualList) return;
		int contentWidth = node_.layout.width;
		int contentHeight = node_.layout.height;
		visitFormattingChildren(nodes_, id_, [&](int child) {
			Node &childNode = nodes_[child];
			if (isCollapsedFlexItem(childNode) || isDisplayNone(childNode.style) || isOutOfFlowPosition(childNode.style.position)) return;
			// A visible axis contributes descendant overflow; a clipping axis
			// traps it at this child's border box independently of the other axis.
			const int width = overflowX(childNode.style) == 0 ? std::max<int>(childNode.layout.width, childNode.layout.scroll_content_width) : childNode.layout.width;
			const int height = overflowY(childNode.style) == 0 ? std::max<int>(childNode.layout.height, childNode.layout.scroll_content_height) : childNode.layout.height;
			const int right = childNode.layout.x + width + childNode.style.margin[1] + boxInset(node_.style, 1);
			if (right > contentWidth) contentWidth = right;
			const int bottom = childNode.layout.y + height + childNode.style.margin[2] + boxInset(node_.style, 2);
			if (bottom > contentHeight) contentHeight = bottom;
		});
		node_.layout.scroll_content_width = contentWidth;
		node_.layout.scroll_content_height = contentHeight;

		int maxScrollX = node_.layout.scroll_content_width - node_.layout.width;
		if (maxScrollX < 0) maxScrollX = 0;
		if (!scrollsOverflowX(node_.style)) {
			node_.layout.scroll_x = 0;
		} else {
			if (node_.layout.scroll_x < 0) node_.layout.scroll_x = 0;
			if (node_.layout.scroll_x > maxScrollX) node_.layout.scroll_x = maxScrollX;
		}

		int maxScroll = node_.layout.scroll_content_height - node_.layout.height;
		if (maxScroll < 0) maxScroll = 0;
		if (!scrollsOverflowY(node_.style)) {
			node_.layout.scroll_y = 0;
		} else {
			if (node_.layout.scroll_y < 0) node_.layout.scroll_y = 0;
			if (node_.layout.scroll_y > maxScroll) node_.layout.scroll_y = maxScroll;
		}
	}

	// A <virtual-list> only materializes a small pool of real child slot nodes
	// but scrolls over a virtual content height of itemCount * rowHeight. The
	// row height comes from the first slot's resolved CSS height, so the app
	// controls it purely through the template's stylesheet.
	void applyVirtualListContentHeight()
	{
		if (node_.type != NodeType::VirtualList) return;
		int rowHeight = 0;
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			if (isDisplayNone(nodes_[child].style)) continue;
			rowHeight = nodes_[child].layout.height;
			break;
		}
		int content = VirtualListRenderer::virtualContentHeight(id_, rowHeight);
		if (content < node_.layout.height) content = node_.layout.height;
		node_.layout.scroll_content_width = node_.layout.width;
		node_.layout.scroll_content_height = content;
		node_.layout.scroll_x = 0;
		int maxScroll = node_.layout.scroll_content_height - node_.layout.height;
		if (maxScroll < 0) maxScroll = 0;
		if (node_.layout.scroll_y < 0) node_.layout.scroll_y = 0;
		if (node_.layout.scroll_y > maxScroll) node_.layout.scroll_y = maxScroll;
	}

	void captureBlockStaticPositions()
	{
		for (int c = node_.first_child; c >= 0; c = nodes_[c].next_sibling)
			if (isOutOfFlowPosition(nodes_[c].style.position)) nodes_[c].layout.static_block_axis = 0;
		if (node_.style.display != kDisplayBlock || node_.style.flex_direction_explicit ||
		    (LayoutEngine::isInlineLevelNode(node_) && !isOutOfFlowPosition(node_.style.position))) return;
		const bool vertical = writingMode(node_) != 0;
		const bool reversed = vertical && gridAxisReversed(node_, true);
		int cursor = boxInset(node_.style, blockMarginSide(false));
		bool first = true;
		CollapsedMargin pending;
		visitFormattingChildren(nodes_, id_, [&](int id) {
			Node &child = nodes_[id];
			if (isDisplayNone(child.style)) return;
			if (isOutOfFlowPosition(child.style.position)) {
				if (child.parent != id_ || !blockBox(child)) return;
				const int edge = cursor + (first && collapsesWithChildren(id_, false) ? 0 : pending.value());
				child.layout.static_block_start = clampInt16(reversed ? node_.layout.width - edge : edge);
				child.layout.static_block_axis = vertical ? 2 : 1;
				return;
			}
			if (child.style.float_side || suppressAnonymousWhitespace(nodes_, child.parent, id)) return;
			if (blockBox(child) && collapsesThrough(id)) {
				pending.add(childEdgeMargin(id_, id, false));
				return;
			}
			const int end = vertical ? (reversed ? node_.layout.width - child.layout.x : child.layout.x + child.layout.width)
			                         : child.layout.y + child.layout.height;
			cursor = blockBox(child) ? end : std::max(cursor, end);
			pending = blockBox(child) ? childEdgeMargin(id_, id, true) : CollapsedMargin{};
			first = false;
		});
	}

	void positionAbsoluteChildren()
	{
		captureBlockStaticPositions();
		for (int child = node_.first_child; child >= 0; child = nodes_[child].next_sibling) {
			Node &childNode = nodes_[child];
			if (isDisplayNone(childNode.style) || !isOutOfFlowPosition(childNode.style.position)) continue;

			int childAvailWidth, childAvailHeight;
			int areaX, areaY;
			LayoutEngine::absoluteContainingArea(node_, childNode, areaX, areaY, childAvailWidth, childAvailHeight);
			engine_.layoutNode(child, childAvailWidth, childAvailHeight);
			stretchAbsoluteChild(child, childNode, childAvailWidth, childAvailHeight);
			positionAbsoluteChild(childNode);
		}
	}

	void stretchAbsoluteChild(int child, Node &childNode, int childAvailWidth, int childAvailHeight)
	{
		bool resized = false;
		if (!hasExplicitWidth(childNode) && !isIntrinsicSizeExpression(childNode.style.width_expression) &&
		    hasPositionOffset(childNode, 3) &&
		    hasPositionOffset(childNode, 1)) {
			int width = childAvailWidth - resolvedPositionOffsetWithBasis(childNode, 3, childAvailWidth) - resolvedPositionOffsetWithBasis(childNode, 1, childAvailWidth) -
			            childNode.style.margin[1] - childNode.style.margin[3];
			if (width < 0) width = 0;
			width = clampLayoutSize(childNode, width, true);
			if (childNode.layout.width != width) {
				childNode.layout.width = width;
				resized = true;
			}
		}

		if (!hasExplicitHeight(childNode) && !isIntrinsicSizeExpression(childNode.style.height_expression) &&
		    hasPositionOffset(childNode, 0) &&
		    hasPositionOffset(childNode, 2)) {
			int height = childAvailHeight - resolvedPositionOffsetWithBasis(childNode, 0, childAvailHeight) - resolvedPositionOffsetWithBasis(childNode, 2, childAvailHeight) -
			             childNode.style.margin[0] - childNode.style.margin[2];
			if (height < 0) height = 0;
			height = clampLayoutSize(childNode, height, false);
			if (childNode.layout.height != height) {
				childNode.layout.height = height;
				resized = true;
			}
		}

		if (resized) engine_.repositionChildren(child);
	}

	void positionAbsoluteChild(Node &childNode)
	{
		int x, y, width, height;
		LayoutEngine::absoluteContainingArea(node_, childNode, x, y, width, height);
		if (hasPositionOffset(childNode, 3)) childNode.layout.x = x + resolvedPositionOffsetWithBasis(childNode, 3, width) + childNode.style.margin[3];
		else if (hasPositionOffset(childNode, 1)) childNode.layout.x = x + width - childNode.layout.width - resolvedPositionOffsetWithBasis(childNode, 1, width) - childNode.style.margin[1];
		else childNode.layout.x = alignedAbsoluteChildPosition(childNode, true);
		if (hasPositionOffset(childNode, 0)) childNode.layout.y = y + resolvedPositionOffsetWithBasis(childNode, 0, height) + childNode.style.margin[0];
		else if (hasPositionOffset(childNode, 2)) childNode.layout.y = y + height - childNode.layout.height - resolvedPositionOffsetWithBasis(childNode, 2, height) - childNode.style.margin[2];
		else childNode.layout.y = alignedAbsoluteChildPosition(childNode, false);
	}

	int alignedAbsoluteChildPosition(const Node &childNode, bool horizontal) const
	{
		return LayoutEngine::alignedAbsoluteOffset(node_, childNode, horizontal);
	}

	void applyRelativeOffsets()
	{
		auto apply = [&](auto &&self, int parent) -> void {
			for (int child = nodes_[parent].first_child; child >= 0; child = nodes_[child].next_sibling) {
				Node &childNode = nodes_[child];
				if (splitInlineChildren_ && splitInlineWrapper(nodes_, child)) self(self, child);
				if (childNode.style.position != 2) continue;
				if (hasPositionOffset(childNode, 0))
					childNode.layout.y += resolvedRelativePositionOffset(childNode, 0);
				else if (hasPositionOffset(childNode, 2))
					childNode.layout.y -= resolvedRelativePositionOffset(childNode, 2);
				if (hasPositionOffset(childNode, 3))
					childNode.layout.x += resolvedRelativePositionOffset(childNode, 3);
				else if (hasPositionOffset(childNode, 1))
					childNode.layout.x -= resolvedRelativePositionOffset(childNode, 1);
			}
		};
		apply(apply, id_);
	}

	bool hasPositionOffset(const Node &node, int side) const
	{
		return node.style.pos_offsets[side] != kUnset || node.style.pos_offset_percent[side] != kUnset;
	}

	int resolvedPositionOffset(const Node &node, int side) const
	{
		return resolvedPositionOffsetWithBasis(node, side, (side == 0 || side == 2) ? node_.layout.height : node_.layout.width);
	}

	int resolvedRelativePositionOffset(const Node &node, int side) const
	{
		int basis = (side == 0 || side == 2) ? node_.layout.height : node_.layout.width;
		if (isDisplayGrid(node_.style)) {
			basis = (side == 0 || side == 2) ? node.layout.height : node.layout.width;
		}
		return resolvedPositionOffsetWithBasis(node, side, basis);
	}

	int resolvedPositionOffsetWithBasis(const Node &node, int side, int percentBasis) const
	{
		int offset = node.style.pos_offsets[side] != kUnset ? node.style.pos_offsets[side] : 0;
		const int percent = node.style.pos_offset_percent[side];
		if (percent != kUnset) {
			const int numerator = percentBasis * percent;
			offset += (numerator + (numerator >= 0 ? 500 : -500)) / 1000;
		}
		return offset;
	}

	LayoutEngine &engine_;
	int id_ = 0;
	int availWidth_ = 0;
	int availHeight_ = 0;
	Node *nodes_ = nullptr;
	Node &node_;
};

bool establishesTransformContainingBlock(const Node &node)
{
	if (!ViewRenderer::isTransformableBox(node)) return false;
	const auto &r = rstyle(node.style);
	return r.translate_present || r.transform_present || r.rotate_present || r.scale_present || r.filter_present || r.transform_preserve_3d ||
	    r.transform_rotate || r.transform_rotate_x || r.transform_rotate_y ||
	    composedTranslateX(r) || composedTranslateY(r) || composedTranslateZ(r) ||
	    composedTranslateXPercent(r) || composedTranslateYPercent(r) ||
	    r.transform_scale_x != 1000 || r.transform_scale_y != 1000 || r.transform_scale_z != 1000 || r.perspective || r.filter_blur_radius;
}

bool establishesAbsoluteContainingBlock(const Node &node)
{
	return isOutOfFlowPosition(node.style.position) || node.style.position == 2 || establishesTransformContainingBlock(node);
}

int containingBlockForAbsoluteNode(int node, int root, Node *nodes)
{
	if (nodes[node].style.position == kPositionFixed) {
		const int fixed = LayoutEngine::fixedContainingBlock(nodes[node]);
		return fixed >= 0 ? fixed : root;
	}
	for (int cursor = nodes[node].parent; cursor >= 0; cursor = nodes[cursor].parent) {
		if (establishesAbsoluteContainingBlock(nodes[cursor])) return cursor;
	}
	return root;
}

bool hasPositionOffsetValue(const Node &node, int side)
{
	return node.style.pos_offsets[side] != kUnset || node.style.pos_offset_percent[side] != kUnset;
}

int resolvedPositionOffsetForBasis(const Node &node, int side, int percentBasis)
{
	int offset = node.style.pos_offsets[side] != kUnset ? node.style.pos_offsets[side] : 0;
	const int percent = node.style.pos_offset_percent[side];
	if (percent != kUnset) {
		const int numerator = percentBasis * percent;
		offset += (numerator + (numerator >= 0 ? 500 : -500)) / 1000;
	}
	return offset;
}

void offsetFromAncestorToNode(int ancestor, int node, Node *nodes, int *outX, int *outY)
{
	int x = 0;
	int y = 0;
	for (int cursor = node; cursor >= 0 && cursor != ancestor; cursor = nodes[cursor].parent) {
		const Node &current = nodes[cursor];
		x += current.layout.x;
		y += current.layout.y;
		if (scrollsOverflowX(current.style)) x -= current.layout.scroll_x;
		if (scrollsOverflowY(current.style)) y -= current.layout.scroll_y;
	}
	*outX = x;
	*outY = y;
}

void stretchAbsoluteNodeToContainingBlock(int node, int containingWidth, int containingHeight, Node *nodes)
{
	Node &absolute = nodes[node];
	bool resized = false;
	if (!hasExplicitWidth(absolute) && !isIntrinsicSizeExpression(absolute.style.width_expression) &&
	    hasPositionOffsetValue(absolute, 3) &&
	    hasPositionOffsetValue(absolute, 1)) {
		int width = containingWidth -
		            resolvedPositionOffsetForBasis(absolute, 3, containingWidth) -
		            resolvedPositionOffsetForBasis(absolute, 1, containingWidth) -
		            absolute.style.margin[1] - absolute.style.margin[3];
		if (width < 0) width = 0;
		width = clampLayoutSize(absolute, width, true);
		if (absolute.layout.width != width) {
			absolute.layout.width = width;
			resized = true;
		}
	}

	if (!hasExplicitHeight(absolute) && !isIntrinsicSizeExpression(absolute.style.height_expression) &&
	    hasPositionOffsetValue(absolute, 0) &&
	    hasPositionOffsetValue(absolute, 2)) {
		int height = containingHeight -
		             resolvedPositionOffsetForBasis(absolute, 0, containingHeight) -
		             resolvedPositionOffsetForBasis(absolute, 2, containingHeight) -
		             absolute.style.margin[0] - absolute.style.margin[2];
		if (height < 0) height = 0;
		height = clampLayoutSize(absolute, height, false);
		if (absolute.layout.height != height) {
			absolute.layout.height = height;
			resized = true;
		}
	}

	if (resized) LayoutEngine::instance().repositionChildren(node);
}

void layoutAbsoluteNode(int node, int containing, int areaX, int areaWidth, int areaHeight, Node *nodes)
{
	Node &absolute = nodes[node];
	auto &engine = LayoutEngine::instance();
	const bool left = hasPositionOffsetValue(absolute, 3), right = hasPositionOffsetValue(absolute, 1);
	if (absolute.type != NodeType::View || writingMode(absolute) != 0 ||
	    hasExplicitWidth(absolute) || isIntrinsicSizeExpression(absolute.style.width_expression) ||
	    (left && right) || (preferredRatio(absolute) > 0 && hasExplicitHeight(absolute))) {
		engine.layoutNode(node, areaWidth, areaHeight);
		return;
	}
	// Auto inline sizes shrink to fit the space from their resolved inset (or
	// static position) to the opposite containing-block edge. Keep the actual
	// containing width for percentages during the final used-width layout.
	IntrinsicSizeScope scope(absolute, 1);
	engine.layoutNode(node, 1, areaHeight, true);
	const int minimum = std::max(0, absolute.layout.width - boxInsets(absolute.style, true));
	scope.constraint = 2;
	engine.layoutNode(node, 32767, areaHeight, true);
	const int maximum = std::max(minimum, absolute.layout.width - boxInsets(absolute.style, true));
	resolveLayoutBoxLengths(node, areaWidth);
	int available = areaWidth - absolute.style.margin[3] - absolute.style.margin[1];
	if (left || right) {
		available -= resolvedPositionOffsetForBasis(absolute, left ? 3 : 1, areaWidth);
	} else {
		int parentX = 0, parentY = 0;
		offsetFromAncestorToNode(containing, absolute.parent, nodes, &parentX, &parentY);
		const int staticX = parentX + engine.alignedAbsoluteOffset(nodes[absolute.parent], absolute, true);
		available = rightToLeft(nodes[absolute.parent])
		    ? staticX + absolute.layout.width - areaX - absolute.style.margin[3]
		    : areaX + areaWidth - staticX - absolute.style.margin[1];
	}
	const int edges = boxInsets(absolute.style, true);
	scope.borderSize = edges + std::min(maximum, std::max(minimum, available - edges));
	scope.constraint = 0;
	engine.layoutNode(node, areaWidth, areaHeight);
}

void positionAbsoluteNodeInContainingBlock(int node, int containing, Node *nodes)
{
	Node &absolute = nodes[node];
	const Node &containingNode = nodes[containing];
	const int parent = absolute.parent;
	if (parent < 0) return;
	if (LayoutEngine::isViewportFixed(absolute)) {
		const int width = containingNode.layout.memo_avail_w;
		const int height = containingNode.layout.memo_avail_h;
		LayoutEngine::instance().layoutNode(node, width, height);
		stretchAbsoluteNodeToContainingBlock(node, width, height, nodes);
		int staticX = LayoutEngine::alignedAbsoluteOffset(nodes[parent], absolute, true);
		int staticY = LayoutEngine::alignedAbsoluteOffset(nodes[parent], absolute, false);
		for (int id = parent; id >= 0; id = nodes[id].parent) {
			staticX += nodes[id].layout.x; staticY += nodes[id].layout.y;
			if (LayoutEngine::isViewportFixed(nodes[id])) break;
		}
		absolute.layout.x = hasPositionOffsetValue(absolute, 3) ? resolvedPositionOffsetForBasis(absolute, 3, width) + absolute.style.margin[3]
		    : hasPositionOffsetValue(absolute, 1) ? width - absolute.layout.width - resolvedPositionOffsetForBasis(absolute, 1, width) - absolute.style.margin[1] : staticX;
		absolute.layout.y = hasPositionOffsetValue(absolute, 0) ? resolvedPositionOffsetForBasis(absolute, 0, height) + absolute.style.margin[0]
		    : hasPositionOffsetValue(absolute, 2) ? height - absolute.layout.height - resolvedPositionOffsetForBasis(absolute, 2, height) - absolute.style.margin[2] : staticY;
		return;
	}

	int areaX, areaY, areaWidth, areaHeight;
	LayoutEngine::absoluteContainingArea(containingNode, absolute, areaX, areaY, areaWidth, areaHeight);
	// The provisional parent pass may have measured this box against a static
	// wrapper (or an as-yet-unsized grid). Resolve dimensions using the same
	// final containing block that supplies its insets. The static-position
	// rectangle still comes from the parent, but alignment must use the final
	// dimensions (for example, a percentage-sized child centered in a grid
	// whose containing block is an outer positioned ancestor).
	layoutAbsoluteNode(node, containing, areaX, areaWidth, areaHeight, nodes);
	stretchAbsoluteNodeToContainingBlock(node, areaWidth, areaHeight, nodes);
	int parentOffsetX = 0, parentOffsetY = 0;
	offsetFromAncestorToNode(containing, parent, nodes, &parentOffsetX, &parentOffsetY);
	if (!hasPositionOffsetValue(absolute, 3) && !hasPositionOffsetValue(absolute, 1))
		absolute.layout.x = LayoutEngine::alignedAbsoluteOffset(nodes[parent], absolute, true, &containingNode, areaX - parentOffsetX, areaWidth);
	if (!hasPositionOffsetValue(absolute, 0) && !hasPositionOffsetValue(absolute, 2))
		absolute.layout.y = LayoutEngine::alignedAbsoluteOffset(nodes[parent], absolute, false, &containingNode, areaY - parentOffsetY, areaHeight);

	int containingX = absolute.layout.x + parentOffsetX;
	if (hasPositionOffsetValue(absolute, 3))
		containingX = areaX + resolvedPositionOffsetForBasis(absolute, 3, areaWidth) + absolute.style.margin[3];
	else if (hasPositionOffsetValue(absolute, 1))
		containingX = areaX + areaWidth - absolute.layout.width -
		              resolvedPositionOffsetForBasis(absolute, 1, areaWidth) - absolute.style.margin[1];

	int containingY = absolute.layout.y + parentOffsetY;
	if (hasPositionOffsetValue(absolute, 0))
		containingY = areaY + resolvedPositionOffsetForBasis(absolute, 0, areaHeight) + absolute.style.margin[0];
	else if (hasPositionOffsetValue(absolute, 2))
		containingY = areaY + areaHeight - absolute.layout.height -
		              resolvedPositionOffsetForBasis(absolute, 2, areaHeight) - absolute.style.margin[2];

	absolute.layout.x = containingX - parentOffsetX;
	absolute.layout.y = containingY - parentOffsetY;
}

void resolvePositionedDescendants(int node, int root, Node *nodes)
{
	for (int child = nodes[node].first_child; child >= 0; child = nodes[child].next_sibling) {
		if (isDisplayNone(nodes[child].style)) continue;
		if (isOutOfFlowPosition(nodes[child].style.position))
			positionAbsoluteNodeInContainingBlock(child, containingBlockForAbsoluteNode(child, root, nodes), nodes);
		resolvePositionedDescendants(child, root, nodes);
	}
}

void resolveAbsoluteContainingBlocks(int root)
{
	resolvePositionedDescendants(root, root, Tree::instance().nodes());
}

}  // namespace

LayoutEngine &LayoutEngine::instance()
{
	static LayoutEngine engine;
	return engine;
}

int LayoutEngine::fixedContainingBlock(const Node &node)
{
	const Node *nodes = Tree::instance().nodes();
	for (int id = node.parent; id >= 0; id = nodes[id].parent) {
		if (establishesTransformContainingBlock(nodes[id])) return id;
	}
	return -1;
}

bool LayoutEngine::isViewportFixed(const Node &node)
{
	return node.style.position == kPositionFixed && fixedContainingBlock(node) < 0;
}

bool LayoutEngine::containsViewportFixed(int node)
{
	if (!treeState().fixedPositionUsed) return false;
	const Node *nodes = Tree::instance().nodes();
	if (isViewportFixed(nodes[node])) return true;
	for (int child = nodes[node].first_child; child >= 0; child = nodes[child].next_sibling)
		if (containsViewportFixed(child)) return true;
	return false;
}

void LayoutEngine::absoluteContainingArea(const Node &parent, const Node &child, int &x, int &y, int &width, int &height)
{
	x = y = 0;
	width = parent.layout.width; height = parent.layout.height;
	if (isDocumentCanvasRoot(parent) && !establishesAbsoluteContainingBlock(parent)) {
		// The initial containing block is viewport-sized, even when the HTML
		// element has an auto height, margins, or an explicit smaller size.
		x = -parent.layout.x; y = -parent.layout.y;
		width = parent.layout.memo_avail_w; height = parent.layout.memo_avail_h;
		return;
	}
	if (establishesAbsoluteContainingBlock(parent) || child.style.position == kPositionFixed) {
		// Insets and percentages reference the padding box, excluding borders.
		x = boxInset(parent.style, 3) - parent.style.padding[3];
		y = boxInset(parent.style, 0) - parent.style.padding[0];
		width = std::max(0, width - x - boxInset(parent.style, 1) + parent.style.padding[1]);
		height = std::max(0, height - y - boxInset(parent.style, 2) + parent.style.padding[2]);
	}
	absoluteGridArea(parent, child, x, y, width, height);
}

bool LayoutEngine::absoluteGridArea(const Node &parent, const Node &child, int &x, int &y, int &width, int &height)
{
	if (!isDisplayGrid(parent.style)) return false;
	if (child.style.position == kPositionFixed) {
		if (fixedContainingBlock(child) != &parent - Tree::instance().nodes()) return false;
	} else if (!establishesAbsoluteContainingBlock(parent)) return false;
	const auto *rare = rareDataFor(static_cast<int>(&parent - Tree::instance().nodes()));
	const GridTrackLayout *tracks = rare ? rare->gridLayout.get() : nullptr;
	const bool vertical = writingMode(parent) != 0;
	for (bool columns : {true, false}) {
		const bool horizontal = columns != vertical;
		const int explicitCount = columns ? rstyle(parent.style).grid_column_count : rstyle(parent.style).grid_row_count;
		const auto &lines = rstyle(child.style).grid_line;
		const std::vector<int> *starts = tracks ? (columns ? &tracks->columnStart : &tracks->rowStart) : nullptr;
		const std::vector<int> *ends = tracks ? (columns ? &tracks->columnEnd : &tracks->rowEnd) : nullptr;
		const int origin = tracks ? (columns ? tracks->columnOrigin : tracks->rowOrigin) : 0;
		const int count = starts ? static_cast<int>(starts->size()) : 0;
		auto existing = [origin, count](int line) { return line != kUnset && line + origin >= 0 && line + origin <= count; };
		// Invalid absolute line references become auto before a dependent span
		// is resolved; otherwise an absent end line could invent a start line.
		auto validLine = [&](int value) {
			if (!value || value >= kGridLineSpan) return value;
			const int index = value > 0 ? value - 1 : explicitCount + 1 + value;
			return existing(index) ? value : 0;
		};
		auto placement = gridAxisPlacement(validLine(lines[columns ? 1 : 0]), validLine(lines[columns ? 3 : 2]), explicitCount, true);
		const bool hasStart = existing(placement.start), hasEnd = existing(placement.end);
		const int available = (horizontal ? parent.layout.width : parent.layout.height) - boxInsets(parent.style, horizontal);
		int beforeSide = horizontal ? 3 : 0, afterSide = horizontal ? 1 : 2;
		if (gridAxisReversed(parent, horizontal)) std::swap(beforeSide, afterSide);
		int start = -parent.style.padding[beforeSide], end = available + parent.style.padding[afterSide];
		if (hasStart) {
			const int index = placement.start + origin;
			start = count == 0 ? 0 : index == count ? ends->back() : (*starts)[index];
		}
		if (hasEnd) {
			const int index = placement.end + origin;
			end = count == 0 ? 0 : index == 0 ? starts->front() : (*ends)[index - 1];
		}
		gridPhysicalRange(parent, columns, start, std::max(start, end), horizontal ? x : y, horizontal ? width : height);
	}
	return true;
}

int LayoutEngine::alignedAbsoluteOffset(const Node &parent, const Node &childNode, bool horizontal,
                                        const Node *containing, int containingStart, int containingSize)
{
	const Node *treeNodes = Tree::instance().nodes();
	const int childId = static_cast<int>(&childNode - treeNodes);
	if (const auto *rare = rareDataFor(childId); rare && rare->inlineStaticPosition.valid) {
		int continuationOrigin = 0;
		if (horizontal && rare->inlineStaticPosition.continuationLine) {
			// Resolve fragmented inline origins here, after ancestor layout has
			// finalized their first-fragment x positions. The continuation line
			// starts at the nearest formatting block's content edge, not at the
			// inline parent's first-fragment padding edge.
			int inlineOffset = 0;
			int cursor = static_cast<int>(&parent - treeNodes);
			while (cursor >= 0 && isCssInlineLevelBox(treeNodes[cursor])) {
				inlineOffset += treeNodes[cursor].layout.x;
				cursor = treeNodes[cursor].parent;
			}
			if (cursor >= 0) continuationOrigin = boxInset(treeNodes[cursor].style, 3) - inlineOffset;
		}
		return (horizontal ? rare->inlineStaticPosition.x + continuationOrigin + childNode.style.margin[3]
		                   : rare->inlineStaticPosition.y + childNode.style.margin[0]);
	}
	if (parent.style.display == kDisplayBlock && !parent.style.flex_direction_explicit &&
	    childNode.layout.static_block_axis == (horizontal ? 2 : 1)) {
		const bool reversed = horizontal && gridAxisReversed(parent, true);
		return childNode.layout.static_block_start + (reversed ? -childNode.layout.width - childNode.style.margin[1]
		    : childNode.style.margin[horizontal ? 3 : 0]);
	}
	// In block layout the hypothetical box starts at the parent's inline
	// content edge. RTL anchors its inline-end margin edge, even when the absolute
	// box is wider than its static-position parent. Its actual containing
	// block can be an entirely different ancestor.
	if (horizontal == (writingMode(parent) == 0) && parent.style.display == kDisplayBlock &&
	    (!isInlineLevelNode(parent) || isOutOfFlowPosition(parent.style.position)) &&
	    !parent.style.flex_direction_explicit) {
		return rightToLeft(parent)
		    ? (horizontal ? parent.layout.width : parent.layout.height) - boxInset(parent.style, horizontal ? 1 : 2) -
		        (horizontal ? childNode.layout.width : childNode.layout.height) - childNode.style.margin[horizontal ? 1 : 2]
		    : boxInset(parent.style, horizontal ? 3 : 0) + childNode.style.margin[horizontal ? 3 : 0];
	}
	const bool grid = isDisplayGrid(parent.style), flex = parent.style.display == kDisplayFlex;
	const bool row = flex ? flexRowDirection(parent.style) != (writingMode(parent) != 0) : usesRowLayout(parent.style);
	const int crossAlign = childNode.style.align_self >= 0 ? childNode.style.align_self : parent.style.align_items;
	const int inlineAlign = rstyle(childNode.style).justify_self >= 0 ? rstyle(childNode.style).justify_self : parent.style.justify_items;
	int align = grid
		? (horizontal == (writingMode(parent) == 0) ? inlineAlign : crossAlign)
		: (horizontal ? (row ? parent.style.justify_content : crossAlign)
		              : (row ? crossAlign : parent.style.justify_content));
	if (flex && horizontal == row && isDistributedAlignment(align)) {
		// Static position lays out the absolute box as the sole hypothetical
		// flex item: between falls back to flex-start, around/evenly to center.
		// This static center anchor remains centered even when it overflows.
		align = ((align & 15) == 3 ? 6 : 1) | kAlignUnsafe;
	}
	// A grid supplies its content box when the containing block is outside
	// it; when it is the containing block, grid placement supplies the area.
	int inset = grid || flex ? boxInset(parent.style, horizontal ? 3 : 0) : 0;
	int parentSize = (horizontal ? parent.layout.width : parent.layout.height) -
	                       (grid || flex ? boxInsets(parent.style, horizontal) : 0);
	int areaX, areaY, areaWidth, areaHeight;
	if (absoluteGridArea(parent, childNode, areaX, areaY, areaWidth, areaHeight)) {
		inset = horizontal ? areaX : areaY;
		parentSize = horizontal ? areaWidth : areaHeight;
	}
	const int childSize = horizontal ? childNode.layout.width : childNode.layout.height;
	const int beforeMargin = horizontal ? childNode.style.margin[3] : childNode.style.margin[0];
	const int afterMargin = horizontal ? childNode.style.margin[1] : childNode.style.margin[2];
	int available = parentSize;
	if (available < 0) available = 0;
	const int occupied = childSize + beforeMargin + afterMargin;
	int free = available - occupied;
	const bool baseline = (align & 15) == 5 || (align & 15) == kAlignLastBaseline;
	// Baseline static positions already use their specified fallback edge
	// without overflow safety; do not turn that fallback into safe alignment.
	const bool resolveInsets = containing && (grid || flex) && (align & 15) != 0 && !baseline;
	const int pointAlignment = resolveInsets ? (align & ~kAlignSafe) | kAlignUnsafe : align;
	if (!grid && !flex && !baseline && free < 0 && !(align & (kAlignSafe | kAlignUnsafe))) free = 0;
	const int self = physicalSelfAlignment(parent, childNode, pointAlignment, free, horizontal);
	int used = grid || flex ? usedGridAlignment(parent, childNode, pointAlignment, free, horizontal, true)
	    : self >= 0 ? self : baseline ? baselineFallbackAlignment(parent, childNode, pointAlignment, free, horizontal, true)
	                                  : usedAlignment(pointAlignment, free);
	const int keyword = align & 15;
	if (flex && horizontal == row && parent.style.flex_direction_explicit && parent.style.flex_direction >= 2 &&
	    (keyword == 0 || keyword == 2 || keyword == 6)) used = used == 2 ? 6 : 2;
	int position = inset + beforeMargin;
	if (used == 1) position += free / 2;
	else if (used == 2) position += free;
	if (resolveInsets) {
		// CSS Position 3.5.1 resolves the automatic inset(s) from the static
		// alignment point before overflow safety uses the available rectangle.
		const int containingEnd = containingStart + containingSize;
		int start = used == 2 ? containingStart : inset;
		int end = used == 2 ? inset + available : containingEnd;
		if (used == 1) {
			const int centerTwice = 2 * inset + available;
			const int startDistanceTwice = centerTwice - 2 * containingStart;
			const int endDistanceTwice = 2 * containingEnd - centerTwice;
			if (startDistanceTwice <= endDistanceTwice) {
				start = containingStart; end = start + std::abs(startDistanceTwice);
			} else {
				end = containingEnd; start = end - std::abs(endDistanceTwice);
			}
		}
		const bool reversed = gridAxisReversed(parent, horizontal);
		if (end < start) { if (reversed) start = end; else end = start; }
		const int remaining = end - start - occupied;
		if ((align & kAlignSafe) && remaining < 0) {
			position = reversed ? end - childSize - afterMargin : start + beforeMargin;
		} else if (flex && !(align & (kAlignSafe | kAlignUnsafe)) && remaining < 0) {
			const int limitStart = std::min(start, containingStart), limitEnd = std::max(end, containingEnd);
			if (occupied > limitEnd - limitStart)
				position = reversed ? limitEnd - childSize - afterMargin : limitStart + beforeMargin;
			else position = std::clamp(position, limitStart + beforeMargin, limitEnd - childSize - afterMargin);
		}
		// Unqualified grid self-alignment keeps the unsafe fallback used by browsers
		// and CSS Align's allowance for engines without smart overflow safety.
		// Applying a partial default-safety rule here changes ordinary grid
		// center/end alignment, even when no safe keyword was specified.
	}
	if (containing && flex && horizontal == row && keyword == 0 && !(align & (kAlignSafe | kAlignUnsafe))) {
		// The hypothetical single flex item can overflow its main-axis start
		// when that axis runs opposite to the actual containing block. Default
		// alignment keeps its start side reachable in the containing block.
		if (gridAxisReversed(*containing, horizontal))
			position = std::min(position, containingStart + containingSize - childSize - afterMargin);
		else position = std::max(position, containingStart + beforeMargin);
	}
	return position;
}

bool LayoutEngine::isInlineLevelNode(const Node &n)
{
	if (n.style.display == kDisplayFlex || n.style.display == kDisplayGrid || n.style.display == kDisplayNone)
		return false;
	// An explicit `display:block` makes even an inline-level tag (a <span>) — or a
	// Text/Image node — a BLOCK-LEVEL box, so it stacks vertically among siblings
	// instead of flowing into an inline row. Browsers do this; gea must too (e.g.
	// the weather city list's .city-name/.city-detail spans set display:block to
	// stack the name over the region). Without an explicit display, fall back to
	// the tag's intrinsic inline-ness.
	if (n.style.display == kDisplayBlock && n.style.display_explicit) return false;
	// In Gea's native layout model, vertical margins on inline-level wrappers are
	// used as an authoring signal that the wrapper occupies its own line. Keep
	// default spans inline, but do not merge title/score spans with margin-bottom
	// into one row.
	if (n.style.margin[0] != 0 || n.style.margin[2] != 0) return false;
	if (n.type == NodeType::Text || n.type == NodeType::Image) return true;
	return isInlineLevelTag(tagFromId(n.tag_id));
}

bool LayoutEngine::isCssInlineLevelBox(const Node &n, bool hypothetical)
{
	if (n.style.display == kDisplayNone || n.style.display_explicit) return false;
	if (!hypothetical && (n.style.float_side || isOutOfFlowPosition(n.style.position))) return false;
	if (!hypothetical && n.parent >= 0) {
		const Node &parent = Tree::instance().nodes()[n.parent];
		if (parent.style.display == kDisplayFlex || isDisplayGrid(parent.style)) return false;
	}
	if (n.type == NodeType::Text || n.type == NodeType::Image) return true;
	return n.type == NodeType::View && isInlineLevelTag(tagFromId(n.tag_id));
}

int LayoutEngine::clampSize(int size, int minSize, int maxSize) const
{
	if (minSize > 0 && size < minSize) size = minSize;
	if (maxSize != kUnset && size > maxSize) size = maxSize;
	return size;
}

int LayoutEngine::collectChildren(int parent, int *out, int max, bool skipAbs) const
{
	int n = 0;
	Node *nodes = Tree::instance().nodes();
	for (int child = nodes[parent].first_child; child >= 0; child = nodes[child].next_sibling) {
		if (isDisplayNone(nodes[child].style)) continue;
		if (suppressAnonymousWhitespace(nodes, parent, child)) continue;
		if (skipAbs && isOutOfFlowPosition(nodes[child].style.position)) continue;
		if (n < max) out[n++] = child;
	}
	// All consumers (layout, painting, and hit testing) start from this stable
	// order. Subsequent stable z-index/depth sorting preserves it for ties.
	// Absolutely positioned children have order 0 for painting and are excluded
	// by skipAbs when collecting actual flex/grid items for layout.
	if (nodes[parent].style.display == kDisplayFlex || isDisplayGrid(nodes[parent].style)) {
		auto order = [&](int child) { return isOutOfFlowPosition(nodes[child].style.position) ? 0 : nodes[child].style.order; };
		for (int i = 1; i < n; ++i) {
			const int child = out[i];
			int j = i;
			while (j > 0 && order(out[j - 1]) > order(child)) {
				out[j] = out[j - 1];
				--j;
			}
			out[j] = child;
		}
	}
	return n;
}

namespace {
// Intra-pass memo serial (see LayoutBox::memo_pass). 0 is never a valid pass
// so zero-initialized nodes can't produce a stale hit.
std::uint32_t gLayoutPassSerial = 1;
}  // namespace

void LayoutEngine::beginLayoutPass()
{
	if (++gLayoutPassSerial == 0) gLayoutPassSerial = 1;
}

void LayoutEngine::layoutNode(int id, int avail_w, int avail_h, bool intrinsicBoxEdges)
{
	Node &node = Tree::instance().nodes()[id];
	const Node *parent = node.parent >= 0 ? &Tree::instance().nodes()[node.parent] : nullptr;
	// Horizontal non-replaced floats use the same min/max-content measurement
	// and final used-width phase as fit-content. Floats on flex/grid items are
	// ignored; orthogonal automatic inline sizing is handled separately.
	const bool shrinkFloat = node.type == NodeType::View && node.style.float_side &&
	    !isOutOfFlowPosition(node.style.position) &&
	    (!parent || (parent->style.display != kDisplayFlex && !isDisplayGrid(parent->style))) &&
	    node.style.width == kUnset && node.style.width_percent == kUnset && node.style.width_expression == -1 &&
	    writingMode(node) == 0;
	// CSS Writing Modes 7.3.2: an orthogonal flow root's automatic inline
	// size is fit-content. Measure before fixing the used size so cyclic
	// percentage gaps are zero in the probes but resolve in final layout.
	// Flex and grid items obtain their automatic sizes from those algorithms.
	const bool horizontalInline = writingMode(node) == 0;
	const bool orthogonalAuto = node.type == NodeType::View && parent &&
	    horizontalInline != (writingMode(*parent) == 0) &&
	    parent->style.display != kDisplayFlex && !isDisplayGrid(parent->style) &&
	    !isOutOfFlowPosition(node.style.position) &&
	    (horizontalInline ? node.style.width : node.style.height) == kUnset &&
	    (horizontalInline ? node.style.width_percent : node.style.height_percent) == kUnset &&
	    (horizontalInline ? node.style.width_expression : node.style.height_expression) == -1;
	const bool intrinsicWidth = isIntrinsicSizeExpression(node.style.width_expression) && !intrinsicSizeScope(node, true);
	const bool horizontal = intrinsicWidth || !orthogonalAuto || horizontalInline;
	if ((intrinsicWidth || shrinkFloat || orthogonalAuto) &&
	    !intrinsicSizeScope(node, horizontal) && !flexBasisScope(node, horizontal)) {
		const int inherited = parent ? intrinsicSizeConstraint(*parent, horizontal) : 0;
		const int keyword = intrinsicWidth ? node.style.width_expression : kSizeFitContent;
		IntrinsicSizeScope scope(node, 1, horizontal);
		auto measure = [&](int mode) {
			scope.constraint = mode;
			const int available = mode == 1 ? 1 : 32767;
			layoutNode(id, horizontal ? available : avail_w, horizontal ? avail_h : available, true);
			return std::max(0, (horizontal ? node.layout.width : node.layout.height) - boxInsets(node.style, horizontal));
		};
		if (inherited) {
			measure(keyword == kSizeMinContent ? 1 : keyword == kSizeMaxContent ? 2 : inherited);
			return;
		}
		const int minimum = keyword == kSizeMaxContent ? 0 : measure(1);
		const int maximum = keyword == kSizeMinContent ? minimum : std::max(minimum, measure(2));
		const int inlineBasis = node.parent >= 0 && writingMode(Tree::instance().nodes()[node.parent]) != 0 ? avail_h : avail_w;
		resolveLayoutBoxLengths(id, intrinsicBoxEdges ? 0 : inlineBasis);
		const int edges = boxInsets(node.style, horizontal);
		const int stretch = std::max(0, (horizontal ? avail_w : avail_h) -
		    node.style.margin[horizontal ? 1 : 0] - node.style.margin[horizontal ? 3 : 2] - edges);
		scope.borderSize = edges + (keyword == kSizeMinContent ? minimum : keyword == kSizeMaxContent ? maximum : std::min(maximum, std::max(minimum, stretch)));
		scope.constraint = 0;
		layoutNode(id, avail_w, avail_h, intrinsicBoxEdges);
		return;
	}
	if (node.type == NodeType::Text && intrinsicWidthConstraint(node) == 1)
		avail_w = std::max(avail_w, TextRenderer::minContentWidth(node) + boxInsets(node.style, true));
	const int inlineBasis = node.parent >= 0 && writingMode(Tree::instance().nodes()[node.parent]) != 0 ? avail_h : avail_w;
	if (resolveLayoutBoxLengths(id, intrinsicBoxEdges ? 0 : inlineBasis)) {
		node.layout.memo_pass = node.layout.memo2_pass = 0;
	}
	const bool memoizable = !gIntrinsicSizeScope && !gFlexBasisScope && avail_w >= INT16_MIN && avail_w <= INT16_MAX &&
	                        avail_h >= INT16_MIN && avail_h <= INT16_MAX;
	if (memoizable) {
		if (node.layout.memo_pass == gLayoutPassSerial &&
		    node.layout.memo_avail_w == avail_w &&
		    node.layout.memo_avail_h == avail_h &&
		    node.layout.memo_result_w == node.layout.width &&
		    node.layout.memo_result_h == node.layout.height) {
			refreshPerfStatsMutable().treeLayoutMemoHits++;
			return;
		}
		if (node.layout.memo2_pass == gLayoutPassSerial &&
		    node.layout.memo2_avail_w == avail_w &&
		    node.layout.memo2_avail_h == avail_h &&
		    node.layout.memo2_result_w == node.layout.width &&
		    node.layout.memo2_result_h == node.layout.height) {
			// Promote the hit to the MRU slot.
			std::swap(node.layout.memo_avail_w, node.layout.memo2_avail_w);
			std::swap(node.layout.memo_avail_h, node.layout.memo2_avail_h);
			std::swap(node.layout.memo_result_w, node.layout.memo2_result_w);
			std::swap(node.layout.memo_result_h, node.layout.memo2_result_h);
			std::swap(node.layout.memo_pass, node.layout.memo2_pass);
			refreshPerfStatsMutable().treeLayoutMemoHits++;
			return;
		}
	}
	refreshPerfStatsMutable().treeLayoutNodeCalls++;
	LayoutNodePass(*this, id, avail_w, avail_h).run();
	const int beforeRatioWidth = node.layout.width, beforeRatioHeight = node.layout.height;
	applyPreferredRatio(node, avail_w, avail_h);
	if (node.first_child >= 0 && (node.layout.width != beforeRatioWidth || node.layout.height != beforeRatioHeight))
		LayoutNodePass(*this, id, node.layout.width, node.layout.height).repositionChildren();
	if (node.parent < 0) {
		// The initial containing block places the root; there is no parent
		// layout pass to position its margin box or apply float alignment.
		node.layout.x = node.style.float_side == 2
		    ? avail_w - node.layout.width - node.style.margin[1]
		    : node.style.margin[3];
		node.layout.y = node.style.margin[0];
	}
	node.layout.memo2_avail_w = node.layout.memo_avail_w;
	node.layout.memo2_avail_h = node.layout.memo_avail_h;
	node.layout.memo2_result_w = node.layout.memo_result_w;
	node.layout.memo2_result_h = node.layout.memo_result_h;
	node.layout.memo2_pass = node.layout.memo_pass;
	if (memoizable) {
		node.layout.memo_avail_w = static_cast<std::int16_t>(avail_w);
		node.layout.memo_avail_h = static_cast<std::int16_t>(avail_h);
		node.layout.memo_result_w = node.layout.width;
		node.layout.memo_result_h = node.layout.height;
		node.layout.memo_pass = gLayoutPassSerial;
	} else {
		node.layout.memo_pass = 0;
	}
}

void LayoutEngine::repositionChildren(int id)
{
	refreshPerfStatsMutable().treeLayoutRepositionCalls++;
	Node *node = &Tree::instance().nodes()[id];
	LayoutNodePass(*this, id, node->layout.width, node->layout.height).repositionChildren();
}

bool LayoutEngine::layoutNodeScoped(int scope, int treeRoot)
{
	Tree &tree = Tree::instance();
	Node *nodes = tree.nodes();
	const int nodeCount = tree.nodeCount();
	if (scope < 0 || scope >= nodeCount) return false;
	Node &a = nodes[scope];
	if (a.layout.memo_pass == 0) {
		refreshPerfStatsMutable().treeScopedRejectReason = 2;
		refreshPerfStatsMutable().treeScopedRejectNode = scope;
		return false;
	}
	if (!LayoutNodePass(*this, scope, a.layout.memo_avail_w, a.layout.memo_avail_h).containsChildMargins()) {
		refreshPerfStatsMutable().treeScopedRejectReason = 5;
		refreshPerfStatsMutable().treeScopedRejectNode = scope;
		return false;
	}

	auto inScope = [&](int node) {
		for (int c = node; c >= 0 && c < nodeCount; c = nodes[c].parent) {
			if (c == scope) return true;
		}
		return false;
	};

	// Absolute descendants positioned against a containing block OUTSIDE the
	// scope mix coordinate spaces (the global containing-block pass runs in
	// pre-resolve relative space) — those trees must take the full path.
	for (int i = 0; i < nodeCount; ++i) {
		const Node &n = nodes[i];
		if (n.parent < 0 || !isOutOfFlowPosition(n.style.position) || isDisplayNone(n.style)) continue;
		if (!inScope(i)) continue;
		const int containing = containingBlockForAbsoluteNode(i, treeRoot, nodes);
		if (containing >= 0 && !inScope(containing)) {
			refreshPerfStatsMutable().treeScopedRejectReason = 3;
			refreshPerfStatsMutable().treeScopedRejectNode = i;
			return false;
		}
	}

	const int prevX = a.layout.x;
	const int prevY = a.layout.y;
	const int prevW = a.layout.width;
	const int prevH = a.layout.height;
	beginLayoutPass();
	layoutNode(scope, a.layout.memo_avail_w, a.layout.memo_avail_h);
	const bool widthChanged = a.layout.width != prevW;
	const bool heightChanged = a.layout.height != prevH;
	if (widthChanged || heightChanged) {
		// layoutNode() derives an auto-sized scope from its content. Restore an
		// axis only when the parent genuinely owns that axis (stretch, grid,
		// flex-grow/basis) or the scope has an explicit size. Otherwise the new
		// intrinsic size must propagate through the parent: reject this scope so
		// refresh() retries at a stable ancestor. Treating every changed axis as
		// parent-owned kept a city chip at its placeholder width when "--" became
		// "13°", forcing flex-shrink to turn the shorter "Berlin" into "Ber…".
		bool parentOwnsWidth = hasExplicitWidth(a);
		bool parentOwnsHeight = hasExplicitHeight(a);
		if (a.parent >= 0 && a.parent < nodeCount) {
			const Node &parent = nodes[a.parent];
			if (isDisplayGrid(parent.style)) {
				const int inlineAlign = rstyle(a.style).justify_self >= 0 ? rstyle(a.style).justify_self : parent.style.justify_items;
				const int blockAlign = a.style.align_self >= 0 ? a.style.align_self : parent.style.align_items;
				const int alignX = writingMode(parent) == 0 ? inlineAlign : blockAlign;
				const int alignY = writingMode(parent) == 0 ? blockAlign : inlineAlign;
				parentOwnsWidth = parentOwnsWidth || (alignX == 0 && !(a.style.margin_auto & 10));
				parentOwnsHeight = parentOwnsHeight || (alignY == 0 && !(a.style.margin_auto & 5));
			} else {
				const bool row = usesRowLayout(parent.style);
				const bool parentOwnsMain = a.style.flex > 0 || hasFlexBasis(a);
				const int crossAlign = a.style.align_self >= 0 ? a.style.align_self : parent.style.align_items;
				const bool parentOwnsCross = crossAlign == 0;
				parentOwnsWidth = parentOwnsWidth || (row ? parentOwnsMain : parentOwnsCross);
				parentOwnsHeight = parentOwnsHeight || (row ? parentOwnsCross : parentOwnsMain);
			}
		}
		if ((widthChanged && !parentOwnsWidth) || (heightChanged && !parentOwnsHeight)) {
			refreshPerfStatsMutable().treeScopedRejectReason = 4;
			refreshPerfStatsMutable().treeScopedRejectNode = scope;
			return false;
		}

		// A parent-owned box cannot be reproduced by laying only the child's
		// subtree. Put those dimensions back and position its descendants inside
		// the same box exactly as the omitted parent pass would.
		a.layout.width = static_cast<std::int16_t>(prevW);
		a.layout.height = static_cast<std::int16_t>(prevH);
		repositionChildren(scope);
	}
	a.layout.x = prevX;
	a.layout.y = prevY;

	// Scoped equivalent of the root containing-block pass (both the node and
	// its containing block are inside the scope, in the same relative space).
	for (int i = 0; i < nodeCount; ++i) {
		if (isDisplayNone(nodes[i].style) || !isOutOfFlowPosition(nodes[i].style.position) || nodes[i].parent < 0) continue;
		if (!inScope(i) || i == scope) continue;
		const int containing = containingBlockForAbsoluteNode(i, scope, nodes);
		if (containing < 0) continue;
		positionAbsoluteNodeInContainingBlock(i, containing, nodes);
	}

	int childParentX = a.layout.x;
	int childParentY = a.layout.y;
	if (a.style.overflow == 2) {
		if (scrollsOverflowX(a.style)) childParentX -= a.layout.scroll_x;
		if (scrollsOverflowY(a.style)) childParentY -= a.layout.scroll_y;
	}
	for (int child = a.first_child; child >= 0; child = nodes[child].next_sibling) {
		resolveAbsoluteCoords(child, childParentX, childParentY);
	}
	return true;
}

void LayoutEngine::resolveAbsoluteCoords(int id, int parent_x, int parent_y)
{
	Node *nodes = Tree::instance().nodes();
	Node *node = &nodes[id];
	if (node->parent < 0) resolveAbsoluteContainingBlocks(id);
	if (!isViewportFixed(*node)) {
		node->layout.x += parent_x;
		node->layout.y += parent_y;
	}

	int childParentX = node->layout.x;
	int childParentY = node->layout.y;
	if (node->style.overflow == 2) {
		if (scrollsOverflowX(node->style)) childParentX -= node->layout.scroll_x;
		if (scrollsOverflowY(node->style)) childParentY -= node->layout.scroll_y;
	}

	for (int child = node->first_child; child >= 0; child = nodes[child].next_sibling) {
		resolveAbsoluteCoords(child, childParentX, childParentY);
	}
}

}  // namespace gea::embedded::ui
