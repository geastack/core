#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/detached-background-placement"

mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"

cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP

# The bug this guards was an out-of-bounds read whose effect depended on the
# address layout, so only a sanitizer makes it fail every run. The shared
# builder takes one compiler for compiling and linking; this wrapper gives
# both the sanitizer flags, and its own path keeps these objects apart from
# the plain ones in the object cache.
CXX_BIN="$BUILD_DIR/sanitized-cxx"
printf '#!/bin/sh\nexec "%s" -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer "$@"\n' \
  "${CXX:-clang++}" > "$CXX_BIN"
chmod +x "$CXX_BIN"
# A native Windows ccache cannot execute a shell script as its compiler; there
# bash runs the wrapper itself.
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) export GEA_NATIVE_CCACHE=0 ;; esac

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/detached-background-placement-test" \
  "$ROOT/packages/core/test/test_detached_background_placement_main.cpp"

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
  "$BUILD_DIR/detached-background-placement-test" "$@"
