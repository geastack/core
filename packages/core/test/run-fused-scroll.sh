#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-1}"
mkdir -p "$BUILD_DIR"
export TMPDIR="$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/fused-scroll" \
  "$ROOT/packages/core/test/test_fused_scroll_main.cpp" \
  -DGEA_EMBEDDED_DISPLAY_FUSE_REPLAY_FLUSH=1
"$BUILD_DIR/fused-scroll"
