import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const testDir = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(testDir, '../../..')
const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-font-generator-'))

// The app this generates for is built here, out of this package's own font:
// all it needs from an app is one @font-face naming a real TTF.
const appDir = path.join(tempRoot, 'app')
fs.mkdirSync(path.join(appDir, 'fonts'), { recursive: true })
fs.copyFileSync(
  path.join(testDir, 'website/fonts/bebas-neue-400.ttf'),
  path.join(appDir, 'fonts/bebas-neue-400.ttf'),
)
fs.writeFileSync(
  path.join(appDir, 'fonts.css'),
  "@font-face {\n  font-family: 'Bebas Neue';\n  src: url('./fonts/bebas-neue-400.ttf');\n}\n",
)

const cssDir = path.join(tempRoot, 'css')
const outCpp = path.join(tempRoot, 'gea_embedded_font_generated.cpp')
const outH = path.join(tempRoot, 'gea_embedded_font_generated.h')
process.on('exit', () => fs.rmSync(tempRoot, { recursive: true, force: true }))

fs.mkdirSync(cssDir, { recursive: true })
fs.writeFileSync(path.join(cssDir, 'styles.css'), `
.fallback {
  font-family: 'Bebas Neue';
}

.exact {
  font-family: 'Bebas Neue';
  font-size: 20px;
}

.fluid {
  font-family: 'Bebas Neue';
  font-size: 1vmax;
}
`)

execFileSync(process.execPath, [
  path.join(repoRoot, 'packages/core/scripts/generate-gea-embedded-fonts.mjs'),
  '--app-dir', appDir,
  '--css-dir', cssDir,
  '--out-cpp', outCpp,
  '--out-h', outH,
  '--viewport-width', '200',
  '--viewport-width', '720',
  '--viewport-height', '200',
  '--viewport-height', '1440',
  '--device-pixel-ratio', '1.5',
  '--device-pixel-ratio', '2',
], { cwd: appDir, stdio: 'pipe' })

const generated = fs.readFileSync(outCpp, 'utf8')
const sizes = [...generated.matchAll(/font_data_\d+\s*=\s*\{\s*\d+,\s*(\d+),/g)].map((match) => Number(match[1]))

assert.deepEqual(
  sizes,
  [2, 14, 24, 30, 32, 40],
  'font generator should bake paired viewport sizes and DPR-scaled CSS px defaults',
)
assert.equal(sizes.includes(7), false, 'paired viewport generation must not synthesize 720x200 vmax')
assert.equal(sizes.includes(16), false, 'default CSS 16px font size must be scaled by DPR')
for (const codepoint of [0x2014, 0x2019, 0x2026, 0x2039, 0x203a, 0x2190, 0x2192, 0x21bb]) {
  assert.match(generated, new RegExp(`\\{\\s*${codepoint},`), `font atlas should include U+${codepoint.toString(16).toUpperCase()}`)
}

// A display size's atlas can be narrowed to the characters its text is made of,
// but only while every rule that names that size agrees -- one that doesn't
// takes the atlas back to the baseline, because the atlas is shared.
const charsetDir = path.join(tempRoot, 'charset-css')
const charsetCpp = path.join(tempRoot, 'charset.cpp')
const charsetH = path.join(tempRoot, 'charset.h')
fs.mkdirSync(charsetDir, { recursive: true })
fs.writeFileSync(
  path.join(charsetDir, 'styles.css'),
  `
.body {
  font-family: 'Bebas Neue';
  font-size: 10px;
}

.display {
  font-family: 'Bebas Neue';
  font-size: 50px;
  --gea-font-charset: '0123456789\\2014';
}

.shared {
  font-family: 'Bebas Neue';
  font-size: 60px;
  --gea-font-charset: 'AB';
}

.shared-widened {
  font-family: 'Bebas Neue';
  font-size: 60px;
}
`,
)

execFileSync(
  process.execPath,
  [
    path.join(repoRoot, 'packages/core/scripts/generate-gea-embedded-fonts.mjs'),
    '--app-dir', appDir,
    '--css-dir', charsetDir,
    '--out-cpp', charsetCpp,
    '--out-h', charsetH,
    '--device-pixel-ratio', '2',
  ],
  { cwd: appDir, stdio: 'pipe' },
)

const charsetGenerated = fs.readFileSync(charsetCpp, 'utf8')
const glyphCounts = new Map(
  [...charsetGenerated.matchAll(/font_glyphs_(\d+)\[(\d+)\]/g)].map((match) => [Number(match[1]), Number(match[2])]),
)
const atlasGlyphCounts = new Map(
  [...charsetGenerated.matchAll(/font_data_(\d+)\s*=\s*\{\s*\d+,\s*(\d+),/g)].map((match) => [
    Number(match[2]),
    glyphCounts.get(Number(match[1])),
  ]),
)

// Printable ASCII plus the punctuation the baseline adds.
const baselineGlyphCount = 0x7e - 0x20 + 1 + 31
assert.equal(atlasGlyphCounts.get(20), baselineGlyphCount, 'a size with no charset bakes the whole baseline')
// Runtime measurement labels and controls use these outside literal CSS text.
// Counting alone would miss a same-sized replacement of the required glyphs.
const baselineFontId = [...charsetGenerated.matchAll(/font_data_(\d+)\s*=\s*\{\s*\d+,\s*(\d+),/g)]
  .find((match) => Number(match[2]) === 20)[1]
const baselineGlyphTable = charsetGenerated.match(
  new RegExp(`font_glyphs_${baselineFontId}\\[\\d+\\] = \\{([\\s\\S]*?)\\n\\};`),
)[1]
for (const codepoint of [0x00b5, 0x00b7, 0x2212]) {
  assert.match(baselineGlyphTable, new RegExp(`\\{ ${codepoint},`), `baseline keeps UI codepoint ${codepoint}`)
}
// The ten digits, the escaped em dash, and the space and `?` every atlas keeps.
assert.equal(atlasGlyphCounts.get(100), 13, 'a charset narrows its own atlas, honouring CSS escapes')
assert.equal(atlasGlyphCounts.get(120), baselineGlyphCount, 'one rule without a charset widens the shared atlas back')
