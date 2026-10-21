#!/usr/bin/env bash
# Gpio and ECMA-419 Digital (runtime/gpio.ts) compiled by geatsc and run
# against fake pins: interrupt and polled edges, edge filters, close() from a
# callback, and the errors.
set -euo pipefail
[[ $# == 0 ]] || { echo 'No arguments' >&2;exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/dist/native-gpio-contract"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
: "${GEA_COMPILER_CLI:?Set GEA_COMPILER_CLI to the actual shared compiler dist/cli.js}"
node "$GEA_COMPILER_CLI" compile "$ROOT/packages/core/test/native-gpio-contract.ts" --out-dir "$BUILD_DIR" --project "$ROOT/packages/core/test/tsconfig-native-gpio.json" --plugin "$ROOT/packages/geatsc-plugin-gea/dist/index.js"
source "$ROOT/packages/core/test/native-test-common.sh"
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/native-gpio-contract-test" "$ROOT/packages/core/test/test_native_gpio_contract.cpp" \
  "$ROOT/packages/host/host/gpio.cpp"
expected='led true
key starts true
pin 4 is busy
an output has no edge
out 1 number
legacy true true true
-- key pressed
key false
-- key released
key true
-- bounce that settled back
-- pin 5 rises
readable 1 five 1
-- pin 5 falls, not watched
-- pin 5 rises again
readable 1 five 2
-- polled pin rises
polled 1
-- polled pin falls, before its next poll
-- polled pin is due
polled 0
-- pin 8 falls
closing
-- pin 8 is closed
ok'
actual="$("$BUILD_DIR/native-gpio-contract-test")"
if [[ "$actual" != "$expected" ]]; then
  diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") >&2 || true
  echo 'native-gpio-contract: FAIL' >&2
  exit 1
fi
echo 'native-gpio-contract: PASS'
