#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && "$1" == --build-only ) ]] || { echo 'Use --build-only or no arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/native-reference-identity"
CXX_BIN="${CXX:-clang++}"
: "${GEA_TEST_RUNTIME_HEADER:?Set actual shared compiler runtime gea_runtime.h}"
# A Windows path (C:\...) is absolute too; cygpath spells it /c/... for the check.
runtime_header_path=$GEA_TEST_RUNTIME_HEADER
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) runtime_header_path=$(cygpath -u "$runtime_header_path") ;; esac
[[ "$runtime_header_path" == /* && -f "$runtime_header_path" ]] || { echo 'Actual absolute shared runtime header required' >&2;exit 2; }
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-1}"
mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-reference-identity" "$ROOT/packages/core/test/test_native_reference_identity_tree.cpp" "-DGEA_TEST_RUNTIME_HEADER=\"$GEA_TEST_RUNTIME_HEADER\""
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/native-reference-identity.objects.txt"
if [[ $# == 0 ]]; then "$BUILD_DIR/native-reference-identity";fi
