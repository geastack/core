#!/usr/bin/env bash
set -euo pipefail
TEST_DIR=$(cd "$(dirname "$0")" && pwd)
CORE_ROOT=$(cd "$TEST_DIR/../../.." && pwd)
OUT_DIR="$TEST_DIR/.build/native-timer-retirement"
mkdir -p "$OUT_DIR"
CXX_BIN=${CXX:-clang++}
# The host flags give Windows its unistd.h (alarm) and the CRT's POSIX names.
source "$TEST_DIR/native-host.sh"
FLAGS=(-std=c++17 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} -I"$CORE_ROOT/packages/host/include" -I"$CORE_ROOT/packages/core/include")
"$CXX_BIN" "${FLAGS[@]}" "$CORE_ROOT/packages/host/host/timers.cpp" "$TEST_DIR/test_native_timer_retirement.cpp" -o "$OUT_DIR/test"
for scenario in reset compact terminal timer-active frame-active; do
  "$OUT_DIR/test" "$scenario"
done
