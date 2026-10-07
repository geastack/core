#!/usr/bin/env node
// Scans an app directory for binary assets (images/fonts/audio) and emits a C++ translation
// unit that embeds each one as a named, externally-visible `.rodata` symbol.
//
// This is the build half of "store images as files, read them as files": the
// PNG stays a real file in the app folder, and instead of a hand-written byte
// array in the JS source, the bytes are linked into the app binary (the ESP32
// app partition; the macOS executable; the WASM module — the web build runs
// with NO_FILESYSTEM=1, so compiled-in bytes are the only option).
//
// Each asset is exported as `gea_asset_<sanitized-path>` + a matching `_len`,
// with the SAME public-path normalization and sanitization the codegen uses (see
// canonicalAssetPath() / assetSymbol() here and in
// packages/geatsc-plugin-gea/src/cpp-mounted-lowering.ts). An `<img src="x.png">`
// is lowered to a direct reference to that symbol. That reference is what pulls
// this object into the link: ESP-IDF links framework code from static archives,
// and an archive member with no referenced symbol (e.g. a static-init-only TU)
// is DISCARDED — so registration via a global constructor silently never runs
// on-device. A referenced data symbol is kept on every toolchain.
//
// Symbols are `weak` so several apps' asset TUs can be linked together (the
// shared binary) without duplicate-definition errors when they share
// an asset filename; the linker keeps one copy.

import { describeStaticAsset } from './gea-static-asset.mjs'
import { compileTimeOnlyAssetPaths } from './gea-embedded-asset-policy.mjs'
import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { fileURLToPath } from 'node:url'

const args = process.argv.slice(2)

function readOption(name) {
  const index = args.indexOf(name)
  return index >= 0 ? args[index + 1] : undefined
}

function fail(message) {
  process.stderr.write(`${message}\n`)
  process.exit(1)
}

const appDir = path.resolve(readOption('--app-dir') ?? fail('missing --app-dir <dir>'))
const outCpp = path.resolve(readOption('--out-cpp') ?? fail('missing --out-cpp <file>'))
const manifestPath = path.join(appDir, 'package.json')
const compileTimeOnly = compileTimeOnlyAssetPaths(
  fs.existsSync(manifestPath) ? JSON.parse(fs.readFileSync(manifestPath, 'utf8')) : {},
)

for (const relative of compileTimeOnly) {
  const file = path.join(appDir, relative)
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    fail(`compile-time-only asset is not a file: ${relative}`)
  }
}

const ASSET_EXTENSIONS = new Set(['.png', '.jpg', '.jpeg', '.gif', '.ttf', '.otf', '.wav'])
// Launcher/package icons are metadata, not runtime `<Image>` assets. If an app
// wants to render one, keep a copy under `public/` so the reference is explicit.
const PRUNE_DIRS = new Set(['node_modules', 'dist', 'build'])
// Hidden directories are never an app's assets. They are where tools keep their
// own state -- .git, .vite, .gea, an editor's cache, a browser automation run's
// screenshots -- and an app cannot reference what is inside one anyway, because
// nothing writes an <Image src=".something/...">. Scanning them once put three
// megabytes of a test tool's PNGs into a firmware image, where they cost flash
// and, because flash rodata and PSRAM share one address window on the ESP32-S3,
// three megabytes of addressable PSRAM with it.

// Keep this identical to canonicalAssetPath() / assetSymbol() in
// packages/geatsc-plugin-gea/src/cpp-mounted-lowering.ts.
function canonicalAssetPath(relPath) {
  const normalized = relPath.startsWith('/') ? relPath.slice(1) : relPath
  return normalized.startsWith('public/') ? normalized.slice('public/'.length) : normalized
}

function assetSymbol(relPath) {
  return `gea_asset_${canonicalAssetPath(relPath).replace(/[^A-Za-z0-9]/g, '_')}`
}

function collectAssets(dir) {
  const found = []
  const walk = (current) => {
    let entries
    try {
      entries = fs.readdirSync(current, { withFileTypes: true })
    } catch {
      return
    }
    for (const entry of entries) {
      const full = path.join(current, entry.name)
      if (entry.isDirectory()) {
        const rel = path.relative(appDir, full).split(path.sep).join('/')
        if (!PRUNE_DIRS.has(entry.name) && !entry.name.startsWith('.') && rel !== 'icons') walk(full)
      } else if (entry.isFile() && ASSET_EXTENSIONS.has(path.extname(entry.name).toLowerCase())) {
        if (!compileTimeOnly.has(relPath(full))) {
          found.push(full)
        }
      }
    }
  }
  walk(dir)
  found.sort()
  return found
}

function relPath(full) {
  return path.relative(appDir, full).split(path.sep).join('/')
}

function byteArrayBody(buffer) {
  const lines = []
  const perLine = 16
  for (let i = 0; i < buffer.length; i += perLine) {
    const chunk = []
    for (let j = i; j < Math.min(i + perLine, buffer.length); j++) chunk.push(buffer[j])
    lines.push('  ' + chunk.join(', ') + (i + perLine < buffer.length ? ',' : ''))
  }
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
      process.stderr.write('generate-gea-embedded-assets: clang-format not found; generated C++ will be written unformatted\n')
      return contents
    }
    throw error
  }
}

// Formatting an embedded-asset TU (megabytes of byte arrays) and then throwing
// the result away because nothing changed is pure waste, so prove the formatter
// cannot produce anything new instead of running it. A sidecar stamp records the
// hash of the UNFORMATTED input, the hash of the formatted bytes it produced and
// the formatter identity; when all three still hold, the file on disk already IS
// the formatter's output. See the identical guard in
// generate-gea-embedded-fonts.mjs.
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

const assets = collectAssets(appDir)

const parts = []
parts.push('// Generated by scripts/generate-gea-embedded-assets.mjs. Do not edit by hand.')
parts.push(`// app: ${relPath(appDir) || path.basename(appDir)}`)
parts.push('')
parts.push('#include <cstring>')
parts.push('')

if (assets.length === 0) {
  parts.push('// No embedded assets for this app.')
  // Still define gea_embedded_asset_lookup (always false): host/image.cpp declares
  // it __attribute__((weak)), which is a weak *reference* on GCC/Xtensa (resolves to
  // null when undefined) but NOT on Apple's ld — there an undefined weak symbol is a
  // hard link error. Defining it unconditionally keeps no-asset apps linking on macOS/iOS.
  parts.push('extern "C" __attribute__((weak)) bool gea_embedded_asset_lookup(const char *, const unsigned char **, unsigned long *) {')
  parts.push('  return false;')
  parts.push('}')
  parts.push('')
} else {
  // Symbols are defined with C linkage at global scope. The codegen references
  // them from inside `namespace gea_ir`, but when a program uses isolated symbols
  // inside a bundle (e.g. the launcher) geatsc wraps that whole program — and
  // its `gea_ir` — in an anonymous namespace. A C++ `extern` would then resolve
  // to `(anonymous namespace)::gea_ir::gea_asset_<path>` (internal linkage),
  // which no separate TU can define. `extern "C"` gives each asset one flat,
  // unmangled symbol that the reference resolves to identically whether its
  // enclosing `gea_ir` is global or anonymous-namespace-nested. Keep this in
  // sync with imageSrcAssetLines() / the forward decls in
  // packages/geatsc-plugin-gea/src/cpp-mounted-lowering.ts and cpp-ir.ts.
  //
  // `extern "C"` + initializer gives the arrays external linkage (a plain
  // `const` at namespace scope would be internal and invisible to the
  // reference). `weak` lets duplicate asset filenames across bundled apps (the
  // shared binary) link without a duplicate-definition error.
  const seen = new Set()
  for (const file of assets) {
    const rel = relPath(file)
    const sym = assetSymbol(rel)
    if (seen.has(sym)) {
      process.stderr.write(`generate-gea-embedded-assets: WARNING duplicate asset symbol ${sym} for ${rel}; skipping\n`)
      continue
    }
    seen.add(sym)
    const buffer = fs.readFileSync(file)
    parts.push(`// ${rel} (${buffer.length} bytes)`)
    // `extern "C"` on the definition: a namespace-scope `const` defaults to
    // INTERNAL linkage, and a `weak` symbol must be public — GCC (Xtensa)
    // rejects `weak` on an internal const. `extern "C"` + initializer = a weak
    // definition with external, unmangled (namespace-independent) linkage,
    // which both GCC and Clang accept.
    parts.push(`extern "C" __attribute__((weak)) const unsigned char ${sym}[] = {`)
    parts.push(byteArrayBody(buffer))
    parts.push('};')
    parts.push(`extern "C" __attribute__((weak)) const unsigned long ${sym}_len = ${buffer.length}UL;`)
    parts.push('')
  }
  parts.push('extern "C" __attribute__((weak)) bool gea_embedded_asset_lookup(const char *path, const unsigned char **data, unsigned long *length) {')
  parts.push('  if (!path || !data || !length) return false;')
  for (const file of assets) {
    const rel = relPath(file)
    const sym = assetSymbol(rel)
    const aliases = [rel, describeStaticAsset(file).url]
    if (rel.startsWith('public/')) aliases.push(rel.slice('public/'.length))
    const condition = aliases
      .map((alias) => `(std::strcmp(path, ${JSON.stringify(alias)}) == 0 || (path[0] == '/' && std::strcmp(path + 1, ${JSON.stringify(alias)}) == 0))`)
      .join(' || ')
    parts.push(`  if (${condition}) {`)
    parts.push(`    *data = ${sym};`)
    parts.push(`    *length = ${sym}_len;`)
    parts.push('    return true;')
    parts.push('  }')
  }
  parts.push('  return false;')
  parts.push('}')
  parts.push('')
}

writeFileIfChanged(outCpp, parts.join('\n'))
process.stderr.write(`generate-gea-embedded-assets: ${assets.length} asset(s) -> ${path.relative(process.cwd(), outCpp)}\n`)
