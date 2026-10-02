#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."

# Set GEA_COMPILER_RUNTIME to an installed released compiler's runtime headers
# to check compatibility without enabling any Worker features.
"${CXX:-clang++}" -std=c++20 -fsyntax-only \
  -I"${GEA_COMPILER_RUNTIME:-../compiler/src/targets/cpp/runtime}" \
  -Ipackages/host/include -Ipackages/core/include \
  -Ipackages/engine -Ipackages/engine/ui -Ipackages/elements/ui \
  packages/core/test/test_worker_header_compat.cpp
echo "Worker/audio host declarations: compiler runtime compatibility OK"
