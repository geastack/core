import assert from 'node:assert/strict'
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'
import { geaModuleGraphPlugins } from '../scripts/gea-vite-module-graph-plugin.mjs'

// Exercise resolver/load hooks against tracked files without creating fixtures.
const source = fileURLToPath(new URL('../package.json', import.meta.url))
const importer = fileURLToPath(new URL('../scripts/gea-vite-module-graph-plugin.mjs', import.meta.url))
const [assets] = geaModuleGraphPlugins({ outDir: '.' })
const context = { resolve: async (id) => ({ id }) }
const id = await assets.resolveId.call(context, source + '?raw', importer)
assert.equal(id, fs.realpathSync(source) + '.gearawmodule.js')
assert.equal(assets.load(id), 'export default ' + JSON.stringify(fs.readFileSync(source, 'utf8')) + '\n')
assert.equal(await assets.resolveId.call(context, source + '?inline', importer), null)
assert.equal(await assets.resolveId.call(context, source + '?raw&inline', importer), null)
assert.equal(await assets.resolveId.call(context, source + '?raw', undefined), null)
console.log('raw imports preserve UTF-8 contents in a distinct compiler-visible module')
