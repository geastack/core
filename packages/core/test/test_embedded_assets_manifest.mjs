import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const core = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const fixture = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-assets-manifest-'))

try {
  // Dropping file storage must not leave the previous deployment manifest for
  // deploy tooling to push.
  const app = path.join(fixture, 'app')
  const outCpp = path.join(fixture, 'images.cpp')
  fs.mkdirSync(path.join(app, 'public'), { recursive: true })
  fs.writeFileSync(path.join(app, 'public', 'pixel.png'), 'x')
  const generate = (gea) => {
    fs.writeFileSync(path.join(app, 'package.json'), JSON.stringify({ gea }))
    execFileSync(process.execPath, [path.join(core, 'scripts', 'generate-gea-embedded-assets.mjs'), '--app-dir', app, '--out-cpp', outCpp], {
      stdio: 'pipe'
    })
  }
  const windows = process.platform === 'win32'
  const store = path.join(fixture, 'store')
  const directory = windows ? '/' + store.slice(path.parse(store).root.length).replaceAll('\\', '/') : store
  generate({ id: 'manifest-test', embeddedAssets: { storage: { type: 'file', directory } } })
  assert.ok(fs.existsSync(outCpp + '.assets.json'), 'a storage-backed build writes its manifest')
  generate({ id: 'manifest-test' })
  assert.ok(!fs.existsSync(outCpp + '.assets.json'), 'a build without storage removes the stale manifest')

  console.log('Embedded assets manifest: a build without file storage removes the stale deployment manifest')
} finally {
  fs.rmSync(fixture, { recursive: true, force: true })
}
