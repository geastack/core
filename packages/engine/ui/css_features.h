// SPDX-License-Identifier: Apache-2.0
#pragma once

// Custom-property containers and dependency caches need whole-source proof.
#ifndef GEA_CSS_CUSTOM_PROPERTIES
#define GEA_CSS_CUSTOM_PROPERTIES 1
#endif

// v17 default-field reachability facts; only a whole-source proof disables them.
#ifndef GEA_CSS_MARGINS
#define GEA_CSS_MARGINS 1
#endif
#ifndef GEA_CSS_PADDING
#define GEA_CSS_PADDING 1
#endif
#ifndef GEA_CSS_FLEX_FACTORS
#define GEA_CSS_FLEX_FACTORS 1
#endif
#ifndef GEA_CSS_GAP
#define GEA_CSS_GAP 1
#endif
#ifndef GEA_CSS_BORDER_WIDTHS
#define GEA_CSS_BORDER_WIDTHS 1
#endif
#ifndef GEA_CSS_BORDER_COLORS
#define GEA_CSS_BORDER_COLORS 1
#endif
#ifndef GEA_CSS_FONT_WEIGHT
#define GEA_CSS_FONT_WEIGHT 1
#endif
#ifndef GEA_CSS_TEXT_ALIGN
#define GEA_CSS_TEXT_ALIGN 1
#endif
#ifndef GEA_CSS_WHITE_SPACE
#define GEA_CSS_WHITE_SPACE 1
#endif
#ifndef GEA_CSS_TEXT_OVERFLOW
#define GEA_CSS_TEXT_OVERFLOW 1
#endif

// v16 common-style reachability facts. Unknown builds keep native defaults.
#ifndef GEA_CSS_FLEX_DIRECTION
#define GEA_CSS_FLEX_DIRECTION 1
#endif
#ifndef GEA_CSS_JUSTIFY_CONTENT
#define GEA_CSS_JUSTIFY_CONTENT 1
#endif
#ifndef GEA_CSS_ALIGN_ITEMS
#define GEA_CSS_ALIGN_ITEMS 1
#endif
#ifndef GEA_CSS_BOX_SIZING
#define GEA_CSS_BOX_SIZING 1
#endif
#ifndef GEA_CSS_MARGIN_AUTO
#define GEA_CSS_MARGIN_AUTO 1
#endif
#ifndef GEA_CSS_LINE_HEIGHT_MULTIPLIER
#define GEA_CSS_LINE_HEIGHT_MULTIPLIER 1
#endif
// Automatic whole-source percentage reachability. Older builds retain fields.
#ifndef GEA_CSS_WIDTH_PERCENT
#define GEA_CSS_WIDTH_PERCENT 1
#endif
#ifndef GEA_CSS_HEIGHT_PERCENT
#define GEA_CSS_HEIGHT_PERCENT 1
#endif

#ifndef GEA_CSS_WIDTH_EXPRESSIONS
#define GEA_CSS_WIDTH_EXPRESSIONS 1
#endif
#ifndef GEA_CSS_MIN_HEIGHT
#define GEA_CSS_MIN_HEIGHT 1
#endif
#ifndef GEA_CSS_MAX_WIDTH
#define GEA_CSS_MAX_WIDTH 1
#endif
#ifndef GEA_CSS_ACTIVE_BACKGROUND
#define GEA_CSS_ACTIVE_BACKGROUND 1
#endif


#ifndef GEA_CSS_PSEUDO_ELEMENTS
#define GEA_CSS_PSEUDO_ELEMENTS 1
#endif

// Opaque/default colour proof, generated from the complete source graph.
#ifndef GEA_CSS_TEXT_ALPHA
#define GEA_CSS_TEXT_ALPHA 1
#endif
#ifndef GEA_CSS_BORDER_ALPHA
#define GEA_CSS_BORDER_ALPHA 1
#endif

// Whole-source animation reachability; older/opaque builds retain support.
#ifndef GEA_CSS_ANIMATIONS
#define GEA_CSS_ANIMATIONS 1
#endif

// Build-generated reachability facts. The application does not configure these.
// A driver without CSS analysis retains every feature. All translation units
// must receive the same facts, because they specialize per-node storage too.
#ifndef GEA_CSS_TRANSFORMS
#define GEA_CSS_TRANSFORMS 1
#endif
#ifndef GEA_CSS_GRID
#define GEA_CSS_GRID 1
#endif
#ifndef GEA_CSS_FLOATS
#define GEA_CSS_FLOATS 1
#endif
#ifndef GEA_CSS_WRITING_MODE
#define GEA_CSS_WRITING_MODE 1
#endif
#ifndef GEA_CSS_PERCENT_RADIUS
#define GEA_CSS_PERCENT_RADIUS 1
#endif
#ifndef GEA_CSS_PERCENT_GAP
#define GEA_CSS_PERCENT_GAP 1
#endif
#ifndef GEA_CSS_BLINK
#define GEA_CSS_BLINK 1
#endif
#ifndef GEA_CSS_ORDER
#define GEA_CSS_ORDER 1
#endif
#ifndef GEA_CSS_BORDER_RELIEF
#define GEA_CSS_BORDER_RELIEF 1
#endif

#ifndef GEA_CSS_OPACITY
#define GEA_CSS_OPACITY 1
#endif

#ifndef GEA_CSS_TEXT_DECORATION
#define GEA_CSS_TEXT_DECORATION 1
#endif

#ifndef GEA_CSS_TEXT_TRANSFORM
#define GEA_CSS_TEXT_TRANSFORM 1
#endif

#ifndef GEA_CSS_VISIBILITY
#define GEA_CSS_VISIBILITY 1
#endif

#ifndef GEA_CSS_POINTER_EVENTS
#define GEA_CSS_POINTER_EVENTS 1
#endif

#ifndef GEA_CSS_MASK
#define GEA_CSS_MASK 1
#endif

#ifndef GEA_CSS_IMAGE_FIT
#define GEA_CSS_IMAGE_FIT 1
#endif

#ifndef GEA_CSS_FILTERS
#define GEA_CSS_FILTERS 1
#endif

#ifndef GEA_CSS_BOX_SHADOW
#define GEA_CSS_BOX_SHADOW 1
#endif

#ifndef GEA_CSS_FLEX_WRAP
#define GEA_CSS_FLEX_WRAP 1
#endif

#ifndef GEA_CSS_JUSTIFY_ITEMS
#define GEA_CSS_JUSTIFY_ITEMS 1
#endif

#ifndef GEA_CSS_ALIGN_CONTENT
#define GEA_CSS_ALIGN_CONTENT 1
#endif

#ifndef GEA_CSS_ALIGN_SELF
#define GEA_CSS_ALIGN_SELF 1
#endif

#ifndef GEA_CSS_MIN_WIDTH
#define GEA_CSS_MIN_WIDTH 1
#endif

#ifndef GEA_CSS_HEIGHT_EXPRESSIONS
#define GEA_CSS_HEIGHT_EXPRESSIONS 1
#endif

#ifndef GEA_CSS_Z_INDEX
#define GEA_CSS_Z_INDEX 1
#endif

#ifndef GEA_CSS_ASPECT_RATIO
#define GEA_CSS_ASPECT_RATIO 1
#endif

#ifndef GEA_CSS_MARGIN_TRIM
#define GEA_CSS_MARGIN_TRIM 1
#endif

#ifndef GEA_CSS_CONTAINMENT
#define GEA_CSS_CONTAINMENT 1
#endif

#ifndef GEA_CSS_JUSTIFY_SELF
#define GEA_CSS_JUSTIFY_SELF 1
#endif

#ifndef GEA_CSS_FLEX_LINE_COUNT
#define GEA_CSS_FLEX_LINE_COUNT 1
#endif

#ifndef GEA_CSS_BOX_EXPRESSIONS
#define GEA_CSS_BOX_EXPRESSIONS 1
#endif


#ifndef GEA_CSS_AXIS_GAP
#define GEA_CSS_AXIS_GAP 1
#endif

#ifndef GEA_CSS_CORNER_RADIUS
#define GEA_CSS_CORNER_RADIUS 1
#endif

// Uniform-corner applications retain a real array with a single element.
// Every index becomes the constant zero before C++ compilation: no accessor,
// branch, decoding or additional load. Arguments must be side-effect free.
#if GEA_CSS_CORNER_RADIUS
#define GEA_CSS_RADIUS_COUNT 4
#define GEA_CSS_RADIUS_INDEX(index) (index)
#else
#define GEA_CSS_RADIUS_COUNT 1
#define GEA_CSS_RADIUS_INDEX(index) 0
#endif

// Shaped overflow clips: a rounded or transformed `overflow: hidden` box clips
// its content to the padding box's inner radii, or to its screen parallelogram.
// Off, every overflow clip is a rectangle and a transformed box does not clip,
// and replay carries none of the save/restore machinery.
#ifndef GEA_CSS_SHAPED_CLIPS
#define GEA_CSS_SHAPED_CLIPS 1
#endif

#ifndef GEA_CSS_FIRST_LINE
#define GEA_CSS_FIRST_LINE 1
#endif

#ifndef GEA_CSS_SIDE_BORDERS
#define GEA_CSS_SIDE_BORDERS 1
#endif

#ifndef GEA_CSS_BACKGROUND_LAYERS
#define GEA_CSS_BACKGROUND_LAYERS 1
#endif

// CSS Overflow 4 line clamping (line-clamp, max-lines, continue, block-ellipsis)
// and CSS Multi-column (column-count, column-width, column-fill, column-span).
#ifndef GEA_CSS_LINE_CLAMP
#define GEA_CSS_LINE_CLAMP 1
#endif
#ifndef GEA_CSS_MULTICOL
#define GEA_CSS_MULTICOL 1
#endif

#ifndef GEA_CSS_LINE_HEIGHT_EXPRESSIONS
#define GEA_CSS_LINE_HEIGHT_EXPRESSIONS 1
#endif

#ifndef GEA_CSS_SCROLLING
#define GEA_CSS_SCROLLING 1
#endif

#ifndef GEA_CSS_FLEX_BASIS_EXPRESSIONS
#define GEA_CSS_FLEX_BASIS_EXPRESSIONS 1
#endif
#ifndef GEA_CSS_CUSTOM_PROPERTY_LENGTHS
#define GEA_CSS_CUSTOM_PROPERTY_LENGTHS 1
#endif

#ifndef GEA_CSS_MAX_HEIGHT
#define GEA_CSS_MAX_HEIGHT 1
#endif
#ifndef GEA_CSS_FLEX_BASIS
#define GEA_CSS_FLEX_BASIS 1
#endif
#ifndef GEA_CSS_OVERFLOW_AXES
#define GEA_CSS_OVERFLOW_AXES 1
#endif

// Positive range proofs supplied by the build; unknown inputs retain 16 bits.
#ifndef GEA_CSS_U8_PADDING
#define GEA_CSS_U8_PADDING 0
#endif
#ifndef GEA_CSS_U8_GAP
#define GEA_CSS_U8_GAP 0
#endif
#ifndef GEA_CSS_U8_BORDER
#define GEA_CSS_U8_BORDER 0
#endif
#ifndef GEA_CSS_U8_RADIUS
#define GEA_CSS_U8_RADIUS 0
#endif
#ifndef GEA_CSS_U8_FONT
#define GEA_CSS_U8_FONT 0
#endif
#ifndef GEA_CSS_U8_LINE_HEIGHT
#define GEA_CSS_U8_LINE_HEIGHT 0
#endif
#ifndef GEA_CSS_U8_FLEX
#define GEA_CSS_U8_FLEX 0
#endif

#define GEA_CSS_U8_BORDER_FLAGS (!GEA_CSS_SIDE_BORDERS && (GEA_CSS_U8_PADDING || GEA_CSS_U8_GAP || GEA_CSS_U8_BORDER || GEA_CSS_U8_RADIUS || GEA_CSS_U8_FONT || GEA_CSS_U8_LINE_HEIGHT || GEA_CSS_U8_FLEX))

// Per-edge reachability. Opaque/older builds keep the original four-edge arrays.
#ifndef GEA_CSS_POSITION_TOP
#define GEA_CSS_POSITION_TOP 1
#endif
#ifndef GEA_CSS_POSITION_TOP_PERCENT
#define GEA_CSS_POSITION_TOP_PERCENT 1
#endif
#ifndef GEA_CSS_POSITION_RIGHT
#define GEA_CSS_POSITION_RIGHT 1
#endif
#ifndef GEA_CSS_POSITION_RIGHT_PERCENT
#define GEA_CSS_POSITION_RIGHT_PERCENT 1
#endif
#ifndef GEA_CSS_POSITION_BOTTOM
#define GEA_CSS_POSITION_BOTTOM 1
#endif
#ifndef GEA_CSS_POSITION_BOTTOM_PERCENT
#define GEA_CSS_POSITION_BOTTOM_PERCENT 1
#endif
#ifndef GEA_CSS_POSITION_LEFT
#define GEA_CSS_POSITION_LEFT 1
#endif
#ifndef GEA_CSS_POSITION_LEFT_PERCENT
#define GEA_CSS_POSITION_LEFT_PERCENT 1
#endif

#define GEA_CSS_POSITION_PX_ALL (GEA_CSS_POSITION_TOP && GEA_CSS_POSITION_RIGHT && GEA_CSS_POSITION_BOTTOM && GEA_CSS_POSITION_LEFT)
#if GEA_CSS_POSITION_PX_ALL
#define GEA_CSS_POSITION_PX_0(style) ((style).pos_offsets[0])
#define GEA_CSS_POSITION_PX_1(style) ((style).pos_offsets[1])
#define GEA_CSS_POSITION_PX_2(style) ((style).pos_offsets[2])
#define GEA_CSS_POSITION_PX_3(style) ((style).pos_offsets[3])
#else
#define GEA_CSS_POSITION_PX_0(style) ((style).pos_top)
#define GEA_CSS_POSITION_PX_1(style) ((style).pos_right)
#define GEA_CSS_POSITION_PX_2(style) ((style).pos_bottom)
#define GEA_CSS_POSITION_PX_3(style) ((style).pos_left)
#endif
// The selector must be a compile-time constant: no runtime edge mapping.
#define GEA_CSS_POSITION_PX(style, side) \
	(std::integral_constant<int, (side)>::value == 0 ? GEA_CSS_POSITION_PX_0(style) : \
	 std::integral_constant<int, (side)>::value == 1 ? GEA_CSS_POSITION_PX_1(style) : \
	 std::integral_constant<int, (side)>::value == 2 ? GEA_CSS_POSITION_PX_2(style) : GEA_CSS_POSITION_PX_3(style))

#define GEA_CSS_POSITION_PERCENT_ALL (GEA_CSS_POSITION_TOP_PERCENT && GEA_CSS_POSITION_RIGHT_PERCENT && GEA_CSS_POSITION_BOTTOM_PERCENT && GEA_CSS_POSITION_LEFT_PERCENT)
#if GEA_CSS_POSITION_PERCENT_ALL
#define GEA_CSS_POSITION_PERCENT_0(style) ((style).pos_offset_percent[0])
#define GEA_CSS_POSITION_PERCENT_1(style) ((style).pos_offset_percent[1])
#define GEA_CSS_POSITION_PERCENT_2(style) ((style).pos_offset_percent[2])
#define GEA_CSS_POSITION_PERCENT_3(style) ((style).pos_offset_percent[3])
#else
#define GEA_CSS_POSITION_PERCENT_0(style) ((style).pos_percent_top)
#define GEA_CSS_POSITION_PERCENT_1(style) ((style).pos_percent_right)
#define GEA_CSS_POSITION_PERCENT_2(style) ((style).pos_percent_bottom)
#define GEA_CSS_POSITION_PERCENT_3(style) ((style).pos_percent_left)
#endif
// The selector must be a compile-time constant: no runtime edge mapping.
#define GEA_CSS_POSITION_PERCENT(style, side) \
	(std::integral_constant<int, (side)>::value == 0 ? GEA_CSS_POSITION_PERCENT_0(style) : \
	 std::integral_constant<int, (side)>::value == 1 ? GEA_CSS_POSITION_PERCENT_1(style) : \
	 std::integral_constant<int, (side)>::value == 2 ? GEA_CSS_POSITION_PERCENT_2(style) : GEA_CSS_POSITION_PERCENT_3(style))

// Optional default-valued fields: only a complete compact-style source proof
// can omit them. Layout uses these constants directly in a pruned application.
#ifndef GEA_CSS_LINE_HEIGHT
#define GEA_CSS_LINE_HEIGHT 1
#endif
#ifndef GEA_CSS_DISPLAY_EXPLICIT
#define GEA_CSS_DISPLAY_EXPLICIT 1
#endif
