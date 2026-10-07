#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
source "$ROOT/packages/core/test/native-test-common.sh"
mkdir -p "$BUILD_DIR"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/debugger-picker" \
  "$ROOT/packages/core/test/test_debugger_picker_main.cpp" -DGEA_NATIVE_DEBUGGER=1
"$BUILD_DIR/debugger-picker"
