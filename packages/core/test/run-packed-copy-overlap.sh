#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build/packed-copy-overlap"
CXX_BIN="${CXX:-clang++}"

mkdir -p "$BUILD_DIR"

# Windows: clang finds its UBSan runtime by name, searching the MSVC toolset
# libraries first, and MSVC 14.51+ ships a clang_rt.ubsan_standalone of its own
# there, built against the DLL C++ runtime for its bundled clang. lld-link then
# refuses it beside clang's default static runtime (/failifmismatch
# RuntimeLibrary). Link this clang's own UBSan runtime by path instead.
UBSAN_RUNTIME=()
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
  UBSAN_RUNTIME=(-fno-sanitize-link-runtime)
  for library in clang_rt.ubsan_standalone-x86_64.lib clang_rt.ubsan_standalone_cxx-x86_64.lib; do
    UBSAN_RUNTIME+=("$("$CXX_BIN" -fsanitize=undefined -print-file-name="$library")")
  done
  ;;
esac

# Header-only: PackedPixels lives entirely in pixel.h, and the test instantiates
# both depths (4bpp GRAY4, 2bpp GRAY2) directly, so no target format needs to be
# selected and no engine sources are linked.
"$CXX_BIN" -std=c++20 -O2 \
  -fsanitize=undefined \
  -Wno-deprecated \
  -ferror-limit=40 \
  -I "$ROOT/packages/core" \
  -I "$ROOT/packages/core/include" \
  "$ROOT/packages/core/test/test_packed_copy_overlap_main.cpp" \
  ${UBSAN_RUNTIME[@]+"${UBSAN_RUNTIME[@]}"} \
  -o "$BUILD_DIR/packed-copy-overlap-test"

"$BUILD_DIR/packed-copy-overlap-test"
