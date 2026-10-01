// SPDX-License-Identifier: Apache-2.0
#pragma once
// Positive whole-program node reachability facts. Older or opaque builds
// retain every native node kind; applications do not opt into compact storage.
#ifndef GEA_UI_IMAGE_NODES
#define GEA_UI_IMAGE_NODES 1
#endif
#ifndef GEA_UI_INPUT_NODES
#define GEA_UI_INPUT_NODES 1
#endif

// Generated from the whole-source class-token bound. Unknown programs retain
// four inline entries. Overflow support is independent of this capacity.
#ifndef GEA_UI_CLASS_INLINE_TOKENS
#define GEA_UI_CLASS_INLINE_TOKENS 4
#endif
static_assert(GEA_UI_CLASS_INLINE_TOKENS >= 1 && GEA_UI_CLASS_INLINE_TOKENS <= 4,
              "Class inline capacity must be between one and four tokens");

// A separate positive proof covers every class assignment and rules out opaque
// mutation. Older analyzers and unknown programs retain overflow support.
#ifndef GEA_UI_CLASS_OVERFLOW
#define GEA_UI_CLASS_OVERFLOW 1
#endif

// Closed whole-source auxiliary-state proof. Unknown/older programs keep all
// owners; callers cannot prune these through application manifest switches.
#ifndef GEA_UI_NODE_LISTENERS
#define GEA_UI_NODE_LISTENERS 1
#endif
#ifndef GEA_UI_NODE_ATTRIBUTES
#define GEA_UI_NODE_ATTRIBUTES 1
#endif
#ifndef GEA_UI_DEFAULT_STYLES
#define GEA_UI_DEFAULT_STYLES 1
#endif
