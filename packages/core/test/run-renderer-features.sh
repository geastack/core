#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
mkdir -p "$BUILD_DIR"
export TMPDIR="$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP

gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/renderer-features-enabled" \
  "$ROOT/packages/core/test/test_renderer_features_main.cpp"
enabled="$($BUILD_DIR/renderer-features-enabled "$@")"
# Full CSS gradients remain covered, without falsely claiming that disabled
# semantic fields must accept authored declarations.
"$BUILD_DIR/renderer-features-enabled" --css-storage > "$BUILD_DIR/renderer-features-css-enabled.log"

# Current CSS-only radius-8 proof. Exercise hits at the bounds and the same
# native commands above them; shrinking caches must never remove drawing.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/renderer-features-bounded" \
  "$ROOT/packages/core/test/test_renderer_features_main.cpp" \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_RADIUS_MAX=8 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_MAX=19 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_SPAN_MAX=16 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_SLOTS=4
bounded="$($BUILD_DIR/renderer-features-bounded "$@")"
if [[ "$enabled" != "$bounded" ]]; then
  diff -u <(printf '%s\n' "$enabled") <(printf '%s\n' "$bounded")
  exit 1
fi

# Deliberately invalid sizes must be irrelevant when their instructions are
# absent. This is automatic elimination, not an app selecting tiny cache sizes.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/renderer-features-pruned" \
  "$ROOT/packages/core/test/test_renderer_features_main.cpp" \
  -DGEA_EMBEDDED_RENDERER_CIRCLES=0 \
  -DGEA_EMBEDDED_RENDERER_TRANSFORMS=0 \
  -DGEA_EMBEDDED_RENDERER_LINEAR_GRADIENTS=0 \
  -DGEA_EMBEDDED_RENDERER_RADIAL_GRADIENTS=0 \
  -DGEA_EMBEDDED_RENDERER_TRIANGLE_OCCLUSION=0 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_RADIUS_MAX=0 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_SPAN_MAX=0 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_SLOTS=0 \
  -DGEA_EMBEDDED_UI_TRANSFORM_CACHE_SLOTS=0 \
  -DGEA_EMBEDDED_UI_DEPTH_CACHE_SLOTS=0 \
  -DGEA_EMBEDDED_UI_CORNER_CACHE_SLOTS=0 \
  -DGEA_EMBEDDED_PROJECTED_TEXT_CACHE_BANKS=0 \
  -DGEA_EMBEDDED_GRADIENT_LUT_SLOTS=0 \
  -DGEA_EMBEDDED_TRANSFORMED_GRADIENT_LUT_SLOTS=0
pruned="$($BUILD_DIR/renderer-features-pruned "$@")"
if [[ "$enabled" != "$pruned" ]]; then
  diff -u <(printf '%s\n' "$enabled") <(printf '%s\n' "$pruned")
  exit 1
fi

symbols='CanvasMath::instance.*::math|cachedNodeTransform.*::cache|averageDepth.*::cache|transformCorners.*::(cache|prevCache)|GradientDrawer::replay.*::(lutSlots|lutStorage|lutKey|cache|transCache|bgKey|bgLastKey|rowColor|rowAlpha|rowPermille)|slotsByBank|slotRRByBank|slotConstAlphaByBank|drawProjectedText.*::cacheBanks|pieFgCore|pieA5Core|s_occlBits'
if nm -C "$BUILD_DIR/renderer-features-pruned" | grep -E "$symbols"; then
  echo 'FAIL: pruned renderer retained cache storage symbols' >&2
  exit 1
fi
if ! nm -C "$BUILD_DIR/renderer-features-enabled" | grep 's_occlBits' > /dev/null; then
  echo 'FAIL: enabled renderer did not retain triangle occlusion scratch' >&2
  exit 1
fi
if ! nm -C "$BUILD_DIR/renderer-features-enabled" | grep -E "$symbols" > /dev/null; then
  echo 'FAIL: enabled renderer did not exercise cache storage' >&2
  exit 1
fi
printf '%s\n' "$pruned"
echo 'PASS: full/bounded/uncached pixels and geometry match; pruned cache symbols absent'
