#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# An engine-only stub program: it needs no compiler pipeline output, so it
# owns its build directory instead of borrowing one another test left behind.
BUILD_DIR="${GEA_NATIVE_TEST_BUILD_DIR:-$ROOT/packages/core/test/.build/bitmap-font-kerning}"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"

export TMPDIR="$BUILD_DIR"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/bitmap-font-kerning" \
  "$ROOT/packages/core/test/test_bitmap_font_kerning_main.cpp"
"$BUILD_DIR/bitmap-font-kerning"
