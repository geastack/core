#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# Its own build directory: tests run in parallel, and a shared one has them
# overwrite each other's program.cpp and object cache mid-build.
BUILD_DIR="$ROOT/packages/core/test/.build/canvas-overlay"
mkdir -p "$BUILD_DIR"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
export TMPDIR="$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/canvas-overlay" \
  "$ROOT/packages/core/test/test_canvas_overlay_main.cpp" \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_RADIUS_MAX=1 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_MAX=1 \
  -DGEA_EMBEDDED_CANVAS_CIRCLE_BOX_SPAN_SLOTS=1
"$BUILD_DIR/canvas-overlay"
