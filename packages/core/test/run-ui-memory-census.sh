#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
export TMPDIR="$BUILD_DIR"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
: "${GEA_CENSUS_REFERENCE_ENGINE:?Set the reference engine package path}"
: "${GEA_CENSUS_CANDIDATE_ENGINE:?Set the candidate engine package path}"
common=(-DGEA_EMBEDDED_MAX_NODES=160)
pruned=(-DGEA_UI_IMAGE_NODES=0 -DGEA_UI_INPUT_NODES=0 -DGEA_EMBEDDED_RENDERER_LINEAR_GRADIENTS=0 -DGEA_EMBEDDED_RENDERER_RADIAL_GRADIENTS=0)
# This fixture contains none of the optional families. Retain the default full
# engine when building the reference package, which predates these build facts.
for feature in TEXT_ALPHA BORDER_ALPHA ANIMATIONS TRANSFORMS GRID FLOATS WRITING_MODE BORDER_RELIEF BLINK ORDER PERCENT_RADIUS PERCENT_GAP OPACITY TEXT_DECORATION TEXT_TRANSFORM VISIBILITY POINTER_EVENTS MASK IMAGE_FIT FILTERS BOX_SHADOW FLEX_WRAP JUSTIFY_ITEMS ALIGN_CONTENT ALIGN_SELF MIN_WIDTH HEIGHT_EXPRESSIONS Z_INDEX ASPECT_RATIO MARGIN_TRIM CONTAINMENT JUSTIFY_SELF FLEX_LINE_COUNT BOX_EXPRESSIONS AXIS_GAP CORNER_RADIUS FIRST_LINE SIDE_BORDERS BACKGROUND_LAYERS LINE_HEIGHT_EXPRESSIONS SCROLLING FLEX_BASIS_EXPRESSIONS CUSTOM_PROPERTY_LENGTHS MAX_HEIGHT FLEX_BASIS OVERFLOW_AXES; do
  pruned+=("-DGEA_CSS_${feature}=0")
done
for family in PADDING GAP BORDER RADIUS FONT LINE_HEIGHT FLEX; do
  pruned+=("-DGEA_CSS_U8_${family}=1")
done
GEA_NATIVE_TEST_ELEMENTS_ROOT="${GEA_CENSUS_REFERENCE_ELEMENTS:-$ROOT/packages/elements}" GEA_NATIVE_TEST_ENGINE_ROOT="$GEA_CENSUS_REFERENCE_ENGINE" gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/ui-memory-reference" "$ROOT/packages/core/test/test_ui_memory_census_main.cpp" "${common[@]}"
GEA_NATIVE_TEST_ENGINE_ROOT="$GEA_CENSUS_CANDIDATE_ENGINE" gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/ui-memory-candidate" "$ROOT/packages/core/test/test_ui_memory_census_main.cpp" "${common[@]}" "${pruned[@]}"
"$BUILD_DIR/ui-memory-reference" > "$BUILD_DIR/ui-memory-reference.csv"
"$BUILD_DIR/ui-memory-candidate" > "$BUILD_DIR/ui-memory-candidate.csv"
"$BUILD_DIR/ui-memory-reference" --rounded > "$BUILD_DIR/ui-memory-reference-rounded.csv"
"$BUILD_DIR/ui-memory-candidate" --rounded > "$BUILD_DIR/ui-memory-candidate-rounded.csv"
python3 - "$BUILD_DIR" <<'PYTHON'
import csv, pathlib, sys
root = pathlib.Path(sys.argv[1])
print('fixture,phase,reference_allocated,candidate_allocated,saved_bytes,reference_blocks,candidate_blocks')
for suffix in ['', '-rounded']:
    before = list(csv.reader((root / f'ui-memory-reference{suffix}.csv').open()))
    after = list(csv.reader((root / f'ui-memory-candidate{suffix}.csv').open()))
    assert len(before) == len(after) == 6
    for a, b in zip(before, after):
        assert a[0] == b[0] and a[4] == b[4], ('pixel mismatch', suffix, a, b)
        print(f'{suffix or "plain"},{a[0]},{a[1]},{b[1]},{int(a[1])-int(b[1])},{a[2]},{b[2]}')
PYTHON

# Include the complete ownership/allocation cost of custom-property maps. This
# exposed a sharing design that saved copies but regressed independent writes.
GEA_NATIVE_TEST_ELEMENTS_ROOT="${GEA_CENSUS_REFERENCE_ELEMENTS:-$ROOT/packages/elements}" GEA_NATIVE_TEST_ENGINE_ROOT="$GEA_CENSUS_REFERENCE_ENGINE" gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/custom-property-reference" "$ROOT/packages/core/test/test_custom_property_storage_main.cpp" "${common[@]}"
GEA_NATIVE_TEST_ENGINE_ROOT="$GEA_CENSUS_CANDIDATE_ENGINE" gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/custom-property-candidate" "$ROOT/packages/core/test/test_custom_property_storage_main.cpp" "${common[@]}" "${pruned[@]}"
"$BUILD_DIR/custom-property-reference" > "$BUILD_DIR/custom-property-reference.csv"
"$BUILD_DIR/custom-property-candidate" > "$BUILD_DIR/custom-property-candidate.csv"
python3 - "$BUILD_DIR" <<'PYTHON'
import csv, pathlib, sys
root = pathlib.Path(sys.argv[1])
before = list(csv.reader((root / 'custom-property-reference.csv').open()))
after = list(csv.reader((root / 'custom-property-candidate.csv').open()))
assert len(before) == len(after) == 5
for a, b in zip(before, after):
    assert a[0] == b[0] and a[3] == b[3], ('variable mismatch', a, b)
    print(f'custom-properties,{a[0]},{a[1]},{b[1]},{int(a[1])-int(b[1])},{a[2]},{b[2]}')
PYTHON

# The display harness is RGB565. Exercise the store independently for both
# full-color formats, including the high/sign bit of each cached value.
for format in 1 2; do
  "$CXX_BIN" -std=c++20 -O2 -ffunction-sections -fdata-sections "$link_gc" \
    -DGEA_STORE_TEST_ONLY=1 -DGEA_EMBEDDED_PIXEL_FORMAT="$format" \
    -I"$ROOT/packages/core/include" -I"$GEA_CENSUS_CANDIDATE_ENGINE" -I"$ROOT/packages/host/include" \
    "$ROOT/packages/core/test/test_custom_property_storage_main.cpp" \
    "$GEA_CENSUS_CANDIDATE_ENGINE/ui/tree_state.cpp" "$GEA_CENSUS_CANDIDATE_ENGINE/ui/css_atom.cpp" \
    -o "$BUILD_DIR/custom-property-color-$format"
  "$BUILD_DIR/custom-property-color-$format" > "$BUILD_DIR/custom-property-color-$format.csv"
done
