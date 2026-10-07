#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${GEA_NATIVE_TEST_BUILD_DIR:-$ROOT/packages/core/test/.build/wrapped-text-box-overflow}"
CXX_BIN="${CXX:-clang++}"

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Use an existing native test build directory with GEA_NATIVE_TEST_BUILD_DIR" >&2
  exit 1
fi

export TMPDIR="$BUILD_DIR"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/bitmap-font-kerning" \
  "$ROOT/packages/core/test/test_bitmap_font_kerning_main.cpp"
"$BUILD_DIR/bitmap-font-kerning"
