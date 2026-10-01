#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
export TMPDIR="$BUILD_DIR" GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
class_overflow=1
class_capacity=4
if [[ "${1:-}" == "--compact-classes" ]]; then
  class_overflow=0
  class_capacity=1
fi
base_style_flags=("-DGEA_EMBEDDED_UI_STATE_DYNAMIC_INIT=${GEA_BALLS_STATE_DYNAMIC_INIT:-0}" "-DGEA_CSS_CUSTOM_PROPERTIES=${GEA_BALLS_CUSTOM_PROPERTIES:-1}" "-DGEA_CSS_WIDTH_PERCENT=${GEA_BALLS_WIDTH_PERCENT:-1}" "-DGEA_CSS_HEIGHT_PERCENT=${GEA_BALLS_HEIGHT_PERCENT:-1}")
base_style_enabled=1
if [[ "${GEA_BALLS_BASE_STYLE_PRUNED:-0}" == 1 ]]; then base_style_enabled=0; fi
for feature in FLEX_DIRECTION JUSTIFY_CONTENT ALIGN_ITEMS BOX_SIZING MARGIN_AUTO LINE_HEIGHT_MULTIPLIER WIDTH_EXPRESSIONS MIN_HEIGHT MAX_WIDTH ACTIVE_BACKGROUND MARGINS PADDING FLEX_FACTORS GAP BORDER_WIDTHS BORDER_COLORS FONT_WEIGHT TEXT_ALIGN WHITE_SPACE TEXT_OVERFLOW; do
  base_style_flags+=("-DGEA_CSS_${feature}=$base_style_enabled")
done
if [[ "${GEA_BALLS_NARROW_STYLE:-0}" == 1 ]]; then
  base_style_flags+=("-DGEA_CSS_U8_RADIUS=1" "-DGEA_CSS_CORNER_RADIUS=0" "-DGEA_CSS_U8_LINE_HEIGHT=1")
fi
if [[ "${GEA_BALLS_COMPACT_STORAGE:-0}" == 1 ]]; then
  base_style_flags+=("-DGEA_UI_NODE_LISTENERS=0" "-DGEA_UI_NODE_ATTRIBUTES=0" "-DGEA_UI_DEFAULT_STYLES=0" "-DGEA_CSS_LINE_HEIGHT=0" "-DGEA_CSS_DISPLAY_EXPLICIT=0" "-DGEA_CSS_POSITION_TOP_PERCENT=0" "-DGEA_CSS_POSITION_LEFT_PERCENT=0")
fi
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/bouncing-balls-regressions" \
  "$ROOT/packages/core/test/test_bouncing_balls_regressions_main.cpp" \
  -DGEA_EMBEDDED_SHARED_STYLES="${GEA_BALLS_SHARED_STYLES:-0}" -DGEA_EMBEDDED_PERF="${GEA_BALLS_PERF:-1}" \
  "${base_style_flags[@]}" -DGEA_UI_CLASS_OVERFLOW="$class_overflow" -DGEA_UI_CLASS_INLINE_TOKENS="$class_capacity" \
  -DGEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS=8 -DGEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS=64
"$BUILD_DIR/bouncing-balls-regressions" "${GEA_BALLS_TEST_CASE:-all}"

if [[ "$class_overflow" == 0 ]]; then
  python3 - "$BUILD_DIR/bouncing-balls-regressions" <<'PYTEST'
import resource, signal, subprocess, sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
result = subprocess.run([sys.argv[1], 'classes-overflow'], capture_output=True)
assert result.returncode == -signal.SIGABRT, result
print('PASS: violated class bound aborts instead of silently dropping a token')
PYTEST
fi

if [[ "${1:-}" == "--mutations" ]]; then
  # Mutate copies in normal ignored build output, never the working tree.
  # Compile/link errors are failures here too: only the named behavioral
  # assertion proves that the regression was caught.
  definition="$(declare -f gea_native_sources)"
  eval "${definition/gea_native_sources/gea_original_native_sources}"
  gea_native_sources() {
    gea_original_native_sources
    for i in "${!GEA_NATIVE_SRCS[@]}"; do
      if [[ "${GEA_NATIVE_SRCS[$i]}" == "$ROOT/packages/engine/ui/$changed_source" ]]; then
        GEA_NATIVE_SRCS[$i]="$BUILD_DIR/bouncing-balls-mutant.cpp"
      fi
    done
  }
  for mutation in minima integer leaf split epochs memo; do
    [[ -z "${2:-}" || "$mutation" == "$2" ]] || continue
    case "$mutation" in
      minima) changed_source=absolute_leaf_refresh.cpp; selected=retained; expected='moving fixed-size leaves must use retained absolute layout' ;;
      memo) changed_source=layout.cpp; selected=memo; expected='fresh layout scratch must not reuse a prior zero-size result' ;;
      epochs) changed_source=style.cpp; selected=class-epochs; expected='batched class deduplication and descendant invalidation must survive epoch reuse' ;;
      integer) changed_source=style.cpp; selected=integer; expected='complete small signed integers must use the fast parser' ;;
      leaf) changed_source=render.cpp; selected=retained; expected='leaf motion must not scan' ;;
      split) changed_source=render.cpp; selected=replay; expected='already-clipped DMA replay must not submit' ;;
    esac
    python3 - "$ROOT/packages/engine/ui/$changed_source" "$BUILD_DIR/bouncing-balls-mutant.cpp" "$mutation" <<'PYTEST'
import pathlib,sys
source, output, mutation = sys.argv[1:]
s = pathlib.Path(source).read_text()
if mutation == 'minima':
    old = '(node.computedStyle().min_width != kUnset && node.computedStyle().min_width != 0) ||\n\t    (node.computedStyle().min_height != kUnset && node.computedStyle().min_height != 0)'
    new = 'node.computedStyle().min_width != 0 || node.computedStyle().min_height != 0'
elif mutation == 'memo':
    old = 'if (memo.valid1 &&'
    new = 'if (persistent.memo_pass &&'
elif mutation == 'epochs':
    old = '\t\tif (serial == 0) {\n\t\t\tfor (std::uint8_t &mark : marks) mark = 0;\n'
    new = '\t\tif (serial == 0) {\n'
elif mutation == 'integer':
    old = 'if (applyIntegerPosition(node, declaration, rawValue, source)) return;'
    new = '// regression: always parse via generic CSS'
elif mutation == 'leaf':
    old = 'if (Tree::instance().nodes()[node].first_child < 0) return;'
    new = '// regression: scan descendant clips even for a leaf'
else:
    old = 'DisplayCommandReplayer::replaySimpleClippedDirtyRegion(x0, y0, x1, y1, origin, /*allowSplit=*/false);'
    new = old.replace('/*allowSplit=*/false', '/*allowSplit=*/true')
assert s.count(old) == 1, (mutation, s.count(old))
pathlib.Path(output).write_text(s.replace(old, new))
PYTEST
    gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/bouncing-balls-mutant" \
      "$ROOT/packages/core/test/test_bouncing_balls_regressions_main.cpp" \
      -DGEA_EMBEDDED_SHARED_STYLES=0 -DGEA_EMBEDDED_PERF=1 \
      -DGEA_EMBEDDED_RENDER_PARALLEL_MIN_ROWS=8 -DGEA_EMBEDDED_RENDER_PARALLEL_MIN_PIXELS=64
    if "$BUILD_DIR/bouncing-balls-mutant" "$selected" > "$BUILD_DIR/bouncing-balls-mutant.log" 2>&1; then
      echo "FAIL: old $mutation regression passed" >&2; exit 1
    fi
    if ! grep -q "$expected" "$BUILD_DIR/bouncing-balls-mutant.log"; then
      cat "$BUILD_DIR/bouncing-balls-mutant.log" >&2; exit 1
    fi
    echo "PASS: old $mutation regression rejected by its behavioral assertion"
  done
fi
