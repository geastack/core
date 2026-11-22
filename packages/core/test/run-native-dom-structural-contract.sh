#!/usr/bin/env bash
set -euo pipefail
if [[ "$#" -gt 1 || ( "$#" == 1 && "$1" != "--build-only" && "$1" != "--prepare-only" ) ]]; then echo "usage: $0 [--build-only|--prepare-only]" >&2; exit 2; fi
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
gea_require_plugin_build
BUILD_DIR="$ROOT/dist/native-dom-structural-contract"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
if [[ $# == 0 || "$1" == --prepare-only || ! -f "$BUILD_DIR/contract-provenance.json" ]];then
 node "$ROOT/packages/core/test/native-contract-pipeline.mjs" native-dom-structural prepare
fi
node "$ROOT/packages/core/test/native-contract-pipeline.mjs" native-dom-structural check
if [[ $# == 1 && "$1" == --prepare-only ]];then exit 0;fi
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-dom-structural-contract-test" "$ROOT/packages/core/test/test_native_dom_structural_contract.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-dom-structural-contract-test.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/native-dom-structural-contract-test.cxx-flags.txt"
printf '%s\n' "${GEA_NATIVE_LINK_FLAGS[@]}" > "$BUILD_DIR/native-dom-structural-contract-test.link-flags.txt"
if [[ "$#" == 0 ]]; then "$BUILD_DIR/native-dom-structural-contract-test"; fi
