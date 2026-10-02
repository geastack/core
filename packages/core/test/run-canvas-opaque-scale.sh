#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out="$PWD/packages/geatsc-plugin-gea/dist"
test -d "$out"
export TMPDIR="$out"
cxx="${CXX:-clang++}"
link_gc=-Wl,--gc-sections
if [[ "$(uname -s)" == Darwin ]]; then link_gc=-Wl,-dead_strip; fi
for endian in 0 1; do
  "$cxx" -std=c++20 -O2 -fsanitize=address,undefined -Wno-deprecated \
    -ffunction-sections -fdata-sections "$link_gc" -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN="$endian" \
    -Ipackages/core/include packages/engine/canvas.cpp packages/core/test/test_canvas_opaque_scale.cpp \
    -o "$out/test_canvas_opaque_scale"
  "$out/test_canvas_opaque_scale"
done
