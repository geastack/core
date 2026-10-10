#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# Its own build directory: tests run in parallel, and a shared one has them
# overwrite each other's program.cpp and object cache mid-build.
BUILD_DIR="$ROOT/packages/core/test/.build/native-jpeg"
mkdir -p "$BUILD_DIR"
export TMPDIR="$BUILD_DIR"
"${CC:-clang}" -DGEA_EMBEDDED_GIF_C_API -c "$ROOT/packages/engine/vendor/AnimatedGIF/AnimatedGIF.c" \
  -I "$ROOT/packages/engine/vendor/AnimatedGIF" -o "$BUILD_DIR/native-jpeg-gif.o"
"${CXX:-clang++}" -std=c++20 -DGEA_EMBEDDED_ROM_TJPGD=1 -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN=1 \
  -I "$ROOT/packages/core/test/fixtures/rom-jpeg" -I "$ROOT/packages/core/include" \
  -I "$ROOT/packages/engine/vendor/stb" -I "$ROOT/packages/engine/vendor/AnimatedGIF" \
  "$ROOT/packages/engine/image_store.cpp" "$ROOT/packages/core/test/test_native_jpeg_main.cpp" \
  "$BUILD_DIR/native-jpeg-gif.o" -o "$BUILD_DIR/native-jpeg"
"$BUILD_DIR/native-jpeg" "$ROOT/packages/core/test/fixtures/native-jpeg.jpg"
for endian in 0 1; do
  "${CXX:-clang++}" -std=c++20 -DGEA_EMBEDDED_ROM_TJPGD=1 -DGEA_EMBEDDED_ESP_JPEG=1 -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN="$endian" \
    -I "$ROOT/packages/core/test/fixtures/esp-jpeg" -I "$ROOT/packages/core/test/fixtures/rom-jpeg" -I "$ROOT/packages/core/include" \
    -I "$ROOT/packages/engine/vendor/stb" -I "$ROOT/packages/engine/vendor/AnimatedGIF" \
    "$ROOT/packages/engine/image_store.cpp" "$ROOT/packages/core/test/test_native_jpeg_main.cpp" \
    "$BUILD_DIR/native-jpeg-gif.o" -o "$BUILD_DIR/native-jpeg-esp"
  "$BUILD_DIR/native-jpeg-esp" "$ROOT/packages/core/test/fixtures/native-jpeg.jpg"
done
