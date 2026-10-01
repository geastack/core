// SPDX-License-Identifier: Apache-2.0
#pragma once

// Generated from the app's source/style analysis by the build driver. These
// describe reachable instructions, not app configuration switches. Builds made
// without an analyzer retain the complete renderer. Cache sizing only matters
// inside a feature's enabled branch; the disabled branches use no cache storage.
#ifndef GEA_EMBEDDED_RENDERER_CIRCLES
#define GEA_EMBEDDED_RENDERER_CIRCLES 1
#endif
#ifndef GEA_EMBEDDED_RENDERER_TRANSFORMS
#define GEA_EMBEDDED_RENDERER_TRANSFORMS 1
#endif
#ifndef GEA_EMBEDDED_RENDERER_LINEAR_GRADIENTS
#define GEA_EMBEDDED_RENDERER_LINEAR_GRADIENTS 1
#endif
#ifndef GEA_EMBEDDED_RENDERER_RADIAL_GRADIENTS
#define GEA_EMBEDDED_RENDERER_RADIAL_GRADIENTS 1
#endif

// Independent proof: old analyzers cannot certify triangle batches absent.
#ifndef GEA_EMBEDDED_RENDERER_TRIANGLE_OCCLUSION
#define GEA_EMBEDDED_RENDERER_TRIANGLE_OCCLUSION 1
#endif
