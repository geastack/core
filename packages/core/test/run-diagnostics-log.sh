#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out"
export TMPDIR="$out"
"${CXX:-clang++}" -std=c++20 -pthread -fsanitize=address,undefined \
  -Ipackages/core/include -Ipackages/host/services \
  packages/core/test/test_diagnostics_log.cpp packages/host/services/diagnostics.cpp \
  -o "$out/test_diagnostics_log"
"$out/test_diagnostics_log" > /dev/null
