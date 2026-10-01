#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="$ROOT/packages/core/test/.build"
mkdir -p "$BUILD_DIR"
"${CXX:-clang++}" -std=c++20 -O2 -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN=1 -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 \
  -I "$ROOT/packages/core/include" -I "$ROOT/packages/engine" \
  -ffunction-sections -fdata-sections \
  "$ROOT/packages/core/test/test_text_sprite_cache_main.cpp" \
  "$ROOT/packages/engine/rasterized_font.cpp" \
  -Wl,-dead_strip -o "$BUILD_DIR/text-sprite-cache"
"$BUILD_DIR/text-sprite-cache"
if [[ "${1:-}" == "--mutations" ]]; then
  python3 - "$ROOT/packages/engine/canvas.cpp" "$BUILD_DIR/text-sprite-cache-mutant.cpp" <<'PY'
import pathlib,sys
s = pathlib.Path(sys.argv[1]).read_text()
start = s.index('TextSprite &textSpriteFor(')
end = s.index('\n// Rasterize `text`', start)
s = s[:start] + '''TextSprite &textSpriteFor(const char *text, const RasterizedFont &) {
    unsigned hash = 2166136261u;
    for (const char *p = text; *p; ++p) { hash ^= static_cast<unsigned char>(*p); hash *= 16777619u; }
    return gTextSprites[hash % kTextSpriteSlots];
}
''' + s[end:]
pathlib.Path(sys.argv[2]).write_text(s)
PY
  "${CXX:-clang++}" -std=c++20 -O2 -DGEA_EMBEDDED_PIXEL_PANEL_ENDIAN=1 -DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 \
    "-DGEA_TEXT_CACHE_SOURCE=\"$BUILD_DIR/text-sprite-cache-mutant.cpp\"" \
    -I "$ROOT/packages/core/include" -I "$ROOT/packages/engine" -ffunction-sections -fdata-sections \
    "$ROOT/packages/core/test/test_text_sprite_cache_main.cpp" "$ROOT/packages/engine/rasterized_font.cpp" \
    -Wl,-dead_strip -o "$BUILD_DIR/text-sprite-cache-mutant"
  if "$BUILD_DIR/text-sprite-cache-mutant" > "$BUILD_DIR/text-sprite-cache-mutant.log" 2>&1; then
    echo 'FAIL: direct-mapped text collision passed' >&2; exit 1
  fi
  grep -q 'alternating 59/60 must both warm up' "$BUILD_DIR/text-sprite-cache-mutant.log"
  echo 'PASS: old text-cache collision rejected by its behavioral assertion'
fi
