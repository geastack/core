import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import test from 'node:test'
import { fileURLToPath } from 'node:url'
import { build } from 'vite'

import { geaModuleGraphPlugins } from '../scripts/gea-vite-module-graph-plugin.mjs'

const testDir = path.dirname(fileURLToPath(import.meta.url))
const fixtureDir = fs.realpathSync(path.join(testDir, 'fixtures/node-env'))
const outDir = path.join(testDir, '.build/module-graph-node-env')
const entry = path.join(fixtureDir, 'index.ts')

// The embedded driver builds in Vite's lib mode, which leaves process.env.NODE_ENV
// to a later bundler. geatsc compiles each module from disk and is that last
// consumer, so the graph states the disk text with the define applied.
async function graphOf(define) {
  const writes = new Map()
  const originals = { rmSync: fs.rmSync, mkdirSync: fs.mkdirSync, writeFileSync: fs.writeFileSync }
  const output = (name) => assert.ok(name === outDir || name.startsWith(outDir + path.sep), 'only graph output is intercepted')
  try {
    fs.rmSync = output
    fs.mkdirSync = output
    fs.writeFileSync = (name, contents) => { output(name); writes.set(name, contents) }
    await build({
      root: fixtureDir, configFile: false, logLevel: 'silent', ...(define ? { define } : {}),
      plugins: geaModuleGraphPlugins({ outDir, entryReachableOnly: true }),
      build: { write: false, lib: { entry, formats: ['es'] }, outDir, emptyOutDir: false },
    })
  } finally {
    Object.assign(fs, originals)
  }
  const graph = JSON.parse(writes.get(path.join(outDir, 'gea-module-graph.json')))
  const moduleOf = (name) => graph.modules.find((module) => module.id === path.join(fixtureDir, name))
  const read = (relative) => {
    assert.ok(relative, 'the graph states a source for this module')
    return writes.get(path.join(outDir, relative))
  }
  return { moduleOf, read }
}

test('a module that reads NODE_ENV is stated with the value an app build bakes in', async () => {
  const { moduleOf, read } = await graphOf()
  const ts = read(moduleOf('index.ts').definedSource)
  // Typed as string, as @types/node types it: a bare literal would make the
  // checker reject `=== 'development'` as a comparison with no overlap.
  assert.match(ts, /if \(\("production" as string\) !== 'production'\)/)
  assert.match(ts, /development = \("production" as string\) === 'development'/)
  assert.match(ts, /fromGlobal = \("production" as string\)/)
  assert.match(ts, /text = 'process\.env\.NODE_ENV'/, 'a string is not a reference')
  assert.match(ts, /return process\.env\.NODE_ENV/, 'a local binding shadows the global')
  assert.match(ts, /process\.env\.NODE_ENV = 'test'/, 'a write is left to fail as written')
  assert.equal(ts.split('\n').length, fs.readFileSync(entry, 'utf8').split('\n').length, 'every line keeps its number')
  assert.match(read(moduleOf('mode.js').definedSource), /jsMode = \/\*\* @type \{string\} \*\/ \("production"\)/)
  assert.equal(moduleOf('plain.ts').definedSource, undefined, 'a module with no such read is compiled from disk as before')
  assert.match(read(moduleOf('index.ts').originalSource), /if \(process\.env\.NODE_ENV !== 'production'\)/, 'the snapshots stay the record of Vite')
})

test('a define for NODE_ENV wins, as it does in the bundle', async () => {
  const { moduleOf, read } = await graphOf({ 'process.env.NODE_ENV': JSON.stringify('development') })
  assert.match(read(moduleOf('index.ts').definedSource), /if \(\("development" as string\) !== 'production'\)/)
})
