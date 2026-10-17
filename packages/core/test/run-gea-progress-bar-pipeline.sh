#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/gea-progress-bar-pipeline"
CXX_BIN="${CXX:-clang++}"

source "$ROOT/packages/core/test/native-test-common.sh"
gea_require_app_project

mkdir -p "$BUILD_DIR"

node "$ROOT/packages/core/scripts/build-gea-vite-geatsc.mjs" \
  --app-dir "$GEA_APP_PROJECT/apps/progress-bar" \
  --entry "index.tsx" \
  --out-dir "$BUILD_DIR" \
  --gea-embedded-compat \
  --gea-ir-backend \
  --pixel-panel-endian 1 \
  --allow-any

gea_build_native_test \
  "$BUILD_DIR" \
  "$BUILD_DIR/gea-progress-bar-test" \
  "$ROOT/packages/core/test/test_gea_progress_bar_main.cpp"

"$BUILD_DIR/gea-progress-bar-test"
