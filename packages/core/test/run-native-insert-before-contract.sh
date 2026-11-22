#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && ( "$1" == --build-only || "$1" == --prepare-only ) ) ]] || { echo "usage: $0 [--build-only|--prepare-only]" >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_plugin_build
BUILD_DIR="$ROOT/dist/native-insert-before-contract"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-1}"
if [[ $# == 0 || "$1" == --prepare-only || ! -f "$BUILD_DIR/contract-provenance.json" ]]; then
 node "$ROOT/packages/core/test/native-contract-pipeline.mjs" native-insert-before prepare
fi
node "$ROOT/packages/core/test/native-contract-pipeline.mjs" native-insert-before check
if [[ $# == 1 && "$1" == --prepare-only ]];then exit 0;fi
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-insert-before-contract" "$ROOT/packages/core/test/test_native_insert_before_contract.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-insert-before-contract.objects.txt"
if [[ $# == 0 ]];then "$BUILD_DIR/native-insert-before-contract";fi
