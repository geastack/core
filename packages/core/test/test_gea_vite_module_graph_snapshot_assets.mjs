import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import test from 'node:test'
import { fileURLToPath } from 'node:url'
import { build } from 'vite'

import { geaModuleGraphPlugins } from '../scripts/gea-vite-module-graph-plugin.mjs'

test('original snapshot asset bindings survive transforms without resurrecting unreachable assets', async () => {
  const testDir = path.dirname(fileURLToPath(import.meta.url))
  const outDir = path.join(testDir, '.build/module-graph')
  const entry = fileURLToPath(import.meta.url)
  const unreachable = path.join(testDir, 'test_embedded_asset_policy.mjs')
  const image = fs.realpathSync(path.join(testDir, 'fixtures/native-jpeg.jpg'))
  const text = fs.realpathSync(path.join(testDir, '../package.json'))
  const deadText = fs.realpathSync(path.join(testDir, '../LICENSE'))
  const plugins = geaModuleGraphPlugins({ outDir, entryReachableOnly: true })
  for (const plugin of plugins) plugin.configResolved?.({ root: testDir })

  const context = {
    async resolve(specifier, importer, options) {
      if (options?.skipSelf !== true) throw new Error('unexpected recursive resolution')
      const asset = await plugins[0].resolveId.call({
        resolve: async (source) => ({ id: path.resolve(path.dirname(importer), source) }),
      }, specifier, importer)
      return { id: asset ?? path.resolve(path.dirname(importer), specifier) }
    },
    getModuleIds: () => [entry, unreachable],
    getModuleInfo: (id) => id === entry ? { isEntry: true, importedIds: [], moduleSideEffects: false } : null,
  }
  const original = "import image from './fixtures/native-jpeg.jpg'\nimport text from '../package.json?raw'\nexport const result = image + text\n"
  plugins[1].transform(original, entry)
  // A component transform consumed both bindings. Rollup sees neither module;
  // geatsc still compiles the original snapshot and must resolve both imports.
  await plugins[2].transform.call(context, 'export const result = 1\n', entry)
  plugins[1].transform("import dead from '../LICENSE?raw'\nexport const result = dead\n", unreachable)
  await plugins[2].transform.call(context, 'export const result = 0\n', unreachable)

  const writes = new Map()
  const originals = { rmSync: fs.rmSync, mkdirSync: fs.mkdirSync, writeFileSync: fs.writeFileSync }
  const output = (name) => {
    assert.ok(name === outDir || name.startsWith(outDir + path.sep), 'only graph output is intercepted')
  }
  try {
    fs.rmSync = output
    fs.mkdirSync = output
    fs.writeFileSync = (name, contents) => { output(name); writes.set(name, contents) }
    plugins[2].generateBundle.call(context, {}, { entry: { type: 'chunk', modules: { [entry]: {} } } })
  } finally {
    Object.assign(fs, originals)
  }

  const graph = JSON.parse(writes.get(path.join(outDir, 'gea-module-graph.json')))
  const imageModule = graph.modules.find(module => module.assetSource === image)
  const rawModule = graph.modules.find(module => module.id === text + '.gearawmodule.js')
  assert.ok(imageModule, 'unloaded asset module must remain compiler-visible')
  assert.ok(rawModule, 'unloaded raw module must remain compiler-visible')
  assert.equal(writes.get(path.join(outDir, imageModule.originalSource)).trim(), `export default ${JSON.stringify(imageModule.assetUrl)}`)
  assert.equal(writes.get(path.join(outDir, rawModule.originalSource)).trim(), `export default ${JSON.stringify(fs.readFileSync(text, 'utf8'))}`)
  assert.ok(!graph.modules.some(module => module.id === deadText + '.gearawmodule.js'), 'unreachable original bindings remain pruned')
  const source = graph.modules.find(module => module.id === entry)
  assert.equal(source.imports.length, 2)
  assert.ok(source.imports.every(imported => imported.tracked && graph.modules.some(module => module.id === imported.resolvedId)))
})

test('Vite preserves a native asset binding consumed before Rollup loads its module', async () => {
  const entry = fileURLToPath(import.meta.url)
  const root = path.dirname(entry)
  const outDir = path.join(root, '.build/module-graph')
  const graphPlugins = geaModuleGraphPlugins({ outDir, entryReachableOnly: true })
  const writes = new Map()
  const originals = { rmSync: fs.rmSync, mkdirSync: fs.mkdirSync, writeFileSync: fs.writeFileSync }
  const output = (name) => assert.ok(name === outDir || name.startsWith(outDir + path.sep))
  try {
    fs.rmSync = output
    fs.mkdirSync = output
    fs.writeFileSync = (name, contents) => { output(name); writes.set(name, contents) }
    await build({
      root, configFile: false, logLevel: 'silent',
      plugins: [
        { name: 'snapshot-fixture-source', load: (id) => id === entry ? "import image from './fixtures/native-jpeg.jpg'\nexport const result = image\n" : null },
        graphPlugins[0], graphPlugins[1],
        { name: 'framework-consumes-asset', enforce: 'pre', transform: (_code, id) => id === entry ? 'export const result = 1\n' : null },
        graphPlugins[2],
      ],
      build: { write: false, lib: { entry, formats: ['es'] }, outDir, emptyOutDir: false },
    })
  } finally {
    Object.assign(fs, originals)
  }
  const graph = JSON.parse(writes.get(path.join(outDir, 'gea-module-graph.json')))
  assert.equal(graph.assets.length, 1)
  const source = graph.modules.find(module => module.id === entry)
  assert.equal(source.imports.length, 1)
  assert.equal(source.imports[0].resolvedId, graph.assets[0].moduleId)
  assert.match(writes.get(path.join(outDir, source.originalSource)), /import image/)
  assert.doesNotMatch(writes.get(path.join(outDir, source.transformedSource)), /import image/)
})
