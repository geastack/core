#!/usr/bin/env bash
set -euo pipefail
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_plugin_build
BUILD_DIR="$ROOT/packages/core/test/.build/native-window-contract"
CXX_BIN="${CXX:-clang++}"
: "${GEA_COMPILER_CLI:?Set GEA_COMPILER_CLI to the actual shared compiler dist/cli.js}"
node "$GEA_COMPILER_CLI" compile "$ROOT/packages/core/test/native-window-contract.ts" --out-dir "$BUILD_DIR" --project "$ROOT/packages/core/test/tsconfig-native-window.json" --plugin "$ROOT/packages/geatsc-plugin-gea/dist/index.js"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-window-contract" "$ROOT/packages/core/test/test_native_window_contract.cpp"
"$BUILD_DIR/native-window-contract"
