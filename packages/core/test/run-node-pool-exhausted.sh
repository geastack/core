#!/usr/bin/env bash
set -euo pipefail

# A full node pool must refuse new nodes and report it once: callers drop the
# -1, so without the message a view silently disappears. Built with a small
# pool so the test fills it quickly.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/node-pool-exhausted"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"

cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/node-pool-exhausted" \
  "$ROOT/packages/core/test/test_node_pool_exhausted_main.cpp" \
  -DGEA_EMBEDDED_MAX_NODES=32

output="$("$BUILD_DIR/node-pool-exhausted")"
reports="$(grep -c '^\[tree\] node pool full (32 nodes, GEA_EMBEDDED_MAX_NODES)' <<<"$output" || true)"
if [[ "$reports" != "1" ]]; then
  echo "FAIL: expected one node-pool-full report, got $reports" >&2
  printf '%s\n' "$output" >&2
  exit 1
fi
