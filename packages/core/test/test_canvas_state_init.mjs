// Exercise the production cache declarations with both initialization policies.
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { mkdirSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const source = readFileSync(new URL('../../engine/canvas.cpp', import.meta.url), 'utf8')
const core = process.env.GEA_TEST_CORE_ROOT || fileURLToPath(new URL('..', import.meta.url))
const build = process.env.GEA_TEST_BUILD_DIR || path.join(core, 'build')
mkdirSync(build, { recursive: true })
function declaration(name) {
  const start = source.indexOf(`struct ${name} {`)
  assert.notEqual(start, -1)
  const end = source.indexOf('\n};', start)
  assert.notEqual(end, -1)
  return source.slice(start, end + 3)
}
const program = `
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
#include "graphics/font.h"
#include "ui/state_init.h"
using gea::framework::graphics::Glyph;
using gea::framework::graphics::RasterizedFontData;
namespace pixel { using native_t = std::uint16_t; }
constexpr int kTextSpriteMaxLen = 63;
${declaration('TextSprite')}
${declaration('SolidGlyphRow')}
${declaration('SolidGlyphCacheEntry')}
TextSprite sprites[4];
SolidGlyphCacheEntry glyphs[48];
void check(const TextSprite &s) {
  for (char c : s.text) assert(c == 0);
  assert(!s.fontData && s.sizePx == -1 && s.state == TextSprite::Empty);
  assert(s.minLX == 0 && s.minLY == 0 && s.w == 0 && s.h == 0);
  assert(!s.cov && !s.rowX0 && !s.rowX1 && s.covCap == 0 && s.extCap == 0);
  assert(!s.rleVal && !s.rleLen && !s.rleRowStart);
  assert(s.rleCap == 0 && s.rleRowCap == 0 && !s.rleValid);
}
void check(const SolidGlyphCacheEntry &s) {
  assert(!s.font && s.color == 0 && s.background == 0 && s.codepoint == 0);
  assert(s.glyph.codepoint == 0 && s.glyph.width == 0 && s.glyph.height == 0);
  assert(s.glyph.advance == 0 && s.glyph.bearingX == 0 && s.glyph.bearingY == 0);
  assert(s.glyph.advance16 == -1 && s.rows.empty() && s.pixels.empty());
  assert(s.lastUsed == 0);
}
int main() {
  // Global construction must install the nonzero sentinels before first use.
  for (const auto &s : sprites) check(s);
  for (const auto &s : glyphs) check(s);
  // Also exercise construction into nonzero storage, assignment and destruction.
  // This catches an accidental zero-BSS-only initializer or broken vector lifetime.
  alignas(TextSprite) unsigned char spriteStorage[sizeof(TextSprite)];
  alignas(SolidGlyphCacheEntry) unsigned char glyphStorage[sizeof(SolidGlyphCacheEntry)];
  for (int i = 0; i < 128; ++i) {
    std::memset(spriteStorage, 0xa5, sizeof spriteStorage);
    auto *s = new (spriteStorage) TextSprite;
    check(*s);
    s->text[0] = 'x'; s->sizePx = 48; s->state = TextSprite::Seen;
    *s = TextSprite{}; check(*s); s->~TextSprite();
    std::memset(glyphStorage, 0xa5, sizeof glyphStorage);
    auto *g = new (glyphStorage) SolidGlyphCacheEntry;
    check(*g);
    g->rows.push_back({1, 2, 3, 4}); g->pixels.assign(29, 0x1234);
    g->glyph.advance16 = 91; g->lastUsed = 13;
    *g = SolidGlyphCacheEntry{}; check(*g); g->~SolidGlyphCacheEntry();
  }
}
`
// Windows has no `c++`, and spawn finds an executable only by its .exe name.
const windows = process.platform === 'win32'
for (const enabled of [undefined, 0, 1]) {
  const binary = path.join(build, `canvas-state-init-${enabled ?? 'default'}${windows ? '.exe' : ''}`)
  const compiled = spawnSync(process.env.CXX || (windows ? 'clang++' : 'c++'), [
    '-std=c++20', '-O2', '-g', '-fsanitize=address,undefined',
    ...(enabled === undefined ? [] : [`-DGEA_EMBEDDED_UI_STATE_DYNAMIC_INIT=${enabled}`]),
    '-I', path.join(core, 'include'), '-I', path.join(core, '../engine'),
    '-x', 'c++', '-', '-o', binary,
  ], { input: program, encoding: 'utf8', env: { ...process.env, TMPDIR: build } })
  assert.ifError(compiled.error)
  assert.equal(compiled.status, 0, compiled.stderr)
  const ran = spawnSync(binary, [], { encoding: 'utf8', timeout: 15000 })
  assert.ifError(ran.error)
  assert.equal(ran.status, 0, ran.stderr)
}
console.log('Canvas state: default/off/on initialization, sentinels, reuse and vector lifetime passed')
