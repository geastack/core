#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 || ( $# == 1 && "$1" == --build-only ) ]] || { echo 'Use --build-only or no arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "$ROOT/packages/core/test/test-env.sh"
: "${GEA_TEST_RUNTIME_HEADER:?Set actual shared compiler runtime gea_runtime.h}"
[[ -f "$GEA_TEST_RUNTIME_HEADER" ]] || { echo 'Actual shared runtime header missing' >&2;exit 2; }
BUILD_DIR="$ROOT/dist/event-listener-trace/normal"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS=1
mkdir -p "$BUILD_DIR"
"$CXX_BIN" -std=c++20 -fno-rtti -I"$ROOT/packages/core/include" -MD -MF "$BUILD_DIR/baseline.d" "$ROOT/packages/core/test/test_event_listener_baseline.cpp" -o "$BUILD_DIR/baseline"
"$CXX_BIN" -std=c++20 -fno-rtti -fno-exceptions -I"$ROOT/packages/core/include" -MD -MF "$BUILD_DIR/compile-only.d" -c "$ROOT/packages/core/test/test_event_listener_compile_only.cpp" -o "$BUILD_DIR/compile-only.o"
"$CXX_BIN" -std=c++20 -O1 -g -fno-rtti -fsanitize=address,undefined -I"$ROOT/packages/core/include" -I"$(dirname "$GEA_TEST_RUNTIME_HEADER")" -MD -MF "$BUILD_DIR/reference-cycle.d" "$ROOT/packages/core/test/test_event_listener_reference_cycle.cpp" -o "$BUILD_DIR/reference-cycle"
printf 'void __gea_top_level() {}\n' > "$BUILD_DIR/program.cpp"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/host-bridges" "$ROOT/packages/core/test/test_event_listener_host_bridges.cpp" "-DGEA_TEST_RUNTIME_HEADER=\"$GEA_TEST_RUNTIME_HEADER\"" -fno-rtti
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/host-bridges.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/host-bridges.flags.txt"
# The compact runtime owns a linked release batch instead of the default vector.
# Compile the same real host fixture against that actual configuration as well.
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/host-bridges-compact" "$ROOT/packages/core/test/test_event_listener_host_bridges.cpp" "-DGEA_TEST_RUNTIME_HEADER=\"$GEA_TEST_RUNTIME_HEADER\"" -DGEA_RUNTIME_COMPACT_CODE=1 -fno-rtti
printf '%s\n' "${GEA_NATIVE_OBJECTS[@]}" > "$BUILD_DIR/host-bridges-compact.objects.txt"
printf '%s\n' "${GEA_NATIVE_CXXFLAGS[@]}" > "$BUILD_DIR/host-bridges-compact.flags.txt"
if [[ $# == 0 ]]; then
 "$BUILD_DIR/baseline"
 # LeakSanitizer is unsupported on Darwin; assertions and ASan/UBSan remain.
 if [[ "$(uname -s)" == Darwin ]]; then ASAN_OPTIONS=detect_leaks=0 "$BUILD_DIR/reference-cycle";else "$BUILD_DIR/reference-cycle";fi
 "$BUILD_DIR/host-bridges-compact" ordered-cleanup-throw
 for mode in original node-root document-root window-root body-root node-self document-self window-self body-self erase-destructor rare-destructor subscription-destructor clear-destructor ordered-cleanup-throw;do
  "$BUILD_DIR/host-bridges" "$mode"
 done
fi
