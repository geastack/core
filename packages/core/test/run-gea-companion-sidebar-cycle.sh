#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
APPLE_ROOT="${GEA_APPLE_ROOT:-}"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_require_apple_root
gea_require_macos
BUILD_DIR="$APPLE_ROOT/targets/macos/generated/gea-companion"
OUTPUT_DIR="$ROOT/packages/core/test/.build/gea-companion-sidebar-cycle"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$OUTPUT_DIR"

gea_build_native_test \
  "$BUILD_DIR" \
  "$OUTPUT_DIR/gea-companion-sidebar-cycle-test" \
  "$ROOT/packages/core/test/test_gea_companion_sidebar_cycle_main.cpp" \
  "$ROOT/packages/core/test/fake_companion_device_control.cpp" \
  -framework Foundation

"$OUTPUT_DIR/gea-companion-sidebar-cycle-test"
