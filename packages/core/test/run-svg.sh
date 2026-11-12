#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/svg"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"

# The tree is built through the C++ Document API; no app is compiled.
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
printf '%s\n' "$BUILD_DIR/program.cpp" > "$BUILD_DIR/geatsc-sources.txt"

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/svg" \
  "$ROOT/packages/core/test/test_svg_main.cpp"

"$BUILD_DIR/svg"
