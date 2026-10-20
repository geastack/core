#!/usr/bin/env bash
set -euo pipefail
TEST_DIR=$(cd "$(dirname "$0")" && pwd)
CORE_ROOT=$(cd "$TEST_DIR/../../.." && pwd)
OUT_DIR="$TEST_DIR/.build/native-main-context-retirement"
mkdir -p "$OUT_DIR"
CXX_BIN=${CXX:-clang++}
# The host flags give Windows its unistd.h (alarm) and the CRT's POSIX names.
source "$TEST_DIR/native-host.sh"
FLAGS=(-std=c++20 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} -I"$CORE_ROOT/packages/host/include")
"$CXX_BIN" "${FLAGS[@]}" -MD -MF "$OUT_DIR/worker.d" -c "$CORE_ROOT/packages/host/host/worker.cpp" -o "$OUT_DIR/worker.o"
"$CXX_BIN" "${FLAGS[@]}" -MD -MF "$OUT_DIR/test.d" -c "$TEST_DIR/test_native_main_context_retirement.cpp" -o "$OUT_DIR/test.o"
"$CXX_BIN" "${FLAGS[@]}" "$OUT_DIR/worker.o" "$OUT_DIR/test.o" -o "$OUT_DIR/test"
for scenario in pending active active-pump cleanup late uncreated dedicated;do "$OUT_DIR/test" "$scenario";done
