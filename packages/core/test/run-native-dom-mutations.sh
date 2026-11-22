#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && "$1" == --build-only ) ]] || { echo 'Use --build-only or no arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
BUILD_DIR="$ROOT/dist/native-dom-mutations"
mkdir -p "$BUILD_DIR"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
source "$ROOT/packages/core/test/native-test-common.sh"
# Engine API regression only; no compiler emission is claimed.
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-dom-mutations-test" "$ROOT/packages/core/test/test_native_dom_mutations.cpp"
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-dom-mutations-test.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/native-dom-mutations-test.cxx-flags.txt"
printf '%s\n' "${GEA_NATIVE_LINK_FLAGS[@]}" > "$BUILD_DIR/native-dom-mutations-test.link-flags.txt"
if [[ $# == 0 ]]; then
  for mode in replace-retained foreign-reference remove-detach focus-detach focus-retained focus-refocus focus-reentrant focus-throw move-focus reference-aba replace-atomic fragment-reference fragment-repeat fragment-direct-first; do
    "$BUILD_DIR/native-dom-mutations-test" "$mode"
  done
  : "${GEA_JSDOM_PACKAGE:?Set GEA_JSDOM_PACKAGE to the installed jsdom package directory}"
  node "$ROOT/packages/core/test/native-dom-mutations-oracle.mjs"
fi
