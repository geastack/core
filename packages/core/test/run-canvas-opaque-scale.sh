#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
source packages/core/test/test-env.sh
gea_require_plugin_build
out="$PWD/packages/geatsc-plugin-gea/dist" # the plugin package's normal build output
export TMPDIR="$out"
cxx="${CXX:-clang++}"
source packages/core/test/native-host.sh
for endian in 0 1; do
  "$cxx" -std=c++20 -O2 -fsanitize=address,undefined -Wno-deprecated \
    -ffunction-sections -fdata-sections "${GEA_HOST_LINK_GC[@]}" ${GEA_HOST_CXXFLAGS[@]+"${GEA_HOST_CXXFLAGS[@]}"} -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN="$endian" \
    -Ipackages/core/include packages/engine/canvas.cpp packages/core/test/test_canvas_opaque_scale.cpp \
    -o "$out/test_canvas_opaque_scale"
  "$out/test_canvas_opaque_scale"
done
