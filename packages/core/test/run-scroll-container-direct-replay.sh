#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/scroll-container-direct-replay"
CXX_BIN="${CXX:-clang++}"
mkdir -p "$BUILD_DIR"
export TMPDIR="$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/scroll-container-direct-replay" \
  "$ROOT/packages/core/test/test_scroll_container_direct_replay_main.cpp"
"$BUILD_DIR/scroll-container-direct-replay"
