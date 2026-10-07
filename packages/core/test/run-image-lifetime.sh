#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
CXX_BIN="${CXX:-clang++}"
export GEA_NATIVE_JOBS="${GEA_NATIVE_JOBS:-2}"
export TMPDIR="$BUILD_DIR"
mkdir -p "$BUILD_DIR"
source "$ROOT/packages/core/test/native-test-common.sh"
cat > "$BUILD_DIR/program.cpp" <<'CPP'
void __gea_top_level() {}
CPP
gea_build_native_test "$BUILD_DIR" "$BUILD_DIR/image-lifetime" \
  "$ROOT/packages/core/test/test_image_lifetime.cpp" \
  "$ROOT/packages/host/host/image.cpp" \
  -DGEA_NATIVE_TEST_REAL_IMAGE_HOST=1
"$BUILD_DIR/image-lifetime" "$BUILD_DIR" "$ROOT/packages/core/test/fixtures/native-jpeg.jpg"
