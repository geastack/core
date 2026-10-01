import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import { resolveCompilerRuntimeEntry } from '../scripts/gea-native-style-plugin.mjs'
import assert from 'node:assert/strict'

import { transformGeaEmbeddedCompatSource } from '../scripts/gea-embedded-compat-transform.mjs'
import {
  COMPAT_STAGING_IGNORED_DIRECTORIES,
  normalizeEmbeddedJsxOptions,
  shouldIgnoreCompatStagingDirectory,
} from '../scripts/gea-embedded-compat-staging.mjs'

for (const directory of ['.build-test', '.scratch', '.test-tmp', 'generated-output']) {
  assert.equal(
    shouldIgnoreCompatStagingDirectory(directory),
    true,
    `compat staging should exclude transient directory ${directory}`,
  )
  assert.ok(COMPAT_STAGING_IGNORED_DIRECTORIES.includes(directory))
}
assert.equal(shouldIgnoreCompatStagingDirectory('components'), false)

const componentFromGeastackCore = `
import { Component } from '@geastack/core'

export function App() {
  return <display><text>Hello</text></display>
}
`

const transformedExistingComponent = transformGeaEmbeddedCompatSource(componentFromGeastackCore, '/tmp/App.tsx')

assert.equal(
  transformedExistingComponent.includes('from "gea-embedded"') || transformedExistingComponent.includes("from 'gea-embedded'"),
  true,
  'existing Component import from @geastack/core should move to the resolvable gea-embedded runtime import',
)
assert.match(
  transformedExistingComponent,
  /import \{\s*Component\s*\} from ['"]gea-embedded['"]/,
  'Component should be moved to the Gea runtime import recognized by the IR plugin',
)
assert.match(
  transformedExistingComponent,
  /export class App extends Component/,
  'function components should still become Component subclasses',
)

const coreImportWithoutComponent = `
import { Store } from '@geastack/core'

export const Tile = () => <display><text>Tile</text></display>
`

const transformedMissingComponent = transformGeaEmbeddedCompatSource(coreImportWithoutComponent, '/tmp/Tile.tsx')

assert.match(
  transformedMissingComponent,
  /import \{\s*Store,\s*Component\s*\} from ['"]gea-embedded['"]/,
  'Store and Component should be imported from the Gea runtime package recognized by the IR plugin',
)
assert.equal(
  (transformedMissingComponent.match(/from ['"]gea-embedded['"]/g) ?? []).length,
  1,
  'adding Component to a normalized runtime import must create exactly one gea-embedded import',
)

const storeOnlyModule = `
import { Store } from '@geastack/core'

export class GameStore extends Store {
  cell0 = ' '
}

export const game = new GameStore()
`

const transformedStoreOnlyModule = transformGeaEmbeddedCompatSource(storeOnlyModule, '/tmp/GameStore.tsx')

assert.match(
  transformedStoreOnlyModule,
  /import \{\s*Store\s*\} from ['"]gea-embedded['"]/,
  'non-JSX store modules should still be normalized for the Gea IR plugin',
)

const browserComponent = transformGeaEmbeddedCompatSource(
  "import { Component as View, Store, Router } from '@geajs/core'; export class App extends View { template() { return <div /> } }",
  'src/App.tsx',
)
assert.match(browserComponent, /import \{ Component as View, Store \} from ["']gea-embedded["']/)
assert.match(browserComponent, /import \{ Router \} from ["']@geajs\/core["']/)
const browserStore = transformGeaEmbeddedCompatSource(
  storeOnlyModule.replace('@geastack/core', '@geajs/core'), 'src/GameStore.ts',
)
assert.match(browserStore, /import \{\s*Store\s*\} from ["']gea-embedded["']/)
assert.doesNotMatch(browserStore, /@geajs\/core/)
const embeddedOptions = { jsx: 'preserve', jsxImportSource: '@geajs/core', strict: true }
normalizeEmbeddedJsxOptions(embeddedOptions)
assert.deepEqual(embeddedOptions, { jsx: 'preserve', strict: true })
const customOptions = { jsxImportSource: 'custom-jsx-provider' }
normalizeEmbeddedJsxOptions(customOptions)
assert.equal(customOptions.jsxImportSource, 'custom-jsx-provider')

// A workspace child and a web sub-app must resolve the same typed source as
// an app at the install root, even when package.json is hidden by exports.
const resolutionFixture = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-runtime-resolution-'))
try {
  const workspace = path.join(resolutionFixture, 'workspace')
  const child = path.join(workspace, 'apps', 'balls')
  const core = path.join(resolutionFixture, 'framework')
  const packageRoot = path.join(workspace, 'node_modules', '@geajs', 'core')
  fs.mkdirSync(child, { recursive: true })
  fs.mkdirSync(path.join(packageRoot, 'src'), { recursive: true })
  fs.writeFileSync(path.join(packageRoot, 'package.json'), JSON.stringify({name:'@geajs/core', exports:{'.':'./dist/index.js'}}))
  for (const name of ['index.ts', 'compiler-runtime.ts']) fs.writeFileSync(path.join(packageRoot, 'src', name), '')
  const expected = fs.realpathSync(path.join(packageRoot, 'src/index.ts'))
  assert.equal(resolveCompilerRuntimeEntry([workspace, core]), expected)
  assert.equal(resolveCompilerRuntimeEntry([child, core]), expected)
  assert.equal(resolveCompilerRuntimeEntry([path.join(workspace, 'web'), core]), expected)
  fs.rmSync(path.join(packageRoot, 'src/compiler-runtime.ts'))
  assert.equal(resolveCompilerRuntimeEntry([child]), '', 'untyped dist packages cannot satisfy the native source requirement')
  const nested = path.join(core, 'node_modules', '@geajs', 'core', 'src')
  fs.mkdirSync(nested, { recursive: true })
  for (const name of ['index.ts', 'compiler-runtime.ts']) fs.writeFileSync(path.join(nested, name), '')
  assert.equal(resolveCompilerRuntimeEntry([child, core]), fs.realpathSync(path.join(nested, 'index.ts')))
} finally {
  fs.rmSync(resolutionFixture, { recursive: true, force: true })
}
