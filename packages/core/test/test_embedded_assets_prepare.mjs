import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// gea_app_entry.cpp references gea_embedded_assets_prepare, and Apple's ld rejects a
// weak reference nothing defines, so the generated assets unit has to define it for
// every app. Link the real entry object with each kind of generated unit and check
// that the hook resolves. lld-link has no relocatable link, so COFF is covered by the
// windows target's hook-defaults test instead.
if (process.platform === 'win32') {
  console.log('Embedded assets prepare: skipped on Windows (no relocatable link)')
  process.exit(0)
}

const core = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const packages = path.dirname(core)
const fixture = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-assets-prepare-'))
const cxx = process.env.CXX || 'clang++'
const includes = ['core/include', 'core', 'host/include', 'host', 'engine', 'engine/ui', 'elements', 'elements/ui', 'geaos'].map(
  (dir) => '-I' + path.join(packages, dir)
)

try {
  const entry = path.join(fixture, 'entry.o')
  execFileSync(cxx, ['-std=c++20', '-O2', '-c', path.join(core, 'gea_app_entry.cpp'), ...includes, '-o', entry], { stdio: 'inherit' })

  const linked = (name, gea, asset) => {
    const app = path.join(fixture, name)
    fs.mkdirSync(path.join(app, 'public'), { recursive: true })
    if (asset) fs.writeFileSync(path.join(app, 'public', 'pixel.png'), 'x')
    fs.writeFileSync(path.join(app, 'package.json'), JSON.stringify({ gea }))
    const cpp = path.join(fixture, name + '.cpp')
    execFileSync(process.execPath, [path.join(core, 'scripts', 'generate-gea-embedded-assets.mjs'), '--app-dir', app, '--out-cpp', cpp], {
      stdio: 'pipe'
    })
    const unit = path.join(fixture, name + '.o')
    execFileSync(cxx, ['-std=c++20', '-O2', '-c', cpp, ...includes, '-o', unit], { stdio: 'inherit' })
    const combined = path.join(fixture, name + '-linked.o')
    execFileSync(cxx, ['-r', entry, unit, '-o', combined], { stdio: 'inherit' })
    const symbols = execFileSync(process.env.NM || 'nm', [combined], { encoding: 'utf8' })
    return symbols.split('\n').find((line) => /\b_?gea_embedded_assets_prepare$/.test(line))
  }

  for (const [name, gea, asset] of [
    ['no-storage', { id: 'no-storage' }, true],
    ['no-assets', { id: 'no-assets' }, false],
    ['file-backed', { id: 'file-backed', embeddedAssets: { storage: { type: 'file', directory: '/nand/assets' } } }, true]
  ]) {
    const prepare = linked(name, gea, asset)
    assert.ok(prepare, `${name}: the linked object names gea_embedded_assets_prepare`)
    assert.doesNotMatch(prepare, /\s[Uwv]\s/, `${name}: gea_app_entry's reference resolves: ${prepare.trim()}`)
  }

  console.log('Embedded assets prepare: entry links against no-storage, no-asset and file-backed generated units')
} finally {
  fs.rmSync(fixture, { recursive: true, force: true })
}
