#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && "$1" == --build-only ) ]] || { echo 'Use --build-only or no arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_plugin_build
BUILD_DIR="$ROOT/dist/native-input-contract"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
if [[ $# == 0 ]]; then
  : "${GEA_COMPILER_CLI:?Set GEA_COMPILER_CLI to the actual shared compiler dist/cli.js}"
  rm -f "$BUILD_DIR/program.cpp"
  node "$GEA_COMPILER_CLI" compile "$ROOT/packages/core/test/native-input-contract.ts" --out-dir "$BUILD_DIR" --project "$ROOT/packages/core/test/tsconfig-native-input.json" --plugin "$ROOT/packages/geatsc-plugin-gea/dist/index.js"
fi
[[ -s "$BUILD_DIR/geatsc-sources.txt" ]] || { echo 'Actual normal CLI source manifest required' >&2;exit 2; }
if [[ -f "$BUILD_DIR/program.cpp" && "$BUILD_DIR/program.cpp" -nt "$BUILD_DIR/geatsc-sources.txt" ]]; then
  echo 'Refuse newer fallback program.cpp' >&2;exit 2
fi
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-input-contract-test" "$ROOT/packages/core/test/test_native_input_contract.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-input-contract-test.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/native-input-contract-test.cxx-flags.txt"
printf '%s\n' "${GEA_NATIVE_LINK_FLAGS[@]}" > "$BUILD_DIR/native-input-contract-test.link-flags.txt"
if [[ $# == 0 ]]; then
  "$BUILD_DIR/native-input-contract-test"
  : "${GEA_JSDOM_PACKAGE:?Set GEA_JSDOM_PACKAGE to the installed jsdom package directory}"
  node "$ROOT/packages/core/test/native-input-oracle.mjs"
fi
