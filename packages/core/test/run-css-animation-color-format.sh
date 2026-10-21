#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/css-animation-color-format"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"

# The display harness is RGB565, so build the color track check once per pixel
# format: RGB565, RGBA8888 and ARGB8888.
for format in 0 1 2; do
  "$CXX_BIN" -std=c++20 -fsanitize=address,undefined -DGEA_EMBEDDED_PIXEL_FORMAT="$format" \
    -I"$ROOT/packages/core/include" -I"$ROOT/packages/engine" -I"$ROOT/packages/host/include" \
    "$ROOT/packages/core/test/test_css_animation_color_format.cpp" \
    -o "$BUILD_DIR/css-animation-color-format-$format"
  "$BUILD_DIR/css-animation-color-format-$format"
done
