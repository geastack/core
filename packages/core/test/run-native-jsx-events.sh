#!/usr/bin/env bash
set -euo pipefail
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_plugin_build
BUILD_DIR="$ROOT/packages/core/test/.build/native-jsx-events"
CXX_BIN="${CXX:-clang++}"
: "${GEA_COMPILER_CLI:?Set GEA_COMPILER_CLI to the actual shared compiler dist/cli.js}"
node "$GEA_COMPILER_CLI" compile "$ROOT/packages/core/test/native-jsx-events.tsx" --out-dir "$BUILD_DIR" --project "$ROOT/packages/core/test/tsconfig-native-jsx-events.json" --plugin "$ROOT/packages/geatsc-plugin-gea/dist/index.js"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-jsx-events" "$ROOT/packages/core/test/test_native_jsx_events.cpp"
"$BUILD_DIR/native-jsx-events"
