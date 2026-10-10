#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# Its own build directory: tests run in parallel, and a shared one has them
# overwrite each other's program.cpp and object cache mid-build.
BUILD_DIR="$ROOT/packages/core/test/.build/css-feature-elimination"
mkdir -p "$BUILD_DIR"
CXX_BIN="${CXX:-clang++}"
export TMPDIR="$BUILD_DIR"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/css-features-full" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp"
"$BUILD_DIR/css-features-full" > "$BUILD_DIR/css-features-full.txt"
compact_flags=(-DGEA_CSS_U8_PADDING=1 -DGEA_CSS_U8_GAP=1 -DGEA_CSS_U8_BORDER=1 -DGEA_CSS_U8_RADIUS=1 -DGEA_CSS_U8_FONT=1 -DGEA_CSS_U8_LINE_HEIGHT=1 -DGEA_CSS_U8_FLEX=1)
pruned_flags=(-DGEA_CSS_PSEUDO_ELEMENTS=0 -DGEA_CSS_TEXT_ALPHA=0 -DGEA_CSS_BORDER_ALPHA=0 -DGEA_CSS_ANIMATIONS=0 -DGEA_CSS_TRANSFORMS=0 -DGEA_CSS_GRID=0 -DGEA_CSS_FLOATS=0 -DGEA_CSS_WRITING_MODE=0 -DGEA_CSS_BORDER_RELIEF=0 -DGEA_CSS_BLINK=0 -DGEA_CSS_ORDER=0 -DGEA_CSS_PERCENT_RADIUS=0 -DGEA_CSS_PERCENT_GAP=0 -DGEA_CSS_OPACITY=0 -DGEA_CSS_TEXT_DECORATION=0 -DGEA_CSS_TEXT_TRANSFORM=0 -DGEA_CSS_VISIBILITY=0 -DGEA_CSS_POINTER_EVENTS=0 -DGEA_CSS_MASK=0 -DGEA_CSS_IMAGE_FIT=0 -DGEA_CSS_FILTERS=0 -DGEA_CSS_BOX_SHADOW=0 -DGEA_CSS_FLEX_WRAP=0 -DGEA_CSS_JUSTIFY_ITEMS=0 -DGEA_CSS_ALIGN_CONTENT=0 -DGEA_CSS_ALIGN_SELF=0 -DGEA_CSS_MIN_WIDTH=0 -DGEA_CSS_HEIGHT_EXPRESSIONS=0 -DGEA_CSS_Z_INDEX=0 -DGEA_CSS_ASPECT_RATIO=0 -DGEA_CSS_MARGIN_TRIM=0 -DGEA_CSS_CONTAINMENT=0 -DGEA_CSS_JUSTIFY_SELF=0 -DGEA_CSS_FLEX_LINE_COUNT=0 -DGEA_CSS_BOX_EXPRESSIONS=0 -DGEA_CSS_AXIS_GAP=0 -DGEA_CSS_CORNER_RADIUS=0 -DGEA_CSS_FIRST_LINE=0 -DGEA_CSS_SIDE_BORDERS=0 -DGEA_CSS_BACKGROUND_LAYERS=0 -DGEA_CSS_LINE_HEIGHT_EXPRESSIONS=0 -DGEA_CSS_SCROLLING=0 -DGEA_CSS_FLEX_BASIS_EXPRESSIONS=0 -DGEA_CSS_CUSTOM_PROPERTY_LENGTHS=0 -DGEA_CSS_MAX_HEIGHT=0 -DGEA_CSS_FLEX_BASIS=0 -DGEA_CSS_OVERFLOW_AXES=0 -DGEA_EMBEDDED_RENDERER_LINEAR_GRADIENTS=0 -DGEA_EMBEDDED_RENDERER_RADIAL_GRADIENTS=0 "${compact_flags[@]}" -DGEA_UI_IMAGE_NODES=0 -DGEA_UI_INPUT_NODES=0)
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/css-features-pruned" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" "${pruned_flags[@]}"
"$BUILD_DIR/css-features-pruned" > "$BUILD_DIR/css-features-pruned.txt"
diff -u "$BUILD_DIR/css-features-full.txt" "$BUILD_DIR/css-features-pruned.txt"
echo 'PASS: CSS elimination preserves flat layout, border-box sizing and rendered pixels across updates'
for mode in full pruned; do
  flags=()
  if [[ "$mode" == pruned ]]; then flags=("${pruned_flags[@]}"); fi
  gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/repaint-scratch-$mode" "$ROOT/packages/core/test/test_repaint_scratch_main.cpp" ${flags[@]+"${flags[@]}"}
  "$BUILD_DIR/repaint-scratch-$mode" > "$BUILD_DIR/repaint-scratch-$mode.txt"
done
diff -u "$BUILD_DIR/repaint-scratch-full.txt" "$BUILD_DIR/repaint-scratch-pruned.txt"
echo 'PASS: coalesced repaint shortcuts match full replay and presented pixels in both layouts'


# These families must be independent. A previous guard accidentally made RTL
# depend on float support; testing only all-on/all-off cannot detect that.
for writing in 0 1; do
  floats=$((1 - writing))
  binary="$BUILD_DIR/css-writing-${writing}-floats-${floats}"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" -DGEA_CSS_WRITING_MODE="$writing" -DGEA_CSS_FLOATS="$floats"
  "$binary" > "$binary.txt"
  diff -u "$BUILD_DIR/css-features-full.txt" "$binary.txt"
done
echo 'PASS: writing modes and floats operate independently'

# Grid storage and deferred edge lengths are independent of the other rare
# sizing properties. Retained-property assertions above still run in this mix.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/css-no-grid-or-box-expressions" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" -DGEA_CSS_GRID=0 -DGEA_CSS_BOX_EXPRESSIONS=0 -DGEA_CSS_Z_INDEX=0
"$BUILD_DIR/css-no-grid-or-box-expressions" > "$BUILD_DIR/css-no-grid-or-box-expressions.txt"
diff -u "$BUILD_DIR/css-features-full.txt" "$BUILD_DIR/css-no-grid-or-box-expressions.txt"
echo 'PASS: rare sizing remains independent of grid and box-expression storage'

# Scalar/independent corner radii and pixel/percentage gaps must not become
# coupled. Each run compares the same update fingerprints to the full engine.
for corners in 0 1; do
  axes=$((1 - corners))
  binary="$BUILD_DIR/css-corners-${corners}-axes-${axes}"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" -DGEA_CSS_CORNER_RADIUS="$corners" -DGEA_CSS_AXIS_GAP="$axes"
  "$binary" > "$binary.txt"
  diff -u "$BUILD_DIR/css-features-full.txt" "$binary.txt"
done
echo 'PASS: uniform corners and gap axes preserve independent percentage support'

# First-line metadata is independent of ordinary inline layout and floats.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/css-no-first-line" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" -DGEA_CSS_FIRST_LINE=0
"$BUILD_DIR/css-no-first-line" > "$BUILD_DIR/css-no-first-line.txt"
diff -u "$BUILD_DIR/css-features-full.txt" "$BUILD_DIR/css-no-first-line.txt"
echo 'PASS: first-line metadata prunes independently of other CSS families'

# Scroll metadata pruning must not require the other optional families to be
# absent: overflow clipping and ordinary layout still match the full engine.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/css-no-scrolling" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" -DGEA_CSS_SCROLLING=0
"$BUILD_DIR/css-no-scrolling" > "$BUILD_DIR/css-no-scrolling.txt"
diff -u "$BUILD_DIR/css-features-full.txt" "$BUILD_DIR/css-no-scrolling.txt"
echo 'PASS: unused scroll metadata prunes independently while clipping matches'

# Deferred flex bases and custom-property length caches are independent: either
# can disappear while every other rare style and length operation remains.
for feature in FLEX_BASIS_EXPRESSIONS CUSTOM_PROPERTY_LENGTHS MAX_HEIGHT FLEX_BASIS OVERFLOW_AXES; do
  binary="$BUILD_DIR/css-no-${feature}"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" "-DGEA_CSS_${feature}=0"
  "$binary" > "$binary.txt"
  diff -u "$BUILD_DIR/css-features-full.txt" "$binary.txt"
done
echo 'PASS: deferred bases and custom-property length caches prune independently'

# Byte-sized common fields also coexist with retained rare/expression families.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/css-byte-fields-full" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" "${compact_flags[@]}"
"$BUILD_DIR/css-byte-fields-full" > "$BUILD_DIR/css-byte-fields-full.txt"
diff -u "$BUILD_DIR/css-features-full.txt" "$BUILD_DIR/css-byte-fields-full.txt"
echo 'PASS: bounded byte fields preserve scaling, inheritance and full-feature pixels'

# Native payload removal does not require CSS feature removal or each other.
for feature in IMAGE INPUT; do
  binary="$BUILD_DIR/css-no-native-${feature}"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_css_feature_elimination_main.cpp" "-DGEA_UI_${feature}_NODES=0"
  "$binary" > "$binary.txt"
  diff -u "$BUILD_DIR/css-features-full.txt" "$binary.txt"
done
echo 'PASS: native image/input payloads prune independently of CSS and each other'

bash "$ROOT/packages/core/test/run-position-storage.sh"

# No-float builds still use the mixed inline/block formatting path.
for floats in 1 0; do
  binary="$BUILD_DIR/css-float-walks-$floats"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_no_float_layout_main.cpp" "-DGEA_CSS_FLOATS=$floats"
  "$binary" > "$binary.txt"
done
diff -u "$BUILD_DIR/css-float-walks-1.txt" "$BUILD_DIR/css-float-walks-0.txt"
echo 'PASS: 18 nested mixed-flow frames and height clamps survive float-walk pruning'

# Generated inline capacities must not truncate or alias overflow/copy state.
for capacity in 1 2 3 4; do
  binary="$BUILD_DIR/class-storage-$capacity"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_class_storage_main.cpp" "-DGEA_UI_CLASS_INLINE_TOKENS=$capacity"
  "$binary"
done

# Padding reuse must preserve adjacent state across copies, moves and spills.
for mode in full pruned; do
  flags=()
  if [[ "$mode" == pruned ]]; then flags=("${pruned_flags[@]}"); fi
  binary="$BUILD_DIR/record-storage-$mode"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_record_storage_main.cpp" ${flags[@]+"${flags[@]}"}
  "$binary" > "$binary.txt"
done
diff -u "$BUILD_DIR/record-storage-full.txt" "$BUILD_DIR/record-storage-pruned.txt"
echo 'PASS: embedded record padding preserves copy/move and selector state'

# CSS motion elimination must not remove requestAnimationFrame scheduling.
for mode in full pruned; do
  flags=()
  if [[ "$mode" == pruned ]]; then flags=(-DGEA_CSS_ANIMATIONS=0); fi
  gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/animation-elimination-$mode" "$ROOT/packages/core/test/test_animation_elimination_main.cpp" ${flags[@]+"${flags[@]}"}
  "$BUILD_DIR/animation-elimination-$mode" > "$BUILD_DIR/animation-elimination-$mode.txt"
done
diff -u "$BUILD_DIR/animation-elimination-full.txt" "$BUILD_DIR/animation-elimination-pruned.txt"

# The generated-node, first-line and animation buckets are independently
# removable; keep positive behavior covered under each enum remapping.
for mode in pseudo first-line animation; do
  case "$mode" in
    pseudo) test_main=test_css_block_and_flexbasis_main.cpp; flags=(-DGEA_CSS_FIRST_LINE=0 -DGEA_CSS_ANIMATIONS=0) ;;
    first-line) test_main=test_css_first_line_background_main.cpp; flags=(-DGEA_CSS_PSEUDO_ELEMENTS=0 -DGEA_CSS_ANIMATIONS=0) ;;
    animation) test_main=test_css_animation_priming.cpp; flags=(-DGEA_CSS_PSEUDO_ELEMENTS=0 -DGEA_CSS_FIRST_LINE=0) ;;
  esac
  binary="$BUILD_DIR/independent-buckets-$mode"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/$test_main" ${flags[@]+"${flags[@]}"}
  "$binary"
done

# Compact snapshots must preserve all inherited changes, including the
# class-token and custom-property spill paths. The display harness is RGB565;
# snapshots store the custom-property color type, whose full-color formats
# run-ui-memory-census.sh exercises store-only.
for mode in full pruned; do
  flags=()
  if [[ "$mode" == pruned ]]; then flags=("${pruned_flags[@]}" -DGEA_UI_CLASS_INLINE_TOKENS=2); fi
  binary="$BUILD_DIR/style-snapshots-$mode"
  gea_build_native_test "$BUILD_DIR" "$binary" "$ROOT/packages/core/test/test_style_snapshot_storage_main.cpp" ${flags[@]+"${flags[@]}"}
  "$binary"
done
