#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && ( "$1" == --build-only || "$1" == --run ) ) ]] || { echo 'Use --build-only, --run or no arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_plugin_build
BUILD_DIR="$ROOT/dist/native-button-contract"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
# Root owns the real CLI generation into this normal output directory. This
# runner consumes its actual manifest; it never substitutes a top-level stub.
if [[ $# == 0 ]]; then
  : "${GEA_COMPILER_CLI:?Set GEA_COMPILER_CLI to the actual shared compiler dist/cli.js}"
  rm -f "$BUILD_DIR/program.cpp"
  node "$GEA_COMPILER_CLI" compile "$ROOT/packages/core/test/native-button-contract.ts" --out-dir "$BUILD_DIR" --project "$ROOT/packages/core/test/tsconfig-native-button.json" --plugin "$ROOT/packages/geatsc-plugin-gea/dist/index.js"
fi
[[ -s "$BUILD_DIR/geatsc-sources.txt" ]] || { echo 'Actual CLI geatsc-sources.txt required' >&2;exit 2; }
if [[ -f "$BUILD_DIR/program.cpp" && "$BUILD_DIR/program.cpp" -nt "$BUILD_DIR/geatsc-sources.txt" ]];then
  echo 'Refuse helper fallback to program.cpp newer than actual CLI source manifest' >&2;exit 2
fi
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-button-contract-test" "$ROOT/packages/core/test/test_native_button_contract.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-button-contract-test.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/native-button-contract-test.cxx-flags.txt"
printf '%s\n' "${GEA_NATIVE_LINK_FLAGS[@]}" > "$BUILD_DIR/native-button-contract-test.link-flags.txt"
if [[ $# == 0 || "$1" == --run ]];then "$BUILD_DIR/native-button-contract-test";fi
