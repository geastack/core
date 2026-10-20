#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/native-selectors"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
BINARY="$BUILD_DIR/native-selectors$GEA_HOST_EXE_SUFFIX"
gea_build_native_test "$BUILD_DIR" "$BINARY" "$ROOT/packages/core/test/test_native_selectors.cpp"
"$BINARY"
: "${GEA_JSDOM_PACKAGE:?Set GEA_JSDOM_PACKAGE to the installed jsdom package directory}"
node "$ROOT/packages/core/test/native-selectors-dom-oracle.mjs" "$(gea_native_path "$BINARY")"
