#!/usr/bin/env bash
set -euo pipefail
TEST_DIR=$(cd "$(dirname "$0")" && pwd)
CORE_ROOT=$(cd "$TEST_DIR/../../.." && pwd)
source "$TEST_DIR/test-env.sh"
COMPILER_ROOT=${GEA_COMPILER_ROOT:-}
# The lifetime test itself is one of the compiler's own test sources, which the
# published @geastack/compiler package does not ship.
if [[ ! -f "$COMPILER_ROOT/test/runtime/native-websocket-lifetime.cpp" ]]; then
  echo "SKIP: needs a geastack/compiler checkout next to core (or GEA_COMPILER_CLI pointing into one) for test/runtime/native-websocket-lifetime.cpp" >&2
  exit 0
fi
OUT_DIR="$TEST_DIR/.build/native-typed-websocket-retirement"
mkdir -p "$OUT_DIR"
CXX_BIN=${CXX:-clang++}
# The host flags give Windows its unistd.h (alarm) and the CRT's POSIX names.
source "$TEST_DIR/native-host.sh"
FLAGS=(-std=c++20 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} -I"$CORE_ROOT/packages/host/include" -I"$CORE_ROOT/packages/core/include" -I"$CORE_ROOT/packages/engine" -I"$CORE_ROOT/packages/engine/ui" -I"$CORE_ROOT/packages/elements/ui" -I"$COMPILER_ROOT/src/targets/cpp/runtime")
for unit in worker websocket;do
 "$CXX_BIN" "${FLAGS[@]}" -MD -MF "$OUT_DIR/$unit.d" -c "$CORE_ROOT/packages/host/host/$unit.cpp" -o "$OUT_DIR/$unit.o"
done
"$CXX_BIN" "${FLAGS[@]}" -MD -MF "$OUT_DIR/test.d" -c "$COMPILER_ROOT/test/runtime/native-websocket-lifetime.cpp" -o "$OUT_DIR/test.o"
"$CXX_BIN" "${FLAGS[@]}" "$OUT_DIR/worker.o" "$OUT_DIR/websocket.o" "$OUT_DIR/test.o" -o "$OUT_DIR/test"
for mode in destroyed terminal null-first replacement;do "$OUT_DIR/test" "$mode";done
