#!/usr/bin/env bash
# GeaOS ownership table (packages/geaos/geaos_owned.h), with ASan and UBSan.
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
"${CXX:-clang++}" -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ipackages/geaos packages/core/test/test_geaos_owned.cpp -o "$out/test_geaos_owned"
"$out/test_geaos_owned"
