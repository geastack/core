#!/usr/bin/env node
import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import zlib from 'node:zlib'
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'

const args = process.argv.slice(2)

function readOption(name) {
  const index = args.indexOf(name)
  return index >= 0 ? args[index + 1] : undefined
}

function hasFlag(name) {
  return args.includes(name)
}

function readAllOptions(name) {
  const values = []
  for (let i = 0; i < args.length; i++) {
    if (args[i] === name && i + 1 < args.length) values.push(args[i + 1])
  }
  return values
}

function fail(message) {
  process.stderr.write(`${message}\n`)
  process.exit(1)
}

const appDir = path.resolve(readOption('--app-dir') ?? fail('missing --app-dir <dir>'))
const cssDir = path.resolve(readOption('--css-dir') ?? fail('missing --css-dir <dir>'))
const outCpp = path.resolve(readOption('--out-cpp') ?? fail('missing --out-cpp <file>'))
const outH = path.resolve(readOption('--out-h') ?? fail('missing --out-h <file>'))
const symbolPrefix = readOption('--symbol-prefix')
const viewportWidths = parseViewportWidths(readAllOptions('--viewport-width'))
const viewportHeights = parseViewportWidths(readAllOptions('--viewport-height'))
const devicePixelRatios = parseDevicePixelRatios(readAllOptions('--device-pixel-ratio'))
const runtimeTtfFonts = hasFlag('--runtime-ttf-fonts')
// Glyph rasterization supersampling. 4 (default) = antialiased coverage for
// grayscale/color panels. 1 = sample at 1:1 = NO antialiasing = crisp binary
// glyphs, which read better on 1-bit B/W e-paper than thresholded AA (every
// glyph pixel is fully on/off, so the panel's mono threshold is a no-op).
// Set per board by build-gea-vite-geatsc.mjs.
const glyphSupersample = Math.max(1, Math.round(Number(readOption('--supersample') ?? '4')) || 4)
// Atlas pixel depth. 8 (default) = one coverage byte per pixel. 2 = four pixels
// per byte, quantized offline to the four levels a 4-gray e-paper panel can
// actually show — the atlas costs a quarter of the flash bandwidth to read for
// no visible loss on such a panel. Set per board by build-gea-vite-geatsc.mjs.
const atlasBits = Number(readOption('--atlas-bits') ?? '8')
if (atlasBits !== 2 && atlasBits !== 8) fail(`unsupported --atlas-bits ${atlasBits} (expected 2 or 8)`)

const genericFamilies = new Set(['serif', 'sans-serif', 'monospace', 'cursive', 'fantasy', 'system-ui'])

// Baseline glyph coverage for prose and UI chrome. EPUB text routinely uses
// typographic quotes, dashes and ellipses even when the language is otherwise
// plain ASCII; navigation controls also use arrows and single guillemets. If
// these codepoints are absent from the baked atlas the renderer falls back to
// `?`, despite the source TTF containing the glyph.
const embeddedFontExtraCodepoints = [
  0x00a0, // no-break space
  0x00a9, // copyright
  0x00ab, // left double guillemet
  0x00b0, // degree
  0x00b1, // plus-minus
  0x00b5, // micro sign (µs, µF)
  0x00b7, // middle dot (common UI separator; only appears in dynamic strings)
  0x00bb, // right double guillemet
  0x00d7, // multiplication sign (close buttons, 2× scale)
  0x2010, // hyphen
  0x2011, // non-breaking hyphen
  0x2013, // en dash
  0x2014, // em dash
  0x2018, // left single quotation mark
  0x2019, // right single quotation mark
  0x201c, // left double quotation mark
  0x201d, // right double quotation mark
  0x2022, // bullet
  0x2026, // ellipsis
  0x2212, // minus sign (UI −/+ controls read better than ASCII hyphen)
  0x2032, // prime
  0x2033, // double prime
  0x2039, // left single guillemet
  0x203a, // right single guillemet
  0x20ac, // euro
  0x2122, // trademark
  0x2190, // left arrow
  0x2191, // up arrow
  0x2192, // right arrow
  0x2193, // down arrow
  0x21bb, // clockwise open circle arrow
]

function baselineCodepoints() {
  const codepoints = []
  for (let cp = 0x20; cp <= 0x7e; cp += 1) codepoints.push(cp)
  codepoints.push(...embeddedFontExtraCodepoints)
  return codepoints
}

// Every atlas carries that whole baseline, at every size the app asks for. A
// glyph's area grows with the square of the size, so one display size can cost
// more than all the body sizes together -- and its text is usually a closed set:
// a tuner's note names, a clock's digits. An app narrows such an atlas by naming
// the characters it can ever render, on the rule that sets the size:
//
//   .tuner-note { font-size: 52px; --gea-font-charset: 'ABCDEFG#0123456789-' }
//
// The charset is a list of characters, so it cannot contain a `;`, and it
// replaces the baseline rather than intersecting it -- an app may name a
// codepoint the baseline omits, as long as the font has the glyph. CSS escapes
// (`\2014` for an em dash) are honoured, so a stylesheet never has to carry a
// literal non-ASCII character to name one.
function parseFontCharset(value) {
  const text = unquoteCss(value.trim())
  if (text === '') return null
  // Space is a glyph like any other, and `?` is what the renderer substitutes
  // for one it cannot find, so neither is ever worth leaving out.
  const codepoints = new Set([0x20, 0x3f])
  // A CSS escape is a backslash, up to six hex digits, and one optional space
  // that ends the digits rather than being a character in its own right.
  const escapeRe = /\\([0-9a-fA-F]{1,6})[ ]?|\\(.)|([\s\S])/gu
  let escape
  while ((escape = escapeRe.exec(text)) !== null) {
    if (escape[1] !== undefined) codepoints.add(Number.parseInt(escape[1], 16))
    else codepoints.add((escape[2] ?? escape[3]).codePointAt(0))
  }
  return codepoints
}

// null is "unrestricted", and it wins: a size is one atlas however many rules
// ask for it, so a single rule naming that size without a charset has to take
// the atlas back to the baseline. Narrowing on the strength of the rules that
// opted in would render the other rules' text as `?`.
function mergeCharsets(a, b) {
  if (a === null || b === null) return null
  return new Set([...a, ...b])
}

function findCssFiles(dir) {
  const files = []
  const stack = [dir]
  while (stack.length > 0) {
    const current = stack.pop()
    if (!current || !fs.existsSync(current)) continue
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) stack.push(full)
      else if (entry.isFile() && entry.name.endsWith('.css')) files.push(full)
    }
  }
  files.sort()
  return files
}

function stripCssComments(css) {
  return css.replace(/\/\*[\s\S]*?\*\//g, '')
}

function unquoteCss(value) {
  const text = value.trim()
  if (text.length >= 2 && ((text[0] === '"' && text[text.length - 1] === '"') || (text[0] === "'" && text[text.length - 1] === "'"))) {
    return text.slice(1, -1)
  }
  return text
}

function firstFontFamily(value) {
  let text = value.trim()
  if (!text) return null
  if (text[0] === '"' || text[0] === "'") {
    const quote = text[0]
    const end = text.indexOf(quote, 1)
    return end > 0 ? text.slice(1, end) : null
  }
  const comma = text.indexOf(',')
  if (comma >= 0) text = text.slice(0, comma)
  const family = unquoteCss(text.trim())
  return family && !genericFamilies.has(family.toLowerCase()) ? family : null
}

function parseViewportWidths(values) {
  const widths = []
  for (const value of values) {
    for (const part of String(value).split(',')) {
      const width = Math.round(Number(part.trim()))
      if (width > 0 && !widths.includes(width)) widths.push(width)
    }
  }
  return widths
}

function parseDevicePixelRatios(values) {
  const ratios = []
  for (const value of values) {
    for (const part of String(value).split(',')) {
      const ratio = Number(part.trim())
      if (ratio > 0 && !ratios.includes(ratio)) ratios.push(ratio)
    }
  }
  return ratios.length > 0 ? ratios : [1]
}

function splitTopLevel(value, delimiter) {
  const out = []
  let start = 0
  let depth = 0
  for (let i = 0; i < value.length; i++) {
    const char = value[i]
    if (char === '(') depth++
    else if (char === ')' && depth > 0) depth--
    else if (char === delimiter && depth === 0) {
      out.push(value.slice(start, i).trim())
      start = i + 1
    }
  }
  out.push(value.slice(start).trim())
  return out.filter(Boolean)
}

function functionInner(value, name) {
  const text = value.trim()
  const prefix = `${name}(`
  if (!text.toLowerCase().startsWith(prefix) || !text.endsWith(')')) return null
  return text.slice(prefix.length, -1)
}

function uniqueSizes(sizes) {
  return sizes
    .map((size) => Math.round(size))
    .filter((size, index, list) => size > 0 && list.indexOf(size) === index)
}

function viewportCombos() {
  if (viewportWidths.length === 0 && viewportHeights.length === 0) return [{ width: 0, height: 0 }]
  if (viewportWidths.length === 0) return viewportHeights.map((height) => ({ width: 0, height }))
  if (viewportHeights.length === 0) return viewportWidths.map((width) => ({ width, height: 0 }))
  if (viewportWidths.length === viewportHeights.length) {
    return viewportWidths.map((width, index) => ({ width, height: viewportHeights[index] }))
  }
  const combos = []
  for (const width of viewportWidths) {
    for (const height of viewportHeights) combos.push({ width, height })
  }
  return combos
}

function defaultFontSizes() {
  return uniqueSizes(devicePixelRatios.map((dpr) => 16 * dpr))
}

// Resolve a single CSS length token to ONE physical-pixel size for a specific
// (viewportWidth, viewportHeight, devicePixelRatio) combination, mirroring the
// runtime's parseLengthForNode. Returns a float (caller rounds) or null when the
// token isn't a recognized length.
//
// Crucially, clamp()/min()/max() resolve to their actual CSS *value* for the
// combination — `clamp(a, b, c)` = `max(a, min(b, c))` — NOT the union of their
// literal arguments. The old union behavior baked an atlas for every literal,
// so `clamp(46px, 17vh, 220px)` on a 200x200 board baked a 330px (220px x 1.5
// DPR) glyph atlas (~3.4 MB) that can NEVER render there (the clamp resolves to
// ~69px). Resolving correctly bakes only the size the device actually requests,
// which is also an exact atlas hit (the board's physical viewport divided by DPR
// equals its logical viewport, so vh-no-DPR here == logical-vh x DPR at runtime).
function resolveLengthForCombo(value, width, height, dpr) {
  const text = value.trim()
  const absoluteMatch = text.match(/^(\d+(?:\.\d+)?)(px)?$/)
  if (absoluteMatch) {
    const cssPx = Number(absoluteMatch[1])
    return absoluteMatch[2] ? cssPx * dpr : cssPx
  }
  const vwMatch = text.match(/^(\d+(?:\.\d+)?)vw$/)
  if (vwMatch) return (Number(vwMatch[1]) * width) / 100
  const vhMatch = text.match(/^(\d+(?:\.\d+)?)vh$/)
  if (vhMatch) return (Number(vhMatch[1]) * height) / 100
  const vminMatch = text.match(/^(\d+(?:\.\d+)?)vmin$/)
  if (vminMatch) return (Number(vminMatch[1]) * Math.min(width, height)) / 100
  const vmaxMatch = text.match(/^(\d+(?:\.\d+)?)vmax$/)
  if (vmaxMatch) return (Number(vmaxMatch[1]) * Math.max(width, height)) / 100
  const clampInner = functionInner(text, 'clamp')
  if (clampInner !== null) {
    const parts = splitTopLevel(clampInner, ',')
    if (parts.length !== 3) return null
    const resolved = parts.map((part) => resolveLengthForCombo(part, width, height, dpr))
    if (resolved.some((size) => size === null)) return null
    const [lo, preferred, hi] = resolved
    return Math.max(lo, Math.min(preferred, hi))
  }
  const minInner = functionInner(text, 'min')
  if (minInner !== null) {
    const resolved = splitTopLevel(minInner, ',').map((part) => resolveLengthForCombo(part, width, height, dpr))
    if (resolved.length === 0 || resolved.some((size) => size === null)) return null
    return Math.min(...resolved)
  }
  const maxInner = functionInner(text, 'max')
  if (maxInner !== null) {
    const resolved = splitTopLevel(maxInner, ',').map((part) => resolveLengthForCombo(part, width, height, dpr))
    if (resolved.length === 0 || resolved.some((size) => size === null)) return null
    return Math.max(...resolved)
  }
  return null
}

// Bake one atlas size per distinct resolved value across every configured
// (viewport, DPR) combination. Each (width, height) pair is one board; absolute
// px is viewport-independent (width/height fall back to 0 so it still resolves).
// uniqueSizes drops non-positive (viewport-relative tokens with no viewport) and
// duplicate sizes. This mirrors the runtime so each requested px hits an atlas.
function parseFontSizes(value) {
  const combos = viewportCombos()
  const sizes = []
  for (const dpr of devicePixelRatios) {
    for (const { width, height } of combos) {
      const size = resolveLengthForCombo(value, width, height, dpr)
      if (size !== null) sizes.push(size)
    }
  }
  return uniqueSizes(sizes)
}

function simpleCssSelectors(selectorText) {
  return selectorText
    .split(',')
    .map((selector) => selector.trim())
    .filter((selector) => /^\.?[A-Za-z][A-Za-z0-9_-]*$/.test(selector))
}

function rememberFontTuple(tuples, fontFaces, family, sizePx, charset) {
  if (!family || !fontFaces.has(family)) return
  const resolvedSize = sizePx ?? defaultFontSizes()[0] ?? 16
  const key = `${family}\0${resolvedSize}`
  const existing = tuples.get(key)
  const merged = existing ? mergeCharsets(existing.charset, charset ?? null) : (charset ?? null)
  tuples.set(key, { family, sizePx: resolvedSize, charset: merged })
}

function rememberFontTuples(tuples, fontFaces, family, sizePxs, charset) {
  if (!sizePxs || sizePxs.length === 0) {
    for (const sizePx of defaultFontSizes()) rememberFontTuple(tuples, fontFaces, family, sizePx, null)
    return
  }
  for (const sizePx of sizePxs) rememberFontTuple(tuples, fontFaces, family, sizePx, charset)
}

function parseFontFaces(css, cssFile) {
  const faces = []
  const cssDirName = path.dirname(cssFile)
  const re = /@font-face\s*\{([^}]+)\}/gi
  let match
  while ((match = re.exec(css)) !== null) {
    const block = match[1]
    const familyMatch = block.match(/font-family\s*:\s*(?:"([^"]+)"|'([^']+)'|([^;]+))/i)
    const srcMatch = block.match(/src\s*:\s*url\(\s*(?:"([^"]+)"|'([^']+)'|([^)"']+))\s*\)/i)
    const family = familyMatch ? unquoteCss((familyMatch[1] ?? familyMatch[2] ?? familyMatch[3]).trim()) : null
    const rawSrc = srcMatch ? (srcMatch[1] ?? srcMatch[2] ?? srcMatch[3]).trim() : null
    if (!family || !rawSrc || /^https?:\/\//i.test(rawSrc) || rawSrc.startsWith('data:')) continue
    const cleanSrc = rawSrc.split(/[?#]/, 1)[0]
    const resolved = path.resolve(cssDirName, cleanSrc)
    if (fs.existsSync(resolved)) faces.push({ family, src: resolved })
  }
  return faces
}

function collectSourceFontFaces() {
  // The app, then whatever the project shares between its apps. The project is
  // the directory this ran in.
  const sourceDirs = [appDir, path.join(process.cwd(), 'components')]
  const fontFaces = new Map()
  for (const dir of sourceDirs) {
    for (const file of findCssFiles(dir)) {
      const css = stripCssComments(fs.readFileSync(file, 'utf8'))
      for (const face of parseFontFaces(css, file)) {
        if (!fontFaces.has(face.family)) fontFaces.set(face.family, face.src)
      }
    }
  }
  return fontFaces
}

function collectUsedFontTuples(fontFaces) {
  const tuples = new Map()
  const usedFamilies = new Set()
  const inheritedSizes = new Map()
  for (const file of findCssFiles(cssDir)) {
    const css = stripCssComments(fs.readFileSync(file, 'utf8'))
    const selectorStates = new Map()
    const ruleRe = /([^{}]+)\{([^{}]*)\}/g
    let match
    while ((match = ruleRe.exec(css)) !== null) {
      const selector = match[1].trim()
      if (selector.startsWith('@')) continue
      const selectors = simpleCssSelectors(selector)
      if (selectors.length === 0) continue
      let family = null
      let sizePxs = null
      let charset = null
      for (const declaration of match[2].split(';')) {
        const colon = declaration.indexOf(':')
        if (colon <= 0) continue
        const name = declaration.slice(0, colon).trim().toLowerCase()
        const value = declaration.slice(colon + 1).trim()
        if (name === 'font-family') family = firstFontFamily(value)
        else if (name === 'font-size') sizePxs = parseFontSizes(value)
        else if (name === '--gea-font-charset') charset = parseFontCharset(value)
      }
      // Accumulate every family/size a selector is ever assigned: repeated
      // rules are usually media-query branches (the block regex sees through
      // `@media { ... }` wrappers), and the generator can't evaluate media
      // conditions, so every branch needs an atlas.
      for (const selector of selectors) {
        const state = selectorStates.get(selector) ?? { families: new Set(), sizePxs: new Set(), charset: undefined }
        if (family) state.families.add(family)
        // Only a rule that names a size has an opinion about that size's atlas.
        // A rule on the same selector that just sets a colour must not widen it.
        if (sizePxs) {
          for (const sizePx of sizePxs) state.sizePxs.add(sizePx)
          state.charset = state.charset === undefined ? charset : mergeCharsets(state.charset, charset)
        }
        selectorStates.set(selector, state)
      }
    }
    for (const state of selectorStates.values()) {
      const charset = state.charset ?? null
      for (const family of state.families) {
        if (fontFaces.has(family)) usedFamilies.add(family)
        rememberFontTuples(tuples, fontFaces, family, [...state.sizePxs], charset)
      }
      if (state.families.size === 0) {
        for (const sizePx of state.sizePxs) {
          const merged = inheritedSizes.has(sizePx) ? mergeCharsets(inheritedSizes.get(sizePx), charset) : charset
          inheritedSizes.set(sizePx, merged)
        }
      }
    }
  }
  // A font-size on a selector that never declares font-family applies to text
  // whose family is inherited from an ancestor element. Which ancestor is
  // unknowable from CSS alone, so bake that size for every family the app
  // uses — otherwise the runtime's nearest-size fallback silently renders
  // such text from whatever atlas exists (e.g. the family's default 16px).
  // Sizes declared alongside a family stay exact pairs: spreading them across
  // families would bake atlases no element resolves to (a display family's
  // huge numeral size costs hundreds of KB at the body family).
  if (inheritedSizes.size > 0) {
    for (const family of usedFamilies) {
      for (const [sizePx, charset] of inheritedSizes) {
        rememberFontTuples(tuples, fontFaces, family, [sizePx], charset)
      }
    }
  }
  return [...tuples.values()].sort((a, b) => a.family.localeCompare(b.family) || a.sizePx - b.sizePx)
}

function loadOpentype() {
  const packageJsons = [
    path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'package.json'),
    path.join(appDir, 'package.json'),
    path.join(process.cwd(), 'package.json'),
  ]
  for (const packageJson of packageJsons) {
    if (!fs.existsSync(packageJson)) continue
    try {
      return createRequire(packageJson)('opentype.js')
    } catch {
      // Try the next package root.
    }
  }
  fail('missing opentype.js from the @geastack/core installation')
}

function rasterizeFont(opentype, fontPath, family, familyId, sizePx, fontId, charset) {
  const fontBuffer = fs.readFileSync(fontPath)
  const arrayBuffer = fontBuffer.buffer.slice(fontBuffer.byteOffset, fontBuffer.byteOffset + fontBuffer.byteLength)
  const font = opentype.parse(arrayBuffer)

  const scale = sizePx / font.unitsPerEm
  const ascender = Math.ceil(font.ascender * scale)
  const descender = Math.ceil(Math.abs(font.descender * scale))
  const lineHeight = ascender + descender
  const glyphs = []
  const glyphBitmaps = []

  const codepoints = charset ? [...charset].sort((a, b) => a - b) : baselineCodepoints()

  for (const cp of codepoints) {
    const glyph = font.charToGlyph(String.fromCodePoint(cp))
    const advance = Math.round((glyph.advanceWidth ?? 0) * scale)
    const bounds = glyph.getBoundingBox()
    const x0 = Math.floor(bounds.x1 * scale)
    const y0 = Math.floor(-bounds.y2 * scale)
    const x1 = Math.ceil(bounds.x2 * scale)
    const y1 = Math.ceil(-bounds.y1 * scale)
    const gw = Math.max(x1 - x0, 0)
    const gh = Math.max(y1 - y0, 0)
    const bitmap = new Uint8Array(gw * gh)

    if (gw > 0 && gh > 0) {
      rasterizeGlyph(glyph.getPath(0, 0, sizePx).commands, bitmap, gw, gh, x0, y0)
    }

    glyphs.push({ codepoint: cp, sourceX: 0, sourceY: 0, width: gw, height: gh, advance, bearingX: x0, bearingY: -y0 })
    glyphBitmaps.push({ data: bitmap, width: gw, height: gh })
  }

  const { atlasWidth, atlasHeight } = packGlyphAtlas(glyphs)
  if (atlasWidth === 0 || atlasHeight === 0) {
    return {
      id: fontId,
      family,
      familyId,
      sizePx,
      lineHeight,
      ascender,
      descender,
      glyphs,
      atlasWidth: 1,
      atlasHeight: 1,
      atlasBits,
      atlasData: new Uint8Array(1),
    }
  }

  const atlasData = new Uint8Array(atlasWidth * atlasHeight)
  for (let i = 0; i < glyphs.length; i += 1) {
    const glyph = glyphs[i]
    const bitmap = glyphBitmaps[i]
    // Quantize per glyph, not across the packed atlas: the neighbourhood rules
    // below must not see a neighbouring glyph's ink through the 1px padding.
    const source = atlasBits === 2 ? quantizeCoverageToLevels(bitmap.data, bitmap.width, bitmap.height) : bitmap.data
    for (let row = 0; row < bitmap.height; row += 1) {
      for (let col = 0; col < bitmap.width; col += 1) {
        atlasData[(glyph.sourceY + row) * atlasWidth + glyph.sourceX + col] = source[row * bitmap.width + col]
      }
    }
  }

  return {
    id: fontId,
    family,
    familyId,
    sizePx,
    lineHeight,
    ascender,
    descender,
    glyphs,
    atlasWidth,
    atlasHeight,
    atlasBits,
    atlasData: atlasBits === 2 ? packLevelsTo2Bit(atlasData, atlasWidth, atlasHeight) : atlasData,
  }
}

// Offline quantization of 8-bit supersampled coverage to the four levels of a
// 4-gray panel. Level 3 = full ink, level 0 = bare paper; the runtime maps a
// level back out as level * 85, so 3 -> 255 and 0 -> 0 exactly.
//
// The thresholds are deliberately NON-linear — 208/128/48, so bands of width
// 48/80/80/48 rather than an even quartering of the range at 64/128/192. They
// are the mirror image (in the coverage domain, coverage = 255 - luma) of the
// panel-side curve documented in targets/esp32-s3-epaper-1.54/main/display.cpp
// `grayLevelFromLuma`, and exist for the same reason: glyph rim pixels cluster
// near the extremes, so quarter-width outer bands snap almost every antialiased
// edge to solid ink or to nothing and small text reads as if it were 1-bit.
// Narrow outer bands and wide middle ones pull those rim pixels into the grey
// levels, which is the whole point of having them. It also lands close to
// round-to-nearest of the four output values (boundaries 42.5/127.5/212.5), so
// there is no systematic darkening or lightening across a stroke.
//
// Consequences worth stating explicitly:
//   - A fully covered interior pixel (255) is always level 3. Stems stay solid,
//     and are never dithered — there is no dithering here at all, ordered or
//     diffused. Dithering a glyph rim scatters every letter into speckle on a
//     4-level panel (same reasoning as `kDitherToPanel` in that display.cpp).
//   - Levels 1 and 2 are therefore reachable only from partial coverage, i.e.
//     they are reserved for edge/rim pixels by construction.
//   - The 48 floor drops coverage below ~19% instead of promoting it to level 1
//     (85/255 = 33%), so faint spill outside a stroke does not fatten it.
function quantizeCoverageToLevels(coverage, width, height) {
  const levels = new Uint8Array(width * height)
  for (let i = 0; i < coverage.length; i += 1) {
    const value = coverage[i]
    levels[i] = value >= 208 ? 3 : value >= 128 ? 2 : value >= 48 ? 1 : 0
  }

  // Neighbourhood fixups, read from the pass above and written to a copy so the
  // result does not depend on scan order.
  const result = Uint8Array.from(levels)
  const levelAt = (x, y) => (x < 0 || y < 0 || x >= width || y >= height ? 0 : levels[y * width + x])
  const coverageAt = (x, y) => (x < 0 || y < 0 || x >= width || y >= height ? 0 : coverage[y * width + x])
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const index = y * width + x
      // Stem solidity: a pixel enclosed by fully-inked neighbours is interior,
      // whatever rounding did to its own coverage. Forcing it to level 3 stops
      // rasterizer noise from punching grey pinholes down the middle of a stem.
      if (
        levels[index] >= 2 &&
        coverageAt(x - 1, y) >= 208 &&
        coverageAt(x + 1, y) >= 208 &&
        coverageAt(x, y - 1) >= 208 &&
        coverageAt(x, y + 1) >= 208
      ) {
        result[index] = 3
        continue
      }
      // Speckle removal: a faint pixel with no ink at all around it is a stray
      // dot off a curve tip, not a stroke. On four levels it reads as dirt.
      if (levels[index] !== 1) continue
      let neighbours = 0
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dx = -1; dx <= 1; dx += 1) {
          if (dx !== 0 || dy !== 0) neighbours += levelAt(x + dx, y + dy)
        }
      }
      if (neighbours === 0) result[index] = 0
    }
  }
  return result
}

// Pack 0..3 levels four to a byte, rows byte-aligned (stride = ceil(width / 4)).
// The leftmost pixel of a byte occupies the most significant bit pair, so a hex
// dump of a row reads left to right like the glyph does. Kept in sync with
// `RasterizedFont::coverage` in packages/engine/rasterized_font.cpp.
function packLevelsTo2Bit(levels, width, height) {
  const stride = (width + 3) >> 2
  const packed = new Uint8Array(stride * height)
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const level = levels[y * width + x] & 0x3
      packed[y * stride + (x >> 2)] |= level << ((3 - (x & 3)) * 2)
    }
  }
  return packed
}

function rasterizeGlyph(cmds, bitmap, gw, gh, x0, y0) {
  const ss = glyphSupersample
  const ssWidth = gw * ss
  const ssHeight = gh * ss
  const hires = new Uint8Array(ssWidth * ssHeight)
  const segments = pathCommandsToSegments(cmds, x0, y0)
  const maxEdgesPerRow = 128
  const scanlines = new Float32Array(ssHeight * maxEdgesPerRow)
  const scanCounts = new Int32Array(ssHeight)

  for (const [sx0, sy0, sx1, sy1] of segments) {
    const hsy0 = sy0 * ss
    const hsy1 = sy1 * ss
    const hsx0 = sx0 * ss
    const hsx1 = sx1 * ss
    const minRow = Math.max(0, Math.floor(Math.min(hsy0, hsy1)))
    const maxRow = Math.min(ssHeight - 1, Math.ceil(Math.max(hsy0, hsy1)))
    for (let row = minRow; row <= maxRow; row += 1) {
      const y = row + 0.5
      if ((hsy0 <= y && hsy1 > y) || (hsy1 <= y && hsy0 > y)) {
        const pos = hsx0 + ((y - hsy0) / (hsy1 - hsy0)) * (hsx1 - hsx0)
        const count = scanCounts[row]
        if (count < maxEdgesPerRow) scanlines[row * maxEdgesPerRow + scanCounts[row]++] = pos
      }
    }
  }

  for (let row = 0; row < ssHeight; row += 1) {
    const count = scanCounts[row]
    if (count < 2) continue
    const edges = Array.from(scanlines.slice(row * maxEdgesPerRow, row * maxEdgesPerRow + count)).sort((a, b) => a - b)
    for (let i = 0; i < edges.length - 1; i += 2) {
      // At 1:1 (no supersampling) ink a pixel only when its CENTER falls inside
      // the span — the standard 50% coverage rule — so glyphs keep their true
      // weight. Rounding the span outward (floor/ceil) fattens every stroke by
      // up to a pixel per side and reads as bold. Supersampled paths keep the
      // outward rounding: it's averaged away on downsample and avoids dropping
      // thin sub-pixel coverage.
      const left = ss === 1
        ? Math.max(0, Math.ceil(edges[i] - 0.5))
        : Math.max(0, Math.floor(edges[i]))
      const right = ss === 1
        ? Math.min(ssWidth - 1, Math.floor(edges[i + 1] - 0.5))
        : Math.min(ssWidth - 1, Math.ceil(edges[i + 1]))
      for (let col = left; col <= right; col += 1) hires[row * ssWidth + col] = 1
    }
  }

  for (let py = 0; py < gh; py += 1) {
    for (let px = 0; px < gw; px += 1) {
      let sum = 0
      for (let sy = 0; sy < ss; sy += 1) {
        for (let sx = 0; sx < ss; sx += 1) sum += hires[(py * ss + sy) * ssWidth + (px * ss + sx)]
      }
      bitmap[py * gw + px] = Math.round((sum / (ss * ss)) * 255)
    }
  }
}

function pathCommandsToSegments(cmds, x0, y0) {
  const segments = []
  let curX = 0
  let curY = 0
  let startX = 0
  let startY = 0

  for (const cmd of cmds) {
    if (cmd.type === 'M') {
      curX = cmd.x - x0
      curY = cmd.y - y0
      startX = curX
      startY = curY
    } else if (cmd.type === 'L') {
      const endX = cmd.x - x0
      const endY = cmd.y - y0
      segments.push([curX, curY, endX, endY])
      curX = endX
      curY = endY
    } else if (cmd.type === 'C') {
      for (let s = 1; s <= 12; s += 1) {
        const t = s / 12
        const mt = 1 - t
        const endX = mt ** 3 * curX + 3 * mt * mt * t * (cmd.x1 - x0) + 3 * mt * t * t * (cmd.x2 - x0) + t ** 3 * (cmd.x - x0)
        const endY = mt ** 3 * curY + 3 * mt * mt * t * (cmd.y1 - y0) + 3 * mt * t * t * (cmd.y2 - y0) + t ** 3 * (cmd.y - y0)
        segments.push([curX, curY, endX, endY])
        curX = endX
        curY = endY
      }
    } else if (cmd.type === 'Q') {
      for (let s = 1; s <= 8; s += 1) {
        const t = s / 8
        const mt = 1 - t
        const endX = mt * mt * curX + 2 * mt * t * (cmd.x1 - x0) + t * t * (cmd.x - x0)
        const endY = mt * mt * curY + 2 * mt * t * (cmd.y1 - y0) + t * t * (cmd.y - y0)
        segments.push([curX, curY, endX, endY])
        curX = endX
        curY = endY
      }
    } else if (cmd.type === 'Z') {
      if (curX !== startX || curY !== startY) segments.push([curX, curY, startX, startY])
      curX = startX
      curY = startY
    }
  }

  return segments
}

function packGlyphAtlas(glyphs) {
  const padding = 1
  const maxAtlasWidth = 512
  let atlasWidth = 0
  let curX = 0
  let curY = 0
  let maxRowHeight = 0

  for (const glyph of glyphs) {
    if (curX + glyph.width + padding > maxAtlasWidth) {
      curX = 0
      curY += maxRowHeight + padding
      maxRowHeight = 0
    }
    glyph.sourceX = curX
    glyph.sourceY = curY
    if (curX + glyph.width > atlasWidth) atlasWidth = curX + glyph.width
    if (glyph.height > maxRowHeight) maxRowHeight = glyph.height
    curX += glyph.width + padding
  }

  return { atlasWidth, atlasHeight: curY + maxRowHeight }
}

function cppString(value) {
  return JSON.stringify(value)
}

function generatedHeader(fontCount, familyCount, options = {}) {
  const lines = [
    '#pragma once',
    '#define GEA_EMBEDDED_HAS_GENERATED_FONTS 1',
    '#include "graphics/font.h"',
    `#define GEA_EMBEDDED_FONT_COUNT ${fontCount}`,
    `#define GEA_EMBEDDED_FONT_FAMILY_COUNT ${familyCount}`,
  ]
  if (options.runtimeTtfFonts) lines.push('#define GEA_EMBEDDED_HAS_RUNTIME_TTF_FONTS 1')
  lines.push('')
  return lines.join('\n')
}

function fontFunctionNames() {
  if (!symbolPrefix) {
    return {
      namespaceOpen: 'namespace gea::framework::graphics::generated {',
      namespaceClose: '}  // namespace gea::framework::graphics::generated',
      ensureLinked: 'ensureLinked',
      lookupFont: 'lookupFont',
      lookupFontForFamily: 'lookupFontForFamily',
      lookupFontFamily: 'lookupFontFamily',
      lookupFontFamilyName: 'lookupFontFamilyName',
      lookupRuntimeTtfFontForFamily: 'lookupRuntimeTtfFontForFamily',
    }
  }
  return {
    namespaceOpen: '',
    namespaceClose: '',
    ensureLinked: `${symbolPrefix}_ensure_linked_fonts`,
    lookupFont: `${symbolPrefix}_lookup_font`,
    lookupFontForFamily: `${symbolPrefix}_lookup_font_for_family`,
    lookupFontFamily: `${symbolPrefix}_lookup_font_family`,
    lookupFontFamilyName: `${symbolPrefix}_lookup_font_family_name`,
    lookupRuntimeTtfFontForFamily: `${symbolPrefix}_lookup_runtime_ttf_font_for_family`,
  }
}

function emitEmptyFontLookup(lines, names) {
  lines.push(`void ${names.ensureLinked}()`)
  lines.push('{')
  lines.push('}')
  lines.push('')
  lines.push(`const gea::framework::graphics::RasterizedFontData *${names.lookupFont}(int fontId)`)
  lines.push('{')
  lines.push('    (void)fontId;')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  lines.push(`const gea::framework::graphics::RasterizedFontData *${names.lookupFontForFamily}(int familyId, int sizePx)`)
  lines.push('{')
  lines.push('    (void)familyId;')
  lines.push('    (void)sizePx;')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  lines.push(`int ${names.lookupFontFamily}(const char *family)`)
  lines.push('{')
  lines.push('    (void)family;')
  lines.push('    return -1;')
  lines.push('}')
  lines.push('')
  lines.push(`const char *${names.lookupFontFamilyName}(int familyId)`)
  lines.push('{')
  lines.push('    (void)familyId;')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
}

function generatedCpp(fonts, families) {
  const names = fontFunctionNames()
  const lines = [
    '#include "gea_embedded_font_generated.h"',
    '#include <cstring>',
    '',
  ]
  if (names.namespaceOpen) {
    lines.push(names.namespaceOpen)
    lines.push('')
  } else {
    lines.push('using gea::framework::graphics::Glyph;')
    lines.push('using gea::framework::graphics::RasterizedFontData;')
    lines.push('')
  }

  if (fonts.length === 0) {
    emitEmptyFontLookup(lines, names)
    if (names.namespaceClose) lines.push(names.namespaceClose)
    lines.push('')
    return lines.join('\n')
  }

  // Atlas bitmaps are the bulk of the font footprint and are frequently
  // byte-identical across isolated programs (same family + px size +
  // DPR). Naming each by a content hash and giving it weak (external) linkage —
  // instead of a per-app `static` symbol — lets the linker fold identical
  // copies into one, deduping the bundle with no runtime change. We dedup within
  // this TU too (a weak symbol can't be defined twice in one TU); `font_data`
  // points at the shared symbol either way.
  const atlasSymbols = new Map()
  for (const font of fonts) {
    const hash = createHash('sha1').update(Buffer.from(font.atlasData)).digest('hex').slice(0, 16)
    const symbol = `gea_font_atlas_${hash}`
    font.atlasSymbol = symbol
    if (atlasSymbols.has(hash)) continue
    atlasSymbols.set(hash, symbol)
    // `extern` + initializer: a namespace-scope `const` is internal by default,
    // and `weak` requires external linkage ("weak declaration must be public").
    lines.push(`extern __attribute__((weak)) const std::uint8_t ${symbol}[${font.atlasData.length}] = {`)
    for (let i = 0; i < font.atlasData.length; i += 32) {
      const chunk = Array.from(font.atlasData.slice(i, Math.min(i + 32, font.atlasData.length)))
      lines.push(`    ${chunk.map((byte) => `0x${byte.toString(16).padStart(2, '0')}`).join(', ')},`)
    }
    lines.push('};')
    lines.push('')
  }

  for (const font of fonts) {
    // `atlasBits` defaults to 8 in RasterizedFontData, so an 8-bit atlas leaves
    // the trailing initializer off entirely and keeps this TU byte-identical to
    // what every board generated before 2-bit atlases existed.
    const atlasBitsInit = font.atlasBits === 8 ? '' : `, ${font.atlasBits}`
    lines.push(`static const Glyph font_glyphs_${font.id}[${font.glyphs.length}] = {`)
    for (const glyph of font.glyphs) {
      lines.push(
        `    { ${glyph.codepoint}, ${glyph.sourceX}, ${glyph.sourceY}, ${glyph.width}, ${glyph.height}, ${glyph.advance}, ${glyph.bearingX}, ${glyph.bearingY} },`,
      )
    }
    lines.push('};')
    lines.push('')

    lines.push(
      `static const RasterizedFontData font_data_${font.id} = { ${font.id}, ${font.sizePx}, ${font.lineHeight}, ${font.ascender}, ${font.descender}, ${font.glyphs.length}, font_glyphs_${font.id}, ${font.atlasWidth}, ${font.atlasHeight}, ${font.atlasSymbol}${atlasBitsInit} };`,
    )
    lines.push('')
  }

  lines.push('struct GeneratedFontEntry {')
  lines.push('    int familyId;')
  lines.push('    const RasterizedFontData *font;')
  lines.push('};')
  lines.push('')
  lines.push('static const GeneratedFontEntry generated_fonts[] = {')
  for (const font of fonts) lines.push(`    { ${font.familyId}, &font_data_${font.id} },`)
  lines.push('};')
  lines.push('')
  lines.push('struct GeneratedFontFamily {')
  lines.push('    int id;')
  lines.push('    const char *name;')
  lines.push('};')
  lines.push('')
  lines.push('static const GeneratedFontFamily generated_families[] = {')
  for (const family of families) lines.push(`    { ${family.id}, ${cppString(family.name)} },`)
  lines.push('};')
  lines.push('')
  lines.push(`void ${names.ensureLinked}()`)
  lines.push('{')
  lines.push('}')
  lines.push('')
  lines.push(`const gea::framework::graphics::RasterizedFontData *${names.lookupFont}(int fontId)`)
  lines.push('{')
  lines.push('    for (const auto &entry : generated_fonts) {')
  lines.push('        if (entry.font && entry.font->id == fontId) return entry.font;')
  lines.push('    }')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  lines.push(`const gea::framework::graphics::RasterizedFontData *${names.lookupFontForFamily}(int familyId, int sizePx)`)
  lines.push('{')
  lines.push('    const RasterizedFontData *best = nullptr;')
  lines.push('    int bestDiff = 2147483647;')
  lines.push('    for (const auto &entry : generated_fonts) {')
  lines.push('        if (entry.familyId != familyId || !entry.font) continue;')
  lines.push('        int diff = sizePx > 0 ? entry.font->sizePx - sizePx : 0;')
  lines.push('        if (diff < 0) diff = -diff;')
  lines.push('        if (diff < bestDiff) {')
  lines.push('            best = entry.font;')
  lines.push('            bestDiff = diff;')
  lines.push('        }')
  lines.push('    }')
  lines.push('    return best;')
  lines.push('}')
  lines.push('')
  lines.push(`int ${names.lookupFontFamily}(const char *family)`)
  lines.push('{')
  lines.push('    if (!family) return -1;')
  lines.push('    for (const auto &entry : generated_families) {')
  lines.push('        if (std::strcmp(entry.name, family) == 0) return entry.id;')
  lines.push('    }')
  lines.push('    return -1;')
  lines.push('}')
  lines.push('')
  lines.push(`const char *${names.lookupFontFamilyName}(int familyId)`)
  lines.push('{')
  lines.push('    for (const auto &entry : generated_families) {')
  lines.push('        if (entry.id == familyId) return entry.name;')
  lines.push('    }')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  if (names.namespaceClose) lines.push(names.namespaceClose)
  lines.push('')
  return lines.join('\n')
}

function generatedRuntimeTtfCpp(runtimeFonts, families) {
  const names = fontFunctionNames()
  const lines = [
    '#include "gea_embedded_font_generated.h"',
    '#include "memory.h"',
    '#include <cstddef>',
    '#include <cstring>',
    '',
    'extern "C" int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);',
    '',
  ]
  if (names.namespaceOpen) {
    lines.push(names.namespaceOpen)
    lines.push('')
  } else {
    lines.push('using gea::framework::graphics::RasterizedFontData;')
    lines.push('')
  }

  for (const font of runtimeFonts) {
    const symbol = `gea_runtime_ttf_${font.id}`
    const compressed = zlib.deflateSync(font.data, { level: 9 })
    font.byteSymbol = `${symbol}_deflated`
    font.byteLengthSymbol = `${symbol}_deflated_len`
    font.uncompressedLength = font.data.length
    lines.push(`extern __attribute__((weak)) const std::uint8_t ${font.byteSymbol}[${compressed.length}] = {`)
    for (let i = 0; i < compressed.length; i += 32) {
      const chunk = Array.from(compressed.slice(i, Math.min(i + 32, compressed.length)))
      lines.push(`    ${chunk.map((byte) => `0x${byte.toString(16).padStart(2, '0')}`).join(', ')},`)
    }
    lines.push('};')
    lines.push(`extern __attribute__((weak)) const unsigned long ${font.byteLengthSymbol} = ${compressed.length}UL;`)
    lines.push(`extern __attribute__((weak)) const unsigned long ${symbol}_len = ${font.data.length}UL;`)
    lines.push('')
  }

  lines.push('struct GeneratedFontFamily {')
  lines.push('    int id;')
  lines.push('    const char *name;')
  lines.push('};')
  lines.push('')
  lines.push('static const GeneratedFontFamily generated_families[] = {')
  for (const family of families) lines.push(`    { ${family.id}, ${cppString(family.name)} },`)
  lines.push('};')
  lines.push('')
  lines.push('struct RuntimeTtfFontEntry {')
  lines.push('    int familyId;')
  lines.push('    const std::uint8_t *compressedBytes;')
  lines.push('    const unsigned long *compressedLength;')
  lines.push('    unsigned long uncompressedLength;')
  lines.push('    std::uint8_t *decodedBytes;')
  lines.push('};')
  lines.push('')
  lines.push('static RuntimeTtfFontEntry runtime_ttf_fonts[] = {')
  for (const font of runtimeFonts) {
    lines.push(`    { ${font.familyId}, ${font.byteSymbol}, &${font.byteLengthSymbol}, ${font.uncompressedLength}UL, nullptr },`)
  }
  lines.push('};')
  lines.push('')
  lines.push('std::uint8_t *decodeRuntimeTtfFont(RuntimeTtfFontEntry &entry)')
  lines.push('{')
  lines.push('    if (entry.decodedBytes) return entry.decodedBytes;')
  lines.push('    if (!entry.compressedBytes || !entry.compressedLength || *entry.compressedLength == 0UL || entry.uncompressedLength == 0UL) return nullptr;')
  lines.push('    auto *decoded = static_cast<std::uint8_t *>(gea::framework::memory::Allocator::allocatePreferSpiram(entry.uncompressedLength, alignof(std::max_align_t)));')
  lines.push('    if (!decoded) return nullptr;')
  lines.push('    const int written = stbi_zlib_decode_buffer(reinterpret_cast<char *>(decoded),')
  lines.push('        static_cast<int>(entry.uncompressedLength),')
  lines.push('        reinterpret_cast<const char *>(entry.compressedBytes),')
  lines.push('        static_cast<int>(*entry.compressedLength));')
  lines.push('    if (written != static_cast<int>(entry.uncompressedLength)) {')
  lines.push('        gea::framework::memory::Allocator::free(decoded);')
  lines.push('        return nullptr;')
  lines.push('    }')
  lines.push('    entry.decodedBytes = decoded;')
  lines.push('    return entry.decodedBytes;')
  lines.push('}')
  lines.push('')
  lines.push(`void ${names.ensureLinked}()`)
  lines.push('{')
  lines.push('}')
  lines.push('')
  lines.push(`const gea::framework::graphics::RasterizedFontData *${names.lookupFont}(int fontId)`)
  lines.push('{')
  lines.push('    (void)fontId;')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  lines.push(`const gea::framework::graphics::RasterizedFontData *${names.lookupFontForFamily}(int familyId, int sizePx)`)
  lines.push('{')
  lines.push('    (void)familyId;')
  lines.push('    (void)sizePx;')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  lines.push(`int ${names.lookupFontFamily}(const char *family)`)
  lines.push('{')
  lines.push('    if (!family) return -1;')
  lines.push('    for (const auto &entry : generated_families) {')
  lines.push('        if (std::strcmp(entry.name, family) == 0) return entry.id;')
  lines.push('    }')
  lines.push('    return -1;')
  lines.push('}')
  lines.push('')
  lines.push(`const char *${names.lookupFontFamilyName}(int familyId)`)
  lines.push('{')
  lines.push('    for (const auto &entry : generated_families) {')
  lines.push('        if (entry.id == familyId) return entry.name;')
  lines.push('    }')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  lines.push(`const std::uint8_t *${names.lookupRuntimeTtfFontForFamily}(int familyId, unsigned long *length)`)
  lines.push('{')
  lines.push('    for (auto &entry : runtime_ttf_fonts) {')
  lines.push('        if (entry.familyId != familyId) continue;')
  lines.push('        if (length) *length = entry.uncompressedLength;')
  lines.push('        return decodeRuntimeTtfFont(entry);')
  lines.push('    }')
  lines.push('    if (length) *length = 0UL;')
  lines.push('    return nullptr;')
  lines.push('}')
  lines.push('')
  if (names.namespaceClose) lines.push(names.namespaceClose)
  lines.push('')
  return lines.join('\n')
}

// These TUs are megabytes of generated byte arrays that no human reads, and
// clang-format is quadratic enough on them to cost minutes of CPU on a first
// build (15 CPU-minutes on a 2.8 MB asset TU, on every fresh clone and every
// CI runner, because the stamp below only helps a machine that already paid
// it once). Formatting is therefore opt-in: set GEATSC_FORMAT_CPP=1 when the
// generated output is being read by a person.
const CPP_FORMAT_EXTENSIONS = /\.(?:c|cc|cpp|cxx|m|mm|h|hh|hpp|hxx)$/i
const CPP_FORMAT_STYLE = '{BasedOnStyle: LLVM, SortIncludes: false, ColumnLimit: 160}'
let clangFormatUnavailable = false

function formatCppSourceIfNeeded(filePath, contents) {
  if (!CPP_FORMAT_EXTENSIONS.test(filePath)) return contents
  if (process.env.GEATSC_FORMAT_CPP !== '1') return contents
  if (clangFormatUnavailable) return contents
  const clangFormat = process.env.CLANG_FORMAT || 'clang-format'
  try {
    return execFileSync(clangFormat, [`--style=${CPP_FORMAT_STYLE}`, `--assume-filename=${filePath}`], {
      input: contents,
      encoding: 'utf8',
      maxBuffer: 512 * 1024 * 1024
    })
  } catch (error) {
    if (error?.code === 'ENOENT') {
      clangFormatUnavailable = true
      process.stderr.write('generate-gea-embedded-fonts: clang-format not found; generated C++ will be written unformatted\n')
      return contents
    }
    throw error
  }
}

// clang-format on a multi-megabyte generated font TU costs seconds, and the
// result is thrown away on every build where the font set did not change (the
// common case: the .cpp is regenerated byte-identically). Formatting BEFORE
// comparing paid that cost unconditionally.
//
// The invariant "what lands on disk is formatted" is preserved by proving the
// formatter cannot produce anything new, not by skipping it: a sidecar stamp
// records the hash of the UNFORMATTED input, the hash of the formatted bytes
// that input produced, and the formatter identity (binary + version + style).
// When the current input hashes to the recorded raw hash, the formatter is the
// same, and the file on disk still hashes to the recorded formatted hash, then
// re-running clang-format would reproduce exactly the bytes already there.
// Anything else (missing or stale stamp, edited file, different clang-format)
// falls back to the original format-then-compare path.
const FORMAT_STAMP_VERSION = 1
let clangFormatIdentity

function sha256(value) {
  return createHash('sha256').update(value, typeof value === 'string' ? 'utf8' : undefined).digest('hex')
}

function formatterIdentity() {
  if (clangFormatIdentity !== undefined) return clangFormatIdentity
  const clangFormat = process.env.CLANG_FORMAT || 'clang-format'
  try {
    const version = execFileSync(clangFormat, ['--version'], { encoding: 'utf8' }).trim()
    clangFormatIdentity = `${clangFormat} ${version} ${CPP_FORMAT_STYLE}`
  } catch {
    clangFormatIdentity = ''
  }
  return clangFormatIdentity
}

function formatStampPath(filePath) {
  return path.join(path.dirname(filePath), `.${path.basename(filePath)}.fmtstamp`)
}

function writeFileIfChanged(filePath, contents) {
  const formats = CPP_FORMAT_EXTENSIONS.test(filePath) && process.env.GEATSC_FORMAT_CPP === '1' && !clangFormatUnavailable
  if (!formats) {
    if (fs.existsSync(filePath) && fs.readFileSync(filePath, 'utf8') === contents) return false
    fs.mkdirSync(path.dirname(filePath), { recursive: true })
    fs.writeFileSync(filePath, contents)
    return true
  }

  const stampPath = formatStampPath(filePath)
  const rawHash = sha256(contents)
  const identity = formatterIdentity()
  if (identity) {
    let stamp
    try {
      stamp = JSON.parse(fs.readFileSync(stampPath, 'utf8'))
    } catch {
      stamp = undefined
    }
    if (stamp?.version === FORMAT_STAMP_VERSION && stamp.raw === rawHash && stamp.identity === identity) {
      try {
        if (sha256(fs.readFileSync(filePath)) === stamp.formatted) return false
      } catch {
        // fall through and format
      }
    }
  }

  const formattedContents = formatCppSourceIfNeeded(filePath, contents)
  const changed = !fs.existsSync(filePath) || fs.readFileSync(filePath, 'utf8') !== formattedContents
  if (changed) {
    fs.mkdirSync(path.dirname(filePath), { recursive: true })
    fs.writeFileSync(filePath, formattedContents)
  }
  if (identity) {
    try {
      fs.writeFileSync(
        stampPath,
        JSON.stringify({ version: FORMAT_STAMP_VERSION, raw: rawHash, formatted: sha256(formattedContents), identity })
      )
    } catch {
      // a stamp we cannot write only costs the next build the formatting pass
    }
  }
  return changed
}

function writeGeneratedFonts() {
  const fontFaces = collectSourceFontFaces()
  const tuples = collectUsedFontTuples(fontFaces)
  const familyNames = [...new Set(tuples.map((tuple) => tuple.family))].sort()
  const families = familyNames.map((name, id) => ({ id, name }))
  const familyIds = new Map(families.map((family) => [family.name, family.id]))

  let fonts = []
  let runtimeFonts = []
  if (runtimeTtfFonts) {
    runtimeFonts = families.map((family) => {
      const fontPath = fontFaces.get(family.name)
      if (!fontPath) fail(`missing @font-face src for font family ${family.name}`)
      return { id: family.id, familyId: family.id, family: family.name, data: fs.readFileSync(fontPath) }
    })
  } else if (tuples.length > 0) {
    const opentype = loadOpentype()
    let fontId = 0
    fonts = tuples.map((tuple) => {
      const fontPath = fontFaces.get(tuple.family)
      if (!fontPath) fail(`missing @font-face src for font family ${tuple.family}`)
      return rasterizeFont(opentype, fontPath, tuple.family, familyIds.get(tuple.family), tuple.sizePx, fontId++, tuple.charset)
    })
  }

  writeFileIfChanged(outH, generatedHeader(fonts.length, families.length, { runtimeTtfFonts }))
  writeFileIfChanged(outCpp, runtimeTtfFonts ? generatedRuntimeTtfCpp(runtimeFonts, families) : generatedCpp(fonts, families))
}

writeGeneratedFonts()
