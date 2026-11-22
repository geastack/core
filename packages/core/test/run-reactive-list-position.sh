#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# Its own build directory: tests run in parallel, and a shared one has them
# overwrite each other's program.cpp and object cache mid-build.
BUILD_DIR="$ROOT/packages/core/test/.build/reactive-list-position"
mkdir -p "$BUILD_DIR"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-1}"
export TMPDIR="$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_compiler_runtime
RUNTIME_DIR="$GEA_COMPILER_RUNTIME"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/reactive-list-position" \
  "$ROOT/packages/core/test/test_reactive_list_position_main.cpp" \
  "-I$RUNTIME_DIR"
"$BUILD_DIR/reactive-list-position"
