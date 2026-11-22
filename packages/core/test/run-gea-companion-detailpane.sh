#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
APPLE_ROOT="${GEA_APPLE_ROOT:-}"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_require_apple_root
gea_require_macos
BUILD_DIR="$APPLE_ROOT/targets/macos/generated/gea-companion"
OUTPUT_DIR="$ROOT/packages/core/test/.build/gea-companion-detailpane"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$OUTPUT_DIR"

gea_build_native_test \
  "$BUILD_DIR" \
  "$OUTPUT_DIR/gea-companion-detailpane-test" \
  "$ROOT/packages/core/test/test_gea_companion_detailpane_main.cpp" \
  "$APPLE_ROOT/targets/macos/main/macos_host_device_control.mm" \
  -framework Foundation

"$OUTPUT_DIR/gea-companion-detailpane-test"
